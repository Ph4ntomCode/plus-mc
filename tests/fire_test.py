#!/usr/bin/env python3
"""Integration test: fire, burning and a water bottle over the wire.

The fire module (world/fire.{h,cpp}) gives `minecraft:fire` real placement and
survival rules and gives a living entity a real `remainingFireTicks`, which is
what makes the potion module's water half observable instead of unfalsifiable:
before this, the fire block could not exist, nothing could burn, and the splash
water bottle's `dowseFire`/`extinguishFire` branches had nothing to act on.

This test drives the whole loop against the real server, from a stone floor the
test lays itself with real `use_item_on` packets:

  * `/give FireBot minecraft:flint_and_steel` then a real `use_item_on` on the
    floor below the player's feet places *fire* in the cell the player is
    standing in: block_update (0x08) at (0, 66, 0) carrying state 3406, the
    `minecraft:fire` default state (`age=0`, no face connected — the floor below
    is sturdy stone, so `FireBlock.getStateForPlacement` takes the default
    branch) — and the flint and steel is *damaged*, not consumed: the slot
    resync still holds one of it, with `minecraft:damage` = 1;
  * standing in that fire burns the player: a damage_event (0x19) of type
    `minecraft:in_fire` (21) on the very first tick (with its health drop), then
    20 ticks later the countdown reaches zero and the player *catches*, which is
    `minecraft:on_fire` (31) and the on-fire bit of the shared flags (Set Entity
    Data 0x63, index 0 = 1) that the client draws the flames from;
  * the water bottle puts it out: `/give
    minecraft:splash_potion[potion_contents={potion:"minecraft:water"}]` thrown
    straight down spawns the splash potion entity (Add Entity type 105), breaks
    on the fire block it lands on, raises level_event (0x2e) 2002 (water carries
    no instantaneous effect, so not the 2007 a harming potion raises) and
    douses the cell the bottle was in — block_update at (0, 66, 0) back to state
    0, air;
  * ...and extinguishes the player who was standing in it: a second shared-flags
    packet with the on-fire bit clear, and no further `in_fire` damage.

Fire has two clocks and this test uses both. The player's own burning countdown
is a *survival*-tick quantity, and the server counts packets for it
(`PLUSMC_SURVIVAL_CLOCK=packet`): one recognized serverbound packet is one tick
and idle wall time is none, so the twenty ticks between catching fire and the
in_fire hit that starts it are a count `wire_client.barrier` can bound on both
sides. The fire *block* (its ageing and spread) and the thrown bottle's flight
run on the server's own 50 ms world tick, which is real time — `tick()` gives
that clock the same wall time it always did, and is documented as doing so.

Run: fire_test.py /path/to/plusmc
"""
import os, re, shutil, socket, struct, subprocess, sys, tempfile, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nbt_component import chat_text  # noqa: E402
import wire_client
from wire_client import make_frame, read_packet

PROTO = 777
HOST = "127.0.0.1"

# Item ids (src/world/item_registry.h).
ITEM_STONE = 1
ITEM_FLINT_AND_STEEL = 1005
ITEM_SPLASH_POTION = 1442

# minecraft:entity_type protocol ids (src/protocol/ids.h).
ET_SPLASH_POTION = 108

# minecraft:damage_type wire ids (src/world/damage_table.h).
DMG_IN_FIRE = 21  # minecraft:in_fire,  index 21 in kDamageTypes
DMG_ON_FIRE = 31  # minecraft:on_fire,  index 31 in kDamageTypes

# The block state ids the test reads (src/world/default_block_states.h and
# src/world/block_states.h: air is the table's own 0).
STATE_FIRE_DEFAULT = 4962
STATE_AIR = 0

# Data component ids (src/world/item_registry.h).
COMPONENT_DAMAGE = 3
COMPONENT_ENCHANTMENTS = 13
COMPONENT_POTION_CONTENTS = 53

# Clientbound packet ids.
CB_SPAWN_ENTITY = 0x01
CB_BLOCK_UPDATE = 0x08
CB_DAMAGE_EVENT = 0x19
CB_LEVEL_EVENT = 0x2F
CB_REMOVE_ENTITIES = 0x4E
CB_SET_ENTITY_DATA = 0x65
CB_SET_HEALTH = 0x6A
CB_CONTAINER_SET_CONTENT = 0x12
CB_CONTAINER_SET_SLOT = 0x14
CB_SYSTEM_CHAT = 0x7C
CB_UPDATE_ATTRIBUTES = 0x86
CB_KEEP_ALIVE = 0x2D
CB_PLAYER_POSITION = 0x49

# Serverbound packet ids.
SB_CHAT_COMMAND = 0x07
SB_FLYING = 0x21
SB_POSITION = 0x1E
SB_SET_CARRIED_ITEM = 0x36
SB_SET_CREATIVE_SLOT = 0x39
SB_USE_ITEM_ON = 0x42
SB_USE_ITEM = 0x43
SB_KEEP_ALIVE = 0x1C

# Entity data: the shared-flags accessor and its on-fire bit (entity_packets.h).
SHARED_FLAGS_INDEX = 0
FLAG_ON_FIRE = 0x01
SER_BYTE = 0

# The floor the test lays: 3x3 stone at y=65, so the player's feet stand at 66
# and the fire `FlintAndSteelItem.useOn` places lands in the player's own cell.
FLOOR_Y = 65
FEET_Y = FLOOR_Y + 1
PLAYER_X = 0.5
PLAYER_Z = 0.5

# Menu slots: hotbar slot 0 is menu slot 36.
SLOT_MENU_HOTBAR_FIRST = 36

# Looking straight down: `AbstractThrownPotion`'s `shootFromRotation` tilts that
# by another -20 degrees, so the bottle leaves along -y.
PITCH_DOWN = 90.0

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
    ln, pos = rd_varint(buf, pos)
    return buf[pos:pos + ln].decode("utf-8"), pos + ln


def position(x, y, z):
    # x<<38 | z<<12 | y, packed as an *unsigned* 64-bit word (the floor's x/z may
    # be negative, which a signed pack would not take).
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


# --- decoders ---------------------------------------------------------------

def read_block_update(body):
    """block_update: [i64 packed position][varint state id]."""
    raw = struct.unpack(">q", body[0:8])[0]
    state, pos = rd_varint(body, 8)
    assert pos == len(body), f"block_update: {len(body) - pos} trailing byte(s)"
    return unpack_position(raw), state


def read_stack(body, pos=0):
    """The *trusted* (server-sent) ItemStack: [varint count][varint item]
    [varint added][varint removed], then per added entry [varint id][value] with
    the value inline (no length prefix), which is the whole difference from the
    delimited form a client sends. Handles the three components this server can
    write: damage, enchantments and potion_contents."""
    count, pos = rd_varint(body, pos)
    if count == 0:
        return None, pos
    item, pos = rd_varint(body, pos)
    added, pos = rd_varint(body, pos)
    removed, pos = rd_varint(body, pos)
    assert removed == 0, f"a removed component is not modelled: {removed}"
    stack = {"item": item, "count": count, "damage": 0, "potion": None}
    for _ in range(added):
        cid, pos = rd_varint(body, pos)
        if cid == COMPONENT_DAMAGE:
            stack["damage"], pos = rd_varint(body, pos)
        elif cid == COMPONENT_ENCHANTMENTS:
            entries, pos = rd_varint(body, pos)
            for _ in range(entries):
                _, pos = rd_varint(body, pos)
                _, pos = rd_varint(body, pos)
        elif cid == COMPONENT_POTION_CONTENTS:
            stack["potion"], pos = read_potion_contents(body, pos)
        else:
            raise AssertionError(f"unexpected component {cid}")
    return stack, pos


def read_potion_contents(buf, pos):
    """PotionContents.STREAM_CODEC: [bool][varint potion]? [bool][varint color]?
    [varint instances]... [bool][utf8 name]? -> (potion id or None, pos)."""
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
    for _ in range(count):
        _, pos = rd_varint(buf, pos)  # effect id
        _, pos = rd_varint(buf, pos)  # amplifier
        _, pos = rd_varint(buf, pos)  # duration
        pos += 3                      # ambient, visible, show_icon
        has_hidden = buf[pos]
        pos += 1
        assert has_hidden == 0, "a nested hidden effect is not modelled"
    has_name = buf[pos]
    pos += 1
    if has_name:
        _, pos = rd_string(buf, pos)
    return potion, pos


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


def read_level_event(body):
    """level_event: [i32 type][i64 packed position][i32 data][bool global]."""
    assert len(body) == 17, f"level_event body is {len(body)} bytes"
    event = struct.unpack(">i", body[0:4])[0]
    raw = struct.unpack(">q", body[4:12])[0]
    data = struct.unpack(">i", body[12:16])[0]
    return event, unpack_position(raw), data, body[16]


def read_remove_entities(body):
    count, pos = rd_varint(body, 0)
    ids = []
    for _ in range(count):
        v, pos = rd_varint(body, pos)
        ids.append(v)
    assert pos == len(body), f"remove_entities: {len(body) - pos} trailing byte(s)"
    return ids


def read_set_health(body):
    """set_health: [f32 health][varint food][f32 saturation]."""
    health = struct.unpack(">f", body[0:4])[0]
    food, pos = rd_varint(body, 4)
    return round(health, 3), food


def read_shared_flags(body, wanted):
    """Set Entity Data for *one* entity, whose only accessors are BYTE-serialized
    (the shared flags this test watches and the second-skin model-parts mask the
    join sends). Another entity's packet returns None before anything is read, so
    the packet's other serializers never reach this decoder."""
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
        assert serializer == SER_BYTE, f"entity data index {index}: serializer {serializer}"
        values[index] = body[pos]
        pos += 1
    assert pos == len(body), f"set_entity_data: {len(body) - pos} trailing byte(s)"
    return values


def on_fire_bits(packets, eid):
    """Every shared-flags byte this drain carried for `eid`, in order.
    `Entity.setSharedFlagOnFire` publishes the on-fire bit on change only, so
    the *last* one is the current state and the sequence is the transitions."""
    bits = []
    for body in all_of(packets, CB_SET_ENTITY_DATA):
        read = read_shared_flags(body, eid)
        if read is not None and SHARED_FLAGS_INDEX in read:
            bits.append(read[SHARED_FLAGS_INDEX])
    return bits


# --- the client -------------------------------------------------------------

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
            self.position = struct.unpack(">ddd", body[0:24])
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
            if pid == CB_KEEP_ALIVE:
                self.send(SB_KEEP_ALIVE, body[:8])
            elif pid == 0x0B:
                batch, _ = rd_varint(body)
                self.send(0x0B, varint(batch))
                break
        return True

    def move_to(self, x, y, z, ground=1):
        self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([ground]))


def drain(bot, timeout=1.2):
    """Every packet the reader thread queued in the next `timeout`
    seconds."""
    return wire_client.drain(bot, timeout)
def tick(bot, count):
    """Advance the survival clock by exactly `count` game ticks and give the
    world clock `count` packets' worth of real time.

    The server runs with `PLUSMC_SURVIVAL_CLOCK=packet`, so the survival tick
    (which is what burns the player) advances one tick per `flying` packet and
    the sleep below no longer has any hand in it. The sleep is here for the
    *world* clock the rest of this test leans on: the fire block's ageing and
    spread, and the flight of the thrown water bottle, run on the server's own
    50 ms main loop, which is real time. Where a survival-tick count is asserted
    (`wire_client.barrier`) the sleep is not used — those steps need no world
    time at all.
    """
    for _ in range(count):
        bot.send(SB_FLYING, bytes([1]))
        time.sleep(TICK_MS)


def all_of(packets, pid):
    return [b for p, b in packets if p == pid]


def feedback(packets):
    out = []
    for body in all_of(packets, CB_SYSTEM_CHAT):
        try:
            out.append(chat_text(body))
        except AssertionError:
            pass
    return " | ".join(out)


def send_chat_command(bot, command):
    bot.send(SB_CHAT_COMMAND, varint(len(command)) + command.encode())


def use_item_on(bot, x, y, z, face, seq, cursor=(0.5, 0.5, 0.5)):
    """ServerboundUseItemOn: [varint hand][i64 position][varint face][3 f32
    cursor][bool inside][bool border][varint sequence]. The cursor is checked
    against the block's *centre* (`|location - atCenterOf(pos)| < 1.0000001`), so
    a half-block offset is on it whatever the face."""
    bot.send(SB_USE_ITEM_ON,
             varint(0) + position(x, y, z) + varint(face)
             + struct.pack(">fff", *cursor) + bytes([0, 0]) + varint(seq))


def use_item(bot, pitch=PITCH_DOWN):
    """ServerboundUseItem: [varint hand][varint sequence][f32 yRot][f32 xRot]."""
    bot.send(SB_USE_ITEM, varint(0) + varint(0) + struct.pack(">ff", 0.0, pitch))


def select(bot, slot):
    assert SLOT_MENU_HOTBAR_FIRST <= slot < SLOT_MENU_HOTBAR_FIRST + 9, \
        f"menu slot {slot} is outside the hotbar"
    bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", slot - SLOT_MENU_HOTBAR_FIRST))
    drain(bot, 0.2)


class Harness:
    def __init__(self, bot):
        self.bot = bot
        self.health = (20.0, 20)
        self.slots = None

    def observe(self, packets):
        for body in all_of(packets, CB_SET_HEALTH):
            self.health = read_set_health(body)
        for body in all_of(packets, CB_CONTAINER_SET_CONTENT):
            _, _, slots, _ = decode_content(body)
            self.slots = slots
        for body in all_of(packets, CB_CONTAINER_SET_SLOT):
            _, _, slot, stack = decode_set_slot(body)
            if self.slots is not None and 0 <= slot < len(self.slots):
                self.slots[slot] = stack
        return packets

    def command(self, text, timeout=1.2):
        step(f"-> /{text}")
        send_chat_command(self.bot, text)
        return self.observe(drain(self.bot, timeout))

    def give(self, item, count=1):
        """`/give` an item and return the menu slot it landed in, read off the
        content resync the command sends."""
        packets = self.command(f"give {self.bot.name} {item} {count}")
        assert "Gave " in feedback(packets), feedback(packets)
        assert self.slots is not None, "a /give must resync the inventory"
        return self.slots

    def slot_of(self, item_id):
        for i, stack in enumerate(self.slots or []):
            if stack is not None and stack["item"] == item_id:
                return i
        raise AssertionError(f"item {item_id} is nowhere in the menu: {self.slots}")


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
    world = tempfile.mkdtemp(prefix="plusmc-fire-")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    # The player's burning countdown is a survival-tick quantity, and this makes
    # those ticks the test's own packets (one each, no wall-time catch-up) — see
    # the module docstring. The world clock is untouched by it.
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"

    proc = None
    try:
        step(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--motd", "fire test",
             "--offline-mode", "--gamemode", "1", "--data", data_root, "--world", world],
            wait_listening, env=env)

        bot = Bot("FireBot", port)
        wire_client.record(bot, note="fire-wire: flint and steel, burning, dousing")
        bot.join()
        h = Harness(bot)
        join_packets = h.observe(drain(bot, 1.2))
        attrs = all_of(join_packets, CB_UPDATE_ATTRIBUTES)
        assert attrs, "the join must push the attribute set (which carries the entity id)"
        bot.entity_id, _ = rd_varint(attrs[-1], 0)
        step(f"  OK joined as entity {bot.entity_id} at {h.health[0]:g} health")

        # --- 1. the floor the fire is lit on ---------------------------------
        # A 3x3 of stone at y=65, laid the way the mobs test lays its own: the
        # bot stands one above the layer and clicks the cell below the target
        # with face=up, so the server places one block above the click. The void
        # world has nothing to stand on, so the position is the bot's own claim
        # (`Player.isWithinBlockInteractionRange` is measured from where the
        # client says it is).
        bot.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", SLOT_MENU_HOTBAR_FIRST) + varint(64)
                 + varint(ITEM_STONE) + b"\x00\x00")
        time.sleep(0.2)
        bot.move_to(PLAYER_X, float(FEET_Y), PLAYER_Z)
        time.sleep(0.1)
        seq = 1
        for x in (-1, 0, 1):
            for z in (-1, 0, 1):
                use_item_on(bot, x, FLOOR_Y - 1, z, 1, seq)
                seq += 1
        packets = h.observe(drain(bot, 2.0))
        updates = [read_block_update(b) for b in all_of(packets, CB_BLOCK_UPDATE)]
        assert len(updates) == 9, f"expected 9 floor blocks, got {len(updates)}"
        assert all(at[1] == FLOOR_Y for at, _ in updates), updates
        step(f"  OK floor: {len(updates)} stone blocks at y={FLOOR_Y}")

        # --- 2. flint and steel lights real fire ------------------------------
        # Survival, because a creative player is invulnerable and the flames
        # would never do anything.
        h.command("gamemode survival FireBot")
        h.give("minecraft:flint_and_steel")
        slot = h.slot_of(ITEM_FLINT_AND_STEEL)
        assert h.slots[slot]["count"] == 1 and h.slots[slot]["damage"] == 0, h.slots[slot]
        step(f"  OK the menu holds a fresh flint and steel in slot {slot}")
        select(bot, slot)

        # Click the floor below the player's feet with face=up: `useOn` places
        # fire in the cell past the face, which is the cell the player is
        # standing in.
        use_item_on(bot, 0, FLOOR_Y, 0, 1, seq)
        seq += 1
        packets = h.observe(drain(bot, 1.5))
        updates = [read_block_update(b) for b in all_of(packets, CB_BLOCK_UPDATE)]
        lit = [(at, state) for at, state in updates if at == (0, FEET_Y, 0)]
        assert lit, f"no fire block update at (0, {FEET_Y}, 0): {updates}"
        at, state = lit[-1]
        # `FireBlock.getStateForPlacement`'s default branch: the block below is
        # sturdy stone, so the state is fire at age 0 with no face connected —
        # the default state of `minecraft:fire`.
        assert state == STATE_FIRE_DEFAULT, (state, STATE_FIRE_DEFAULT)
        step(f"  OK flint and steel lit fire at {at} (state {state})")

        # `ItemStack.hurtAndBreak(1, ...)`: the slot is resynced *still holding*
        # the flint and steel, one wear deeper — not consumed, which is what
        # separates `FlintAndSteelItem` from a block placement.
        worn = [decode_set_slot(b) for b in all_of(packets, CB_CONTAINER_SET_SLOT)]
        worn = [s for s in worn if s[2] == slot]
        assert worn, f"the used slot must resync: {[s[2] for s in worn]}"
        _, _, _, stack = worn[-1]
        assert stack is not None and stack["item"] == ITEM_FLINT_AND_STEEL, stack
        assert stack["count"] == 1, stack
        assert stack["damage"] == 1, f"one use is one point of wear: {stack}"
        step("  OK the flint and steel wore by one instead of being consumed")

        # --- 3. the player burns ---------------------------------------------
        # The player is standing in the fire they lit. `BaseFireBlock.fireIgnite`
        # walks the countdown up one tick at a time from `-getFireImmuneTicks()`
        # (a player's 20), so `in_fire` lands at once while catching takes a
        # full second; both are the same damage sink every other source uses.
        #
        # That second is twenty *survival* ticks, and the packet clock makes them
        # a count: nineteen of them leave the countdown on -1 and the player
        # merely *in* fire, and the twentieth reaches zero — which `fire_ignite`
        # answers with `ignite_seconds(8)` (160 ticks) and the on-fire bit of the
        # shared flags. Both sides of that are asserted below. (The loop this
        # replaced sent forty-eight packets and let the wall clock supply the
        # ticks, so it could not have told "sixteen" from "twenty".)
        packets = h.observe(wire_client.barrier(bot, 19))
        assert not [b for b in on_fire_bits(packets, bot.entity_id) if b & FLAG_ON_FIRE], \
            f"the player must not be alight 19 ticks into the fire: {packets}"
        packets += h.observe(wire_client.barrier(bot, 1))
        assert [b for b in on_fire_bits(packets, bot.entity_id) if b & FLAG_ON_FIRE], \
            f"the twentieth tick in fire ignites the player: {packets}"
        # ...and the `on_fire` damage itself lands one tick later, because
        # `burning.tick()` reads the countdown at the *start* of a tick (a player
        # who catches at the end of tick 20 takes the first `on_fire` on 21).
        packets += h.observe(wire_client.barrier(bot, 1))
        events = [read_damage_event(b) for b in all_of(packets, CB_DAMAGE_EVENT)]
        mine = [e for e in events if e["entity"] == bot.entity_id]
        in_fire = [e for e in mine if e["type"] == DMG_IN_FIRE]
        on_fire = [e for e in mine if e["type"] == DMG_ON_FIRE]
        assert in_fire, f"standing in fire must hurt: {[e['type'] for e in mine]}"
        assert on_fire, f"the player must catch fire: {[e['type'] for e in mine]}"
        # `damageSources().inFire()` has no entity behind it, so both cause and
        # direct are absent (the wire's 0 is the "none" sentinel).
        assert in_fire[0]["cause"] == -1 and in_fire[0]["direct"] == -1, in_fire[0]
        healths = [read_set_health(b) for b in all_of(packets, CB_SET_HEALTH)]
        assert healths and healths[-1][0] < 20.0, healths[-3:]
        step(f"  OK burning: {len(in_fire)} in_fire and {len(on_fire)} on_fire hits, "
             f"health {h.health[0]:g}")

        # The flames the client draws: `Entity.setSharedFlagOnFire` through the
        # shared flags, sent once per change.
        flags = []
        for body in all_of(packets, CB_SET_ENTITY_DATA):
            read = read_shared_flags(body, bot.entity_id)
            if read is not None and SHARED_FLAGS_INDEX in read:
                flags.append(read[SHARED_FLAGS_INDEX])
        assert flags and flags[-1] & FLAG_ON_FIRE, flags
        step("  OK the shared flags carry the on-fire bit (the client's flames)")

        # --- 4. a thrown water bottle douses it -------------------------------
        # A *splash* potion carrying plain water: `AbstractThrownPotion
        # .onHitAsWater`, not a drink. Thrown straight down, it breaks on the
        # fire block itself (the server's solid predicate is "not air and not a
        # fluid", and fire is neither).
        h.give('minecraft:splash_potion[potion_contents={potion:"minecraft:water"}]')
        bottle = h.slot_of(ITEM_SPLASH_POTION)
        assert h.slots[bottle]["potion"] == 0, h.slots[bottle]  # minecraft:water
        select(bot, bottle)
        use_item(bot)
        # The bottle flies on the *world* clock (real time) but the douse's
        # extinguish is applied by the session's own tick, which only advances on
        # a serverbound packet — so the two are advanced together here.
        packets = []
        for _ in range(6):
            tick(bot, 12)
            packets += h.observe(drain(bot, 0.25))

        spawns = [read_spawn(b) for b in all_of(packets, CB_SPAWN_ENTITY)]
        thrown = [s for s in spawns if s["type"] == ET_SPLASH_POTION]
        assert thrown, f"no splash potion was thrown: {[s['type'] for s in spawns]}"
        assert abs(thrown[0]["x"] - PLAYER_X) < 1e-6, thrown[0]
        step(f"  OK threw splash potion entity {thrown[0]['entity']} from the eyes")

        updates = [read_block_update(b) for b in all_of(packets, CB_BLOCK_UPDATE)]
        doused = [(at, state) for at, state in updates
                  if at == (0, FEET_Y, 0) and state == STATE_AIR]
        assert doused, f"the water must douse the fire block: {updates}"
        step(f"  OK the fire at {doused[-1][0]} was doused (block update to air)")

        # Water carries no instantaneous effect, so the break's level event is
        # the 2002 ("splash") rather than a harming potion's 2007.
        events = [read_level_event(b) for b in all_of(packets, CB_LEVEL_EVENT)]
        assert events, "the break must raise a level event"
        assert events[-1][0] == 2002, events
        step(f"  OK level event {events[-1][0]} at {events[-1][1]}")

        # ...and the player who was standing in it goes out: another shared-flags
        # packet, this time with the bit clear, and no further `in_fire` damage
        # once the block is gone.
        flags = []
        for body in all_of(packets, CB_SET_ENTITY_DATA):
            read = read_shared_flags(body, bot.entity_id)
            if read is not None and SHARED_FLAGS_INDEX in read:
                flags.append(read[SHARED_FLAGS_INDEX])
        # `Entity.setSharedFlagOnFire` only sends on a *change*, and step 3 saw
        # the bit set — so this packet can only be the extinguishing.
        assert flags and not (flags[-1] & FLAG_ON_FIRE), flags

        after = []
        for _ in range(5):
            tick(bot, 12)
            after += h.observe(drain(bot, 0.2))
        burned_again = [read_damage_event(b) for b in all_of(after, CB_DAMAGE_EVENT)]
        burned_again = [e for e in burned_again
                        if e["entity"] == bot.entity_id and e["type"] == DMG_IN_FIRE]
        assert not burned_again, f"a doused fire must stop hurting: {burned_again}"
        step("  OK the flames went out and no further in_fire damage landed")

        bot.close()
        print("fire: all steps passed")
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
