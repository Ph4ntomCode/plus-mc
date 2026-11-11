#!/usr/bin/env python3
"""Integration test: throwing a potion over the wire.

A splash or a lingering potion is not a consumable: `ThrowablePotionItem.use`
spawns an `AbstractThrownPotion` and consumes the bottle, and the *entity* is
what applies the effects — in a 4 x 2 x 4 box around where it broke, or through
the `AreaEffectCloud` a lingering one leaves behind. This test drives that end to
end against the real server, from a real terrain fixture the bottle can break on:

  * `/give TossBot minecraft:splash_potion[potion_contents={potion:"minecraft:
    harming"}]` hands out a bottle the menu decodes back, component and all;
  * using it *throws* instead of drinking: Add Entity (0x01) with the splash
    potion entity type (105) at the player's eyes minus 0.1, Set Entity Data
    (0x63) carrying the thrown *stack* (index 8, ITEM_STACK) with its
    potion_contents — which is what the client tints the bottle with — and the
    thrown slot resynced as empty (`ItemStack.consume`);
  * the "throw" rotation is the *packet's*: `handleUseItem` snaps the player to
    the yRot/xRot in `use_item` before the item runs, so a 90-degree pitch sends
    the bottle straight down rather than along the last movement packet's look;
  * the bottle *flies*: entity_position_sync (0x23) tracks it falling from the
    eyes, and it breaks on the floor — level_event (0x2e) 2007 (a harming potion
    carries an instantaneous effect) at the impact block, then remove_entities
    (0x4d);
  * the effects land on the thrower, who is a valid *area* victim even though
    `Projectile.leftOwner` refuses them a direct hit: damage_event (0x19) with
    the `indirect_magic` type (23) and the *thrower* as the causing entity,
    exactly 6 health (the impact point overlaps the victim's box, so the
    `1 - sqrt(distance) / 4` falloff is 1.0, and `6 << 0` is `Healing`'s other
    face);
  * a *mob* is a victim too, and at a distance: a pig summoned into the ring is
    reached by the same box but with the falloff applied, so its health drop is
    the rounded `(1 - 1.425 / 4) * 6` rather than 6;
  * a lingering potion leaves an area_effect_cloud (Add Entity type 3) sitting on
    the face the bottle broke on (`new AreaEffectCloud(level, getX(), getY(),
    getZ())`), radius 3.0 and the waiting flag set, whose radius then shrinks on
    every later update; and the cloud applies the same effects itself, at the
    lingering potion's own `minecraft:potion_duration_scale` of 0.25 —
    `minecraft:speed` for 900 ticks rather than 3600;
  * a creative throw still throws but keeps the bottle (`ItemStack.consume` is a
    no-op for a player with infinite materials).

Run: thrown_potion_test.py /path/to/plusmc
"""
import math, os, re, shutil, socket, struct, subprocess, sys, tempfile, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nbt_component import chat_text  # noqa: E402
import wire_client
from wire_client import make_frame, read_packet

PROTO = 777
HOST = "127.0.0.1"

# minecraft:entity_type protocol ids (src/protocol/ids.h).
ET_AREA_EFFECT_CLOUD = 3
ET_PIG = 101
ET_SPLASH_POTION = 108
ET_LINGERING_POTION = 109

# Item ids (src/world/item_registry.h).
ITEM_SPLASH_POTION = 1442
ITEM_LINGERING_POTION = 1445
ITEMS_BY_THROWN_NAME = {"splash_potion": ITEM_SPLASH_POTION,
                        "lingering_potion": ITEM_LINGERING_POTION}

# minecraft:potion ids (src/world/potion_table.h).
POTION_SWIFTNESS = 13
POTION_HARMING = 26

# minecraft:mob_effect ids (src/world/effect_table.h).
EFFECT_SPEED = 0

# minecraft:damage_type ids (src/world/damage_table.h). A thrown potion's harm
# is `damageSources().indirectMagic(source, owner)` — not the `magic` a *ticked*
# instant effect hurts with.
DMG_INDIRECT_MAGIC = 23

# The data component id of minecraft:potion_contents.
COMPONENT_POTION_CONTENTS = 53

# Clientbound packet ids.
CB_SPAWN_ENTITY = 0x01
CB_CONTAINER_SET_CONTENT = 0x12
CB_CONTAINER_SET_SLOT = 0x14
CB_DAMAGE_EVENT = 0x19
CB_ENTITY_POSITION_SYNC = 0x23
CB_HURT_ANIMATION = 0x2B
CB_LEVEL_EVENT = 0x2F
CB_REMOVE_ENTITIES = 0x4E
CB_SET_ENTITY_DATA = 0x65
CB_SET_HEALTH = 0x6A
CB_UPDATE_MOB_EFFECT = 0x87
CB_SYSTEM_CHAT = 0x7C
CB_UPDATE_ATTRIBUTES = 0x86
CB_KEEP_ALIVE = 0x2D
CB_PLAYER_POSITION = 0x49

# Serverbound packet ids.
SB_CHAT_COMMAND = 0x07
SB_FLYING = 0x21
SB_KEEP_ALIVE = 0x1C
SB_SET_CARRIED_ITEM = 0x36
SB_USE_ITEM = 0x43

# Entity data indices and serializers (src/server/entity_packets.h).
THROWABLE_ITEM_STACK_INDEX = 8
CLOUD_RADIUS_INDEX = 8
CLOUD_WAITING_INDEX = 9
CLOUD_PARTICLE_INDEX = 10
LIVING_HEALTH_INDEX = 9
SER_BYTE = 0
SER_FLOAT = 3
SER_ITEM_STACK = 7
SER_BOOLEAN = 8
SER_PARTICLE = 16
SER_PARTICLES = 17
# minecraft:entity_effect, the one particle type an effect's particle list
# carries a payload for (the ARGB colour).
PARTICLE_ENTITY_EFFECT = 28

UNSIGNED_MAX = 0xFFFFFFFF

FLAG_VISIBLE = 2
FLAG_SHOW_ICON = 4
FLAG_BLEND = 8

# AbstractThrownPotion.SPLASH_RANGE, and the entity box sizes the server uses.
SPLASH_RANGE = 4.0
POTION_HALF_WIDTH = 0.125
POTION_HEIGHT = 0.25
PIG_HALF_WIDTH = 0.45
PIG_HEIGHT = 0.9

# Where step 5's thrower stands, as an offset from the block the *summon* ring
# is measured from. `/summon` lands a mob at `player + 2` in x, so the thrower's
# own column is two blocks from the mob's cell; this offset then puts the
# box-to-box distance at 2.15 blocks — the middle of the band in which
# `(int)((1 - d / 4) * 6 + 0.5)` is the integer 3. The band is 2/3 of a block
# wide and the bottle's own age can widen the victim's box by up to 0.3
# (`potion_entity_margin`'s clamp), so only the middle holds a fixed integer.
THROW_OFFSET_X = -0.225
# The fixture's own commands are one drain each, and none of them needs a long
# window: the "/setblock" confirmation and its block update are the whole reply.
PLACE_TIMEOUT = 0.3

# Menu slots: a fresh session has hotbar 0 selected, which /give fills first.
SLOT_MENU_HOTBAR_FIRST = 36
MENU_HOTBAR_SLOTS = 9

TICK_MS = 0.055
# Looking straight down: `shootFromRotation(..., -20.0f, 0.5f, 1.0f)` tilts that
# by another -20 degrees, so the bottle leaves along -y at the 0.5 power.
PITCH_DOWN = 90.0


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
    length, pos = rd_varint(buf, pos)
    return buf[pos:pos + length].decode(), pos + length


# --- decoding ---------------------------------------------------------------

def read_spawn(body):
    """Add Entity: [varint eid][uuid 16][varint type][f64 x y z][LpVec3]
    [u8 pitch][u8 yaw][u8 head yaw][varint data]."""
    eid, pos = rd_varint(body, 0)
    uuid = body[pos:pos + 16]
    pos += 16
    etype, pos = rd_varint(body, pos)
    x, y, z = struct.unpack(">ddd", body[pos:pos + 24])
    pos += 24
    # LpVec3 (src/protocol/lp_vec.h): a single zero byte is the zero vector;
    # otherwise two marker bytes, a big-endian u32, and — when the marker's low
    # three bits say the scale did not fit — a scale varint on the end. A flat
    # six bytes mis-frames the pitch/yaw/data that follow it.
    first = body[pos]
    pos += 1 if first == 0 else 6
    if first & 0x4:
        _, pos = rd_varint(body, pos)
    pitch, yaw, head = body[pos], body[pos + 1], body[pos + 2]
    pos += 3
    data, pos = rd_varint(body, pos)
    assert pos == len(body), f"spawn_entity: {len(body) - pos} trailing byte(s)"
    return {"entity": eid, "uuid": uuid, "type": etype, "x": x, "y": y, "z": z,
            "pitch": pitch, "yaw": yaw, "head": head, "data": data}


def read_position_sync(body):
    """sync_entity_position (26.3): [varint eid][varint PositionPath type =
    LINEAR 0][f64 x y z][f32 yRot][f32 xRot][bool onGround]."""
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


def read_stack(body, pos=0):
    """The *trusted* (server-sent) ItemStack: [varint count][varint item]
    [varint added][varint removed] and then, per added entry, [varint id][value]
    — no length prefix, which is the whole difference from the delimited form a
    client sends. Only potion_contents is understood, so any other component is
    an assertion failure rather than something silently walked over."""
    count, pos = rd_varint(body, pos)
    if count == 0:
        return None, pos
    item, pos = rd_varint(body, pos)
    added, pos = rd_varint(body, pos)
    removed, pos = rd_varint(body, pos)
    assert removed == 0, f"a removed component is not modelled: {removed}"
    patch = {}
    for _ in range(added):
        cid, pos = rd_varint(body, pos)
        assert cid == COMPONENT_POTION_CONTENTS, f"unexpected component {cid}"
        contents, pos = read_potion_contents(body, pos)
        patch[cid] = contents
    return {"item": item, "count": count, "patch": patch}, pos


def read_effect_details(buf, pos):
    amplifier, pos = rd_varint(buf, pos)
    duration, pos = rd_varint(buf, pos)
    ambient, visible, show_icon = buf[pos], buf[pos + 1], buf[pos + 2]
    pos += 3
    has_hidden = buf[pos]
    pos += 1
    hidden = None
    if has_hidden:
        hidden, pos = read_effect_details(buf, pos)
    return {"amplifier": amplifier, "duration": duration, "ambient": ambient,
            "visible": visible, "show_icon": show_icon, "hidden": hidden}, pos


def read_potion_contents(buf, pos):
    """PotionContents.STREAM_CODEC: [bool][varint potion]? [bool][INT color]?
    [varint count][instances] [bool][utf8 name]? -> ((potion, effects), pos)."""
    potion = None
    if buf[pos]:
        potion, pos = rd_varint(buf, pos + 1)
    else:
        pos += 1
    has_color = buf[pos]
    pos += 1
    if has_color:
        pos += 4  # ByteBufCodecs.INT, fixed-width
    count, pos = rd_varint(buf, pos)
    effects = []
    for _ in range(count):
        effect, pos = rd_varint(buf, pos)
        details, pos = read_effect_details(buf, pos)
        effects.append((effect, details))
    has_name = buf[pos]
    pos += 1
    if has_name:
        _, pos = rd_string(buf, pos)
    return (potion, effects), pos


def read_entity_data(body, wanted=None):
    """[varint eid] then [u8 index][varint serializer][value]... to 0xFF, for the
    serializers the player, mob, item and cloud entities use. Returns None for
    another entity's packet when `wanted` filters by id."""
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
        if serializer == SER_BYTE:
            values[index] = body[pos]
            pos += 1
        elif serializer == 1:  # INT
            v, pos = rd_varint(body, pos)
            values[index] = v
        elif serializer == SER_FLOAT:
            values[index] = struct.unpack(">f", body[pos:pos + 4])[0]
            pos += 4
        elif serializer == SER_ITEM_STACK:
            stack, pos = read_stack(body, pos)
            values[index] = stack
        elif serializer == SER_BOOLEAN:
            values[index] = body[pos] != 0
            pos += 1
        elif serializer == SER_PARTICLE:
            particle, pos = rd_varint(body, pos)
            colour = struct.unpack(">I", body[pos:pos + 4])[0]
            pos += 4
            values[index] = (particle, colour)
        elif serializer == SER_PARTICLES:
            # `LivingEntity.DATA_EFFECT_PARTICLES` (index 10 on a living entity,
            # and a different value at the same index on the cloud above): a
            # varint count, then each entry's particle type id and that type's
            # own payload.
            count, pos = rd_varint(body, pos)
            particles = []
            for _ in range(count):
                ptype, pos = rd_varint(body, pos)
                colour = None
                if ptype == PARTICLE_ENTITY_EFFECT:
                    colour = struct.unpack(">I", body[pos:pos + 4])[0]
                    pos += 4
                particles.append((ptype, colour))
            values[index] = particles
        else:
            raise AssertionError(f"entity data index {index}: serializer {serializer}")
    assert pos == len(body), f"set_entity_data: {len(body) - pos} trailing byte(s)"
    return eid, values


def read_set_health(body):
    """set_health: [f32 health][varint food][f32 saturation]."""
    health = struct.unpack(">f", body[0:4])[0]
    food, pos = rd_varint(body, 4)
    saturation = struct.unpack(">f", body[pos:pos + 4])[0]
    assert pos + 4 == len(body), f"set_health: {len(body) - pos - 4} trailing byte(s)"
    return round(health, 4), food, round(saturation, 4)


def read_mob_effect(body):
    """update_mob_effect: [varint entity][varint effect][varint amplifier]
    [varint duration][u8 flags]."""
    pos = 0
    eid, pos = rd_varint(body, pos)
    effect, pos = rd_varint(body, pos)
    amplifier, pos = rd_varint(body, pos)
    duration, pos = rd_varint(body, pos)
    flags = body[pos]
    pos += 1
    assert pos == len(body), f"update_mob_effect: {len(body) - pos} trailing byte(s)"
    if duration == UNSIGNED_MAX:
        duration = -1
    return eid, effect, amplifier, duration, flags


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
    position = None
    if has_position:
        position = struct.unpack(">ddd", body[pos:pos + 24])
        pos += 24
    assert pos == len(body), f"damage_event: {len(body) - pos} trailing byte(s)"
    return {"entity": eid, "type": type_id, "cause": cause - 1, "direct": direct - 1,
            "position": position}


def read_hurt_animation(body):
    eid, pos = rd_varint(body, 0)
    yaw = struct.unpack(">f", body[pos:pos + 4])[0]
    assert pos + 4 == len(body), f"hurt_animation: {len(body) - pos - 4} trailing byte(s)"
    return eid, yaw


def read_level_event(body):
    """level_event: [i32 type][i64 packed position][i32 data][bool global]."""
    assert len(body) == 17, f"level_event body is {len(body)} bytes"
    event = struct.unpack(">i", body[0:4])[0]
    raw = struct.unpack(">q", body[4:12])[0]
    x = raw >> 38
    z = (raw >> 12) & 0x3FFFFFF
    y = raw & 0xFFF
    if z >= 0x2000000:
        z -= 0x4000000
    if y >= 0x800:
        y -= 0x1000
    data = struct.unpack(">i", body[12:16])[0]
    return event, (x, y, z), data, body[16]


def read_remove_entities(body):
    count, pos = rd_varint(body, 0)
    ids = []
    for _ in range(count):
        v, pos = rd_varint(body, pos)
        ids.append(v)
    assert pos == len(body), f"remove_entities: {len(body) - pos} trailing byte(s)"
    return ids


def decode_content(body):
    """container_set_content: (window, state, [stacks], cursor)."""
    window, pos = rd_varint(body, 0)
    state, pos = rd_varint(body, pos)
    count, pos = rd_varint(body, pos)
    slots = []
    for _ in range(count):
        stack, pos = read_stack(body, pos)
        slots.append(stack)
    cursor, pos = read_stack(body, pos)
    assert pos == len(body), f"container_set_content: {len(body) - pos} trailing byte(s)"
    return window, state, slots, cursor


def decode_set_slot(body):
    """container_set_slot: (window, state, slot, stack)."""
    window, pos = rd_varint(body, 0)
    state, pos = rd_varint(body, pos)
    slot = struct.unpack(">h", body[pos:pos + 2])[0]
    stack, pos = read_stack(body, pos + 2)
    assert pos == len(body), f"container_set_slot: {len(body) - pos} trailing byte(s)"
    return window, state, slot, stack


def potion_of(stack):
    assert stack is not None, "no stack to read contents from"
    assert COMPONENT_POTION_CONTENTS in stack["patch"], \
        f"the stack carries no potion_contents: {sorted(stack['patch'])}"
    return stack["patch"][COMPONENT_POTION_CONTENTS]


def stack_delta(lo, hi):
    """`AABB.distanceToSqr`: the squared gap between two boxes, 0 when they
    overlap. Each box is (min_x, min_y, min_z, max_x, max_y, max_z) with the
    position's y as its *bottom* (EntityDimensions.makeBoundingBox)."""
    dx = max(max(lo[0], hi[0]) - min(lo[3], hi[3]), 0.0)
    dy = max(max(lo[1], hi[1]) - min(lo[4], hi[4]), 0.0)
    dz = max(max(lo[2], hi[2]) - min(lo[5], hi[5]), 0.0)
    return dx * dx + dy * dy + dz * dz


def potion_box(x, y, z):
    return (x - POTION_HALF_WIDTH, y, z - POTION_HALF_WIDTH,
            x + POTION_HALF_WIDTH, y + POTION_HEIGHT, z + POTION_HALF_WIDTH)


def mob_box(x, y, z, half_width, height):
    return (x - half_width, y, z - half_width, x + half_width, y + height, z + half_width)


def splash_amount(impact, victim_box):
    """`ThrownSplashPotion.onHitAsPotion`'s falloff for one victim, as the health
    delta it lands: `(int)((1 - sqrt(distance) / 4) * 6 + 0.5)` for a harming
    potion on a target that is not in `minecraft:inverted_healing_and_harm`."""
    distance = math.sqrt(stack_delta(potion_box(*impact), victim_box))
    return int((1.0 - distance / SPLASH_RANGE) * 6.0 + 0.5)


# --- the packets the test sends ---------------------------------------------

def use_item(bot, pitch=PITCH_DOWN, yaw=0.0):
    """ServerboundUseItem: [varint hand][varint sequence][f32 yRot][f32 xRot]."""
    bot.send(SB_USE_ITEM, varint(0) + varint(0) + struct.pack(">ff", yaw, pitch))


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
            _, pos = rd_varint(body, 0)
            self.position = struct.unpack(">ddd", body[pos:pos + 24])
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
            if pid == 0x2D:
                self.send(SB_KEEP_ALIVE, body[:8])
            elif pid == 0x0B:
                batch, _ = rd_varint(body)
                self.send(0x0B, varint(batch))
                break
        return True


def drain(bot, timeout=1.2):
    """Every packet the reader thread queued in the next `timeout`
    seconds."""
    # A reply window ends when the server has been quiet for `quiet` seconds
    # rather than always burning the whole window; `timeout` stays the bound a
    # missing reply is judged against. Requires the reader (started after each
    # join below).
    return wire_client.drain_quiet(bot, quiet=0.3, limit=timeout)
def settle(bot, seconds):
    """Collect for `seconds`, sending one game tick between reads: the *world*
    tick (which flies the bottle and ticks the cloud) runs on the server's own
    50 ms clock, but the *session* clock — which is what drains a queued potion
    effect onto the player — only advances on a recognized serverbound packet."""
    packets = []
    end = time.time() + seconds
    while time.time() < end:
        bot.send(SB_FLYING, bytes([1]))
        packets += drain(bot, 0.12)
        time.sleep(TICK_MS)
    return packets


def all_of(packets, pid):
    return [b for p, b in packets if p == pid]


def chat_literals(packets):
    out = []
    for body in all_of(packets, CB_SYSTEM_CHAT):
        try:
            out.append(chat_text(body))
        except AssertionError:
            pass
    return out


def feedback(packets):
    return " | ".join(chat_literals(packets))


def send_chat_command(bot, command):
    bot.send(SB_CHAT_COMMAND, varint(len(command)) + command.encode())


START = time.time()


def step(text):
    print(f"[{time.time() - START:6.2f}s] {text}")


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


class Harness:
    def __init__(self, bot):
        self.bot = bot
        self.health = (20.0, 20, 5.0)
        self.state = None
        self.slots = None

    def observe(self, packets):
        for body in all_of(packets, CB_SET_HEALTH):
            self.health = read_set_health(body)
        for body in all_of(packets, CB_CONTAINER_SET_CONTENT):
            _, self.state, self.slots, _ = decode_content(body)
        for body in all_of(packets, CB_CONTAINER_SET_SLOT):
            window, state, slot, stack = decode_set_slot(body)
            self.state = state
            if window == 0 and self.slots is not None and slot < len(self.slots):
                self.slots[slot] = stack
        return packets

    def command(self, text, timeout=1.2):
        step(f"-> /{text}")
        send_chat_command(self.bot, text)
        return self.observe(drain(self.bot, timeout))

    def setblock(self, x, y, z, block):
        """`/setblock <x> <y> <z> <block>`, confirmed by its own feedback line.

        Step 5 builds its own ground and its own cell with this, because the
        generated surface is not a place to measure a distance on: its height
        changes from column to column, and the column the probe throw lands in is
        itself only as stable as the bottle's spread (`pos.x` draws within
        0.017 of the thrower, which is enough to pick either side of a block
        boundary). The confirmation is the command's own reply rather than the
        block update it broadcasts, so a cell that was *already* in the state
        being asked for is still a success.
        """
        packets = self.command(f"setblock {x} {y} {z} {block}", timeout=PLACE_TIMEOUT)
        assert "Changed the block at" in feedback(packets), feedback(packets)

    def select(self, slot):
        assert SLOT_MENU_HOTBAR_FIRST <= slot < SLOT_MENU_HOTBAR_FIRST + MENU_HOTBAR_SLOTS, \
            f"menu slot {slot} is outside the hotbar"
        self.bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", slot - SLOT_MENU_HOTBAR_FIRST))
        drain(self.bot, 0.2)

    def give_thrown(self, item, potion, count=1):
        """`/give` a thrown bottle whose contents name `potion`; returns the
        hotbar menu slot it landed in."""
        packets = self.command(
            f'give {self.bot.name} minecraft:{item}[potion_contents={{potion:"{potion}"}}] '
            f'{count}')
        assert "Gave " in feedback(packets), feedback(packets)
        contents = all_of(packets, CB_CONTAINER_SET_CONTENT)
        assert contents, "a /give must resync the inventory"
        _, _, self.slots, _ = decode_content(contents[-1])
        item_id = ITEMS_BY_THROWN_NAME[item]
        for i, stack in enumerate(self.slots):
            if stack is not None and stack["item"] == item_id:
                return i
        raise AssertionError(f"the potion is nowhere in the menu: {self.slots}")

    def throw(self, slot, seconds=1.8, pitch=PITCH_DOWN):
        """Select `slot`, throw it, and collect the flight."""
        self.select(slot)
        use_item(self.bot, pitch=pitch)
        return self.observe(settle(self.bot, seconds))


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-thrown-potion-")
    # Chunk (-2, -1) of this fixture is real terrain — stone under a grass
    # surface — which is what the thrown bottles break on.
    shutil.copytree(os.path.join(os.path.dirname(__file__), "fixtures", "ow3region"),
                    os.path.join(world, "region"))
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    # Natural spawning off: every geometric claim in this file is exact — the
    # face a bottle breaks on, the block the thrower stands on, the falloff
    # that lands on their own feet — and the one thing that can move any of
    # them is a mob wandering into the fall: its box top stops the bottle a
    # block above the ground (a cow's is 1.4 up) and its own body takes a slice
    # of the splash. The fixture carries no mob save, so with the pass off the
    # only entities a run can meet are the ones it summons itself (the celled
    # pig in step 5).
    env["PLUSMC_MOB_SPAWNING"] = "0"
    # The bottle's flight, its break and the cloud's own re-apply interval are
    # the *world* clock (compressed by PLUSMC_TICK_SCALE); the durations asserted
    # below are the values the cloud wrote into update_mob_effect, not countdowns
    # this test watches. The survival tick is pinned to packets — one tick each,
    # no wall-time catch-up — so nothing here can come to depend on wall time.
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"

    proc = None
    try:
        step(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", "127.0.0.1", "--port", "0", "--motd", "thrown potion test",
             "--offline-mode", "--gamemode", "0", "--data", data_root, "--world", world,
             "--no-flat-world"], wait_listening, env=env)

        bot = Bot("TossBot", port)
        wire_client.record(bot, note="thrown-potion-wire: the throw, the break, the cloud")
        bot.join()
        bot.start_reader()
        h = Harness(bot)
        join_packets = h.observe(drain(bot, 1.2))
        attrs = all_of(join_packets, CB_UPDATE_ATTRIBUTES)
        assert attrs, "the join must push the attribute set (which carries the entity id)"
        bot.entity_id, _ = rd_varint(attrs[-1], 0)

        h.command("tp -24 100 -8")
        assert bot.position == (-24.0, 100.0, -8.0), bot.position
        feet = bot.position
        feet_block = (math.floor(feet[0]), math.floor(feet[1]), math.floor(feet[2]))
        step(f"  OK standing at {feet} in block {feet_block}")

        # --- 1. the bottle the server hands out ------------------------------
        slot = h.give_thrown("splash_potion", "minecraft:harming")
        stack = h.slots[slot]
        potion, custom = potion_of(stack)
        assert stack["item"] == ITEM_SPLASH_POTION and stack["count"] == 1, stack
        assert potion == POTION_HARMING and custom == [], (potion, custom)
        assert slot == SLOT_MENU_HOTBAR_FIRST, (slot, h.slots)
        step(f"  OK the menu stack is a splash potion carrying potion {potion}")

        # --- 2. a throw is a spawn, not a drink ------------------------------
        # The fixture floor is roughly 37 blocks below the player's eyes; at
        # vanilla projectile gravity, two seconds only captures the first part
        # of the arc. Wait for the deterministic impact rather than asserting
        # against a wall-clock window that can end mid-flight.
        packets = h.throw(slot, seconds=5.0)
        spawns = [read_spawn(b) for b in all_of(packets, CB_SPAWN_ENTITY)]
        thrown = next((s for s in spawns if s["type"] == ET_SPLASH_POTION), None)
        assert thrown is not None, f"no splash potion spawn: {[s['type'] for s in spawns]}"
        # `ThrowableItemProjectile`'s constructor puts it at the thrower's own x/z
        # and `getEyeY() - 0.1f`, and the throw's aim has no horizontal component
        # at pitch 90 (the -20 degree offset only tilts the vertical).
        assert abs(thrown["x"] - feet[0]) < 1e-6 and abs(thrown["z"] - feet[2]) < 1e-6, thrown
        assert abs(thrown["y"] - (feet[1] + 1.62 - 0.1)) < 1e-6, thrown
        step(f"  OK Add Entity: entity {thrown['entity']} type {thrown['type']} at "
             f"({thrown['x']:.2f}, {thrown['y']:.2f}, {thrown['z']:.2f})")

        # The synched stack is the *bottle*, components and all: it is what the
        # client tints.
        item_stacks = []
        for body in all_of(packets, CB_SET_ENTITY_DATA):
            read = read_entity_data(body, thrown["entity"])
            if read is None:
                continue
            _, values = read
            if THROWABLE_ITEM_STACK_INDEX in values:
                item_stacks.append(values[THROWABLE_ITEM_STACK_INDEX])
        assert item_stacks, "no throwable item data on the potion entity"
        assert item_stacks[-1]["item"] == ITEM_SPLASH_POTION, item_stacks[-1]
        assert item_stacks[-1]["count"] == 1, item_stacks[-1]
        assert potion_of(item_stacks[-1])[0] == POTION_HARMING, item_stacks[-1]
        step("  OK Set Entity Data carries the thrown bottle with its potion_contents")

        # The throw consumed the stack: `itemStack.consume(1, player)`.
        sets = all_of(packets, CB_CONTAINER_SET_SLOT)
        assert sets, "the thrown slot must resync"
        window, _, used_slot, after = decode_set_slot(sets[0])
        assert window == 0 and used_slot == slot, (window, used_slot, slot)
        assert after is None, f"the bottle must be consumed: {after}"
        step("  OK the thrown slot is empty")

        # The flight: position syncs on the potion's own entity id, downward.
        syncs = [read_position_sync(b) for b in all_of(packets, CB_ENTITY_POSITION_SYNC)
                 if rd_varint(b, 0)[0] == thrown["entity"]]
        assert len(syncs) >= 2, f"the bottle must fly: {len(syncs)} sync(s)"
        assert syncs[0]["y"] < thrown["y"], syncs[0]
        assert all(s["y"] <= syncs[0]["y"] + 1e-9 for s in syncs), [s["y"] for s in syncs]
        step(f"  OK flew through {len(syncs)} position syncs, {thrown['y']:.2f} -> "
             f"{syncs[-1]['y']:.2f}")

        # The break: `level.levelEvent(2007, blockPosition, color)` — 2007 because
        # a harming potion has an instantaneous effect — and `discard()`. This
        # throw doubles as the test's probe for where the fixture's ground is:
        # the bottle falls the whole way from the eyes and stops on the first
        # block below it.
        events = [read_level_event(b) for b in all_of(packets, CB_LEVEL_EVENT)]
        assert events, "the break must raise a level event"
        event = events[-1]
        assert event[0] == 2007, event
        assert event[1][1] < feet_block[1], (event[1], feet_block)
        assert event[3] == 0, event
        removals = [read_remove_entities(b) for b in all_of(packets, CB_REMOVE_ENTITIES)]
        assert any(thrown["entity"] in r for r in removals), removals
        ground = event[1]
        step(f"  OK broke at {ground} (level event 2007) and despawned")

        # --- 2b. stand on the block the bottle broke on -----------------------
        # `blockPosition()` is the block the potion's own position was in, which
        # for a bottle stopped by a face is the *air* block whose floor is the
        # block below — so teleporting to that y puts the thrower exactly on top
        # of it. From there every later throw is exact: it breaks on the same
        # face, the impact point is the thrower's own feet (distance zero, so
        # the falloff is 1.0), and the fractional x/z put the uncertainty
        # triangle — one draw per axis, `0.0172275 * (a - b)`, under 0.05 even
        # over a long fall — nowhere near a block boundary.
        stand = (ground[0] + 0.5, float(ground[1]), ground[2] + 0.5)
        h.command(f"tp {stand[0]} {stand[1]} {stand[2]}")
        assert bot.position == stand, (bot.position, stand)
        feet = bot.position
        feet_block = (ground[0], ground[1], ground[2])
        impact_y = float(ground[1])
        step(f"  OK standing on it at {feet}")

        # --- 3. the effects land on the thrower ------------------------------
        # The thrower is a valid *area* victim — `ThrownSplashPotion.onHitAsPotion`
        # scans every living entity in the box, the thrower included. Only a
        # *direct* hit on them is what `Projectile.leftOwner` gates, and the
        # trajectory never touches them (they are inside their own collision
        # range the whole way down) — and with natural spawning off (see `env`
        # above) the thrower is the only entity near the fall, so the break
        # below is the ground's own face and not another box's top.
        slot = h.give_thrown("splash_potion", "minecraft:harming")
        packets = h.throw(slot, seconds=2.0)
        events = [read_level_event(b) for b in all_of(packets, CB_LEVEL_EVENT)]
        assert events, "the throw must break on the ground below"
        event = events[-1]
        assert event[0] == 2007, event
        assert event[1] == feet_block, (event[1], feet_block)
        step(f"  OK broke at {event[1]}, the thrower's own block")
        damages = [read_damage_event(b) for b in all_of(packets, CB_DAMAGE_EVENT)]
        assert damages, "the splash must damage the player"
        hit = damages[-1]
        assert hit["entity"] == bot.entity_id, (hit, bot.entity_id)
        assert hit["type"] == DMG_INDIRECT_MAGIC, hit
        # `damageSources().indirectMagic(source, owner)`: the *thrower* is the
        # causing entity, which is what the death message and the loot gate read.
        assert hit["cause"] == bot.entity_id, (hit, bot.entity_id)
        # The potion's box overlaps the victim's (the impact point is at their
        # own feet), so the distance is zero and the falloff is 1.0: `6 << 0`.
        # The *first* health after the hit is the one to read: a full food bar
        # with saturation regenerates one point per 10 ticks, so a later packet
        # would already be healing it back.
        damage_at = max(i for i, (pid, _) in enumerate(packets) if pid == CB_DAMAGE_EVENT)
        healths = [read_set_health(b) for i, (pid, b) in enumerate(packets)
                   if pid == CB_SET_HEALTH and i > damage_at]
        assert healths, "the damage must sync the health bar"
        assert healths[0][0] == 14.0, f"20 - 6 = 14: {healths}"
        step(f"  OK splashed for 6 (indirect_magic, caused by the thrower): 20.0 -> "
             f"{healths[0][0]}")

        # --- 4. a lingering potion leaves a cloud that keeps ticking -----------
        # *This* step comes before the mob one on purpose, and now for two
        # reasons. Its claim is an exact position — the cloud is spawned where the
        # bottle stopped — and a wandering passive mob breaks that claim: a
        # 29-servers-at-once load run had the pig summoned below walk under the
        # falling bottle, whose entity clip then stopped it on the pig's own box
        # top (feet + Pig's 0.9 height, where that run's cloud sat). And the
        # cloud's own radius is 3.0 against the cell's 2.15 blocks, so a pig
        # already in the cell would be granted the swiftness this step reads off
        # the thrower, mixing a second entity's rows into the assertion.
        slot = h.give_thrown("lingering_potion", "minecraft:swiftness")
        packets = h.throw(slot, seconds=3.5)
        clouds = [s for s in (read_spawn(b) for b in all_of(packets, CB_SPAWN_ENTITY))
                  if s["type"] == ET_AREA_EFFECT_CLOUD]
        assert clouds, "no area effect cloud was spawned"
        cloud = clouds[0]
        # `new AreaEffectCloud(level, this.getX(), this.getY(), this.getZ())`:
        # the bottle's own position, i.e. the face it broke on.
        assert abs(cloud["y"] - impact_y) < 1e-6, (cloud, impact_y)
        step(f"  OK the lingering potion left a cloud (entity {cloud['entity']}) at "
             f"({cloud['x']:.2f}, {cloud['y']:.2f}, {cloud['z']:.2f})")

        cloud_values = []
        for body in all_of(packets, CB_SET_ENTITY_DATA):
            read = read_entity_data(body)
            if read is None:
                continue
            eid, values = read
            if eid == cloud["entity"]:
                cloud_values.append(values)
        assert cloud_values, "the cloud must carry synched data"
        first = cloud_values[0]
        assert first[CLOUD_RADIUS_INDEX] == 3.0, first
        assert first[CLOUD_WAITING_INDEX] is False, first
        assert CLOUD_PARTICLE_INDEX in first, first
        # `setRadius(radiusPerTick)` on every non-waiting tick, so the later
        # updates report a strictly smaller radius until MINIMAL_RADIUS discards
        # the cloud.
        radii = [v[CLOUD_RADIUS_INDEX] for v in cloud_values if CLOUD_RADIUS_INDEX in v]
        assert len(radii) >= 2, radii
        assert radii[-1] < radii[0], radii
        step(f"  OK cloud radius {radii[0]:.3f} -> {radii[-1]:.3f} over {len(radii)} updates")

        # The cloud applies the effects itself, at the lingering potion's own
        # minecraft:potion_duration_scale of 0.25: 3600 * 0.25 = 900.
        effects = [read_mob_effect(b) for b in all_of(packets, CB_UPDATE_MOB_EFFECT)]
        speed = [e for e in effects if e[1] == EFFECT_SPEED]
        assert speed, effects
        # The first one *lands* (`ServerPlayer.onEffectAdded`, so the blend flag
        # is set); the cloud re-applies every 20 ticks while the player stands in
        # it, and each of those is a merge onto an instance that has since ticked
        # down — a longer duration wins, which is an *update* with no blend
        # (`MobEffectInstance.update` -> `onEffectUpdated`).
        assert speed[0] == (bot.entity_id, EFFECT_SPEED, 0, 900,
                            FLAG_VISIBLE | FLAG_SHOW_ICON | FLAG_BLEND), speed
        for repeat in speed[1:]:
            assert repeat == (bot.entity_id, EFFECT_SPEED, 0, 900,
                              FLAG_VISIBLE | FLAG_SHOW_ICON), speed
        step(f"  OK the cloud granted speed for {speed[0][3]} ticks (3600 * 0.25), "
             f"re-applied {len(speed) - 1} time(s)")

        # --- 5. a mob is a victim too, and at a distance ----------------------
        # `/summon` puts its mob two blocks out in +x, at the summoner's own y,
        # and this step is about the *distance* — so both ends of the throw are
        # built here rather than borrowed from the terrain.
        #
        # Why a cell: a pig is free to walk, and in the seconds a `/give` + throw
        # take it crosses the whole falloff curve. A 29-servers-at-once load run
        # had it stroll under the falling bottle and take the full six, which is
        # exactly what a *range* assertion cannot tell from a falloff bug. The
        # cell — a floor, a ceiling (a ground mob jumps a full block: see
        # `pathfinding.cpp`'s step-up) and four walls — fixes the mob's box to
        # the block, so the amount becomes a number rather than a sample.
        #
        # The geometry, in the probe's own frame: the thrower's block is
        # (gx, gy, gz), the cell is the ring's landing spot (gx + 2, gy, gz), and
        # the throw comes from `THROW_OFFSET_X` into the cell next door, which is
        # the 2.15 blocks the module's constant explains.
        gx, gy, gz = ground
        cell = (gx + 2, gy, gz)
        throw_x = gx + THROW_OFFSET_X
        step(f"-> a cell at {cell} for the pig, and a stand at ({throw_x}, {gy}, {gz})")
        for x, y, z, block in (
                # The thrower's own footing (the neighbouring column is not
                # necessarily at this height) and the space they stand in.
                (gx - 1, gy - 1, gz, "stone"),
                (gx - 1, gy, gz, "air"), (gx - 1, gy + 1, gz, "air"),
                # The cell: floor, interior, ceiling, and a wall on each side.
                (cell[0], gy - 1, cell[2], "stone"),
                (cell[0], gy, cell[2], "air"),
                (cell[0], gy + 1, cell[2], "stone"),
                (cell[0] + 1, gy, cell[2], "stone"),
                (cell[0] - 1, gy, cell[2], "stone"),
                (cell[0], gy, cell[2] + 1, "stone"),
                (cell[0], gy, cell[2] - 1, "stone")):
            h.setblock(x, y, z, block)
        step("  OK the cell and the stand are the server's own blocks")

        # Back onto the summoning spot before the summon: step 3's splash left
        # the player where the hit pushed them, and the ring is measured from
        # where the *server* last saw the player, not from where this test last
        # put them.
        stand = (gx + 0.5, float(gy), gz + 0.5)
        h.command(f"tp {stand[0]} {stand[1]} {stand[2]}")
        assert bot.position == stand, (bot.position, stand)
        spawns = [read_spawn(b) for b in all_of(h.command("summon minecraft:pig 1"),
                                               CB_SPAWN_ENTITY)]
        pig = next((s for s in spawns if s["type"] == ET_PIG), None)
        assert pig is not None, f"no pig spawn: {[s['type'] for s in spawns]}"
        # The fixture is only worth its commands if the ring really lands the mob
        # *in* the cell, so the spawn packet is checked against the cell rather
        # than the other way round.
        assert (abs(pig["x"] - (cell[0] + 0.5)) < 1e-6 and abs(pig["y"] - gy) < 1e-6
                and abs(pig["z"] - (cell[2] + 0.5)) < 1e-6), (
            f"the ring must land the pig at the cell's own centre "
            f"({cell[0] + 0.5}, {gy}, {cell[2] + 0.5}), not "
            f"({pig['x']:.3f}, {pig['y']:.3f}, {pig['z']:.3f})")
        step(f"  OK the ring landed the pig (entity {pig['entity']}) in the cell, "
             f"at ({pig['x']:.2f}, {pig['y']:.2f}, {pig['z']:.2f})")

        h.command(f"tp {throw_x} {gy} {gz + 0.5}")
        assert bot.position == (throw_x, float(gy), gz + 0.5), bot.position
        slot = h.give_thrown("splash_potion", "minecraft:harming")
        packets = h.throw(slot, seconds=2.0)
        hurt = [read_hurt_animation(b) for b in all_of(packets, CB_HURT_ANIMATION)]
        mob_healths = []
        for body in all_of(packets, CB_SET_ENTITY_DATA):
            read = read_entity_data(body)
            if read is None:
                continue
            eid, values = read
            if LIVING_HEALTH_INDEX in values:
                mob_healths.append((eid, values[LIVING_HEALTH_INDEX]))
        pig_hits = [(eid, hp) for eid, hp in mob_healths if eid == pig["entity"]]
        assert pig_hits, f"the pig must take splash damage: {mob_healths}"
        pig_health = pig_hits[-1][1]
        assert pig["entity"] in [eid for eid, _ in hurt], (hurt, mob_healths)
        # What the falloff owes at the fixture's own geometry, rather than at
        # whatever position the wire last named for the pig: a moving mob's syncs
        # are rate-limited (vanilla's `ServerEntity` sends one at most every other
        # tick), which is the half of this the cell does not fix — so the amount
        # is read against the cell and the thrower, both of which are blocks.
        expected = splash_amount((throw_x, float(gy), gz + 0.5),
                                 mob_box(cell[0] + 0.5, float(gy), cell[2] + 0.5,
                                         PIG_HALF_WIDTH, PIG_HEIGHT))
        pig_damage = 10.0 - pig_health
        assert pig_damage == expected, (
            f"a pig two blocks out must take the falloff's own {expected}: it took "
            f"{pig_damage} (health {pig_health}, {mob_healths})")
        step(f"  OK the pig (entity {pig['entity']}) took {pig_damage:g} at the "
             f"fixture's 2.15 blocks, against the thrower's full 6 at zero — so "
             f"health {pig_health} and a hurt animation")

        # --- 6. a creative throw does not consume the bottle -------------------
        h.command("gamemode creative TossBot")
        slot = h.give_thrown("splash_potion", "minecraft:swiftness")
        packets = h.throw(slot, seconds=0.8)
        assert all_of(packets, CB_SPAWN_ENTITY), "a creative throw still throws"
        for body in all_of(packets, CB_CONTAINER_SET_SLOT):
            _, _, changed, stack = decode_set_slot(body)
            assert not (changed == slot and stack is None), \
                "`ItemStack.consume` is a no-op for infinite materials"
        step("  OK the creative throw left the bottle in hand")

        bot.close()
        print("thrown_potion: all steps passed")
        return 0
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(world, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
