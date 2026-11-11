#!/usr/bin/env python3
"""Integration test: Phase 3 milestone F over the real wire protocol.

One survival bot joins and the server has to *actually* run the survival layer:

  * the join pushes change_difficulty (0x0a), the player attribute set (0x86,
    max_health = 20 with no armour modifiers) and the first set_health (0x68),
  * `/damage` raises damage_event (0x19) with the registry id of the type, the
    hurt animation (0x2a) and a set_health carrying the new health,
  * equipping a diamond chestplate — a real container click into the player
    menu's chest slot — pushes the armour attribute modifier (0x86) and tells
    the *other* player with set_equipment (0x66),
  * armour really reduces the next hit (the CombatRules formula, on the wire),
  * a lethal hit runs the death sequence: player_combat_kill (0x44) with the
    translated death message, the death entity_event (0x22), set_health 0,
  * the respawn click (client_command 0x0c action 0) sends respawn (0x52), the
    death location, the restored health and the dropped inventory,
  * creative's `abilities.invulnerable` refuses /damage but not /kill,
  * the food bar drains from sprinting (exhaustion -> saturation -> food) and an
    eaten cooked beef refills it — including the refusal to eat while full.

The survival tick is the packet clock, and the server is told to *count packets*
rather than wall time (`PLUSMC_SURVIVAL_CLOCK=packet`): one recognized serverbound
packet is exactly one game tick, and idle wall time contributes none. So the
test decides every tick it asserts about — the food bar's exhaustion spend, a
cooked beef's 32-tick eat — instead of relying on its own drain windows to have
banked enough of them. Where a tick count *is* the assertion,
`wire_client.barrier` (a ping whose pong cannot be written until the server has
processed every packet sent before it) makes it two-sided: one tick short of the
32 does not finish the eat, the next one does.

Run: health_test.py /path/to/plusmc
"""
import os, re, socket, struct, subprocess, sys, tempfile, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nbt_component import chat_text, chat_translate, rd_nbt_translate  # noqa: E402
import wire_client
from wire_client import make_frame, read_packet

PROTO = 777
HOST = "127.0.0.1"

# Registry ids (generated item table, src/world/item_registry.h).
ITEM_DIAMOND_CHESTPLATE = 1085   # armour 8, toughness 2
ITEM_COOKED_BEEF = 1261         # nutrition 8, saturation 12.8, 32 consume ticks

# Registry ids (generated damage table, src/world/damage_table.h: wire order).
DMG_PLAYER_ATTACK = 34
DMG_GENERIC_KILL = 19

# Clientbound packet ids.
CB_CHANGE_DIFFICULTY = 0x0A
CB_CONTAINER_SET_CONTENT = 0x12
CB_CONTAINER_SET_SLOT = 0x14
CB_DAMAGE_EVENT = 0x19
CB_ENTITY_EVENT = 0x22
CB_HURT_ANIMATION = 0x2B
CB_PLAYER_COMBAT_KILL = 0x45
CB_PLAYER_POSITION = 0x49
CB_RESPAWN = 0x54
CB_SET_EQUIPMENT = 0x68
CB_SET_ENTITY_DATA = 0x65
CB_SET_HEALTH = 0x6A
CB_SPAWN_ENTITY = 0x01
CB_SYSTEM_CHAT = 0x7C
CB_UPDATE_ATTRIBUTES = 0x86
CB_KEEP_ALIVE = 0x2D

# Serverbound packet ids.
SB_FLYING = 0x21
SB_PLAYER_COMMAND = 0x2A
SB_CLIENT_COMMAND = 0x0C
SB_USE_ITEM = 0x43
SB_CONTAINER_CLICK = 0x12
SB_CHAT = 0x09
SB_CHAT_COMMAND = 0x07
SB_KEEP_ALIVE = 0x1C
SB_SET_CARRIED_ITEM = 0x36

# Attribute ids (minecraft:attribute registration order, attribute_table.h).
ATTR_ARMOR = 1
ATTR_ARMOR_TOUGHNESS = 2
ATTR_MAX_HEALTH = 23

# Player menu slots: 0 result, 1..4 the 2x2 grid, 5..8 armour (head..feet),
# 9..35 main, 36..44 hotbar, 45 offhand.
SLOT_MENU_CHEST = 6
SLOT_MENU_HOTBAR_FIRST = 36

EQUIPMENT_CHEST = 4
EQUIPMENT_MAINHAND = 0
ENTITY_EVENT_DEATH = 3

# Synched-data serializers and the health word's index (LivingEntity's accessors
# are 8..14, and DATA_HEALTH_ID is the second of them).
SER_BYTE = 0
SER_FLOAT = 3
SER_PARTICLES = 17
# minecraft:entity_effect, the one particle type in an effect's particle list
# whose codec carries a payload (the ARGB colour).
PARTICLE_ENTITY_EFFECT = 28
HEALTH_WORD_INDEX = 9
# Player's own first accessor, after Avatar's two (main hand 15, skin mask 16).
ABSORPTION_INDEX = 17


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

def read_stack(body, pos):
    """ItemStack.OPTIONAL_STREAM_CODEC -> ((item, count), new_pos)."""
    count, pos = rd_varint(body, pos)
    if count <= 0:
        return (0, 0), pos
    item, pos = rd_varint(body, pos)
    # The two patch counts come first, then the entries. A component *value* has
    # no length on the wire, so a reader that does not model one cannot skip it:
    # an empty patch is asserted rather than skipped, so a component-carrying
    # stack is a failure here instead of a misread one
    # (container_gui_test.read_stack_full decodes them).
    added, pos = rd_varint(body, pos)
    removed, pos = rd_varint(body, pos)
    assert added == 0 and removed == 0, f"unmodelled stack component patch {added}/{removed}"
    return (item, count), pos


def read_set_health(body):
    """: [f32 health][varint food][f32 saturation], and nothing else."""
    health = struct.unpack(">f", body[0:4])[0]
    food, pos = rd_varint(body, 4)
    saturation = struct.unpack(">f", body[pos:pos + 4])[0]
    assert pos + 4 == len(body), f"set_health: {len(body) - pos - 4} trailing byte(s)"
    return round(health, 4), food, round(saturation, 4)


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


def read_damage_event(body):
    """: [varint entity][varint type][varint cause+1][varint direct+1][bool
    has position][3 doubles]."""
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
    return {
        "entity": eid,
        "type": type_id,
        "cause": cause - 1,
        "direct": direct - 1,
        "has_position": bool(has_position),
    }


def read_entity_event(body):
    """: [i32 entity id][u8 event] — the one non-varint entity id here."""
    assert len(body) == 5, f"entity_event body is {len(body)} bytes"
    return struct.unpack(">i", body[0:4])[0], body[4]


def read_equipment(body):
    """: [varint entity][u8 slot | 0x80][stack]... — the continuation bit ends it."""
    eid, pos = rd_varint(body, 0)
    entries = []
    while True:
        raw = body[pos]
        pos += 1
        slot = raw & 0x7F
        more = (raw & 0x80) != 0
        stack, pos = read_stack(body, pos)
        entries.append((slot, stack))
        if not more:
            break
    assert pos == len(body), f"set_equipment: {len(body) - pos} trailing byte(s)"
    return eid, entries


def unpack_block_pos(packed):
    """BlockPos.getX/getY/getZ: 26 bits of x, 12 of y, 26 of z, signs included.

    Masking each field out and *then* putting its sign back is what Java's
    `(int)((l << 38) >> 38)` does. Doing the bit test without the mask is wrong
    for a negative x — Python's `-1 & (1 << 25)` is truthy, so x = -1 came back
    as -67108865 — and reading x straight off the shift only works while the
    argument is the signed (`">q"`) read. The shared codec
    (`wire_client.unpack_block_pos`) is the reference; this one matches it."""
    packed &= 0xFFFFFFFFFFFFFFFF
    x = (packed >> 38) & 0x3FFFFFF
    z = (packed >> 12) & 0x3FFFFFF
    y = packed & 0xFFF
    if x & (1 << 25):
        x -= 1 << 26
    if z & (1 << 25):
        z -= 1 << 26
    if y & (1 << 11):
        y -= 1 << 12
    return x, y, z


def read_respawn(body):
    """CommonPlayerSpawnInfo + dataToKeep -> a dict."""
    dim_type, pos = rd_varint(body, 0)
    dimension, pos = rd_string(body, pos)
    seed = struct.unpack(">q", body[pos:pos + 8])[0]
    pos += 8
    game_mode, previous = body[pos], body[pos + 1]
    pos += 2
    is_debug, is_flat = body[pos], body[pos + 1]
    pos += 2
    death = None
    if body[pos] == 1:
        pos += 1
        dim, pos = rd_string(body, pos)
        death = (dim, struct.unpack(">q", body[pos:pos + 8])[0])
        pos += 8
    else:
        pos += 1
    portal, pos = rd_varint(body, pos)
    sea, pos = rd_varint(body, pos)
    keep = body[pos]
    pos += 1
    assert pos == len(body), f"respawn: {len(body) - pos} trailing byte(s)"
    return {
        "dimension_type": dim_type, "dimension": dimension, "seed": seed,
        "game_mode": game_mode, "previous": previous, "debug": is_debug,
        "flat": is_flat, "death": death, "portal": portal, "sea": sea, "keep": keep,
    }


def read_player_combat_kill(body):
    """: [varint player id][component] -> (id, key, args). Unlike system_chat
    there is no overlay byte here, so the component runs to the end."""
    eid, pos = rd_varint(body, 0)
    key, args, end = rd_nbt_translate(body, pos)
    assert end == len(body), f"player_combat_kill: {len(body) - end} trailing byte(s)"
    return eid, key, args


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
        # Every packet `join` read, handshake included: the newcomer's
        # first view of the world is inside it (see `join`).
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
        """The full offline join: login, configuration, then the first play
        packets — which is where the burst a newcomer is sent about the world
        lives: the tab entries, spawns, synched data and equipment of every
        player already in it (`PlayerList.placeNewPlayer`). That burst is read
        *here* (this loop ends at the chunk batch's finish), so it is kept in
        `join_packets` rather than discarded — a test asserting on a joining
        client's first view of an existing player has no other place to look,
        because the next drain starts with the recipe book and the player menu,
        which the server sends later.
        """
        def read():
            pid, body = self.recv()
            self.join_packets.append((pid, body))
            return pid, body

        hs = (varint(0) + varint(PROTO) + varint(len(HOST)) + HOST.encode()
              + struct.pack(">H", 0) + varint(2))
        self.sock.sendall(make_frame(hs, -1))
        self.send(0x00, varint(len(self.name)) + self.name.encode() + b"\x00" * 16)
        while True:
            pid, body = read()
            if pid == 0x03:
                self.threshold, _ = rd_varint(body)
            elif pid == 0x02:
                self.send(0x03)
                break
            else:
                raise AssertionError(f"unexpected login packet 0x{pid:02x}")
        while True:
            pid, body = read()
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
            pid, _ = read()
            if pid == 0x32:  # store cookie? (config finished marker)
                break
        for _ in range(400):
            pid, body = read()
            if pid == 0x2D:  # keep-alive during configuration
                self.send(0x1C, body[:8])
            elif pid == 0x0B:  # finish configuration
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
def find(packets, pid):
    for p, body in packets:
        if p == pid:
            return body
    return None


def all_of(packets, pid):
    return [b for p, b in packets if p == pid]


def chat_literals(packets):
    """The SystemChats that carry a literal component (command feedback).

    A translate component (a death message) is not a literal, so it is skipped
    rather than mis-decoded: the two shapes are checked by their own helpers.
    """
    out = []
    for body in all_of(packets, CB_SYSTEM_CHAT):
        try:
            out.append(chat_text(body))
        except AssertionError:
            pass
    return out


def chat_translated(packets):
    """The SystemChats that carry a `{translate, with}` component."""
    out = []
    for body in all_of(packets, CB_SYSTEM_CHAT):
        try:
            out.append(chat_translate(body))
        except AssertionError:
            pass
    return out


def health_around_damage(packets):
    """(health before the hit, health after) from a drain around one hit.

    The order inside the drain is the server's: the tick batch (and any
    regeneration it pays for) first, then the damage_event, then the health
    packet the hit raised. Anything after that is more regeneration, so the
    hit has to be picked out by its position rather than by being the last
    set_health in the list.
    """
    before = None
    after = None
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


def float_words(packets, eid, wanted):
    """Every FLOAT `set_entity_data` carried for `eid` at synched-data `wanted`.

    An entry this decoder does not know is a failure rather than a guess.
    """
    out = []
    for body in all_of(packets, CB_SET_ENTITY_DATA):
        got, pos = rd_varint(body, 0)
        if got != eid:
            continue
        while True:
            index = body[pos]
            pos += 1
            if index == 0xFF:
                break
            serializer, pos = rd_varint(body, pos)
            if serializer == SER_BYTE:
                pos += 1
            elif serializer == SER_FLOAT:
                value = struct.unpack(">f", body[pos:pos + 4])[0]
                pos += 4
                if index == wanted:
                    out.append(value)
            elif serializer == SER_PARTICLES:
                # `LivingEntity.DATA_EFFECT_PARTICLES` (index 10): a varint count,
                # then each entry's particle type id and that type's own payload
                # — the ARGB colour for minecraft:entity_effect (28), nothing at
                # all for a bare SimpleParticleType. This file grants absorption,
                # which is a visible effect, so its list is on the wire here.
                count, pos = rd_varint(body, pos)
                for _ in range(count):
                    ptype, pos = rd_varint(body, pos)
                    if ptype == PARTICLE_ENTITY_EFFECT:
                        pos += 4
            else:
                raise AssertionError(f"entity data index {index}: serializer {serializer}")
        assert pos == len(body), f"set_entity_data: {len(body) - pos} trailing byte(s)"
    return out


def health_words(packets, eid):
    """Every health word (`LivingEntity.DATA_HEALTH_ID`, index 9) `packets`
    carried for `eid`, in order.

    A living entity's health is synched data, and vanilla's tracker flushes it
    to the players watching the entity (`ServerEntity.sendDirtyEntityData` ->
    `sendToTrackingPlayersAndSelf`, with no player-specific arm) and packs the
    same non-default values for a new viewer (`sendPairingData`). So this is
    where one player's health reaches another; the owner's own bar is the
    separate set_health.
    """
    return float_words(packets, eid, HEALTH_WORD_INDEX)


def absorption_words(packets, eid):
    """Every absorption word (`Player.DATA_PLAYER_ABSORPTION_ID`, index 17)
    `packets` carried for `eid`, in order.

    The same mechanism as the health word, and the *only* packet that carries
    this number: set_health is health, food and saturation, so the client's own
    yellow hearts come from here.
    """
    return float_words(packets, eid, ABSORPTION_INDEX)


def tick(bot, count):
    """Advance the survival clock by exactly `count` game ticks: one `flying`
    packet each, which under `PLUSMC_SURVIVAL_CLOCK=packet` is one tick each.

    There is no sleep between them, because there is nothing to wait for: this
    test's world is empty (no mob, orb, furnace or weather ticks on the server's
    own clock), so the survival tick is the only clock it has to pace. Where a
    tick count is asserted, `wire_client.barrier` is used instead of this, so the
    assertion can be two-sided.
    """
    for _ in range(count):
        bot.send(SB_FLYING, bytes([1]))


def send_chat_command(bot, command):
    payload = command.encode()
    bot.send(SB_CHAT_COMMAND, varint(len(payload)) + payload)


START = time.time()


def log_line(text):
    sys.stderr.write(f"[{time.time() - START:6.2f}s] {text}")
    sys.stderr.flush()


def step(text):
    print(f"[{time.time() - START:6.2f}s] {text}")


def wait_listening(proc, timeout=20):
    """Wait for the server's listening line, and keep printing its log after
    that: a disconnect raised in the middle of the test is only diagnosable
    with the server's own output ("timed out", "unknown packet", ...)."""
    deadline = time.time() + timeout
    port = None
    while time.time() < deadline:
        line = proc.stdout.readline().decode(errors="replace")
        if line:
            log_line(line)
            m = re.search(r"listening on.*:(\d+)", line)
            if m:
                port = int(m.group(1))
                break

    def pump():
        for raw in iter(proc.stdout.readline, b""):
            log_line(raw.decode(errors="replace"))

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


def check_join(bot, packets):
    """The join must push the difficulty, the attribute set and the health."""
    diff = find(packets, CB_CHANGE_DIFFICULTY)
    assert diff is not None, f"no change_difficulty at join: {[hex(p) for p, _ in packets]}"
    # The body is [u8 difficulty][bool locked] — the packet id is already
    # stripped by read_packet.
    assert diff[0] == 2 and diff[1] == 0, f"the default difficulty is normal (2): {diff.hex()}"

    attrs = all_of(packets, CB_UPDATE_ATTRIBUTES)
    assert attrs, f"no update_attributes at join: {[hex(p) for p, _ in packets]}"
    eid, values = read_attributes(attrs[-1])
    bot.entity_id = eid
    assert values[ATTR_MAX_HEALTH][0] == 20.0, f"max_health {values.get(ATTR_MAX_HEALTH)}"
    assert values[ATTR_ARMOR][0] == 0.0 and values[ATTR_ARMOR][1] == [], (
        f"a fresh player has no armour: {values.get(ATTR_ARMOR)}")
    assert values[ATTR_ARMOR_TOUGHNESS][0] == 0.0, values.get(ATTR_ARMOR_TOUGHNESS)

    healths = [read_set_health(b) for b in all_of(packets, CB_SET_HEALTH)]
    assert healths, f"no set_health at join: {[hex(p) for p, _ in packets]}"
    assert healths[-1] == (20.0, 20, 5.0), f"a fresh player: {healths[-1]}"
    step(f"  OK join: attributes on entity {eid}, health {healths[-1]}")


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-health-")
    env = dict(os.environ)
    # The survival clock counts *packets*: one game tick each, with no wall-time
    # catch-up. A game tick used to happen whenever a recognized packet turned
    # banked wall time into one (up to twenty per packet), which made the food
    # bar's exhaustion spend, a 32-tick eat and the regeneration between two
    # observations a function of how long the test spent draining. Under this
    # clock they are the packets the test sends. Keep-alives are slowed to their
    # clamped maximum so the only clock in play is the test's own.
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"

    proc = None
    try:
        step(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", "127.0.0.1", "--port", "0", "--motd", "health test",
             "--offline-mode", "--gamemode", "0", "--data", data_root, "--world", world,
             "--no-flat-world"], wait_listening, env=env)

        bot = Bot("HealthBot", port)
        wire_client.record(bot, note="health: damage, armour, food and respawn")
        bot.join()
        bot.start_reader()
        check_join(bot, drain(bot, 1.2))

        viewer = Bot("HealthViewer", port)
        wire_client.record(viewer, note="health: the client that watches the fight")
        viewer.join()
        viewer.start_reader()
        step("  viewer joined")

        # --- 1. a plain hit, no armour ---------------------------------------
        step("-> /damage HealthBot 6 player_attack")
        send_chat_command(bot, "damage HealthBot 6 player_attack")
        packets = drain(bot, 1.5)
        feedback = " ".join(chat_literals(packets))
        assert "Applied 6 damage" in feedback, f"missing feedback: {feedback!r}"
        events = all_of(packets, CB_DAMAGE_EVENT)
        assert events, f"no damage_event: {[hex(p) for p, _ in packets]}"
        event = read_damage_event(events[-1])
        assert event == {"entity": bot.entity_id, "type": DMG_PLAYER_ATTACK, "cause": -1,
                         "direct": -1, "has_position": False}, event
        assert find(packets, CB_HURT_ANIMATION) is not None, "no hurt animation"
        healths = [read_set_health(b) for b in all_of(packets, CB_SET_HEALTH)]
        assert healths and healths[-1] == (14.0, 20, 5.0), f"health after 6 damage: {healths}"
        step(f"  OK damage_event type {event['type']}, health {healths[-1][0]}")

        # The guard: exactly one set_health per hit, not one per tick.
        assert len(healths) == 1, f"the health packet must fire once, got {len(healths)}"
        # The owner is not sent the word: its own copy of the number is the bar
        # above (vanilla's only self-writer of this value), so index 9 on this
        # stream would be a second writer of one value.
        assert health_words(packets, bot.entity_id) == [], \
            "the health word is for the other clients; the owner's bar is set_health"
        # The viewer's own arrival is on the *bot's* stream: the clients already
        # in the world are sent an arriving player's synched values — vanilla's
        # `ServerEntity.sendPairingData` packs every value that differs from its
        # default, and health's default is 1.0 — so both directions of the
        # pairing are the same packet, and the spawn that precedes it names the
        # entity. (The newcomer's own view of the player already here is pinned
        # in tests/join_test.py, whose client keeps the join burst this test's
        # `Bot.join` consumes.)
        spawn = find(packets, CB_SPAWN_ENTITY)
        assert spawn is not None, \
            f"the bot must be told the viewer spawned: {[hex(p) for p, _ in packets]}"
        viewer_eid, _ = rd_varint(spawn, 0)
        assert viewer_eid != bot.entity_id, "the spawn in this window is the viewer's"
        words = health_words(packets, viewer_eid)
        assert words == [20.0], \
            f"the arriving player's health word must reach the clients already here: {words}"
        step(f"  OK the bot was told the arriving player's health ({words[0]})")
        # ...and the watcher, which has no bar for this player at all: the only
        # health it holds is the synched word, and the hit has to move it.
        viewer_packets = drain(viewer, 1.0)
        words = health_words(viewer_packets, bot.entity_id)
        assert words and words[-1] == 14.0, \
            f"the watcher must be sent the player's new health: {words}"
        step(f"  OK the watcher's copy of the health moved with the hit: {words}")
        # From here on the food bar pays for natural regeneration (5/6 of a
        # heart every ten ticks, which every banked packet-tick spends), so the
        # later assertions compare *deltas* against the last health the server
        # reported rather than absolute values.
        last_health = healths[-1][0]

        # --- 2. equip a chestplate (a real click, not a cheat) ---------------
        step("-> /give a diamond chestplate and click it into the chest slot")
        send_chat_command(bot, "give HealthBot minecraft:diamond_chestplate 1")
        packets = drain(bot, 1.0)
        contents = all_of(packets, CB_CONTAINER_SET_CONTENT)
        assert contents, f"/give must resync the inventory: {[hex(p) for p, _ in packets]}"
        window, state, slots, cursor = decode_content(contents[-1])
        assert window == 0 and slots[SLOT_MENU_HOTBAR_FIRST] == (ITEM_DIAMOND_CHESTPLATE, 1), \
            f"the chestplate should be in hotbar 0: {slots[SLOT_MENU_HOTBAR_FIRST]}"

        def latest_state(packets, fallback):
            states = []
            for body in all_of(packets, CB_CONTAINER_SET_SLOT):
                states.append(decode_set_slot(body)[1])
            return max(states) if states else fallback

        bot.send(SB_CONTAINER_CLICK, click_body(window, state, SLOT_MENU_HOTBAR_FIRST, 0, 0))
        packets = drain(bot, 0.8)
        state = latest_state(packets, state)
        bot.send(SB_CONTAINER_CLICK,
                 click_body(window, state, SLOT_MENU_CHEST, 0, 0, (ITEM_DIAMOND_CHESTPLATE, 1)))
        packets = drain(bot, 1.0)
        state = latest_state(packets, state)
        attrs = all_of(packets, CB_UPDATE_ATTRIBUTES)
        assert attrs, f"equipping must push the attributes: {[hex(p) for p, _ in packets]}"
        # The health may have moved by itself here: a hurt player with a full
        # food bar regenerates 5/6 of a heart every ten ticks (FoodData.tick),
        # and every banked packet-tick spends one. Track it so the next step can
        # assert the *damage*, which is the part this test is about.
        seen = [read_set_health(b) for b in all_of(packets, CB_SET_HEALTH)]
        if seen:
            last_health = seen[-1][0]
        _, values = read_attributes(attrs[-1])
        assert values[ATTR_ARMOR][1] == [("minecraft:armor.chest", 8.0, 0)], values[ATTR_ARMOR]
        assert values[ATTR_ARMOR_TOUGHNESS][1] == [("minecraft:armor_toughness.chest", 2.0, 0)], \
            values[ATTR_ARMOR_TOUGHNESS]
        step(f"  OK armour attributes: {values[ATTR_ARMOR][1]}")

        viewer_packets = drain(viewer, 1.0)
        equipment = all_of(viewer_packets, CB_SET_EQUIPMENT)
        assert equipment, f"the other player must see the equipment: " \
                          f"{[hex(p) for p, _ in viewer_packets]}"
        eid, entries = read_equipment(equipment[-1])
        assert eid == bot.entity_id, f"equipment for entity {eid}, wanted {bot.entity_id}"
        assert (EQUIPMENT_CHEST, (ITEM_DIAMOND_CHESTPLATE, 1)) in entries, entries
        step(f"  OK set_equipment to the viewer: {entries}")

        # --- 2b. the *held* item, the equipment packet's other half ----------
        # `LivingEntity.detectEquipmentUpdates` diffs `EquipmentSlot.VALUES` —
        # mainhand and offhand first, then the four armour slots — and
        # `handleEquipmentChanges` sends the changed entries to the tracking
        # players; `ServerEntity.sendPairingData` sends the same set to a new one.
        # The item in another player's hand is carried by that packet and by
        # nothing else: the held stack is not in synched data (`Avatar.
        # DATA_PLAYER_MAIN_HAND`, index 15, is which *arm* is the main one). This
        # server's equipment sync covered the armour slots and nothing more, so
        # `kEquipmentMainHand` was a named constant with no writer and a hotbar
        # switch was invisible to everyone but the player who made it.
        step("-> /give a beef into hotbar 0 and a chestplate into hotbar 1, then switch")
        send_chat_command(bot, "give HealthBot minecraft:cooked_beef 1")
        packets = drain(bot, 1.0)
        contents = all_of(packets, CB_CONTAINER_SET_CONTENT)
        assert contents, f"/give must resync the inventory: {[hex(p) for p, _ in packets]}"
        window, state, slots, cursor = decode_content(contents[-1])
        assert slots[SLOT_MENU_HOTBAR_FIRST] == (ITEM_COOKED_BEEF, 1), \
            f"the beef should be in hotbar 0: {slots[SLOT_MENU_HOTBAR_FIRST]}"
        send_chat_command(bot, "give HealthBot minecraft:diamond_chestplate 1")
        packets = drain(bot, 1.0)
        contents = all_of(packets, CB_CONTAINER_SET_CONTENT)
        assert contents, f"/give must resync the inventory: {[hex(p) for p, _ in packets]}"
        window, state, slots, cursor = decode_content(contents[-1])
        assert slots[SLOT_MENU_HOTBAR_FIRST + 1] == (ITEM_DIAMOND_CHESTPLATE, 1), \
            f"the chestplate should be in hotbar 1: {slots[SLOT_MENU_HOTBAR_FIRST + 1]}"
        drain(viewer, 0.3)  # whatever the two gives themselves published
        # The switch moves the inventory not at all: the only thing that changed
        # is which hotbar slot is selected, so the only thing that can tell the
        # viewer is the equipment push the carried-item handler makes.
        bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", 1))
        viewer_packets = drain(viewer, 1.0)
        equipment = all_of(viewer_packets, CB_SET_EQUIPMENT)
        assert equipment, \
            f"switching the held slot must tell the other player: " \
            f"{[hex(p) for p, _ in viewer_packets]}"
        held = []
        for body in equipment:
            seen_eid, seen_entries = read_equipment(body)
            assert seen_eid == bot.entity_id, f"equipment for entity {seen_eid}"
            held += [e for e in seen_entries if e[0] == EQUIPMENT_MAINHAND]
        assert (EQUIPMENT_MAINHAND, (ITEM_DIAMOND_CHESTPLATE, 1)) in held, held
        step(f"  OK the other player sees the held item: {held[-1]}")
        # Leave the selection where the rest of this file expects it: the eating
        # steps below use whatever is in the held slot, without selecting one.
        bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", 0))
        drain(viewer, 0.5)

        # --- 2c. the same six slots, to a client that arrives later ----------
        # `ServerEntity.sendPairingData` is this packet's *other* send: when a
        # tracker starts tracking an entity that already exists, vanilla packs
        # the non-default synched values (the health word's other half) and the
        # one Set Equipment holding what that entity wears and carries. The
        # change path (`broadcast_equipment`) cannot cover it — it fires on a
        # slot *moving*, and nothing moves for a client that was not there to
        # see it — so without the pairing a newcomer drew every player already
        # in the world empty-handed and unarmoured until a slot next changed.
        # This file's `Bot.join` keeps the packets it read (`join_packets`) for
        # exactly this assertion: the presence burst is consumed by the join and
        # is in no later drain.
        step("-> a third client joins and must be told what HealthBot wears and holds")
        latecomer = Bot("HealthLatecomer", port)
        wire_client.record(latecomer, note="health: the client that arrives after the equipment")
        latecomer.join()
        latecomer.start_reader()
        paired = []
        for body in all_of(latecomer.join_packets, CB_SET_EQUIPMENT):
            seen_eid, entries = read_equipment(body)
            if seen_eid == bot.entity_id:
                paired += entries
        assert (EQUIPMENT_MAINHAND, (ITEM_COOKED_BEEF, 1)) in paired, \
            f"the newcomer must be told the held item: {paired}"
        assert (EQUIPMENT_CHEST, (ITEM_DIAMOND_CHESTPLATE, 1)) in paired, \
            f"...and the armour: {paired}"
        step(f"  OK the newcomer's burst carries the hands and the armour: {paired}")
        # Nothing *changes* after the join, so nothing may say it again: the
        # burst above is the pairing path, not a change arriving late.
        late = drain(latecomer, 0.5)
        assert not [b for b in all_of(late, CB_SET_EQUIPMENT)
                    if read_equipment(b)[0] == bot.entity_id], \
            "the equipment must be in the join burst, not a later packet"
        # The newcomer's own arrival is on both streams; drain it so the windows
        # below hold only what they assert about.
        drain(bot, 0.3)
        drain(viewer, 0.3)

        # --- 3. armour turns the same hit down -------------------------------
        # The 20-tick window `LivingEntity.hurtServer` opens on a hit is spent
        # *here*, deliberately: inside it a second hit only lands for the part
        # that exceeds the first one's raw damage (`10 - 6 = 4` before armour,
        # not 10), which is not what this step is about. Under the wall clock the
        # window expired on its own while the test drained the inventory clicks
        # above; under the packet clock the ticks are the test's, and banking
        # them keeps the claim about armour rather than about the window. The
        # ticks also advance the food bar's regeneration, so the health they
        # synced is carried forward.
        window = wire_client.barrier(bot, 20)
        synced = [read_set_health(b) for b in all_of(window, CB_SET_HEALTH)]
        if synced:
            last_health = synced[-1][0]
        step(f"-> 20 ticks past the first hit (bar now {last_health})")
        step("-> /damage HealthBot 10 player_attack (with the chestplate on)")
        send_chat_command(bot, "damage HealthBot 10 player_attack")
        packets = drain(bot, 1.5)
        healths = [read_set_health(b) for b in all_of(packets, CB_SET_HEALTH)]
        # CombatRules.getDamageAfterAbsorb: toughness 2 -> 2 + 2/4 = 2.5, real
        # armour = clamp(8 - 10/2.5, 8 * 0.2, 20) = 4, so 10 * (1 - 4/25) = 8.4
        # lands instead of 10.
        assert "Applied 10 damage" in " ".join(chat_literals(packets)), "the hit was refused"
        before_hit, after_hit = health_around_damage(packets)
        assert after_hit is not None, \
            f"the armoured hit must report health: {[hex(p) for p, _ in packets]}"
        # The value just before the hit is whatever the regeneration had reached
        # (a full food bar heals 5/6 of a heart every ten ticks, which the ticks
        # above spend), so the assertion is on the *delta*.
        from_health = before_hit[0] if before_hit is not None else last_health
        landed = from_health - after_hit[0]
        assert abs(landed - 8.4) < 0.01, \
            f"the chestplate must turn 10 into 8.4; landed {landed} ({from_health} -> {after_hit[0]})"
        assert after_hit[0] > 0.0, f"8.4 damage must not be lethal here: {after_hit}"
        last_health = after_hit[0]
        step(f"  OK the chestplate turned 10 damage into {round(landed, 4)}"
             f" (health {after_hit[0]})")

        # --- 4. death --------------------------------------------------------
        step("-> /damage HealthBot 100 player_attack (lethal)")
        send_chat_command(bot, "damage HealthBot 100 player_attack")
        packets = drain(bot, 1.5)
        kills = all_of(packets, CB_PLAYER_COMBAT_KILL)
        assert kills, f"no player_combat_kill: {[hex(p) for p, _ in packets]}"
        eid, key, args = read_player_combat_kill(kills[-1])
        assert eid == bot.entity_id and key == "death.attack.player" and args == ["HealthBot"], \
            (eid, key, args)
        played = [read_entity_event(b) for b in all_of(packets, CB_ENTITY_EVENT)]
        assert (bot.entity_id, ENTITY_EVENT_DEATH) in played, f"death event missing: {played}"
        healths = [read_set_health(b) for b in all_of(packets, CB_SET_HEALTH)]
        assert healths and healths[-1][0] == 0.0, f"death must report 0 health: {healths}"
        translated = chat_translated(packets)
        assert ("death.attack.player", ["HealthBot"]) in translated, translated
        step(f"  OK death: {key}{tuple(args)}, health 0, entity_event 3")

        # --- 5. respawn ------------------------------------------------------
        step("-> client_command PERFORM_RESPAWN")
        bot.send(SB_CLIENT_COMMAND, varint(0))
        packets = drain(bot, 1.5)
        respawn = find(packets, CB_RESPAWN)
        assert respawn is not None, f"no respawn packet: {[hex(p) for p, _ in packets]}"
        info = read_respawn(respawn)
        assert info["dimension"] == "minecraft:overworld" and info["death"] is not None, info
        assert info["death"][0] == "minecraft:overworld", info["death"]
        healths = [read_set_health(b) for b in all_of(packets, CB_SET_HEALTH)]
        assert healths and healths[-1] == (20.0, 20, 5.0), f"respawn health: {healths}"
        assert find(packets, CB_CONTAINER_SET_CONTENT) is not None, "respawn must resync"
        assert find(packets, CB_PLAYER_POSITION) is not None, "respawn must teleport"
        drops = all_of(packets, CB_SPAWN_ENTITY)
        assert drops, "the death must drop what the player carried"
        step(f"  OK respawn: died at {unpack_block_pos(info['death'][1])}, "
             f"health {healths[-1][0]}, {len(drops)} dropped stack(s)")

        # --- 6. creative: invulnerable to /damage, not to /kill --------------
        step("-> creative: /damage is refused, /kill is not")
        send_chat_command(bot, "gamemode creative")
        drain(bot, 1.0)
        send_chat_command(bot, "damage HealthBot 100 player_attack")
        packets = drain(bot, 1.0)
        assert not all_of(packets, CB_DAMAGE_EVENT), "a creative player took damage"
        assert not all_of(packets, CB_SET_HEALTH), "a refused hit must not move health"
        feedback = " ".join(chat_literals(packets))
        assert "Could not damage" in feedback, f"creative refusal feedback: {feedback!r}"

        send_chat_command(bot, "kill HealthBot")
        packets = drain(bot, 1.5)
        kills = all_of(packets, CB_PLAYER_COMBAT_KILL)
        assert kills, f"/kill must kill a creative player: {[hex(p) for p, _ in packets]}"
        _, key, args = read_player_combat_kill(kills[-1])
        assert key == "death.attack.genericKill", (key, args)
        step(f"  OK creative gate: refused /damage, died to {key}")

        bot.send(SB_CLIENT_COMMAND, varint(0))
        drain(bot, 1.0)
        send_chat_command(bot, "gamemode survival")
        drain(bot, 1.0)

        # --- 7. eating while full is refused ---------------------------------
        step("-> eating at a full food bar must do nothing")
        send_chat_command(bot, "give HealthBot minecraft:cooked_beef 1")
        packets = drain(bot, 1.0)
        contents = all_of(packets, CB_CONTAINER_SET_CONTENT)
        assert contents, "the beef must reach the client"
        window, state, slots, _ = decode_content(contents[-1])
        # One, and not the two a collected death drop would give: the beef the
        # player died with *is* a real item entity now, but this test's world is
        # an empty Anvil world — the scripted bot hovers at the spawn point in
        # mid-air, and by the time `LivingEntity.createItemStackToDrop`'s forty
        # ticks are up the drop has fallen out of the inflated player box. The
        # collectable-drop path is `item-pickup-wire`'s subject; here the /give
        # stands alone.
        assert slots[SLOT_MENU_HOTBAR_FIRST] == (ITEM_COOKED_BEEF, 1), \
            f"the given beef should be alone in hotbar 0: " \
            f"{slots[SLOT_MENU_HOTBAR_FIRST]}"

        bot.send(SB_USE_ITEM, varint(0) + varint(0) + struct.pack(">ff", 0.0, 0.0))
        tick(bot, 40)
        packets = drain(bot, 0.8)
        assert not all_of(packets, CB_SET_HEALTH), \
            f"eating at full food must not move health: {all_of(packets, CB_SET_HEALTH)}"
        assert not [s for s in all_of(packets, CB_CONTAINER_SET_SLOT)
                    if decode_set_slot(s)[2] == SLOT_MENU_HOTBAR_FIRST], \
            "the beef must not be consumed while the food bar is full"
        step("  OK the beef was left uneaten")

        # --- 8. hunger: sprinting drains exhaustion, then food ---------------
        # 0.1 exhaustion a metre while sprinting. The *first* position packet
        # only seeds the server's last-known position, so seven steps 50 blocks
        # apart add 6 * 5.0 = 30 exhaustion; the tick spends 4 at a time, five
        # on the saturation bar and two on the food level: food 20 -> 18, with
        # 2 exhaustion left over.
        # Clear the watcher's stream first: the assertion after the sprint is
        # about this window, and the food-only change in it must not resend the
        # health word (the word is a diff of *health*; step 8 leaves health at
        # the full bar while the food bar drains).
        drain(viewer, 0.3)
        step("-> sprinting 300 blocks must drain the food bar")
        bot.send(SB_PLAYER_COMMAND, varint(bot.entity_id) + varint(1) + varint(0))
        x = 0.0
        for _ in range(7):
            x += 50.0
            bot.send(0x1E, struct.pack(">ddd", x, 64.0, 0.0) + bytes([1]))
        # The hunger effect spends the exhaustion four points a tick, and the
        # barrier hands back exactly what its twenty-five ticks produced — so
        # "food 18" below is the result of these ticks and not of the drain's
        # wall time.
        packets = wire_client.barrier(bot, 25)
        bot.send(SB_PLAYER_COMMAND, varint(bot.entity_id) + varint(2) + varint(0))
        healths = [read_set_health(b) for b in all_of(packets, CB_SET_HEALTH)]
        assert healths, f"the food bar must move: {[hex(p) for p, _ in packets]}"
        assert healths[-1][1] == 18, f"expected food 18 after 30 exhaustion, got {healths}"
        assert healths[-1][2] == 0.0, f"saturation should be spent first: {healths}"
        step(f"  OK hunger: food {healths[-1][1]}, saturation {healths[-1][2]}")
        viewer_hunger = drain(viewer, 1.0)
        words = health_words(viewer_hunger, bot.entity_id)
        assert words == [], \
            f"a food-only change must not move the health word: {words}"
        # ...and the same for the absorption word, which is a diff of its own
        # number: no absorption here has not *changed* at any point in this
        # window, so nothing may be said about it.
        words = absorption_words(viewer_hunger, bot.entity_id)
        assert words == [], \
            f"a food-only change must not move the absorption word either: {words}"

        # --- 9. and eating brings it back ------------------------------------
        # Cooked beef's `consumable.consume_seconds` is 1.6 s = 32 ticks, and
        # `LivingEntity.updateUsingItem` finishes the use on the tick the
        # countdown reaches zero — so this duration can be *bounded* now rather
        # than merely outlasted: one tick short of 32 the beef is still in the
        # hand and the bar has not moved, and the 32nd tick does both.
        step("-> eating the beef restores the food bar (32-tick use, bounded)")
        bot.send(SB_USE_ITEM, varint(0) + varint(0) + struct.pack(">ff", 0.0, 0.0))
        early = wire_client.barrier(bot, 31)
        assert not all_of(early, CB_SET_HEALTH), \
            f"a 32-tick use must not finish at 31: {all_of(early, CB_SET_HEALTH)}"
        early_slots = [decode_set_slot(s) for s in all_of(early, CB_CONTAINER_SET_SLOT)]
        assert not [s for s in early_slots if s[2] == SLOT_MENU_HOTBAR_FIRST and s[3] == (0, 0)], \
            f"the beef must still be in the hand at 31 of 32 ticks: {early_slots}"
        step("  OK one tick short: nothing eaten")
        packets = wire_client.barrier(bot, 1)
        healths = [read_set_health(b) for b in all_of(packets, CB_SET_HEALTH)]
        assert healths and healths[-1] == (20.0, 20, 12.8), f"after eating: {healths}"
        eaten = [decode_set_slot(s) for s in all_of(packets, CB_CONTAINER_SET_SLOT)]
        assert any(slot == SLOT_MENU_HOTBAR_FIRST and stack == (0, 0)
                   for _, _, slot, stack in eaten), f"the beef should be gone: {eaten}"
        step(f"  OK ate the beef on the 32nd tick: {healths[-1]}")

        # --- 10. absorption hearts: the number no bar carries ---------------
        # `Player.DATA_PLAYER_ABSORPTION_ID` (index 17) is a synched FLOAT like
        # the health word, written by `Player.setAbsorptionAmount` and flushed
        # by the same tracker send — but its fan-out includes the player's
        # *own* client, because set_health (health, food, saturation) does not
        # carry it and the yellow hearts over the bar are drawn from this number
        # alone. The Absorption effect's modifier is what makes any absorption
        # exist at all (`MAX_ABSORPTION` is 0 without it).
        step("-> /effect give HealthBot absorption 100 0")
        send_chat_command(bot, "effect give HealthBot absorption 100 0")
        packets = drain(bot, 1.0)
        words = absorption_words(packets, bot.entity_id)
        assert words == [4.0], \
            f"the owner's own absorption must reach its client: {words}"
        viewer_absorb = drain(viewer, 1.0)
        words = absorption_words(viewer_absorb, bot.entity_id)
        assert words == [4.0], f"and the watcher's copy of it: {words}"
        step("  OK both clients were told 4.0 absorption points")
        # ...and the number falling again: the hearts eat the hit first
        # (`Player.actuallyHurt`'s absorption floor), so 6 damage spends all 4
        # and 2 reach the bar.
        step("-> /damage HealthBot 6 player_attack (the hearts are spent first)")
        send_chat_command(bot, "damage HealthBot 6 player_attack")
        packets = drain(bot, 1.5)
        healths = [read_set_health(b) for b in all_of(packets, CB_SET_HEALTH)]
        assert healths and healths[-1][0] == 18.0, f"2 of the 6 reach the bar: {healths}"
        words = absorption_words(packets, bot.entity_id)
        assert words == [0.0], f"spending the hearts must move the word: {words}"
        step(f"  OK the hearts ate 4 and the word followed them down {words}")

        bot.close()
        viewer.close()
        latecomer.close()
    finally:
        proc.terminate()
        proc.wait(timeout=5)
    print("\nPASS: health test")
    return 0


if __name__ == "__main__":
    sys.exit(main())
