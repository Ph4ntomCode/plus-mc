#!/usr/bin/env python3
"""Integration test: TNT over the wire.

TNT exists so that three things that had nowhere to happen become observable:

  * `TntBlock.useItemOn` — flint and steel on a TNT block primes it, takes the
    block away and wears the item by one;
  * `PrimedTnt` — the entity the block becomes: the spawn, the 80-tick fuse the
    client counts down, the gravity, and the discard at zero;
  * `ServerExplosion` — the blast: the blocks it destroys (a `block_update` per
    cell), the entities it hurts (a `damage_event` plus the knockback, as both
    the packet that means exactly that — `set_entity_motion` — and the
    `playerKnockback` the explode packet carries for the player it hit), and the
    *chained* TNT (`TntBlock.wasExploded`), which primed with a short fuse
    instead of 80.
  * `ClientboundExplodePacket` — the blast's own packet: the centre, the radius,
    the block count, the particle and sound registry ids (`EXPLOSION_EMITTER` and
    `GENERIC_EXPLODE` for a power-4 blast) and the weighted block-particle list
    (`DEFAULT_EXPLOSION_BLOCK_PARTICLES`: POOF at 0.5x, SMOKE at 1.0x). Its
    audience is vanilla's fixed 64-block sphere, which the far viewer walks out
    of in step 5 and must then receive nothing from.

The world is the committed flat fixture and the test runs with
`PLUSMC_TICK_SCALE=2`, so the 80-tick fuse is about two real seconds: long
enough to watch it count down packet by packet, short enough to wait out.

What each step can only be explained by:

  1. the join carries no TNT at all (a fresh world has none);
  2. a fire lit beside two TNT blocks primes one of them — the `add_entity` for
     type 133 arrives with a *fuse of 80*, which is `TntBlock.prime`'s default
     and therefore the `FireBlock.checkBurnOut` path (a chained blast would have
     sent 10..29). `minecraft:tnt` sits on the flammability table at ignite 15 /
     burn 100, which is what makes a fire dangerous to it;
  3. a second client that joins while that fuse burns is told about it during
     its own join burst (`send_live_tnt`), at the fuse it has left;
  4. flint and steel on another TNT block sends the block's own cell to air,
     spawns the entity, and then syncs a *decreasing* fuse — the countdown;
  5. the fuse reaching zero is a `remove_entities` followed by the blast: block
     updates to air in cells the blast reached, and a `damage_event` for the
     player standing 4.5 blocks away, with the primed entity as the *direct*
     source and the player themselves as the cause (a TNT you lit is your own
     `player_explosion`, exactly as vanilla credits it), plus a knockback;
  6. two adjacent TNT blocks chain: priming the first ends with a second
     `add_entity` at the second's own cell, with a *short* fuse, and no item
     entity there, because `dropFromExplosion` is false for TNT.

Three honest notes. Every click in the file proves itself: the server acks the
`use_item_on` sequence on every path `handle_block_place` can take — the reach
and cursor refusals included — so a placement asserts its own ack and its own
aimed cell rather than reading the drain as a set of block updates. (That is not
belt-and-braces: a set comparison over a drain cannot confirm or refute a
placement, because the window is not private and a stream that has lost one byte
of framing decodes cells nobody wrote. See `place`, and the "placement that never
was" section of docs/PHASE3.md.) The damage the player takes is not asserted to
a number: at
4.5 blocks the fall-off leaves them at a few hearts, and the test asserts that
the hit landed, that it was the right *kind* of hit, and that it was not lethal.
And the creative half of the test is the *control* for the damage in step 5:
the same blasts happen around the same player while they are invulnerable, and
send no damage event at all.

This file is also where the wire client's transcript is wired up, because the
flakes above are the reason it exists: `PLUSMC_TRANSCRIPT_DIR=... python3
tests/tnt_wire_test.py <binary>` records the whole run (both clients, both
directions, written even when the run fails), and `PLUSMC_REPLAY=<file> python3
tests/tnt_wire_test.py` re-runs this file against that recording with no server
at all — same steps, same numbers — so a failure can be re-read instead of
re-raced. See `tests/wire_client.py` and the "A failure you can read back"
section of docs/PHASE3.md.

The fuse, the flight of the entity and the blast are all the *world* clock, which
`PLUSMC_TICK_SCALE=2` compresses — this test measures none of them in survival
ticks, and a ping/pong barrier could not: it bounds the player's tick, and the
queue time of a blast (see step 5) is a world-tick event. The server does count
packets for the player's survival tick (`PLUSMC_SURVIVAL_CLOCK=packet`), which is
what makes `tick(bot, 12)` twelve ticks and not however long the drain took.
"""
import math, os, re, shutil, socket, struct, subprocess, sys, tempfile, threading, time
import wire_client
from wire_client import make_frame, read_packet

PROTO = 777
HOST = "127.0.0.1"

# Item ids (src/world/item_registry.h).
ITEM_TNT = 853
ITEM_FLINT_AND_STEEL = 1005

# Entity type ids (src/protocol/ids.h).
TYPE_ITEM = 72
TYPE_PRIMED_TNT = 136

# Damage types (src/world/damage_table.h, in wire order).
DMG_PLAYER_EXPLOSION = 35

# Clientbound packet ids.
CB_SPAWN_ENTITY = 0x01
# `block_changed_ack`: [varint sequence]. `handle_block_place` writes one on
# *every* path it can take — the reach and cursor refusals included — so it is
# the proof that the server processed a click, whatever it then decided.
CB_BLOCK_CHANGED_ACK = 0x04
CB_BLOCK_UPDATE = 0x08
CB_DAMAGE_EVENT = 0x19
CB_KEEP_ALIVE = 0x2D
CB_REMOVE_ENTITIES = 0x4E
CB_SET_ENTITY_DATA = 0x65
CB_SET_ENTITY_MOTION = 0x67
CB_ENTITY_POSITION_SYNC = 0x23
CB_EXPLODE = 0x24
CB_SET_HEALTH = 0x6A
CB_PLAYER_POSITION = 0x49
CB_PLAYER_COMBAT_KILL = 0x45

# Serverbound packet ids.
SB_CHAT_COMMAND = 0x07
SB_FLYING = 0x21
SB_POSITION = 0x1E
SB_SET_CARRIED_ITEM = 0x36
SB_SET_CREATIVE_SLOT = 0x39
SB_USE_ITEM_ON = 0x42
SB_KEEP_ALIVE = 0x1C

# Registry ids the explode packet names (src/world/explosion_effects_table.h,
# read from the 26.2 server's own --reports registry dump: particle_type and
# sound_event are built-in registries, so the ids are registration order).
PARTICLE_EXPLOSION_EMITTER = 29
PARTICLE_EXPLOSION = 30
PARTICLE_POOF = 66
PARTICLE_SMOKE = 69
SOUND_GENERIC_EXPLODE = 699  # a Holder reference rides as id + 1

# Synched-entity-data indices and serializers (src/server/entity_packets.h).
FUSE_INDEX = 8
SER_BYTE = 0
SER_INT = 1
SER_FLOAT = 3
SER_BLOCK_STATE = 8

# The fixture world's flat floor is grass at -61, so everything the test places
# sits at -60: the cell above the ground.
GROUND_Y = -61
ON_Y = -60

# The three scenes. The fire has *two* TNT neighbours so `checkBurnOut` rolls for
# one of them twice a tick (100 burn odds against a 300 chance, 250 above and
# below), which lands the prime in a fire tick or two rather than on a coin flip.
FIRE_CELL = (4, ON_Y, 19)
FIRE_TNT = [(4, ON_Y, 20), (4, ON_Y, 18)]
CHAIN_TNT = (12, ON_Y, 4)
CHAIN_NEIGHBOUR = (13, ON_Y, 4)
SOLO_TNT = (4, ON_Y, 4)

# Menu slots: hotbar slot 0 is menu slot 36.
SLOT_MENU_HOTBAR_FIRST = 36
SLOT_TNT = SLOT_MENU_HOTBAR_FIRST
SLOT_FLINT = SLOT_MENU_HOTBAR_FIRST + 1

# Where the player stands to click things: inside the 4.5-block interaction
# range of the clicked ground cell, and clear of the cells being built on.
FIRE_STAND = (6.5, float(ON_Y), 19.0)
CHAIN_STAND = (12.5, float(ON_Y), 6.0)
BUILD_STAND = (6.0, float(ON_Y), 4.0)
# The blast the player is meant to be caught by: 4.5 blocks from the primed
# TNT's centre, which leaves them alive (the fall-off is about 18.6 of 20).
BLAST_STAND = (4.5, float(ON_Y), 9.0)
# Far enough that a power-4 blast cannot reach: the query is a 9-block box and
# the damage gate is 8.
CLEAR_STAND = (12.5, float(ON_Y), 13.5)

# The blast's own numbers (src/world/tnt.h). `kTntExplosionPower` is the 4.0
# `PrimedTnt` explodes with, `PrimedTnt.explode` takes its centre
# `getY(0.0625)` up the entity's 0.98-high cube, and the knockback's origin is
# `Entity.getEyePosition`, i.e. the player's feet plus the standing eye height.
TNT_POWER = 4.0
TNT_CENTRE_LIFT = 0.0625 * 0.98
EYE_HEIGHT = 1.62

# 2 game ticks per real tick: an 80-tick fuse is ~2 s and a fire's own tick
# (30..40 game ticks) about a second.
TICK_SCALE = 2
FUSE_SECONDS = 6.0
CHAIN_SECONDS = 8.0
FIRE_SECONDS = 15.0
TICK_MS = 0.05


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


# --- readers ----------------------------------------------------------------

def read_lp_vec3(body, pos):
    """LpVec3 (src/protocol/lp_vec.h), decoded the way the encoder packs it: a
    zero vector is one 0x00 byte, otherwise two marker bytes and a u32 holding
    three 15-bit components, plus a scale varint when the first byte's low three
    bits say the scale did not fit. (`pos += 6` would be right only for a scale
    of 1..3, which is why this decodes it rather than skipping it.)

    The six bytes are *not* a little-endian integer. The writer is
    `u8(buffer) | u8(buffer >> 8) | u32(buffer >> 16)`, so the four bytes after
    the first two are the buffer's high half, big-endian; reading all six
    little-endian swaps the middle two bytes of the u32 with its ends, which
    scrambles every component into a different plausible vector. The decoded
    numbers are asserted below rather than merely read, because that is the kind
    of wrong a test cannot notice by looking at it.
    """
    first = body[pos]
    if first == 0:
        return (0.0, 0.0, 0.0), pos + 1
    assert pos + 6 <= len(body), "lp_vec3: truncated marker bytes"
    markers = first & 0x7
    packed = (first | (body[pos + 1] << 8)
              | (struct.unpack(">I", body[pos + 2:pos + 6])[0] << 16))
    pos += 6
    if markers & 0x4:
        extra, pos = rd_varint(body, pos)
        scale = (extra << 2) | (markers & 0x3)
    else:
        scale = markers

    def unpack(bits):
        return (bits / 32766.0 * 2.0 - 1.0) * scale

    return (unpack((packed >> 3) & 0x7FFF), unpack((packed >> 18) & 0x7FFF),
            unpack((packed >> 33) & 0x7FFF)), pos


def read_position_sync(body):
    """sync_entity_position (26.3): [varint eid][varint PositionPath type =
    LINEAR 0][f64 x y z][f32 yRot][f32 xRot][bool onGround]. The authoritative
    position of one entity, which the world tick writes once per tick for every
    primed TNT; 26.3 no longer carries delta movement on this packet."""
    eid, pos = rd_varint(body, 0)
    path_type, pos = rd_varint(body, pos)
    assert path_type == 0, f"position_sync path type {path_type}"
    x, y, z = struct.unpack(">ddd", body[pos:pos + 24])
    pos += 24
    yaw, pitch = struct.unpack(">ff", body[pos:pos + 8])
    pos += 8
    ground = bool(body[pos])
    pos += 1
    assert pos == len(body), f"position_sync: {len(body) - pos} trailing byte(s)"
    return {"entity": eid, "x": x, "y": y, "z": z,
            "yaw": yaw, "pitch": pitch, "ground": ground}


def read_explode(body):
    """explode (0x24): [f64 x y z][f32 radius][varint block count]
    [bool has knockback][f64 x y z when set][varint particle][varint sound]
    [varint block-particle count], then per entry [varint particle][f32
    scaling][f32 speed][varint weight]."""
    x, y, z = struct.unpack(">ddd", body[0:24])
    radius, = struct.unpack_from(">f", body, 24)
    pos = 28
    blocks, pos = rd_varint(body, pos)
    has_knockback = bool(body[pos])
    pos += 1
    knockback = None
    if has_knockback:
        knockback = struct.unpack(">ddd", body[pos:pos + 24])
        pos += 24
    particle, pos = rd_varint(body, pos)
    sound, pos = rd_varint(body, pos)
    count, pos = rd_varint(body, pos)
    block_particles = []
    for _ in range(count):
        pid, pos = rd_varint(body, pos)
        scaling, speed = struct.unpack_from(">ff", body, pos)
        pos += 8
        weight, pos = rd_varint(body, pos)
        block_particles.append((pid, round(scaling, 4), round(speed, 4), weight))
    assert pos == len(body), f"explode: {len(body) - pos} trailing byte(s)"
    return {"x": x, "y": y, "z": z, "radius": radius, "blocks": blocks,
            "knockback": knockback, "particle": particle, "sound": sound,
            "block_particles": block_particles}


def read_block_update(body):
    """block_update: [i64 packed position][varint state id]."""
    raw = struct.unpack(">q", body[0:8])[0]
    state, pos = rd_varint(body, 8)
    assert pos == len(body), f"block_update: {len(body) - pos} trailing byte(s)"
    return unpack_position(raw), state


def read_spawn(body):
    """Add Entity: [varint eid][uuid 16][varint type][f64 x y z][LpVec3]
    [u8 pitch][u8 yaw][u8 head yaw][varint data]."""
    eid, pos = rd_varint(body, 0)
    uuid = body[pos:pos + 16]
    pos += 16
    etype, pos = rd_varint(body, pos)
    x, y, z = struct.unpack(">ddd", body[pos:pos + 24])
    pos += 24
    velocity, pos = read_lp_vec3(body, pos)
    pitch, yaw, head = body[pos], body[pos + 1], body[pos + 2]
    pos += 3
    data, pos = rd_varint(body, pos)
    assert pos == len(body), f"spawn_entity: {len(body) - pos} trailing byte(s)"
    return {"entity": eid, "uuid": uuid, "type": etype, "x": x, "y": y, "z": z,
            "velocity": velocity, "pitch": pitch, "yaw": yaw, "head": head, "data": data}


def read_entity_data(body, wanted):
    """Set Entity Data for `wanted`: {index: value}. Another entity's packet
    returns None before anything is read, so this never has to understand every
    serializer a mob might send."""
    eid, pos = rd_varint(body, 0)
    if eid != wanted:
        return None
    values = {}
    while True:
        index = body[pos]
        pos += 1
        if index == 0xFF:
            break
        serializer, pos = rd_varint(body, pos)
        if serializer in (SER_INT, SER_BLOCK_STATE):
            value, pos = rd_varint(body, pos)
        elif serializer == SER_BYTE:
            value = body[pos]
            pos += 1
        elif serializer == SER_FLOAT:
            value = struct.unpack(">f", body[pos:pos + 4])[0]
            pos += 4
        else:
            raise AssertionError(f"entity data index {index}: serializer {serializer}")
        values[index] = value
    assert pos == len(body), f"set_entity_data: {len(body) - pos} trailing byte(s)"
    return values


def read_remove_entities(body):
    """remove_entities: [varint count][varint id]..."""
    count, pos = rd_varint(body, 0)
    ids = []
    for _ in range(count):
        value, pos = rd_varint(body, pos)
        ids.append(value)
    assert pos == len(body), f"remove_entities: {len(body) - pos} trailing byte(s)"
    return ids


def read_damage_event(body):
    """damage_event: [varint entity][varint type][varint cause+1]
    [varint direct+1][bool has position][3 doubles]."""
    pos = 0
    eid, pos = rd_varint(body, pos)
    type_id, pos = rd_varint(body, pos)
    cause, pos = rd_varint(body, pos)
    direct, pos = rd_varint(body, pos)
    has_position = body[pos]
    pos += 1
    if has_position:
        pos += 24
    assert pos == len(body), f"damage_event: {len(body) - pos} trailing byte(s)"
    return {"entity": eid, "type": type_id, "cause": cause - 1, "direct": direct - 1}


def read_set_health(body):
    """set_health: [f32 health][varint food][f32 saturation]."""
    health = struct.unpack(">f", body[0:4])[0]
    food, pos = rd_varint(body, 4)
    saturation = struct.unpack(">f", body[pos:pos + 4])[0]
    pos += 4
    assert pos == len(body), f"set_health: {len(body) - pos} trailing byte(s)"
    return round(health, 4), food, round(saturation, 4)


def read_motion(body):
    """set_entity_motion: [varint eid][LpVec3]."""
    eid, pos = rd_varint(body, 0)
    velocity, pos = read_lp_vec3(body, pos)
    assert pos == len(body), f"set_entity_motion: {len(body) - pos} trailing byte(s)"
    return eid, velocity


# --- the client -------------------------------------------------------------

class Bot:
    def __init__(self, name, port):
        self.name = name
        # A real connection, or — when PLUSMC_REPLAY names a transcript — the
        # recorded stream for *this* client, so the whole test (both bots) can be
        # re-run offline. See tests/wire_client.py's transcript section.
        self.sock = wire_client.transport(HOST, port)
        self.sock.settimeout(10)
        self.threshold = -1
        self.entity_id = None
        self.position = None
        self.join_packets = []
        # The read side belongs to a thread once the join is done: a windowed
        # drain loses packets, and the fuse here is only about two seconds long.
        # See tests/wire_client.py: it reads whole frames on a blocking socket,
        # so a window that closes between two of them loses nothing.
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
        seen = self.join_packets
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
            seen.append((pid, body))
            if pid == CB_KEEP_ALIVE:
                self.send(SB_KEEP_ALIVE, body[:8])
            elif pid == 0x0B:
                batch, _ = rd_varint(body)
                self.send(0x0B, varint(batch))
                break
        return seen

    def move_to(self, x, y, z, ground=1):
        self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([ground]))


def drain(bot, timeout=1.0):
    """Every packet that has arrived in the next `timeout` seconds. Before the
    reader thread starts (the join itself) the socket is read directly."""
    # A reply window ends when the server has been quiet for `quiet` seconds
    # rather than always burning the whole window; `timeout` stays the bound a
    # missing reply is judged against. Requires the reader (started after each
    # join below).
    return wire_client.drain_quiet(bot, quiet=0.3, limit=timeout)


def use_item_on(bot, x, y, z, face, seq, cursor=(0.5, 0.5, 0.5)):
    """ServerboundUseItemOn: [varint hand][i64 position][varint face][3 f32
    cursor][bool inside][bool border][varint sequence]."""
    bot.send(SB_USE_ITEM_ON,
             varint(0) + position(x, y, z) + varint(face)
             + struct.pack(">fff", *cursor) + bytes([0, 0]) + varint(seq))


def read_ack(body):
    """`block_changed_ack`: the sequence number the server is answering."""
    seq, _ = rd_varint(body, 0)
    return seq


def select(bot, menu_slot):
    bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", menu_slot - SLOT_MENU_HOTBAR_FIRST))
    drain(bot, 0.2)


def creative_item(bot, menu_slot, item_id, count=64):
    bot.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", menu_slot) + varint(count)
             + varint(item_id) + b"\x00\x00")
    drain(bot, 0.2)


def send_chat_command(bot, command):
    bot.send(SB_CHAT_COMMAND, varint(len(command)) + command.encode())


def tick(bot, count):
    """Advance the survival clock by exactly `count` game ticks (one `flying`
    packet each, which `PLUSMC_SURVIVAL_CLOCK=packet` makes one tick each) and
    give the world clock `count` packets' worth of real time.

    The survival tick is what applies a blast's queued hit to the player, so
    this is how the test asks for it; the sleep is what the *world* clock (the
    fuse, gravity, the fire) rides, multiplied by `PLUSMC_TICK_SCALE`. There is
    no barrier here: the queue time of a blast is a world-tick event, and the
    `remove_entities` that opens the blast is written *before* `explode_tnt`
    runs, so a ping cannot bound when a hit became pending.
    """
    for _ in range(count):
        bot.send(SB_FLYING, bytes([1]))
        time.sleep(TICK_MS)


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


class Session:
    """A bot plus everything it has seen, so a step can look back at packets
    that arrived while it was waiting for something else."""

    def __init__(self, bot):
        self.bot = bot
        self.seq = 1
        self.seen = []

    def observe(self, packets):
        self.seen += packets
        return packets

    def command(self, text, timeout=1.2):
        step(f"-> /{text}")
        send_chat_command(self.bot, text)
        return self.observe(drain(self.bot, timeout))

    def move_to(self, xyz):
        step(f"-> stand at ({xyz[0]:.1f}, {xyz[1]:.0f}, {xyz[2]:.1f})")
        self.bot.move_to(*xyz)
        time.sleep(0.3)

    def place(self, cells, item_slot):
        """Click the ground's up face under each cell and *prove* every click
        landed, one click at a time.

        A set comparison over "the block updates in this window" is not
        evidence, and this is where that was learned the hard way. The window is
        not private — the fire lit in step 2 goes on writing in its own corner of
        the world while later scenes are built — and worse, a stream that has
        lost its framing decodes *plausible* cells that were never written: a
        `block_update` is a packed position and an id, and one missing byte
        anywhere in the stream will shift every packet after it into a `(5, -60,
        18)` that looks exactly like a real placement. (The failure that led here
        claimed "the TNT blocks did not land" while the server log for the same
        run said `placed minecraft:tnt at (4, -60, 20)` and `(4, -60, 18)`; a
        probe that drops one byte from the socket reproduces that report on
        demand — see docs/PHASE3.md.)

        So each click is identified rather than inferred. `handle_block_place`
        acks the click's sequence on every path, refusals included, so the ack
        is proof the server *processed* the click and a missing one means the
        read side is broken, not that a placement was refused. And the cell the
        click aimed at must then carry a block — checked per cell, with the
        cells the scene next door writes ignored on purpose.
        """
        select(self.bot, item_slot)
        seqs = []
        for x, _y, z in cells:
            use_item_on(self.bot, x, GROUND_Y, z, 1, self.seq)
            seqs.append(self.seq)
            self.seq += 1
        packets = self.observe(drain(self.bot, 1.5))
        acked = {read_ack(b) for p, b in packets if p == CB_BLOCK_CHANGED_ACK}
        unacked = [s for s in seqs if s not in acked]
        assert not unacked, (
            f"the server never acked click(s) {unacked}, so this is a broken read "
            f"side and not a refused placement: {len(packets)} packet(s) in the "
            f"window, acks {sorted(acked)}")
        landed = {at for at, state in [read_block_update(b) for p, b in packets
                                       if p == CB_BLOCK_UPDATE]
                  if state != 0}
        missed = [c for c in cells if c not in landed]
        assert not missed, (
            f"click(s) acked and nothing landed in {missed}: the updates were "
            f"{sorted(landed)} (ids {[hex(p) for p, _ in packets]}). If those "
            f"acks are themselves doubtful, read it as a mis-framed stream: "
            f"see this method's docstring")
        return packets

    def light(self, x, y, z):
        """Flint and steel on the ground cell under `(x, z)`, so the fire or the
        prime lands in the cell at `y` — this file's other click, held to the
        same proof as `place`: the sequence it sent must be acked, or nothing
        below can be read as the server's answer to it."""
        select(self.bot, SLOT_FLINT)
        use_item_on(self.bot, x, y, z, 1, self.seq)
        seq = self.seq
        self.seq += 1
        packets = self.observe(drain(self.bot, 1.0))
        assert seq in {read_ack(b) for p, b in packets if p == CB_BLOCK_CHANGED_ACK}, (
            f"the server never acked the light click (seq {seq}), so this is a "
            f"broken read side and not a refused interaction: {len(packets)} "
            f"packet(s) in the window")
        return packets

    def watch(self, seconds, done, slice_seconds=0.2):
        """Drain until `done()` or the deadline; the packets seen are returned
        and kept in `self.seen`."""
        seen = []
        deadline = time.time() + seconds
        while time.time() < deadline and not done():
            batch = drain(self.bot, slice_seconds)
            self.observe(batch)
            seen += batch
        return seen

    def spawns(self, entity_type):
        out = []
        for body in [b for p, b in self.seen if p == CB_SPAWN_ENTITY]:
            spawn = read_spawn(body)
            if spawn["type"] == entity_type:
                out.append(spawn)
        return out

    def spawns_at(self, entity_type, x, y, z, slack=0.6):
        return [s for s in self.spawns(entity_type)
                if abs(s["x"] - x) <= slack and abs(s["y"] - y) <= slack
                and abs(s["z"] - z) <= slack]

    def fuses(self, eid):
        out = []
        for body in [b for p, b in self.seen if p == CB_SET_ENTITY_DATA]:
            values = read_entity_data(body, eid)
            if values is not None and FUSE_INDEX in values:
                out.append(values[FUSE_INDEX])
        return out

    def despawned(self, eid):
        for body in [b for p, b in self.seen if p == CB_REMOVE_ENTITIES]:
            if eid in read_remove_entities(body):
                return True
        return False

    def block_updates(self):
        return [read_block_update(b) for p, b in self.seen if p == CB_BLOCK_UPDATE]

    def damage_events(self, entity_id):
        out = []
        for body in [b for p, b in self.seen if p == CB_DAMAGE_EVENT]:
            event = read_damage_event(body)
            if event["entity"] == entity_id:
                out.append(event)
        return out

    def healths(self):
        return [read_set_health(b) for p, b in self.seen if p == CB_SET_HEALTH]

    def motions(self):
        return [read_motion(b) for p, b in self.seen if p == CB_SET_ENTITY_MOTION]

    def air_cells(self):
        return {at for at, state in self.block_updates() if state == 0}


def main():
    # The binary is only needed for a live run: a replayed run starts no server,
    # so it can be re-asked about a failure by anyone holding the transcript.
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}  "
              f"(no binary needed)", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    fixtures = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")
    world = tempfile.mkdtemp(prefix="plusmc-tnt-")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_TICK_SCALE"] = str(TICK_SCALE)
    # The player's survival tick (which applies a blast's queued hit) is pinned
    # to this test's own packets — see the module docstring. The fuse and the
    # blast stay on the world clock, which TICK_SCALE above compresses.
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
    # The spawn burst is the join's real cost: at the default 9 chunks a tick a
    # view distance is delivered one batch at a time, which measured ~2.5 s —
    # and step 3 asks a client to join *while an 80-tick fuse is burning*, which
    # at TICK_SCALE 2 is 2.0 s of world time. Under load the join grew and the
    # fuse did not, so the TNT it was supposed to be paired with had been
    # discarded by the time the play state arrived. A test bot has no meshing to
    # hide behind, so handing it the view in one tick removes the race without
    # touching what step 3 asserts.
    env["PLUSMC_CHUNKS_PER_TICK"] = "1000"

    proc = None
    viewer = None
    try:
        # Replayed run: no server, no world, no fixture — the packets come from
        # the transcript this run was pointed at. A replayed run that fails is a
        # failure of the *reading* (the assertions over the recorded packets),
        # not of the server, which is the point: the server that produced this
        # transcript is gone.
        if not wire_client.replaying():
            region_dir = os.path.join(world, "dimensions", "minecraft", "overworld", "region")
            os.makedirs(region_dir, exist_ok=True)
            shutil.copyfile(os.path.join(fixtures, "r.0.0.mca"),
                            os.path.join(region_dir, "r.0.0.mca"))
            step(f"world dir: {world} (flat fixture: chunk (0,0) is plains)")
        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--motd", "tnt test",
             "--offline-mode", "--gamemode", "1", "--data", data_root, "--world", world],
            wait_listening, env=env)

        bot = Bot("TntBot", port)
        # Record the run when asked (PLUSMC_TRANSCRIPT_DIR=... or
        # PLUSMC_TRANSCRIPT=file): the file is written at exit even when the
        # test fails, which is when it is worth having.
        wire_client.record(bot, note="tnt-wire: fire priming, flint and steel, the "
                                     "fuse, the blast and the chain")
        join_packets = bot.join()
        bot.start_reader()
        join_packets += drain(bot, 1.0)
        session = Session(bot)
        attrs = [b for p, b in join_packets if p == 0x86]  # update_attributes
        assert attrs, "the join must push the attribute set (which carries the entity id)"
        bot.entity_id, _ = rd_varint(attrs[-1], 0)

        # --- 1. a fresh world has no TNT -------------------------------------
        step("-> join: a fresh world carries no primed TNT")
        assert not [read_spawn(b) for p, b in join_packets if p == CB_SPAWN_ENTITY
                    and read_spawn(b)["type"] == TYPE_PRIMED_TNT], \
            "nothing was primed, so nothing may spawn"

        # --- 2. the fire path -------------------------------------------------
        step("-> creative: TNT round a fire, and the fire's own clock")
        creative_item(bot, SLOT_TNT, ITEM_TNT, 64)
        creative_item(bot, SLOT_FLINT, ITEM_FLINT_AND_STEEL, 1)
        session.move_to(FIRE_STAND)
        # `place` proves each click itself — the server acked its sequence, and
        # the cell it aimed at holds a block — rather than comparing the window's
        # updates as a set. Nothing here takes the drain's word for a cell it
        # never asked for (see `place`, which is where that used to go wrong).
        session.place(FIRE_TNT, SLOT_TNT)
        step(f"  OK both TNT blocks landed at {FIRE_TNT}")

        lit = session.light(FIRE_CELL[0], GROUND_Y, FIRE_CELL[2])
        ats = [at for at, state in [read_block_update(b) for p, b in lit if p == CB_BLOCK_UPDATE]
               if at == FIRE_CELL and state != 0]
        assert ats, "the fire never landed"

        step(f"-> watch {FIRE_SECONDS:.0f}s: checkBurnOut primes a TNT neighbour")
        session.watch(FIRE_SECONDS,
                      lambda: bool(session.spawns_at(TYPE_PRIMED_TNT, FIRE_TNT[0][0] + 0.5,
                                                     float(FIRE_TNT[0][1]), FIRE_TNT[0][2] + 0.5)
                                   or session.spawns_at(TYPE_PRIMED_TNT, FIRE_TNT[1][0] + 0.5,
                                                        float(FIRE_TNT[1][1]), FIRE_TNT[1][2] + 0.5)))
        by_fire = (session.spawns_at(TYPE_PRIMED_TNT, FIRE_TNT[0][0] + 0.5, float(FIRE_TNT[0][1]),
                                     FIRE_TNT[0][2] + 0.5)
                   + session.spawns_at(TYPE_PRIMED_TNT, FIRE_TNT[1][0] + 0.5, float(FIRE_TNT[1][1]),
                                       FIRE_TNT[1][2] + 0.5))
        assert by_fire, "the fire never primed its TNT neighbour (no add_entity for type 133)"
        fire_eid = by_fire[0]["entity"]
        fire_fuse = session.fuses(fire_eid)
        assert fire_fuse and fire_fuse[0] == 80, \
            f"a fire's prime is TntBlock.prime's default 80-tick fuse, not {fire_fuse[:4]}"
        assert not session.damage_events(bot.entity_id), \
            "a creative player cannot be hurt by a blast"
        step(f"  OK the fire primed entity {fire_eid} with a {fire_fuse[0]}-tick fuse, "
             f"and the blast it became hurt nobody in creative")

        # --- 3. a viewer that joins mid-fuse is told about it -----------------
        step("-> a second client joins while that fuse is burning")
        viewer = Bot("TntViewer", port)
        # The second client is a connection of its own in the transcript: a
        # replay serves each record back to the client that made it.
        wire_client.record(viewer, note="tnt-wire: the viewer that joins mid-fuse")
        viewer_join = viewer.join()
        viewer.start_reader()
        # A joiner is tracked from the position it logs in at, which is the
        # world spawn and not this scene; the live-entity pairing is gated by
        # distance like every other pairing now (docs/MOB_VISIBILITY.md). Walk
        # it to the scene and let the five-tick sweep pair it with the entity
        # that is still burning — the fuse it is sent is the fuse it has left.
        viewer.move_to(6.0, float(ON_Y), 16.0)
        viewer_join += drain(viewer, 1.0)
        live = [read_spawn(b) for p, b in viewer_join if p == CB_SPAWN_ENTITY
                and read_spawn(b)["type"] == TYPE_PRIMED_TNT]
        assert live, "the joiner was never paired with the TNT still burning in range"
        viewer_fuse = [read_entity_data(b, live[0]["entity"]) for p, b in viewer_join
                       if p == CB_SET_ENTITY_DATA]
        viewer_fuse = [v[FUSE_INDEX] for v in viewer_fuse if v is not None and FUSE_INDEX in v]
        assert viewer_fuse and 0 < viewer_fuse[0] <= 80, f"the joiner's fuse: {viewer_fuse}"
        step(f"  OK the joiner saw entity {live[0]['entity']} at fuse {viewer_fuse[0]}")

        # --- 4. chaining -------------------------------------------------------
        step(f"-> creative: two adjacent TNT blocks at {CHAIN_TNT} and {CHAIN_NEIGHBOUR}")
        session.move_to(CHAIN_STAND)
        # The two clicks are what is asserted, one ack and one cell each. The
        # window itself is not quiet: the fire lit in step 2 is still burning in
        # its own corner of the world (and the TNT it primed is still ticking),
        # so its writes land in this drain too — a fire spreading into the cell a
        # primed neighbour left behind made this step fail roughly one run in
        # three. Ignoring every cell but the two clicked is what keeps the claim
        # about the clicks, and the ack check in `place` is what tells a refused
        # click apart from a read side that has lost its framing.
        session.place([CHAIN_TNT, CHAIN_NEIGHBOUR], SLOT_TNT)
        step(f"  OK both chained-pair TNT blocks landed at {CHAIN_TNT} and "
             f"{CHAIN_NEIGHBOUR}")

        session.move_to((CHAIN_TNT[0] + 1.0, float(ON_Y), CHAIN_TNT[2] + 2.0))
        lit = session.light(CHAIN_TNT[0], CHAIN_TNT[1], CHAIN_TNT[2])
        first = [s for s in (read_spawn(b) for p, b in lit if p == CB_SPAWN_ENTITY)
                 if s["type"] == TYPE_PRIMED_TNT]
        assert first, "the first block of the pair must prime"
        step(f"  OK entity {first[0]['entity']} primed at {CHAIN_TNT}")
        session.move_to(CLEAR_STAND)

        neighbour_xy = (CHAIN_NEIGHBOUR[0] + 0.5, float(CHAIN_NEIGHBOUR[1]),
                        CHAIN_NEIGHBOUR[2] + 0.5)
        session.watch(CHAIN_SECONDS,
                      lambda: bool(session.spawns_at(TYPE_PRIMED_TNT, *neighbour_xy)))
        chained = session.spawns_at(TYPE_PRIMED_TNT, *neighbour_xy)
        assert chained, "the blast must prime the TNT beside it (`TntBlock.wasExploded`)"
        short = session.fuses(chained[0]["entity"])
        assert short and 10 <= short[0] <= 29, \
            f"a chained prime is getRandomShortFuse(80) = 10..29, not {short[:4]}"
        assert not session.spawns_at(TYPE_ITEM, neighbour_xy[0], neighbour_xy[1] + 0.5,
                                     neighbour_xy[2], slack=0.2), \
            "TNT drops nothing when it explodes (dropFromExplosion is false)"
        # Both of them go off: the chained entity is discarded exactly as the
        # first one was, which is the blast resolving rather than fizzling. Its
        # 10..29-tick fuse is a fraction of the first one's, so the wait is short.
        chained_eid = chained[0]["entity"]
        session.watch(CHAIN_SECONDS, lambda: session.despawned(chained_eid), slice_seconds=0.05)
        assert session.despawned(chained_eid), "the chained TNT must explode too"
        step(f"  OK the blast chained entity {chained[0]['entity']} with a "
             f"{short[0]}-tick fuse, dropped nothing, and it exploded")

        # The explode packet rides vanilla's fixed 64-block sphere
        # (`ServerLevel.explode`: `distanceToSqr(center) < 4096.0`), not the
        # entity tracking range: walk the second client out of it so the solo
        # blast below has a control that must be told nothing. Its window is
        # cleared first — the fire and chain blasts above were inside the sphere.
        step("-> the viewer walks 96 blocks out of the blast's packet sphere")
        viewer.move_to(6.0, float(ON_Y), 100.0)
        drain(viewer, 0.6)
        far_seen = []

        # --- 5. the fuse and the blast, in survival ----------------------------
        step(f"-> survival: a TNT {BLAST_STAND[2] - SOLO_TNT[2] - 0.5:.1f} blocks from the player")
        session.command("gamemode survival", timeout=1.0)
        session.move_to(BUILD_STAND)
        session.place([SOLO_TNT], SLOT_TNT)
        step(f"  OK the solo TNT landed at {SOLO_TNT}")
        session.move_to(BLAST_STAND)

        lit = session.light(SOLO_TNT[0], SOLO_TNT[1], SOLO_TNT[2])
        spawns = [s for s in (read_spawn(b) for p, b in lit if p == CB_SPAWN_ENTITY)
                  if s["type"] == TYPE_PRIMED_TNT]
        assert spawns, "flint and steel must prime the block"
        tnt_eid = spawns[0]["entity"]
        assert (SOLO_TNT, 0) in [read_block_update(b) for p, b in lit if p == CB_BLOCK_UPDATE], \
            "the primed block must be taken away"
        fuses = session.fuses(tnt_eid)
        assert fuses and fuses[0] == 80, f"a fresh prime is 80 ticks, not {fuses[:4]}"
        step(f"  OK entity {tnt_eid} spawned at fuse {fuses[0]}, its block -> air")

        session.watch(FUSE_SECONDS, lambda: session.despawned(tnt_eid))
        assert session.despawned(tnt_eid), "the fuse must run out and discard the entity"
        # The blast's hit on a *player* is queued by the world tick and applied by
        # this session's own survival tick, which the client's packets drive:
        # without a flying packet the damage would sit in the queue. Twelve of them
        # is twelve ticks now (`PLUSMC_SURVIVAL_CLOCK=packet`), and the window is
        # deliberately wide: the `remove_entities` above is written *before*
        # `explode_tnt` runs, so the wire cannot say which tick filled the queue —
        # a tighter count would be a race rather than a claim.
        tick(bot, 12)
        session.observe(drain(bot, 0.8))
        far_seen += drain(viewer, 0.8)
        fuses = session.fuses(tnt_eid)
        assert len(fuses) >= 4, f"the fuse must be synced as it counts down: {fuses}"
        assert all(fuses[i] >= fuses[i + 1] for i in range(len(fuses) - 1)), \
            f"the fuse must not go back up: {fuses}"
        assert fuses[0] == 80 and fuses[-1] < 40, f"the fuse: {fuses[:3]}...{fuses[-3:]}"
        step(f"  OK the fuse ran {fuses[0]} -> {fuses[-1]} over {len(fuses)} sync packet(s)")

        destroyed = session.air_cells()
        assert len(destroyed) >= 3, f"a power-4 blast destroys blocks: {sorted(destroyed)}"
        assert SOLO_TNT in destroyed, "the TNT's own cell is air"

        events = session.damage_events(bot.entity_id)
        assert events, "the blast must hurt the player standing in it"
        hit = events[0]
        assert hit["type"] == DMG_PLAYER_EXPLOSION, \
            f"a player's own TNT is player_explosion (35), not {hit['type']}"
        assert hit["direct"] == tnt_eid, f"the direct source is the primed TNT: {hit}"
        assert hit["cause"] == bot.entity_id, f"the cause is the player who lit it: {hit}"
        hp = session.healths()
        assert hp and hp[-1][0] < 20.0, f"the hit must move health: {hp}"
        assert hp[-1][0] > 0.0, f"4.5 blocks away is not lethal: {hp[-1]}"
        assert not [b for p, b in session.seen if p == CB_PLAYER_COMBAT_KILL], \
            "the blast must not be lethal here"
        # ...and the impulse is a number, not just "nonzero".
        # `ServerExplosion.hurtEntities` pushes `normalize(origin - center) * p`,
        # where `origin` is the player's eye, `center` is the primed entity's own
        # position lifted by `getY(0.0625)` on its 0.98 cube, and
        # `p = (1 - distance / 2r) * exposure`. Both halves are things this run
        # knows: the entity's position arrives as a `sync_entity_position` every
        # tick, so the last one for it is where the blast found it, and `p` is the
        # fall-off the *damage* came out of — `explosion_entity_damage` puts it
        # through `(p^2 + p) / 2 * 7 * 2r + 1` and `explosion_knockback` scales
        # the unit direction by it, so reading it back off the health is what
        # keeps this from being a fitted constant (and the health is still not
        # asserted to a number of its own).
        pushed = [v for eid, v in session.motions() if eid == bot.entity_id]
        assert pushed, f"the blast must push the player: {pushed}"
        push = pushed[-1]
        tnt_at = [read_position_sync(b) for p, b in session.seen
                  if p == CB_ENTITY_POSITION_SYNC]
        tnt_at = [s for s in tnt_at if s["entity"] == tnt_eid]
        assert tnt_at, "the world tick syncs a primed entity's position every tick"
        # The lowest bar is the one the blast itself sent: the player is at full
        # health until here, and the tick *after* a hit can heal a fraction of a
        # point out of saturation (the last bar of this run is 0.8333 higher),
        # so "the health after the blast" is the minimum rather than the latest.
        damage = 20.0 - min(h for h, _, _ in hp)
        last = tnt_at[-1]
        centre = (last["x"], last["y"] + TNT_CENTRE_LIFT, last["z"])
        eye = (BLAST_STAND[0], BLAST_STAND[1] + EYE_HEIGHT, BLAST_STAND[2])
        direction = [eye[i] - centre[i] for i in range(3)]
        length = math.sqrt(sum(c * c for c in direction))
        # `damage - 1 = 28 * (p^2 + p)` for the power-4 blast (28 = 7 * 2r / 2),
        # so the fall-off is the positive root of `p^2 + p - (damage - 1) / 28`,
        # i.e. `(-1 + sqrt(1 + (damage - 1) / 7)) / 2`.
        power = (-1.0 + math.sqrt(1.0 + (damage - 1.0) / 7.0)) / 2.0
        want = tuple(c / length * power for c in direction)
        assert all(abs(a - b) < 2e-3 for a, b in zip(push, want)), \
            f"the blast's impulse is normalize(eye - centre) * p = {want} " \
            f"(p {power:.4f} for {damage:.4f} damage): got {push}"
        step(f"  OK player_explosion: direct {hit['direct']}, cause {hit['cause']}, "
             f"health {20.0 - damage:.4f} ({damage:.4f} damage), push "
             f"{tuple(round(c, 3) for c in push)}, fall-off {power:.4f}")

        # --- 6. the blast's own packet ----------------------------------------
        # `ServerLevel.explode`'s last step: one explode packet per player within
        # 64 blocks, and this client is 4.5 of them from the centre. The packet
        # names its particle, its sound and its block particles by *registry id*
        # (src/world/explosion_effects_table.h, read from the 26.2 server's own
        # `--reports` dump), so this is also what pins those numbers: a shifted
        # table reads here immediately.
        step("-> the blast's own packet: the particle, the sound and the knockback")
        blasts = [read_explode(b) for p, b in session.seen if p == CB_EXPLODE]
        # The primed entity drifts on its `TntBlock.prime` velocity (0.02 blocks
        # a tick), so the blast's centre is the last position sync's, not the
        # block's own cell centre: identify it against `centre`, the same place
        # the knockback assertion above reads the TNT at.
        solo = [e for e in blasts
                if all(abs(e[axis] - value) < 0.01
                       for axis, value in zip(("x", "y", "z"), centre))]
        assert len(solo) == 1, f"the solo blast's own packet: {solo} of {blasts}"
        explosion = solo[0]
        assert abs(explosion["y"] - centre[1]) < 0.01, \
            f"the packet's centre is the primed entity's: {explosion['y']} vs {centre[1]}"
        assert explosion["radius"] == 4.0, f"a TNT's power is the radius: {explosion['radius']}"
        assert explosion["blocks"] >= 3, \
            f"the block count is `toBlow.size()`, the ray list: {explosion['blocks']}"
        assert explosion["particle"] == PARTICLE_EXPLOSION_EMITTER, \
            f"a power-4 blast is large (`!isSmall`): {explosion['particle']}"
        assert explosion["particle"] != PARTICLE_EXPLOSION, "the small puff is the other arm"
        assert explosion["sound"] == SOUND_GENERIC_EXPLODE + 1, \
            f"GENERIC_EXPLODE as a holder reference (id + 1): {explosion['sound']}"
        assert explosion["block_particles"] == [(PARTICLE_POOF, 0.5, 1.0, 1),
                                                 (PARTICLE_SMOKE, 1.0, 1.0, 1)], \
            f"DEFAULT_EXPLOSION_BLOCK_PARTICLES: {explosion['block_particles']}"
        assert explosion["knockback"] is not None, \
            "the player the blast hit carries the packet's Optional<Vec3>"
        # The two carry the same vector, but only to LpVec3's precision: the
        # motion packet quantizes it (`read_lp_vec3`), the explode packet sends
        # three plain doubles.
        assert all(abs(a - b) < 2e-3 for a, b in zip(explosion["knockback"], push)), \
            f"the packet's knockback is the push the motion packet carried: " \
            f"{explosion['knockback']} vs {push}"
        step(f"  OK explode: centre {tuple(round(c, 3) for c in centre)}, radius "
             f"{explosion['radius']}, {explosion['blocks']} blocks, particle "
             f"{explosion['particle']}, sound {explosion['sound']}, knockback "
             f"{tuple(round(c, 3) for c in explosion['knockback'])}")
        # ...and the audience: vanilla's sphere is 64 blocks, so the client that
        # walked out of it must have received no explode packet at all.
        assert not [b for p, b in far_seen if p == CB_EXPLODE], \
            f"a client 96 blocks away is outside the packet's sphere: " \
            f"{[read_explode(b) for p, b in far_seen if p == CB_EXPLODE]}"
        step("  OK the client 96 blocks away was sent no explode packet")

        bot.close()
        viewer.close()
    finally:
        if proc is not None:
            proc.terminate()
            proc.wait(timeout=5)
    print("\nPASS: tnt wire test")
    return 0


if __name__ == "__main__":
    sys.exit(main())
