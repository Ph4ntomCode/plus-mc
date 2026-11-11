#!/usr/bin/env python3
"""Integration test: Phase 3 milestone I over the real wire protocol.

Mobs are the first entities this server *simulates*: they are spawned, they
walk, they attack, and they die. This test drives all of it with a real client
conversation against a void world (the server's default in-memory world), so the
floor the mobs stand on is built by the test itself out of real `use_item_on`
placements:

  * `/summon zombie` answers with Add Entity (0x01) carrying type 154 at the
    summoning player's ring position, plus Set Entity Data (0x63) carrying
    LivingEntity's health (index 9, a FLOAT) — the mob's first appearance,
  * the mob *chases*: the player crosses to the far corner of the floor and the
    zombie walks most of a seven-block diagonal to follow, reporting
    entity_position_sync (0x23) the whole way,
  * the mob *attacks*: damage_event (0x19) with `minecraft:mob_attack` (damage
    type 28), the player's hurt animation (0x2a) and a set_health (0x68) that
    really drops — by exactly the zombie's ATTACK_DAMAGE of 3.0,
  * line of sight is what a *blow* needs and not what the *chase* needs: walled
    off mid-chase the zombie keeps walking to where it last saw the player (it
    ends up pressed against the wall, inside its own reach) and lands nothing at
    all,
  * the player fights back: a serverbound attack (0x01) with the diamond sword in
    hotbar 0 produces the mob's own damage_event (type `minecraft:player_attack`,
    34), hurt animation, and a health update; a full-charge hit takes exactly the
    6.888 the sword's 7.0 comes to against the zombie's ARMOR 2.0, and the third
    one kills it,
  * the kill runs the death sequence on the server tick: the death entity_event
    (0x22), remove_entities (0x4d), the loot (0x01, type 72) and experience orbs
    (0x01, type 50) that fly to the player and are collected (0x7c
    take_item_entity) — the XP bar moves on collection, not on the kill, and a
    zombie is worth exactly 5,
  * `/mobs` reports the population and `/mobs kill` empties it *and* broadcasts
    the remove_entities,
  * a passive mob never raises mob_attack, and a pig always drops porkchop,
  * the melee enchantment hooks (Phase 4 milestone G), read off the health bar:
    the same full-charge sword takes 6.888 off a zombie, and 9.84 once
    `/enchant MobBot sharpness 5` has put the 3.0 the level's linear value is
    worth on it,
  * a full-charge sword swing *sweeps*: a second zombie sharing the target's own
    box (Player.doSweepAttack's `getBoundingBox().inflate(1.0, 0.25, 1.0)`) is
    hit in the same packet handler, for    the sweep's flat 1.0 — 0.94 through
    the zombie's ARMOR 2.0 — and for 1.0 + the weapon's own Sharpness after the
    enchantment lands,
  * the critical gate is Player.canCriticalAttack's own, read off the state it
    reads: a full-charge swing made in a real fall (the move packets' own dy and
    on-ground flag) takes the ×1.5 — 10.332 off a zombie where the standing
    swing takes 6.888 — and the identical swing with `/effect give MobBot
    blindness infinite` does not, which is the gate's `!isMobilityRestricted()`
    clause live,
  * a second zombie is left to finish the player off: the death sequence reports
    0 health, plays the death entity event, and the player_combat_kill (0x44)
    component reads `death.attack.mob` with `[MobBot, Zombie]`,
  * a mob that falls out of the world is *discarded*: a pig summoned over the
    void (off the platform's edge, so there is nothing under it to land on) is
    gone by a remove_entities some four seconds later — `Entity.checkBelowWorld`
    `discard()`ing it at y = -128, the overworld's `minY - 64` — with no death
    entity event and no loot or experience.

The server runs with `PLUSMC_SURVIVAL_CLOCK=packet` (see the note in
`main()`), so the player's survival clock is this test's own packets: one tick
each, and not one tick while the test is idle. That is what keeps the blows,
the swing charges and the death from being a function of how loaded the machine
is; the *world* clock (the mobs' AI, the orbs' flight) is still real time and is
paced by the drains and the explicit sleeps, exactly as a live server is.

Run: mobs_test.py /path/to/plusmc
"""
import math, os, re, socket, struct, subprocess, sys, tempfile, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nbt_component import chat_text, rd_nbt_translate  # noqa: E402
import wire_client
from wire_client import make_frame, read_packet

PROTO = 777
HOST = "127.0.0.1"

# minecraft:entity_type protocol ids (reports/registries.json; the same ids the
# server's mob_table.h carries).
ET_ZOMBIE = 154
ET_PIG = 101
ET_ITEM = 72
ET_EXPERIENCE_ORB = 50

# minecraft:damage_type wire ids (damage_table.h).
DMG_PLAYER_ATTACK = 34  # minecraft:player_attack, index 34 in kDamageTypes
DMG_MOB_ATTACK = 28     # minecraft:mob_attack, index 28 in kDamageTypes
DMG_MACE_SMASH = 26     # minecraft:mace_smash, index 26 in kDamageTypes
DMG_FALL = 10           # minecraft:fall, index 10 in kDamageTypes
DMG_THORNS = 44         # minecraft:thorns, index 44 in kDamageTypes

ITEM_STONE = 1
ITEM_DIAMOND = 1012
ITEM_DIAMOND_SWORD = 1050  # +6.0 attack damage, -2.4 attack speed
ITEM_MACE = 1374         # +5.0 attack damage, -3.4 attack speed (a 33-tick charge)
ITEM_DIAMOND_CHESTPLATE = 1085  # 528 durability, 8 armour points, +2 toughness

# minecraft:enchantment registry ids (enchantment_table.h; the same ids the
# enchantments component carries on the wire).
ENCHANT_THORNS = 39

# minecraft:data_component_type ids (item_registry.h).
COMPONENT_DAMAGE = 3
COMPONENT_ENCHANTMENTS = 13

# InventoryMenu slot numbers (world/inventory.h): 5..8 are the armour slots, in
# head, chest, legs, feet order — the reverse of the inventory's own.
MENU_CHEST_ARMOR = 6

# Clientbound packet ids.
CB_SPAWN_ENTITY = 0x01
CB_BLOCK_UPDATE = 0x08
CB_BLOCK_ACK = 0x04
CB_DAMAGE_EVENT = 0x19
CB_ENTITY_EVENT = 0x22
CB_ENTITY_POSITION_SYNC = 0x23
CB_HURT_ANIMATION = 0x2B
CB_REMOVE_ENTITIES = 0x4E
CB_CONTAINER_SET_SLOT = 0x14
CB_SET_ENTITY_DATA = 0x65
CB_SET_ENTITY_MOTION = 0x67
CB_SET_EXPERIENCE = 0x69
CB_SET_HEALTH = 0x6A
CB_TAKE_ITEM_ENTITY = 0x7F
CB_SYSTEM_CHAT = 0x7C
CB_PLAYER_COMBAT_KILL = 0x45
CB_KEEP_ALIVE = 0x2D

# Serverbound packet ids.
CB_SB_ATTACK = 0x01
SB_CHAT = 0x09
SB_CHAT_COMMAND = 0x07
SB_POSITION = 0x1E
SB_FLYING = 0x21
SB_PLAYER_COMMAND = 0x2A
SB_SET_CARRIED_ITEM = 0x36
SB_SET_CREATIVE_SLOT = 0x39
SB_USE_ITEM_ON = 0x42
SB_KEEP_ALIVE = 0x1C
# ServerboundPlayerAbilitiesPacket: one bitfield byte, FLAG_FLYING = 2.
SB_PLAYER_ABILITIES = 0x28

ENTITY_EVENT_DEATH = 3
MOB_HEALTH_INDEX = 9
FLOAT_SERIALIZER = 3


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
    # x<<38 | z<<12 | y, packed as an *unsigned* 64-bit word: a negative x sets
    # the top bits, which no signed pack would take (the floor the test lays is
    # 5x5, so x and z go negative).
    packed = (((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF)) & 0xFFFFFFFFFFFFFFFF
    return struct.pack(">Q", packed)


# --- decoders ---------------------------------------------------------------

def read_set_slot_header(body):
    """container_set_slot (0x14): [varint container id][varint state id][i16 slot]
    then the stack. Only the header is decoded: the refusals below are about which
    slot was answered, not what landed in it."""
    cid, pos = rd_varint(body, 0)
    state, pos = rd_varint(body, pos)
    if pos + 2 > len(body):
        return None
    slot = struct.unpack(">h", body[pos:pos + 2])[0]
    return cid, state, slot


def read_stack_at(body, pos):
    """The *clientbound* ItemStack at `pos`: [varint count][varint item] then a
    DataComponentPatch written plainly (`ItemStack.STREAM_CODEC` — no per-value
    length, unlike the delimited patch a client sends). Returns
    (item, count, damage, enchantments, pos).

    Only the two components this test's own stacks carry are decoded; anything
    else is a packet this decoder was not written for, which is worth failing on
    rather than skipping, because a skipped component would silently mis-frame
    the next field.
    """
    count, pos = rd_varint(body, pos)
    if count == 0:
        return 0, 0, 0, [], pos
    item, pos = rd_varint(body, pos)
    added, pos = rd_varint(body, pos)
    removed, pos = rd_varint(body, pos)
    damage = 0
    enchantments = []
    for _ in range(added):
        cid, pos = rd_varint(body, pos)
        if cid == COMPONENT_DAMAGE:
            damage, pos = rd_varint(body, pos)
        elif cid == COMPONENT_ENCHANTMENTS:
            entries, pos = rd_varint(body, pos)
            for _ in range(entries):
                enchant, pos = rd_varint(body, pos)
                level, pos = rd_varint(body, pos)
                enchantments.append((enchant, level))
        else:
            raise AssertionError(f"component id {cid} on a stack this test wrote")
    for _ in range(removed):
        _, pos = rd_varint(body, pos)
    return item, count, damage, enchantments, pos


def slot_stack(batch, menu_slot):
    """(item, count, damage, enchantments) for the last container_set_slot in the
    batch that names `menu_slot`, or None when the batch said nothing about it."""
    latest = None
    for body in all_of(batch, CB_CONTAINER_SET_SLOT):
        header = read_set_slot_header(body)
        if header is None or header[2] != menu_slot:
            continue
        _cid, pos = rd_varint(body, 0)
        _state, pos = rd_varint(body, pos)
        item, count, damage, enchantments, _end = read_stack_at(body, pos + 2)
        latest = (item, count, damage, enchantments)
    return latest


def chest_wear(batch):
    """The chestplate's cumulative `minecraft:damage` after this batch, or None
    when the batch said nothing about the slot.

    `ItemStack.hurtAndBreak` writes the new damage and the equipment sync pushes
    the slot, so every point `Player.hurtArmor` takes off is visible in a
    container_set_slot for the chest armour slot — there is no other read of a
    player's own armour in this protocol.
    """
    stack = slot_stack(batch, MENU_CHEST_ARMOR)
    if stack is None:
        return None
    item, count, damage, _ench = stack
    return damage if count and item == ITEM_DIAMOND_CHESTPLATE else None


def read_spawn(body):
    """Add Entity: [varint eid][uuid 16][varint type][f64 x y z][LpVec3]
    [u8 pitch][u8 yaw][u8 head yaw][varint data]."""
    eid, pos = rd_varint(body, 0)
    uuid = body[pos:pos + 16]
    pos += 16
    etype, pos = rd_varint(body, pos)
    x, y, z = struct.unpack(">ddd", body[pos:pos + 24])
    pos += 24
    # LpVec3 (src/protocol/lp_vec.h): a single 0 byte means a zero vector;
    # otherwise two marker bytes, a big-endian u32, and — when the marker's low
    # three bits say the scale did not fit — a scale varint on the end. A flat
    # six bytes mis-frames everything after it for a spawn velocity of 3 or more.
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


def read_position_sync(body):
    """26.3: [varint eid][varint PositionPath type = LINEAR 0][f64 x y z]
    [f32 yRot][f32 xRot][bool onGround]."""
    eid, pos = rd_varint(body, 0)
    path_type, pos = rd_varint(body, pos)
    assert path_type == 0, f"position_sync path type {path_type}"
    x, y, z = struct.unpack(">ddd", body[pos:pos + 24])
    pos += 24
    yaw, pitch = struct.unpack(">ff", body[pos:pos + 8])
    pos += 8
    ground = body[pos]
    pos += 1
    assert pos == len(body), f"position_sync: {len(body) - pos} trailing byte(s)"
    return {"entity": eid, "x": x, "y": y, "z": z,
            "yaw": yaw, "pitch": pitch, "ground": bool(ground)}


def read_entity_data(body, wanted=None):
    """[varint eid] then [u8 index][varint serializer][value]... to 0xFF.

    `wanted` filters by entity id and returns None for anyone else's data: this
    decoder knows the FLOAT and INT serializers the mob health uses, and the
    server also sends other entities' data (a player's own, an item drop's), which
    it has no business guessing at.
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
        if serializer == FLOAT_SERIALIZER:
            values[index] = struct.unpack(">f", body[pos:pos + 4])[0]
            pos += 4
        elif serializer == 1:  # INT
            v, pos = rd_varint(body, pos)
            values[index] = v
        else:
            raise AssertionError(f"entity data index {index}: serializer {serializer}")
    assert pos == len(body), f"set_entity_data: {len(body) - pos} trailing byte(s)"
    return eid, values


def read_damage_event(body):
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


def read_entity_event(body):
    assert len(body) == 5, f"entity_event body is {len(body)} bytes"
    return struct.unpack(">i", body[0:4])[0], body[4]


def read_hurt_animation(body):
    eid, pos = rd_varint(body, 0)
    yaw = struct.unpack(">f", body[pos:pos + 4])[0]
    return eid, yaw


def read_remove_entities(body):
    count, pos = rd_varint(body, 0)
    ids = []
    for _ in range(count):
        v, pos = rd_varint(body, pos)
        ids.append(v)
    assert pos == len(body), f"remove_entities: {len(body) - pos} trailing byte(s)"
    return ids


def read_player_combat_kill(body):
    """: [varint player id][component] -> (id, key, args). Unlike system_chat
    there is no overlay byte here, so the component runs to the end of the packet."""
    eid, pos = rd_varint(body, 0)
    key, args, end = rd_nbt_translate(body, pos)
    assert end == len(body), f"player_combat_kill: {len(body) - end} trailing byte(s)"
    return eid, key, args


def read_take_item(body):
    item, pos = rd_varint(body, 0)
    player, pos = rd_varint(body, pos)
    amount, pos = rd_varint(body, pos)
    assert pos == len(body), f"take_item_entity: {len(body) - pos} trailing byte(s)"
    return item, player, amount


def read_set_experience(body):
    """[f32 progress][varint level][varint total]."""
    progress = struct.unpack(">f", body[0:4])[0]
    level, pos = rd_varint(body, 4)
    total, pos = rd_varint(body, pos)
    assert pos == len(body), f"set_experience: {len(body) - pos} trailing byte(s)"
    return round(progress, 4), level, total


def read_set_health(body):
    health = struct.unpack(">f", body[0:4])[0]
    food, pos = rd_varint(body, 4)
    return round(health, 3), food


def read_lp_vec3(body, pos):
    """LpVec3 (src/protocol/lp_vec.h), decoded the way the encoder packs it: a
    zero vector is one 0x00 byte, otherwise two marker bytes and a u32 holding
    three 15-bit components, plus a scale varint when the first byte's low three
    bits say the scale did not fit.

    The six bytes are `u8(buffer) | u8(buffer >> 8) | u32(buffer >> 16)`, so the
    four after the first two are the buffer's high half *big-endian*: reading
    all six as one little-endian integer swaps the middle two bytes of the u32
    with its ends, which is enough to scramble every component.
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


def read_motion(body):
    """set_entity_motion: [varint eid][LpVec3] -> (eid, (vx, vy, vz))."""
    eid, pos = rd_varint(body, 0)
    velocity, pos = read_lp_vec3(body, pos)
    assert pos == len(body), f"set_entity_motion: {len(body) - pos} trailing byte(s)"
    return eid, velocity


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
            if pid == 0x2D:
                self.send(0x1C, body[:8])
            elif pid == 0x0B:
                batch, _ = rd_varint(body)
                self.send(0x0B, varint(batch))
                break
        return True

    def move_to(self, x, y, z, ground=1):
        self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([ground]))


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


def feedback(packets):
    """The literal SystemChats (command replies)."""
    out = []
    for body in all_of(packets, CB_SYSTEM_CHAT):
        try:
            out.append(chat_text(body))
        except AssertionError:
            pass
    return " ".join(out)


def wait_for_xp(bot, min_total, timeout=10.0, settle=1.0):
    """Drain until the experience total has reached `min_total` and held still for
    `settle` seconds.

    The orbs fly to the player on the *world* clock (the real one, not the packet
    clock this test drives), so a kill's reward arrives over the next second or
    two rather than with the death packet. A merged stack is also handed over one
    unit at a time (Player.takeXpDelay paces it 2 ticks apart), so the total can
    climb in steps and the wait has to watch it settle rather than just change.
    Returns everything seen while waiting.
    """
    orbs, collected, xp = [], [], []
    last_total = xp[-1][2] if xp else -1
    last_change = time.time()
    end = time.time() + timeout
    while time.time() < end:
        batch = drain(bot, 0.2)
        for body in all_of(batch, CB_SPAWN_ENTITY):
            spawn = read_spawn(body)
            if spawn["type"] == ET_EXPERIENCE_ORB:
                orbs.append(spawn)
        collected += [read_take_item(b) for b in all_of(batch, CB_TAKE_ITEM_ENTITY)]
        xp += [read_set_experience(b) for b in all_of(batch, CB_SET_EXPERIENCE)]
        total = xp[-1][2] if xp else -1
        if total != last_total:
            last_total = total
            last_change = time.time()
        if total >= min_total and time.time() - last_change >= settle:
            break
    return orbs, collected, xp


def tick(bot, count):
    """Advance the player's survival clock by exactly `count` game ticks.

    The server runs with `PLUSMC_SURVIVAL_CLOCK=packet`, so the clock *is* these
    packets — one tick each, and nothing at all while the test is not sending.
    There is deliberately no sleep between them: with the wall clock each idle
    second was another twenty ticks, which made a swing's charge (12.5 ticks for
    the diamond sword) and the invulnerability windows a function of how busy
    the machine was.

    The *world* clock is not this one. The mobs' AI, the orbs' flight and the
    furnaces run on the server's own 50 ms tick in real time, so a step that
    needs the world to advance paces itself with a drain window or an explicit
    `time.sleep` — see the fight loops below.
    """
    for _ in range(count):
        bot.send(SB_FLYING, bytes([1]))


def send_chat_command(bot, command):
    payload = command.encode()
    bot.send(SB_CHAT_COMMAND, varint(len(payload)) + payload)


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


FLOOR_Y = 63  # the mobs stand *on* this layer, so the player's feet are at 64
# The corner the player stands in for the chase: inside the 9x9 floor (whose
# cells cover x,z in [-4, 5]), and far enough from the summon ring that the
# zombie has most of a seven-block diagonal to walk.
PLAYER_X = -3.5
PLAYER_Z = -3.5


def build_floor(bot, radius=5, y=FLOOR_Y):
    """Places a stone platform with real `use_item_on` packets, so the world the
    mobs walk on is the server's own, not a test shortcut. Clicking the cell
    below the target with face=up places one block above it.

    The server measures its block interaction range from where the client says
    it is (Player.isWithinBlockInteractionRange: 4.5 + a 1.0 lag buffer, from the
    eye to the block's *box*), so the bot walks onto each row and lays it — which
    is what a player building a platform does. One standing spot could not reach
    the corners of a 9x9.
    """
    bot.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", 36) + varint(64) + varint(ITEM_STONE)
             + b"\x00\x00")
    time.sleep(0.2)
    placed = 0
    seq = 1
    for x in range(-radius + 1, radius):
        # Stand on the row being built: one block above the layer we click, at
        # the row's own x (so every cell of it is within reach along z).
        bot.move_to(x + 0.5, float(y + 1), 0.5)
        for z in range(-radius + 1, radius):
            body = (varint(0) + position(x, y - 1, z) + varint(1)
                    + struct.pack(">fff", 0.5, 0.0, 0.5) + bytes([0, 0]) + varint(seq))
            bot.send(SB_USE_ITEM_ON, body)
            seq += 1
            placed += 1
    packets = drain(bot, 3.5)
    updates = all_of(packets, CB_BLOCK_UPDATE)
    assert len(updates) == placed, (
        f"expected {placed} block updates for the floor, got {len(updates)}")
    return placed


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-mobs-")
    env = dict(os.environ)
    # The mob tick rides the *server* clock (one batch per main-loop tick)
    # rather than the survival clock, so the packet clock set below changes the
    # player's tick without touching the mobs' AI; keep the keep-alive noise out
    # of the way.
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    # The player's survival clock counts *this test's own packets*: one game tick
    # each, with no wall-time catch-up (`tick_survival_on_packet`, session.h).
    # Every survival-tick quantity in here is otherwise a function of idle wall
    # time — the zombie's blows land on `drain_mob_damage`, the swing charge is
    # the packets sent since the last one, and the invulnerability windows count
    # themselves down a tick per packet — so on a loaded machine the assertions
    # below would measure the load. Under this clock the test decides the ticks:
    # `tick(bot, 14)` is fourteen of them whether the machine is idle or busy.
    # The world clock (mobs, orbs, furnaces) stays real, and is what the fight
    # loops' `time.sleep(0.4)` and drain windows pace.
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"

    proc = None
    try:
        step(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--motd", "mobs test",
             "--offline-mode", "--gamemode", "1", "--data", data_root, "--world", world,
             "--no-flat-world"], wait_listening, env=env)
        step(f"server listening on {port}")

        bot = Bot("MobBot", port)
        wire_client.record(bot, note="mobs: a zombie chasing, hitting, and dying to the player")
        bot.join()
        bot.start_reader()
        packets = drain(bot, 1.2)
        for body in all_of(packets, 0x86):  # update_attributes carries the entity id
            bot.entity_id, _ = rd_varint(body, 0)
        assert bot.entity_id is not None, "the join must send update_attributes"
        step(f"  joined as entity {bot.entity_id}")

        # Stand still at a known spot, then build the floor under it. The floor is
        # 9x9 so the mob has room to walk a real distance to the player, and so a
        # knockback cannot push it off the edge into the void.
        for _ in range(3):
            bot.move_to(0.5, 64.0, 0.5)
            time.sleep(0.05)
        step("-> placing a stone floor (81 real use_item_on packets)")
        placed = build_floor(bot)
        assert placed == 81, placed
        step(f"  OK floor: {placed} blocks placed")
        # The floor build walked the bot along the last row; the rest of the test
        # (the /summon ring, which is offset from the player) works from the floor's
        # centre, so step back to it.
        for _ in range(2):
            bot.move_to(0.5, 64.0, 0.5)
            time.sleep(0.05)

        # The weapon goes into hotbar 0 by hand rather than through /give, because
        # the floor just filled slot 36 with stone and /give would have to pick
        # another slot; the point of this test is the fight, and /give has its own
        # coverage in commands_test.py.
        step("-> hotbar 0 = diamond sword, hotbar 1 = stone (creative slots)")
        bot.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", 36) + varint(1)
                 + varint(ITEM_DIAMOND_SWORD) + b"\x00\x00")
        # The stone for the wall goes into hotbar 1 *now*, while the player is
        # still creative. It has to: the whole body of
        # ServerGamePacketListenerImpl.handleSetCreativeModeSlot sits inside
        # `if (this.player.hasInfiniteMaterials())`, so a survival client cannot
        # write a slot at all. That is what the wall phase below honours by
        # switching the *held* hotbar slot instead of conjuring the block into
        # one, and it is why the sword can stay in hotbar 0 for the fight.
        bot.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", 37) + varint(64)
                 + varint(ITEM_STONE) + b"\x00\x00")
        time.sleep(0.2)
        drain(bot, 0.3)

        # The positive control for the survival refusal below: this exact write,
        # while the player is still creative, reaches the wire. The gate below is
        # therefore proved by the *silence* of the same packet, not by a packet
        # that never had a chance of being parsed. Hotbar 2 is free (the sword is
        # in 0, the stone in 1); the diamonds it leaves there are inert.
        hotbar2_write = (struct.pack(">h", 38) + varint(64) + varint(ITEM_DIAMOND)
                         + b"\x00\x00")
        bot.send(SB_SET_CREATIVE_SLOT, hotbar2_write)
        written = [read_set_slot_header(b)
                   for b in all_of(drain(bot, 0.6), CB_CONTAINER_SET_SLOT)]
        assert any(s is not None and s[2] == 38 for s in written), (
            f"in creative the write must be echoed for menu slot 38: {written}")
        step("  OK creative slot 38 echoed (the control)")

        # Survival, so the zombie has a reason to come over: a creative or
        # spectator player is not an attackable target
        # (EntitySelector.NO_CREATIVE_OR_SPECTATOR) and the mob would only ever
        # stroll.
        step("-> /gamemode survival")
        send_chat_command(bot, "gamemode survival")
        packets = drain(bot, 0.8)
        assert "survival" in feedback(packets).lower(), feedback(packets)
        step("  OK the player is in survival")

        # ...and in survival a set_creative_slot is refused outright. The whole
        # body of ServerGamePacketListenerImpl.handleSetCreativeModeSlot sits
        # inside `if (this.player.hasInfiniteMaterials())` (Player.java:1197), so
        # the packet that put the sword in hotbar 0 a moment ago must now do
        # nothing at all: no slot echo, no inventory change. Without the gate a
        # survival client could conjure any item into any of its own slots with
        # one packet, which is the hole the audit closed.
        step("-> the same set_creative_slot must now be refused")
        bot.send(SB_SET_CREATIVE_SLOT, hotbar2_write)
        packets = drain(bot, 0.8)
        written = [read_set_slot_header(b) for b in all_of(packets, CB_CONTAINER_SET_SLOT)]
        written = [s for s in written if s is not None and s[2] == 38]
        assert not written, (
            "a survival client's creative slot write must be refused, but the "
            f"server echoed it: {written}")
        # The write was byte-identical to the one echoed a moment ago in creative,
        # so the silence here is the gate and not a truncated packet.
        step("  OK no slot echo (hotbar 2 left alone)")

        # --- 1. /summon -------------------------------------------------------
        step("-> /summon zombie 1")
        send_chat_command(bot, "summon zombie 1")
        packets = drain(bot, 1.5)
        assert "Summoned 1 minecraft:zombie" in feedback(packets), feedback(packets)
        spawns = [read_spawn(b) for b in all_of(packets, CB_SPAWN_ENTITY)]
        mob = next((s for s in spawns if s["type"] == ET_ZOMBIE), None)
        assert mob is not None, f"no zombie spawn: {[s['type'] for s in spawns]}"
        assert mob["data"] == 0, mob
        step(f"  OK spawn: entity {mob['entity']} type {mob['type']} at "
             f"({mob['x']:.2f}, {mob['y']:.2f}, {mob['z']:.2f})")
        # The summoning ring puts it two blocks out, at the summoner's own y.
        assert abs(mob["x"] - 2.5) < 0.01 and abs(mob["z"] - 0.5) < 0.01, mob
        assert abs(mob["y"] - 64.0) < 0.01, mob

        healths = []
        for body in all_of(packets, CB_SET_ENTITY_DATA):
            parsed = read_entity_data(body, mob["entity"])
            if parsed and MOB_HEALTH_INDEX in parsed[1]:
                healths.append(parsed[1][MOB_HEALTH_INDEX])
        assert healths and abs(healths[0] - 20.0) < 0.01, f"zombie health: {healths}"
        step(f"  OK mob health data: {healths[0]} (LivingEntity index {MOB_HEALTH_INDEX})")

        # --- 2. it walks to the player ---------------------------------------
        # The player crosses the floor, so the chase is a real *path* rather than
        # the two blocks the summon ring leaves: the zombie has to close most of a
        # seven-block diagonal. Its movement rides the server's own tick clock
        # (20 TPS in real time), so this loop only has to drain.
        step("-> /tp to the far corner; the zombie must follow (position syncs)")
        send_chat_command(bot, f"tp {PLAYER_X} 64.0 {PLAYER_Z}")
        packets = drain(bot, 0.8)
        assert "Teleported to" in feedback(packets), feedback(packets)
        moved = []
        for _ in range(60):
            packets = drain(bot, 0.15)
            for body in all_of(packets, CB_ENTITY_POSITION_SYNC):
                sync = read_position_sync(body)
                if sync["entity"] == mob["entity"]:
                    moved.append(sync)
            time.sleep(0.05)
        assert moved, "the zombie never moved: no entity_position_sync for it"
        travelled = max(math.hypot(s["x"] - mob["x"], s["z"] - mob["z"]) for s in moved)
        last = moved[-1]
        gap = math.hypot(last["x"] - PLAYER_X, last["z"] - PLAYER_Z)
        assert travelled > 3.5, f"the zombie barely moved ({travelled:.2f} blocks): {last}"
        assert gap < 2.5, f"the zombie never reached the player ({gap:.2f} away): {last}"
        step(f"  OK chased {travelled:.2f} blocks, now {gap:.2f} from the player "
             f"at ({last['x']:.2f}, {last['z']:.2f})")

        # --- 3. it attacks the player ----------------------------------------
        # The hit is *queued* by the world tick and applied on this player's own
        # packet clock (a PlayState belongs to one thread), so this loop has to
        # send flying packets: that is what makes the survival tick run.
        step("-> the zombie must hit the player (mob_attack + set_health)")
        events = []
        player_health = []
        hurt = []
        # damage_player writes the event, then the hurt animation, then the new
        # health, in that order on this session's own stream, so one blow's size
        # is the gap between the bar the client last *read* and the bar that
        # arrives right behind the event. Two traps make a looser reading lie:
        # this step fell into the first, and the second is why it must not:
        #
        #   * a remembered "full bar" is not the pre-hit bar. The food bar heals
        #     between blows, and vanilla's fast bar heals
        #     `min(saturation, 6) / 6` a step (`FoodData.tick`), so the bar sits on
        #     fractions like 18.5 — and reading a 3.0 blow off 20.0 reports
        #     20.0 - 15.5 = 4.5. That is exactly what the harness's old `HEALTHS`
        #     seed did once the windowed drain that wrote it was replaced by
        #     `wire_client`'s reader: the seed became the constant 20.0, and the
        #     "zombie hits for 4.5" it produced was the drop from full health,
        #     not the blow. (The unit test pins the number on the server side too:
        #     mob_test's `report.attacks[0].damage == 3.0`.)
        #   * two blows inside one drain would fold into one gap. The pairing
        #     therefore counts the `mob_attack` events that fall between the
        #     baseline packet and the packet it measures against, and accepts
        #     exactly one.
        #
        # The first gap with a baseline in front of it is the one measured, so a
        # blow the zombie landed while it was still walking the chase above (whose
        # health packet is the first thing this loop reads) only establishes the
        # baseline.
        landed = None
        blows_in_gap = 0
        seen_health = None
        for _ in range(40):
            tick(bot, 5)
            packets = drain(bot, 0.2)
            for pid, body in packets:
                if pid == CB_DAMAGE_EVENT:
                    event = read_damage_event(body)
                    if event["entity"] == bot.entity_id:
                        events.append(event)
                        blows_in_gap += 1
                elif pid == CB_SET_HEALTH:
                    health = read_set_health(body)
                    player_health.append(health)
                    if seen_health is not None and blows_in_gap:
                        landed = (seen_health, health[0], blows_in_gap)
                    blows_in_gap = 0
                    seen_health = health[0]
                elif pid == CB_HURT_ANIMATION:
                    hurt.append(read_hurt_animation(body))
            if landed is not None:
                break
        assert events, "the zombie never attacked"
        assert events[0]["type"] == DMG_MOB_ATTACK, events[0]
        assert events[0]["cause"] == mob["entity"], (
            f"the attacking mob should be the causing entity: {events[0]}")
        assert landed is not None, f"the player's health never dropped: {player_health}"
        before, after, blows = landed
        assert blows == 1, (
            f"the measured gap holds {blows} blows, not one: {player_health}")
        assert after < before, (
            f"the packet behind the hit is not a drop ({before} -> {after}): {player_health}")
        took = before - after
        assert abs(took - 3.0) < 0.01, (
            f"a zombie hits for exactly 3.0, took {took} off a bar at {before} "
            f"(a full-bar baseline would have read {20.0 - after}): {player_health}")
        assert after > 6.0, f"the player must survive one hit to fight: {after}"
        assert any(eid == bot.entity_id for eid, _ in hurt), f"no player hurt animation: {hurt}"
        step(f"  OK zombie hit for {took:.1f} off a bar at {before:.2f} through "
             f"minecraft:mob_attack (cause entity {events[0]['cause']}, health {after:.1f})")

        # --- 3b. line of sight: the chase persists, the blow does not --------
        # TargetGoal.unseenMemoryTicks versus MeleeAttackGoal.canPerformAttack,
        # over the wire. The player runs to the far side of the floor and the test
        # walls the two apart mid-chase: a mob that had *seen* the player keeps
        # walking to where it last saw them — ending up pressed against the wall,
        # inside its own reach — but a blow also wants `hasLineOfSight`, so not one
        # lands. (A zombie's reach is 2.04 squared: its 0.6-wide box doubled, plus
        # the target's width.)
        step("-> wall the player off mid-chase: it keeps coming, and cannot hit")
        # The player crosses to the far edge — the *middle* of it, not the corner:
        # the wall is nine blocks wide and the block interaction range reaches 5.5
        # blocks from the eye, so one standing spot can only build the whole wall
        # from the middle of it. The zombie walks to the far side either way, and
        # what this phase tests is the sight break, not the bearing.
        send_chat_command(bot, "tp 0.5 64.0 4.5")
        drain(bot, 0.3)
        # Select the stone out of hotbar 1 rather than writing it into a slot:
        # the player is in survival now, and a survival client's
        # set_creative_slot is refused (handleSetCreativeModeSlot gates the whole
        # body on hasInfiniteMaterials). The sword stays in hotbar 0.
        bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", 1))
        time.sleep(0.2)
        drain(bot, 0.2)
        # A two-high wall across the whole floor at z = 1: a zombie steps 0.6, so
        # one block already stops it walking over, and the floor ends in the void,
        # so there is nowhere to path around either. The zombie is still on the
        # player's side when it goes up and has seen them cross, so the only thing
        # that can keep it coming is the unseen memory.
        seq = 1000
        for x in range(-4, 5):
            for y in (64, 65):
                bot.send(SB_USE_ITEM_ON, varint(0) + position(x, y - 1, 1) + varint(1)
                         + struct.pack(">fff", 0.5, 0.0, 0.5) + bytes([0, 0]) + varint(seq))
                seq += 1
        packets = drain(bot, 1.4)
        wall = len(all_of(packets, CB_BLOCK_UPDATE))
        assert wall == 18, f"the wall needs 18 placements, got {wall}"
        blind = [read_position_sync(b) for b in all_of(packets, CB_ENTITY_POSITION_SYNC)]
        blind = [s for s in blind if s["entity"] == mob["entity"]]
        assert len(blind) >= 2, f"the mob stopped reporting once blind: {len(blind)}"
        walked = blind[-1]["z"] - blind[0]["z"]
        assert walked > 1.0, (
            f"a mob that has seen a player keeps walking to where it saw them: "
            f"only {walked:.2f} blocks after the wall went up")
        stalled = blind[-1]
        assert stalled["z"] < 1.0, f"it walked through the wall: {stalled}"
        step(f"  OK it walked {walked:.2f} blocks with no line of sight and stopped "
             f"against the wall at z {stalled['z']:.2f}")

        # Stand right behind the wall, in front of the zombie: out of its sight, but
        # inside `isWithinMeleeAttackRange` — which is what makes "it lands nothing"
        # a test of the line-of-sight gate rather than of the distance.
        stand_x = round(stalled["x"], 1)
        stand_z = 2.0
        send_chat_command(bot, f"tp {stand_x} 64.0 {stand_z}")
        drain(bot, 0.4)
        hits = []
        last = stalled
        # The loop is paced in *both* clocks, on purpose. The survival ticks are
        # the packets — five each, which is what charges the player's swing and
        # decays the invulnerability windows — and the world clock is the drain
        # window, because the mob's swing is MeleeAttackGoal's 20-tick interval
        # on the server's own tick. Three 0.5 s windows put at least one of those
        # attempts inside the loop; a shorter one would pass without ever giving
        # the zombie the chance to be refused, which is the only thing this
        # assertion is about. (Under the wall clock the sleep inside `tick` was
        # silently paying for this.)
        for _ in range(3):
            tick(bot, 5)
            packets = drain(bot, 0.5)
            for body in all_of(packets, CB_DAMAGE_EVENT):
                event = read_damage_event(body)
                if event["entity"] == bot.entity_id:
                    hits.append(event)
            for body in all_of(packets, CB_ENTITY_POSITION_SYNC):
                sync = read_position_sync(body)
                if sync["entity"] == mob["entity"]:
                    last = sync
        # MeleeAttackGoal.getAttackReachSqr, in the server's own terms: the gap from
        # the mob's centre to the player's box, against (2 * bbWidth)^2 + width.
        dx = max(0.0, abs(stand_x - last["x"]) - 0.3)
        dz = max(0.0, abs(stand_z - last["z"]) - 0.3)
        dy = max(0.0, max(64.0 - (last["y"] + 1.95), last["y"] - (64.0 + 1.8)))
        reach_sqr = (0.6 * 2) ** 2 + 0.6
        assert dx * dx + dy * dy + dz * dz <= reach_sqr, (
            f"the zombie should be standing in reach ({dx:.2f}, {dy:.2f}, {dz:.2f} "
            f"vs {reach_sqr}): {last}")
        assert not hits, f"a blind mob must not hit through a wall: {hits}"
        step(f"  OK in reach ({dx * dx + dy * dy + dz * dz:.2f} <= {reach_sqr}) and "
             f"not one blow: the swing wants line of sight")

        # And the same rule from the player's end: a sword cannot reach through the
        # wall either. The stone goes back to the diamond sword, the swing is a full
        # charge, and the zombie is 1.5 blocks away — well inside
        # Player.isWithinAttackRange's 6.0 (the 3.0 ENTITY_INTERACTION_RANGE plus
        # the 3.0 lag buffer handleAttack passes) — so the segment between the two
        # pairs of eyes is the only thing that can refuse it. The next step lands a
        # 6.888 hit on this very mob with this very sword, which is what makes this
        # a statement about the wall rather than about the attack.
        # Back to the sword: it is still in hotbar 0, so this only selects it.
        bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", 0))
        time.sleep(0.2)
        drain(bot, 0.3)
        tick(bot, 14)  # charge the swing: an uncharged one would only graze
        bot.send(CB_SB_ATTACK, varint(mob["entity"]))
        packets = drain(bot, 0.5)
        tick(bot, 14)
        packets += drain(bot, 0.4)
        swung = [read_damage_event(b) for b in all_of(packets, CB_DAMAGE_EVENT)]
        assert not swung, f"a sword must not reach through a wall: {swung}"
        for body in all_of(packets, CB_SET_ENTITY_DATA):
            data = read_entity_data(body, mob["entity"])
            if data and MOB_HEALTH_INDEX in data[1]:
                assert abs(data[1][MOB_HEALTH_INDEX] - 20.0) < 0.01, (
                    f"the mob's health moved behind the wall: {data[1]}")
        step("  OK the player's swing is refused the same way (no damage_event, "
             "mob health still 20.0)")

        # --- 4. the player fights back ---------------------------------------
        # The fight happens at the middle of the floor. The zombie chases the
        # player back over, and the kill then leaves its orbs about a block and a
        # half from the player — inside the platform, which matters because an
        # orb's pull can overshoot (vanilla's is a 0.1/tick acceleration against
        # only a 0.98 drag, so a launched orb really can clear a couple of blocks),
        # and an orb that leaves the platform falls into the void for good.
        step("-> back to the middle of the floor, select hotbar 0, then attack")
        send_chat_command(bot, f"tp 0.5 64.0 0.5")
        drain(bot, 0.5)
        bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", 0))
        time.sleep(0.2)
        drain(bot, 0.3)

        mob_hits = 0
        mob_damage_events = []
        death_seen = False
        # Two clocks pace this loop. The *mob's* invulnerability window is
        # decremented by the world tick, in real time (20 ticks = one second), so
        # the loop waits about that long between swings. The *player's* attack
        # charge is counted by its own packet tick, so each attempt also sends
        # enough flying packets (14 >= the diamond sword's 12.5-tick delay) to
        # make the swing a full-charge hit rather than the 0.2-scale minimum.
        for attempt in range(10):
            bot.send(CB_SB_ATTACK, varint(mob["entity"]))
            packets = drain(bot, 0.3)
            tick(bot, 14)
            packets += drain(bot, 0.3)
            for body in all_of(packets, CB_DAMAGE_EVENT):
                event = read_damage_event(body)
                if event["entity"] == mob["entity"]:
                    mob_damage_events.append(event)
            for body in all_of(packets, CB_SET_ENTITY_DATA):
                parsed = read_entity_data(body, mob["entity"])
                if parsed and MOB_HEALTH_INDEX in parsed[1]:
                    healths.append(parsed[1][MOB_HEALTH_INDEX])
            for eid, event in [read_entity_event(b) for b in all_of(packets, CB_ENTITY_EVENT)]:
                if eid == mob["entity"] and event == ENTITY_EVENT_DEATH:
                    death_seen = True
            mob_hits += 1
            if death_seen:
                break
            time.sleep(0.4)
        assert mob_damage_events, "the player's attack produced no damage_event for the mob"
        assert mob_damage_events[0]["type"] == DMG_PLAYER_ATTACK, mob_damage_events[0]
        assert mob_damage_events[0]["cause"] == bot.entity_id, mob_damage_events[0]
        assert healths[-1] < 20.0, f"the mob's health never moved: {healths}"
        # A full-charge diamond sword is 7.0 (1.0 base + 6.0 modifier), and the
        # zombie's ARMOR 2.0 turns that into 6.888 (CombatRules.getDamageAfterAbsorb:
        # clamp(2 - 7/2, 0.4, 20) = 0.4, so 7 * (1 - 0.4/25)). Asserting the number
        # is what proves the *held* weapon drives the hit: an empty hand would only
        # ever take 0.94 off.
        first_drop = healths[0] - healths[1]
        assert abs(first_drop - 6.888) < 0.01, (
            f"a full-charge diamond sword takes 6.888 off a zombie, got {first_drop}: {healths}")
        step(f"  OK the mob took damage: health {healths[0]} -> {healths[-1]} "
             f"({len(mob_damage_events)} hits recorded, {first_drop:.3f} the first)")

        # --- 5. it dies and leaves loot and experience ------------------------
        assert death_seen, f"the zombie never died after {mob_hits} attacks: {healths}"
        # The death event and the remove_entities that follows it can land in the
        # very batch that carried the killing blow, so keep that batch as well as
        # whatever the tick pumps out next.
        packets += drain(bot, 2.0)
        removed = []
        for body in all_of(packets, CB_REMOVE_ENTITIES):
            removed.extend(read_remove_entities(body))
        assert mob["entity"] in removed, f"the dead mob was not removed: {removed}"
        step("  OK death: entity_event 3 + remove_entities")

        drops = [read_spawn(b) for b in all_of(packets, CB_SPAWN_ENTITY)]
        flesh = [d for d in drops if d["type"] == ET_ITEM]
        # A zombie's loot is a uniform 0..2 rotten flesh, so an empty roll is a
        # genuine vanilla outcome rather than a missing drop. All this asserts is
        # that nothing unexpected fell out; the deterministic loot proof is the
        # pig killed in step 7, whose porkchop roll is 1..3.
        assert all(d["type"] in (ET_ITEM, ET_EXPERIENCE_ORB) for d in drops), \
            f"unexpected entity types at the kill: {[d['type'] for d in drops]}"
        step(f"  OK death drops: {len(flesh)} item entity(ies) "
             f"({'an empty 0..2 rotten-flesh roll' if not flesh else 'rotten flesh'})")

        # The orbs arrive as entities too, and the bar moves only once they are
        # collected: the orbs fly to the player on the world clock, so poll until
        # the bar has taken the whole reward.
        orbs = [d for d in drops if d["type"] == ET_EXPERIENCE_ORB]
        collected = [read_take_item(b) for b in all_of(packets, CB_TAKE_ITEM_ENTITY)]
        xp = [read_set_experience(b) for b in all_of(packets, CB_SET_EXPERIENCE)]
        more_orbs, more_collected, more_xp = wait_for_xp(bot, 5)
        orbs += more_orbs
        collected += more_collected
        xp += more_xp
        assert orbs or collected, "no experience orbs at the kill"
        assert collected, f"the orbs were never collected: orbs={len(orbs)} xp={xp}"
        assert xp and xp[-1][2] == 5, (
            f"a zombie is worth exactly 5 experience, and only on collection: {xp}")
        step(f"  OK {len(orbs)} orb(s) spawned, {len(collected)} collected, "
             f"xp total {xp[-1][2]}")

        # --- 6. /mobs and /mobs kill -----------------------------------------
        step("-> /mobs, then /summon + /mobs kill")
        send_chat_command(bot, "mobs")
        packets = drain(bot, 1.0)
        assert "No mobs are loaded" in feedback(packets), feedback(packets)
        step("  OK /mobs is empty after the kill")

        send_chat_command(bot, "summon pig 3")
        packets = drain(bot, 1.5)
        assert "Summoned 3 minecraft:pig" in feedback(packets), feedback(packets)
        send_chat_command(bot, "mobs")
        packets = drain(bot, 1.0)
        assert "minecraft:pig x3" in feedback(packets), feedback(packets)
        step("  OK /mobs reports the population")

        send_chat_command(bot, "mobs kill")
        packets = drain(bot, 1.0)
        assert "Removed 3 entities" in feedback(packets), feedback(packets)
        removed = []
        for body in all_of(packets, CB_REMOVE_ENTITIES):
            removed.extend(read_remove_entities(body))
        assert len(removed) == 3, f"/mobs kill must despawn all three: {removed}"
        step("  OK /mobs kill despawned 3")

        # --- 7. a pig does not fight back, and always drops its loot ---------
        step("-> a pig never attacks, and a pig always drops porkchop")
        send_chat_command(bot, "summon pig 1")
        packets = drain(bot, 1.0)
        assert "Summoned 1 minecraft:pig" in feedback(packets), feedback(packets)
        pig = next((read_spawn(b) for b in all_of(packets, CB_SPAWN_ENTITY)), None)
        assert pig is not None and pig["type"] == ET_PIG, pig
        pig_health = []
        pig_dead = False
        pig_at = (pig["x"], pig["z"])
        for _ in range(6):
            # A sword wants line of sight now, and a pig wanders: the player walks
            # to wherever the pig's own position syncs put it, in the same batch as
            # the swing so nothing moves in between. (A pig that has strolled
            # behind the wall the previous phase built cannot be hit from the
            # middle of the floor, which is rather the point.)
            send_chat_command(bot, f"tp {pig_at[0]:.2f} 64.0 {pig_at[1]:.2f}")
            bot.send(CB_SB_ATTACK, varint(pig["entity"]))
            packets = drain(bot, 0.25)
            tick(bot, 14)
            packets += drain(bot, 0.3)
            for body in all_of(packets, CB_ENTITY_POSITION_SYNC):
                sync = read_position_sync(body)
                if sync["entity"] == pig["entity"]:
                    pig_at = (sync["x"], sync["z"])
            # A passive mob has no MeleeAttackGoal at all, so mob_attack can never
            # name the player while one is in play.
            for body in all_of(packets, CB_DAMAGE_EVENT):
                event = read_damage_event(body)
                assert not (event["entity"] == bot.entity_id and
                            event["type"] == DMG_MOB_ATTACK), f"a pig attacked: {event}"
            for body in all_of(packets, CB_SET_ENTITY_DATA):
                parsed = read_entity_data(body, pig["entity"])
                if parsed and MOB_HEALTH_INDEX in parsed[1]:
                    pig_health.append(parsed[1][MOB_HEALTH_INDEX])
            for eid, event in [read_entity_event(b) for b in all_of(packets, CB_ENTITY_EVENT)]:
                if eid == pig["entity"] and event == ENTITY_EVENT_DEATH:
                    pig_dead = True
            if pig_dead:
                break
            time.sleep(0.4)
        assert pig_dead, f"the pig never died: {pig_health}"
        packets += drain(bot, 1.5)
        drops = [read_spawn(b) for b in all_of(packets, CB_SPAWN_ENTITY)]
        pork = [d for d in drops if d["type"] == ET_ITEM]
        # A pig's porkchop count is a uniform 1..3, so a kill always leaves
        # something behind — unlike the zombie's 0..2 flesh roll above.
        assert pork, f"a pig must always drop porkchop: {[d['type'] for d in drops]}"
        # Animal.getExperienceReward is 1 + nextInt(3), so a kill always leaves
        # experience behind as well. Whether those orbs *reach* the player is not
        # asserted here — a passive mob wanders before it dies, so the orbs can
        # start several blocks out and the platform is finite — but the zombie's
        # step already proved the collection path end to end.
        orb_spawns = [d for d in drops if d["type"] == ET_EXPERIENCE_ORB]
        assert orb_spawns, f"a pig must always leave experience: {[d['type'] for d in drops]}"
        step(f"  OK no mob_attack from the pig; killed for {len(pork)} porkchop and "
             f"{len(orb_spawns)} experience orb(s)")

        # --- 8. the melee enchantment hooks, and the sweep -------------------
        # Player.attack's two enchantment hooks from the far side of the wire, and
        # Player.doSweepAttack. Both are read off the *mob's* health bar, which is
        # the only place a damage number is observable at all — and both are read
        # against a weapon whose numbers this test knows.
        #
        # The player is put in creative for the whole scenario, and that is what
        # makes the numbers deterministic rather than merely likely: a creative
        # player is not an attackable target (EntitySelector.NO_CREATIVE_OR_
        # SPECTATOR), and even a zombie the swing provokes lands nothing on one
        # (ServerPlayer.hurtServer refuses an `abilities.invulnerable` victim). The
        # zombies below therefore never touch the bar the assertions read, and the
        # only damage in play is the damage this test computes.
        step("-> /gamemode creative, then a fresh zombie pair and one swing")
        send_chat_command(bot, "gamemode creative")
        packets = drain(bot, 0.8)
        assert "creative" in feedback(packets).lower(), feedback(packets)

        def fresh_pair():
            """Two zombies still inside each other's sweep box, sword selected.

            Summoned one at a time on purpose: /summon's ring spreads a *count*
            evenly around the summoner, so `summon zombie 2` would put the pair
            two blocks apart on opposite sides of the player — outside each
            other's sweep box. Two single summons land both at the same ring
            cell.

            How long they stay there is the whole difficulty of this step. A mob
            with no target *strolls* — WaterAvoidingRandomStrollGoal is the
            no-target branch of the goal selector for every mob, monsters
            included — and each stroll is its own random walk of 1..3 blocks, so
            two mobs summoned together drift apart at about a block a second
            (measured: 1.7 and 1.95 blocks in the 1.8 s the first draft spent
            reading the pair before swinging).
            The swing therefore follows the second summon by a quarter of a
            second, which is the most a drain can be cut to and still read the
            spawn; the separation the pair has *reported* by then is checked
            against the sweep box, so a pair that did drift fails with the
            positions rather than with a mystery.
            """
            send_chat_command(bot, "tp 0.5 64.0 0.5")
            drain(bot, 0.2)
            # The sword first: the connection is ordered, so the swing that
            # follows the summons has nothing left to wait for. The ring puts the
            # pair two blocks out, well inside the 6.0 attack range.
            bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", 0))
            pair = []
            # The bar each zombie arrives with, so a hit can be read as the gap
            # between two of its own health updates rather than as an absolute.
            health = {}
            at = {}
            for _ in range(2):
                send_chat_command(bot, "summon zombie 1")
                batch = drain(bot, 0.25)
                spawned = [read_spawn(b) for b in all_of(batch, CB_SPAWN_ENTITY)]
                spawned = [s for s in spawned if s["type"] == ET_ZOMBIE]
                assert spawned, "no zombie spawn"
                pair.append(spawned[0])
                at[spawned[0]["entity"]] = (spawned[0]["x"], spawned[0]["z"])
                for body in all_of(batch, CB_SET_ENTITY_DATA):
                    parsed = read_entity_data(body, spawned[0]["entity"])
                    if parsed and MOB_HEALTH_INDEX in parsed[1]:
                        health[spawned[0]["entity"]] = [parsed[1][MOB_HEALTH_INDEX]]
                # A stroll's first step is already a position sync, and it is the
                # truthful answer about where the mob is now.
                for body in all_of(batch, CB_ENTITY_POSITION_SYNC):
                    sync = read_position_sync(body)
                    if sync["entity"] == spawned[0]["entity"]:
                        at[sync["entity"]] = (sync["x"], sync["z"])
            assert all(len(v) == 1 for v in health.values()), health
            (ax, az), (bx, bz) = at[pair[0]["entity"]], at[pair[1]["entity"]]
            # The box is the target's own box inflated by (1.0, 0.25, 1.0): two
            # 0.6-wide mobs overlap it while they are within 1.6 blocks in x and
            # in z, and Player.doSweepAttack's own test wants 3.0 between centres.
            assert abs(ax - bx) <= 1.5 and abs(az - bz) <= 1.5, (
                f"the pair drifted apart before the swing: {at}")
            return pair, health

        def swing(target, health):
            """One full-charge swing at `target`, folding the bars and events in."""
            events = {}
            tick(bot, 14)  # >= the sword's 12.5-tick delay: a full charge
            bot.send(CB_SB_ATTACK, varint(target["entity"]))
            batch = drain(bot, 0.6)
            for body in all_of(batch, CB_DAMAGE_EVENT):
                event = read_damage_event(body)
                events.setdefault(event["entity"], []).append(event)
            for body in all_of(batch, CB_SET_ENTITY_DATA):
                for eid in health:
                    parsed = read_entity_data(body, eid)
                    if parsed and MOB_HEALTH_INDEX in parsed[1]:
                        health[eid].append(parsed[1][MOB_HEALTH_INDEX])
            return events

        def drop(health, eid):
            """The one hit that landed on `eid`: its pre-hit and post-hit bars."""
            values = health.get(eid, [])
            assert len(values) == 2, f"{eid} needs exactly one hit's two bars: {health}"
            return values[0] - values[1]

        pair, health = fresh_pair()
        target, bystander = pair[0], pair[1]
        step(f"  OK zombies {target['entity']} and {bystander['entity']} both at "
             f"({target['x']:.2f}, {target['z']:.2f})")
        events = swing(target, health)
        # A full-charge diamond sword against ARMOR 2.0 is the same 6.888 step 4
        # pins, and the sweep is `1.0 + ratio * baseDamage` with no Sweeping Edge
        # at all: the flat 1.0 vanilla adds — which the intuitive `ratio *
        # baseDamage` would make 0 — through the same armour. At 1.0 damage the
        # absorb term is `clamp(2 - 1/2, 2/5, 20)` = 1.5, so 1.0 * (1 - 1.5/25).
        assert abs(drop(health, target["entity"]) - 6.888) < 0.02, health
        assert [e["type"] for e in events.get(target["entity"], [])] == [DMG_PLAYER_ATTACK], \
            events
        assert abs(drop(health, bystander["entity"]) - 0.94) < 0.02, (
            f"the swept zombie took {drop(health, bystander['entity'])}: {health}")
        assert [e["type"] for e in events.get(bystander["entity"], [])] == [DMG_PLAYER_ATTACK], \
            events
        step(f"  OK the target took {drop(health, target['entity']):.3f} and the swept "
             f"bystander {drop(health, bystander['entity']):.3f} (the sweep's flat 1.0)")

        # --- 8b. the critical gate, and Blindness ---------------------------
        # Player.canCriticalAttack is eight clauses, and the one this server
        # could get wrong without anyone noticing is `!isMobilityRestricted()`,
        # whose whole body is `hasEffect(BLINDNESS)` (Player.java): the effect
        # table carries minecraft:blindness and `/effect` applies it, so a
        # blinded player used to land the 1.5x critical on a falling swing.
        #
        # Both halves are proved on the state the gate actually reads, not on a
        # test-side flag. The fall distance is Entity.checkFallDamage's own
        # accumulation of the move packets' dy and the on-ground flag is the
        # packet's own (`apply_movement_effects`), so a descent sent with
        # ground=0 is a genuine airborne swing — the player is two blocks above
        # the floor when the blow lands. And the sword is still the plain one
        # from the swing above, so the only thing that differs from the 6.888
        # pinned there is the critical.
        step("-> a falling swing is the ×1.5 critical; a blinded one is not")

        def summon_one():
            """One zombie on the ring, with the one bar it arrives with."""
            send_chat_command(bot, "mobs kill")
            drain(bot, 0.4)
            send_chat_command(bot, "summon zombie 1")
            batch = drain(bot, 0.3)
            spawned = [read_spawn(b) for b in all_of(batch, CB_SPAWN_ENTITY)]
            spawned = [s for s in spawned if s["type"] == ET_ZOMBIE]
            assert spawned, "no zombie spawn"
            zombie = spawned[0]
            health = []
            for body in all_of(batch, CB_SET_ENTITY_DATA):
                parsed = read_entity_data(body, zombie["entity"])
                if parsed and MOB_HEALTH_INDEX in parsed[1]:
                    health.append(parsed[1][MOB_HEALTH_INDEX])
            assert len(health) == 1, health
            return zombie, health

        def falling_swing(zombie, health):
            """One full-charge swing made in mid-air, and the hit's two bars."""
            # Stand tall, then fall: the descent has to be the last thing the
            # server heard about where the player is, or it is a landing that
            # spent the fall distance rather than a fall.
            bot.move_to(0.5, 69.0, 0.5, ground=1)
            drain(bot, 0.15)
            tick(bot, 14)  # >= the sword's 12.5-tick delay: a full charge
            bot.move_to(0.5, 66.0, 0.5, ground=0)  # three blocks down, airborne
            bot.send(CB_SB_ATTACK, varint(zombie["entity"]))
            batch = drain(bot, 0.6)
            events = []
            for body in all_of(batch, CB_DAMAGE_EVENT):
                event = read_damage_event(body)
                if event["entity"] == zombie["entity"]:
                    events.append(event)
            for body in all_of(batch, CB_SET_ENTITY_DATA):
                parsed = read_entity_data(body, zombie["entity"])
                if parsed and MOB_HEALTH_INDEX in parsed[1]:
                    health.append(parsed[1][MOB_HEALTH_INDEX])
            # Land, and spend the fall distance while the player is still
            # creative: Player.causeFallDamage skips a mayfly player outright,
            # so the landing is free and the next step starts from the floor.
            bot.move_to(0.5, 64.0, 0.5, ground=1)
            drain(bot, 0.2)
            return events

        # Down to the floor *before* the summon, so the ring puts the zombie at
        # the floor's own y (step 1's rule) rather than up in the air with the
        # player that is about to fall past it.
        bot.move_to(0.5, 64.0, 0.5, ground=1)
        drain(bot, 0.2)
        zombie, health = summon_one()
        events = falling_swing(zombie, health)
        # 7.0 at full charge with the critical's 1.5 on top is 10.5, which the
        # zombie's ARMOR 2.0 takes to 10.5 * (1 - clamp(2 - 10.5/2, 0.4, 20)/25)
        # = 10.332. The swing above pins 6.888 for the same sword *standing*, so
        # the 3.444 between them is the critical and nothing else.
        assert [e["type"] for e in events] == [DMG_PLAYER_ATTACK], events
        assert len(health) == 2, f"the falling swing must land once: {health}"
        crit_drop = health[0] - health[1]
        assert abs(crit_drop - 10.332) < 0.02, (
            f"a falling full-charge diamond sword is the 1.5x critical: {health}")
        step(f"  OK the falling swing took {crit_drop:.3f} "
             f"(the standing 6.888, ×1.5)")

        # ...and the same swing, blinded, must not. `/effect give` is what puts
        # it there, so this is the live surface a player has — the effect is
        # really in the set, ticked and sent to the client, not a flag.
        step("-> /effect give MobBot blindness infinite, then the same fall")
        send_chat_command(bot, "effect give MobBot blindness infinite")
        packets = drain(bot, 0.8)
        assert "Blindness" in feedback(packets), feedback(packets)
        zombie, health = summon_one()
        events = falling_swing(zombie, health)
        assert [e["type"] for e in events] == [DMG_PLAYER_ATTACK], events
        assert len(health) == 2, f"the blinded swing must land once: {health}"
        # Blinded, the gate's `!isMobilityRestricted()` is false, so the hit is
        # the plain 7.0 the swing above pins at 6.888: the critical is absent.
        blind_drop = health[0] - health[1]
        assert abs(blind_drop - 6.888) < 0.02, (
            f"a blinded swing must not be a critical: {health}")
        step(f"  OK blinded it took {blind_drop:.3f}: no critical")

        # Leave the effect set as it was found, so the steps after this one are
        # not silently blinded too.
        send_chat_command(bot, "effect clear MobBot")
        packets = drain(bot, 0.5)
        assert "Removed" in feedback(packets), feedback(packets)
        send_chat_command(bot, "mobs kill")
        drain(bot, 0.6)

        # ...and the same swing once the sword carries the hook. Sharpness V is
        # the linear 1.0 + 0.5 * (5 - 1) = 3.0, so the target takes 10.0 and the
        # sweep reaches the bystander as `1.0 + 3.0` — the enchantment resolves
        # against *each* victim the sweep lands on, not once for the swing.
        step("-> /enchant MobBot sharpness 5, then the same pair and swing")
        send_chat_command(bot, "enchant MobBot sharpness 5")
        packets = drain(bot, 0.8)
        assert "Applied enchantment" in feedback(packets), feedback(packets)
        send_chat_command(bot, "mobs kill")
        drain(bot, 0.8)
        pair, health = fresh_pair()
        target, bystander = pair[0], pair[1]
        events = swing(target, health)
        assert abs(drop(health, target["entity"]) - 9.84) < 0.02, (
            f"sharpness 5 is 7.0 + 3.0 through the armour: {health}")
        assert abs(drop(health, bystander["entity"]) - 3.936) < 0.02, (
            f"the sweep is 1.0 + 3.0 through the armour: {health}")
        step(f"  OK sharpness 5: the target took {drop(health, target['entity']):.3f} "
             f"and the swept bystander {drop(health, bystander['entity']):.3f}")

        send_chat_command(bot, "mobs kill")
        drain(bot, 0.8)

        # --- 8c. the mace's smash --------------------------------------------
        # MaceItem is the one item whose getAttackDamageBonus is not 0.0f, and
        # its smash is four things at once: the `minecraft:mace_smash` damage
        # type in place of player_attack, the fall-distance curve (4 a block to
        # three, 2 a block to eight, 1 after) added to the mace's own 6.0,
        # Density's `smash_damage_per_fallen_block` scaled by the same distance,
        # and a 3.5-block knockback sweep around the victim. This step reads the
        # first three off the wire; the sweep is step 8d's.
        #
        # The fall is deliberately the *least* a smash can be: 1.6 blocks, sent
        # as one move packet, so the fall distance the server accumulates is
        # exactly 1.6. The target is a 20-HP zombie and the numbers have to stay
        # under the bar, which is why two of the runs are blinded —
        # MaceItem.canSmashAttack does not read the effect, but
        # Player.canCriticalAttack does, and 6.0 + 6.4 fits under 20 where
        # (6.0 + 6.4) x 1.5 does not.
        step("-> hotbar 1 = minecraft:mace, then a standing hit and a 1.6-block fall")
        bot.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", 37) + varint(1) + varint(ITEM_MACE)
                 + b"\x00\x00")
        drain(bot, 0.3)
        bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", 1))
        drain(bot, 0.3)

        def mace_swing(zombie, fall, bars, before_attack=None):
            """One full-charge mace swing from `fall` blocks up, run from the
            floor: the hit's damage events, the batch, and `bars` grown by the
            target's own health update (which is how the arrival bar the summon
            already read stays in the list).

            The mace's attack speed is 0.6, a 33.3-tick delay, so the charge is
            34 flying packets — sent while standing at the top of the drop. The
            descent then has to be the last word on where the player is, or a
            landing spends the fall distance and there is no smash left to make
            (`Entity.checkFallDamage`: it accumulates on descent and is spent on
            the ground).

            `before_attack`, when given, runs after that descent and immediately
            before the attack packet: the last moment at which a bystander can be
            re-placed against a victim that has strolled since setup.
            """
            bot.move_to(0.5, 64.0 + fall, 0.5, ground=1)
            drain(bot, 0.15)
            tick(bot, 34)
            if fall:
                bot.move_to(0.5, 64.0, 0.5, ground=0)
            if before_attack is not None:
                before_attack()
            bot.send(CB_SB_ATTACK, varint(zombie["entity"]))
            # Poll rather than assert on one 0.6 s window: on a loaded box the
            # server can be more than a tick behind this test's clock, and the
            # blow still lands. Read until its damage event arrives (or 2.5 s
            # passes), then keep the window open a little longer for the
            # knockback position syncs step 8d reads out of the same batch.
            batch = []
            deadline = time.time() + 2.5
            seen_damage = False
            while time.time() < deadline:
                part = drain(bot, 0.25)
                batch.extend(part)
                for body in all_of(part, CB_DAMAGE_EVENT):
                    event = read_damage_event(body)
                    if event["entity"] == zombie["entity"]:
                        seen_damage = True
                        break
                if seen_damage:
                    batch.extend(drain(bot, 0.3))
                    break
            events = []
            for body in all_of(batch, CB_DAMAGE_EVENT):
                event = read_damage_event(body)
                if event["entity"] == zombie["entity"]:
                    events.append(event)
            for body in all_of(batch, CB_SET_ENTITY_DATA):
                parsed = read_entity_data(body, zombie["entity"])
                if parsed and MOB_HEALTH_INDEX in parsed[1]:
                    bars.append(parsed[1][MOB_HEALTH_INDEX])
            # Land, so the next run starts from the floor. In creative the
            # landing itself costs nothing; what it does here is spend whatever
            # fall distance is left.
            bot.move_to(0.5, 64.0, 0.5, ground=1)
            drain(bot, 0.2)
            return events, batch

        def one_bar(bars):
            assert len(bars) == 2, f"the swing must land exactly once: {bars}"
            return bars[0] - bars[1]

        zombie, bars = summon_one()
        events, _ = mace_swing(zombie, 0.0, bars)
        assert [e["type"] for e in events] == [DMG_PLAYER_ATTACK], events
        standing_drop = one_bar(bars)
        # The mace is 6.0 (the player's base 1.0 plus its 5.0), and standing there
        # is no smash at all: through the zombie's ARMOR 2.0 that is 6.0 x 0.984.
        assert abs(standing_drop - 5.904) < 0.03, (
            f"a standing mace is a plain 6.0 weapon: {bars}")
        step(f"  OK standing: {standing_drop:.3f} and damage type "
             f"{DMG_PLAYER_ATTACK} (player_attack)")

        step("-> /effect give blindness, then the 1.6-block fall with and without Density V")
        send_chat_command(bot, "effect give MobBot blindness infinite")
        packets = drain(bot, 0.8)
        assert "Blindness" in feedback(packets), feedback(packets)
        zombie, bars = summon_one()
        events, _ = mace_swing(zombie, 1.6, bars)
        assert [e["type"] for e in events] == [DMG_MACE_SMASH], events
        smash_drop = one_bar(bars)
        # 6.0 + 4 x 1.6 = 12.4, through the same armour: 12.4 x 0.984, and the
        # event itself is the proof of the damage *type* switch — the standing
        # hit above arrived as player_attack.
        assert abs(smash_drop - 12.2016) < 0.03, (
            f"a blinded 1.6-block smash is 6.0 + 6.4: {bars}")
        step(f"  OK the smash took {smash_drop:.3f} and arrived as mace_smash "
             f"({DMG_MACE_SMASH}), not player_attack")

        # Density goes on the *held* mace through /enchant, which is also what
        # proves the generated items of #minecraft:enchantable/mace really are
        # the mace's id.
        send_chat_command(bot, "enchant MobBot density 5")
        packets = drain(bot, 0.8)
        assert "Applied enchantment" in feedback(packets), feedback(packets)
        zombie, bars = summon_one()
        events, _ = mace_swing(zombie, 1.6, bars)
        assert [e["type"] for e in events] == [DMG_MACE_SMASH], events
        density_drop = one_bar(bars)
        # Density V is the linear 0.5 + 0.5 x 4 = 2.5 a block fallen, so the
        # bonus grows by 2.5 x 1.6 = 4.0 and the hit by 4.0 x 0.984. The gap
        # between the two blind runs is the enchantment's whole share.
        assert abs(density_drop - 16.1376) < 0.03, (
            f"Density V adds 2.5 x 1.6 to the smash: {bars}")
        assert abs((density_drop - smash_drop) - 3.936) < 0.03, (density_drop, smash_drop)
        step(f"  OK Density V took it to {density_drop:.3f} "
             f"(+{density_drop - smash_drop:.3f} over the same fall)")

        send_chat_command(bot, "effect clear MobBot")
        packets = drain(bot, 0.5)
        assert "Removed" in feedback(packets), feedback(packets)

        # With the effect gone the same swing is a critical as well: the smash's
        # gate does not ask, and Player.canCriticalAttack answers yes on exactly
        # the state a smash is made from. Player.attack's order is why the number
        # is (6.0 + 6.4) x 1.5 and not 6.0 x 1.5 + 6.4 — the add is at line 910,
        # the multiplier at 913.
        #
        # The creative slot is written again first: an enchantment lives on the
        # *stack*, so replacing it is how a control run gets a plain mace back
        # (there is no /unenchant).
        bot.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", 37) + varint(1) + varint(ITEM_MACE)
                 + b"\x00\x00")
        drain(bot, 0.4)
        zombie, bars = summon_one()
        events, _ = mace_swing(zombie, 1.6, bars)
        assert [e["type"] for e in events] == [DMG_MACE_SMASH], events
        crit_drop = one_bar(bars)
        assert abs(crit_drop - 18.3024) < 0.03, (
            f"an unblinded 1.6-block smash is (6.0 + 6.4) x 1.5: {bars}")
        step(f"  OK unblinded: {crit_drop:.3f}, the smash and the critical together")

        # --- 8d. the smash's 3.5-block knockback -----------------------------
        # MaceItem.knockback pushes every living entity inside 3.5 blocks of the
        # *victim* — the victim and the attacker excepted — with
        # `(3.5 - distance) * 0.7` away from it and a flat 0.7 upward (doubled
        # only past five blocks of fall). The pair step 8 built is the shape to
        # watch: the bystander takes no damage at all, so its own position syncs
        # are the only trace the push leaves on the wire — and a mob standing on
        # the flat floor reports y = 64.0 every time, which is what makes "y went
        # up" a launch rather than a walk.
        step("-> two zombies together, one smashed: the bystander must be launched")
        send_chat_command(bot, "mobs kill")
        drain(bot, 0.5)
        pair, _ = fresh_pair()
        target, bystander = pair[0], pair[1]
        # fresh_pair stages the *sword* in hotbar 0; the smash needs the mace.
        bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", 1))
        drain(bot, 0.2)
        events, packets = mace_swing(target, 1.6, [])
        assert [e["type"] for e in events] == [DMG_MACE_SMASH], events
        launched = [read_position_sync(b)
                    for b in all_of(packets, CB_ENTITY_POSITION_SYNC)]
        launched = [s for s in launched if s["entity"] == bystander["entity"]]
        assert launched, "the bystander never reported a position: no push at all"
        assert any(s["y"] > 64.05 for s in launched), (
            f"the bystander was never launched off the floor: {launched}")
        step(f"  OK the bystander was pushed: {len(launched)} sync(s), y up to "
             f"{max(s['y'] for s in launched):.2f} from 64.00")

        send_chat_command(bot, "mobs kill")
        drain(bot, 0.8)

        # --- 8e. the sweep reaches players, and creative flight is exempt ----
        # MaceItem.knockback's predicate is `!nearby.isSpectator() && nearby !=
        # attacker && nearby != entity && ... && !(nearby instanceof Player &&
        # isCreative() && abilities.flying)`. Step 8d proved two of those clauses
        # on a *mob*; the ones only a player can answer are the exemption's two
        # halves, and this is where a second connection answers them. What makes
        # a player a different population is only where the push *lands*: a mob's
        # velocity is the server's to write and a player's belongs to its own
        # client, so here the packet is the whole evidence.
        step("-> a second client joins; the smash pushes it, then leaves it alone once it flies")
        bot2 = Bot("SmashViewer", port)
        wire_client.record(bot2, note="mobs: the second client the smash sweeps")
        bot2.join()
        bot2.start_reader()
        packets = drain(bot2, 1.2)
        for body in all_of(packets, 0x86):  # update_attributes carries the entity id
            bot2.entity_id, _ = rd_varint(body, 0)
        assert bot2.entity_id is not None, "the second client must see its own attributes"
        step(f"  joined as entity {bot2.entity_id}")

        def smash_with_bystander():
            """One mace smash from 1.6 blocks up with the second client parked
            beside the victim: (the smasher's damage events, everything the
            second client was sent while the blow landed).

            The victim is what the 3.5 blocks are measured from, so the second
            client is placed against it — by its own position packet, which the
            server applies as sent. A block to the side rather than on the same
            cell, so the direction is a real one: two entities at one point are
            pushed straight up and nothing else.
            """
            zombie, _ = summon_one()
            bot2.move_to(zombie["x"] + 1.0, 64.0, zombie["z"])
            drain(bot2, 0.8)

            def repark():
                # The sweep is measured from where the victim stands *at the
                # blow*, and the victim strolls: the park above is stale by the
                # time the 34-packet charge ends. Re-park against the freshest
                # position the attacker itself has been sent — the position
                # syncs already buffered are at most a tick or two old — and let
                # that move land before the sweep reads it.
                zx, zz = zombie["x"], zombie["z"]
                for body in all_of(drain(bot, 0.05), CB_ENTITY_POSITION_SYNC):
                    sync = read_position_sync(body)
                    if sync["entity"] == zombie["entity"]:
                        zx, zz = sync["x"], sync["z"]
                bot2.move_to(zx + 1.0, 64.0, zz)
                drain(bot2, 0.15)

            events, _ = mace_swing(zombie, 1.6, [], before_attack=repark)
            return events, drain(bot2, 0.6)

        def smash_until(want_push):
            """One smash whose blow actually landed *and* whose sweep gave the
            answer asked for, retried while the stroll parked the bystander out
            of the 3.5 blocks entirely (a setup miss, not a result to assert
            on). Whether the push is expected is still the caller's assertion:
            the flying case asks for its absence, the other two for its
            presence.
            """
            events, seen = None, None
            for _ in range(3):
                events, seen = smash_with_bystander()
                if [e["type"] for e in events] != [DMG_MACE_SMASH]:
                    continue
                if (push_for(seen) is not None) == want_push:
                    break
            return events, seen

        def push_for(seen):
            """The velocity the second client's own motion packet carried, or
            None when the sweep left it out."""
            for body in all_of(seen, CB_SET_ENTITY_MOTION):
                eid, velocity = read_motion(body)
                if eid == bot2.entity_id:
                    return velocity
            return None

        events, seen = smash_until(want_push=True)
        assert [e["type"] for e in events] == [DMG_MACE_SMASH], events
        push = push_for(seen)
        assert push is not None, (
            f"the smash must push the other player: {[hex(p) for p, _ in seen]}")
        # The vertical is MaceItem's flat 0.7 for every entity the sweep reaches,
        # however far out — which is what makes it the push's fingerprint. The
        # horizontal is `(3.5 - distance) * 0.7` away from the *victim*, and the
        # victim is a strolling zombie, so it is bounded rather than pinned.
        horizontal = math.hypot(push[0], push[2])
        assert abs(push[1] - 0.7) < 0.02, push
        assert 0.0 < horizontal < 2.45, push
        step(f"  OK the smash pushed the other player: vy {push[1]:.3f}, "
             f"horizontal {horizontal:.3f}")

        # ...and the same client, one abilities packet later, is skipped. The
        # packet's flying bit is the only place the server can learn the second
        # half of `isCreative() && abilities.flying`; the first half is the
        # gamemode it joined with.
        bot2.send(SB_PLAYER_ABILITIES, bytes([0x02]))
        drain(bot2, 0.3)
        events, seen = smash_until(want_push=False)
        assert [e["type"] for e in events] == [DMG_MACE_SMASH], events
        assert push_for(seen) is None, (
            f"a creative player who is flying must be left out of the sweep: {seen}")
        step("  OK flying in creative: no motion packet at all")

        # The bit is the variable, not the client: clearing it brings the push
        # back, which is what keeps the assertion above from passing because
        # nothing was pushed for some other reason.
        bot2.send(SB_PLAYER_ABILITIES, bytes([0x00]))
        drain(bot2, 0.3)
        events, seen = smash_until(want_push=True)
        assert [e["type"] for e in events] == [DMG_MACE_SMASH], events
        assert push_for(seen) is not None, "clearing the flying bit restores the push"
        step("  OK not flying again: the push is back")

        bot2.close()
        send_chat_command(bot, "mobs kill")
        drain(bot, 0.5)
        send_chat_command(bot, "gamemode survival")
        packets = drain(bot, 0.8)
        assert "survival" in feedback(packets).lower(), feedback(packets)

        # --- 8f. postHurtEnemy's fall-distance reset --------------------------
        # MaceItem.postHurtEnemy is `if (canSmashAttack(attacker))
        # attacker.resetFallDistance()`, and it is the half of the smash that no
        # damage number shows: it is only visible where a landing costs
        # something, which is a survival player. The control is the *same*
        # descent with no smash at all — a 6-block fall is floor(6 - 3) = 3
        # damage through minecraft:fall — and the smash run is that fall with
        # the blow thrown in the middle of it. Creative cannot answer this
        # question at all: Player.causeFallDamage skips a mayfly player.
        step("-> in survival a 6-block fall costs 3; the same fall with a smash costs 0")

        def land_from(height):
            """Drop `height` blocks to the floor and land: the landing's own
            packets, fall damage and all."""
            bot.move_to(0.5, 64.0 + height, 0.5, ground=1)
            drain(bot, 0.15)
            bot.move_to(0.5, 64.0, 0.5, ground=0)  # Entity.checkFallDamage accumulates
            bot.move_to(0.5, 64.0, 0.5, ground=1)  # ...and the landing spends it
            return drain(bot, 0.6)

        def fall_hits(batch):
            """The minecraft:fall hits this batch reports against the player."""
            return [e for e in (read_damage_event(b) for b in all_of(batch, CB_DAMAGE_EVENT))
                    if e["entity"] == bot.entity_id and e["type"] == DMG_FALL]

        def healths_in(batch):
            """The bars this batch carried (set_health is sent whenever health,
            food or the saturation-zero flag moved, so a landing has exactly
            one)."""
            return [read_set_health(b)[0] for b in all_of(batch, CB_SET_HEALTH)]

        # The fight steps left the player short of full health, and both landings
        # below have to be survived, so the bar goes back to 20 the way a player
        # would put it there: instant_health's onEffectStarted heal, which is the
        # same path a potion takes. The food bar is left alone, and that is what
        # keeps natural regeneration out of the arithmetic — at the 17 those
        # fights left it at, both of FoodData.tick's gates are closed (`food >=
        # 18` for the slow bar, `food >= 20` behind saturation for the fast one),
        # so the only thing that moves health between the two landings is the
        # fall.
        step("-> /effect give MobBot instant_health, then two identical 6-block falls")
        send_chat_command(bot, "effect give MobBot instant_health 3 10")
        healed = drain(bot, 0.6)
        assert "Applied Instant health" in feedback(healed), feedback(healed)
        assert all(h <= 20.0 for h in healths_in(healed)), healths_in(healed)

        first = land_from(6.0)
        # LivingEntity.hurt's invulnerability window: a second hit inside ten
        # ticks only lands the part that exceeds the previous one, and two
        # identical 3-damage landings exceed nothing. Twelve ticks with nothing
        # moving — the flying packet has no handler, so these are the survival
        # clock and nothing else — is what lets the second landing land at all.
        tick(bot, 12)
        second = land_from(6.0)
        for batch in (first, second):
            assert len(fall_hits(batch)) == 1, (
                f"a 6-block landing is exactly one minecraft:fall hit: {batch}")
        # The heal's own bar can be the first thing in the first window, so each
        # landing is read as the *last* bar its window left behind.
        bars = [healths_in(b)[-1] for b in (first, second)]
        assert abs((bars[0] - bars[1]) - 3.0) < 0.01, (
            f"floor(6 + 1e-6 - 3) = 3 damage a landing: {bars}")
        step(f"  OK each plain fall took {bars[0] - bars[1]:.1f}: "
             f"{bars[0]} -> {bars[1]}")

        # The same six blocks, with the blow thrown in the middle of the descent:
        # the charge happens standing at the top, the descent is the last word on
        # where the player is (`Entity.checkFallDamage` accumulates on the way
        # down and spends on the ground), and the attack is sent from the bottom
        # of it, which is the only place a smash can be made from.
        zombie, _ = summon_one()
        bot.move_to(0.5, 70.0, 0.5, ground=1)
        drain(bot, 0.15)
        tick(bot, 34)  # the mace's full charge, still standing at the top
        bot.move_to(0.5, 64.0, 0.5, ground=0)
        bot.send(CB_SB_ATTACK, varint(zombie["entity"]))
        smashed_at = drain(bot, 0.6)
        assert DMG_MACE_SMASH in [read_damage_event(b)["type"]
                                  for b in all_of(smashed_at, CB_DAMAGE_EVENT)], smashed_at
        # The victim goes before the landing, so the only thing left in the
        # landing's window is the landing: a zombie that got a hit in first would
        # otherwise be read as the fall damage this step is looking for.
        send_chat_command(bot, "mobs kill")
        removed = drain(bot, 0.5)
        # A zombie that got a hit in before it was removed is folded in here, so
        # that the landing below is judged against the health it actually starts
        # from rather than against the control's.
        seen_bars = healths_in(smashed_at) + healths_in(removed)
        before = seen_bars[-1] if seen_bars else bars[-1]
        assert before > 0.0, f"the smasher must survive its own smash: {seen_bars}"
        bot.move_to(0.5, 64.0, 0.5, ground=1)
        landed = drain(bot, 0.6)
        assert not fall_hits(landed), (
            f"postHurtEnemy resets the fall distance, so the landing is free: {landed}")
        after = healths_in(landed)
        assert all(h >= before for h in after), (
            f"the landing must cost nothing: {before} -> {after}")
        step(f"  OK the same 6-block fall after a smash: no minecraft:fall, "
             f"health still {before}")

        # --- 8b. a mob that falls out of the world is discarded ---------------
        # `Entity.checkBelowWorld`, in `Entity.baseTick` just after the fire
        # block: an entity whose feet fall more than 64 blocks under the
        # dimension's min build height is taken by `onBelowWorld()` ->
        # `discard()`. The overworld's min build height is -64, so the floor is
        # y = -128. This world is the void (the only solid cells are the 9x9 the
        # test laid), so a mob summoned off its edge falls freely — and the
        # discard is silent: a remove_entities and nothing else, no death entity
        # event, no loot, no experience.
        step("-> a mob summoned over the void is discarded below y = -128")
        # Stand clear of the platform: `/summon` places the mob on a ring two
        # blocks from the player at the player's own y, so from out here the ring
        # is over the void and the mob has nothing to land on.
        send_chat_command(bot, "tp 20.5 64.0 0.5")
        drain(bot, 0.4)
        send_chat_command(bot, "summon pig 1")
        packets = drain(bot, 1.2)
        faller = next((read_spawn(b) for b in all_of(packets, CB_SPAWN_ENTITY)), None)
        assert faller is not None and faller["type"] == ET_PIG, faller
        assert faller["y"] > 0.0, f"the pig must start in the air: {faller}"
        step(f"  summoned a pig at y {faller['y']:.1f}; watching it fall out")
        # The world clock is real time, and the pig reaches y = -128 in about
        # four seconds at the 3.0-block terminal velocity. Poll until the
        # remove_entities lands; a discard carries no death event and no drops.
        removed = []
        death_events = []
        drops = []
        deadline = time.time() + 20.0
        while time.time() < deadline and faller["entity"] not in removed:
            batch = drain(bot, 0.4)
            for body in all_of(batch, CB_REMOVE_ENTITIES):
                removed.extend(read_remove_entities(body))
            for body in all_of(batch, CB_ENTITY_EVENT):
                eid, event = read_entity_event(body)
                if eid == faller["entity"]:
                    death_events.append(event)
            drops.extend(read_spawn(b) for b in all_of(batch, CB_SPAWN_ENTITY))
        assert faller["entity"] in removed, (
            f"the pig fell out of the world but was never removed: {removed}")
        assert not death_events, (
            f"a below-world discard is not a death: entity event(s) {death_events}")
        assert not any(d["type"] in (ET_ITEM, ET_EXPERIENCE_ORB) for d in drops), (
            f"a discard leaves no loot or orbs: {[d['type'] for d in drops]}")
        step("  OK the fallen pig was discarded (remove_entities, no death, no drops)")

        # --- 8h. armour durability, and the Thorns entry ----------------------
        # `Player.hurtArmor` -> `LivingEntity.doHurtEquipment` was the one part of
        # the damage pipeline with no model at all: a worn piece never lost a
        # point, which is also why the Thorns post_attack entry's
        # `change_item_damage` (2 durability) was resolved and tested but never
        # written back (docs/PHASE4.md, milestone G). Both are live now, and this
        # step reads them off the wire — the wear's own slot resync.
        #
        # The piece is equipped with a *creative* slot write rather than by
        # clicking, which is also how it arrives already enchanted: the creative
        # screen's armour slots are menu slots 5..8 (head, chest, legs, feet) and
        # the patch that packet carries is the *delimited* one a real client sends
        # (`ItemStack.OPTIONAL_UNTRUSTED_STREAM_CODEC`), so one packet is one
        # worn, enchanted chestplate.
        step("-> a Thorns III diamond chestplate, equipped through the chest slot")
        send_chat_command(bot, "gamemode creative")
        assert "creative" in feedback(drain(bot, 0.8)).lower(), "not creative"
        enchantments = varint(1) + varint(ENCHANT_THORNS) + varint(3)
        bot.send(SB_SET_CREATIVE_SLOT,
                 struct.pack(">h", MENU_CHEST_ARMOR) + varint(1)
                 + varint(ITEM_DIAMOND_CHESTPLATE) + varint(1) + varint(0)
                 + varint(COMPONENT_ENCHANTMENTS) + varint(len(enchantments))
                 + enchantments)
        worn = drain(bot, 0.6)
        equipped = slot_stack(worn, MENU_CHEST_ARMOR)
        assert equipped is not None, f"the write must be echoed: {worn}"
        item, count, damage, ench = equipped
        assert (item, count, damage) == (ITEM_DIAMOND_CHESTPLATE, 1, 0), equipped
        assert ench == [(ENCHANT_THORNS, 3)], f"the piece arrives enchanted: {ench}"
        step("  OK worn at 0 damage, Thorns III, and the echo carries the component")

        # --- the wear, on a number the test controls --------------------------
        # `/damage` is the one damage path with no attacker, so nothing here can
        # fire Thorns and the durability movement is `hurtArmor` alone.
        step("-> /damage 5 takes one point, then a windowed 20 takes three more")
        send_chat_command(bot, "gamemode survival")
        assert "survival" in feedback(drain(bot, 0.8)).lower(), "not survival"
        # The fight steps left the player short; Instant Health puts the bar back
        # so the two hits below are not what kills this run.
        send_chat_command(bot, "effect give MobBot instant_health 3 10")
        assert "Applied Instant health" in feedback(drain(bot, 0.6)), "no heal"
        send_chat_command(bot, "damage MobBot 5 minecraft:player_attack")
        first = drain(bot, 0.6)
        assert "Applied 5 damage" in feedback(first), feedback(first)
        assert chest_wear(first) == 1, (
            f"5 damage is (int)max(1, 5/4) = one point: {chest_wear(first)}")
        # The second hit arrives inside the invulnerability window (the survival
        # clock is one packet — `PLUSMC_SURVIVAL_CLOCK=packet` — and the command
        # above was the only packet since), so LivingEntity.hurt lands only the
        # excess and `getDamageAfterArmorAbsorb` charges hurtArmor *that* number:
        # (20 - 5) / 4 = 3 more points, where the command's own 20/4 = 5 would be
        # a fifth of nothing the health bar ever saw.
        send_chat_command(bot, "damage MobBot 20 minecraft:player_attack")
        second = drain(bot, 0.6)
        assert "Applied 20 damage" in feedback(second), feedback(second)
        assert chest_wear(second) == 4, (
            f"a windowed 20 lands 15, i.e. three more points: {chest_wear(second)}")
        step("  OK the chestplate went 0 -> 1 -> 4: 5 landed, then the 15 of a "
             "windowed 20")

        # --- the thorns branch, on a real mob's melee ------------------------
        # This is the path no test reached before: `drain_mob_damage` applies the
        # mob's hit through the damage sink and then runs the victim-side
        # post_attack entries, so the zombie takes `minecraft:thorns` *and* the
        # chestplate takes the entry's own `change_item_damage` of 2.
        step("-> a zombie's melee: one point a landed hit, two more a Thorns trigger")
        send_chat_command(bot, "tp 0.5 64.0 0.5")
        drain(bot, 0.3)
        # Resistance IV so the ledger below cannot be cut short by a death. The
        # reduction lands *after* the armour step in vanilla's order
        # (getDamageAfterMagicAbsorb), so it does not touch what the armour is
        # charged — which is exactly what this step is measuring.
        send_chat_command(bot, "effect give MobBot resistance 999 4")
        assert "Applied Resistance" in feedback(drain(bot, 0.6)), "no resistance"

        def armed_zombie():
            """One zombie on the ring, with the batch it arrived in."""
            send_chat_command(bot, "mobs kill")
            drain(bot, 0.4)
            send_chat_command(bot, "summon zombie 1")
            batch = drain(bot, 0.6)
            spawned = [s for s in (read_spawn(b) for b in all_of(batch, CB_SPAWN_ENTITY))
                       if s["type"] == ET_ZOMBIE]
            assert spawned, f"no zombie spawn: {batch}"
            return spawned[0], batch

        ledger = {"wear": 4, "hits": 0, "thorns": 0}
        for _attempt in range(4):
            if ledger["thorns"]:
                break
            zombie, batch = armed_zombie()
            for _slice in range(60):
                tick(bot, 6)
                batch = drain(bot, 0.25)
                landed = thorns = 0
                for body in all_of(batch, CB_DAMAGE_EVENT):
                    event = read_damage_event(body)
                    if event["entity"] == bot.entity_id and event["type"] == DMG_MOB_ATTACK:
                        landed += 1
                    if event["entity"] == zombie["entity"] and event["type"] == DMG_THORNS:
                        # `new DamageSource(type, item.owner())`: the wearer is the
                        # *cause*, which is what credits the kill to the player.
                        assert event["cause"] == bot.entity_id, event
                        thorns += 1
                if landed or thorns:
                    got = chest_wear(batch)
                    assert got is not None, (
                        "the wear prints its own slot resync: no chest slot here")
                    want = ledger["wear"] + landed + 2 * thorns
                    assert got == want, (
                        f"one point a landed hit and two a Thorns trigger: {got} "
                        f"!= {want} ({landed} hit(s), {thorns} trigger(s))")
                    ledger["wear"] = got
                    ledger["hits"] += landed
                    ledger["thorns"] += thorns
                died = any(eid == zombie["entity"] and event == ENTITY_EVENT_DEATH
                           for eid, event in (read_entity_event(b)
                                              for b in all_of(batch, CB_ENTITY_EVENT)))
                if ledger["thorns"] or died:
                    break
        send_chat_command(bot, "mobs kill")
        drain(bot, 0.4)
        assert ledger["hits"] >= 1, "the zombie never landed a hit on the player"
        assert ledger["thorns"] >= 1, (
            f"Thorns III (45% a hit) never fired in {ledger['hits']} hits — the "
            f"session's branch is unreached")
        step(f"  OK {ledger['hits']} mob hit(s), {ledger['thorns']} Thorns trigger(s): "
             f"the chestplate is at {ledger['wear']} damage")

        # Leave the run as it was found: the resistance and the chestplate would
        # both change the death step below (a Thorns piece can kill the zombie
        # before it finishes the player off).
        step("-> strip the plate and clear the effects for the step below")
        send_chat_command(bot, "effect clear MobBot")
        assert "Removed every effect" in feedback(drain(bot, 0.6)), "effects left"
        send_chat_command(bot, "gamemode creative")
        drain(bot, 0.5)
        bot.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", MENU_CHEST_ARMOR) + varint(0))
        stripped = drain(bot, 0.6)
        assert slot_stack(stripped, MENU_CHEST_ARMOR) == (0, 0, 0, []), stripped
        send_chat_command(bot, "gamemode survival")
        assert "survival" in feedback(drain(bot, 0.8)).lower(), "not survival"
        step("  OK no effects, nothing worn")

        # --- 9. the mob can kill the player ----------------------------------
        # The other half of "players can fight": a hostile mob that keeps landing
        # hits ends the player, and the death message names it. The hits arrive on
        # the victim's packet clock, so this loop is the same shape as step 3's.
        step("-> a zombie must be able to finish the player off (death.attack.mob)")
        # Back to the middle of the floor first: the pig phase ends wherever the
        # pig happened to die (it wanders), and /summon puts the zombie two blocks
        # east of the player — off the platform, into the void, if that is near the
        # edge. From the middle the summon lands on the platform every run.
        send_chat_command(bot, "tp 0.5 64.0 0.5")
        drain(bot, 0.4)
        send_chat_command(bot, "summon zombie 1")
        packets = drain(bot, 1.2)
        killer = next((read_spawn(b) for b in all_of(packets, CB_SPAWN_ENTITY)), None)
        assert killer is not None and killer["type"] == ET_ZOMBIE, killer
        # The kill can already have landed in the *summon's* own drain: the mob
        # tick runs on the server's real 50 ms clock while the player's damage
        # only lands on the packet clock (`drain_mob_damage`), so a zombie that
        # closes in during that 1.2 s has its hits queued and the first of them
        # is applied by the first packet the test sends afterwards. A player left
        # low by the fight with the first zombie can be finished off by it, and
        # both death packets are one-shot: a batch read and thrown away takes
        # them with it, and no later drain will ever see them again.
        kill = None
        dead = False
        healths = [read_set_health(b) for b in all_of(packets, CB_SET_HEALTH)]

        def scan(batch):
            """Fold one batch into the kill/death/health state above."""
            nonlocal kill, dead
            healths.extend(read_set_health(b) for b in all_of(batch, CB_SET_HEALTH))
            for body in all_of(batch, CB_PLAYER_COMBAT_KILL):
                kill = read_player_combat_kill(body)
            for eid, event in [read_entity_event(b) for b in all_of(batch, CB_ENTITY_EVENT)]:
                if eid == bot.entity_id and event == ENTITY_EVENT_DEATH:
                    dead = True

        scan(packets)
        for _iter in range(90):
            if kill is not None and dead:
                break
            tick(bot, 12)
            scan(drain(bot, 0.15))
        assert kill is not None, "the zombie never finished the player off"
        pid, key, args = kill
        # DamageSource.getLocalizedDeathMessage: with a cause entity the key is the
        # bare "death.attack." + msgId, and the attacker's display name is the
        # second argument. minecraft:mob_attack's msgId is "mob", so this is
        # "death.attack.mob" — not "death.attack.mob.player", which no lang file
        # has and the client would print as the raw key.
        assert pid == bot.entity_id and key == "death.attack.mob", kill
        assert args == [bot.name, "Zombie"], kill
        assert dead, "the death must also play the entity event"
        assert healths and healths[-1][0] == 0.0, f"death must report 0 health: {healths}"
        step(f"  OK the zombie killed the player: {key}{tuple(args)}")

        bot.close()
    finally:
        if proc is not None:
            proc.terminate()
            proc.wait(timeout=5)
    print("\nPASS: mobs test")
    return 0


if __name__ == "__main__":
    sys.exit(main())
