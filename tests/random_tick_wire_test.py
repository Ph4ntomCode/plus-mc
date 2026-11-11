#!/usr/bin/env python3
"""Integration test: the per-chunk random-tick pass over the wire.

The block-tick loop milestone left a hole: fire ticked because it was
*scheduled*, but nothing sampled the world per chunk, so no block ever got a
random tick. This test drives the pass that does, and the two block families
that hang off it — `CropBlock` (world/crop.h) and `LeavesBlock`
(world/leaves.h).

Everything is set up with `/setblock`, which is also the command that makes the
randomly ticking blocks reachable without a world generator: it writes the
block's *default* state, so `/setblock <pos> oak_leaves` is the state
generation writes (`persistent=false, distance=7` — the one that decays) rather
than the persistent leaf a player places.

Four observations, each of which can only come from the pass running:

  1. **A crop grows to maturity.** The crop is *planted* the way a player plants
     one — a real `use_item_on` with `minecraft:wheat_seeds` on hydrated farmland,
     which is the block-item table doing its job: the seed's name is not the
     crop's (`wheat_seeds` places `wheat`) and the item named `wheat` places
     nothing. It is then watched until its cell's block update reaches the state
     id that `wheat[age=7]` was given — the age `getMaxAge` stops at. Two
     controls, both never written again: a crop set straight to `age=7`, which
     `CropBlock.isRandomlyTicking` reports as not ticking at all, and the same
     crop sealed in a stone pocket, whose `getRawBrightness` is 0 — so it *is*
     sampled, counts towards its section, and is refused by the gate.
  2. **A leaf with no wood decays.** A lone non-persistent leaf at distance 7:
     `LeavesBlock.decaying` is true for it, so its own random tick removes the
     block and the client sees air.
  3. **A leaf's distance relaxes to the wood beside it.** A persistent leaf at
     distance 7 with nothing next to it has a log placed beside it; the leaf's
     scheduled tick writes distance 1 — the state a reference leaf set to
     `distance=1` was given — which is the `updateShape` -> `scheduleTick` ->
     `updateDistance` chain the engine has no general neighbour-update pass for.
  4. **And relaxes back.** Removing that log walks the leaf back to distance 7,
     the exact state it started in, and (being persistent) it does not decay
     along the way.

  5. **A placed log anchors a leaf too.** The same neighbour update, reached
     through `BlockItem.place` (a `use_item_on` with the log item in hand) rather
     than through the command: the leaf relaxes to distance 1 again.

and one negative, because `/setblock`'s own parser is part of the milestone:

  6. **The refusals are refusals.** An unknown block, a property the block does
     not carry, a property value no state of it has, and `keep` over a cell that
     is not air all leave the world unwritten.

The server runs with `PLUSMC_TICK_SCALE` (the world clock moves 20 game ticks
per 50 ms) and `PLUSMC_RANDOM_TICK_SPEED=256` (a sixteenth of a section's cells
are sampled per tick), which is what makes a 1-in-3 growth roll per sample
observable in about a second rather than in hours. Nothing is sent once a
step's setup is done, so every block update after that is the pass's own
writing.

The client reads on a background thread and harvests what arrived; a read that
gave up mid-frame (a short socket timeout) would lose the rest of that frame
and desynchronise the stream for the rest of the test.

Run: random_tick_wire_test.py /path/to/plusmc
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
SB_SET_CARRIED_ITEM = 0x36
SB_SET_CREATIVE_SLOT = 0x39
SB_USE_ITEM_ON = 0x42

# Item ids (src/world/item_registry.h).
ITEM_WHEAT_SEEDS = 1065
ITEM_OAK_LOG = 163

# Player menu slots: hotbar 0 is menu slot 36.
SLOT_MENU_HOTBAR_FIRST = 36

# `minecraft:air`'s only state, which `air_state_id()` reports (block_states.h).
AIR_STATE = 0

# The farm sits high in an all-air column, so it is open sky: `raw_brightness`
# (the real `Level.getRawBrightness`, from the chunk's own light) is 15 there.
GROUND_Y = 100
CROP_Y = GROUND_Y + 1

# Everything lives in chunk (0, 0), which `player_view_chunks()` keeps loaded
# while the bot stands inside it — `random_tick_sections` is per chunk, so one
# chunk makes the sampling that matters the only sampling there is.
CROP_CELL = (2, CROP_Y, 2)      # planted by hand, then grows to the last age
MATURE_CELL = (12, CROP_Y, 12)  # the control: set to the last age up front
# The light gate's control: the same crop in a sealed pocket, so its own
# `raw_brightness` is 0. A single stone roof is *not* dark — sky light leaks in
# from the open columns beside it, and five steps out still leaves 10 — so the
# pocket is closed on all six faces: stone above, on the four sides, and hydrated
# farmland below (`CropBlock.getGrowthSpeed`'s best case, which keeps the speed
# gate out of the way: the cell is refused by the light, never by the soil).
DARK_CROP = (5, 61, 5)
DARK_ROOF = (5, 62, 5)
DARK_SHELL = ((5, 60, 5),   # the farmland floor: still opaque to light
              (4, 61, 5), (6, 61, 5), (5, 61, 4), (5, 61, 6))
LEAF_CELL = (6, CROP_Y, 6)      # decays: distance 7, not persistent
ANCHOR_LEAF = (9, CROP_Y, 9)    # persistent; its distance follows the log at...
ANCHOR_LOG = (9, CROP_Y, 10)    # ...its z+1 neighbour
REFERENCE_LEAF = (14, CROP_Y, 14)
# In reach of the farmland under CROP_CELL (the `use_item_on` that plants it is
# a real placement, so the reach gate applies to it like any other).
BOT_POSITION = (2.5, float(CROP_Y), 3.5)

# 20 game ticks per 50 ms loop iteration.
TICK_SCALE = 20
# Samples per ticking section per tick, of the section's 4096 cells.
RANDOM_TICK_SPEED = 256
# Each observation's budget. Every one of them lands inside a single window
# (about a second of real time); the budget is generous because the world
# clock's rate is load-dependent.
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
        # `seen` is every packet of the session, in order. The pass can write a
        # block *while a command's own drain is still running*, so an observation
        # has to be able to search past the setup that produced it — hence the
        # running log and the marks `setblock` returns.
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


def creative_item(bot, menu_slot, item_id, count=64):
    """Put a creative stack in a menu slot (`ServerboundSetCreativeModeSlot`:
    [i16 slot][varint count][varint item][empty component patch])."""
    bot.send(SB_SET_CREATIVE_SLOT,
             struct.pack(">h", menu_slot) + varint(count) + varint(item_id) + b"\x00\x00")
    drain(bot, 0.15)


def select(bot, menu_slot):
    bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", menu_slot - SLOT_MENU_HOTBAR_FIRST))
    drain(bot, 0.15)


def use_item_on(bot, x, y, z, face, seq, cursor=(0.5, 0.5, 0.5)):
    """`ServerboundUseItemOn`: [varint hand][i64 position][varint face][3 f32
    cursor][bool inside][bool border][varint sequence]. The cursor is checked
    against the block's centre, so a half-block offset is on it."""
    bot.send(SB_USE_ITEM_ON,
             varint(0) + position(x, y, z) + varint(face)
             + struct.pack(">fff", *cursor) + bytes([0, 0]) + varint(seq))


def setblock(bot, cell, block, mode=None, timeout=0.5):
    """`/setblock` one cell and return `(state id written, mark)`.

    The queue is emptied first, so the update returned is this command's own
    broadcast — `setBlock` writes and then tells the clients, and a random tick
    can only write to that cell after the value exists. `mark` is the index in
    the session log just before that write, so an observation that follows can
    search from the setup that caused it (and not miss a write that happened
    inside this drain).
    """
    drain(bot, 0.0)
    mark = len(bot.seen)
    x, y, z = cell
    text = f"setblock {x} {y} {z} {block}" + (f" {mode}" if mode else "")
    send_chat_command(bot, text)
    packets = drain(bot, timeout)
    wrote = updates_at(packets, cell)
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
    world = tempfile.mkdtemp(prefix="plusmc-randomtick-")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_TICK_SCALE"] = str(TICK_SCALE)
    env["PLUSMC_RANDOM_TICK_SPEED"] = str(RANDOM_TICK_SPEED)
    # Everything measured here is the *world* clock: the random-tick pass's
    # sample rate (PLUSMC_RANDOM_TICK_SPEED) and the block ticks a crop's growth
    # and a leaf's decay ride, both compressed by PLUSMC_TICK_SCALE. The survival
    # tick is pinned to packets — one tick each, no wall-time catch-up — so the
    # bot's own clock (food, burning, use progress) cannot drift behind the
    # block updates these steps compare.
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"

    proc = None
    try:
        step(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--motd", "random tick test",
             "--offline-mode", "--gamemode", "1", "--data", data_root, "--world", world],
            wait_listening, env=env)

        bot = Bot("TickBot", port)
        wire_client.record(bot, note="random-tick-wire: crops growing and leaves decaying")
        bot.join()
        bot.move_to(*BOT_POSITION)
        drain(bot, 1.0)
        step("  OK joined (creative, so nothing here can hurt the bot)")

        # --- 1. a crop grows to maturity -------------------------------------
        # A 3x3 of hydrated farmland under the crop is `getGrowthSpeed`'s best
        # case: 3.0 for the cell directly below plus 0.75 for each of the eight
        # around it, so the roll is `nextInt(25/10 + 1) == 0`, one in three.
        for x in range(CROP_CELL[0] - 1, CROP_CELL[0] + 2):
            for z in range(CROP_CELL[2] - 1, CROP_CELL[2] + 2):
                setblock(bot, (x, GROUND_Y, z), "farmland[moisture=7]", timeout=0.25)
        mature_id, mature_mark = setblock(bot, MATURE_CELL, "wheat[age=7]", timeout=0.3)
        # Age 0, learned from the one crop that cannot change it: the same block
        # in a sealed pocket, where `raw_brightness` is 0, so its own random
        # ticks are sampled and refused — which is the light gate, and it makes
        # this the second control as well as the reference state.
        #
        # The pocket is sealed *first* and the crop is planted inside it.
        # Planting first would leave the crop open to the sky between the
        # `/setblock`s, and at RANDOM_TICK_SPEED samples a tick it can reach a
        # later age inside that window — which is the race that made this
        # control flaky (a lit crop on air grows on roughly one sample in 26).
        for i, (x, y, z) in enumerate(DARK_SHELL):
            kind = "farmland[moisture=7]" if i == 0 else "stone"
            setblock(bot, (x, y, z), kind, timeout=0.3)
        setblock(bot, DARK_ROOF, "stone", timeout=0.3)
        roof_mark = len(bot.seen)
        seed_id, _ = setblock(bot, DARK_CROP, "wheat", timeout=0.3)
        assert seed_id != mature_id, "age 0 and age 7 are the same state id"
        step(f"  OK wheat states: age 0 is {seed_id}, age 7 is {mature_id}")

        # Plant the crop that is meant to grow the way a player does: a real
        # `use_item_on` with the *seed* on the farmland, which is what the
        # block-item table is for (`minecraft:wheat_seeds` places
        # `minecraft:wheat`, and the item named `minecraft:wheat` places
        # nothing). Creative, so the seed is not consumed.
        creative_item(bot, SLOT_MENU_HOTBAR_FIRST, ITEM_WHEAT_SEEDS)
        select(bot, SLOT_MENU_HOTBAR_FIRST)
        seed_mark = len(bot.seen)
        use_item_on(bot, CROP_CELL[0], GROUND_Y, CROP_CELL[2], 1, seq=1)
        planted = updates_at(drain(bot, 0.5), CROP_CELL)
        assert planted, "planting wheat_seeds wrote no block at the crop cell"
        assert planted[0] == seed_id, (
            f"the grain the seed placed is {planted[0]}, not the crop's age 0 ({seed_id})")
        step("  OK a use_item_on with wheat_seeds planted the crop (age 0)")

        found, seen = observe_until(
            bot, lambda ups: mature_id in [s for at, s in ups if at == CROP_CELL], seed_mark)
        assert found, (f"the crop never reached its last age in {OBSERVE_SECONDS}s "
                       f"(saw {[s for at, s in seen if at == CROP_CELL]})")
        stages = [s for at, s in seen if at == CROP_CELL]
        # The two controls: a crop already at its last age is not randomly ticking
        # at all, and one in the dark is sampled and then refused by the light
        # gate. Neither cell is ever written.
        control = [s for at, s in block_updates(bot.seen[mature_mark:]) if at == MATURE_CELL]
        assert control == [mature_id], (
            f"the mature control crop was written again: {control} (its own write was {mature_id})")
        dark = [s for at, s in block_updates(bot.seen[roof_mark:]) if at == DARK_CROP]
        assert dark == [seed_id], (
            f"the dark crop was written again: {dark} (its own write was {seed_id})")
        step(f"  OK it grew {stages} to the last age; neither control was written again")

        # --- 2. a lone leaf decays -------------------------------------------
        # `/setblock oak_leaves` writes the *default* state — distance 7 and not
        # persistent — which is exactly what `LeavesBlock.decaying` is true for.
        leaf_id, leaf_mark = setblock(bot, LEAF_CELL, "oak_leaves", timeout=0.3)
        assert leaf_id != AIR_STATE, "the leaf was written as air"
        step(f"  OK placed a non-persistent distance-7 leaf (state {leaf_id})")
        found, seen = observe_until(
            bot, lambda ups: AIR_STATE in [s for at, s in ups if at == LEAF_CELL], leaf_mark)
        assert found, (f"the leaf never decayed in {OBSERVE_SECONDS}s "
                       f"(saw {[s for at, s in seen if at == LEAF_CELL]})")
        step("  OK its own random tick removed it (a block update to air)")

        # --- 3. a leaf's distance relaxes to the wood beside it ---------------
        reference_id, _ = setblock(bot, REFERENCE_LEAF,
                                   "oak_leaves[distance=1,persistent=true]", timeout=0.3)
        anchor_id, _ = setblock(bot, ANCHOR_LEAF, "oak_leaves[persistent=true]", timeout=0.3)
        assert anchor_id != reference_id, "distance 7 and distance 1 are the same state id"
        step(f"  OK persistent leaf: distance 7 is {anchor_id}, distance 1 is {reference_id}")
        _, log_mark = setblock(bot, ANCHOR_LOG, "oak_log", timeout=0.3)
        found, seen = observe_until(
            bot, lambda ups: reference_id in [s for at, s in ups if at == ANCHOR_LEAF], log_mark)
        assert found, (f"the leaf never took its distance from the log in {OBSERVE_SECONDS}s "
                       f"(saw {[s for at, s in seen if at == ANCHOR_LEAF]})")
        step("  OK the log's write scheduled the leaf's tick and it relaxed to distance 1")

        # --- 4. and relaxes back when the log goes ----------------------------
        _, gone_mark = setblock(bot, ANCHOR_LOG, "air", timeout=0.3)
        found, seen = observe_until(
            bot, lambda ups: anchor_id in [s for at, s in ups if at == ANCHOR_LEAF], gone_mark)
        assert found, (f"the leaf never went back to distance 7 in {OBSERVE_SECONDS}s "
                       f"(saw {[s for at, s in seen if at == ANCHOR_LEAF]})")
        # A persistent leaf is never `decaying`, so it survived the round trip.
        assert AIR_STATE not in [s for at, s in seen if at == ANCHOR_LEAF], (
            "the persistent leaf decayed")
        step("  OK removing the log walked it back to distance 7 (and it did not decay)")

        # --- 5. a log the *player* places anchors the canopy just the same -----
        # The same neighbour update, reached through `BlockItem.place` instead of
        # through `/setblock`: a real `use_item_on` with an oak_log item. The bot
        # has to walk over first — placement is range-checked
        # (`isWithinBlockInteractionRange`, 5.5 blocks) and the farm the crop was
        # planted from is nine blocks away. A stone is put under the log's cell so
        # there is a block to click the top face of (a log cannot be placed into
        # a cell the client never aimed at).
        bot.move_to(ANCHOR_LOG[0] + 0.5, float(CROP_Y), ANCHOR_LOG[2] + 1.5)
        drain(bot, 0.5)
        setblock(bot, (ANCHOR_LOG[0], GROUND_Y, ANCHOR_LOG[2]), "stone", timeout=0.3)
        creative_item(bot, SLOT_MENU_HOTBAR_FIRST + 1, ITEM_OAK_LOG)
        select(bot, SLOT_MENU_HOTBAR_FIRST + 1)
        placed_mark = len(bot.seen)
        use_item_on(bot, ANCHOR_LOG[0], GROUND_Y, ANCHOR_LOG[2], 1, seq=2)
        wrote = updates_at(drain(bot, 0.5), ANCHOR_LOG)
        assert wrote, "the use_item_on placed no log"
        found, seen = observe_until(
            bot, lambda ups: reference_id in [s for at, s in ups if at == ANCHOR_LEAF],
            placed_mark)
        assert found, (f"a placed log never anchored the leaf in {OBSERVE_SECONDS}s "
                       f"(saw {[s for at, s in seen if at == ANCHOR_LEAF]})")
        step("  OK the placement path told the leaf, and it relaxed to distance 1")

        # --- 6. /setblock refuses what vanilla refuses -------------------------
        # Every one of these is a parse the command has to fail: a name the
        # registry does not know, a property the block does not carry, a property
        # *value* no state of it has, and `keep` over a cell that is not air. A
        # refusal is silent on the wire, which is the assertion — the cell is not
        # written.
        refuse_cell = (0, CROP_Y, 0)
        setblock(bot, refuse_cell, "stone", timeout=0.3)
        written = len(updates_at(bot.seen, refuse_cell))
        for text in ("no_such_block", "stone[no_such_property=1]", "wheat[age=99]"):
            send_chat_command(bot, f"setblock {refuse_cell[0]} {refuse_cell[1]} "
                                   f"{refuse_cell[2]} {text}")
            drain(bot, 0.3)
            assert len(updates_at(bot.seen, refuse_cell)) == written, (
                f"/setblock accepted '{text}'")
        send_chat_command(bot, f"setblock {refuse_cell[0]} {refuse_cell[1]} "
                               f"{refuse_cell[2]} dirt keep")
        drain(bot, 0.3)
        assert len(updates_at(bot.seen, refuse_cell)) == written, (
            "/setblock keep overwrote a cell that was not air")
        step("  OK an unknown block, unknown property, impossible value and keep were all refused")

        bot.close()
        print("random tick: all steps passed")
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
