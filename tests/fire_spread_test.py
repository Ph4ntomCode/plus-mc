#!/usr/bin/env python3
"""Integration test: fire spreads, ages and burns blocks down over the wire.

The fire milestone left one honest limitation: a lit fire was inert, because
`FireBlock.tick` is a *scheduled* block tick and the server had no block-tick
loop to hold the schedule. This test drives that loop end to end.

The scenario is a 3x3 hay-block platform the test lays itself with real
`use_item_on` packets, lit on all nine of its top cells with flint and steel.
Fire is then left alone and the client watches for the loop's own output:

  * a hay block consumed — replaced by fire or gone to air
    (`FireBlock.checkBurnOut`'s burn-odds roll against the block the fire is
    sitting on), or
  * fire appearing in a cell the test never lit (`FireBlock.tick`'s spread loop,
    which only ever lands in an empty cell whose own neighbour is flammable).

Either one can only come from `fire_tick` running, so either one proves the
per-chunk block-tick loop is wired and ticking. Nothing is sent from the moment
the fire is lit, so every block update after that is the loop's own doing.

The server runs with `PLUSMC_TICK_SCALE` so the 30..40-tick fire delay passes in
a couple of real ticks instead of two seconds.

Run: fire_spread_test.py /path/to/plusmc
"""
import os, re, shutil, socket, struct, subprocess, sys, tempfile, threading, time
import wire_client
from wire_client import make_frame, read_packet

PROTO = 777
HOST = "127.0.0.1"

# Item ids (src/world/item_registry.h).
ITEM_HAY_BLOCK = 579
ITEM_FLINT_AND_STEEL = 1005

# `minecraft:fire`'s block-state ids (src/world/block_states.h): AGE_15 with the
# five connection properties is 512 states, 4931..5442. Any of them can be a
# spread's result, depending on which neighbours burn where it lands.
FIRE_STATE_MIN = 4931
FIRE_STATE_MAX = 5442

# Clientbound packet ids.
CB_BLOCK_UPDATE = 0x08
CB_SET_HEALTH = 0x6A
CB_KEEP_ALIVE = 0x2D
CB_PLAYER_POSITION = 0x49

# Serverbound packet ids.
SB_FLYING = 0x21
SB_POSITION = 0x1E
SB_SET_CARRIED_ITEM = 0x36
SB_SET_CREATIVE_SLOT = 0x39
SB_USE_ITEM_ON = 0x42
SB_KEEP_ALIVE = 0x1C

# The platform and the player standing on it. The hay goes at y=65 so the
# player's feet are at 66; a click on each hay block's *up* face puts the fire in
# the cell above it, which for the middle one is the cell the player is standing
# in (`FlintAndSteelItem.useOn` places past the clicked face).
FLOOR_Y = 65
FEET_Y = FLOOR_Y + 1
PLAYER_X = 0.5
PLAYER_Z = 0.5
PLATFORM = [(-1, -1), (0, -1), (1, -1),
            (-1, 0), (0, 0), (1, 0),
            (-1, 1), (0, 1), (1, 1)]

# Menu slots: hotbar slot 0 is menu slot 36.
SLOT_MENU_HOTBAR_FIRST = 36

# 20 game ticks per real tick, so a 30..40-tick fire delay lands in about 100 ms.
TICK_SCALE = 20
OBSERVE_SECONDS = 8.0


# --- framing ----------------------------------------------------------------

def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            b |= 0x80
        out.append(b)
        if not n:
            return bytes(out)


def rd_varint(buf, pos=0):
    val, shift = 0, 0
    while pos < len(buf):
        b = buf[pos]
        pos += 1
        val |= (b & 0x7F) << shift
        shift += 7
        if not (b & 0x80):
            return val, pos
    return None, pos


def rd_string(buf, pos=0):
    ln, pos = rd_varint(buf, pos)
    return buf[pos:pos + ln].decode("utf-8"), pos + ln


def position(x, y, z):
    packed = (((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF)) & 0xFFFFFFFFFFFFFFFF
    return struct.pack(">Q", packed)


def unpack_position(raw):
    x = raw >> 38
    z = (raw >> 12) & 0x3FFFFFF
    y = raw & 0xFFF
    if x >= 0x2000000:
        x -= 0x4000000
    if z >= 0x2000000:
        z -= 0x4000000
    if y >= 0x800:
        y -= 0x1000
    return x, y, z


def read_block_update(body):
    """block_update: [i64 packed position][varint state id]."""
    raw = struct.unpack(">q", body[0:8])[0]
    state, pos = rd_varint(body, 8)
    assert pos == len(body), f"block_update: {len(body) - pos} trailing byte(s)"
    return unpack_position(raw), state


def read_set_health(body):
    """set_health: [f32 health][varint food][f32 saturation]."""
    health = struct.unpack(">f", body[0:4])[0]
    food, _ = rd_varint(body, 4)
    return round(health, 3), food


# --- the client -------------------------------------------------------------

class Bot:
    def __init__(self, name, port):
        self.name = name
        self.sock = wire_client.transport(HOST, port, timeout=10)
        self.sock.settimeout(10)
        self.threshold = -1
        # The read side belongs to a thread from the first drain after the join
        # (`wire_client.Reader`): a windowed drain that reads the socket itself
        # can abandon a half-taken packet when its window closes, which leaves
        # every packet after it garbage, and it can also miss a keep-alive.
        self.reader = wire_client.Reader(self.sock, self)
        self.entity_id = None
        self.position = None

    def send(self, pid, body=b""):
        self.reader.send(pid, body)

    def recv(self):
        pid, body = read_packet(self.sock, self.threshold)
        if pid == CB_PLAYER_POSITION:
            self.position = struct.unpack(">ddd", body[0:24])
        return pid, body

    def start_reader(self):
        self.reader.start()


    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def join(self):
        hs = (varint(0) + varint(PROTO) + varint(len(HOST)) + HOST.encode()
              + struct.pack(">H", 0) + varint(2))
        self.sock.sendall(make_frame(hs, -1))
        self.send(0x00, varint(len(self.name)) + self.name.encode() + b"\x00" * 16)
        while True:
            pid, body = self.recv()
            if pid == 0x03:
                self.threshold, _ = rd_varint(body)
            elif pid == 0x02:
                self.send(0x03)
                break
            else:
                raise AssertionError(f"unexpected login packet 0x{pid:02x}")
        while True:
            pid, body = self.recv()
            if pid == 0x0F:
                npacks, i = rd_varint(body)
                packs = []
                for _ in range(npacks):
                    ns, i = rd_string(body, i)
                    pk, i = rd_string(body, i)
                    ver, i = rd_string(body, i)
                    packs.append((ns, pk, ver))
                out = varint(len(packs))
                for ns, pk, ver in packs:
                    out += (varint(len(ns)) + ns.encode() + varint(len(pk)) + pk.encode()
                            + varint(len(ver)) + ver.encode())
                self.send(0x07, out)
            elif pid == 0x03:
                self.send(0x03)
                break
        while True:
            pid, _ = self.recv()
            if pid == 0x32:
                break
        for _ in range(400):
            pid, body = self.recv()
            if pid == CB_KEEP_ALIVE:
                self.send(SB_KEEP_ALIVE, body[:8])
            elif pid == 0x0B:
                batch, _ = rd_varint(body)
                self.send(0x0B, varint(batch))
                break
        return True

    def move_to(self, x, y, z, ground=1):
        self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([ground]))


def drain(bot, timeout=1.0):
    """Every packet the reader thread queued in the next `timeout`
    seconds."""
    return wire_client.drain(bot, timeout)
def all_of(packets, pid):
    return [b for p, b in packets if p == pid]


def block_updates(packets):
    return [read_block_update(b) for b in all_of(packets, CB_BLOCK_UPDATE)]


def use_item_on(bot, x, y, z, face, seq, cursor=(0.5, 0.5, 0.5)):
    """ServerboundUseItemOn: [varint hand][i64 position][varint face][3 f32
    cursor][bool inside][bool border][varint sequence]. The cursor is checked
    against the block's *centre*, so a half-block offset is on it."""
    bot.send(SB_USE_ITEM_ON,
             varint(0) + position(x, y, z) + varint(face)
             + struct.pack(">fff", *cursor) + bytes([0, 0]) + varint(seq))


def select(bot, menu_slot):
    bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", menu_slot - SLOT_MENU_HOTBAR_FIRST))
    drain(bot, 0.2)


def creative_item(bot, menu_slot, item_id, count=64):
    bot.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", menu_slot) + varint(count)
             + varint(item_id) + b"\x00\x00")
    drain(bot, 0.2)


def fire_tick_progress(updates, lit, hay, hay_state):
    """The first block update only `fire_tick` could have produced, or None.

    A hay cell that is no longer hay has been consumed (to fire, or to air); a
    cell that is not one of the nine the test lit, holding a fire state, is a
    spread. Everything else — the placements themselves — is the test's own
    doing.
    """
    for at, state in updates:
        if at in hay and state != hay_state:
            return ("a hay block was consumed", at, state)
        if at not in lit and FIRE_STATE_MIN <= state <= FIRE_STATE_MAX:
            return ("fire spread into an unlit cell", at, state)
    return None


START = time.time()


def step(text):
    print(f"[{time.time() - START:6.2f}s] {text}", flush=True)


def wait_listening(proc, timeout=20):
    deadline = time.time() + timeout
    port = None
    while time.time() < deadline:
        line = proc.stdout.readline().decode(errors="replace")
        if line:
            sys.stderr.write(line)
            m = re.search(r"listening on.*:(\d+)", line)
            if m:
                port = int(m.group(1))
                break

    def pump():
        for raw in iter(proc.stdout.readline, b""):
            sys.stderr.write(raw.decode(errors="replace"))

    threading.Thread(target=pump, daemon=True).start()
    if port is None:
        raise RuntimeError("server did not start")
    for _ in range(30):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=1)
            s.close()
            break
        except OSError:
            time.sleep(0.1)
    return port


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-firespread-")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_TICK_SCALE"] = str(TICK_SCALE)
    # The spread is the *world* clock (the scheduled block ticks and the random
    # pass, compressed by PLUSMC_TICK_SCALE). The survival tick is pinned to
    # packets — one recognized packet is one tick, idle wall time is none — so
    # the bot's own clock (which is what a burning player counts down on) cannot
    # drift with wall time and sit underneath the state claims below.
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"

    proc = None
    try:
        step(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--motd", "fire spread test",
             "--offline-mode", "--gamemode", "1", "--data", data_root, "--world", world],
            wait_listening, env=env)

        bot = Bot("SpreadBot", port)
        wire_client.record(bot, note="fire-spread-wire: the random-tick pass burning outward")
        bot.join()
        drain(bot, 1.2)
        step("  OK joined (creative, so the fire cannot kill the bot)")

        # --- 1. a 3x3 hay platform -------------------------------------------
        # Hay, because it is the most flammable thing a creative client can lay
        # down easily: ignite odds 60 feed the spread, burn odds 20 feed the
        # burn-out.
        creative_item(bot, SLOT_MENU_HOTBAR_FIRST, ITEM_HAY_BLOCK)
        bot.move_to(PLAYER_X, float(FEET_Y), PLAYER_Z)
        time.sleep(0.2)
        seq = 1
        for x, z in PLATFORM:
            use_item_on(bot, x, FLOOR_Y - 1, z, 1, seq)
            seq += 1
        updates = block_updates(drain(bot, 1.5))
        assert len(updates) == len(PLATFORM), f"expected {len(PLATFORM)} hay blocks, got {updates}"
        assert all(at[1] == FLOOR_Y for at, _ in updates), updates
        states = {state for _, state in updates}
        assert len(states) == 1, f"the platform is not one state: {states}"
        hay_state = states.pop()
        hay = {(x, FLOOR_Y, z) for x, z in PLATFORM}
        step(f"  OK platform: {len(updates)} hay blocks at y={FLOOR_Y} (state {hay_state})")

        # --- 2. light every top cell ----------------------------------------
        creative_item(bot, SLOT_MENU_HOTBAR_FIRST + 1, ITEM_FLINT_AND_STEEL, count=1)
        select(bot, SLOT_MENU_HOTBAR_FIRST + 1)
        for x, z in PLATFORM:
            use_item_on(bot, x, FLOOR_Y, z, 1, seq)
            seq += 1
        updates = block_updates(drain(bot, 1.5))
        lit = {(x, FEET_Y, z) for x, z in PLATFORM}
        # Every cell must have been fire at least once. It is *not* "the last
        # update for this cell is fire": the block-tick loop is already running
        # (PLUSMC_TICK_SCALE makes a fire tick about 100 ms), so a cell can be lit
        # and then have its fire burn out again inside this very drain.
        saw_fire = {at for at, state in updates
                    if at in lit and FIRE_STATE_MIN <= state <= FIRE_STATE_MAX}
        assert saw_fire == lit, f"not every cell was lit: {sorted(saw_fire)} of {sorted(lit)}"
        step(f"  OK lit all {len(lit)} cells with flint and steel")

        # --- 3. the block-tick loop moves the fire on its own -----------------
        # Nothing is sent from here on: whatever arrives can only be `fire_tick`
        # writing a block. The lighting drain may already have caught a spread,
        # so it is searched first.
        seen = list(updates)
        found = fire_tick_progress(updates, lit, hay, hay_state)
        deadline = time.time() + OBSERVE_SECONDS
        while time.time() < deadline and found is None:
            updates = block_updates(drain(bot, 0.35))
            seen += updates
            found = fire_tick_progress(updates, lit, hay, hay_state)
        assert found is not None, (
            f"the fire never spread or burned anything in {OBSERVE_SECONDS}s: {seen}")
        what, at, state = found
        step(f"  OK {what} at {at} (state {state})")

        bot.close()
        print("fire spread: all steps passed")
        return 0
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(world, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
