#!/usr/bin/env python3
"""Integration test: arrows and thrown items over the wire (Phase 4 milestone H).

`BowItem.releaseUsing`, and `SnowballItem.use` and its two siblings, end to end
against the real server. All of the *trajectory* is world/projectile.{h,cpp} —
pure, unit-tested rules — so what this test adds is the part a unit test cannot
see: that the numbers the session puts on the wire are the numbers the module
computed, in the order vanilla integrates them.

  * the **charge curve**: `use_item` starts a draw (`BowItem.use`), the draw
    advances on the session's packet clock (`LivingEntity.updateUsingItem`), and
    a RELEASE_USE_ITEM action ends it. A two-tick draw is below
    `getPowerForTime`'s 0.1 gate and fires *nothing at all*; three ticks fires at
    `0.1075 * 3.0`; twenty fires at the full 3.0 with the critical flag set.
    Both ends of the quadratic are checked against its closed form, so a curve
    that is merely monotonic fails;
  * the **arrow's tick order**: `AbstractArrow.tick` moves through
    `stepMoveAndHit` *before* it damps the velocity, so the first position sync
    is exactly one launch-velocity step from the spawn point (not a hair less),
    and every later one is the previous sync's velocity away. That velocity is
    then dragged by 0.99 and charged 0.05 of gravity — the *opposite* order from
    a throwable's, which is what makes the pair of assertions load-bearing;
  * the **spawn point**: the firer's own x/z and `getEyeY() - 0.1f`, which is the
    *bottom* of the arrow's box;
  * the **arrow's damage**: a full draw's 3.0 is `ceil(3.0 * baseDamage)` = 6
    against an unarmoured mob, seen as the mob's health on the wire;
  * the three **throwables**: each spawns at its own entity type with the item
    throw's fixed 1.5 power — a bow's 3.0 is `BowItem`'s multiplier, not the
    projectile's — and a landed ender pearl charges its thrower exactly 5.0 of
    `minecraft:ender_pearl`;
  * the **ammunition's own entity type**: the arrow that flies is the one the
    *item* creates (`SpectralArrowItem.createArrow` vs `ArrowItem.createArrow`),
    so a spectral arrow fires entity type 123 and an ordinary one fires 6;
  * the **in-ground datum's two edges**: `IN_GROUND` is synched data, so an arrow
    the world frees again (`startFalling`, when the cell it is stuck in stops
    colliding) is a Set Entity Data packet with the flag *false* — the clear a
    server that only ever sent the bury would never send, and one no
    position-watching assertion can see;
  * the **LpVec3 framing**: the six bytes of a non-zero LpVec3 are two markers
    and a big-endian u32, plus a scale varint when the marker's low three bits
    say the scale did not fit in them. A reader that skips a flat six bytes would
    pass everything this server sends today (a bow's ceiling is exactly 3.0, so
    the scale is at most 3) and silently mis-frame the first vector that is not —
    so the decoder is pinned here against a hand-built scale-5 vector, which is a
    check that can fail.

Run: projectile_wire_test.py /path/to/plusmc
"""
import math, os, re, shutil, socket, struct, sys, tempfile, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nbt_component import chat_text  # noqa: E402
import wire_client
from wire_client import make_frame, read_packet

PROTO = 777
HOST = "127.0.0.1"

# minecraft:entity_type protocol ids (src/protocol/ids.h).
ET_ARROW = 6
ET_EGG = 40
ET_ENDER_PEARL = 45
ET_SNOWBALL = 123
ET_SPECTRAL_ARROW = 126
ET_ZOMBIE = 154

# Item ids (src/world/item_registry.h).
ITEMS_BY_NAME = {"bow": 1008, "arrow": 1009, "snowball": 1132, "egg": 1148,
                 "ender_pearl": 1265, "spectral_arrow": 1443}

# Clientbound packet ids.
CB_SPAWN_ENTITY = 0x01
CB_CONTAINER_SET_CONTENT = 0x12
CB_CONTAINER_SET_SLOT = 0x14
CB_DAMAGE_EVENT = 0x19
CB_ENTITY_POSITION_SYNC = 0x23
CB_HURT_ANIMATION = 0x2B
CB_KEEP_ALIVE = 0x2D
CB_REMOVE_ENTITIES = 0x4E
CB_PLAYER_POSITION = 0x49
CB_SET_ENTITY_DATA = 0x65
CB_SET_HEALTH = 0x6A
CB_SYSTEM_CHAT = 0x7C
CB_UPDATE_ATTRIBUTES = 0x86
CB_UPDATE_MOB_EFFECT = 0x87

# Serverbound packet ids (src/protocol/packet_table.cpp). Note 0x24 is
# `pick_item_from_block` and 0x29 is the packet whose class is
# ServerboundPlayerActionPacket — the table's canonical name for it is
# `block_dig`, and it is the one that carries RELEASE_USE_ITEM.
SB_CHAT_COMMAND = 0x07
SB_KEEP_ALIVE = 0x1C
# `move_player_pos`: a client's own claim of where it stands — the one way a
# second player can be put somewhere, and one tick of that player's own clock.
SB_POSITION = 0x1E
SB_FLYING = 0x21
SB_PLAYER_ACTION = 0x29
SB_SET_CARRIED_ITEM = 0x36
SB_SET_CREATIVE_SLOT = 0x39
SB_USE_ITEM = 0x43

# `ServerboundPlayerActionPacket.Action.RELEASE_USE_ITEM`.
ACTION_RELEASE_USE_ITEM = 5

# Entity data indices and serializers (src/server/entity_packets.h).
ARROW_FLAGS_INDEX = 8
ARROW_IN_GROUND_INDEX = 10
# `Entity.DATA_SHARED_FLAGS_ID` is the *first* accessor an Entity defines, so its
# index is 0, and the two bits that matter for a mob are `Entity.FLAG_ONFIRE`
# (bit 0) and `Entity.FLAG_GLOWING` (bit 6, the outline `updateGlowingStatus`
# raises from a Glowing effect). They share one byte, so a packet that carries
# one and assumes the other clear erases it.
SHARED_FLAGS_INDEX = 0
FLAG_ON_FIRE = 0x01
FLAG_GLOWING = 0x40
ARROW_FLAG_CRIT = 0x01
LIVING_HEALTH_INDEX = 9
SER_BYTE = 0
SER_FLOAT = 3
SER_BOOLEAN = 8
SER_PARTICLES = 17
# minecraft:entity_effect: the one particle type an effect's particle list
# carries a payload for (the ARGB colour). A spectral arrow's Glowing lands an
# effect on the player it hits, and the list is how every client draws it.
PARTICLE_ENTITY_EFFECT = 28

# BowItem / AbstractArrow / ThrowableItemProjectile constants
# (src/world/projectile.h).
BOW_VELOCITY_MULTIPLIER = 3.0
ARROW_GRAVITY = 0.05
ARROW_AIR_DRAG = 0.99
THROWABLE_SHOOT_POWER = 1.5
EYE_HEIGHT = 1.62
SPAWN_EYE_OFFSET = 0.1

# `Projectile.getMovementToShoot` perturbs the aim: `normalize(aim).add(
# triangle(0.0, 0.0172275 * uncertainty), ...)` per axis, and
# `RandomSource.triangle(mean, spread)` is `mean + spread * (nextDouble() -
# nextDouble())`. `BowItem.releaseUsing` shoots with `uncertainty = 1.0f` and so
# do all three throwables, so every launch deviates from its aim by strictly
# less than this on each axis — and by how much is the server's own RNG.
PROJECTILE_SPREAD = 0.0172275

# minecraft:damage_type ids: the index of the row in
# `src/world/damage_table.h`'s `kDamageTypes[]`, which is the registry id the
# damage event carries. The array is alphabetical, and the three already pinned
# elsewhere in the suite cross-check it (`fall` is 10, the one
# `apply_fall_damage` sends).
DMG_ARROW = 0
DMG_ENDER_PEARL = 8

# The effect registry's `minecraft:glowing` row (src/world/effect_table.h) and
# `ClientboundUpdateMobEffectPacket`'s blend bit (src/protocol/ids.h), which
# `ServerPlayer.onEffectAdded` sets for an effect a client has not seen.
EFFECT_GLOWING = 23
FLAG_BLEND = 8

# Menu slots: a fresh session has hotbar 0 selected, which /give fills first
# (`PlayerInventory` is 9 crafting/armour slots then 27 main then 9 hotbar).
SLOT_MENU_HOTBAR_FIRST = 36
MENU_HOTBAR_SLOTS = 9

FULL_HEALTH = 20.0
ENDER_PEARL_DAMAGE = 5.0
ZOMBIE_MAX_HEALTH = 20.0

TICK_MS = 0.055


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

def read_lp_vec3(body, pos):
    """LpVec3 (src/protocol/lp_vec.h), decoded the way the encoder packs it.

    The writer is `u8(buffer) | u8(buffer >> 8) | u32(buffer >> 16)`, so the four
    bytes after the first two are the buffer's high half, big-endian. Reading all
    six as a single little-endian integer moves the u32's middle two bytes to its
    ends and scrambles every component into a different plausible vector. A scale
    that does not fit the marker's low three bits adds a varint after the six,
    which is why this decodes the scale rather than skipping a fixed width.
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


def read_position_sync(body):
    """sync_entity_position (26.3): [varint eid][varint PositionPath type =
    LINEAR 0][f64 x y z][f32 yRot][f32 xRot][bool onGround]. 26.3 no longer
    carries delta movement; the position steps are the velocity sequence."""
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


def read_stack(buf, pos):
    """The trusted (server-sent) ItemStack: [varint count][varint item][varint
    added][varint removed]. The items this test gives out carry no components, so
    any added entry is an assertion failure rather than something walked over."""
    count, pos = rd_varint(buf, pos)
    if count == 0:
        return None, pos
    item, pos = rd_varint(buf, pos)
    added, pos = rd_varint(buf, pos)
    removed, pos = rd_varint(buf, pos)
    assert added == 0, f"unexpected component count {added}"
    assert removed == 0, f"unexpected removed count {removed}"
    return {"item": item, "count": count}, pos


def read_entity_data(body, wanted=None):
    """[varint eid] then [u8 index][varint serializer][value]... to 0xFF.

    Only the serializers a bow's arrow and a mob's health use are understood; an
    index this does not know is a failure rather than a silent skip.
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
        if serializer == SER_BYTE:
            values[index] = body[pos]
            pos += 1
        elif serializer == 1:  # INT
            values[index], pos = rd_varint(body, pos)
        elif serializer == SER_FLOAT:
            values[index] = struct.unpack(">f", body[pos:pos + 4])[0]
            pos += 4
        elif serializer == SER_BOOLEAN:
            values[index] = body[pos] != 0
            pos += 1
        elif serializer == SER_PARTICLES:
            # `LivingEntity.DATA_EFFECT_PARTICLES` (index 10): a varint count,
            # then each entry's particle type id and that type's own payload.
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
    return (health, food, saturation)


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


def read_mob_effect(body):
    """update_mob_effect: [varint entity][varint effect][varint amplifier]
    [varint duration][u8 flags], and nothing else. Only the effect's own owner is
    ever sent this (`ServerPlayer.onEffectAdded`/`onEffectUpdated`), which is why
    a mob's effects can only be read off the shared-flags byte instead."""
    pos = 0
    eid, pos = rd_varint(body, pos)
    effect, pos = rd_varint(body, pos)
    amplifier, pos = rd_varint(body, pos)
    duration, pos = rd_varint(body, pos)
    flags = body[pos]
    pos += 1
    assert pos == len(body), f"update_mob_effect: {len(body) - pos} trailing byte(s)"
    return eid, effect, amplifier, duration, flags


def read_remove_entities(body):
    eid, pos = rd_varint(body, 0)
    ids = [eid]
    while pos < len(body):
        more, pos = rd_varint(body, pos)
        ids.append(more)
    return ids


def decode_content(body):
    """container_set_content: (window, state, [stacks], cursor). The stacks are
    positional, so the list index *is* the menu slot."""
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


# --- the closed forms -------------------------------------------------------

def bow_power_for_time(ticks):
    """`BowItem.getPowerForTime`: `pow = ticks / 20`, `(pow^2 + 2 pow) / 3`, min 1."""
    pow_ = ticks / 20.0
    pow_ = (pow_ * pow_ + pow_ * 2.0) / 3.0
    return min(pow_, 1.0)


def charge_velocity(ticks):
    """The launch speed a draw of `ticks` produces: the power times BowItem's 3."""
    return bow_power_for_time(ticks) * BOW_VELOCITY_MULTIPLIER


def after_zombie_armor(damage):
    """`CombatRules.getDamageAfterAbsorb(damage, armor, toughness)` for a
    zombie's own two armour points and no toughness — the step `Mob.actuallyHurt`
    puts every hit through, and the reason the health on the wire is not the
    integer the arrow computed.

    The effective armour is `clamp(armor - damage / (2 + toughness / 4), armor *
    0.2, 20)`, which at two points and six-to-eleven damage clamps to the 0.4
    floor, so the fraction is a flat 0.4 / 25.
    """
    toughness_term = 2.0 + 0.0 / 4.0
    real_armor = min(max(2.0 - damage / toughness_term, 2.0 * 0.2), 20.0)
    return damage * (1.0 - real_armor / 25.0)


def assert_shot(velocity, power, aim, label):
    """`Projectile.getMovementToShoot`'s arithmetic, component by component.

    Every launch is `(aim + T) * power` where `T` is one
    `RandomSource.triangle(0.0, 0.0172275 * uncertainty)` deviation, and the bow
    and the three throwables all pass `uncertainty = 1.0f`. `T` is
    `spread * (nextDouble() - nextDouble())`, so the server's own RNG picks a
    value strictly inside ±`PROJECTILE_SPREAD` and the wire cannot name it — a
    single launch carries no way to solve for the draws.

    What the wire *can* pin, and what this asserts, is the line underneath: the
    aim, the power, and the bound on the deviation, per component. A launch with
    the wrong power, the wrong aim or the wrong uncertainty constant leaves this
    band, and the band is vanilla's own number rather than a tolerance widened
    until the packet fits.
    """
    assert len(velocity) == 3 and len(aim) == 3, (velocity, aim)
    for axis, (got, want) in enumerate(zip(velocity, aim)):
        deviation = got / power - want
        assert abs(deviation) < PROJECTILE_SPREAD, (
            f"{label}: launch component {axis} is {got:.10f}, which is "
            f"{deviation:.10f} off the aim {want} at power {power:.7f}; a "
            f"triangle deviation is strictly under {PROJECTILE_SPREAD}")


# --- transport --------------------------------------------------------------

def use_item(bot, yaw=0.0, pitch=0.0):
    """ServerboundUseItem: [varint hand][varint sequence][f32 yRot][f32 xRot].

    The session snaps the player to these angles before the item runs — vanilla's
    `handleUseItem` does the same — which is how a bow's release gets an aim at
    all, since a RELEASE_USE_ITEM packet carries none.
    """
    bot.send(SB_USE_ITEM, varint(0) + varint(0) + struct.pack(">ff", yaw, pitch))


def release_use_item(bot):
    """ServerboundPlayerActionPacket: [varint action][i64 pos][u8 face][varint
    sequence]. RELEASE_USE_ITEM is action 5; the position and face are ignored
    for it, exactly as vanilla ignores them."""
    body = varint(ACTION_RELEASE_USE_ITEM) + struct.pack(">q", 0) + bytes([1]) + varint(0)
    bot.send(SB_PLAYER_ACTION, body)


class Bot:
    def __init__(self, name, port):
        self.name = name
        self.sock = wire_client.transport(HOST, port, timeout=10)
        self.sock.settimeout(10)
        self.threshold = -1
        self.entity_id = None
        self.position = None
        # What the join read after the login handshake, kept rather than dropped:
        # the attribute set that carries this client's entity id arrives there.
        self.join_packets = []

    def send(self, pid, body=b""):
        self.sock.sendall(make_frame(varint(pid) + body, self.threshold))

    def recv(self):
        pid, body = read_packet(self.sock, self.threshold)
        if pid == CB_PLAYER_POSITION:
            _, pos = rd_varint(body, 0)
            self.position = struct.unpack(">ddd", body[pos:pos + 24])
        return pid, body

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
            if pid == 0x0F:  # select_known_packs (0x0e is update_tags in 26.3)
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
                        out += (varint(len(ns)) + ns.encode() + varint(len(pk))
                                + pk.encode() + varint(len(ver)) + ver.encode())
                    self.send(0x07, out)
            elif pid == 0x03:
                self.send(0x03)
                break
        while True:
            pid, body = self.recv()
            self.join_packets.append((pid, body))
            if pid == 0x32:  # Join Game in 26.3 (0x31 in 26.2)
                break
        for _ in range(400):
            pid, body = self.recv()
            self.join_packets.append((pid, body))
            if pid == CB_KEEP_ALIVE:
                self.send(SB_KEEP_ALIVE, body[:8])
            elif pid == 0x0B:
                batch, _ = rd_varint(body)
                self.send(0x0B, varint(batch))
                break
        return True


def drain(bot, timeout=1.2):
    """Every packet that has arrived in the next `timeout` seconds.

    Left as a full window: this test's `Bot` reads the socket directly (there is
    no reader thread here — `other`, below, is the wire_client client that has
    one), so a quiet window has nothing to observe silence from.
    """
    return wire_client.drain(bot, timeout)


def settle(bot, seconds):
    """Collect for `seconds`, sending one game tick between reads: the *world*
    tick runs on the server's own clock, but the session's survival clock — which
    is what advances a bow's draw and drains a landed pearl's teleport — only
    advances on a recognized serverbound packet."""
    packets = []
    end = time.time() + seconds
    while time.time() < end:
        bot.send(SB_FLYING, bytes([1]))
        packets += drain(bot, 0.12)
        time.sleep(TICK_MS)
    return packets


def advance(bot, ticks):
    """Advance the session clock by exactly `ticks` recognized packets, without
    waiting on wall time. Under `PLUSMC_SURVIVAL_CLOCK=packet` one packet is one
    survival tick, so this is precisely the bow's draw."""
    packets = []
    for _ in range(ticks):
        bot.send(SB_FLYING, bytes([1]))
        packets += drain(bot, 0.08)
    return packets


def advance_until(bot, done, budget=10.0):
    """Advance one recognized packet at a time until `done(packets)` says the
    thing being waited for has happened.

    `budget` is *wall time*, and deliberately so, because the wait is straddling
    two clocks that are not the same one. Under `PLUSMC_SURVIVAL_CLOCK=packet` a
    packet is one *survival* tick — which is the bow's draw, and a quantity this
    test decides — but a shot's flight, the damage it lands and the block it
    buries itself in are the **world** loop's work (`tick_world` runs off the
    50 ms clock in `src/main.cpp`). A budget counted in packets therefore counts
    the load the machine happened to be under: with the world busy, twenty
    packets can go by with the arrow barely ticking, and the wait then reports
    "nothing happened" for a flight that is still in the air — which is
    indistinguishable, inside the assertions that follow, from one that landed
    and was never reported.

    Wall time is the clock the world itself runs on, so a wall budget means the
    same thing on an idle and a loaded machine; and it is a *failure* bound only
    — the wait ends the moment `done` says the thing happened, and a server that
    is still ticking will always answer inside it.
    """
    packets = []
    end = time.time() + budget
    while time.time() < end:
        packets += advance(bot, 1)
        if done(packets):
            return packets
    return packets


def wait_settled(bot, eid, timeout=6.0, quiet=0.45, last=None, seen=None):
    """Collect until `eid` has reported no movement for `quiet` seconds.

    A mob sends a position sync on every tick it moved and on no other, so a
    stretch with no sync for it is the mob's own report that it has stopped.

    `last` is a position already known for `eid`: a mob that is *already*
    standing still sends no further sync at all, so a wait that starts from
    nothing would sit out its timeout and return None — and a mob that reports no
    movement has not moved, so the position already known is still its position.

    Waiting for that, rather than assuming where the mob went from where the
    summon put it, is what makes the shot land. The ring's landing is not where
    the mob ends up: it falls, it strolls, and even an untargeted one is not
    axis-exact about either, so a third of a block of drift is normal and a
    0.6-wide box does not cover it. A shot released mid-stroll is aimed half a
    block behind the mob — the two packets between the aim and the release are
    ~100 ms, which is ~0.4 blocks of walking — so the aim has to be taken from a
    standstill.
    """
    last_seen = time.time() if last is not None else None
    end = time.time() + timeout
    while time.time() < end:
        now = time.time()
        batch = settle(bot, 0.15)
        if seen is not None:
            seen += batch
        for body in all_of(batch, CB_ENTITY_POSITION_SYNC):
            sync = read_position_sync(body)
            if sync["entity"] != eid:
                continue
            last = sync
            last_seen = now
        if last_seen is not None and now - last_seen >= quiet:
            break
    return last


def newest_word(window, last, eid):
    """`last` folded through every position `eid` reported in `window`.

    The packets are in the order the server sent them and a mob reports on each
    tick it moved, so the last word is where it is now. A mob that has stopped
    sends nothing at all, which is why the caller passes the position it already
    has rather than starting from nothing.
    """
    out = last
    for body in all_of(window, CB_ENTITY_POSITION_SYNC):
        word = read_position_sync(body)
        if word["entity"] == eid:
            out = word
    return out


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
        self.health = (FULL_HEALTH, 20, 5.0)
        self.state = 0
        self.slots = None

    def observe(self, packets):
        for body in all_of(packets, CB_SET_HEALTH):
            self.health = read_set_health(body)
        for body in all_of(packets, CB_CONTAINER_SET_CONTENT):
            _, self.state, self.slots, _ = decode_content(body)
        for body in all_of(packets, CB_CONTAINER_SET_SLOT):
            window, state, slot, stack = decode_set_slot(body)
            self.state = state
            if window == 0 and self.slots is not None and 0 <= slot < len(self.slots):
                self.slots[slot] = stack
        return packets

    def command(self, text, timeout=1.2):
        step(f"-> /{text}")
        send_chat_command(self.bot, text)
        return self.observe(drain(self.bot, timeout))

    def give(self, item, count=1):
        """`/give <item> <count>`; returns the menu slot it landed in."""
        packets = self.command(f"give {self.bot.name} minecraft:{item} {count}")
        assert "Gave " in feedback(packets), feedback(packets)
        contents = all_of(packets, CB_CONTAINER_SET_CONTENT)
        assert contents, "a /give must resync the inventory"
        _, _, self.slots, _ = decode_content(contents[-1])
        want = ITEMS_BY_NAME[item]
        for i, stack in enumerate(self.slots):
            if stack is not None and stack["item"] == want:
                return i
        raise AssertionError(f"{item} is nowhere in the menu: {self.slots}")

    def select(self, slot):
        assert SLOT_MENU_HOTBAR_FIRST <= slot < SLOT_MENU_HOTBAR_FIRST + MENU_HOTBAR_SLOTS, \
            f"menu slot {slot} is outside the hotbar"
        self.bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", slot - SLOT_MENU_HOTBAR_FIRST))
        drain(self.bot, 0.2)

    def draw_and_release(self, slot, draw_packets, yaw=0.0, pitch=0.0, collect=1.4):
        """Select `slot`, start a draw, advance it `draw_packets` session ticks,
        release, and collect the flight.

        The release packet's own clock tick lands *before* the handler
        (`tick_survival_on_packet` runs first), so the drawn time the server
        measures is `draw_packets + 1` — which is why callers here think in
        drawn time and subtract one.
        """
        self.select(slot)
        use_item(self.bot, yaw=yaw, pitch=pitch)
        packets = self.observe(drain(self.bot, 0.25))
        packets += self.observe(advance(self.bot, draw_packets))
        release_use_item(self.bot)
        packets += self.observe(settle(self.bot, collect))
        return packets


def spawns(packets):
    return [read_spawn(b) for b in all_of(packets, CB_SPAWN_ENTITY)]


def arrow_spawns(packets, etype=ET_ARROW):
    """Every spawned entity of the arrow *kind* asked for: `minecraft:arrow` by
    default, or `minecraft:spectral_arrow` — the two arrow items are two entity
    types."""
    return [s for s in spawns(packets) if s["type"] == etype]


def shared_flags(packets, eid):
    """Every shared-flags byte the entity `eid` was sent, in packet order.

    A mob has no effect packet — `update_mob_effect` is only ever sent to the
    effect's own owner (`ServerPlayer.onEffectAdded`/`onEffectUpdated`) or to
    their passengers — so the whole visible state of a mob's effects in vanilla
    is this one byte, resent only when it changes.
    """
    out = []
    for body in all_of(packets, CB_SET_ENTITY_DATA):
        data = read_entity_data(body, eid)
        if data is not None and SHARED_FLAGS_INDEX in data[1]:
            out.append(data[1][SHARED_FLAGS_INDEX])
    return out


def mob_health_words(packets, eid):
    """Every health word `packets` carries for `eid`, in order: the spawn's own
    `make_health_word` and each later `Mob.actuallyHurt`.

    A living entity's health is synched only when something changes it, so the
    list is a record of what has happened to the mob rather than a poll: a fresh
    one has exactly its spawn's full health, and anything below that is a hit
    that landed.
    """
    out = []
    for body in all_of(packets, CB_SET_ENTITY_DATA):
        data = read_entity_data(body, eid)
        if data is not None and LIVING_HEALTH_INDEX in data[1]:
            out.append(data[1][LIVING_HEALTH_INDEX])
    return out


def arrow_in_ground(packets, eid):
    """Whether the arrow `eid` has resent its metadata with the in-ground flag
    set — the packet an arrow that has just hit the ground sends."""
    data = [read_entity_data(b, eid) for b in all_of(packets, CB_SET_ENTITY_DATA)]
    return any(v is not None and v[1].get(ARROW_IN_GROUND_INDEX) for v in data)


def arrow_in_ground_cleared(packets, eid):
    """Whether the arrow `eid` has resent its metadata with the in-ground flag
    *clear* — the packet `AbstractArrow.startFalling` sends when the cell holding
    a buried arrow stops colliding."""
    for body in all_of(packets, CB_SET_ENTITY_DATA):
        data = read_entity_data(body, eid)
        if data is not None and data[1].get(ARROW_IN_GROUND_INDEX) is False:
            return True
    return False


def arrow_gone(packets, eid):
    """Whether the arrow `eid` has been *removed* — the other way a flight ends.

    `AbstractArrow.onHitEntity` ends its accepted branch with `if
    (this.getPierceLevel() <= 0) this.discard()` (the decomp's own lines, which
    `resolve_arrow_entity_hit` in `src/world/projectile.cpp` mirrors and
    `tests/projectile_test.cpp` pins), so a plain bow shot that *damages* a
    living thing is gone a tick or two into its arc and no in-ground datum is
    ever coming for it. That is not a corner case a wait can sit out: it is what
    a mob the natural spawn pass left standing in the line of fire does to a
    shot, and the wait below then reads a mid-flight position as the one the
    arrow landed on.

    Entity ids are handed out again after a removal, so a Remove Entities packet
    only counts for this arrow when it arrives *after* this arrow's own spawn.
    """
    seen = False
    for pid, body in packets:
        if pid == CB_SPAWN_ENTITY:
            if read_spawn(body)["entity"] == eid:
                seen = True
        elif seen and pid == CB_REMOVE_ENTITIES and eid in read_remove_entities(body):
            return True
    return False


def fly_until_buried(bot, max_ticks=60, etype=ET_ARROW):
    """Advance until the arrow this shot spawned reaches the end of its flight.

    Two things end a flight and the world reports both: `onHitBlock`'s
    `setInGround(true)` — a synched datum — and the arrow's removal, which is how
    a flight that met something alive ends (`arrow_gone`). The second is the one
    this wait used to be blind to, and an arrow the world has already taken away
    is not a bury that is *late*: it is a flight over, and waiting out a guard for
    a datum that will never come hands the step a mid-flight position to assert
    its landing on.

    The arrow's own id is not known before it spawns, and an earlier shot's
    *buried* arrow is the same entity type — its flag can land inside these same
    packets — so the id is taken from the spawn this window sees and not assumed
    from anything earlier. `etype` is the entity type the ammunition fired as.

    `max_ticks` counts the arrow's *own* world ticks, not packets. A flight is
    the world loop's work (`tick_projectiles` runs off the 50 ms clock in
    `src/main.cpp`), while a serverbound packet advances only the *survival*
    clock under `PLUSMC_SURVIVAL_CLOCK=packet`. The two keep pace on an idle
    machine, which is what makes a packet budget look like a tick budget; under
    load they do not, and the wait then reads a mid-flight arrow as one that has
    already landed (or, here, as one that never will). The arrow's own position
    syncs are the one clock it reports: one per world tick it moved. The packet
    loop therefore keeps going while the world is behind — up to a guard of eight
    times the tick budget, which a world that is merely slow will not reach but a
    world that is not ticking at all will, so a stopped server still fails the
    step instead of hanging the suite.
    """
    packets = []
    eid = None
    ticks = 0
    for _ in range(max_ticks * 8):
        batch = advance(bot, 1)
        packets += batch
        if eid is None:
            fired = arrow_spawns(packets, etype)
            if fired:
                eid = fired[-1]["entity"]
            continue
        ticks += sum(1 for body in all_of(batch, CB_ENTITY_POSITION_SYNC)
                     if read_position_sync(body)["entity"] == eid)
        if (arrow_in_ground(packets, eid) or arrow_gone(packets, eid)
                or ticks >= max_ticks):
            return packets
    return packets


def spawn_summary(packets):
    """Every spawned entity as (type, position, velocity) — what a failed
    "nothing spawned" assertion needs in order to say *why*."""
    return [(s["type"], (round(s["x"], 3), round(s["y"], 3), round(s["z"], 3)),
             tuple(round(c, 4) for c in s["velocity"])) for s in spawns(packets)]


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-projectile-")
    # Chunk (-2, -1) of this fixture is real terrain — stone under a grass
    # surface — which is what an arrow buries itself in at the end of its arc.
    shutil.copytree(os.path.join(os.path.dirname(__file__), "fixtures", "ow3region"),
                    os.path.join(world, "region"))
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
    # No natural spawn pass. A mob standing in a shot's arc is not an arrow's
    # landing, it is the arrow's *removal* — `AbstractArrow.onHitEntity` discards
    # a non-piercing arrow whose hit landed — so a step that reads a flight's
    # bury would be reading whatever the spawner happened to put under it. The
    # two steps that mean to hit a mob summon one by hand, and step 5c proves the
    # removal on purpose, so nothing here loses coverage by turning the pass off.
    env["PLUSMC_MOB_SPAWNING"] = "0"

    proc = None
    try:
        step(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", "127.0.0.1", "--port", "0", "--motd", "projectile test",
             "--offline-mode", "--gamemode", "0", "--data", data_root, "--world", world,
             "--no-flat-world"], wait_listening, env=env)

        bot = Bot("ShotBot", port)
        wire_client.record(bot, note="projectile-wire: the bow, the arrow, the throwables")
        bot.join()
        h = Harness(bot)
        join_packets = h.observe(drain(bot, 1.5))
        attrs = all_of(join_packets, CB_UPDATE_ATTRIBUTES)
        assert attrs, "the join must push the attribute set (which carries the entity id)"
        bot.entity_id, _ = rd_varint(attrs[-1], 0)

        h.command("tp -24 100 -8")
        assert bot.position == (-24.0, 100.0, -8.0), bot.position
        feet = bot.position
        step(f"  OK standing at {feet}")

        # --- 1. the LpVec3 decoder's own framing -----------------------------
        # A hand-built scale-5 vector of (5, 0, -5), packed exactly as
        # write_lp_vec3 does it: the markers in the buffer's low three bits (bit
        # 2 the "scale did not fit" flag, bits 0-1 the scale's low bits), the
        # three 15-bit components at offsets 3/18/33, then the two low bytes and
        # the high four *big-endian*, then the scale varint (5 >> 2 = 1). Seven
        # bytes, and the components come back exactly — which is the check the
        # flat-six readers cannot make, and what a spawn velocity of 3 or more
        # needs.
        def pack_lp_vec3(x, y, z, scale):
            partial = (scale & 3) != scale
            markers = ((scale & 3) | 4) if partial else scale

            def pack(v):
                return int(math.floor((v * 0.5 + 0.5) * 32766.0 + 0.5))

            buffer = (markers | (pack(x / scale) << 3) | (pack(y / scale) << 18)
                      | (pack(z / scale) << 33))
            out = (bytes([buffer & 0xFF, (buffer >> 8) & 0xFF])
                   + struct.pack(">I", buffer >> 16))
            if partial:
                out += varint(scale >> 2)
            return out

        raw = pack_lp_vec3(5.0, 0.0, -5.0, 5)
        assert len(raw) == 7, len(raw)
        (rx, ry, rz), end = read_lp_vec3(raw, 0)
        assert end == 7, f"a scale-5 LpVec3 is seven bytes, not {end}"
        assert abs(rx - 5.0) < 1e-6 and abs(ry) < 1e-6 and abs(rz + 5.0) < 1e-6, (rx, ry, rz)
        # The same components with a scale that fits in the markers stay six.
        small = pack_lp_vec3(3.0, 0.0, -3.0, 3)
        assert len(small) == 6, len(small)
        (sx, sy, sz), small_end = read_lp_vec3(small, 0)
        assert small_end == 6, small_end
        assert abs(sx - 3.0) < 1e-6 and abs(sz + 3.0) < 1e-6, (sx, sy, sz)
        step("  OK the LpVec3 decoder frames a scale-5 vector in seven bytes")

        # --- 2. the bow and its ammunition -----------------------------------
        # The bow first, then one *spectral* arrow and no ordinary arrow yet:
        # `Player.getProjectile` is "the first arrow stack in the inventory", so
        # the way to fire a particular kind is to hold only that kind. The
        # spectral stack is emptied by the 2b shot below before the ordinary
        # stack is granted, which is also what keeps steps 3-5b on plain arrows.
        bow_slot = h.give("bow", 1)
        spectral_slot = h.give("spectral_arrow", 1)
        assert h.slots[bow_slot]["item"] == ITEMS_BY_NAME["bow"], h.slots[bow_slot]
        assert h.slots[spectral_slot]["item"] == ITEMS_BY_NAME["spectral_arrow"], \
            h.slots[spectral_slot]
        step(f"  OK a bow in {bow_slot}, a spectral arrow in {spectral_slot}")

        # --- 2b. the ammunition's own entity type ----------------------------
        # `SpectralArrowItem.createArrow` is the one arrow item whose entity type
        # is not `minecraft:arrow` — the item and the entity are both
        # `spectral_arrow` (123) — while `ArrowItem.createArrow` covers the tag's
        # other two members. A server that builds every arrow as a plain one
        # fires type 6 here and consumes the spectral stack anyway, which is
        # exactly what this step can see.
        h.select(bow_slot)
        use_item(bot, yaw=0.0, pitch=90.0)
        packets = h.observe(drain(bot, 0.25))
        packets += h.observe(advance(bot, 19))
        release_use_item(bot)
        packets += h.observe(fly_until_buried(bot, etype=ET_SPECTRAL_ARROW))
        spectral = arrow_spawns(packets, ET_SPECTRAL_ARROW)
        assert len(spectral) == 1, \
            f"a spectral arrow must fire as its own entity type: {spawn_summary(packets)}"
        assert h.slots[spectral_slot] is None, h.slots[spectral_slot]
        step(f"  OK a spectral arrow is consumed and fires as type {ET_SPECTRAL_ARROW}"
             f" (not {ET_ARROW})")

        # --- 2c. the ordinary ammunition -------------------------------------
        arrow_slot = h.give("arrow", 32)
        assert h.slots[arrow_slot]["item"] == ITEMS_BY_NAME["arrow"], h.slots[arrow_slot]
        step(f"  OK arrows in {arrow_slot}")

        # --- 3. the charge curve's refusal end -------------------------------
        # A drawn time of 2 ticks is `(0.1^2 + 0.2) / 3` = 0.0700, below
        # `BowItem.releaseUsing`'s 0.1 gate: the release returns false and spawns
        # nothing at all. One draw packet, plus the release packet's own tick.
        packets = h.draw_and_release(bow_slot, draw_packets=1, collect=0.8)
        assert not arrow_spawns(packets), \
            f"a 2-tick draw must not fire: {spawn_summary(packets)}"
        step(f"  OK a 2-tick draw (power {bow_power_for_time(2):.4f}) refuses to fire")

        # --- 4. the charge curve, three ticks --------------------------------
        packets = h.draw_and_release(bow_slot, draw_packets=2, pitch=90.0, collect=1.0)
        fired_list = arrow_spawns(packets)
        assert len(fired_list) == 1, \
            f"a 3-tick draw must fire exactly one arrow: {spawn_summary(packets)}"
        fired = fired_list[0]
        want_speed = charge_velocity(3)
        assert abs(want_speed - 0.3225) < 1e-12, want_speed
        # Straight down: a bow has no pitch offset, so `shootFromRotation`'s
        # `yd = -sin((90 + 0))` is -1 and the aim is the whole speed along -y.
        assert_shot(fired["velocity"], want_speed, (0.0, -1.0, 0.0), "a 3-tick draw")
        step(f"  OK a 3-tick draw (power {bow_power_for_time(3):.4f}) fires at "
             f"{want_speed:.4f} within the {PROJECTILE_SPREAD} triangle")

        # --- 5. a full draw, and the arrow's own tick order -------------------
        # Drawn and released by hand rather than through `draw_and_release`: the
        # flight has to be ticked until it lands rather than collected for a
        # wall-clock second, because this arrow has ~24 blocks of air to cross
        # first and the number of its ticks a second buys is not fixed.
        h.select(bow_slot)
        use_item(bot, yaw=0.0, pitch=90.0)
        packets = h.observe(drain(bot, 0.25))
        packets += h.observe(advance(bot, 19))
        release_use_item(bot)
        packets += h.observe(fly_until_buried(bot))
        arrows = arrow_spawns(packets)
        assert len(arrows) == 1, \
            f"a full draw must fire exactly one arrow: {spawn_summary(packets)}"
        arrow = arrows[0]
        # The flight ended on the in-ground datum (the wait returns on it), which
        # is the only thing the assertions below can read the landing off. An
        # arrow that met something alive is a *different* ending — vanilla
        # discards it (`arrow_gone`) — so the premise that nothing was in the arc
        # is checked rather than assumed: this run turns the natural spawn pass
        # off, and step 5c puts a mob there deliberately to watch the removal.
        assert not arrow_gone(packets, arrow["entity"]), \
            f"the arrow was consumed before it buried: {spawn_summary(packets)}"
        # The spawn point: the firer's own x/z, and `getEyeY() - 0.1f` for y —
        # the *bottom* of the arrow's 0.5-tall box.
        assert abs(arrow["x"] - feet[0]) < 1e-9, arrow
        assert abs(arrow["z"] - feet[2]) < 1e-9, arrow
        assert abs(arrow["y"] - (feet[1] + EYE_HEIGHT - SPAWN_EYE_OFFSET)) < 1e-9, arrow
        assert_shot(arrow["velocity"], 3.0, (0.0, -1.0, 0.0), "a full draw")
        arrow_data = [read_entity_data(b, arrow["entity"])
                      for b in all_of(packets, CB_SET_ENTITY_DATA)]
        arrow_data = [v for v in arrow_data if v is not None]
        assert arrow_data, "the arrow's synched data must follow its spawn"
        assert arrow_data[0][1][ARROW_FLAGS_INDEX] & ARROW_FLAG_CRIT, arrow_data[0]
        step("  OK a 20-tick draw fires at 3.0000 with the critical flag set")

        # `AbstractArrow.tick` runs `stepMoveAndHit` — `position += velocity`,
        # from the velocity the tick *started* with — and only then applies
        # `applyInertia` (the 0.99) and `applyGravity` (the 0.05), so the two
        # halves of that order are visible across successive position steps:
        # the step into a sync is the velocity *before* the damping, while the
        # next step is that velocity damped. 26.3's PositionPath carries only
        # the position, so the steps are the velocity sequence. A server that
        # damped *before* moving is `0.05 - 0.01*|v|` out on the first step,
        # i.e. 0.02 of a block at a full draw.
        syncs = [read_position_sync(b) for b in all_of(packets, CB_ENTITY_POSITION_SYNC)]
        syncs = [s for s in syncs if s["entity"] == arrow["entity"]]
        assert len(syncs) >= 5, f"too few position syncs to check the flight: {len(syncs)}"
        first = syncs[0]
        # The first sync's position step *is* the velocity the tick started
        # with (`position += velocity`, then the drag/gravity halves).
        launch = [first[axis] - arrow[axis] for axis in ("x", "y", "z")]
        # The launch velocity the flight implies, against the one Add Entity
        # carried: that one is an LpVec3, which can only name a component to its
        # own grid — a `2 * scale / 32766` step for the `ceil(max|v|)` scale the
        # encoder chose — so the two agree to half a cell. This is the check
        # that the *client's* velocity is the one the server ticks with.
        half_cell = math.ceil(max(abs(c) for c in launch)) / 32766.0
        for axis, name in enumerate(("x", "y", "z")):
            assert abs(arrow["velocity"][axis] - launch[axis]) <= half_cell, \
                (name, arrow["velocity"], tuple(launch), half_cell)
        step(f"  OK the first step is the undamped launch (vy {launch[1]:.6f}), then the "
             f"drag makes it {syncs[1]['y'] - first['y']:.6f}")
        # Every later tick is the same two lines, and every one of them is a
        # packet: the position advances by the velocity the *previous* sync
        # carried and that velocity is damped and charged again. The last sync
        # is the tick the arrow buried itself on — stepping back 0.05 along the
        # velocity it hit with and stopping dead — so the pair ending there is
        # excluded and asserted on its own.
        # Every normal flight step is the previous step damped (and
        # gravity-charged on y). The last sync is the bury tick, checked below.
        positions = [arrow] + syncs
        for prevprev, prev, cur in zip(positions[:-2], positions[1:-1],
                                       positions[2:-1]):
            for axis in ("x", "y", "z"):
                velocity_step = prev[axis] - prevprev[axis]
                next_step = cur[axis] - prev[axis]
                damped = velocity_step * ARROW_AIR_DRAG
                if axis == "y":
                    damped -= ARROW_GRAVITY
                assert abs(next_step - damped) < 1e-9, \
                    (axis, velocity_step, next_step, damped, prev, cur)
        last = syncs[-1]
        # It found the ground inside one tick: the bury tick moves at most the
        # travel the previous one was carrying, plus the 0.05 it steps back.
        prev = syncs[-2]
        for axis in ("x", "y", "z"):
            carried = prev[axis] - syncs[-3][axis]
            assert abs(last[axis] - prev[axis]) <= abs(carried) + 0.05 + 1e-9, \
                (axis, prev, last)
        arrow_ground = [read_entity_data(b, arrow["entity"])
                        for b in all_of(packets, CB_SET_ENTITY_DATA)]
        arrow_ground = [v for v in arrow_ground if v is not None]
        assert any(v[1].get(ARROW_IN_GROUND_INDEX) for v in arrow_ground), \
            "a buried arrow must resend its in-ground flag"
        removes = [read_remove_entities(b) for b in all_of(packets, CB_REMOVE_ENTITIES)]
        assert all(arrow["entity"] not in ids for ids in removes), \
            f"a buried arrow stays in the world (it despawns after 1200 ticks): {removes}"
        step(f"  OK {len(syncs)} flight ticks follow the move/drag/gravity law, then it "
             f"stops in the ground at y {last['y']:.4f}")

        # The arrow was consumed out of the inventory — one per shot, and the
        # 2-tick draw `releaseUsing` refused took none. Two shots have fired,
        # out of a stack of 32, so the stack is `ProjectileWeaponItem.useAmmo`'s
        # count and not the number of releases.
        assert h.slots[arrow_slot]["count"] == 32 - 2, h.slots[arrow_slot]
        step(f"  OK the arrow stack is down to {h.slots[arrow_slot]['count']}")

        # --- 5b. breaking the ground under a buried arrow ------------------
        # `IN_GROUND` is a synched datum: `AbstractArrow.onHitBlock` sets it true
        # and `startFalling` — the branch a buried arrow takes when the cell it
        # is stuck in stops colliding — sets it false again, both through
        # `SynchedEntityData.set`. The false half is the one a server can forget:
        # the arrow keeps falling either way on the wire, so a test that only
        # watches positions cannot tell the two apart. Which cell holds it up is
        # not a single answer: `shouldFall` probes `noCollision(AABB(position,
        # position).inflate(0.06))`, so the cells that can hold the arrow are
        # exactly the 2x2x2 grid its 0.12 cube can reach into — two cells per
        # axis, because the cube spans the cell border for every axis the shot's
        # spread nudged the bury point across. Every one of them is broken
        # (`/setblock air` on a cell that is already air fails, which is why the
        # failures are tolerated and only a successful change is required).
        #
        # All eight and not just the cube's lowest layer, because where the arrow
        # lands is the spread's choice: when it buries on top of the surface the
        # lowest layer is all the cube spans, but a couple of tenths to the side
        # of that column the terrain has a pocket, the arrow buries several
        # blocks deeper, and its cube then spans two cells vertically. Breaking
        # the whole grid makes the premise of the step — the block holding it up
        # is gone — true wherever it landed.
        cells = []
        for cx in (math.floor(last["x"] - 0.06), math.floor(last["x"] + 0.06)):
            for cy in (math.floor(last["y"] - 0.06), math.floor(last["y"] + 0.06)):
                for cz in (math.floor(last["z"] - 0.06), math.floor(last["z"] + 0.06)):
                    cell = (cx, cy, cz)
                    if cell not in cells:
                        cells.append(cell)
        packets = []
        changed = 0
        for cx, cy, cz in cells:
            broken = h.command(f"setblock {cx} {cy} {cz} air")
            changed += "Changed the block" in feedback(broken)
            packets += broken
        assert changed, f"no cell under the arrow at {last} could be broken: {cells}"
        # The arrow noticing its block is gone is the *world's* work —
        # `shouldFall` runs inside the arrow's own tick — so the wait for the
        # cleared flag is on the world's clock, and ends when the flag arrives.
        packets += h.observe(advance_until(
            bot, lambda ps: arrow_in_ground_cleared(ps, arrow["entity"]), budget=10.0))
        data = [read_entity_data(b, arrow["entity"])
                for b in all_of(packets, CB_SET_ENTITY_DATA)]
        data = [v for v in data if v is not None]
        cleared = [v for v in data if v[1].get(ARROW_IN_GROUND_INDEX) is False]
        assert cleared, \
            f"an arrow freed by its block disappearing must clear its in-ground " \
            f"flag on the wire (broke {cells} under y {last['y']:.4f}): {data}"
        fell = [read_position_sync(b)
                for b in all_of(packets, CB_ENTITY_POSITION_SYNC)]
        fell = [s for s in fell if s["entity"] == arrow["entity"]]
        assert fell and fell[-1]["y"] < last["y"], (last, fell)
        step(f"  OK the block under the buried arrow goes and it falls again "
             f"(y {last['y']:.4f} -> {fell[-1]['y']:.4f}, the in-ground datum cleared)")

        # --- 6. the arrow's damage on a mob ----------------------------------
        # The firer switches to creative for this one, which is what keeps the mob
        # out of it: `NO_CREATIVE_OR_SPECTATOR` is the entity selector the target
        # scan uses, so a creative player is not targeted and the arrow — not a
        # zombie's 3 a second — is the only thing that lands. The mob itself is
        # unaffected: nothing on the impact path looks at the firer's gamemode.
        # It is also what makes the *aim* two blocks' worth of work rather than a
        # chase: an untargeted mob stays where the ring put it.
        h.command("gamemode creative")
        # --- 5c. an arrow that meets a living thing is removed ------------------
        # `AbstractArrow.onHitEntity`'s accepted branch ends with `if
        # (this.getPierceLevel() <= 0) this.discard()` — the decomp's own lines,
        # which `resolve_arrow_entity_hit` mirrors and `projectile_test.cpp` pins
        # — so a plain bow shot that damages a mob is *gone* a tick or two into
        # its arc and never reports in-ground at all. That is the ending
        # `fly_until_buried` had no answer for, so it is the ending to prove from
        # the wire rather than argue: the mob goes into a shot's arc on purpose.
        # The creative gamemode above keeps the mob from hunting the firer (which
        # is also what step 6 wants it for), and this step leaves the world
        # mob-free again for the steps after it.
        h.command("tp -24 80 -8")
        h.observe(drain(bot, 0.3))
        assert bot.position == (-24.0, 80.0, -8.0), bot.position
        feet = bot.position
        summon = h.command("summon zombie 1", timeout=0.6)
        mob = next((s for s in spawns(summon) if s["type"] == ET_ZOMBIE), None)
        assert mob is not None, f"no zombie spawn: {spawn_summary(summon)}"
        # The shot is aimed and re-shot exactly the way step 6b aims its own:
        # from a 1.5-block stand-off taken off the mob's own report, with the
        # `/tp`'s rotation and the release going out back to back so the aim
        # cannot go stale in a drain that is not there, and a bounded number of
        # attempts because what this step is about is the arrow's *removal* and
        # not the archery. A miss leaves its arrow in the ground and the mob
        # where it was, and is re-shot rather than judged.
        still = mob
        order = []
        shot = None
        began = waited = 0.0
        for _ in range(3):
            h.select(bow_slot)
            use_item(bot, yaw=0.0, pitch=0.0)
            drawn = h.observe(drain(bot, 0.25))
            drawn += h.observe(advance(bot, 19))  # a full draw
            # The seed is the mob's *own* newest report, and the draw's window is
            # where it is: a summoned mob walks off the ring while the firer's
            # draw is being ticked, and by the time this wait runs it has
            # stopped — a mob that is standing still sends no packet at all, so
            # the wait can only return the position it was seeded with.
            walked = [read_position_sync(b) for b in all_of(drawn, CB_ENTITY_POSITION_SYNC)]
            walked = [s for s in walked if s["entity"] == mob["entity"]]
            if walked:
                still = walked[-1]
            fresh = wait_settled(bot, mob["entity"], timeout=3.0, quiet=0.3, last=still)
            if fresh is not None:
                still = fresh
            stand = (still["x"], still["y"], still["z"] + 1.5)
            eye = (stand[0], stand[1] + EYE_HEIGHT - SPAWN_EYE_OFFSET, stand[2])
            chest = (still["x"], still["y"] + 1.0, still["z"])
            to_mob = (chest[0] - eye[0], chest[1] - eye[1], chest[2] - eye[2])
            flat = math.hypot(to_mob[0], to_mob[2])
            send_chat_command(bot, f"tp {stand[0]} {stand[1]} {stand[2]} "
                                   f"{math.degrees(math.atan2(-to_mob[0], to_mob[2]))} "
                                   f"{math.degrees(-math.atan2(to_mob[1], flat))}")
            release_use_item(bot)
            began = time.time()
            shot = h.observe(fly_until_buried(bot))
            waited = time.time() - began
            hits = [read_damage_event(b) for b in all_of(shot, CB_DAMAGE_EVENT)]
            if any(e["type"] == DMG_ARROW and e["entity"] == mob["entity"] for e in hits):
                order = hits
                break
        assert shot is not None, "the mob shot must fire an arrow at all"
        fired = arrow_spawns(shot)
        assert len(fired) == 1, \
            f"the shot must fire exactly one arrow: {spawn_summary(shot)}"
        eaten = fired[0]["entity"]
        flight = [(round(s["x"], 3), round(s["y"], 3), round(s["z"], 3))
                  for s in (read_position_sync(b)
                            for b in all_of(shot, CB_ENTITY_POSITION_SYNC))
                  if s["entity"] == eaten]
        assert any(e["type"] == DMG_ARROW and e["entity"] == mob["entity"] for e in order), \
            f"the arrow must land on the mob it was fired at: {order}; the mob " \
            f"{mob['entity']} was reported at {still}; the arrow flew {flight}"
        assert arrow_gone(shot, eaten), \
            "a non-piercing arrow whose hit landed is discarded (vanilla's `if " \
            "(this.getPierceLevel() <= 0) this.discard()`), so it must leave the world"
        assert not arrow_in_ground(shot, eaten), \
            "and it never buries: no in-ground datum can follow the removal"
        # The wait is what this step is really pinning. An arrow the world has
        # already removed is a flight that is *over*, so the wait must end there
        # rather than spend its guard on a bury that is not coming — the guard is
        # 480 packets at this test's own ~0.1s packet pace, i.e. ~50s (the run
        # that established this spent 50.33s of it), against the ~0.2s a removal
        # takes to arrive.
        assert waited < 8.0, \
            f"the wait spent {waited:.1f}s on an arrow the world had already " \
            f"removed: the removal is the end of the flight, not a reason to keep " \
            f"looking for a landing"
        step(f"  OK the arrow hit the mob and was discarded; the wait ended in "
             f"{waited:.2f}s (no in-ground datum)")
        h.command("mobs kill")
        # The firer is on the ground, and the draw is started *before* the mob
        # exists: `/summon`'s ring starts at angle 0, so the mob lands at `px +
        # cos(0) * 2`, `pz + sin(0) * 2` — exactly two blocks along +x — and the
        # use packet's own angles are then already pointing at it. y = 80 is the
        # face the first arrow buried itself on, which is where the ground is.
        h.command("tp -24 80 -8")
        assert bot.position == (-24.0, 80.0, -8.0), bot.position
        feet = bot.position
        ring_x, ring_z = 2.0, 0.0
        # `mob_place` nudges a spawn whose box meets a block to the nearest free
        # cell that has a floor, so a ring cell inside this seed's hillside put
        # the zombie a block short of the ring. Clear the box instead of loosening
        # the assertion: the mob is 0.6 wide and 1.95 tall around the ring's
        # centre, i.e. cells x -23..-22, z -9..-8 at y 80..81, and y 79 under them
        # is the floor it has to stand on. The unstuck itself is still exercised
        # by `plusmc-mob-test` and the fallback below.
        for open_x in (-23, -22):
            for open_z in (-9, -8):
                for open_y in (80, 81):
                    h.observe(h.command(f"setblock {open_x} {open_y} {open_z} air",
                                        timeout=0.4))
                h.observe(h.command(f"setblock {open_x} 79 {open_z} stone", timeout=0.4))
        h.select(bow_slot)
        use_item(bot, yaw=math.degrees(math.atan2(-ring_x, ring_z)), pitch=0.0)
        h.observe(drain(bot, 0.25))
        h.observe(advance(bot, 19))
        summon = h.command("summon zombie 1", timeout=0.4)
        zombie = next((s for s in spawns(summon) if s["type"] == ET_ZOMBIE), None)
        assert zombie is not None, f"no zombie spawn: {spawn_summary(summon)}"
        assert "Summoned" in feedback(summon), feedback(summon)
        dx, dz = zombie["x"] - feet[0], zombie["z"] - feet[2]
        if abs(dx - ring_x) > 1e-9 or abs(dz - ring_z) > 1e-9:
            # An uncleared world can still trip the unstuck. Then the mob must be
            # a *whole-block* move off the ring (the nudge adds integer offsets,
            # so the ring's own 2.0/0.0 arithmetic is still what placed it), no
            # further than the unstuck's four-block reach, with nothing between
            # the two cells: a fractional answer would be the ring, not the
            # nudge, getting the position wrong.
            nudge_x, nudge_z = dx - ring_x, dz - ring_z
            assert abs(nudge_x - round(nudge_x)) < 1e-9 and \
                abs(nudge_z - round(nudge_z)) < 1e-9, (dx, dz, zombie)
            assert 0 < round(nudge_x) ** 2 + round(nudge_z) ** 2 <= 16, (dx, dz, zombie)
            step(f"  note: mob_place owed the unstuck this one — ring ({ring_x}, "
                 f"{ring_z}) landed at ({dx}, {dz})")
        # Its y is the summoner's, or `mob_place`'s unstuck lift (at most four
        # blocks) when its box meets a block; the mob then falls back onto the
        # ground a block below.
        assert 0.0 <= zombie["y"] - feet[1] <= 4.0, (zombie, feet)
        step(f"  OK the zombie lands on ({zombie['x']:.3f}, {zombie['y']:.3f}, "
             f"{zombie['z']:.3f})")
        # It walks at the firer from there and stops, off the ring's line — so the
        # aim is taken from the mob's *own* reported coordinate rather than from
        # the ring's arithmetic. The rotation is set with `/tp`'s optional yaw and
        # pitch, which is the one way to turn a player who is not sending look
        # packets; the arrow spawns at the firer's eyes minus the 0.1 its box sits
        # below them.
        #
        # Two measurements bound what can be promised about that stop
        # (`tools/settle_probe.py`, thirty summons). The mob's resting place is
        # anywhere from 0.9 to 4.4 blocks from the firer — `MeleeAttackGoal`'s own
        # reach (`perceived_distance_sqr <= 2.04`) held in only 7 of the 30, so
        # the "comes to rest inside its own attack reach" this comment used to
        # claim is simply not where a summoned zombie stops — and 3 of the 30
        # answered `wait_settled` and then moved again immediately, which is the
        # same 1-in-10 the suite sees. Waiting *harder* does not fix that: a
        # `wait_stopped` that demanded two consecutive quiet windows at one place
        # measured worse (7 of 20 answers followed by movement, against 4 of 20)
        # and gave up once in twenty, so it was measured and rejected rather than
        # adopted. What is left is to make the aim as fresh as the wire allows,
        # and to have a miss say afterwards which of the two things went wrong.
        settled = wait_settled(bot, zombie["entity"], last=zombie)
        assert settled is not None, "the mob never reported a position"
        # A `wait_settled` answer is `quiet` seconds old the moment it comes back,
        # and a zombie covers about 0.7 blocks in 0.45 s — more than its 0.6-wide
        # box. Whatever it has said *since* is folded in before the aim is
        # computed, the way step 6b folds its own windows: a mob still walking
        # answers with a fresher word, and one that has stopped says nothing at
        # all so the wait's word stands.
        aimed = newest_word(h.observe(settle(bot, 0.25)), settled, zombie["entity"])
        eye = (feet[0], feet[1] + EYE_HEIGHT - SPAWN_EYE_OFFSET, feet[2])
        chest = (aimed["x"], aimed["y"] + 1.0, aimed["z"])
        to_mob = (chest[0] - eye[0], chest[1] - eye[1], chest[2] - eye[2])
        assert to_mob[0] > 0.0, (aimed, eye)
        flat = math.hypot(to_mob[0], to_mob[2])
        shot_yaw = math.degrees(math.atan2(-to_mob[0], to_mob[2]))
        shot_pitch = math.degrees(-math.atan2(to_mob[1], flat))
        step(f"  OK it stopped at ({aimed['x']:.3f}, {aimed['y']:.3f}, "
             f"{aimed['z']:.3f}); aiming yaw {shot_yaw:.2f} pitch {shot_pitch:.2f}")
        # The aim and the release go out back to back, and they have to. `h.command`
        # drains for its 1.2 s timeout after sending, so aiming with it puts a
        # quarter of a second plus 1.2 s — about 1.7 blocks of a walking zombie —
        # between the rotation and the arrow. Two packets from one connection are
        # handled in the order they arrived, so the `/tp`'s rotation is in place by
        # the time the release is handled, and the premise is asserted here so that
        # a drain creeping back in fails at its own line.
        aimed_at = time.time()
        send_chat_command(bot, f"tp {feet[0]} {feet[1]} {feet[2]} "
                               f"{shot_yaw} {shot_pitch}")
        release_use_item(bot)
        stale_by = time.time() - aimed_at
        assert stale_by < 0.5, (
            f"the aim and the release must go out back to back: {stale_by:.2f}s of "
            f"drain between them is most of a block of a walking mob")
        # Ticked until the hit lands, for the same reason the drop above is: two
        # blocks of flight is one tick, but *which* packet it is on is not
        # something a second of wall time can be asked to guarantee.
        packets = h.observe(advance_until(
            bot, lambda ps: any(read_damage_event(b)["type"] == DMG_ARROW
                                for b in all_of(ps, CB_DAMAGE_EVENT))))
        packets += h.observe(advance(bot, 2))
        fired_list = arrow_spawns(packets)
        assert len(fired_list) == 1, f"the mob shot must fire one arrow: {spawn_summary(packets)}"
        shot_arrow = fired_list[0]
        events = [read_damage_event(b) for b in all_of(packets, CB_DAMAGE_EVENT)]
        arrow_events = [e for e in events if e["type"] == DMG_ARROW]
        arrow_flight = [(round(s["x"], 3), round(s["y"], 3), round(s["z"], 3))
                        for s in (read_position_sync(b)
                                  for b in all_of(packets, CB_ENTITY_POSITION_SYNC))
                        if s["entity"] == shot_arrow["entity"]]
        own_flight = [(round(s["x"], 3), round(s["y"], 3), round(s["z"], 3))
                      for s in (read_position_sync(b)
                                for b in all_of(packets, CB_ENTITY_POSITION_SYNC))
                      if s["entity"] == zombie["entity"]]
        # A miss has two causes and they are worlds apart: a mob that walked away
        # from the aim (this test's arithmetic is fine, the world moved under it)
        # or an arrow that went through a mob that was standing still (a product
        # defect). The failure says which one it is instead of leaving the reader
        # to diff two coordinate lists.
        left = bool(own_flight) and max(abs(own_flight[-1][0] - aimed["x"]),
                                        abs(own_flight[-1][1] - aimed["y"]),
                                        abs(own_flight[-1][2] - aimed["z"])) > 0.35
        assert arrow_events, (
            f"no minecraft:arrow damage event: {events}; the arrow flew "
            f"{arrow_flight}; it was aimed at ({aimed['x']:.3f}, {aimed['y']:.3f}, "
            f"{aimed['z']:.3f}) from ({feet[0]:.3f}, {feet[1]:.3f}, {feet[2]:.3f}) "
            f"and the mob reported {own_flight[-4:]} while it flew — "
            + ("the mob left the aim after the shot, so the mob and not the arrow "
               "is what moved" if left
               else "the mob was still where it was aimed, so the arrow is what "
                    "to look at, not the mob"))
        assert arrow_events[-1]["entity"] == zombie["entity"], (arrow_events[-1], zombie)
        # `ClientboundDamageEventPacket` carries the source's *two* entities, and
        # `damageSources().arrow(arrow, owner)` puts the shooter in the causing
        # slot and the arrow itself in the direct one.
        assert arrow_events[-1]["cause"] == bot.entity_id, (arrow_events[-1], bot.entity_id)
        assert arrow_events[-1]["direct"] == shot_arrow["entity"], (arrow_events[-1], shot_arrow)
        healths = [read_entity_data(b, zombie["entity"])
                   for b in all_of(packets, CB_SET_ENTITY_DATA)]
        healths = [v for v in healths if v is not None and LIVING_HEALTH_INDEX in v[1]]
        assert healths, "the mob's health must be synced when the arrow lands"
        landed_health = healths[-1][1][LIVING_HEALTH_INDEX]
        damage = ZOMBIE_MAX_HEALTH - landed_health
        # A full draw is 3.0, so `ceil(|v| * baseDamage)` is 6 (the LpVec3 grid
        # can put |v| a hair over 3.0, which the ceil rounds to 7), and the
        # critical's `nextInt(damage / 2 + 2)` roll is 0 to 4 on top of it — the
        # `pow == 1.0f` that made this arrow critical. Whatever that sum is, it
        # then goes through the zombie's own two armour points, so the health the
        # client is sent is one of exactly these values and nothing else.
        allowed = [after_zombie_armor(raw) for raw in range(6, 12)]
        assert any(abs(damage - want) < 0.01 for want in allowed), \
            f"the arrow's damage must be an armour-adjusted ceil(|v| * 2) plus " \
            f"nextInt(6 / 2 + 2): {damage} is not one of {allowed}"
        step(f"  OK the arrow hit the zombie for {damage:.3f} (health "
             f"{landed_health:.3f})")

        # --- 6b. the spectral arrow's Glowing on the wire ---------------------
        # `SpectralArrow.doPostHurtEffects` is `mob.addEffect(new
        # MobEffectInstance(MobEffects.GLOWING, 200, 0))`, called from inside
        # `AbstractArrow.onHitEntity`'s `if (entity.hurt(...))` branch — the
        # arrow's damage and its effect land or fail together. A mob has no
        # update_mob_effect packet to carry it (that is only sent to the effect's
        # own owner), so what any client can see of it is
        # `LivingEntity.updateGlowingStatus`'s shared flag 6. This server had no
        # lasting mob-effect state at all before this step: the effect had
        # nowhere to live and the byte was never sent for a mob, so a spectral
        # arrow was a plain arrow that happened to be a different entity type.
        #
        # The ammunition decides the entity type (`Player.getProjectile` is "the
        # first arrow stack in the inventory"), so the spectral stack is written
        # *over* the plain one with the creative slot write rather than added
        # beside it: afterwards the only arrows in the inventory are spectral,
        # and "the first arrow stack" is not a race between two kinds.
        h.select(bow_slot)
        bot.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", arrow_slot) + varint(8)
                 + varint(ITEMS_BY_NAME["spectral_arrow"]) + b"\x00\x00")
        h.observe(drain(bot, 0.3))
        assert h.slots[arrow_slot]["item"] == ITEMS_BY_NAME["spectral_arrow"], \
            h.slots[arrow_slot]
        assert all(s is None or s["item"] != ITEMS_BY_NAME["arrow"] for s in h.slots), \
            "the plain arrows must be gone, or 'the first arrow stack' is ambiguous"
        def struck(window):
            """Whether the window holds a `minecraft:arrow` damage event on the
            zombie — the same test step 6 makes of its own shot."""
            return any(read_damage_event(b)["entity"] == zombie["entity"]
                       and read_damage_event(b)["type"] == DMG_ARROW
                       for b in all_of(window, CB_DAMAGE_EVENT))

        # The draw starts before the aim, exactly as in step 6: a
        # RELEASE_USE_ITEM carries no angles, so the shot flies along whatever
        # the last Use Item (or `/tp`) left the player's head pointing.
        #
        # The target is this step's *own* mob, summoned here and unhurt. Step 6's
        # zombie is the obvious target and the wrong one, because `Mob.hurt`'s
        # window can refuse a hit outright (`invulnerable_time >
        # kMobInvulnerableThreshold` and a hit no larger than `last_hurt` is
        # dropped) and that window is counted down one per **world** tick: how
        # long the previous arrow keeps it open in *wall* time is the load's
        # business, and a refused hit is indistinguishable here from a miss. A mob
        # that nothing has hit has no window at all, and the wire says so — the
        # only health word it has sent is its spawn's own full one, which is
        # checked below rather than assumed.
        h.command("mobs kill")
        h.command("tp -24 80 -8")
        h.observe(drain(bot, 0.3))
        assert bot.position == (-24.0, 80.0, -8.0), bot.position
        summon = h.command("summon zombie 1", timeout=0.6)
        zombie = next((s for s in spawns(summon) if s["type"] == ET_ZOMBIE), None)
        assert zombie is not None, f"no zombie spawn: {spawn_summary(summon)}"
        settled_seen = []
        settled = wait_settled(bot, zombie["entity"], timeout=3.0, quiet=0.3,
                               last=zombie, seen=settled_seen)
        assert settled is not None, "the summoned zombie never reported a position"
        # The premise, asserted on the wire instead of assumed: nothing has put a
        # hit on the mob this arrow is about to be aimed at.
        health_words = []
        for window in (summon, settled_seen):
            health_words += mob_health_words(window, zombie["entity"])
        assert health_words and all(abs(h - ZOMBIE_MAX_HEALTH) < 1e-3
                                    for h in health_words), \
            (f"a hit has already landed on the mob this shot is aimed at, and "
             f"`Mob.hurt`'s window (a hit no larger than the previous one, "
             f"refused while `invulnerable_time > kMobInvulnerableThreshold`) "
             f"may now refuse this one: health words {health_words}")
        step(f"  OK a fresh zombie for this shot, at full health "
             f"{health_words[-1]:.1f}")
        still = settled
        packets = None
        misses = []
        # Every attempt drains three windows before it aims — a short one that
        # finishes whatever the last attempt left, the draw itself, and the wait
        # for the mob's next word — and *every* position the mob reports in them
        # is folded into the aim, in the order the mob sent them. That fold is
        # the fix for this step's flake. The loop always collected these words,
        # but only read them for a debug line, so a mob that walked while the
        # firer was drawing and then stood still for the wait left the aim on
        # whatever it had been seeded with — and the retry, seeded from the same
        # place, fired at the same ghost. One discarded window is one repeated
        # miss, four times over, which is exactly the failure that was recorded
        # ("the spectral arrow never landed on the zombie" after all four
        # retries, with the aim still on step 6's `settled`).
        def miss(attempt, word, stand, window, spent):
            """What a *missed* attempt saw, in the words of the things it waited
            on, so that a failure names its cause and not its symptom: the mob's
            own reports while the arrow was in the air (ten seconds of them, and
            the place a retry has to aim), the arrow's flight (a flight of none
            at all is the world's clock, not the mob's position), where the
            arrow ended, and every health word the mob sent."""
            words = [read_position_sync(b) for b in all_of(window,
                                                           CB_ENTITY_POSITION_SYNC)]
            own = [(round(s["x"], 2), round(s["y"], 2), round(s["z"], 2))
                   for s in words if s["entity"] == zombie["entity"]]
            fired = arrow_spawns(window, ET_SPECTRAL_ARROW)
            arrow = fired[0]["entity"] if fired else -1
            fly = [(round(s["x"], 2), round(s["y"], 2), round(s["z"], 2))
                   for s in words if s["entity"] == arrow]
            events = [read_damage_event(b) for b in all_of(window, CB_DAMAGE_EVENT)]
            return (f"[attempt {attempt}] aimed at ({word['x']:.3f}, {word['y']:.3f}, "
                    f"{word['z']:.3f}) from ({stand[0]:.3f}, {stand[1]:.3f}, "
                    f"{stand[2]:.3f}) in {spent:.1f}s; the mob reported {own[-4:]} "
                    f"while the arrow flew; arrow {arrow} "
                    + (f"flew {fly[-4:]}" if fly
                       else "never moved (the world was not ticking)")
                    + f"; removed={bool(all_of(window, CB_REMOVE_ENTITIES))} "
                      f"in_ground={arrow_in_ground(window, arrow)} "
                      f"mob health words={mob_health_words(window, zombie['entity'])} "
                      f"damage events={[(e['type'], e['entity']) for e in events]}")

        for attempt in range(1, 5):
            use_item(bot, yaw=math.degrees(math.atan2(-ring_x, ring_z)), pitch=0.0)
            pre = h.observe(drain(bot, 0.25))
            still = newest_word(pre, still, zombie["entity"])
            drawn = h.observe(advance(bot, 19))  # a full draw
            still = newest_word(drawn, still, zombie["entity"])
            # Where it is now, waited for the way step 6 waited for it: a stretch
            # with no sync of its own. The position already known is passed in,
            # because a mob that is standing still sends nothing at all — without
            # that seed this wait would sit out its timeout and return None.
            waited = []
            fresh = wait_settled(bot, zombie["entity"], timeout=3.0, quiet=0.3,
                                 last=still, seen=waited)
            still = newest_word(waited, fresh if fresh is not None else still,
                                zombie["entity"])
            # The premise, checked on the wire on every attempt: nothing has put a
            # hit on the mob this arrow is aimed at, so `Mob.hurt`'s window cannot
            # be the reason a shot does not land.
            health_words += mob_health_words(pre + drawn + waited, zombie["entity"])
            assert health_words and all(abs(h - ZOMBIE_MAX_HEALTH) < 1e-3
                                        for h in health_words), \
                (f"a hit has already landed on the mob this shot is aimed at, and "
                 f"`Mob.hurt`'s window (a hit no larger than the previous one, "
                 f"refused while `invulnerable_time > kMobInvulnerableThreshold`) "
                 f"may now refuse this one: health words {health_words}")
            # The firer moves to a stand-off from the mob's own report rather
            # than shooting from two and a half blocks away: the arrow's sweep
            # is three blocks of its own, so at 1.5 the whole of a wandering
            # mob's box is on the line and the aim does not have to be dead on
            # a position the *world* clock has been changing since it was
            # reported (the packets here advance only the session's clock).
            stand = (still["x"], still["y"], still["z"] + 1.5)
            eye = (stand[0], stand[1] + EYE_HEIGHT - SPAWN_EYE_OFFSET, stand[2])
            chest = (still["x"], still["y"] + 1.0, still["z"])
            to_mob = (chest[0] - eye[0], chest[1] - eye[1], chest[2] - eye[2])
            flat = math.hypot(to_mob[0], to_mob[2])
            # The aim and the release go out back to back with nothing collected
            # between them: two packets from one connection are handled in the
            # order they arrived, so the `/tp`'s rotation is in place by the time
            # the release is handled — the aim cannot go stale in a drain that is
            # not there.
            send_chat_command(bot, f"tp {stand[0]} {stand[1]} {stand[2]} "
                                   f"{math.degrees(math.atan2(-to_mob[0], to_mob[2]))} "
                                   f"{math.degrees(-math.atan2(to_mob[1], flat))}")
            release_use_item(bot)
            began = time.time()
            shot = h.observe(advance_until(bot, struck))
            shot += h.observe(advance(bot, 3))
            # A hit ends the attempt, whichever packet carried it. A miss is not
            # judged here: the next attempt re-aims from the newest word the mob
            # has sent, and the flight that just ended is ten seconds of exactly
            # those words — where a missed target went.
            if arrow_spawns(shot, ET_SPECTRAL_ARROW) and struck(shot):
                packets = shot
                break
            misses.append(miss(attempt, still, stand, shot, time.time() - began))
            still = newest_word(shot, still, zombie["entity"])
        assert packets is not None, \
            (f"the spectral arrow never landed on the zombie: {spawn_summary(shot)}\n"
             + "\n".join(misses))
        glow_arrows = arrow_spawns(packets, ET_SPECTRAL_ARROW)
        assert len(glow_arrows) == 1, \
            f"the spectral shot must fire one spectral arrow: {spawn_summary(packets)}"
        assert struck(packets), \
            f"the spectral arrow must land on the zombie: {spawn_summary(packets)}"
        flags = shared_flags(packets, zombie["entity"])
        assert flags, "a landed spectral arrow must resend the mob's shared flags"
        assert flags[-1] & FLAG_GLOWING, \
            f"the 200 ticks of Glowing must reach the client as shared flag 6: {flags}"
        assert not flags[-1] & FLAG_ON_FIRE, \
            f"nothing set the zombie on fire, so the other bit must stay clear: {flags}"
        step(f"  OK the spectral arrow lands on the zombie and two ticks later its "
             f"shared flags carry the glowing bit (0x{flags[-1]:02x})")

        # Back to survival: the ender pearl below is *supposed* to cost 5 health,
        # and a creative player takes no damage at all.
        h.command("gamemode survival")

        # --- 7. the three throwables -----------------------------------------
        # Back up out of the mob's reach, still in survival: the zombie follows
        # and hits for 3 a second, and the ender pearl below is measured by the
        # health it costs — a hit inside LivingEntity's twelve-tick invulnerability
        # window only lands the part that exceeds the previous one, so a zombie
        # swinging through the pearl's landing would eat it. Twenty blocks up is
        # `perceived_distance_sqr` beyond any reach.
        h.command("tp -24 100 -8")
        assert bot.position == (-24.0, 100.0, -8.0), bot.position
        feet = bot.position
        # Each spawns at its own entity type with the item throw's fixed 1.5: a
        # bow's 3.0 is BowItem's multiplier, not the projectile's.
        for name, etype in (("snowball", ET_SNOWBALL), ("egg", ET_EGG)):
            slot = h.give(name, 1)
            h.select(slot)
            use_item(bot, yaw=0.0, pitch=0.0)
            packets = h.observe(advance(bot, 6))
            thrown = next((s for s in spawns(packets) if s["type"] == etype), None)
            assert thrown is not None, f"no {name} spawn: {spawn_summary(packets)}"
            speed = math.sqrt(sum(c * c for c in thrown["velocity"]))
            # `spawnProjectileFromRotation(type, ..., 0.0f, 1.5f, 1.0f)` ->
            # `shootFromRotation(source, xRot=0, yRot=0, 0.0f, 1.5f, 1.0f)`: the
            # aim is `(0, 0, 1)` and the throw's own 1.5 is the power.
            assert_shot(thrown["velocity"], THROWABLE_SHOOT_POWER, (0.0, 0.0, 1.0), name)
            assert abs(thrown["y"] - (feet[1] + EYE_HEIGHT - SPAWN_EYE_OFFSET)) < 1e-9, thrown
            assert h.slots[slot] is None, f"the {name} was thrown, not kept: {h.slots[slot]}"
            step(f"  OK {name} (type {etype}) launches at {speed:.4f} along +z and is "
                 "consumed")

        # --- 8. the ender pearl's 5.0 -----------------------------------------
        # Thrown straight down: the pearl lands and its thrower is teleported to
        # `oldPosition()` — where the pearl was when the tick that hit started —
        # and then charged exactly 5.0 of minecraft:ender_pearl, which is what
        # the health bar shows.
        slot = h.give("ender_pearl", 1)
        h.select(slot)
        # The health the client has been told at the moment of the throw. It is
        # only ever *used* if no health packet arrives before the hit, which is
        # itself the proof that it is still current: a change of health is the
        # only thing that sends one, so an absent packet means nothing moved.
        pre_throw_health = h.health[0]
        use_item(bot, yaw=0.0, pitch=90.0)
        # Ticked until the hit, for the same reason: the pearl has a fall to make
        # before its 5.0 is charged at all, and a second of wall time is not a
        # number of ticks. Two ticks past the event so the health update it is
        # paired with is collected too.
        packets = h.observe(advance_until(
            bot, lambda ps: any(read_damage_event(b)["type"] == DMG_ENDER_PEARL
                                for b in all_of(ps, CB_DAMAGE_EVENT))))
        packets += h.observe(advance(bot, 2))
        events = [read_damage_event(b) for b in all_of(packets, CB_DAMAGE_EVENT)]
        pearl_events = [e for e in events if e["type"] == DMG_ENDER_PEARL]
        assert pearl_events, f"no minecraft:ender_pearl damage event: {events}"
        assert pearl_events[-1]["entity"] == bot.entity_id, pearl_events[-1]
        # The bar either side of the *hit*, taken from the packet stream rather
        # than from the last value read before the throw: the player heals out of
        # the food's saturation (`FoodData.tick`), so between the throw and the
        # landing the bar moves on its own and a value captured early is a couple
        # of tenths out. The damage event and its health update come from the same
        # handler with nothing in between, so the pair is the hit's own arithmetic.
        #
        # The last pre-hit packet is the one that counts, and it is usually
        # *younger* than the throw: a heal inside the flight is visible as a packet
        # and overwrites the earlier value as this scan walks the stream. Only when
        # the flight contained no health packet at all does the value the client
        # had at the throw stand for the bar at the hit — which it does exactly,
        # for the reason above: the bar did not move.
        before = None
        after = None
        hit_seen = False
        for pid, body in packets:
            if pid == CB_SET_HEALTH and not hit_seen:
                before = read_set_health(body)[0]
            elif pid == CB_DAMAGE_EVENT and read_damage_event(body)["type"] == DMG_ENDER_PEARL:
                hit_seen = True
            elif pid == CB_SET_HEALTH and hit_seen and after is None:
                after = read_set_health(body)[0]
        from_throw = before is None
        if from_throw:
            before = pre_throw_health
        assert after is not None, (before, after, pre_throw_health, h.health)
        assert abs((before - after) - ENDER_PEARL_DAMAGE) < 1e-4, \
            f"minecraft:ender_pearl is `Player.teleport` then a flat 5.0: " \
            f"{before} -> {after}"
        assert bot.position is not None, "the teleport must move the thrower"
        step(f"  OK the pearl teleported its thrower and charged "
             f"{before - after:.1f} ({before:.3f} -> {after:.3f}, "
             f"{'the bar did not move in flight' if from_throw else 'the bar moved on the wire'}); "
             f"now at ({bot.position[0]:.2f}, {bot.position[1]:.2f}, {bot.position[2]:.2f})")

        # --- 9. the same arrow into a *player* --------------------------------
        # `AbstractArrow.onHitEntity` runs `doPostHurtEffects(victim)` inside the
        # branch its hurt landed in, for *any* LivingEntity, and a spectral
        # arrow's answer is 200 ticks of Glowing. The mob branch of this server's
        # impact sink applies it (step 6b); a **player's** hit is queued for that
        # player's own session thread instead, and the queue carried damage,
        # push, owner and damage type and nothing else — so the effect was
        # dropped for the one victim kind a player can shoot at. A player is also
        # the one victim that *is* sent update_mob_effect (`ServerPlayer
        # .onEffectAdded`), so this shot is witnessed twice: by the victim's own
        # stream and by the shooter's, which can only see the shared-flags byte.
        step("-> join VictimBot and stand it two blocks along +x, in the air")
        other = Bot("VictimBot", port)
        wire_client.record(other, note="projectile-wire: a spectral arrow into a player")
        other.join()
        # The attribute set arrives after the chunk burst, and how long that is
        # depends on the machine, so the wait is for the *packet* rather than for
        # a wall-clock second of it.
        victim_join = list(other.join_packets)
        for _ in range(20):
            if all_of(victim_join, CB_UPDATE_ATTRIBUTES):
                break
            victim_join += drain(other, 0.4)
        attrs = all_of(victim_join, CB_UPDATE_ATTRIBUTES)
        assert attrs, \
            "the victim's join must push the attribute set (its entity id): " \
            f"{[hex(p) for p, _ in victim_join]}"
        other_eid, _ = rd_varint(attrs[-1], 0)
        assert other_eid != bot.entity_id, "the two clients must be two entities"
        # The shooter goes back into the air at y 100, where the join and step 7's
        # throws proved there is nothing to hit on the way, and the victim stands
        # two blocks along +x and says so itself: a client's position is the one
        # its own movement packet claims, and that packet is also one tick of its
        # own survival clock — which is what lets its session publish its byte.
        h.command("tp -24 100 -8")
        assert bot.position == (-24.0, 100.0, -8.0), bot.position
        feet = bot.position
        victim = (feet[0] + 2.0, feet[1], feet[2])
        other.send(SB_POSITION, struct.pack(">ddd", *victim) + bytes([1]))
        drain(other, 0.4)
        # The spectral stack is written *over* the plain one, as in 6b: the
        # ammunition decides the entity type and `Player.getProjectile` is "the
        # first arrow stack in the inventory", so only one kind may be in it.
        h.command("gamemode creative")
        h.select(bow_slot)
        bot.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", arrow_slot) + varint(8)
                 + varint(ITEMS_BY_NAME["spectral_arrow"]) + b"\x00\x00")
        h.observe(drain(bot, 0.3))
        assert h.slots[arrow_slot]["item"] == ITEMS_BY_NAME["spectral_arrow"], \
            h.slots[arrow_slot]
        assert all(s is None or s["item"] != ITEMS_BY_NAME["arrow"] for s in h.slots), \
            "the plain arrows must be gone, or 'the first arrow stack' is ambiguous"
        # A survival shot, the way a player shoots a player: nothing on the
        # impact path looks at the firer's gamemode, and the victim takes the
        # damage it would take in a fight.
        h.command("gamemode survival")
        eye = (feet[0], feet[1] + EYE_HEIGHT - SPAWN_EYE_OFFSET, feet[2])
        chest = (victim[0], victim[1] + 1.0, victim[2])
        to_victim = (chest[0] - eye[0], chest[1] - eye[1], chest[2] - eye[2])
        flat = math.hypot(to_victim[0], to_victim[2])
        h.select(bow_slot)
        use_item(bot, yaw=math.degrees(math.atan2(-to_victim[0], to_victim[2])),
                 pitch=math.degrees(-math.atan2(to_victim[1], flat)))
        h.observe(drain(bot, 0.25))
        h.observe(advance(bot, 19))  # a full draw
        release_use_item(bot)

        def struck_player(window):
            """Whether the window holds a `minecraft:arrow` damage event on the
            second client — the same test steps 6 and 6b make of their shots."""
            return any(read_damage_event(b)["entity"] == other_eid
                       and read_damage_event(b)["type"] == DMG_ARROW
                       for b in all_of(window, CB_DAMAGE_EVENT))

        # Both clocks are in play: the flight is the *world* loop's work (wall
        # time), while the queued hit is drained on the victim's own survival
        # clock — the packets the victim sends — so the loop drives the victim
        # and the shooter together rather than assuming either.
        packets = []
        for _ in range(14):
            other.send(SB_POSITION, struct.pack(">ddd", *victim) + bytes([1]))
            packets += h.observe(advance(bot, 1))
            if struck_player(packets):
                break
        packets += h.observe(advance(bot, 2))
        assert len(arrow_spawns(packets, ET_SPECTRAL_ARROW)) == 1, \
            f"the spectral shot must fire one spectral arrow: {spawn_summary(packets)}"
        assert struck_player(packets), \
            f"the spectral arrow never landed on the player: {spawn_summary(packets)}"
        step("  OK the arrow landed on the second client")
        # The victim's own stream, *waited for* rather than read for a fixed
        # window. The effect is not the shooter's damage event, and it does not
        # ride the victim's own clock either: a red check that stopped feeding
        # the victim its packets here still saw the packet arrive, so what sends
        # it is the victim's session applying the queued hit on the **world**
        # clock. A fixed 0.8 s read is therefore a duration and not a wait, and
        # under load it came back with the victim's own packets and nothing else
        # — `the victim must be sent update_mob_effect: ['0x23', '0x8', …]`,
        # seen twice, once with a diagnostic probe in the world and once with
        # none, which retired the probe as the explanation. So it is read until
        # it arrives, bounded, and it still fails loudly if it never does.
        victim_packets = drain(other, 0.3)
        for _ in range(12):
            if all_of(victim_packets, CB_UPDATE_MOB_EFFECT):
                break
            victim_packets += drain(other, 0.2)
        given = [read_mob_effect(b) for b in all_of(victim_packets, CB_UPDATE_MOB_EFFECT)]
        assert given, \
            f"the victim must be sent update_mob_effect: {[hex(p) for p, _ in victim_packets]}"
        eid, effect, amplifier, duration, flags = given[-1]
        assert (eid, effect, amplifier) == (other_eid, EFFECT_GLOWING, 0), given[-1]
        assert duration == 200, \
            f"SpectralArrow.DEFAULT_DURATION is 200 ticks, got {duration}"
        assert flags & FLAG_BLEND, f"a new effect blends in: flags {flags:#04x}"
        step(f"  OK the victim was sent effect {effect} for {duration} ticks "
             f"(flags {flags:#04x})")
        # ...and the outline both clients draw from it: the victim's own session
        # sends the byte to its own client and to every other one.
        own = shared_flags(victim_packets, other_eid)
        assert own and own[-1] & FLAG_GLOWING, \
            f"the victim's own shared flags must carry the outline: {own}"
        assert not own[-1] & FLAG_ON_FIRE, f"and nothing set it on fire: {own}"
        watched = shared_flags(packets, other_eid)
        assert watched and watched[-1] & FLAG_GLOWING, \
            f"the shooter must be sent the outline too: {watched}"
        step(f"  OK both clients read the outline off shared flags 0x{own[-1]:02x}")
        # The order is vanilla's: the damage has to land first, because
        # `doPostHurtEffects` is inside that branch.
        order = [p for p, _ in victim_packets
                 if p in (CB_DAMAGE_EVENT, CB_UPDATE_MOB_EFFECT, CB_SET_ENTITY_DATA)]
        first_effect = next(i for i, p in enumerate(order) if p == CB_UPDATE_MOB_EFFECT)
        assert any(p == CB_DAMAGE_EVENT for p in order[:first_effect]), \
            f"the effect must follow the damage: {[hex(p) for p in order]}"
        step("  OK the damage event precedes the effect on the victim's own stream")

        # --- 9b. the same shot as the *killing* blow ---------------------------
        # Session #4 applied the queued Glowing inside the `landed` branch and
        # *before* the corpse skip, because vanilla's `doPostHurtEffects` sits in
        # the branch the hurt landed in and a killing blow is in that branch too
        # — and recorded that as a reading of the decomp rather than a proof,
        # since no test drove a lethal player shot. This is that shot: the victim
        # is put on one health through its *own* session (`/damage` is
        # self-targeted, and the amount is read off the victim's own bar so the
        # next arrow kills whatever the first one took), and the second spectral
        # arrow fires into it. What a victim's stream has to show is the 200
        # ticks of Glowing *and* the death: the effect a corpse carries is not a
        # survivor's courtesy, and neither is the outline other clients draw from
        # it.
        def tick_victim(times):
            """Drive the victim's own session clock: under
            `PLUSMC_SURVIVAL_CLOCK=packet` one recognized packet from its client
            is one of its ticks, and `step_survival` decays `invulnerable_time`
            on exactly those ticks. Nothing about a player's position comes from
            the server, so a claim of the same place leaves it where it stands.
            """
            seen = []
            for _ in range(times):
                other.send(SB_POSITION, struct.pack(">ddd", *victim) + bytes([1]))
                seen += drain(other, 0.1)
            return seen

        # The victim is *still* glowing from the shot above, and a bit that has
        # not changed is not resent — `SynchedEntityData.set`'s own rule, and the
        # reason step 9's own assertion is the one that read 0x40. What this step
        # is about is a *new* outline on the tick the victim dies, so the effect
        # set is emptied first and the byte watched back down to zero: the same
        # two bits, moved twice.
        # The shot's 200 ticks decay per packet under `PLUSMC_SURVIVAL_CLOCK=
        # packet`, and the steps between the shot and here send plenty — on a
        # slow box the glow was already gone, so `clear` had nothing to remove
        # and the byte had nowhere to fall from. Re-grant it (infinite, so this
        # step owns the only expiry) before clearing: the corpse-keeps-its-
        # effect claim is the assertion just above, which read 0x40 off both
        # clients, and what this step checks is that a clear publishes the drop.
        send_chat_command(other, f"effect give {other.name} glowing infinite 0 true")
        drain(other, 0.3)
        send_chat_command(other, f"effect clear {other.name}")
        cleared = drain(other, 0.6)
        assert "Removed" in feedback(cleared), feedback(cleared)
        # A blow also leaves `invulnerable_time = 20` and only the amount *past*
        # `last_hurt` lands while it is above 10, so the blow step 9 just landed
        # has to be ten of the victim's own ticks behind us before the next one
        # counts — and the `/damage` below leaves its own ten behind it. Those
        # ticks are the victim's own (`tick_victim`), and they are also what
        # carries the byte down: the sync is the end of the victim's survival
        # tick, so nothing it *is* told about itself is published without one.
        cleared = tick_victim(12)
        cleared += drain(other, 0.3)
        flags_down = shared_flags(cleared, other_eid)
        assert flags_down and not flags_down[-1] & FLAG_GLOWING, \
            f"the cleared effect's bit must go back to zero: {flags_down}"
        # The bar is read *after* those ticks, not from the one the arrow left:
        # the food bar heals out of saturation while they run.
        bar = [read_set_health(b)
               for b in all_of(victim_packets + cleared, CB_SET_HEALTH)]
        assert bar, f"the victim's own bar must be on its stream: {bar}"
        fatal = bar[-1][0] - 1.0
        assert fatal > 0.0, bar[-1]
        send_chat_command(other, f"damage {other.name} {fatal:.2f}")
        downed = drain(other, 0.6)
        bar = [read_set_health(b) for b in all_of(downed, CB_SET_HEALTH)]
        # One health, give or take the tenths the food bar puts back between the
        # command and the arrow that follows: what matters is that the victim is
        # inside the reach of a blow that lands at all, which the arrow's own six
        # or more is.
        assert bar and 0.0 < bar[-1][0] < 4.0, \
            f"the victim must be cut down to a killing blow's reach: {bar}"
        downed += tick_victim(12)
        h.select(bow_slot)
        use_item(bot, yaw=math.degrees(math.atan2(-to_victim[0], to_victim[2])),
                 pitch=math.degrees(-math.atan2(to_victim[1], flat)))
        h.observe(drain(bot, 0.25))
        h.observe(advance(bot, 19))  # a full draw
        release_use_item(bot)
        # Driven the same way as the shot above, and still driven after the hit:
        # the victim's own byte is published on its own tick, and a corpse's tick
        # is the clock it keeps, so the packets that carry the outline are the
        # same ones that carried it while it was alive.
        killed = []
        for _ in range(14):
            other.send(SB_POSITION, struct.pack(">ddd", *victim) + bytes([1]))
            killed += h.observe(advance(bot, 1))
            if struck_player(killed):
                break
        for _ in range(6):
            other.send(SB_POSITION, struct.pack(">ddd", *victim) + bytes([1]))
            killed += h.observe(advance(bot, 1))
        killed += drain(other, 0.6)
        assert struck_player(killed), \
            f"the killing spectral arrow never landed: {spawn_summary(killed)}"
        bar = [read_set_health(b) for b in all_of(killed, CB_SET_HEALTH)]
        assert bar and bar[-1][0] == 0.0, \
            f"the blow must be the one that kills the victim: {bar}"
        given = [read_mob_effect(b) for b in all_of(killed, CB_UPDATE_MOB_EFFECT)]
        assert any(g[1] == EFFECT_GLOWING and g[3] == 200 for g in given), \
            f"a lethal arrow carries doPostHurtEffects too: {given}"
        order = [p for p, _ in killed
                 if p in (CB_DAMAGE_EVENT, CB_UPDATE_MOB_EFFECT, CB_SET_ENTITY_DATA)]
        first_effect = next(i for i, p in enumerate(order) if p == CB_UPDATE_MOB_EFFECT)
        assert any(p == CB_DAMAGE_EVENT for p in order[:first_effect]), \
            f"the damage lands first even when it is fatal: {[hex(p) for p in order]}"
        # ...and the outline, which is the *other* client's whole view of it —
        # the victim is the only one sent update_mob_effect. Step 9's live shot
        # published the byte from the end of the survival tick; a corpse's tick
        # returns before there, so this is the path that asks whether the byte
        # has an owner on it at all.
        own = shared_flags(killed, other_eid)
        assert own and own[-1] & FLAG_GLOWING, \
            f"a lethal hit's outline must reach the other clients: {own}"
        step(f"  OK the killing blow still lands effect {EFFECT_GLOWING} for 200 ticks "
             f"(health 0, outline 0x{own[-1]:02x})")
        other.close()

        bot.close()
    finally:
        if proc is not None:
            proc.terminate()
            proc.wait(timeout=5)
    print("\nPASS: projectile wire test")
    return 0


if __name__ == "__main__":
    sys.exit(main())
