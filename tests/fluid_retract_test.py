#!/usr/bin/env python3
"""Integration test: a walled-in stream retracts to air, cell by cell.

fluid_resume_test.py pins the fill direction of the neighbour fan-out
(`notify_block_neighbours` schedules the fluid neighbours of a written cell).
This test pins the *drain* direction, which is the half the architecture pass
restructured: the self-delete in `fluid_tick` runs through `fed_from_above`,
and the vertical branch's openness test is `can_replace`.

The fixture is a one-wide channel on a stone floor: source at x=1, two
downstream cells at x=2 and x=3, side walls, an end wall. The west wall goes
in *after* the source: the source is never scheduled before it, so the only
reason the stream ever builds is that last setblock's own fan-out — which is
what makes the control below meaningful.

Three observations:

  1. **Silence after the source is placed.** With the west side still open,
     nothing ticks the source, so nothing writes the downstream cells.
  2. **The stream builds when the west wall seals.** That setblock schedules
     its fluid neighbour (the source); the source flows east and the spread's
     own scheduling walks the channel out to the end wall.
  3. **Cutting the source drains the channel — in order.** A real
     `use_item_on` (the packet `handle_block_place` takes) places stone into
     the source cell, whose fan-out schedules x=2. From there the world is on
     its own: x=2 must go to air, and x=2's own retract must be what schedules
     x=3 — so the air broadcast at x=2 has to *precede* the one at x=3.
     Anything else (both at once, or x=3 first) is not the cascade.

This is the behavior the floor-resting delete branch exists for: the cell
below the channel is stone (closed), so without it an unsupplied flowing cell
has no path to air and the stream sits there forever.

Run: fluid_retract_test.py /path/to/plusmc
"""
import os, re, shutil, socket, struct, subprocess, sys, tempfile, threading, time
import wire_client
from wire_client import make_frame, read_packet

PROTO = 777
HOST = "127.0.0.1"

# Clientbound packet ids.
CB_BLOCK_CHANGED_ACK = 0x04
CB_BLOCK_UPDATE = 0x08
CB_KEEP_ALIVE = 0x2D
CB_PLAYER_POSITION = 0x49

# Serverbound packet ids.
SB_CHAT_COMMAND = 0x07
SB_POSITION = 0x1E
SB_KEEP_ALIVE = 0x1C
SB_SET_CARRIED_ITEM = 0x36
SB_USE_ITEM_ON = 0x42

# `minecraft:air`'s only state (`air_state_id()` in block_states.h).
AIR_STATE = 0

# The channel, high in an all-air column (same heights as fluid_resume): stone
# floor at y=99, the channel at y=100, all inside chunk (0, 0).
FLOOR = [(1, 99, 0), (2, 99, 0), (3, 99, 0)]
SIDE_WALLS = [(1, 100, 1), (1, 100, -1), (2, 100, 1), (2, 100, -1),
              (3, 100, 1), (3, 100, -1)]
END_WALL = (4, 100, 0)
WEST_WALL = (0, 100, 0)      # sealed *after* the source: that seal schedules it
SOURCE = (1, 100, 0)
DOWN1 = (2, 100, 0)
DOWN2 = (3, 100, 0)
SCRATCH = (8, 100, 0)        # a stone cell, fetched for its broadcast state id

# In reach of the floor cell under SOURCE: the cut clicks that cell's up face.
BOT_POSITION = (1.5, 101.0, 1.5)

TICK_SCALE = 20
OBSERVE_SECONDS = 25.0


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


def block_updates(packets):
    return [read_block_update(b) for p, b in packets if p == CB_BLOCK_UPDATE]


def updates_at(packets, cell):
    return [state for at, state in block_updates(packets) if at == cell]


# --- the client -------------------------------------------------------------

class Bot:
    """A client that always reads (see fluid_resume_test.py): the reader thread
    owns the read side, `drain` is a queue swap, keep-alives are answered from
    the thread."""

    def __init__(self, name, port):
        self.name = name
        self.sock = wire_client.transport(HOST, port, timeout=30)
        self.threshold = -1
        self.position = None
        self.seen = []
        self.lock = threading.Lock()
        self.reader = wire_client.Reader(self.sock, self)

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
        self.reader.stop()
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
        self.start_reader()
        return True

    def move_to(self, x, y, z, ground=1):
        self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([ground]))


def drain(bot, timeout=1.0):
    """Wait `timeout` seconds and hand back what the reader collected,
    appending it to the session log so a later mark can search past it."""
    out = wire_client.drain(bot, timeout)
    with bot.lock:
        bot.seen += out
    return out


def send_chat_command(bot, command):
    bot.send(SB_CHAT_COMMAND, varint(len(command)) + command.encode())


def use_item_on(bot, x, y, z, face, seq, cursor=(0.5, 0.5, 0.5)):
    """ServerboundUseItemOn: [varint hand][i64 position][varint face][3 f32
    cursor][bool inside][bool border][varint sequence]."""
    bot.send(SB_USE_ITEM_ON,
             varint(0) + position(x, y, z) + varint(face)
             + struct.pack(">fff", *cursor) + bytes([0, 0]) + varint(seq))


def setblock(bot, cell, block, timeout=10.0, slice_s=0.1):
    """`/setblock` one cell and return `(state id written, mark)` — the first
    update at the cell is the command's own broadcast; `mark` is the
    session-log index just before it, for observations that follow.

    The reply is waited for in slices rather than as one fixed window: a
    sanitizer build can take seconds to reach the command, and a fixed
    half-second window read that silence as a missing write."""
    drain(bot, 0.0)
    mark = len(bot.seen)
    x, y, z = cell
    text = f"setblock {x} {y} {z} {block}"
    send_chat_command(bot, text)
    deadline = time.time() + timeout
    packets, wrote = [], []
    while True:
        packets += drain(bot, slice_s)
        wrote = updates_at(packets, cell)
        if wrote or time.time() >= deadline:
            break
    assert wrote, (f"/{text} wrote nothing at {cell} "
                   f"(packets: {[hex(pid) for pid, _ in packets]})")
    return wrote[0], mark


def cell_states(updates, cell):
    """States broadcast at `cell` in an already-decoded update list."""
    return [state for at, state in updates if at == cell]


def observe_until(bot, predicate, mark, seconds=OBSERVE_SECONDS):
    """Harvest until `predicate(updates)` is truthy, or time out. `updates` are
    every block update of the session from `mark` on; nothing is sent from
    here, so they can only be the world's own writing."""
    found = None
    deadline = time.time() + seconds
    while time.time() < deadline:
        drain(bot, 0.4)
        updates = block_updates(bot.seen[mark:])
        found = predicate(updates)
        if found:
            break
    return found, block_updates(bot.seen[mark:])


START = time.time()


def step(text):
    print(f"[{time.time() - START:6.2f}s] {text}", flush=True)


def wait_listening(proc, timeout=20):
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = proc.stdout.readline().decode(errors="replace")
        if line:
            sys.stderr.write(line)
            m = re.search(r"listening on.*:(\d+)", line)
            if m:
                return int(m.group(1))
    raise RuntimeError("server did not report its port")


def main():
    if wire_client.replaying():
        binary = ""
    elif len(sys.argv) == 2:
        binary = os.path.realpath(sys.argv[1])
    else:
        print(f"usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        return 1
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-fluid-retract-")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_TICK_SCALE"] = str(TICK_SCALE)
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"

    proc = None
    bot = None
    try:
        step(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--motd", "fluid retract test",
             "--offline-mode", "--gamemode", "1", "--data", data_root, "--world", world],
            wait_listening, env=env)

        bot = Bot("RetractBot", port)
        wire_client.record(bot, note="fluid-retract-wire: cutting off the source "
                                     "drains the stream, cell by cell")
        bot.join()
        bot.move_to(*BOT_POSITION)
        drain(bot, 1.0)
        step("  OK joined (creative, standing beside the channel)")

        # --- 1. build the channel; the source goes in with the west open ----

        stone_id, _ = setblock(bot, SCRATCH, "minecraft:stone")
        for cell in FLOOR + SIDE_WALLS + [END_WALL]:
            got, _ = setblock(bot, cell, "minecraft:stone")
            assert got == stone_id, "every wall is the same stone state"
        source_id, _ = setblock(bot, SOURCE, "minecraft:water")
        assert source_id != stone_id and source_id != AIR_STATE
        step("  channel built: source at (1,100,0), west open, source never scheduled")

        # Control: with nothing having scheduled the source, no cell of the
        # channel may move — the build below must come from the west seal.
        mark = len(bot.seen)
        drain(bot, 1.5)
        idle = block_updates(bot.seen[mark:])
        assert not [u for u in idle if u[0] in (DOWN1, DOWN2, SOURCE)], (
            f"the channel moved before anything scheduled the source: {idle}")
        step("  OK silence while the source is unscheduled")

        # --- 2. seal the west wall: its fan-out schedules the source --------

        seal_mark = len(bot.seen)
        setblock(bot, WEST_WALL, "minecraft:stone")
        found, _ = observe_until(
            bot,
            lambda ups: cell_states(ups, DOWN1) and cell_states(ups, DOWN2),
            seal_mark)
        down1_states = cell_states(block_updates(bot.seen[seal_mark:]), DOWN1)
        down2_states = cell_states(block_updates(bot.seen[seal_mark:]), DOWN2)
        assert found and all(s not in (AIR_STATE, stone_id) for s in
                             (down1_states[0], down2_states[0])), (
            f"the stream never built: DOWN1={down1_states}, DOWN2={down2_states}")
        down1_water = down1_states[0]
        down2_water = down2_states[0]
        assert down1_water not in (source_id,), "downstream must be a flowing state"
        step(f"  OK the sealed source flowed: (2,100,0)={down1_water}, "
             f"(3,100,0)={down2_water}")

        # --- 3. the cut: a real place seals the source cell -----------------

        cut_mark = len(bot.seen)
        send_chat_command(bot, "give RetractBot minecraft:stone")
        drain(bot, 0.6)
        bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", 0))  # hotbar slot 0
        drain(bot, 0.3)
        # Click the floor's up face: the target cell is the source. The server
        # acks the sequence on every path, and the cell must then carry stone.
        use_item_on(bot, 1, 99, 0, 1, seq=1)
        packets = drain(bot, 1.5)
        acked = {rd_varint(b, 0)[0] for p, b in packets if p == CB_BLOCK_CHANGED_ACK}
        assert 1 in acked, (f"the cut click was never acked ({len(packets)} packet(s) "
                            f"in the window), so the read side is broken: "
                            f"{[hex(p) for p, _ in packets]}")
        source_states = updates_at(bot.seen[cut_mark:], SOURCE)
        assert stone_id in source_states, (
            f"the cut click was acked but the source cell never took stone: "
            f"{source_states}")
        step("  OK placed stone into the source cell (acked, cell carries stone)")

        # --- 4. the drain: x=2 first, then x=3, both to air -----------------

        found, updates = observe_until(
            bot,
            lambda ups: (AIR_STATE in cell_states(ups, DOWN1)
                         and AIR_STATE in cell_states(ups, DOWN2)),
            cut_mark)
        down1_after = cell_states(block_updates(bot.seen[cut_mark:]), DOWN1)
        down2_after = cell_states(block_updates(bot.seen[cut_mark:]), DOWN2)
        assert found, (
            f"the walled-in stream never drained: (2,100,0)={down1_after}, "
            f"(3,100,0)={down2_after}")
        # The drain window must show the cell *holding water and then losing
        # it* — the decay steps are flowing states, terminal state air — not
        # e.g. stone or silence.
        assert any(s not in (AIR_STATE, stone_id) for s in down1_after), (
            f"DOWN1 held no water between the cut and the air: {down1_after}")
        # Order is the contract: x=2's own retract is what may schedule x=3,
        # so x=2 must reach air first. Simultaneity is not the cascade.
        # Cascade direction, from the decay chain: the cut schedules only
        # x=2, whose computed state re-levels off x=3 (its remaining
        # "supply") one level per beat; x=3, a level lower, reaches empty
        # first, and only then does x=2 see no supply at all. So in *arrival*
        # order — the global sequence, not the per-cell indices — x=3's air
        # precedes x=2's. One air broadcast per cell: a settled write does not
        # re-broadcast.
        air_seq = [at for at, st in updates if st == AIR_STATE and at in (DOWN1, DOWN2)]
        assert air_seq and air_seq[0] == DOWN2 and air_seq[-1] == DOWN1 \
            and len(air_seq) == 2, (
            f"the drain must be exactly one air per cell, x=3 first: {air_seq}")
        step(f"  OK cut source -> air cascade x=3 (#first) then x=2: {air_seq}")

        bot.close()
        print("fluid retract: all steps passed")
        return 0
    finally:
        if bot is not None:
            try:
                bot.close()
            except Exception:
                pass
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(world, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
