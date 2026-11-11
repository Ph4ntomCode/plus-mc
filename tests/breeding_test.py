#!/usr/bin/env python3
"""Integration test: Phase 4 milestone I (animal breeding) over the real wire protocol.

An `Animal` is an `AgeableMob` with a food tag, and this test drives the whole
Player -> animal -> child loop against a void world with a stone floor the test
itself lays (81 real `use_item_on` placements, exactly like mobs_test.py):

  * `/summon cow 2` answers with two Add Entity (0x01, type 30) and their health
    words (0x63, LivingEntity.DATA_HEALTH = index 9, a FLOAT),
  * the *range* gate is Player.isWithinEntityInteractionRange: the same right
    click from twenty-six blocks above is refused — no heart burst, no item
    spent — and lands once the player is back on the floor,
  * the *food* gate is `Animal.isFood`: a stone in the hand is refused the same
    way (no EntityEvent, no slot echo), and wheat is what fires,
  * a successful feeding on an adult at age 0 is `setInLove`: exactly one
    entity_event (0x22) with event 18 (the heart burst) and one
    container_set_slot (0x14) that really decrements the held stack,
  * a second feeding while it is still in love is refused (`canFallInLove` is
    `inLove <= 0`): no second burst and nothing spent,
  * the two in-love cows walk together and breed: the child's Add Entity, the
    `DATA_BABY_ID` bit (set_entity_data index 16, BOOLEAN serializer 8) that is
    the *only* thing that makes a client draw the small box, the child's health
    word, the parents' heart burst, and 1..7 experience orbs at the parent's
    position (which the player collects, moving the bar),
  * the parents are left on the 6000-tick post-breeding age, so a feeding on one
    of them a moment later is refused: the birth is not repeatable,
  * the *newborn* keeps to its *nearest* parent (`FollowParentGoal`): the parent
    it is not with is killed with the test's own sword swings and a fresh adult
    is summoned seven and a half blocks past the survivor, so the child's two
    live candidates sit at distances the test chose. Each of those placements
    is caught in a ring of stone and the landing checked against it, and every
    ring comes down before the window starts: the window is pen-free, both
    adults stroll (it asserts each of them walked), and the claim is about a
    *moving* adult rather than about a wall. Over forty seconds of world clock
    the child stays inside its nearest parent's sixteen-block band, has a
    sample inside the three-block ball in every fully sampled eight-second
    stretch and in the last eight seconds, and does not drift into a farther
    band in the tail — the shape the goal's stop-inside-three refusal makes,
    and one no-goal wandering on the same floor produces,
  * the *baby* is fed its way up: each feeding is
    `getSpeedUpSecondsWhenFeeding`'s floored tenth of what is left, the item is
    spent even when the feeding is worth nothing (`Mob.usePlayerItem` spends
    before `ageUp` runs), and the sixty-two feeds it takes to empty the stack
    stall the age below 200 ticks — the natural `++age` finishes the job and the
    server clears the bit with a second set_entity_data on that same index,
  * the grown-up child is an adult at age 0 that never was in love, so the same
    wheat now starts a love instead of an age-up: one more heart burst,
  * `/mobs` reports the three cows and never a fourth: the child is the
    *caller's* to create, and the cooldown is what keeps a pair to one birth.

The server runs with `PLUSMC_SURVIVAL_CLOCK=packet`, so the player's clock is
this test's own packets (one tick each); the *world* clock — the mobs' AI, the
birth, the orbs — is the server's real 50 ms tick, which is what the drains and
the waits pace. `PLUSMC_MOB_SPAWNING=0` keeps the natural spawner out of the
population the assertions count.

Run: breeding_test.py /path/to/plusmc
"""
import math
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nbt_component import chat_text  # noqa: E402
import wire_client  # noqa: E402
from wire_client import make_frame, read_packet  # noqa: E402

PROTO = 777
HOST = "127.0.0.1"

# minecraft:entity_type ids (src/world/mob_table.h).
ET_COW = 30
ET_EXPERIENCE_ORB = 50

ITEM_STONE = 1
ITEM_WHEAT = 1066
ITEM_DIAMOND_SWORD = 1050

# Clientbound packet ids.
CB_SPAWN_ENTITY = 0x01
CB_CONTAINER_SET_SLOT = 0x14
CB_ENTITY_EVENT = 0x22
CB_SET_ENTITY_DATA = 0x65
CB_ENTITY_POSITION_SYNC = 0x23
CB_REMOVE_ENTITIES = 0x4E
CB_SYSTEM_CHAT = 0x7C
CB_UPDATE_ADVANCEMENTS = 0x85
CB_KEEP_ALIVE = 0x2D
CB_AWARD_STATS = 0x03

# Serverbound packet ids.
SB_ATTACK = 0x01
SB_CHAT_COMMAND = 0x07
SB_CLIENT_COMMAND = 0x0C
SB_INTERACT = 0x1A
SB_KEEP_ALIVE = 0x1C
SB_POSITION = 0x1E
SB_SET_CARRIED_ITEM = 0x36
SB_FLYING = 0x21
SB_SET_CREATIVE_SLOT = 0x39
SB_USE_ITEM_ON = 0x42

# Stat ids (src/world/stats.h): `StatType` CUSTOM is 8 in registry order, and
# `animals_bred` is the 34th entry of `BuiltInRegistries.CUSTOM_STAT`.
STAT_CUSTOM = 8
STAT_ANIMALS_BRED = 34

# Synched-entity-data ids (src/server/entity_packets.h).
HEALTH_INDEX = 9
FLOAT_SERIALIZER = 3
BABY_INDEX = 16
BOOLEAN_SERIALIZER = 8
ENTITY_EVENT_LOVE = 18
ENTITY_EVENT_DEATH = 3

# How far past the held parent the summoned adult goes in section 7: enough for
# their three-block balls to stand apart (the child can be inside one without
# being inside the other), and small enough that the child starts inside the
# eight-block scan of both. That second half is a guarantee rather than a hope:
# section 7 pins the child and aims the placement through it, so the gap the
# goal's own query reads is this distance minus the child's own offset from the
# held parent -- arithmetic, not a walk the test waits out.
FAR_ADULT_DISTANCE = 7.5

FLOOR_Y = 63  # the mobs stand *on* this layer, so the player's feet are at 64
PLAYER_X = 0.5
PLAYER_Z = 0.5
# The corridor `build_floor` lays: its interior cells are x = -11..11 by
# z = -3..3, and the rim occupies the edge rows and columns around them. The
# bounds are the last *interior* cell index, which is what a summon's landing is
# checked against (a cell index is its floor, so a cow at x = 11.5 is on interior
# cell 11; cell 12 is the rim's column).
CORRIDOR_INTERIOR_X = 11
CORRIDOR_INTERIOR_Z = 3
# A height no interaction can reach (the gate is 3.0 + a 3.0 lag buffer from the
# eye to the entity's box) but that no packet from here can fall out of: the
# engine only moves a player on that player's own movement packets.
FAR_Y = 90.0

# The held stack of wheat: the first feeding, the second (refused), the other
# cow's, and then the sixty-two a stall needs — the stall is reached after ~50
# feeds (the feeding's worth is `floor(remaining / 200)` seconds) and every feed
# after it is spent for nothing, which is vanilla's own arithmetic.
WHEAT_STACK = 64

# `BreedGoal.canUse` searches eight blocks for a partner, and an in-love animal
# whose search comes up empty has no movement goal at all (the breed goal
# outranks the stroll, so `breed_step`'s early return leaves it standing). Two
# cows that strolled apart before the first feeding therefore never breed, and
# the love just counts down: the scene, not the tick, is what has to keep the
# pair inside that range. A block of margin covers the stroll between the check
# and the second feeding (that window is under two seconds).
SCENE_BREED_GAP = 7.0


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


# --- decoders ---------------------------------------------------------------

def read_spawn(body):
    """Add Entity: [varint eid][uuid 16][varint type][f64 x y z][LpVec3]
    [u8 pitch][u8 yaw][u8 head yaw][varint data]."""
    eid, pos = rd_varint(body, 0)
    uuid = body[pos:pos + 16]
    pos += 16
    etype, pos = rd_varint(body, pos)
    x, y, z = struct.unpack(">ddd", body[pos:pos + 24])
    pos += 24
    first = body[pos]
    if first == 0:
        pos += 1
    else:
        pos += 6
        if first & 0x4:
            _, pos = rd_varint(body, pos)
    pitch, yaw, head = body[pos], body[pos + 1], body[pos + 2]
    pos += 3
    data, pos = rd_varint(body, pos)
    assert pos == len(body), f"spawn_entity: {len(body) - pos} trailing byte(s)"
    return {"entity": eid, "uuid": uuid, "type": etype, "x": x, "y": y, "z": z,
            "pitch": pitch, "yaw": yaw, "head": head, "data": data}


def read_entity_data(body, wanted=None):
    """[varint eid] then [u8 index][varint serializer][value]... to 0xFF.

    The serializers this test reads are the FLOAT health word, the BOOLEAN baby
    bit, and the INT some other entity data carries; anything else is an error
    rather than a guess.
    """
    eid, pos = rd_varint(body, 0)
    if wanted is not None and eid != wanted:
        return None
    values = {}
    while True:
        index = body[pos]
        pos += 1
        if index == 0xFF:
            break
        serializer, pos = rd_varint(body, pos)
        if serializer == 0:  # BYTE (the shared-flags byte, Entity index 0)
            values[index] = body[pos]
            pos += 1
        elif serializer == FLOAT_SERIALIZER:
            values[index] = struct.unpack(">f", body[pos:pos + 4])[0]
            pos += 4
        elif serializer == 1:  # INT
            v, pos = rd_varint(body, pos)
            values[index] = v
        elif serializer == BOOLEAN_SERIALIZER:
            values[index] = body[pos] != 0
            pos += 1
        else:
            raise AssertionError(f"entity data index {index}: serializer {serializer}")
    assert pos == len(body), f"set_entity_data: {len(body) - pos} trailing byte(s)"
    return eid, values


def read_entity_event(body):
    assert len(body) == 5, f"entity_event body is {len(body)} bytes"
    return struct.unpack(">i", body[0:4])[0], body[4]


def tick(bot, count):
    """Advance the player's own survival clock by `count` game ticks.

    The server runs with `PLUSMC_SURVIVAL_CLOCK=packet`, so this clock *is* these
    packets — one tick each — while the world clock is the server's own 50 ms
    tick in real time. A swing's charge is the former (7.0 damage for a charged
    sword swing against 1.62 for a barely-moved one) and a mob's hurt window is
    the latter, which is why the kill loop below sends fourteen of these and then
    waits out a second of wall clock."""
    for _ in range(count):
        bot.send(SB_FLYING, bytes([1]))


def read_remove_entities(body):
    """remove_entities (0x4D): [varint count][varint eid]..."""
    count, pos = rd_varint(body, 0)
    ids = []
    for _ in range(count):
        eid, pos = rd_varint(body, pos)
        ids.append(eid)
    assert pos == len(body), f"remove_entities: {len(body) - pos} trailing byte(s)"
    return ids


def note_positions(packets, positions):
    """Record every entity_position_sync (0x23) a batch carries, so a step that
    has to stand next to a mob that has walked can find it again. 26.3 body:
    [varint eid][varint PositionPath type = LINEAR 0][f64 x y z]
    [f32 yaw][f32 pitch][bool onGround]."""
    for body in all_of(packets, CB_ENTITY_POSITION_SYNC):
        eid, pos = rd_varint(body, 0)
        _, pos = rd_varint(body, pos)  # PositionPath.Type.LINEAR
        x, y, z = struct.unpack(">ddd", body[pos:pos + 24])
        positions[eid] = (x, y, z)
    return positions


def read_set_slot(body):
    """container_set_slot (0x14): [varint container][varint state][i16 slot]
    then the stack — [varint count][varint item id][varint added][varint
    removed] (a wheat stack carries no components)."""
    cid, pos = rd_varint(body, 0)
    state, pos = rd_varint(body, pos)
    slot = struct.unpack(">h", body[pos:pos + 2])[0]
    pos += 2
    count, pos = rd_varint(body, pos)
    item = None
    if count > 0:
        item, pos = rd_varint(body, pos)
        added, pos = rd_varint(body, pos)
        for _ in range(added):
            _, pos = rd_varint(body, pos)
            _, pos = rd_varint(body, pos)  # a component's payload is not decoded
        removed, pos = rd_varint(body, pos)
        assert removed == 0, removed
    assert pos == len(body), f"container_set_slot: {len(body) - pos} trailing byte(s)"
    return {"container": cid, "state": state, "slot": slot, "count": count, "item": item}


def read_award_stats(body):
    """award_stats (0x03): [varint count], then per entry
    [varint stat type][varint value id][varint value] — `Stat.STREAM_CODEC` is
    the STAT_TYPE registry id dispatched to that type's own value registry, so
    a custom stat's entry reads (8, its CUSTOM_STAT id, count)."""
    count, pos = rd_varint(body, 0)
    out = []
    for _ in range(count):
        stype, pos = rd_varint(body, pos)
        sid, pos = rd_varint(body, pos)
        value, pos = rd_varint(body, pos)
        out.append((stype, sid, value))
    assert pos == len(body), f"award_stats: {len(body) - pos} trailing byte(s)"
    return out


def all_of(packets, pid):
    return [b for p, b in packets if p == pid]


def feedback(packets):
    """The literal SystemChats (command replies)."""
    out = []
    for body in all_of(packets, CB_SYSTEM_CHAT):
        try:
            out.append(chat_text(body))
        except AssertionError:
            pass
    return " ".join(out)


def mob_listing(text):
    """`/mobs list`'s lines as `{entity id: (x, y, z)}`.

    One line per mob is `<type>#<id> at x,y,z` followed by that animal's state
    (age, love countdown, breed partner, follow parent). Only the position is
    parsed here; the birth's timeout message prints the whole listing, because
    the state suffix is what separates the ways a lost birth can lose.
    """
    out = {}
    for match in re.finditer(r"#(\d+) at (-?\d+(?:\.\d+)?),(-?\d+(?:\.\d+)?),"
                             r"(-?\d+(?:\.\d+)?)", text):
        out[int(match.group(1))] = (float(match.group(2)), float(match.group(3)),
                                    float(match.group(4)))
    return out


class Bot:
    def __init__(self, name, port):
        self.name = name
        self.sock = wire_client.transport(HOST, port, timeout=10)
        self.sock.settimeout(10)
        self.threshold = -1
        self.reader = wire_client.Reader(self.sock, self)
        self.entity_id = None

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
            elif pid == 0x0B:
                batch, _ = rd_varint(body)
                self.send(0x0B, varint(batch))
                break
        return True

    def move_to(self, x, y, z, ground=1):
        self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([ground]))


def drain(bot, timeout=1.5):
    """Every packet the reader thread queued in the next `timeout` seconds.

    A reply window ends when the server has been quiet for `quiet` seconds
    rather than always burning the whole window; `timeout` stays the bound a
    missing reply is judged against, and the reader is started right after the
    join below so every window in the session is one of these. `quiet` is 0.3
    against the server's 50 ms world tick: four whole ticks of silence before a
    window is called closed, so a reply that straddles two ticks (the two
    cows' spawns, a birth's Add Entity and the baby bit behind it) is still the
    one batch an assertion reads.
    """
    return wire_client.drain_quiet(bot, quiet=0.3, limit=timeout)


def send_chat_command(bot, command):
    payload = command.encode()
    bot.send(SB_CHAT_COMMAND, varint(len(payload)) + payload)


def mob_gap(listing, a_id, b_id):
    """The distance between two entities of a `/mobs list` listing, or None."""
    if a_id not in listing or b_id not in listing:
        return None
    (ax, ay, az) = listing[a_id]
    (bx, by, bz) = listing[b_id]
    return math.sqrt((ax - bx) ** 2 + (ay - by) ** 2 + (az - bz) ** 2)


def assert_inside_corridor(cow, where="summon"):
    """A summoned cow must land on the corridor's floor, inside the rim.

    Two things are checked, both of them properties of the scene rather than of
    the tick: the feet are at the floor's surface (64 — feet at 65 or 66 are a
    cow in the rim or on its top) and the feet cell is interior
    (`CORRIDOR_INTERIOR_X`/`_Z`). A summon blocked by the rim is nudged onto the
    floor by `mob_place`, so a failure here means the cow was placed somewhere
    the scene cannot contain — not that the mob's AI misbehaved.
    """
    feet = FLOOR_Y + 1
    x, y, z = cow["x"], cow["y"], cow["z"]
    assert y == feet, (
        f"{where}: a cow landed at y={y}, not on the floor ({feet}) — "
        f"`mob_place`'s unstuck loop lifted it out of a rim cell")
    assert -CORRIDOR_INTERIOR_X <= math.floor(x) <= CORRIDOR_INTERIOR_X, (
        f"{where}: a cow landed on cell x={math.floor(x)}, outside the interior "
        f"(-{CORRIDOR_INTERIOR_X}..{CORRIDOR_INTERIOR_X}) — past the rim")
    assert -CORRIDOR_INTERIOR_Z <= math.floor(z) <= CORRIDOR_INTERIOR_Z, (
        f"{where}: a cow landed on cell z={math.floor(z)}, outside the interior "
        f"(-{CORRIDOR_INTERIOR_Z}..{CORRIDOR_INTERIOR_Z}) — past the rim")


def summon_cow_pair(bot):
    """`/summon cow 2`, read back off the wire as its two cows.

    **The pair is summoned from the corridor's centre, not from wherever the
    caller left the player.** `/summon`'s ring is two blocks out along x at the
    summoner's own z (`session.cpp`'s `summon` branch: `i/count` turns of a
    circle), so where the player stands decides where the pair lands: from a
    stance near either end one cow is placed past the rim and over the void.
    `mob_place` nudges a summon blocked by the rim back onto the corridor floor,
    but a summon over the void is not blocked — it is a fall, the same as
    vanilla's — so the scene still summons from one known place. The teleport and
    the summon are two packets on one ordered stream, so the server has moved
    the player before it reads the summon.

    The pair lands at x = +-2 (four apart), each with the FULL health word
    (`SET_ENTITY_DATA`'s index 9). The ids come from the Add Entity packets
    because they are what the feedings and the birth wait address; a repaired
    scene (see the birth's gap check) gets new ones.
    """
    send_chat_command(bot, f"tp {PLAYER_X} 64.0 {PLAYER_Z}")
    drain(bot, 0.4)
    send_chat_command(bot, "summon cow 2")
    packets = drain(bot, 1.5)
    assert "Summoned 2 minecraft:cow" in feedback(packets), feedback(packets)
    cows = [read_spawn(b) for b in all_of(packets, CB_SPAWN_ENTITY)]
    cows = [c for c in cows if c["type"] == ET_COW]
    assert len(cows) == 2, f"expected two cows, got {[c['type'] for c in cows]}"
    healths = {}
    for body in all_of(packets, CB_SET_ENTITY_DATA):
        got = read_entity_data(body)
        if got is not None:
            healths[got[0]] = got[1]
    for cow in cows:
        assert healths.get(cow["entity"], {}).get(HEALTH_INDEX) == 10.0, (
            f"a cow's health word must be 10.0: {healths.get(cow['entity'])}")
        assert_inside_corridor(cow)
    return cows[0], cows[1]


def interact(bot, eid):
    """ServerboundInteractPacket (0x1a): [varint entity id][varint main hand]
    [LpVec3 location][bool usingSecondaryAction]. The location is the zero
    vector (one 0x00 byte), which is what a client sends when it has no click
    point to report, and what `Animal.mobInteract` never looks at."""
    bot.send(SB_INTERACT, varint(eid) + varint(0) + b"\x00" + bytes([0]))


def set_creative_slot(bot, slot, count, item):
    """ServerboundSetCreativeModeSlot (0x38): [i16 slot][ItemStack] with the
    delimited (untrusted) component patch — two zero varints for no changes."""
    bot.send(SB_SET_CREATIVE_SLOT,
             struct.pack(">h", slot) + varint(count) + varint(item) + b"\x00\x00")


def build_floor(bot, x_radius=13, z_radius=5, y=FLOOR_Y, rim=2):
    """Lays a stone platform with real `use_item_on` packets: clicking the cell
    below the target with face=up places one block above it.

    The platform is long in x and nine wide in z. The z span is what the
    player's own reach covers from one place — the click lands on the cell
    *below* the target, four blocks out at the ends — while the walk along x is
    what carries the player to the next row. The length is for the follow
    section: two parents that have to stroll some distance apart need the room.

    **The platform is walled, `rim` blocks high.** This world is empty, so the
    platform is the only ground in it and a mob that steps off the edge falls
    for good — a parent did exactly that once in a handful of runs, and its head
    cell came back "That position is outside the world" from the burial below.
    The stroll can reach the edge and a push never consults a path, so the edge
    is built up. Two blocks is a **closed wall** to a strolling mob, not a
    staircase: `kMobJumpVelocity` peaks at about 0.9 of a block, so a cow's feet
    cannot get from the floor (64) onto the rim's top (66), and a jump that is
    short lands it back on the floor. `breeding_scene_soak.py` sampled every
    cow's feet once per half second through six twenty-second strolls and read
    64.00 every time — a stroll never lifts a mob onto this rim.

    A *summon* is the one thing that used to get an animal onto this rim: a ring
    point inside the rim column is a buried box, and `mob_place`'s unstuck loop
    used to lift it a block at a time onto the rim's top, one stroll from the
    outside. The unstuck search now prefers the nearest supported free cell on
    the summon's own level, so the same ring point lands on the corridor floor
    (`test_place_nudges_a_mob_off_a_rim`, and the soak's own edge-summon
    preamble); `summon_cow_pair` still summons from the centre and asserts the
    landing is an interior cell, which is the guard that keeps a future change
    from putting a pair somewhere the scene cannot contain.

    Each rim cell is the platform's own edge cell one and two blocks up, which
    the same stance reaches — the click that lays the edge also lays the wall
    over it (the two ends take one extra pass per stance, skipping the corners
    the edge pass already did)."""
    set_creative_slot(bot, 36, 64, ITEM_STONE)
    time.sleep(0.2)
    placed = 0
    seq = 1

    def put(cx, cy, cz):
        """One `use_item_on`: click (cx, cy, cz) face up, place above it."""
        nonlocal placed, seq
        body = (varint(0) + position(cx, cy, cz) + varint(1)
                + struct.pack(">fff", 0.5, 0.0, 0.5) + bytes([0, 0]) + varint(seq))
        bot.send(SB_USE_ITEM_ON, body)
        seq += 1
        placed += 1

    x_edges = (-x_radius + 1, x_radius - 1)
    z_edges = (-z_radius + 1, z_radius - 1)
    for x in range(-x_radius + 1, x_radius):
        bot.move_to(x + 0.5, float(y + 1), 0.5)
        for z in range(-z_radius + 1, z_radius):
            put(x, y - 1, z)                      # the platform cell itself
        for z in z_edges:                         # the wall along both long edges
            for rise in range(1, rim + 1):
                put(x, y - 1 + rise, z)
        if x in x_edges:                          # ...and across both ends
            for z in range(-z_radius + 1, z_radius):
                if z in z_edges:
                    continue
                for rise in range(1, rim + 1):
                    put(x, y - 1 + rise, z)
    # One update per placement comes back, and the server works through them at
    # about a packet a tick, so wait for all of them rather than for a fixed
    # number of seconds.
    packets = []
    deadline = time.time() + 30.0
    while len(all_of(packets, 0x08)) < placed and time.time() < deadline:
        packets += drain(bot, 0.5)
    updates = all_of(packets, 0x08)  # block_update
    assert len(updates) == placed, (
        f"expected {placed} block updates for the floor, got {len(updates)}")
    return placed


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


# How long a wait for a *world*-clock event is allowed before the test calls it
# a failure: a birth, a baby's growth bit, a mob strolling across the room. These
# events are driven by the server's own 50 ms world tick, which — unlike the
# player's clock — cannot be turned into packets (`PLUSMC_SURVIVAL_CLOCK=packet`
# moves the survival tick only), so a loaded machine advances fewer world ticks
# per real second and the same event takes longer in wall time. This is a bound
# on a *failure*, not the expected wait: each loop returns the moment the event
# lands (a birth is a couple of seconds quietly), so headroom costs nothing while
# a tight number turns "the box was busy" into a red suite. 90 s is several times
# the quiet case and absorbs a `ctest -j4` slice sharing four cores.
WORLD_EVENT_TIMEOUT = 90.0


def wait_for_cow_birth(bot, known_ids, positions, timeout=WORLD_EVENT_TIMEOUT):
    """Drain until a cow that is not one of `known_ids` spawns. Returns the
    packets seen, the child's spawn record, its entity data, the heart events,
    the experience orbs and the raw `update_advancements` bodies, and keeps
    `positions` up to date."""
    child = None
    data = []
    hearts = []
    orbs = []
    advances = []
    end = time.time() + timeout
    packets = []
    while time.time() < end and child is None:
        batch = drain(bot, 0.25)
        packets += batch
        note_positions(batch, positions)
        advances += all_of(batch, CB_UPDATE_ADVANCEMENTS)
        for body in all_of(batch, CB_SPAWN_ENTITY):
            spawn = read_spawn(body)
            if spawn["type"] == ET_COW and spawn["entity"] not in known_ids:
                child = spawn
            elif spawn["type"] == ET_EXPERIENCE_ORB:
                orbs.append(spawn)
        for body in all_of(batch, CB_ENTITY_EVENT):
            hearts.append(read_entity_event(body))
        for body in all_of(batch, CB_SET_ENTITY_DATA):
            got = read_entity_data(body)
            if got is not None:
                data.append(got)
    # A birth's packets can land in the same batch as the spawn; keep draining
    # briefly so the baby bit, the health word and the orbs are all seen.
    if child is not None:
        # The birth's advancement flush runs on the *survival* clock, which is
        # packet-driven (`PLUSMC_SURVIVAL_CLOCK=packet`), so a wait that only
        # drains would never see it: these three `SB_FLYING` are what let the
        # session drain the completion and flush it. They are sent *here*, after
        # the birth, rather than throughout the wait above: survival ticks also
        # shift the random stroll the follow check below samples, and that check
        # is a statistical band this test should not perturb.
        tick(bot, 3)
        batch = drain(bot, 0.6)
        packets += batch
        note_positions(batch, positions)
        advances += all_of(batch, CB_UPDATE_ADVANCEMENTS)
        for body in all_of(batch, CB_ENTITY_EVENT):
            hearts.append(read_entity_event(body))
        for body in all_of(batch, CB_SET_ENTITY_DATA):
            got = read_entity_data(body)
            if got is not None:
                data.append(got)
        for body in all_of(batch, CB_SPAWN_ENTITY):
            spawn = read_spawn(body)
            if spawn["type"] == ET_EXPERIENCE_ORB:
                orbs.append(spawn)
    return packets, child, data, hearts, orbs, advances


def wait_for_baby_bit(bot, child_id, want, positions, timeout=WORLD_EVENT_TIMEOUT):
    """Drain until a set_entity_data for `child_id` carries `DATA_BABY_ID`
    = `want`. Returns (seen, True) or (seen, None), with `positions` kept up to
    date."""
    seen = []
    end = time.time() + timeout
    while time.time() < end:
        batch = drain(bot, 0.25)
        seen += batch
        note_positions(batch, positions)
        for body in all_of(batch, CB_SET_ENTITY_DATA):
            got = read_entity_data(body, wanted=child_id)
            if got is not None and BABY_INDEX in got[1] and got[1][BABY_INDEX] is want:
                return seen, True
    return seen, None


def sample_follow(bot, child_id, parent_ids, positions, seconds=40.0, step_s=0.25):
    """Sample the child and both parents every `step_s` seconds of wall clock
    (the *world* clock is the server's real 50 ms tick here), reading the last
    position_sync each entity sent. Returns [(wall time, child_pos, {parent id: pos})].

    The window sends nothing. It is 40 s, and the socket backstop is derived
    from the keep-alive (`main.cpp`: keep-alive + reply window + 1 s, floored at
    vanilla's 30 s), so the 60 s / 300 s cadence `main` pins for this file puts
    the earliest possible reap at 361 s -- past the end of the window. A
    keep-touch would answer a timeout this window cannot reach, and silence is
    also what the sample wants: the survival clock is packet-driven
    (`PLUSMC_SURVIVAL_CLOCK=packet`), so a still player ticks nothing, and those
    ticks are the one thing that would shift the random stroll this window
    measures. Nothing in it reads a survival-tick quantity, and the server's own
    keep-alives (60 s apart) are answered by the reader thread. If this file's
    cadence is ever shortened so that the floor is what applies, the window
    needs traffic again -- the backstop, not the wall clock, is the constraint."""
    samples = []
    end = time.time() + seconds
    while time.time() < end:
        note_positions(drain(bot, step_s), positions)
        if child_id not in positions:
            continue
        if any(pid not in positions for pid in parent_ids):
            continue
        child_pos = positions[child_id]
        samples.append((time.time(), child_pos, {pid: positions[pid] for pid in parent_ids}))
    return samples


def gap(a, b):
    """Entity.distanceToSqr's measure, un-squared: feet centre to feet centre."""
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2) ** 0.5


def in_follow_scan(child_pos, other_pos):
    """The goal's own query, conservatively: `getBoundingBox().inflate(8.0, 4.0,
    8.0)` against the other mob's box. The box admits centres a little over
    eight blocks apart (the two half widths), so eight is still on the safe
    side. Section 7 calls this on a pinned subject, so it is arithmetic there --
    a placement guarantee checked before the summon and read back off the wire
    after it -- rather than a predicate waited on."""
    return (abs(child_pos[0] - other_pos[0]) <= 8.0 and
            abs(child_pos[2] - other_pos[2]) <= 8.0 and
            abs(child_pos[1] - other_pos[1]) <= 3.5)


def pen_cells(center, levels, open_corners=False):
    """The stone ring that walls the cell at `center` off: its neighbours at
    each of `levels` body cells, and nothing inside the ring.

    One cell thick is enough because the ring is not there to stop a collision
    query -- it is there to stop the *pathfinder*: every neighbour is
    unwalkable, so a strolling mob has nowhere to walk and the goal falls
    through to Idle. It is deliberately not a burial: `Entity.move` clips an
    axis only against the shape slices beyond the box's leading face, so a
    block the box already overlaps is not a collision at all and a buried mob
    walks out of its own cell (that rule is pinned by
    `test_overlapping_box_walks_out` in tests/mob_test.cpp).

    `open_corners` leaves the four diagonal neighbours air. That is only safe
    for a mob standing on its cell's exact centre: its box is then 0.05 clear
    of every wall, and a diagonal step is blocked by the two cardinals it has
    to cross, so the mob is pinned by collision even though the pathfinder can
    see a route. The point of the opening is the *other* mob: a follower can
    stand on a corner cell 1.41 blocks from the subject, inside the goal's
    three-block ball, where a closed ring leaves it no closer than 2.0.
    """
    cx, cy, cz = center
    return [(cx + dx, cy + level, cz + dz)
            for level in range(levels)
            for dx in (-1, 0, 1)
            for dz in (-1, 0, 1)
            if (dx, dz) != (0, 0) and not (open_corners and dx != 0 and dz != 0)]


def place_cells(bot, cells, block="minecraft:stone"):
    """Send one `/setblock` per cell and wait for every answer.

    The server works through the queue on its own clock, so the wait is on the
    feedback count rather than a fixed sleep: a `setblock` always answers
    "Changed the block", even into a cell that already holds that state.
    Returns every packet drained while waiting.
    """
    for (bx, by, bz) in cells:
        send_chat_command(bot, f"setblock {bx} {by} {bz} {block}")
    packets = []
    deadline = time.time() + 20.0
    while feedback(packets).count("Changed the block") < len(cells) and time.time() < deadline:
        packets += drain(bot, 0.3)
    got = feedback(packets).count("Changed the block")
    assert got == len(cells), f"{len(cells)} setblocks, {got} answers: {feedback(packets)}"
    return packets


def cell_of(pos):
    """The block cell a feet position stands in."""
    return (math.floor(pos[0]), math.floor(pos[1]), math.floor(pos[2]))


def wait_for_clear_ring(bot, mid, others, positions, levels, timeout=45.0,
                        open_corners=False):
    """Wait until walling eid `mid` in cannot bury any of `others`.

    A following child stands next to the parent it follows (the goal keeps it
    within three blocks), and a ring one cell thick around the parent's cell
    reaches into the child's own feet cell. Stone there is not a wall the child
    is pushed out of: `Entity.move` can only free a box by moving it, and a box
    that starts inside a solid cell has no axis that reduces the overlap, so
    the child stays put for the whole follow window. A child sharing the
    parent's cell is caged the same way -- by the ring around that cell -- so
    the wait is for the child to be at least two cells away in x or z (Chebyshev
    distance from the centre, which is exactly "neither the cell being walled
    nor any of its eight neighbours"). The parent has not been walled yet, so
    both are still walking and the cell is recomputed from the latest sync each
    pass.

    Returns the parent's cell at the moment the ring is clear.
    """
    deadline = time.time() + timeout
    while True:
        cell = cell_of(positions[mid])
        ring = set(pen_cells(cell, levels, open_corners))
        buried = {oid: positions[oid] for oid in others
                  if oid in positions and
                  (cell_of(positions[oid]) == cell or cell_of(positions[oid]) in ring)}
        if not buried:
            return cell
        assert time.time() < deadline, (
            f"eid {mid} cannot be walled while {sorted(buried)} share its cell "
            f"or ring: after {timeout:.0f}s they are still at {cell} (or next to it)")
        note_positions(drain(bot, 0.25), positions)


def hold_mob(bot, mob_ids, positions, levels=2, rounds=3, clear_of=(), open_corners=False):
    """Wall each mob into its own cell and confirm it stopped.

    A wire test cannot move a mob (there is no `/tp @e`, and the player's own
    `/tp` moves nobody but the player), so mobs are taken out of the scene's
    motion with `setblock` instead. `levels` is the height of the ring in body
    cells: two for an adult's 1.4-high box, one for a baby's 0.7-high one. The
    ring leaves the mob's own cell air, so its box overlaps no wall and
    `mob_place`'s unstuck loop is never in play; the mob is stopped because
    every neighbouring cell is unwalkable, not because it is embedded. The cell
    comes from the last `position_sync` and may be a fraction stale, so this
    walls, watches for two seconds, and re-walls any mob whose syncs are still
    moving (up to `rounds`). A re-wall leaves the earlier ring standing, so
    every cell ever placed is returned and `release_mob` takes them all back
    down -- clearing only the last ring once left a child caged by the first
    and it never moved for the whole follow window.

    `clear_of` names mobs that must not be walled in as a side effect: a
    following child shares a cell boundary with the parent it follows, so the
    ring is only placed once the child has strolled out of it (see
    `wait_for_clear_ring`). The placement is re-checked against a fresh sync
    and taken back down if the child stepped in between the check and the
    `setblock`.

    `open_corners` passes through to `pen_cells`: the diagonal neighbours stay
    air. The mob still cannot leave (a diagonal step has to cross a cardinal
    wall this ring fills), but the *pathfinder* can see a standable cell within
    one of the mob's own, which is what lets a following child find a path
    home: `PathNavigation.moveTo(Entity, speed)` ends a path within one cell of
    the target, so a fully closed ring is a target no path can reach.

    Returns ({mob id: cell}, [cell, ...]) once every mob is still: the cell it
    was walled into, and every stone cell placed for it.
    """
    pens = {}
    placed = []
    for _ in range(rounds):
        for mid in mob_ids:
            pos = positions[mid]
            # The platform is the only ground in this world and it is walled, so
            # anything well below it has left the stage — say that in words
            # rather than letting `/setblock` answer "outside the world".
            assert pos[1] > FLOOR_Y - 8.0, (
                f"mob {mid} left the platform: y={pos[1]:.1f} is below the only "
                f"ground in this world (the rim is supposed to stop that)")
            cell = cell_of(pos)
            if pens.get(mid) == cell:
                continue
            if clear_of:
                cell = wait_for_clear_ring(bot, mid, clear_of, positions, levels,
                                           open_corners=open_corners)
            ring = pen_cells(cell, levels, open_corners)
            place_cells(bot, ring)
            if clear_of:
                # The check above read a position sync that can lag the server
                # by a tick. If a protected mob has stepped into a cell the
                # ring just filled, undo it and start the wait over: leaving
                # it there would cage it for the rest of the scene.
                for _ in range(3):
                    note_positions(drain(bot, 0.3), positions)
                    ring_set = set(ring)
                    buried = [oid for oid in clear_of
                              if oid in positions and
                              (cell_of(positions[oid]) == cell or
                               cell_of(positions[oid]) in ring_set)]
                    if not buried:
                        break
                    release_mob(bot, ring)
                    cell = wait_for_clear_ring(bot, mid, clear_of, positions, levels,
                                               open_corners=open_corners)
                    ring = pen_cells(cell, levels, open_corners)
                    place_cells(bot, ring)
                else:
                    assert False, (
                        f"eid {mid}'s ring kept landing on {buried} at cell {cell}")
            pens[mid] = cell
            placed.extend(ring)
        before = {mid: positions[mid] for mid in mob_ids}
        note_positions(drain(bot, 2.0), positions)
        if all(gap(before[mid], positions[mid]) < 0.03 for mid in mob_ids):
            return pens, placed
    assert False, (f"the harness could not hold {mob_ids} still after {rounds} rounds: "
                   f"{pens}, last positions {[positions[mid] for mid in mob_ids]}")


def release_mob(bot, cells):
    """Take every cell a `hold_mob` placed back down (the mob is free to walk out)."""
    return place_cells(bot, sorted(set(cells)), "minecraft:air")


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-breeding-")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    # One chunk per tick region: the child and its parents are deliberately
    # placed either side of an x=0 cell boundary, so the goal's scan has to see
    # across a region edge even while `parent_entity_id` is cleared (inside the
    # three-block stop distance). With the default eight-chunk regions this
    # scene only straddles a boundary when it happens to sit near x=128k; this
    # pins the coupling every run. See `tick_mobs_regions`' follow-scan union.
    env["PLUSMC_TICK_REGIONS"] = "1"
    # The player's clock is this test's own packets, one tick each; the world
    # clock (the mobs, the birth, the orbs) stays the server's real 50 ms tick.
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
    # The natural spawner would add mobs to the population `/mobs` counts.
    env["PLUSMC_MOB_SPAWNING"] = "0"

    proc = None
    try:
        step(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--motd", "breeding test",
             "--offline-mode", "--gamemode", "1", "--data", data_root, "--world", world,
             "--no-flat-world"], wait_listening, env=env)
        step(f"server listening on {port}")

        bot = Bot("BreedBot", port)
        wire_client.record(bot, note="breeding: feeding two cows, a birth, and a baby growing up")
        bot.join()
        # The read side goes to the reader thread here, not at the follow
        # section: every window below is a quiet one (`drain`), the test spends
        # most of its minute in them, and the thread keeps the keep-alives
        # answered while the test only watches. `Reader.start` is idempotent,
        # so a second handover anywhere would be inert.
        bot.start_reader()
        packets = drain(bot, 1.2)
        for body in all_of(packets, 0x86):  # update_attributes carries the entity id
            bot.entity_id, _ = rd_varint(body, 0)
        assert bot.entity_id is not None, "the join must send update_attributes"
        step(f"  joined as entity {bot.entity_id}")

        for _ in range(3):
            bot.move_to(PLAYER_X, 64.0, PLAYER_Z)
            time.sleep(0.05)
        # 25 x-rows of nine platform cells, the two-high wall down both long
        # edges, and the wall across both ends (less the corners the edges did).
        expected_blocks = 25 * 9 + 25 * 2 * 2 + 2 * 7 * 2
        step(f"-> placing a stone floor, walled ({expected_blocks} real use_item_on packets)")
        placed = build_floor(bot)
        assert placed == expected_blocks, (placed, expected_blocks)
        for _ in range(2):
            bot.move_to(PLAYER_X, 64.0, PLAYER_Z)
            time.sleep(0.05)
        step(f"  OK floor: {placed} blocks placed (the wall included)")

        # Hotbar 0 is the wheat the animals are fed, hotbar 1 is the stone the
        # *wrong item* checks hold, and hotbar 2 is a second wheat stack for the
        # grown-up child (so the later step does not have to leave survival).
        # They go in while the player is still creative: `handleSetCreativeModeSlot`'s
        # whole body sits behind `hasInfiniteMaterials()`, so a survival client
        # cannot write a slot at all.
        step(f"-> hotbar 0 = {WHEAT_STACK} wheat, hotbar 1 = stone, hotbar 2 = wheat, "
             f"hotbar 3 = sword")
        set_creative_slot(bot, 36, WHEAT_STACK, ITEM_WHEAT)
        set_creative_slot(bot, 37, 64, ITEM_STONE)
        set_creative_slot(bot, 38, WHEAT_STACK, ITEM_WHEAT)
        # Hotbar 3 is the diamond sword the follow section kills a parent with:
        # seven damage a swing, so two hits, and `Player.attack`'s own tool.
        set_creative_slot(bot, 39, 1, ITEM_DIAMOND_SWORD)
        time.sleep(0.2)
        drain(bot, 0.4)
        positions = {}

        step("-> /gamemode survival")
        send_chat_command(bot, "gamemode survival")
        packets = drain(bot, 0.8)
        assert "survival" in feedback(packets).lower(), feedback(packets)

        # --- 1. /summon cow 2 -------------------------------------------------
        step("-> /summon cow 2")
        cow_a, cow_b = summon_cow_pair(bot)
        step(f"  OK cows: eid {cow_a['entity']} at x={cow_a['x']:.1f}, "
             f"eid {cow_b['entity']} at x={cow_b['x']:.1f} (health 10.0 each)")

        # --- 2. the range gate ------------------------------------------------
        # `handleInteract`'s `isWithinEntityInteractionRange(bounds, 3.0)`: from
        # 26 blocks above the floor the right click must do nothing at all — no
        # heart event, no slot echo, the stack untouched.
        step("-> /tp 0.5 90.5 0.5, then a right click from out of reach")
        send_chat_command(bot, "tp 0.5 90.5 0.5")
        drain(bot, 0.6)
        interact(bot, cow_a["entity"])
        packets = drain(bot, 0.8)
        events = [read_entity_event(b) for b in all_of(packets, CB_ENTITY_EVENT)]
        slots = [read_set_slot(b) for b in all_of(packets, CB_CONTAINER_SET_SLOT)]
        assert not any(e[1] == ENTITY_EVENT_LOVE for e in events), (
            f"an out-of-range interact must raise no heart event: {events}")
        assert not any(s["slot"] == 36 for s in slots), (
            f"an out-of-range interact must spend nothing: {slots}")
        step("  OK no heart burst, no item spent (the stack is a full 64)")

        step("-> /tp back to the floor")
        send_chat_command(bot, f"tp {PLAYER_X} 64.0 {PLAYER_Z}")
        drain(bot, 0.8)

        # --- 3. the food gate -------------------------------------------------
        # `Animal.mobInteract`'s `if (this.isFood(itemStack))`: a stone is not
        # food, and the packet is refused exactly as silently as the range one.
        step("-> hold stone, right click a cow: not food")
        bot.send(SB_SET_CARRIED_ITEM, struct.pack(">H", 1))
        drain(bot, 0.3)
        interact(bot, cow_a["entity"])
        packets = drain(bot, 0.8)
        events = [read_entity_event(b) for b in all_of(packets, CB_ENTITY_EVENT)]
        slots = [read_set_slot(b) for b in all_of(packets, CB_CONTAINER_SET_SLOT)]
        assert not any(e[1] == ENTITY_EVENT_LOVE for e in events), events
        assert not any(s["slot"] == 37 for s in slots), slots
        step("  OK no heart burst, no stone spent")

        # --- 3b. the scene the birth rests on --------------------------------
        # A wire test cannot move a mob, and the two gate steps above take about
        # five seconds in which the pair roams the corridor on its own
        # (`RandomStrollGoal` is free to run — one of the gates even puts the
        # player 26 blocks up, so nothing is watching them). `BreedGoal.canUse`
        # needs the partner inside eight blocks, and an in-love cow that finds
        # none has no movement goal at all: the breed goal outranks the stroll,
        # so `breed_step`'s early return just stands it still while the love
        # counts down. A pair that drifted apart *here* can therefore never
        # breed, and nothing later in the scene can repair it. The gap is
        # measured before the first feeding, and a pair outside the range is
        # replaced with a fresh `/summon cow 2` — `/mobs kill` is the server's
        # own reset — because the gates above are the thing under test, not
        # these two bodies.
        for _ in range(3):
            send_chat_command(bot, "mobs list")
            scene_gap = mob_gap(mob_listing(feedback(drain(bot, 0.6))),
                                cow_a["entity"], cow_b["entity"])
            if scene_gap is not None and scene_gap <= SCENE_BREED_GAP:
                break
            step(f"  the pair is {scene_gap:.2f} blocks apart" if scene_gap is not None
                 else "  /mobs list did not carry both cows")
            step("  -> /mobs kill, then a fresh /summon cow 2")
            send_chat_command(bot, "mobs kill")
            drain(bot, 0.5)
            cow_a, cow_b = summon_cow_pair(bot)
            step(f"  OK fresh cows: eid {cow_a['entity']} and eid {cow_b['entity']}")
        assert scene_gap is not None and scene_gap <= SCENE_BREED_GAP, (
            f"the birth scene never held the pair inside the partner range: {scene_gap}")
        step(f"  OK pair {scene_gap:.2f} blocks apart (the breed range is 8.0)")

        # --- 4. feeding starts a love ----------------------------------------
        step("-> hold wheat, right click the first cow: setInLove")
        bot.send(SB_SET_CARRIED_ITEM, struct.pack(">H", 0))
        drain(bot, 0.3)
        interact(bot, cow_a["entity"])
        packets = drain(bot, 1.0)
        events = [read_entity_event(b) for b in all_of(packets, CB_ENTITY_EVENT)]
        hearts = [e for e in events if e[1] == ENTITY_EVENT_LOVE]
        assert hearts == [(cow_a["entity"], ENTITY_EVENT_LOVE)], (
            f"one heart burst for the fed cow: {events}")
        slots = [read_set_slot(b) for b in all_of(packets, CB_CONTAINER_SET_SLOT)]
        held = [s for s in slots if s["slot"] == 36]
        assert held and held[-1]["count"] == WHEAT_STACK - 1 and held[-1]["item"] == ITEM_WHEAT, (
            f"the feeding must spend one wheat: {held}")
        step(f"  OK heart event 18 for eid {cow_a['entity']}, wheat {WHEAT_STACK} -> "
             f"{held[-1]['count']}")

        # `canFallInLove` is `inLove <= 0`: the second feeding is refused.
        step("-> right click it again while it is still in love")
        interact(bot, cow_a["entity"])
        packets = drain(bot, 0.8)
        events = [read_entity_event(b) for b in all_of(packets, CB_ENTITY_EVENT)]
        slots = [read_set_slot(b) for b in all_of(packets, CB_CONTAINER_SET_SLOT)]
        assert not any(e[1] == ENTITY_EVENT_LOVE for e in events), events
        assert not any(s["slot"] == 36 for s in slots), slots
        step("  OK refused: no second heart, nothing spent")

        # `BreedGoal.canUse` looks for a partner inside eight blocks, and an
        # in-love animal whose search comes up empty has **no** movement goal:
        # the breed goal outranks the stroll, so `breed_step`'s early return
        # stands the animal still and its love just counts down. The pair
        # strolls freely through the gate steps above (the player is 26 blocks
        # up for one of them), so the gap at the second feeding is the scene
        # precondition the birth rests on. It is measured and printed every
        # run rather than asserted — a birth timeout then reads as "they were
        # 9.3 blocks apart when the second feeding landed" and not as a
        # mystery in the tick.
        send_chat_command(bot, "mobs list")
        listing = mob_listing(feedback(drain(bot, 0.6)))
        if cow_a["entity"] in listing and cow_b["entity"] in listing:
            (ax, ay, az) = listing[cow_a["entity"]]
            (bx, by, bz) = listing[cow_b["entity"]]
            feed_gap = math.sqrt((ax - bx) ** 2 + (ay - by) ** 2 + (az - bz) ** 2)
            step(f"  pair gap before the second feeding: {feed_gap:.2f} blocks "
                 f"(the breed range is 8.0)")
        else:
            step(f"  pair gap: /mobs list carried {len(listing)} mob(s), not both cows")

        step("-> the same feeding on the second cow")
        interact(bot, cow_b["entity"])
        packets = drain(bot, 1.0)
        events = [read_entity_event(b) for b in all_of(packets, CB_ENTITY_EVENT)]
        assert (cow_b["entity"], ENTITY_EVENT_LOVE) in events, (
            f"the second cow must answer with its own heart burst: {events}")
        slots = [read_set_slot(b) for b in all_of(packets, CB_CONTAINER_SET_SLOT)]
        held = [s for s in slots if s["slot"] == 36]
        assert held and held[-1]["count"] == WHEAT_STACK - 2, f"two feedings spent: {held}"
        step(f"  OK both cows in love, wheat {WHEAT_STACK} -> {held[-1]['count']}")

        # --- 5. the birth -----------------------------------------------------
        # The pair walks together (`BreedGoal`'s own navigation) for thirty goal
        # ticks, breeds inside three blocks, and `finalizeSpawnChildFromBreeding`
        # spawns the child at the initiator, takes the parents' 6000-tick age,
        # resets both loves, raises the heart event and drops 1..7 experience.
        step("-> waiting for the pair to breed (world clock)")
        packets, child, data, hearts, orbs, advances = wait_for_cow_birth(
            bot, {cow_a["entity"], cow_b["entity"]}, positions)
        if child is None:
            # A timed-out wait is *diagnosed*, not only reported: `/mobs list`
            # carries each animal's age, love countdown, mate id and position,
            # which separates the three ways this wait can lose (the pair never
            # found each other, the love countdown expired before it got inside
            # the breed distance, or one of them left the population).
            send_chat_command(bot, "mobs list")
            state = feedback(drain(bot, 1.0))
            raise AssertionError(
                f"the two in-love cows never produced a child in "
                f"{WORLD_EVENT_TIMEOUT:.0f}s; animals: {state}")
        assert abs(child["x"] - PLAYER_X) < 3.0 and abs(child["z"] - PLAYER_Z) < 3.0, child
        assert abs(child["y"] - 64.0) < 0.5, f"`snapTo(this)` at the parent's feet: {child}"
        assert len(orbs) >= 1, "the birth must drop 1..7 experience orbs"
        assert any(h[1] == ENTITY_EVENT_LOVE and
                   h[0] in (cow_a["entity"], cow_b["entity"]) for h in hearts), (
            f"the parents' heart burst: {hearts}")
        step(f"  OK child eid {child['entity']} at ({child['x']:.2f}, {child['y']:.2f}, "
             f"{child['z']:.2f}), {len(orbs)} orb(s), hearts {hearts}")

        # `finalizeSpawnChildFromBreeding`'s `CriteriaTriggers.BRED_ANIMALS`: the
        # player who fed the pair earns `minecraft:husbandry/breed_an_animal`
        # (its one condition-less `bred` criterion), which reaches the client as
        # an update_advancements flush on the birth tick.
        #
        # The body's ids are length-prefixed UTF-8, so the id appears in it
        # literally; walking the whole tree here would duplicate
        # advancement_wire_test.py's decoder, which is the thing that would rot
        # if there were two of it.
        assert advances, "the birth must flush update_advancements"
        assert any(b"minecraft:husbandry/breed_an_animal" in body for body in advances), (
            f"the breeding player must earn breed_an_animal: "
            f"{[len(b) for b in advances]}")
        step("  OK breed_an_animal advancement awarded to the feeding player")

        # ...and the same call's other cause-bound payout,
        # `cause.awardStat(Stats.ANIMALS_BRED)`. `awardStat` marks the counter
        # dirty; the statistics screen's own request (`client_command` 0x0c
        # action 1) is what pulls it out of the session as `award_stats`. This is
        # the only place the stat is checked end to end — the counter the
        # milestone's registry had a name for and nothing ever awarded.
        step("-> REQUEST_STATS: the birth's animals_bred counter")
        bot.send(SB_CLIENT_COMMAND, varint(1))
        packets = drain(bot, 1.5)
        stats = []
        for body in all_of(packets, CB_AWARD_STATS):
            stats.extend(read_award_stats(body))
        bred = [value for (stype, sid, value) in stats
                if stype == STAT_CUSTOM and sid == STAT_ANIMALS_BRED]
        assert bred and max(bred) >= 1, (
            f"the birth must award the animals_bred custom stat (8, 34): {stats}")
        step(f"  OK animals_bred = {max(bred)} on the award_stats reply")

        # `DATA_BABY_ID` is what makes the client draw the small box: Add
        # Entity's own `data` int is not synched data, so the bit arrives as its
        # own set_entity_data right behind the spawn.
        baby_words = [(eid, values) for eid, values in data
                      if eid == child["entity"] and BABY_INDEX in values]
        assert baby_words and baby_words[0][1][BABY_INDEX] is True, (
            f"the newborn's DATA_BABY_ID must be set: {data}")
        health_words = [(eid, values) for eid, values in data
                        if eid == child["entity"] and HEALTH_INDEX in values]
        assert health_words and health_words[0][1][HEALTH_INDEX] == 10.0, (
            f"the child's health word: {data}")
        step("  OK DATA_BABY_ID (index 16, BOOLEAN) set with the spawn, health 10.0")

        # --- 6. the parents are on cooldown ----------------------------------
        step("-> right click a parent: the 6000-tick age refuses the feeding")
        interact(bot, cow_a["entity"])
        packets = drain(bot, 0.8)
        events = [read_entity_event(b) for b in all_of(packets, CB_ENTITY_EVENT)]
        slots = [read_set_slot(b) for b in all_of(packets, CB_CONTAINER_SET_SLOT)]
        assert not any(e[1] == ENTITY_EVENT_LOVE for e in events), events
        assert not any(s["slot"] == 36 for s in slots), slots
        step("  OK refused: `canFallInLove` needs an age of 0, the parent's is 6000")

        # --- 7. the newborn keeps to its *nearest* parent ---------------------
        # `FollowParentGoal`: a baby takes the nearest *adult* of its own class
        # whose box touches its own inflated by (8, 4, 8), walks to it and stops
        # inside three blocks (`DONT_FOLLOW_IF_CLOSER_THAN`) — and the stroll is
        # what it does only while no such adult is in range.
        #
        # The pair starts this section on top of each other (they bred inside
        # three blocks of each other), and a question about the *nearest* adult
        # needs the two apart, so the section builds the distance it needs. A
        # wire test cannot move a mob — there is no `/tp @e`, and the player's
        # own `/tp` moves nobody but the player — so the scene is built out of
        # what it *can* do. One `setblock` in a mob's own *head* cell holds it
        # still: every move of its 1.4-high box overlaps that cell, while the
        # baby's own box (0.7 high, the baby dimension column) stays inside the
        # floor cell below and so stays free. The child's candidates are the
        # adults of its class on the platform, so the second parent is *killed*
        # (the wire's own attack, watched to its death event and its removal),
        # the survivor is held where it stands, and a fresh adult is *summoned*
        # where the test wants it: `/summon`'s ring puts a single mob two blocks
        # east of the player, which is what makes the player's `/tp` a placement
        # tool. What is left is exactly two candidates at distances the test
        # chose — the surviving parent, held where it stood, and the summoned
        # adult seven and a half blocks past it. Both holds are scaffolding for
        # that placement: they come down before the follow window, which watches
        # a *moving* pair with no stone of the test's in the arena at all. A wall
        # can park the child, and a parked child is what the 18 Oct runs
        # measured.
        #
        # The kill runs *before* either parent is buried, and that order is not
        # cosmetic: a stone in one parent's head cell can sit on the line
        # `handle_attack` walks from the player's eyes to a second parent
        # standing beside it — the pair bred within three blocks of each other
        # and can share a head cell outright — and the swing is then a miss for
        # every hit of the loop. (The player's own `/tp` to the swing spot can
        # land it inside that stone too.) Only the survivor needs holding, and
        # once it is the last adult left, the child has nowhere else to go.
        # Seed every actor's position from its own spawn packet. The sort below
        # needs all three, and a parent whose first `position_sync` has not
        # landed yet — a loaded box can delay it past every drain above — would
        # otherwise raise `KeyError` instead of being ordered by where it
        # spawned. A later sync overwrites the seed the usual way.
        for actor in (cow_a, cow_b, child):
            positions.setdefault(actor["entity"], (actor["x"], actor["y"], actor["z"]))
        child_pos = positions[child["entity"]]
        held_id, doomed_id = sorted(
            (cow_a["entity"], cow_b["entity"]),
            key=lambda pid: gap(child_pos, positions[pid]))
        step(f"-> the child is with eid {held_id} "
             f"({gap(child_pos, positions[held_id]):.2f} blocks); killing the "
             f"other parent (eid {doomed_id}): one adult left")
        bot.send(SB_SET_CARRIED_ITEM, struct.pack(">H", 3))  # the sword
        drain(bot, 0.3)
        # The strength of a swing is the tick count since the last one against
        # the *held* item's delay (12.5 ticks for a sword), so the first blow
        # wants a moment to charge — it is `Player.attack`'s own 0.2..1.0 scale.
        time.sleep(0.8)
        died = removed = False
        healths = []
        for hits in range(1, 13):
            quarry = positions[doomed_id]
            # Stand where the parent's own last position_sync put it and swing in
            # the same breath: `handle_attack` walks a line of sight from the
            # player's eyes to the mob's, so a parent that strolls out from under
            # the swing is a miss — and its reach is measured from there too.
            send_chat_command(bot, f"tp {quarry[0]:.2f} {float(FLOOR_Y + 1):.3f} "
                                   f"{quarry[2]:.2f}")
            bot.send(SB_ATTACK, varint(doomed_id))
            packets = drain(bot, 0.25)
            # The charge: a sword wants 12.5 ticks of the player's own clock.
            tick(bot, 14)
            # The mob's hurt window is twenty ticks of the *world* clock, which
            # is real time here.
            time.sleep(0.7)
            packets += drain(bot, 0.35)
            # Keep the tracked positions fresh: the next swing's tp goes to the
            # parent's *last* sync, and a whole second of misses is easy to
            # accumulate against a parent that strolls.
            note_positions(packets, positions)
            for body in all_of(packets, CB_SET_ENTITY_DATA):
                word = read_entity_data(body, doomed_id)
                if word is not None and HEALTH_INDEX in word[1]:
                    healths.append(word[1][HEALTH_INDEX])
            hits_events = [read_entity_event(b) for b in all_of(packets, CB_ENTITY_EVENT)]
            ids = []
            for body in all_of(packets, CB_REMOVE_ENTITIES):
                ids.extend(read_remove_entities(body))
            died = died or (doomed_id, ENTITY_EVENT_DEATH) in hits_events
            removed = removed or doomed_id in ids
            if died and removed:
                break
        assert healths and healths[-1] == 0.0, (
            f"the death must report zero health on the wire: {healths}")
        assert died and removed, (
            f"eid {doomed_id} did not die in {hits} hit(s): death event {died}, "
            f"remove_entities {removed}")
        positions.pop(doomed_id, None)
        # The wheat is wanted again: the next section feeds the child its stack.
        bot.send(SB_SET_CARRIED_ITEM, struct.pack(">H", 0))
        step(f"  OK eid {doomed_id} died in {hits} hit(s): health "
             f"{', '.join(f'{h:.2f}' for h in healths)}, entity_event "
             f"{ENTITY_EVENT_DEATH} and its remove_entities")

        step(f"-> holding the surviving parent (eid {held_id}) where it stands")
        # `clear_of` is the child: it follows the parent into the next cell, and
        # a closed ring placed while it is there walls the child into a stone
        # cell. The corners are kept filled: `open_corners` is only safe for a
        # mob on its cell's exact centre, and a hold walls a mob where it stands.
        held, held_cells = hold_mob(bot, [held_id], positions, clear_of=(child["entity"],))
        step(f"  OK eid {held_id} held still at {held[held_id]}, the child "
             f"{gap(positions[child['entity']], positions[held_id]):.2f} blocks away")

        step(f"-> summoning a second adult {FAR_ADULT_DISTANCE:.1f} blocks past the held one")
        anchor = positions[held_id]
        # The scan this section relies on is the child's own box inflated by
        # (8, 4, 8), so where the second adult lands decides whether the child
        # is inside it -- and the child's own stroll carries it a couple of
        # blocks every leg, which is what a wait on that box would be waiting
        # for. The child is therefore *pinned* first (`hold_mob` with one level:
        # a baby's 0.7-high box lives in its feet cell, where an adult's needs
        # the cell above too), and the spot is computed off that fixed point:
        # on the line from the held parent through the child, at
        # `FAR_ADULT_DISTANCE`, so |child - spot| is the pair distance minus the
        # child's own offset -- inside the scan by arithmetic, before the mob
        # exists.
        #
        # Which line matters, and it is not a free choice. Put the second adult
        # 7.5 blocks *behind* the held parent, away from the child, and a child
        # that strolls two blocks past that parent is 9.5 blocks from it: out of
        # the scan, with nothing left that pulls it back, and a wait on the
        # child's random walk then times out against a candidate that can no
        # longer be found. (The engine's own replay of this scene, three mobs
        # under `tick_mobs`, shows the same cliff: the child between the two
        # holds both in scan for every sample, the child behind the held parent
        # parks there with the second adult out of reach.) Through the child,
        # the child starts between the two instead, every point inside its own
        # offset of the pair distance, and the two columns below read what the
        # goal does next rather than how the scene happened to roll.
        step("-> pinning the child, so the spot is placed off a fixed point")
        # The child's own ring is closed too, so it must not reach the parent's
        # cell: the wait keeps it two cells out.
        held_child, child_cells = hold_mob(bot, [child["entity"]], positions, levels=1,
                                           clear_of=(held_id,))
        child_now = positions[child["entity"]]
        step(f"  OK the child is held at {held_child[child['entity']]}, "
             f"{gap(child_now, anchor):.2f} blocks from the held parent")
        # The direction: the ring of landing cells `FAR_ADULT_DISTANCE` from the
        # held parent, searched for the one closest to "straight through the
        # child" that passes the platform and scan tests. The landing is a *cell
        # centre*, not the raw ring point, because the summoned adult's pen (see
        # below) has to be built before it arrives and the cow has to stand in
        # the middle of the cell the pen leaves air.
        #
        # A cell centre is also the one landing a 0.9-wide box cannot straddle:
        # the box reaches 0.45 from the centre and every cell face is 0.5 away.
        # The old fractional spot could sit 0.446 from the two-high rim on the
        # z edge, so the box reached into the rim cell and `mob_place`'s unstuck
        # loop lifted the cow two blocks to y=66 -- read below as a 2.00-block
        # drift. On a cell centre that cannot happen, and the y assertion after
        # the summon says so.
        #
        # Both searches are load-bearing. `build_floor` lays 25 cells of x
        # (-12..12) by nine of z (-4..4) and walls every edge two blocks high,
        # so the interior is the only ground a summon can stand on. And the
        # scan test is what the child's own box reads: a spot perpendicular to
        # the child is up to 8.1 blocks out -- inside the real box (its half
        # widths add to it) but outside the conservative one this test uses, so
        # it is not chosen.
        def landing_cell(spot):
            """The cell centre a summon at `spot` would stand in."""
            return (math.floor(spot[0]) + 0.5, float(FLOOR_Y + 1),
                    math.floor(spot[2]) + 0.5)

        def on_platform(p):
            """Whether a *cell centre* is an interior platform cell.

            `build_floor` lays cells x = -12..12 by z = -4..4 and walls the
            edge cells two blocks high, so the cells a cow can stand in are
            x = -11..11, z = -3..3. Centre bounds: -10.5..11.5 and -2.5..3.5
            (a cell index is its floor, so cell -12's centre is -11.5, not
            -12.5). The old guard stopped at -11.5 / -3.5 in *both*
            directions, which let the negative edge cell through -- the rim
            itself. That is how the summon landed at z=-3.5, a rim cell, and
            `mob_place` lifted it to y=66.
            """
            return -10.5 <= p[0] <= 11.5 and -2.5 <= p[2] <= 3.5

        reach = math.hypot(child_now[0] - anchor[0], child_now[2] - anchor[2])
        through = ((child_now[0] - anchor[0]) / reach,
                   (child_now[2] - anchor[2]) / reach) if reach > 1.0e-6 else (1.0, 0.0)
        best = None
        for i in range(72):
            angle = i * (2.0 * math.pi / 72.0)
            dir_x, dir_z = math.cos(angle), math.sin(angle)
            target = landing_cell((anchor[0] + dir_x * FAR_ADULT_DISTANCE, 0.0,
                                   anchor[2] + dir_z * FAR_ADULT_DISTANCE))
            pair = gap(anchor, target)
            # The landing is placed by the child's own band (4.2 at worst), so
            # the pair must not come in under the distance the scene was built
            # for: at `FAR_ADULT_DISTANCE` the pinned child is clear of both
            # balls and the summoned adult is inside its scan. A half-block of
            # lattice rounding is accepted; a nearer cell would put the second
            # adult inside the child's own three-block ball.
            if not (FAR_ADULT_DISTANCE <= pair <= FAR_ADULT_DISTANCE + 0.5):
                continue
            if not on_platform(target) or not in_follow_scan(child_now, target):
                continue
            alignment = dir_x * through[0] + dir_z * through[1]
            if best is None or alignment > best[0]:
                best = (alignment, target, pair)
        assert best is not None, (
            f"no platform landing cell {FAR_ADULT_DISTANCE:.1f} blocks from {anchor} "
            f"is inside the pinned child's scan at {child_now}")
        target = best[1]
        # The pen goes up *before* the summon, around the cell the cow will land
        # in, with that cell left air. It replaces the old stone in the head
        # cell, which no longer catches anything: `Entity.move` clips an axis
        # only against the shape slices beyond the box's leading face, so a
        # block the box already overlaps is not a collision and the cow walks
        # out from under it (the rule `test_overlapping_box_walks_out` pins).
        # With the ring in place first, every neighbouring cell is unwalkable,
        # so the newborn's stroll falls through to Idle, and because its box
        # never overlaps a pen wall `mob_place` has nothing to lift it out of.
        # The corners are left open: the summon puts the cow on the cell's exact
        # centre (its box 0.05 clear of every wall), so the cardinals pin it by
        # collision while the diagonals stay standable. This ring is up only
        # long enough to catch the landing and read the drift; it comes down
        # with the other two pens before the follow window starts.
        pen = pen_cells((math.floor(target[0]), int(target[1]), math.floor(target[2])), 2,
                        open_corners=True)
        place_cells(bot, pen)
        # One mob on the summon ring lands two blocks east of the player, so the
        # player's own tp is what places it.
        send_chat_command(bot, f"tp {target[0] - 2.0:.3f} {target[1]:.3f} {target[2]:.3f}")
        drain(bot, 0.4)
        send_chat_command(bot, "summon cow 1")
        packets = drain(bot, 1.0)
        assert "Summoned 1 minecraft:cow" in feedback(packets), feedback(packets)
        known = (cow_a["entity"], cow_b["entity"], child["entity"])
        strangers = []
        for body in all_of(packets, CB_SPAWN_ENTITY):
            spawned = read_spawn(body)
            if spawned["type"] == ET_COW and spawned["entity"] not in known:
                strangers.append(spawned)
        assert len(strangers) == 1, f"one summoned adult: {strangers}"
        stranger = strangers[0]
        assert (abs(stranger["x"] - target[0]) < 0.6 and
                abs(stranger["z"] - target[2]) < 0.6), (
            f"the ring puts it two blocks east of the player: {stranger} for {target}")
        assert abs(stranger["y"] - target[1]) < 1.0e-6, (
            f"the pen must leave the landing free: the summon started at "
            f"y={stranger['y']:.2f}, not {target[1]:.2f}, so its box overlapped a "
            f"block and `mob_place`'s unstuck pass lifted it (the y=66 case)")
        positions[stranger["entity"]] = (stranger["x"], stranger["y"], stranger["z"])
        far_id = stranger["entity"]
        note_positions(drain(bot, 1.5), positions)
        drift = gap(positions[far_id], target)
        assert drift < 0.35, (
            f"the pen must catch it at the landing cell "
            f"{target}: it is {drift:.2f} blocks away at {positions[far_id]}")
        step(f"  OK summoned eid {far_id} at ({stranger['x']:.2f}, {stranger['z']:.2f}) "
             f"on y={stranger['y']:.2f}, in its pen after {drift:.2f} blocks of drift")

        step("-> taking every pen down: the follow window is pen-free")
        summoned_id = far_id  # named for what it is, once the columns stop caring
        # The scan, read back off the wire: the child is pinned (its pen is
        # still in place), so this is the same arithmetic as the placement
        # assert, not a race against the child's stride. No wait, no timeout.
        child_pos = positions[child["entity"]]
        assert in_follow_scan(child_pos, positions[summoned_id]), (
            f"the pinned child must be in the summoned adult's scan: "
            f"{gap(child_pos, positions[summoned_id]):.2f} blocks")
        step(f"  OK both adults in scan with no wait: "
             f"{gap(child_pos, positions[summoned_id]):.2f} to the summoned one, "
             f"{gap(child_pos, positions[held_id]):.2f} to the parent")
        # The window watches what `FollowParentGoal` does with a *moving* adult,
        # so nothing stays walled for it. The three pens up to here had one job
        # -- hold each mob still while the next placement was made and checked
        # -- and their presence during the window made the measurement a
        # property of the stone: in the 18 Oct full-suite run a child sat at
        # 4.85 blocks from *both* penned adults for eight seconds (31 identical
        # samples). That is a wall's answer, not the goal's. Every cell this
        # section placed comes down here, and both adults stroll free.
        #
        # The candidate arithmetic is not lost with the pens: the gap columns
        # read the *nearest* adult at every sample, and the tick-level claims
        # (nearest wins and no skip to a farther adult, the three-block band,
        # the sixteen-block far end) are pinned by `test_follow_parent` and
        # `test_follow_parent_band` in tests/mob_test.cpp.
        packets = release_mob(bot, list(child_cells) + list(held_cells) + list(pen))
        note_positions(packets, positions)

        step("-> watching the newborn alone with the two of them (world clock, no stone)")
        samples = sample_follow(bot, child["entity"], [held_id, summoned_id], positions)
        assert len(samples) >= 20, f"too few position samples for the child: {len(samples)}"
        # The child's *nearest* adult at each sample, not one label frozen at
        # the first one: `FollowParentGoal` re-picks the nearest candidate every
        # time it restarts, and with both animals free the two can pass each
        # other over a 40-second window. The nearest is what the goal promises
        # something about, and it is the only column a moving pair supports.
        gaps = [min(gap(child_pos, parents[held_id]), gap(child_pos, parents[summoned_id]))
                for _, child_pos, parents in samples]
        walked = sum(gap(a[1], b[1]) for a, b in zip(samples, samples[1:]))
        # How far each adult actually walked. A pen holds one at 0.00 for the
        # whole window -- exactly the state this window replaced -- so this is
        # the assertion that the scene is free rather than an assumption.
        parent_walked = {
            pid: sum(gap(a[2][pid], b[2][pid]) for a, b in zip(samples, samples[1:]))
            for pid in (held_id, summoned_id)}
        mean = sum(gaps) / len(gaps)
        tail = sum(gaps[len(gaps) // 2:]) / len(gaps[len(gaps) // 2:])
        step(f"  seen {len(samples)} samples: gap mean {mean:.2f}, tail mean {tail:.2f}, "
             f"max {max(gaps):.2f}, walked {walked:.2f}; adults walked "
             f"{parent_walked[held_id]:.2f} and {parent_walked[summoned_id]:.2f} blocks")
        assert walked > 1.0, f"the child never moved: {walked:.2f} blocks"
        # A free cow strolls a two-block leg every few seconds, so either adult
        # reading zero means it is still walled and the window is not the one
        # this section claims to run.
        assert min(parent_walked.values()) > 0.5, (
            f"both adults must be free to stroll: they walked "
            f"{parent_walked[held_id]:.2f} and {parent_walked[summoned_id]:.2f} blocks")
        # The goal's own band, read off `FollowParentGoal`'s constants rather
        # than a calibrated average. `DONT_FOLLOW_IF_CLOSER_THAN` is 3.0
        # (`world/mob.h`), so a following child lives inside a three-block ball
        # around its nearest parent plus the step its own momentum carries over
        # a sample boundary -- while a child that only strolled would walk *away*
        # on a random leg, out of any three-block ball on a platform this size.
        #
        # An earlier cut asserted `mean(gaps) <= 2.0`, which is not vanilla's
        # number but an idle-box measurement: a loaded box (four cores busy) put
        # the same following child at 2.29 with every sample still inside the
        # ball and the window still flat, so that threshold reddened correct
        # behaviour. What the goal actually promises is the ball and the absence
        # of outward drift, and those are what this asserts.
        #
        # With a free adult both ends of the band are live: the child's own
        # stroll can carry it out of the eight-block candidate scan while the
        # follow goal is silent (inside three), and the walk back in is the
        # goal's job rather than a bound the window can pretend. Sixteen is the
        # band's own outer edge -- `canContinueToUse` lets a held parent go past
        # it -- and a child genuinely past it has stopped following. The
        # sustained claims are the recurrence and the tail mean below.
        assert max(gaps) <= 16.0, (
            f"the goal's own band is sixteen blocks: {max(gaps):.2f}")
        # A follower *comes back*: every fully sampled eight-second stretch of
        # the window has to contain a sample inside the nearest parent's ball.
        # The goal restarts the moment the nearest adult is past three and plans
        # its first path on that tick, so a whole eight seconds outside 3.5 is
        # a state the goal cannot produce; a child that only strolls, or one
        # whose follow path is broken, has stretches with no such sample. That
        # is the claim this window can make, and it is the one a per-sample
        # fraction cannot: the goal deliberately parks the child at 2.9 and its
        # own outward stroll legs then sample it at 3.5-3.9, so a
        # 90%-inside-3.5 cut read 80% on a window whose tail mean was 2.41 and
        # whose worst sample was 3.88 -- correct behaviour. The 3.0 line itself
        # is pinned on the tick by `test_follow_parent_band`, not inferred from
        # this stroll.
        first = samples[0][0]
        windows = {}
        for (t, _, _), g in zip(samples, gaps):
            windows.setdefault(int((t - first) // 8.0), []).append(g)
        for index in sorted(windows):
            # The truncated last bucket is not an eight-second window: a single
            # outward stroll sample in it says nothing about a whole stretch.
            if len(windows[index]) < 16:
                continue
            lo, hi = min(windows[index]), max(windows[index])
            assert lo <= 3.5, (
                f"the child spent the whole 8s from {index * 8:.0f}s outside its "
                f"nearest parent's ball: {lo:.2f} to {hi:.2f} blocks over "
                f"{len(windows[index])} samples (mean {mean:.2f})")
        # The window has to *end* in the ball, not merely have visited it: the
        # last eight seconds are checked as a stretch of their own, whatever the
        # bucket boundaries did.
        final = [g for (t, _, _), g in zip(samples, gaps) if t >= samples[-1][0] - 8.0]
        assert min(final) <= 3.5, (
            f"the last 8s must contain a sample inside the ball: "
            f"{min(final):.2f} to {max(final):.2f} over {len(final)} samples")
        # The tail must not be a different, farther band from the head: a
        # wander's mean grows without bound -- each random leg is as likely away
        # as toward -- while a follower's stays in the goal's band in both
        # halves. 3.5 is the ball plus the fraction of a block an 8-second
        # mean can sit above it on stroll legs that are still being turned
        # around.
        head = sum(gaps[:len(gaps) // 2]) / len(gaps[:len(gaps) // 2])
        assert tail <= 3.5, (
            f"the child must not drift away from its parent over the window: "
            f"tail mean {tail:.2f} against a three-block ball (first-half {head:.2f})")
        step("  OK the child came back to the follow's band through the whole window")

        # --- 8. the baby is fed up, then grows up ----------------------------
        # Each feeding is `getSpeedUpSecondsWhenFeeding(-age)` seconds, and the
        # item is spent before `ageUp` runs — including when the feeding is worth
        # nothing. That stall is the point: feeding alone can never reach
        # adulthood (the worth is `floor(remaining / 200)`), so the natural
        # `++age` is what finishes it, and the server clears the bit there.
        remaining = WHEAT_STACK - 2  # two feedings are already in the cows
        child_pos = positions.get(child["entity"], (child["x"], child["y"], child["z"]))
        step(f"-> /tp to the child (it has been following), then feed it the whole "
             f"stack ({remaining} wheat)")
        send_chat_command(bot, f"tp {child_pos[0]:.3f} {child_pos[1]:.3f} {child_pos[2]:.3f}")
        drain(bot, 0.6)
        for _ in range(remaining):
            interact(bot, child["entity"])
        packets = drain(bot, 2.0)
        note_positions(packets, positions)
        slots = [read_set_slot(b) for b in all_of(packets, CB_CONTAINER_SET_SLOT)]
        empty = [s for s in slots if s["slot"] == 36 and s["count"] == 0]
        assert empty, f"the last feeding must empty the stack: {slots[-4:]}"
        step("  OK the stack is empty and the age has stalled below 200 ticks")

        step("-> waiting for the natural ++age to cross 0 (world clock)")
        seen, grown = wait_for_baby_bit(bot, child["entity"], False, positions)
        assert grown, (
            "the baby must grow up: its DATA_BABY_ID has to clear when the age "
            f"crosses 0 (seen {len(seen)} packets)")
        step("  OK DATA_BABY_ID cleared (index 16, false) — the client swaps the box")

        # --- 9. the grown child can now fall in love -------------------------
        # It is an adult at age 0 that has never been fed an adult's food, so the
        # same wheat takes the *other* branch: `setInLove` instead of `ageUp`.
        # Hotbar 2 still holds a full stack, so the step stays in survival.
        child_x, child_y, child_z = positions.get(child["entity"],
                                                  (child["x"], child["y"], child["z"]))
        step(f"-> /tp next to the grown child, hold hotbar 2's wheat, right click it")
        send_chat_command(bot, f"tp {child_x:.3f} {child_y:.3f} {child_z:.3f}")
        drain(bot, 0.6)
        bot.send(SB_SET_CARRIED_ITEM, struct.pack(">H", 2))
        drain(bot, 0.3)
        interact(bot, child["entity"])
        packets = drain(bot, 1.0)
        events = [read_entity_event(b) for b in all_of(packets, CB_ENTITY_EVENT)]
        assert (child["entity"], ENTITY_EVENT_LOVE) in events, (
            f"an adult born in this run must be able to fall in love: {events}")
        slots = [read_set_slot(b) for b in all_of(packets, CB_CONTAINER_SET_SLOT)]
        held = [s for s in slots if s["slot"] == 38]
        assert held and held[-1]["count"] == WHEAT_STACK - 1, f"one wheat spent: {held}"
        step("  OK heart event 18 for the grown child, one wheat spent")

        # --- 10. the population and the cooldown -----------------------------
        step("-> /mobs")
        send_chat_command(bot, "mobs")
        packets = drain(bot, 0.8)
        text = feedback(packets)
        assert "minecraft:cow x3" in text, f"three cows (no fourth birth): {text}"
        step(f"  OK {text.strip()}")

        bot.close()
    finally:
        if proc is not None:
            proc.terminate()
            proc.wait(timeout=5)
    print("\nPASS: breeding test")
    return 0


if __name__ == "__main__":
    sys.exit(main())
