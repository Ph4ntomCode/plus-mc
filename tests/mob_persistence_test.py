#!/usr/bin/env python3
"""Integration test: the mob population across a restart (Phase 4 persistence).

Vanilla gives every entity a UUID and stores living entities in per-chunk
`entities/` region files, re-issuing the run's own numeric entity ids every boot.
This engine keeps the run's ids stable instead: the whole population is written
to `<world>/mobs.dat` on save and adopted whole on load (world/mob_save.h), so a
`BreedGoal`'s partner or a `FollowParentGoal`'s parent — both entity ids — does
not die with the process. What this test drives over the real wire protocol:

  * two cows are summoned, walled into their own cells (a one-block-thick stone
    pen, the same technique breeding_test.py's follow section uses), fed to
    `setInLove`, and pair;
  * `/mobs list` — this engine's own listing, one line per mob — shows each
    cow's entity id, its `love` countdown and `mate=` the *other* cow's id;
  * the server is shut down and restarted on the same world directory, and
    `/mobs list` shows the same two ids, the same types, the same positions, a
    still-running love clock (`0 < love <= before`) and the same reciprocal
    `mate` links;
  * a cow summoned after the restart takes a brand-new id (the file's saved
    `NextEntityId`), so an id the file holds can never be re-issued to a
    stranger while a restored goal still names it.

The cows are held still on purpose: a penned pair can pair (the partner scan is
an eight-block distance test and needs no movement) but can never close inside
the three blocks `BreedGoal.tick` needs to breed, so the state under test cannot
race into a birth during the restart. The pen is not a burial: `Entity.move`
lets a mob whose box overlaps a block walk out of it, so the hold is a ring of
unwalkable neighbours (the stroll has nowhere to path), not a stone inside the
mob's own box. `PLUSMC_MOB_SPAWNING=0` keeps the natural
spawner out, so the population is exactly what this test summoned.

Run: mob_persistence_test.py /path/to/plusmc
   or: PLUSMC_REPLAY=transcript.jsonl mob_persistence_test.py  (no binary needed)
"""
import math
import os
import re
import signal
import struct
import subprocess
import sys
import tempfile
import time
import pathlib
import shutil

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nbt_component import chat_text  # noqa: E402
import wire_client  # noqa: E402
from wire_client import make_frame, read_packet  # noqa: E402

PROTO = 777
HOST = "127.0.0.1"

# minecraft:entity_type ids (src/world/mob_table.h) and item ids (item registry).
ET_COW = 30
ITEM_STONE = 1
ITEM_WHEAT = 1066

# Clientbound packet ids.
CB_SPAWN_ENTITY = 0x01
CB_ENTITY_EVENT = 0x22
CB_ENTITY_POSITION_SYNC = 0x23
CB_SYSTEM_CHAT = 0x7C
CB_KEEP_ALIVE = 0x2D

# Serverbound packet ids.
CB_BATCH = 0x0B
SB_INTERACT = 0x1A
SB_KEEP_ALIVE = 0x1C
SB_POSITION = 0x1E
SB_SET_CARRIED_ITEM = 0x36
SB_SET_CREATIVE_SLOT = 0x39
SB_USE_ITEM_ON = 0x42
SB_CHAT_COMMAND = 0x07

ENTITY_EVENT_LOVE = 18

# The floor the cows stand on: 9x9 stone at y=63, so a summoned mob's feet land
# at y=64 and the player's own feet at 64 too.
FLOOR_Y = 63
PLAYER_X = 0.5
PLAYER_Z = 0.5


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


class Bot:
    def __init__(self, name, port):
        self.name = name
        self.sock = wire_client.transport(HOST, port, timeout=10)
        self.sock.settimeout(10)
        self.threshold = -1
        self.reader = wire_client.Reader(self.sock, self)

    def send(self, pid, body=b""):
        self.reader.send(pid, body)

    def recv(self):
        return read_packet(self.sock, self.threshold)

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
            elif pid == CB_BATCH:
                batch, _ = rd_varint(body)
                self.send(CB_BATCH, varint(batch))
                break
        return True

    def move_to(self, x, y, z, ground=1):
        self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([ground]))


def drain(bot, timeout=1.0):
    return wire_client.drain(bot, timeout)


def send_chat_command(bot, command):
    payload = command.encode()
    bot.send(SB_CHAT_COMMAND, varint(len(payload)) + payload)


def interact(bot, eid):
    """ServerboundInteractPacket (0x1a): [varint eid][varint main hand]
    [LpVec3 zero][bool secondary]."""
    bot.send(SB_INTERACT, varint(eid) + varint(0) + b"\x00" + bytes([0]))


def set_creative_slot(bot, slot, count, item):
    """ServerboundSetCreativeModeSlot (0x38): [i16 slot][ItemStack] with the
    delimited component patch (two zero varints for no changes)."""
    bot.send(SB_SET_CREATIVE_SLOT,
             struct.pack(">h", slot) + varint(count) + varint(item) + b"\x00\x00")


def build_floor(bot, x_radius=5, z_radius=5):
    """81 real `use_item_on` placements: a stone square at y=FLOOR_Y, x and z in
    [-4, 4]. Clicking the cell below the target with face=up places one block."""
    set_creative_slot(bot, 36, 64, ITEM_STONE)
    time.sleep(0.2)
    placed = 0
    seq = 1
    for x in range(-x_radius + 1, x_radius):
        bot.move_to(x + 0.5, float(FLOOR_Y + 1), 0.5)
        for z in range(-z_radius + 1, z_radius):
            body = (varint(0) + position(x, FLOOR_Y - 1, z) + varint(1)
                    + struct.pack(">fff", 0.5, 0.0, 0.5) + bytes([0, 0]) + varint(seq))
            bot.send(SB_USE_ITEM_ON, body)
            seq += 1
            placed += 1
    packets = []
    deadline = time.time() + 30.0
    while len(all_of(packets, 0x08)) < placed and time.time() < deadline:
        packets += drain(bot, 0.5)
    updates = all_of(packets, 0x08)  # block_update
    assert len(updates) == placed, f"expected {placed} block updates, got {len(updates)}"
    return placed


def all_of(packets, pid):
    return [b for p, b in packets if p == pid]


def feedback(packets):
    """The literal SystemChats (command replies), joined."""
    out = []
    for body in all_of(packets, CB_SYSTEM_CHAT):
        try:
            out.append(chat_text(body))
        except AssertionError:
            pass
    return " ".join(out)


def summon_cow(bot, timeout=2.0):
    """`/summon cow 1`, returning the Add Entity packet as soon as it arrives."""
    send_chat_command(bot, "summon cow 1")
    end = time.time() + timeout
    while time.time() < end:
        for body in all_of(drain(bot, 0.15), CB_SPAWN_ENTITY):
            cow = read_spawn(body)
            if cow["type"] == ET_COW:
                return cow
    raise AssertionError("no cow spawn arrived for /summon cow 1")


def read_spawn(body):
    """Add Entity: [varint eid][uuid 16][varint type][f64 x y z][LpVec3]
    [u8 pitch][u8 yaw][u8 head yaw][varint data]."""
    eid, pos = rd_varint(body, 0)
    pos += 16
    etype, pos = rd_varint(body, pos)
    x, y, z = struct.unpack(">ddd", body[pos:pos + 24])
    # The tail (velocity, angles, data) is not read here: the id, the type and
    # the position are all this test compares.
    return {"entity": eid, "type": etype, "x": x, "y": y, "z": z}


def read_entity_event(body):
    assert len(body) == 5, f"entity_event body is {len(body)} bytes"
    return struct.unpack(">i", body[0:4])[0], body[4]


def note_positions(packets, positions):
    """Every entity_position_sync (0x23, 26.3): [varint eid][varint PositionPath
    type = LINEAR 0][f64 x y z][f32 yaw][f32 pitch][bool onGround]."""
    for body in all_of(packets, CB_ENTITY_POSITION_SYNC):
        eid, pos = rd_varint(body, 0)
        _, pos = rd_varint(body, pos)  # PositionPath.Type.LINEAR
        x, y, z = struct.unpack(">ddd", body[pos:pos + 24])
        positions[eid] = (x, y, z)
    return positions


MOB_LINE = re.compile(
    r"^(?P<type>[a-z0-9_:]+)#(?P<id>\d+) at "
    r"(?P<x>-?[\d.]+),(?P<y>-?[\d.]+),(?P<z>-?[\d.]+)(?P<tail>.*)$")


def parse_mob_line(text):
    """One `/mobs list` line: `<type>#<id> at x,y,z` plus the animal tail
    (`age= love= mate= parent=`) and/or a villager's (`profession= level= xp=`)."""
    m = MOB_LINE.match(text.strip())
    if not m:
        return None
    mob = {"id": int(m.group("id")), "type": m.group("type"),
           "x": float(m.group("x")), "y": float(m.group("y")), "z": float(m.group("z"))}
    for token in m.group("tail").split():
        key, _, value = token.partition("=")
        if key in ("age", "love", "mate", "parent", "level", "xp"):
            mob[key] = int(value)
        elif key:
            mob[key] = value
    return mob


def read_mobs(bot, timeout=1.2):
    """`/mobs list` as {id: parsed line}."""
    send_chat_command(bot, "mobs list")
    packets = drain(bot, timeout)
    mobs = {}
    for body in all_of(packets, CB_SYSTEM_CHAT):
        try:
            text = chat_text(body)
        except AssertionError:
            continue
        parsed = parse_mob_line(text)
        if parsed is not None:
            mobs[parsed["id"]] = parsed
    return mobs


def gap(a, b):
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2) ** 0.5


def pen_cells(center, levels, radius=2):
    """The stone ring that walls the cell at `center` off: the outermost cells
    at `radius` around it, at each of `levels` body cells, and nothing inside.

    The default radius is 2, so the mob has a 3x3 interior to stand in and its
    box never overlaps a wall: this save/load test reads the same position back
    from a restart, and a mob whose box overlapped a wall at the save instant
    would be unstuck on load (measured: a 1-cell pen lifted the cow to y=66).
    The ring still stops the *pathfinder* — the partner outside is unreachable,
    and the mob paces its interior — and it was a burial the collision rule
    retired: `Entity.move` clips an axis only against the shape slices beyond
    the box's leading face, so a block the box already overlaps is not a
    collision at all and a buried mob walks out of its own cell
    (tests/mob_test.cpp's `test_overlapping_box_walks_out` pins that).
    """
    cx, cy, cz = center
    return [(cx + dx, cy + level, cz + dz)
            for level in range(levels)
            for dx in range(-radius, radius + 1)
            for dz in range(-radius, radius + 1)
            if max(abs(dx), abs(dz)) == radius]


def place_cells(bot, cells, block="minecraft:stone"):
    """Send one `/setblock` per cell and wait for every answer."""
    for (bx, by, bz) in cells:
        send_chat_command(bot, f"setblock {bx} {by} {bz} {block}")
    packets = []
    deadline = time.time() + 20.0
    while feedback(packets).count("Changed the block") < len(cells) and time.time() < deadline:
        packets += drain(bot, 0.3)
    got = feedback(packets).count("Changed the block")
    assert got == len(cells), f"{len(cells)} setblocks, {got} answers: {feedback(packets)}"
    return packets


def inside_pen(pos, cell, radius=2):
    """Whether a mob stands inside the ring's interior (not "still": an in-love
    cow keeps a live path to the partner outside and paces the pen, and against a
    1-cell ring it bounces, so the claim to check is containment)."""
    cx, cy, cz = cell
    reach = radius - 0.5 - 0.45 + 0.05  # interior half-width minus the box
    return (abs(pos[0] - (cx + 0.5)) <= reach and
            abs(pos[2] - (cz + 0.5)) <= reach and
            -0.05 <= pos[1] - cy <= 1.6)


def hold_still(bot, eids, positions, levels=2, radius=2, rounds=3):
    """Wall each mob into its own pen, then confirm it stayed inside.

    The cell comes from the mob's last position (`/summon`'s spawn packet, or a
    position sync) and may be a fraction stale, so this pens, watches for two
    seconds and re-pens anything that left its ring (up to `rounds`). The ring
    leaves the mob its interior air, so its box overlaps no wall and
    `mob_place`'s unstuck pass is not in play; a held pair can still *pair* —
    the BreedGoal partner scan is a distance test with no line-of-sight
    condition — but with the rings six blocks apart and each interior three
    wide, the closest the pair can stand is still outside the three blocks the
    breed itself needs, so no birth can race the restart. The stone is part of
    the saved world, so the pair is still penned after the restart too.
    """
    pens = {}
    for _ in range(rounds):
        for eid in eids:
            pos = positions[eid]
            cell = (math.floor(pos[0]), math.floor(pos[1]), math.floor(pos[2]))
            if pens.get(eid) == cell:
                continue
            place_cells(bot, pen_cells(cell, levels, radius))
            pens[eid] = cell
        deadline = time.time() + 2.0
        while time.time() < deadline:
            note_positions(drain(bot, 0.25), positions)
        if all(inside_pen(positions[eid], pens[eid], radius) for eid in eids):
            return pens
    assert False, (f"could not pen the mobs: {positions} (pens {pens})")


def paired(mobs, a, b):
    return (a in mobs and b in mobs and mobs[a].get("mate") == b and mobs[b].get("mate") == a
            and mobs[a].get("love", 0) > 0 and mobs[b].get("love", 0) > 0)


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
    if port is None:
        raise RuntimeError("server did not start")
    return port


START = time.time()


def step(text):
    print(f"[{time.time() - START:6.2f}s] {text}", flush=True)


def boot(binary, data_root, world, env, motd):
    return wire_client.start_server(
        [binary, "--host", HOST, "--port", "0", "--motd", motd, "--offline-mode",
         "--gamemode", "1", "--data", data_root, "--world", world, "--no-flat-world"],
        wait_listening, env=env)


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}  (no binary needed)",
              file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-mob-persist-")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
    # The population under test is exactly what this test summons, and the save
    # under test is the shutdown one (the autosave would race it).
    env["PLUSMC_MOB_SPAWNING"] = "0"
    env["PLUSMC_AUTOSAVE_SECS"] = "0"

    proc = None
    proc2 = None
    try:
        step(f"world dir: {world}")
        proc, port = boot(binary, data_root, world, env, "mob persistence test")
        bot = Bot("MobSaveA", port)
        wire_client.record(bot, note="mob persistence: summon, feed and pair — the state a restart must keep")
        bot.join()
        bot.start_reader()
        for _ in range(3):
            bot.move_to(PLAYER_X, float(FLOOR_Y + 1), PLAYER_Z)
            time.sleep(0.05)
        step("-> placing the 9x9 stone floor (81 real use_item_on packets)")
        assert build_floor(bot) == 81
        set_creative_slot(bot, 36, 64, ITEM_WHEAT)  # hotbar 0: the food
        set_creative_slot(bot, 37, 64, ITEM_STONE)
        for _ in range(3):
            bot.move_to(PLAYER_X, float(FLOOR_Y + 1), PLAYER_Z)
            time.sleep(0.05)
        bot.send(SB_SET_CARRIED_ITEM, struct.pack(">H", 0))
        drain(bot, 0.4)

        # --- 1. two cows, six blocks apart ------------------------------------
        # Each is summoned on its own, from a player position two blocks west of
        # where the cow lands (`/summon`'s ring starts at +2 x), so the pair is
        # six blocks apart: comfortably outside the three blocks the breed
        # itself needs, which is what keeps the state under test from racing
        # into a birth across the restart. The pen follows each summon at
        # once, before the cow can stroll (`hold_still` re-pens if it did).
        step("-> /summon cow 1, twice, six blocks apart")
        cows = []
        for player_x in (PLAYER_X, -5.5):
            for _ in range(3):
                bot.move_to(player_x, float(FLOOR_Y + 1), PLAYER_Z)
                time.sleep(0.05)
            cows.append(summon_cow(bot))
        ids = sorted(c["entity"] for c in cows)
        positions = {c["entity"]: (c["x"], c["y"], c["z"]) for c in cows}
        assert gap(positions[ids[0]], positions[ids[1]]) > 5.0, positions
        step(f"  OK cows: eids {ids} at {positions[ids[0]][0]:.2f} / {positions[ids[1]][0]:.2f} x")

        # --- 2. hold them still, then feed both -------------------------------
        hold_still(bot, ids, positions)
        step("-> a stone pen around each cow: paired but unable to close")
        for eid in ids:
            # The player has to stand beside the cow it feeds: the interaction
            # range is 3.0 from the eyes to the box, and the pair is six apart.
            pos = positions[eid]
            for _ in range(3):
                bot.move_to(pos[0] + 2.0, float(FLOOR_Y + 1), PLAYER_Z)
                time.sleep(0.05)
            drain(bot, 0.3)
            interact(bot, eid)
            packets = drain(bot, 0.8)
            events = [read_entity_event(b) for b in all_of(packets, CB_ENTITY_EVENT)]
            assert (eid, ENTITY_EVENT_LOVE) in events, f"no heart burst for {eid}: {events}"
        step(f"  OK both cows in love (heart event {ENTITY_EVENT_LOVE} for each)")

        # --- 3. the pair, on the wire ----------------------------------------
        step("-> /mobs list: each cow's love clock and its partner's id")
        mobs = {}
        deadline = time.time() + 15.0
        while time.time() < deadline:
            mobs = read_mobs(bot)
            if paired(mobs, ids[0], ids[1]):
                break
        assert paired(mobs, ids[0], ids[1]), f"the cows never paired: {mobs}"
        before = {eid: dict(mobs[eid]) for eid in ids}
        for eid in ids:
            assert before[eid]["age"] == 0, before[eid]
            assert before[eid]["love"] > 0, before[eid]
        note_positions(drain(bot, 0.3), positions)
        held_gap = gap((before[ids[0]]["x"], before[ids[0]]["y"], before[ids[0]]["z"]),
                       (before[ids[1]]["x"], before[ids[1]]["y"], before[ids[1]]["z"]))
        assert held_gap > 3.0, (
            f"the held pair must stay outside the breed distance: {held_gap:.2f} blocks")
        step(f"  OK pair: love {before[ids[0]]['love']}/{before[ids[1]]['love']}, "
             f"mate {before[ids[0]]['mate']}/{before[ids[1]]['mate']}")
        bot.close()

        # --- 4. restart -------------------------------------------------------
        # From here on it is the server's own side: the shutdown save, the file,
        # the second boot that loads it. A transcript holds phase 1 — the join,
        # the summons, the feedings, the list — and a replay has no server to
        # save or reload anything.
        if wire_client.skip_server_check(
                "the restart: the shutdown save, mobs.dat, and the state read back "
                "from the second boot"):
            print("\nPASS: mob persistence (phase 1 replayed: summon, feed, pair)")
            return 0
        step("-> SIGTERM: the shutdown save")
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            print("FAIL: server did not exit", file=sys.stderr)
            return 1
        proc = None
        saved = pathlib.Path(world) / "mobs.dat"
        assert saved.exists() and saved.stat().st_size > 0, f"no population file: {saved}"
        step(f"  OK {saved.name}: {saved.stat().st_size} bytes")

        step("-> second boot, same world dir")
        proc2, port2 = boot(binary, data_root, world, env, "mob persistence test 2")
        bot2 = Bot("MobSaveB", port2)
        bot2.join()
        bot2.start_reader()
        for _ in range(3):
            bot2.move_to(PLAYER_X, float(FLOOR_Y + 1), PLAYER_Z)
            time.sleep(0.05)

        # --- 5. the same mobs, ids and references -----------------------------
        mobs2 = read_mobs(bot2)
        assert set(mobs2) == set(ids), (
            f"the loaded population must be the saved one: {mobs2} vs {before}")
        for eid in ids:
            assert mobs2[eid]["type"] == before[eid]["type"], (mobs2[eid], before[eid])
            assert 0 < mobs2[eid]["love"] <= before[eid]["love"], (mobs2[eid], before[eid])
            assert mobs2[eid]["age"] == 0, (mobs2[eid], before[eid])
            assert mobs2[eid]["mate"] == before[eid]["mate"], (
                f"the breed partner reference must survive: {mobs2[eid]} vs {before[eid]}")
            # The same mob stands in the same pen: it is in love and paces the
            # three-wide interior its ring leaves (and bounces against the wall
            # on the ticks the jump control fires), so the exact position is not
            # a constant -- but a fresh spawn at the world spawn, or a mob that
            # fell through the reloaded floor, would not be within a few blocks.
            moved = gap((mobs2[eid]["x"], mobs2[eid]["y"], mobs2[eid]["z"]),
                        (before[eid]["x"], before[eid]["y"], before[eid]["z"]))
            assert moved < 3.0, (
                f"eid {eid} moved {moved:.2f} blocks across the restart: "
                f"{mobs2[eid]} vs {before[eid]}")
        step(f"  OK same ids {ids}, love {mobs2[ids[0]]['love']}/{mobs2[ids[1]]['love']}, "
             f"mate {mobs2[ids[0]]['mate']}/{mobs2[ids[1]]['mate']}")

        # --- 6. a new mob takes a brand-new id --------------------------------
        step("-> /summon cow 1 after the restart: the counter must not reuse an id")
        send_chat_command(bot2, "summon cow 1")
        packets = drain(bot2, 1.5)
        fresh = [read_spawn(b) for b in all_of(packets, CB_SPAWN_ENTITY)]
        fresh = [c for c in fresh if c["type"] == ET_COW]
        assert len(fresh) == 1, f"expected one fresh cow, got {[c['type'] for c in fresh]}"
        fresh_id = fresh[0]["entity"]
        assert fresh_id > max(ids), f"a reused id! fresh {fresh_id}, saved {ids}"
        step(f"  OK fresh eid {fresh_id} (saved ids {ids})")

        bot2.close()
        proc2.terminate()
        try:
            proc2.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc2.kill()
            proc2.wait()
        proc2 = None
        print("\nPASS: mob persistence test (ids, love clocks and mate links across a restart)")
        return 0
    finally:
        for p in (proc, proc2):
            if p is not None:
                p.kill()
                try:
                    p.wait(timeout=5)
                except Exception:
                    pass
        shutil.rmtree(world, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
