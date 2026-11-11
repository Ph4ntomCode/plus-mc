#!/usr/bin/env python3
"""Integration test: Phase 3 milestone G over the real wire protocol.

Status effects and enchantment protection, driven as a client drives them:

  * `/effect give` raises update_mob_effect (0x84) with the *registry id* of the
    effect, its amplifier, its duration in ticks and the four flags — including
    the blend bit, which is set only when the effect is new
    (ServerPlayer.onEffectAdded) and clear when an instance is refreshed;
  * the same command pushes update_attributes (0x86) with the effect's own
    modifier (`minecraft:effect.speed`, ADD_MULTIPLIED_TOTAL), which is what the
    client's movement speed learns from;
  * a weaker/shorter repeat is refused outright (MobEffectInstance.update only
    ever strengthens), and `/effect clear` answers with remove_mob_effect (0x4e);
  * Resistance really turns a hit aside through the damage pipeline
    (`getDamageAfterMagicAbsorb`), 10 damage -> 6 with Resistance II;
  * `/enchant` writes the `minecraft:enchantments` component (13) and re-sends
    the slot, and Protection IV on a worn diamond chestplate lands a *reduced*
    hit on the wire: 10 -> armour 8.4 -> protection 7.056;
  * an effect that runs out sends remove_mob_effect on its own, and dying takes
    the effect set with it (the remove packet arrives before the respawn);
  * a glowing player's outline is the *shared flags* (set_entity_data index 0,
    bit 6) — the byte a mobile's effects are published through, since only the
    effect's own owner is sent update_mob_effect — and the bit leaves the byte
    with the effect; the same byte carries the invisibility bit an effect set
    raises (5) and the sneak and sprint bits a serverbound packet raises (1, 3),
    each derived the way vanilla derives it.

The survival clock *counts packets* (`PLUSMC_SURVIVAL_CLOCK=packet`): one
recognized serverbound packet is exactly one game tick, and idle wall time
contributes none. Two things follow, and both are the point. A hit's size is
only the number this test asserts when the 20-tick invulnerability window
`LivingEntity.hurtServer` opens is *spent* beforehand — `Harness.settle` does
that explicitly, where the wall clock used to expire it during the drains. And
a duration is a count: `wire_client.barrier` (a ping whose pong cannot be
written until the server has processed everything sent before it) lets the
20-tick Speed effect be asserted two-sidedly — one tick short it is still
there, on the 20th tick the removal arrives.

Run: effects_test.py /path/to/plusmc
"""
import os, re, socket, struct, subprocess, sys, tempfile, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nbt_component import chat_text  # noqa: E402
import wire_client
from wire_client import make_frame, read_packet

PROTO = 777
HOST = "127.0.0.1"

# Registry ids (generated tables: src/world/effect_table.h, item_registry.h,
# enchantment_table.h, attribute_table.h, damage_table.h).
EFFECT_SPEED = 0
EFFECT_STRENGTH = 4
EFFECT_RESISTANCE = 10
EFFECT_INVISIBILITY = 13
EFFECT_GLOWING = 23
ITEM_DIAMOND_CHESTPLATE = 1085
ITEM_SHIELD = 1446
ENCHANTMENT_PROTECTION = 28
ENCHANTMENT_FIRE_PROTECTION = 11
COMPONENT_DAMAGE = 3
COMPONENT_ENCHANTMENTS = 13
ATTR_ARMOR = 1
DMG_PLAYER_ATTACK = 34

CB_CONTAINER_SET_CONTENT = 0x12
CB_CONTAINER_SET_SLOT = 0x14
CB_DAMAGE_EVENT = 0x19
CB_PLAYER_COMBAT_KILL = 0x45
CB_RESPAWN = 0x54
CB_REMOVE_MOB_EFFECT = 0x4F
CB_SET_ENTITY_DATA = 0x65
CB_SET_HEALTH = 0x6A
CB_SYSTEM_CHAT = 0x7C
CB_UPDATE_ATTRIBUTES = 0x86
CB_UPDATE_MOB_EFFECT = 0x87
CB_KEEP_ALIVE = 0x2D

SB_CLIENT_COMMAND = 0x0C
SB_CONTAINER_CLICK = 0x12
SB_CHAT = 0x09
SB_CHAT_COMMAND = 0x07
SB_KEEP_ALIVE = 0x1C
SB_PLAYER_COMMAND = 0x2A
SB_PLAYER_INPUT = 0x2B
SB_BLOCK_DIG = 0x29
SB_USE_ITEM = 0x43
SB_SET_CARRIED_ITEM = 0x36

# ClientboundUpdateMobEffectPacket's flag bits (src/protocol/ids.h).
FLAG_AMBIENT = 1
FLAG_VISIBLE = 2
FLAG_SHOW_ICON = 4
FLAG_BLEND = 8

# EntityDataSerializers registration order (BYTE, INT, LONG, FLOAT, STRING,
# COMPONENT, OPTIONAL_COMPONENT, ITEM_STACK, BOOLEAN, ...): the ids a
# set_entity_data value can carry. BOOLEAN's codec is one byte, `true` as 1.
SER_BYTE = 0
SER_FLOAT = 3
SER_BOOLEAN = 8
SER_PARTICLES = 17

# `LivingEntity.DATA_EFFECT_PARTICLES` — the third LivingEntity value, after the
# flags (8) and the health word (9) — and the two particle types these tests see
# in it. minecraft:entity_effect's own codec is one u32 (the ARGB colour); a bare
# SimpleParticleType carries nothing at all.
EFFECT_PARTICLES_INDEX = 10
PARTICLE_ENTITY_EFFECT = 28

# `LivingEntity.DATA_EFFECT_AMBIENCE_ID` — the fourth, written beside that list
# by the same method (`areAllEffectsAmbient`).
EFFECT_AMBIENCE_INDEX = 11

# `MobEffect.getColor` for the three effects the assertions below grant, with the
# 255 alpha `MobEffect`'s own factory puts over it (`ARGB.color(ambient ? 38 :
# 255, color)`), read off src/world/effect_table.h. A particle entry is an
# effect's *colour*, not its icon: `/effect give ... true` (hideParticles) is no
# entry at all.
COLOR_INVISIBILITY = 0xF6F6F6 | 0xFF000000
COLOR_GLOWING = 0x94A061 | 0xFF000000
COLOR_SPEED = 0x33EBFF | 0xFF000000
# Entity.DATA_SHARED_FLAGS_ID: one byte, and every bit of it this server can
# derive. FLAG_ONFIRE (0) is `Entity.setSharedFlagOnFire`'s flames; 1 and 3 are
# the two bits `ServerGamePacketListenerImpl` writes from a serverbound packet
# (`handlePlayerInput` -> `setShiftKeyDown`, `handlePlayerCommand`'s
# START/STOP_SPRINTING -> `setSprinting`); 5 and 6 are the two the effect set
# answers (`LivingEntity.updateInvisibilityStatus` and `updateGlowingStatus`, the
# latter `hasEffect(MobEffects.GLOWING)`). Bits 4 (swimming) and 7 (fall flying)
# have no state to derive from here.
# `LivingEntity.DATA_LIVING_ENTITY_FLAGS`: index 8 (Entity defines 0..7), one
# byte, whose bit 0 is `isUsingItem` — set by `startUsingItem` and cleared by
# `stopUsingItem`/`LivingEntity.die`, which is what every *other* client draws a
# raised shield, a bite and a bow draw from. Bits 0x02 (which hand) and 0x04
# (the mace's auto-spin attack) have no writer here.
LIVING_ENTITY_FLAGS_INDEX = 8
FLAG_USING_ITEM = 0x01

SHARED_FLAGS_INDEX = 0
FLAG_ON_FIRE = 0x01
FLAG_SHIFT_KEY_DOWN = 0x02
FLAG_SPRINTING = 0x08
FLAG_INVISIBLE = 0x20
FLAG_GLOWING = 0x40

# Player menu slots: 0 result, 1..4 the 2x2 grid, 5..8 armour (head..feet),
# 9..35 main, 36..44 hotbar, 45 offhand.
SLOT_MENU_CHEST = 6
SLOT_MENU_HOTBAR_FIRST = 36

# Infinite duration is written as a varint of -1, i.e. the unsigned maximum.
UNSIGNED_MAX = 0xFFFFFFFF


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


# --- decoders ---------------------------------------------------------------

def read_mob_effect(body):
    """update_mob_effect: [varint entity][varint effect][varint amplifier]
    [varint duration][u8 flags], and nothing else."""
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


def read_mob_effect_removed(body):
    """remove_mob_effect: [varint entity][varint effect]."""
    pos = 0
    eid, pos = rd_varint(body, pos)
    effect, pos = rd_varint(body, pos)
    assert pos == len(body), f"remove_mob_effect: {len(body) - pos} trailing byte(s)"
    return eid, effect


def read_attributes(body):
    """update_attributes -> (entity id, {attribute id: (base, [modifiers])})."""
    eid, pos = rd_varint(body, 0)
    count, pos = rd_varint(body, pos)
    values = {}
    for _ in range(count):
        attribute, pos = rd_varint(body, pos)
        base = struct.unpack(">d", body[pos:pos + 8])[0]
        pos += 8
        modifiers, pos = rd_varint(body, pos)
        mods = []
        for _ in range(modifiers):
            name, pos = rd_string(body, pos)
            amount = struct.unpack(">d", body[pos:pos + 8])[0]
            pos += 8
            operation, pos = rd_varint(body, pos)
            mods.append((name, round(amount, 4), operation))
        values[attribute] = (base, mods)
    assert pos == len(body), f"update_attributes: {len(body) - pos} trailing byte(s)"
    return eid, values


def read_set_health(body):
    """: [f32 health][varint food][f32 saturation]."""
    health = struct.unpack(">f", body[0:4])[0]
    food, pos = rd_varint(body, 4)
    saturation = struct.unpack(">f", body[pos:pos + 4])[0]
    assert pos + 4 == len(body), f"set_health: {len(body) - pos - 4} trailing byte(s)"
    return round(health, 4), food, round(saturation, 4)


def read_stack(body, pos):
    """ItemStack.OPTIONAL_STREAM_CODEC, keeping the one component this test
    cares about: the server writes the *concrete* (undelimited) codec, so a
    component is [varint type id][value], with no length prefix. Returns
    ((item, count, [(component id, value)]) | None, new_pos)."""
    count, pos = rd_varint(body, pos)
    if count == 0:
        return None, pos
    item, pos = rd_varint(body, pos)
    # DataComponentPatch: both counts come first, then the added values, then
    # the removed ids.
    added, pos = rd_varint(body, pos)
    removed, pos = rd_varint(body, pos)
    assert removed == 0, f"the server removed {removed} component(s)"
    components = []
    for _ in range(added):
        type_id, pos = rd_varint(body, pos)
        if type_id == COMPONENT_ENCHANTMENTS:
            entries, pos = rd_varint(body, pos)
            enchanted = []
            for _ in range(entries):
                ench, pos = rd_varint(body, pos)
                level, pos = rd_varint(body, pos)
                enchanted.append((ench, level))
            components.append((type_id, tuple(enchanted)))
        elif type_id == COMPONENT_DAMAGE:
            # `DataComponents.DAMAGE`: a VAR_INT, and the worn chestplate picks
            # one up on the first hit it absorbs (LivingEntity.doHurtEquipment /
            # Player.hurtArmor wear the piece, and the slot is re-sent). The
            # *amount* is pinned by the durability model's own unit test
            # (tests/survival_test.cpp); here the value only has to decode, so
            # the assertion is that it is a positive, non-saturated number and
            # that the stack's identity did not change with it.
            damage, pos = rd_varint(body, pos)
            assert damage > 0, "a damage component on the wire is never 0"
            components.append((type_id, damage))
        else:
            # Anything else would silently mis-read the rest of the patch, which
            # is worse than failing: the decoder refuses rather than guesses.
            raise AssertionError(f"unexpected item component {type_id} on the wire")
    return (item, count, tuple(components)), pos


def decode_content(body):
    """: (window, state, [stacks], cursor)."""
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
    """: (window, state, slot, stack)."""
    window, pos = rd_varint(body, 0)
    state, pos = rd_varint(body, pos)
    slot = struct.unpack(">h", body[pos:pos + 2])[0]
    stack, pos = read_stack(body, pos + 2)
    assert pos == len(body), f"container_set_slot: {len(body) - pos} trailing byte(s)"
    return window, state, slot, stack


def hashed_stack(item, count):
    """A HashedStack: [u8 present][varint item][varint count][varint added]
    [varint removed] — a different layout from ItemStack, byte presence flag."""
    if count <= 0:
        return b"\x00"
    return bytes([1]) + varint(item) + varint(count) + varint(0) + varint(0)


def click_body(window, state, slot, button, mode, carried=(0, 0)):
    """A serverbound container_click; `carried` is what the client believes it
    holds, which the server checks before applying the click."""
    return (varint(window) + varint(state) + struct.pack(">h", slot) + bytes([button])
            + varint(mode) + varint(0) + hashed_stack(*carried))


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
        # Everything `join` reads after the login handshake, kept rather than
        # dropped: the join burst is where a client learns what it was not
        # present for (another player's outline or flames, say), and it is
        # over before the caller can drain.
        self.join_packets = []

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
            pid, body = self.recv()
            self.join_packets.append((pid, body))
            if pid == 0x32:
                break
        for _ in range(400):
            pid, body = self.recv()
            self.join_packets.append((pid, body))
            if pid == 0x2D:
                self.send(0x1C, body[:8])
            elif pid == 0x0B:
                batch, _ = rd_varint(body)
                self.send(0x0B, varint(batch))
                break
        return True


def drain(bot, timeout=1.5):
    """Every packet the reader thread queued in the next `timeout`
    seconds."""
    # A reply window ends when the server has been quiet for `quiet` seconds
    # rather than always burning the whole window; `timeout` stays the bound a
    # missing reply is judged against. Requires the reader (started after each
    # join below).
    return wire_client.drain_quiet(bot, quiet=0.3, limit=timeout)
def all_of(packets, pid):
    return [b for p, b in packets if p == pid]


def find(packets, pid):
    for p, body in packets:
        if p == pid:
            return body
    return None


def effects_of(packets):
    return [read_mob_effect(b) for b in all_of(packets, CB_UPDATE_MOB_EFFECT)]


def removals(packets):
    return [read_mob_effect_removed(b) for b in all_of(packets, CB_REMOVE_MOB_EFFECT)]


def entity_data_entries(packets, eid):
    """(index, serializer, value) for every entry a set_entity_data carried about
    `eid`. BYTE decodes to an int, FLOAT to a float, BOOLEAN to 0/1, and
    PARTICLES to a list of (particle type id, color or None) — the codec of a
    particle option is its registry id followed by that type's own payload,
    which only minecraft:entity_effect has (the ARGB colour).
    """
    for body in all_of(packets, CB_SET_ENTITY_DATA):
        first, pos = rd_varint(body, 0)
        if first != eid:
            continue
        while True:
            index = body[pos]
            pos += 1
            if index == 0xFF:
                break
            serializer, pos = rd_varint(body, pos)
            if serializer == SER_BYTE:
                value = body[pos]
                pos += 1
            elif serializer == SER_BOOLEAN:
                value = body[pos]
                pos += 1
            elif serializer == SER_FLOAT:
                value = struct.unpack(">f", body[pos:pos + 4])[0]
                pos += 4
            elif serializer == SER_PARTICLES:
                count, pos = rd_varint(body, pos)
                value = []
                for _ in range(count):
                    ptype, pos = rd_varint(body, pos)
                    color = None
                    if ptype == PARTICLE_ENTITY_EFFECT:
                        color = int.from_bytes(body[pos:pos + 4], "big")
                        pos += 4
                    value.append((ptype, color))
            else:
                raise AssertionError(f"entity data index {index}: serializer {serializer}")
            yield index, serializer, value
        assert pos == len(body), f"set_entity_data: {len(body) - pos} trailing byte(s)"


def entity_data_byte(packets, eid, index):
    """Every BYTE `set_entity_data` carried for `eid` at synched-data `index`."""
    return [v for i, s, v in entity_data_entries(packets, eid)
            if i == index and s == SER_BYTE]


def effect_particles(packets, eid):
    """Every `LivingEntity.DATA_EFFECT_PARTICLES` list (index 10) a
    set_entity_data carried for `eid`, each as a list of (particle type, color)
    in the server's own order — the effect set is kept ordered by effect id, so
    a list is a stable thing to assert on.

    This is the only packet that carries a player's status-effect swirls to
    anyone but that player: update_mob_effect goes to the effect's own owner
    (`ServerPlayer.onEffectAdded`), so before this the *other* clients were told
    nothing at all about a player's effects.
    """
    return [v for i, s, v in entity_data_entries(packets, eid)
            if i == EFFECT_PARTICLES_INDEX and s == SER_PARTICLES]


def effect_ambience(packets, eid):
    """Every `LivingEntity.DATA_EFFECT_AMBIENCE_ID` value (index 11, BOOLEAN) a
    set_entity_data carried for `eid`, as 0 or 1.

    `updateSynchronizedMobEffectParticles` writes this beside the particle list —
    `areAllEffectsAmbient`'s answer, i.e. true when *no visible, non-ambient*
    effect is in the set — and a client multiplies its swirl-rate divisor by 5
    when it is true (`LivingEntity.tick`). It is a value of its own rather than a
    reading of the list: an all-hidden set has an empty list and says true.
    Vanilla writes it only while the set is non-empty (`removeEffectParticles`
    clears the list alone), so an empty set sends no value at all and a client
    keeps the last one.
    """
    return [v for i, s, v in entity_data_entries(packets, eid)
            if i == EFFECT_AMBIENCE_INDEX and s == SER_BOOLEAN]


def shared_flags(packets, eid):
    """Every shared-flags byte (data index 0, BYTE) set_entity_data carried for
    `eid`.

    This byte is how anyone but the effect's own owner learns of an effect:
    ClientboundUpdateMobEffectPacket goes to the effect's own player (and their
    passengers), and a mobile's outline is flag 6 of these shared flags. The bits
    share the one byte, so a sender that carries one and assumes the others are
    clear erases them. `SynchedEntityData.set` writes on change only, so the
    server is the one that remembers what it last sent.
    """
    return entity_data_byte(packets, eid, SHARED_FLAGS_INDEX)


def use_flags(packets, eid):
    """Every `LivingEntity.DATA_LIVING_ENTITY_FLAGS` byte (data index 8) carried
    for `eid`: bit 0 is up while a use is running (`isUsingItem`).
    """
    return entity_data_byte(packets, eid, LIVING_ENTITY_FLAGS_INDEX)


def latest_attributes(packets):
    bodies = all_of(packets, CB_UPDATE_ATTRIBUTES)
    return read_attributes(bodies[-1]) if bodies else (None, {})


def speed_modifiers(packets):
    """The `minecraft:effect.speed` modifiers on movement_speed in the last
    attribute snapshot of the drain."""
    _, values = latest_attributes(packets)
    return [m for m in values.get(26, (0.0, []))[1] if m[0].startswith("minecraft:effect.")]


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


def health_around_damage(packets):
    """(health before the hit, health after) from a drain around one hit.

    The order inside the drain is the server's: the tick batch (and any
    regeneration it pays for) first, then the damage_event, then the health
    packet the hit raised. The hit is therefore picked out by its position
    rather than by being the last set_health in the list.
    """
    before = after = None
    seen_event = False
    for pid, body in packets:
        if pid == CB_DAMAGE_EVENT:
            seen_event = True
        elif pid == CB_SET_HEALTH:
            if seen_event:
                if after is None:
                    after = read_set_health(body)
            else:
                before = read_set_health(body)
    return before, after


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
    """The chat-command/drain/health-tracking plumbing every phase shares."""

    def __init__(self, bot):
        self.bot = bot
        self.last_health = None
        self.state = None  # the player menu's state id

    def command(self, text, timeout=1.5):
        step(f"-> /{text}")
        send_chat_command(self.bot, text)
        packets = drain(self.bot, timeout)
        healths = [read_set_health(b) for b in all_of(packets, CB_SET_HEALTH)]
        if healths:
            self.last_health = healths[-1][0]
        contents = all_of(packets, CB_CONTAINER_SET_CONTENT)
        if contents:
            _, self.state, _, _ = decode_content(contents[-1])
        for body in all_of(packets, CB_CONTAINER_SET_SLOT):
            _, state, _, _ = decode_set_slot(body)
            self.state = state
        return packets

    def click(self, slot, carried=(0, 0)):
        self.bot.send(SB_CONTAINER_CLICK,
                      click_body(0, self.state, slot, 0, 0, carried))
        packets = drain(self.bot, 0.8)
        for body in all_of(packets, CB_CONTAINER_SET_SLOT):
            _, state, _, _ = decode_set_slot(body)
            self.state = state
        return packets

    def settle(self, ticks=20):
        """Spend `ticks` survival ticks and fold what they synced into this
        harness's view of the player, exactly as `command` does for a drain.

        Under the packet clock this is how the 20-tick invulnerability window
        `LivingEntity.hurtServer` opens on a *hit* gets spent before the next
        one: inside it a second hit only lands for the part that exceeds the
        first's raw damage, which would quietly turn "Resistance II takes 40%
        off" into a statement about the window. The wall clock used to expire it
        during the drains between two steps; the ticks are the test's now, so
        they are spent on purpose.
        """
        packets = wire_client.barrier(self.bot, ticks)
        healths = [read_set_health(b) for b in all_of(packets, CB_SET_HEALTH)]
        if healths:
            self.last_health = healths[-1][0]
        return packets

    def hit(self, damage=10, type_name="player_attack"):
        """One /damage and the *delta* it took off the health bar, measured
        across the damage_event so a regenerating player does not confuse it.

        The window is spent first, so the delta is this hit and not what is left
        of it after the previous one.
        """
        self.settle(20)
        # The health before the command ran: `command` below folds this drain's
        # own set_health back into the tracker, so the *prior* value has to be
        # taken first.
        prior = self.last_health
        packets = self.command(f"damage {self.bot.name} {damage} {type_name}")
        assert "Applied" in feedback(packets), f"the hit was refused: {feedback(packets)}"
        before, after = health_around_damage(packets)
        assert after is not None, \
            f"the hit must report health: {[hex(p) for p, _ in packets]}"
        from_health = before[0] if before is not None else prior
        self.last_health = after[0]
        return round(from_health - after[0], 4), packets


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-effects-")
    env = dict(os.environ)
    # Only this test's own packets may advance the survival clock: a keep-alive
    # every 60 s is the clamped maximum, and the day cycle is frozen so nothing
    # else ticks. The clock counts *packets* (one tick each, no wall-time
    # catch-up), so an effect's duration and the invulnerability window between
    # two hits are the packets the test sends — not how long it spent draining.
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"

    proc = None
    try:
        step(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", "127.0.0.1", "--port", "0", "--motd", "effects test",
             "--offline-mode", "--gamemode", "0", "--data", data_root, "--world", world,
             "--no-flat-world"], wait_listening, env=env)

        bot = Bot("EffBot", port)
        wire_client.record(bot, note="effects-wire: /effect, /enchant, the expiry countdown")
        bot.join()
        bot.start_reader()
        h = Harness(bot)
        join_packets = drain(bot, 1.2)
        assert not all_of(join_packets, CB_UPDATE_MOB_EFFECT), \
            "a fresh join has no effects to announce"
        healths = [read_set_health(b) for b in all_of(join_packets, CB_SET_HEALTH)]
        assert healths and healths[-1] == (20.0, 20, 5.0), f"join health: {healths}"
        h.last_health = healths[-1][0]
        joined_eid, values = latest_attributes(join_packets)
        assert joined_eid is not None, "the join must push the attribute set"
        bot.entity_id = joined_eid
        assert values[ATTR_ARMOR][1] == [], f"a fresh player has no armour: {values}"
        step("  OK join: 20/20/5, no effects, no armour")

        # --- 1. /effect give: the packet, the flags and the attribute --------
        step("-> /effect give EffBot speed 5 1")
        packets = h.command("effect give EffBot speed 5 1")
        given = effects_of(packets)
        assert given, f"no update_mob_effect: {[hex(p) for p, _ in packets]}"
        eid, effect, amplifier, duration, flags = given[-1]
        assert eid == bot.entity_id and effect == EFFECT_SPEED, given[-1]
        assert amplifier == 1, f"Speed II is amplifier 1, got {amplifier}"
        # 5 seconds of a non-instantaneous effect is 5 * 20 ticks.
        assert duration == 100, f"5 seconds is 100 ticks, got {duration}"
        assert flags == (FLAG_VISIBLE | FLAG_SHOW_ICON | FLAG_BLEND), \
            f"a *new* effect blends in and is visible: flags {flags:#04x}"
        # ...and the same command pushed the effect's own attribute modifier.
        modifiers = speed_modifiers(packets)
        assert modifiers == [("minecraft:effect.speed", 0.4, 2)], \
            f"Speed II is +40% ADD_MULTIPLIED_TOTAL: {modifiers}"
        assert "for 5 seconds" in feedback(packets), feedback(packets)
        step(f"  OK effect {effect} amp {amplifier} dur {duration} flags {flags:#04x}, "
             f"movement_speed {modifiers}")

        # --- 2. a repeat is an *update*, not a new effect --------------------
        step("-> /effect give EffBot speed 10 1 (the same effect again)")
        packets = h.command("effect give EffBot speed 10 1")
        updated = effects_of(packets)
        assert len(updated) == 1, f"one update packet, got {updated}"
        _, effect, amplifier, duration, flags = updated[0]
        assert (effect, amplifier, duration) == (EFFECT_SPEED, 1, 200), updated[0]
        assert flags & FLAG_BLEND == 0, \
            f"onEffectUpdated clears the blend bit: flags {flags:#04x}"
        step(f"  OK refreshed to dur {duration} with flags {flags:#04x} (no blend)")

        # --- 3. MobEffectInstance.update only ever strengthens ----------------
        step("-> /effect give EffBot speed 10 0 (weaker: refused)")
        packets = h.command("effect give EffBot speed 10 0")
        assert not effects_of(packets), f"a weaker repeat must not be applied: {packets}"
        assert "Unable to apply effect" in feedback(packets), feedback(packets)
        step("  OK the weaker repeat was refused")

        # --- 4. hideParticles -------------------------------------------------
        step("-> /effect give EffBot speed 5 1 true (hideParticles)")
        packets = h.command("effect give EffBot speed 5 1 true")
        updated = effects_of(packets)
        assert updated, f"the visibility change must be announced: {packets}"
        _, effect, _, _, flags = updated[0]
        assert effect == EFFECT_SPEED, updated[0]
        assert flags == 0, \
            f"hideParticles clears visible *and* show_icon: flags {flags:#04x}"
        step(f"  OK flags {flags:#04x} with the particles hidden")

        # --- 5. /effect clear <effect> ---------------------------------------
        step("-> /effect clear EffBot speed")
        packets = h.command("effect clear EffBot speed")
        assert removals(packets) == [(bot.entity_id, EFFECT_SPEED)], removals(packets)
        assert speed_modifiers(packets) == [], \
            "clearing the effect must take its attribute modifier with it"
        step("  OK remove_mob_effect(0) and the modifier is gone")

        step("-> /effect clear EffBot speed (nothing to clear)")
        packets = h.command("effect clear EffBot speed")
        assert not removals(packets), f"nothing to remove: {packets}"
        assert "does not have" in feedback(packets), feedback(packets)

        step("-> /effect clear EffBot (nothing at all)")
        packets = h.command("effect clear EffBot")
        assert "There are no effects to remove" in feedback(packets), feedback(packets)
        step("  OK both refusals")

        # --- 6. Resistance through the damage pipeline ------------------------
        step("-> /effect give EffBot resistance 10 1")
        packets = h.command("effect give EffBot resistance 10 1")
        given = effects_of(packets)
        assert given and given[-1][1] == EFFECT_RESISTANCE and given[-1][2] == 1, given
        step("-> /damage EffBot 10 player_attack under Resistance II")
        landed, packets = h.hit(10)
        # LivingEntity.getDamageAfterMagicAbsorb: (25 - (amplifier + 1) * 5) / 25
        # = 0.6 at Resistance II, so 10 damage lands as 6.
        assert abs(landed - 6.0) < 0.001, f"Resistance II must take 40% off: {landed}"
        step(f"  OK Resistance II turned 10 damage into {landed}")

        packets = h.command("effect clear EffBot")
        assert removals(packets) == [(bot.entity_id, EFFECT_RESISTANCE)], removals(packets)
        step("  OK cleared the Resistance effect")

        # --- 7. /enchant writes the component and re-sends the slot ----------
        step("-> /give a diamond chestplate, then /enchant protection 4")
        h.command("give EffBot minecraft:diamond_chestplate 1")
        packets = h.command("enchant EffBot protection 4")
        sets = all_of(packets, CB_CONTAINER_SET_SLOT)
        assert sets, f"/enchant must re-send the slot: {[hex(p) for p, _ in packets]}"
        window, _, slot, stack = decode_set_slot(sets[-1])
        assert window == 0 and slot == SLOT_MENU_HOTBAR_FIRST, (window, slot)
        assert stack == (ITEM_DIAMOND_CHESTPLATE, 1,
                         ((COMPONENT_ENCHANTMENTS,
                           ((ENCHANTMENT_PROTECTION, 4),)),)), stack
        step(f"  OK slot 36 carries the enchantments component: {stack[2]}")

        # The three refusals, in vanilla's order.
        step("-> re-enchanting with the same enchantment is incompatible")
        packets = h.command("enchant EffBot protection 4")
        assert "cannot support that enchantment" in feedback(packets), feedback(packets)
        assert not all_of(packets, CB_CONTAINER_SET_SLOT), \
            "a refused enchant must not touch the slot"

        step("-> /enchant fire_protection 4 (exclusive with Protection)")
        packets = h.command("enchant EffBot fire_protection 4")
        assert "cannot support that enchantment" in feedback(packets), feedback(packets)

        step("-> /enchant protection 5 (past its max level)")
        packets = h.command("enchant EffBot protection 5")
        assert "higher than the maximum level" in feedback(packets), feedback(packets)
        step("  OK all three refusals")

        # --- 8. Protection IV on a worn chestplate really turns a hit aside --
        step("-> click the chestplate into the chest slot")
        h.click(SLOT_MENU_HOTBAR_FIRST)
        packets = h.click(SLOT_MENU_CHEST, (ITEM_DIAMOND_CHESTPLATE, 1))
        _, values = latest_attributes(packets)
        assert values[ATTR_ARMOR][1] == [("minecraft:armor.chest", 8.0, 0)], values[ATTR_ARMOR]
        step(f"  OK armour attribute: {values[ATTR_ARMOR][1]}")

        step("-> /damage EffBot 10 player_attack (armour 8 + Protection IV)")
        landed, packets = h.hit(10)
        # CombatRules.getDamageAfterAbsorb: 10 * (1 - 4/25) = 8.4 through armour,
        # then CombatRules.getDamageAfterMagicAbsorb with Protection IV's 1 + 1 *
        # (4 - 1) = 4 points: 8.4 * (1 - 4/25) = 7.056.
        assert abs(landed - 7.056) < 0.01, \
            f"armour 8 then Protection IV must land 7.056, got {landed}"
        step(f"  OK armour and Protection IV turned 10 damage into {landed}")

        # --- 9. an effect that runs out reports itself -----------------------
        # `/effect give EffBot speed 1` is 20 ticks, and `MobEffectInstance
        # .tickServer` spends one of them a tick and reports the expiry on the
        # tick the countdown reaches zero — so the duration is a count this test
        # can bound on both sides rather than a window it hopes was long enough.
        step("-> /effect give EffBot speed 1 (20 ticks), then 19, then 1")
        packets = h.command("effect give EffBot speed 1")
        assert effects_of(packets)[-1][3] == 20, effects_of(packets)
        early = wire_client.barrier(bot, 19)
        assert (bot.entity_id, EFFECT_SPEED) not in removals(early), \
            f"the effect is still running at 19 of its 20 ticks: {removals(early)}"
        assert speed_modifiers(early) == [], \
            "and no modifier packet has left with it"
        step("  OK still applied one tick short of its 20")
        packets = wire_client.barrier(bot, 1)
        assert (bot.entity_id, EFFECT_SPEED) in removals(packets), \
            f"an expired effect must be removed on the wire: {removals(packets)}"
        assert speed_modifiers(packets) == [], "the modifier leaves with the effect"
        step("  OK the effect expired on the 20th tick and remove_mob_effect arrived")

        # --- 9b. Glowing is the shared flags' bit 6 --------------------------
        # Ten seconds is 200 ticks, `SpectralArrow.DEFAULT_DURATION` — the arrow
        # that grants it. There is no update_mob_effect for anyone but the
        # effect's own owner, so a glowing player's outline is `LivingEntity
        # .updateGlowingStatus`'s `setSharedFlag(6, hasEffect(GLOWING))` — the
        # shared-flags byte, the *same* byte as the flame bit. Before this, both
        # of this player's senders passed only `on_fire`, so the outline never
        # appeared and a flame-flag resend could erase one.
        step("-> /effect give EffBot glowing 10 (200 ticks)")
        packets = h.command("effect give EffBot glowing 10")
        given = effects_of(packets)
        assert given and given[-1][1] == EFFECT_GLOWING and given[-1][3] == 200, given
        packets += h.settle(1)
        flags = shared_flags(packets, bot.entity_id)
        assert flags and flags[-1] & FLAG_GLOWING, \
            f"a glowing player's shared flags must carry bit 6: {flags}"
        step(f"  OK the shared flags byte is 0x{flags[-1]:02x} with the effect on")

        step("-> /effect clear EffBot glowing")
        packets = h.command("effect clear EffBot glowing")
        assert (bot.entity_id, EFFECT_GLOWING) in removals(packets), removals(packets)
        packets += h.settle(1)
        flags = shared_flags(packets, bot.entity_id)
        assert flags and not flags[-1] & FLAG_GLOWING, \
            f"the bit must leave with the effect: {flags}"
        assert not flags[-1] & FLAG_ON_FIRE, \
            f"and a glowing player is not a burning one: {flags}"
        step(f"  OK the byte went back to 0x{flags[-1]:02x} with the effect")

        # --- 9d. the byte's other three derivable bits -----------------------
        # The one byte carries every bit this server can derive, and the three
        # below are the ones it could already *track* and never published. Two
        # are written by vanilla from a serverbound packet —
        # `ServerGamePacketListenerImpl.handlePlayerInput` -> `setShiftKeyDown`
        # (flag 1) and `handlePlayerCommand`'s START/STOP_SPRINTING ->
        # `LivingEntity.setSprinting` (flag 3) — and this server already uses
        # both states for its own reasons (the sneak bit tells "open the chest"
        # from "place a block against it"; the sprint bit decides whether moving
        # costs exhaustion). The third is the effect set's second answer:
        # `LivingEntity.updateInvisibilityStatus` is
        # `setInvisible(hasEffect(MobEffects.INVISIBILITY))` (flag 5), and
        # `/effect` accepts that effect name, so the bit has to follow the set
        # exactly as the glow bit does.
        step("-> /effect give EffBot invisibility 10 (200 ticks)")
        packets = h.command("effect give EffBot invisibility 10")
        given = effects_of(packets)
        assert given and given[-1][1] == EFFECT_INVISIBILITY and given[-1][3] == 200, given
        packets += h.settle(1)
        flags = shared_flags(packets, bot.entity_id)
        assert flags and flags[-1] & FLAG_INVISIBLE, \
            f"an invisible player's shared flags must carry bit 5: {flags}"
        step(f"  OK the byte is 0x{flags[-1]:02x} with the effect on")

        step("-> /effect clear EffBot invisibility")
        packets = h.command("effect clear EffBot invisibility")
        assert (bot.entity_id, EFFECT_INVISIBILITY) in removals(packets), removals(packets)
        packets += h.settle(1)
        flags = shared_flags(packets, bot.entity_id)
        assert flags and not flags[-1] & FLAG_INVISIBLE, \
            f"the bit must leave with the effect: {flags}"
        step(f"  OK the byte went back to 0x{flags[-1]:02x}")

        # The two packet-driven bits. Each is sent as the packet the vanilla
        # client sends when the state changes, then read back off the wire; the
        # tick that publishes the change is the *next* packet, because this
        # server's session ticks before it dispatches a packet.
        step("-> player_command START_SPRINTING (1), then STOP_SPRINTING (2)")
        bot.send(SB_PLAYER_COMMAND, varint(bot.entity_id) + varint(1) + varint(0))
        flags = shared_flags(wire_client.barrier(bot, 1), bot.entity_id)
        assert flags and flags[-1] & FLAG_SPRINTING, \
            f"a sprinting player's shared flags must carry bit 3: {flags}"
        step(f"  OK the byte is 0x{flags[-1]:02x} while sprinting")
        bot.send(SB_PLAYER_COMMAND, varint(bot.entity_id) + varint(2) + varint(0))
        flags = shared_flags(wire_client.barrier(bot, 1), bot.entity_id)
        assert flags and not flags[-1] & FLAG_SPRINTING, \
            f"and STOP_SPRINTING takes it down: {flags}"
        step(f"  OK the byte is 0x{flags[-1]:02x} after stopping")

        step("-> player_input with sneak held (0x20), then released")
        bot.send(SB_PLAYER_INPUT, bytes([0x20]))
        flags = shared_flags(wire_client.barrier(bot, 1), bot.entity_id)
        assert flags and flags[-1] & FLAG_SHIFT_KEY_DOWN, \
            f"a sneaking player's shared flags must carry bit 1: {flags}"
        step(f"  OK the byte is 0x{flags[-1]:02x} while sneaking")
        bot.send(SB_PLAYER_INPUT, bytes([0x00]))
        flags = shared_flags(wire_client.barrier(bot, 1), bot.entity_id)
        assert flags and not flags[-1] & FLAG_SHIFT_KEY_DOWN, \
            f"and letting it go takes the bit down: {flags}"
        step(f"  OK the byte is 0x{flags[-1]:02x} standing")

        # --- 9c. every *other* client draws this player from the same byte ---
        # update_mob_effect goes to the effect's own owner alone, so the only way
        # one player is drawn with another's outline (or flames) is the shared
        # flags — and vanilla's entity tracker flushes them to everyone tracking
        # the player. This server wrote the byte to its sender only, so a
        # burning or glowing player was invisible as such to everyone else.
        step("-> /effect give EffBot glowing 10 and invisibility, then join OtherBot")
        packets = h.command("effect give EffBot glowing 10")
        assert effects_of(packets)[-1][1] == EFFECT_GLOWING, effects_of(packets)
        packets = h.command("effect give EffBot invisibility 10")
        assert effects_of(packets)[-1][1] == EFFECT_INVISIBILITY, effects_of(packets)
        h.settle(1)  # the tick whose end reads the new effect set
        other = Bot("OtherBot", port)
        wire_client.record(other, note="effects-wire: the second client's view of EffBot")
        other.join()
        other.start_reader()
        # A later change never reaches a client that was not connected yet, so
        # what this newcomer has is the join burst: vanilla's
        # `ServerEntity.sendPairingData` packs the values that differ from their
        # defaults, so a player who is burning or glowing when someone joins is
        # drawn that way at once rather than waiting for a byte change.
        flags = shared_flags(other.join_packets + drain(other, 1.5), bot.entity_id)
        assert flags and flags[-1] & FLAG_GLOWING, \
            f"a joining client must be sent the outline: {flags}"
        # ...and the whole byte rather than the two bits this server used to pack
        # by hand: the invisibility bit is in the registry's copy of it too.
        assert flags[-1] & FLAG_INVISIBLE, \
            f"and the invisibility bit with it: {flags}"
        step(f"  OK OtherBot was sent 0x{flags[-1]:02x} for EffBot in its join")

        # ...and every later change goes to every client, not only to its sender:
        # one byte holds one value, so it is written to the whole list (the sender
        # has just had it). Before this, this server wrote the byte to its sender
        # alone, so nobody else could see a burning or glowing player at all.
        step("-> /effect clear EffBot glowing")
        packets = h.command("effect clear EffBot glowing")
        assert (bot.entity_id, EFFECT_GLOWING) in removals(packets), removals(packets)
        h.settle(1)
        flags = shared_flags(drain(other, 1.5), bot.entity_id)
        assert flags and not flags[-1] & FLAG_GLOWING, \
            f"and the change reaches them: {flags}"
        step(f"  OK OtherBot was sent 0x{flags[-1]:02x} when the effect left")

        step("-> /effect give EffBot glowing 10 again")
        packets = h.command("effect give EffBot glowing 10")
        assert effects_of(packets)[-1][1] == EFFECT_GLOWING, effects_of(packets)
        h.settle(1)
        flags = shared_flags(drain(other, 1.5), bot.entity_id)
        assert flags and flags[-1] & FLAG_GLOWING, \
            f"and the change back reaches them: {flags}"
        step(f"  OK OtherBot was sent 0x{flags[-1]:02x} when it came back")

        step("-> /effect clear EffBot invisibility")
        packets = h.command("effect clear EffBot invisibility")
        assert (bot.entity_id, EFFECT_INVISIBILITY) in removals(packets), removals(packets)
        h.settle(1)
        flags = shared_flags(drain(other, 1.5), bot.entity_id)
        assert flags and not flags[-1] & FLAG_INVISIBLE and flags[-1] & FLAG_GLOWING, \
            f"one bit leaving the byte leaves the other where it is: {flags}"
        step(f"  OK OtherBot was sent 0x{flags[-1]:02x} with only the outline left")

        # --- 9d. the swirls themselves, which are a *different* packet ------ -
        # The shared flags carry the glowing outline and the invisibility bit,
        # and no particle at all. What a client actually draws a player's status
        # effects with is `LivingEntity.DATA_EFFECT_PARTICLES` (index 10), set by
        # `updateSynchronizedMobEffectParticles` and sent by the entity tracker
        # to everyone tracking the player — `sendToTrackingPlayersAndSelf`, which
        # is this server's `publish_player_effects`. update_mob_effect is
        # `ServerPlayer.onEffectAdded`'s and goes to the effect's own owner
        # alone, so before this every effect was invisible on every other client,
        # and a newcomer joined to players with nothing to tell it any of them
        # had an effect at all.
        #
        # EffBot stands with Glowing alone (9c took the invisibility off), but
        # OtherBot's *join burst* was read while both were up — a later change
        # never reaches a client that was not there for it — so that burst is
        # where the pairing half is, and it is `Bot.join`'s own packets
        # (`join_packets`) that hold it. The effect set is kept ordered by effect
        # id, so Invisibility (13) comes before Glowing (23) and the list is a
        # stable thing to assert on.
        step("-> the effect particles OtherBot was paired with, and then sent")
        paired = effect_particles(other.join_packets, bot.entity_id)
        assert paired and paired[-1] == [
            (PARTICLE_ENTITY_EFFECT, COLOR_INVISIBILITY),
            (PARTICLE_ENTITY_EFFECT, COLOR_GLOWING)], \
            f"a joining client must be told which swirls to draw: {paired}"
        step(f"  OK the join burst carried both: {paired[-1]}")

        # ...and every later change goes to them too, because the list is one
        # value and the ticker writes it to the whole list. The two effects are
        # one entry each of one list, so *both* are always carried: this is not a
        # per-effect packet.
        step("-> /effect give EffBot speed 5 1")
        packets = h.command("effect give EffBot speed 5 1")
        assert effects_of(packets)[-1][1] == EFFECT_SPEED, effects_of(packets)
        h.settle(1)
        sent = effect_particles(drain(other, 1.5), bot.entity_id)
        assert sent and sent[-1] == [(PARTICLE_ENTITY_EFFECT, COLOR_SPEED),
                                     (PARTICLE_ENTITY_EFFECT, COLOR_GLOWING)], \
            f"a change must reach the other client's list: {sent}"
        step(f"  OK OtherBot's list followed the change: {sent[-1]}")

        # ...one effect leaving a set is a change to the same one list, so it is
        # the same send: the surviving entry comes again rather than being left
        # where it was.
        step("-> /effect clear EffBot speed")
        h.command("effect clear EffBot speed")
        h.settle(1)
        sent = effect_particles(drain(other, 1.5), bot.entity_id)
        assert sent and sent[-1] == [(PARTICLE_ENTITY_EFFECT, COLOR_GLOWING)], \
            f"one effect leaving must republish the list: {sent}"
        step(f"  OK OtherBot's list dropped to the survivor: {sent[-1]}")

        # ...and the *empty* list is a value, not an absence: it is exactly how
        # the last swirl is taken down (`LivingEntity.removeEffectParticles`
        # writes `List.of()`), so a set that empties has to send it — a client
        # left holding the old list goes on drawing a player's effects after the
        # server has stopped applying them.
        step("-> /effect clear EffBot")
        h.command("effect clear EffBot")
        h.settle(1)
        sent = effect_particles(drain(other, 1.5), bot.entity_id)
        assert sent and sent[-1] == [], \
            f"clearing the last effect must take the swirls down: {sent}"
        step("  OK the empty list took the last swirl down")

        # Leave the fixture where step 10 expects it: Glowing standing at the
        # duration 9c left it (200 ticks), so that step 10's `glowing 30` is an
        # *extension* — a change, which is what makes its packet appear — while
        # the shared byte's glow bit stays where it is.
        h.command("effect give EffBot glowing 10")
        h.settle(1)
        drain(other, 1.5)

        # --- 9e. the target forms: /damage and /effect both land --------------
        # A command that names another player addresses state on that player's
        # *own* session, so the hit and the effect are queued for the target's
        # tick rather than applied here (`tests/commands_test.py` owns the whole
        # family: /tp, /gamemode, /kill, /damage, /give, /xp, /effect and
        # /enchant with both clients driven). One packet per tick is what a real
        # client sends, and what applies the queue.
        step("-> /damage OtherBot 5 and /effect give OtherBot speed 10 both land")
        # The target's health before the hit, read the same way the step reads
        # it after, so a player who took damage earlier cannot pass for a hit
        # that never landed.
        wire_client.barrier(other, 20)
        before = [read_set_health(b)[0] for b in all_of(drain(other, 1.5), CB_SET_HEALTH)]
        baseline = before[-1] if before else 20.0
        packets = h.command("damage OtherBot 5")
        # The barrier's own return is where the tick's packets land; a drain
        # after it would be reading the silence that follows the pong.
        other_packets = wire_client.barrier(other, 20) + drain(other, 1.0)
        healths = [read_set_health(b)[0] for b in all_of(other_packets, CB_SET_HEALTH)]
        assert healths and healths[-1] < baseline, \
            f"a cross-session /damage must reach the target: {baseline} -> {healths}"
        # The effect set is queued the same way: the action carries the effect,
        # its duration and its visibility, and the target's tick raises the same
        # `update_mob_effect` the self form does.
        packets = h.command("effect give OtherBot speed 10")
        other_packets = wire_client.barrier(other, 20) + drain(other, 1.0)
        effects = all_of(other_packets, CB_UPDATE_MOB_EFFECT)
        assert effects, f"a cross-session /effect must reach the target: {[hex(p) for p, _ in other_packets]}"
        step("  OK /damage and /effect reached the target")
        other.close()

        # --- 9f. the ambience byte beside that list (index 11) ----------------
        # `updateSynchronizedMobEffectParticles` writes a *second* value in the
        # same breath as the list: `LivingEntity.DATA_EFFECT_AMBIENCE_ID`, a
        # boolean that is `areAllEffectsAmbient`'s answer — and what a client
        # multiplies its swirl-rate divisor by 5 with (`LivingEntity.tick`:
        # `nextInt((isInvisible() ? 15 : 4) * (ambient ? 5 : 1))`).
        #
        # The predicate is not "every effect is ambient": an instance that is not
        # *visible* is skipped, so a set whose effects are all hidden answers true
        # exactly as an ambient one does. That is what makes it a value of its
        # own — an all-hidden set has an *empty* list, so nothing in the list
        # above says which answer this is — and it is also the half that is
        # reachable here: this server can hide an effect's particles
        # (`/effect give ... true`) but has no source of an *ambient* instance at
        # all (no beacon, no conduit, and every generated consume row is
        # `ambient = false`). What these assertions wire is vanilla's predicate
        # over the sets this server can produce; an ambient instance would take
        # exactly this path, through the same `areAllEffectsAmbient` call its
        # particle colour's alpha already goes through.
        #
        # OtherBot is gone (9e closed it), so the observer is a fresh client
        # instead — which is also the more useful shape for this value: PairBot
        # joins *after* the byte has said yes, so its pairing burst is the only
        # thing that can tell it (the accessor's default is false), and it is
        # still connected for the change that takes the byte back down.
        step("-> /effect give EffBot speed 5 1 true, with nothing else up")
        h.command("effect clear EffBot")
        packets = h.command("effect give EffBot speed 5 1 true")
        assert effects_of(packets)[-1][1] == EFFECT_SPEED, effects_of(packets)
        # The owner's own client is a reader like any other
        # (`sendToTrackingPlayersAndSelf`), and the whole set is hidden, so this
        # is the only entry the packet has: a hidden effect has no swirls, and
        # the list therefore does not move.
        packets += h.settle(1)
        own = effect_ambience(packets, bot.entity_id)
        assert own and own[-1] == 1, f"an all-hidden set must say so: {own}"
        step("  OK EffBot's own client was sent the ambience byte 1")

        step("-> join PairBot while the byte says yes")
        pair = Bot("PairBot", port)
        wire_client.record(pair, note="effects-wire: the ambience byte in a join burst")
        pair.join()
        pair.start_reader()
        paired = pair.join_packets + drain(pair, 1.5)
        sent = effect_ambience(paired, bot.entity_id)
        assert sent and sent[-1] == 1, \
            f"a joining client must be paired with the byte: {sent}"
        assert effect_particles(paired, bot.entity_id) == [], \
            "...and the byte is all it can be paired with: there is no list to read it off"
        step(f"  OK PairBot was paired with the ambience byte {sent[-1]}, on its own")

        # ...and that byte is *not* recomputed when the set empties: vanilla's
        # empty branch is `removeEffectParticles`, which sets the list alone, so a
        # client keeps the last answer a non-empty set gave. A server that
        # "cleaned up" here would send a false that vanilla never sends — the
        # value is stale on purpose, and there is no list to take down either.
        step("-> /effect clear EffBot (the byte stays where it was)")
        packets = h.command("effect clear EffBot")
        assert removals(packets) == [(bot.entity_id, EFFECT_SPEED)], removals(packets)
        h.settle(1)
        assert effect_ambience(drain(pair, 1.5), bot.entity_id) == [], \
            "an empty set writes nothing, so the byte keeps its last value"

        # ...and the next non-empty set is what corrects it: one visible effect
        # makes the answer false again, and it arrives *with* the list, because
        # one `updateSynchronizedMobEffectParticles` call is what writes the two.
        #
        # This is also the fixture step 10 expects: Glowing standing at the
        # duration this leaves it (200 ticks), so that step's `glowing 30` is an
        # *extension* — a change, which is what makes its packet appear — while
        # the shared byte's glow bit stays where it is.
        step("-> /effect give EffBot glowing 10 (the byte comes back down)")
        h.command("effect give EffBot glowing 10")
        h.settle(1)
        stream = drain(pair, 1.5)
        sent = effect_ambience(stream, bot.entity_id)
        assert sent and sent[-1] == 0, \
            f"a visible effect must correct the stale byte: {sent}"
        assert effect_particles(stream, bot.entity_id)[-1] == \
            [(PARTICLE_ENTITY_EFFECT, COLOR_GLOWING)], \
            "...with the list it was written beside"
        step(f"  OK the visible effect took the byte back to {sent[-1]} with the list")

        # ...and the stale rule once more, from the other side — the side that
        # would show a byte *recomputed* rather than kept: `areAllEffectsAmbient`
        # answers true over an empty set (its loop never runs), which is exactly
        # the answer vanilla does not write. A server that asked the predicate
        # here would send a `true` with the take-down; vanilla sends the list
        # alone, and the byte a client holds is the last 0.
        step("-> /effect clear EffBot (the list goes, the byte stays 0)")
        h.command("effect clear EffBot")
        h.settle(1)
        stream = drain(pair, 1.5)
        assert effect_particles(stream, bot.entity_id)[-1] == [], \
            f"the take-down must arrive: {effect_particles(stream, bot.entity_id)}"
        assert effect_ambience(stream, bot.entity_id) == [], \
            "an empty set is no write at all, not an answer of true"

        # Back to the fixture step 10 expects: the outline comes up again (so
        # that step's refresh is the no-change it asserts), and the byte does
        # *not* come with it — the client already holds a false, and
        # `SynchedEntityData.set` writes on change only. The diff, with the byte
        # unchanged, is the list alone.
        step("-> /effect give EffBot glowing 10 again (the list on its own)")
        h.command("effect give EffBot glowing 10")
        h.settle(1)
        stream = drain(pair, 1.5)
        drain(bot, 1.5)  # the owner's own copy of the flags byte that came back with it
        assert effect_particles(stream, bot.entity_id)[-1] == \
            [(PARTICLE_ENTITY_EFFECT, COLOR_GLOWING)], \
            f"the list must come back: {effect_particles(stream, bot.entity_id)}"
        assert effect_ambience(stream, bot.entity_id) == [], \
            "an unchanged byte is not written"
        step("  OK the list came back on its own, with no byte entry")
        pair.close()

        # --- 10. dying takes the effect set with it --------------------------
        step("-> /effect give EffBot strength 10 1, then glowing, then /kill")
        packets = h.command("effect give EffBot strength 10 1")
        assert effects_of(packets)[-1][1] == EFFECT_STRENGTH, effects_of(packets)
        packets = h.command("effect give EffBot glowing 30")
        assert effects_of(packets)[-1][1] == EFFECT_GLOWING, effects_of(packets)
        packets += h.settle(1)
        # A refresh of the same instance leaves the bit where it already was, so
        # nothing is written — `SynchedEntityData.set` writes on change only, and
        # the outline has been up since step 9c.
        assert shared_flags(packets, bot.entity_id) == [], \
            f"refreshing the effect must not resend the flags: " \
            f"{shared_flags(packets, bot.entity_id)}"
        packets = h.command("kill EffBot")
        assert all_of(packets, CB_PLAYER_COMBAT_KILL), f"/kill must kill: {packets}"
        step("-> client_command PERFORM_RESPAWN")
        bot.send(SB_CLIENT_COMMAND, varint(0))
        packets = drain(bot, 1.5)
        assert find(packets, CB_RESPAWN) is not None, f"no respawn: {packets}"
        removed = removals(packets)
        assert (bot.entity_id, EFFECT_STRENGTH) in removed, \
            f"respawning must clear the effects: {removed}"
        # Player.restoreFrom(old, false) drops the effects *before* the respawn
        # packet, so a client never renders a bonus the server has stopped
        # applying.
        order = [i for i, (p, _) in enumerate(packets)
                 if p == CB_REMOVE_MOB_EFFECT or p == CB_RESPAWN]
        assert order and packets[order[0]][0] == CB_REMOVE_MOB_EFFECT, \
            f"the effect removal must precede the respawn: {[hex(p) for p, _ in packets]}"
        healths = [read_set_health(b) for b in all_of(packets, CB_SET_HEALTH)]
        assert healths and healths[-1] == (20.0, 20, 5.0), f"respawn health: {healths}"
        h.last_health = healths[-1][0]
        # `Player.restoreFrom` drops the effect set, and the outline is that set:
        # a bit remembered from before the death would go on drawing a corpse's
        # glow around the respawned player.
        assert (bot.entity_id, EFFECT_GLOWING) in removed, \
            f"respawning clears the glow effect too: {removed}"
        flags += shared_flags(packets, bot.entity_id)
        assert flags and not flags[-1] & FLAG_GLOWING, \
            f"respawning must take the outline down with it: {flags}"
        # ...and the particles with them, by the *other* path: the respawn
        # replaced the survival state wholesale rather than removing the effects
        # one at a time, so the client's list is stale in a way no remove packet
        # describes and the empty one has to be sent
        # (`updateSynchronizedMobEffectParticles`'s `removeEffectParticles`).
        # The sentinel that makes that diff possible lives on the session
        # rather than in the struct the respawn replaces.
        parts = effect_particles(packets, bot.entity_id)
        assert parts and parts[-1] == [], \
            f"respawning must take the swirls down too: {parts}"
        step("  OK the respawn cleared both the effects and their particles")

        # --- 10b. the item-use byte ------------------------------------------
        # `LivingEntity.DATA_LIVING_ENTITY_FLAGS` (index 8) is the *other* byte
        # of a player's synched data, and it is the one that tells every client
        # but the actor that a use is running: `startUsingItem` sets bit 0
        # (`isUsingItem`) and `stopUsingItem` — and `LivingEntity.die`, which
        # runs it — clears it. This server holds the same answer in the three
        # shapes a use has here: a raised shield (`shield_blocking`), a
        # consumable being eaten (`use_ticks_left`) and a bow being drawn
        # (`bow_draw_slot`). The actor's own client animates itself, so until now
        # the other half of the use was published to nobody: another player never
        # saw a shield come up, a bite start, or a bow bend.
        step("-> /give EffBot minecraft:shield 1, then use_item (raise it)")
        packets = h.command("give EffBot minecraft:shield 1")
        assert "Gave" in feedback(packets), feedback(packets)
        # Which hotbar slot the shield went to, rather than assuming the first
        # free slot is the *selected* one: the player respawned on the drop it
        # died with, and a death's drop is a real, collectable item entity now
        # (`ItemEntity.playerTouch`, once `LivingEntity.createItemStackToDrop`'s
        # forty ticks are up) — so whether the chestplate is back in the hotbar
        # by the time `/give` runs decides which slot is free. `use_item` raises
        # whatever slot is selected, so select the shield's.
        _, _, slots, _ = decode_content(all_of(packets, CB_CONTAINER_SET_CONTENT)[-1])
        shield_slot = next(i for i in range(SLOT_MENU_HOTBAR_FIRST, SLOT_MENU_HOTBAR_FIRST + 9)
                           if slots[i] and slots[i][0] == ITEM_SHIELD)
        bot.send(SB_SET_CARRIED_ITEM,
                 struct.pack(">h", shield_slot - SLOT_MENU_HOTBAR_FIRST))
        drain(bot, 0.5)
        bot.send(SB_USE_ITEM, varint(0) + varint(0) + struct.pack(">ff", 0.0, 0.0))
        flags = use_flags(wire_client.barrier(bot, 1), bot.entity_id)
        assert flags and flags[-1] & FLAG_USING_ITEM, \
            f"a raised shield is the item-use byte's bit 0: {flags}"
        step(f"  OK the byte is 0x{flags[-1]:02x} with the shield up")

        # The other half of the same fact: a client that was not here when the
        # shield went up is told by the join burst, from the registry's copy of
        # the byte (vanilla's `ServerEntity.sendPairingData` again).
        step("-> join WatchBot while the shield is up")
        watch = Bot("WatchBot", port)
        wire_client.record(watch, note="effects-wire: the item-use byte for a second client")
        watch.join()
        watch.start_reader()
        flags = use_flags(watch.join_packets + drain(watch, 1.5), bot.entity_id)
        assert flags and flags[-1] & FLAG_USING_ITEM, \
            f"a joining client must be told about the use: {flags}"
        step(f"  OK WatchBot was sent 0x{flags[-1]:02x} for EffBot in its join")

        step("-> player_action RELEASE_USE_ITEM (5)")
        bot.send(SB_BLOCK_DIG, varint(5) + struct.pack(">q", 0) + bytes([0]) + varint(0))
        flags = use_flags(wire_client.barrier(bot, 1), bot.entity_id)
        assert flags and not flags[-1] & FLAG_USING_ITEM, \
            f"and the release takes it down: {flags}"
        step(f"  OK the byte is 0x{flags[-1]:02x} after the release")
        flags = use_flags(drain(watch, 1.5), bot.entity_id)
        assert flags and not flags[-1] & FLAG_USING_ITEM, \
            f"the release reaches the other client too: {flags}"
        step("  OK and WatchBot saw it come down")

        step("-> raise the shield again, then /kill")
        bot.send(SB_USE_ITEM, varint(0) + varint(0) + struct.pack(">ff", 0.0, 0.0))
        flags = use_flags(wire_client.barrier(bot, 1), bot.entity_id)
        assert flags and flags[-1] & FLAG_USING_ITEM, flags
        packets = h.command("kill EffBot")
        flags = use_flags(packets + drain(watch, 1.5), bot.entity_id)
        assert flags and not flags[-1] & FLAG_USING_ITEM, \
            f"a death ends the use (`LivingEntity.die` -> `stopUsingItem`): {flags}"
        step(f"  OK the byte went down with the corpse (0x{flags[-1]:02x})")
        watch.close()
        step("-> client_command PERFORM_RESPAWN")
        bot.send(SB_CLIENT_COMMAND, varint(0))
        packets = drain(bot, 1.5)
        assert find(packets, CB_RESPAWN) is not None, f"no respawn: {packets}"
        healths = [read_set_health(b) for b in all_of(packets, CB_SET_HEALTH)]
        assert healths and healths[-1] == (20.0, 20, 5.0), f"respawn health: {healths}"
        h.last_health = healths[-1][0]
        assert not use_flags(packets, bot.entity_id), \
            "a respawned player is not using anything: " \
            f"{use_flags(packets, bot.entity_id)}"

        # --- 11. a clean slate -----------------------------------------------
        step("-> /effect clear EffBot after respawning")
        packets = h.command("effect clear EffBot")
        assert "There are no effects to remove" in feedback(packets), feedback(packets)
        step("-> /damage EffBot 10 player_attack (no armour, no effects)")
        landed, _ = h.hit(10)
        assert abs(landed - 10.0) < 0.001, \
            f"respawned: 10 damage must land as 10, got {landed}"
        step(f"  OK clean slate: 10 damage landed as {landed}")

        bot.close()
    finally:
        proc.terminate()
        proc.wait(timeout=5)
    print("\nPASS: effects test")
    return 0


if __name__ == "__main__":
    sys.exit(main())
