#!/usr/bin/env python3
"""Integration test: settled fluid resumes when a block beside it is removed.

The engine has no general neighbour-update pass (world/crop.h, world/leaves.h),
so every path that reaches `Level.setBlock(..., 3)` owes its neighbours a
fan-out. That fan-out is one helper in session.cpp — `notify_block_neighbours`
— and this test pins the fluid half of its contract against vanilla: breaking a
wall beside sealed water must schedule the source's tick (vanilla's neighbour
update reaching LevelTicks) so the water flows into the opened cell.

Two observations, which only the removal path can produce:

  1. **Nothing flows while the wall stands.** The cup is built with `/setblock`
     (floor, sealed side walls, wall at x+1, source placed last), so the source
     is never scheduled before the dig: nothing writes the opened cell during a
     settle window. This is the control — a passing run proves the flow below
     came from the break, not from a source still running.
  2. **The water flows in after the dig.** A creative `STOP_DESTROY_BLOCK`
     (the packet block_interact_test uses) at the wall: the dig's own air
     broadcast lands first, then the source's scheduled tick writes
     `water[level=1]` — the state a source's first spread produces — into the
     vacated cell, broadcast back to the bot.

The world runs under `PLUSMC_TICK_SCALE=20`, so vanilla's five-tick fluid delay
elapses inside one loop iteration and the flow lands well inside the wait.
Nothing is sent from here once the dig is in, so every block update after it is
the world's own writing.

Run: fluid_resume_test.py /path/to/plusmc
"""
import os, re, shutil, socket, struct, subprocess, sys, tempfile, threading, time
import wire_client
from wire_client import make_frame, read_packet

PROTO = 777
HOST = "127.0.0.1"

# Clientbound packet ids.
CB_BLOCK_UPDATE = 0x08
CB_KEEP_ALIVE = 0x2D
CB_PLAYER_POSITION = 0x49

# Serverbound packet ids.
SB_CHAT_COMMAND = 0x07
SB_POSITION = 0x1E
SB_KEEP_ALIVE = 0x1C
# ServerboundPlayerAction: creative break is action 2, face 0, any sequence.
SB_PLAYER_ACTION = 0x29

# `minecraft:air`'s only state (`air_state_id()` in block_states.h).
AIR_STATE = 0

# The cup sits high in an all-air column (same height the random-tick fixture
# uses): floor at y=99, the pool at y=100, bot's feet on the pool's level. The
# cells stay in chunk (0, 0), which the join keeps loaded.
FLOOR = (1, 99, 0)
SOURCE = (1, 100, 0)
WALL = (2, 100, 0)            # the wall the dig removes
SEALED = [(0, 100, 0), (1, 100, 1), (1, 100, -1)]
# Same water one level down, fetched for its exact broadcast state id — the id
# the source's spread into the opened cell must match.
SCRATCH = (8, 100, 0)

# In reach of WALL: the server measures 5.5 blocks from the eye to the block's
# box (Player.isWithinBlockInteractionRange), and this stands eye-to-box ~1.8.
BOT_POSITION = (1.5, 101.0, 1.5)

# 20 game ticks per 50 ms loop iteration.
TICK_SCALE = 20
# Each observation's budget; the flow normally lands in well under a second.
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
    """A client that always reads.

    The reader thread owns the socket's read side for the whole test (see
    tests/wire_client.py), so a window that ends never abandons a half-read
    frame; `drain` is a queue swap. Keep-alives are answered from the thread, so
    the session outlives every observation without the test having to poll.
    """

    def __init__(self, name, port):
        self.name = name
        self.sock = wire_client.transport(HOST, port, timeout=30)
        self.threshold = -1
        self.position = None
        # `seen` is every packet of the session, in order: an observation must
        # be able to search past the setup that produced it — hence the running
        # log and the marks `setblock` returns.
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
    """Wait `timeout` seconds and hand back what the reader collected, appending
    it to the session log so a later mark can search past it."""
    out = wire_client.drain(bot, timeout)
    with bot.lock:
        bot.seen += out
    return out


def send_chat_command(bot, command):
    bot.send(SB_CHAT_COMMAND, varint(len(command)) + command.encode())


def setblock(bot, cell, block, timeout=10.0, slice_s=0.1):
    """`/setblock` one cell and return `(state id written, mark)`.

    The queue is emptied first, so the first update at the cell is this
    command's own broadcast — `setBlock` writes and then tells the clients.
    The reply is waited for in slices rather than as one fixed window: a
    sanitizer build can take seconds to reach the command, and a fixed
    half-second window read that silence as a missing write. `mark` is the
    index in the session log just before that write, so an observation that
    follows can search from the setup that caused it.
    """
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


def observe_until(bot, predicate, mark, seconds=OBSERVE_SECONDS):
    """Harvest until `predicate(updates)` is truthy, or time out.

    `updates` are every block update of the session from `mark` on, so a write
    that landed inside the setup's own drain is still seen. Nothing is sent from
    here, so they can only be the world's own writing.
    """
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
    world = tempfile.mkdtemp(prefix="plusmc-fluid-resume-")
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
            [binary, "--host", HOST, "--port", "0", "--motd", "fluid resume test",
             "--offline-mode", "--gamemode", "1", "--data", data_root, "--world", world],
            wait_listening, env=env)

        bot = Bot("FlowBot", port)
        wire_client.record(bot, note="fluid-resume-wire: a dug wall lets settled "
                                     "water flow into the opening")
        bot.join()
        bot.move_to(*BOT_POSITION)
        drain(bot, 1.0)
        step("  OK joined (creative, standing beside the cup)")

        # --- 1. control: while the wall stands, nothing enters the cell -----

        setblock(bot, FLOOR, "minecraft:stone")
        for cell in SEALED + [WALL]:
            setblock(bot, cell, "minecraft:stone")
        level1_id, _ = setblock(bot, SCRATCH, "minecraft:water[level=1]")
        # The source goes in last: no placement before it can schedule it, so
        # the pool is settled vanilla equilibrium — it only ticks when a break
        # owes it one.
        source_id, _ = setblock(bot, SOURCE, "minecraft:water")
        assert source_id != level1_id, "source and flowing states must differ"
        step("  cup built: sealed source at (1,100,0), wall at (2,100,0)")

        mark = len(bot.seen)
        drain(bot, 2.0)
        sealed_in = updates_at(bot.seen[mark:], WALL)
        assert level1_id not in sealed_in, (
            f"water entered the sealed cell while the wall stood: {sealed_in}")
        step("  OK nothing flows while the wall stands")

        # --- 2. the dig: water flows into the opened cell -------------------

        mark = len(bot.seen)
        # Creative STOP_DESTROY_BLOCK at the wall, exactly as block_interact
        # digs: [varint action][i64 pos][varint face][varint sequence].
        bot.send(SB_PLAYER_ACTION, varint(2) + position(*WALL) + varint(0) + varint(1))

        found, updates = observe_until(
            bot,
            lambda ups: any(at == WALL and st == level1_id for at, st in ups),
            mark)
        wall_states = updates_at(bot.seen[mark:], WALL)
        assert found, (
            f"the water never flowed into the dug cell: states at {WALL} "
            f"= {wall_states}")
        assert AIR_STATE in wall_states, (
            f"the dig itself must broadcast air first: {wall_states}")
        assert wall_states.index(AIR_STATE) < wall_states.index(level1_id), (
            f"flow must follow the dig, not precede it: {wall_states}")
        step("  OK dug wall -> the scheduled source flowed into the opening")

        bot.close()
        print("fluid resume: all steps passed")
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
