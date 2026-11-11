#!/usr/bin/env python3
"""Integration test: `minecraft:consumable`'s on_consume_effects over the wire.

Eating is driven the way a client drives it — `use_item` (0x43) starts the use,
the use ticks down over the item's `consume_ticks`, and completing it runs
`Consumable.onConsume` — and the effects the item's list grants are read off the
packets that describe them:

  * a golden apple grants Regeneration II for 100 ticks and Absorption I for
    2400: two update_mob_effect (0x84) packets with the registry ids, the
    amplifiers, the durations and the visible+show_icon+blend flags, in the order
    the item's data lists them;
  * the food lands *before* the effects, which is `Consumable.onConsume`'s order
    (the item's ConsumableListener components, then on_consume_effects): the same
    drain carries the set_health (0x68) the apple's nutrition and saturation
    raised;
  * Absorption is not just an icon: 10 damage lands as 6, because the 4
    absorption points eat the rest (Player.actuallyHurt's absorption floor);
  * the Regeneration instance really is 100 ticks: it reports remove_mob_effect
    (0x4e) when it runs out;
  * a milk bucket clears *everything*, sending one remove_mob_effect per effect
    and taking the Speed attribute modifier away with it — and it is drinkable on
    a full hunger bar, which is Consumable.canConsume's food-only gate;
  * a honey bottle removes poison and nothing else;
  * a chorus fruit hops the player: LivingEntity.randomTeleport's landing search,
    a synchronize_player_position (0x48) and the teleport particle entity_event
    (0x22, 46);
  * an ominous bottle is consumed and does nothing observable, because the one
    thing its list asks for is a sound and this server models no sounds.

...and the two components that run *around* the consume rather than during it
(`ItemStack.applyAfterUseComponentSideEffects`), which this test drives too:

  * minecraft:use_remainder: eating a stew leaves the bowl, a milk bucket leaves
    a bucket and a honey bottle leaves a glass bottle — in the hand slot when the
    stack was emptied, and in the inventory (with a resync) when it was not, so
    the last stew of a stack really does hand you the bowl;
  * minecraft:use_cooldown: a chorus fruit's cooldown (0x16) starts with the
    eat ("minecraft:chorus_fruit", 20 ticks), *refuses* a second use_item inside
    the window — nothing at all comes back from it — and is cleared by the server
    20 ticks later with a duration of 0, after which the fruit works again.

Run: consume_test.py /path/to/plusmc
"""
import os, re, shutil, socket, struct, subprocess, sys, tempfile, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nbt_component import chat_text  # noqa: E402
import wire_client
from wire_client import make_frame, read_packet

PROTO = 777
HOST = "127.0.0.1"

# Registry ids (src/world/effect_table.h, src/world/item_registry.h).
EFFECT_SPEED = 0
EFFECT_REGENERATION = 9
EFFECT_HUNGER = 16
EFFECT_POISON = 18
EFFECT_ABSORPTION = 21
ITEM_BOWL = 1006
ITEM_MUSHROOM_STEW = 1061
ITEM_GOLDEN_APPLE = 1100
ITEM_BUCKET = 1128
ITEM_MILK_BUCKET = 1134
ITEM_GLASS_BOTTLE = 1270
ITEM_CHORUS_FRUIT = 1434
ITEM_HONEY_BOTTLE = 1533
ITEM_OMINOUS_BOTTLE = 1657

# The cooldown group the client is sent for an item whose use_cooldown names no
# group: ItemCooldowns.getCooldownGroup falls back to the item's registry key.
CHORUS_FRUIT_GROUP = "minecraft:chorus_fruit"
COOLDOWN_CHORUS_FRUIT = 20  # UseCooldown.ticks() for its 1.0 second

# The consume_ticks the item table records: 32 is Consumable's 1.6 s default
# (an apple, a milk bucket, a chorus fruit, an ominous bottle), and a honey
# bottle's own `consume_seconds` is 2.0.
TICKS_DEFAULT = 32
TICKS_HONEY_BOTTLE = 40

# The duration the Golden Apple's Regeneration is granted with, which is the
# number the update_mob_effect packet this test reads carries (the item's own
# `on_consume_effects`). With the packet clock it is also a number the test can
# land on exactly.
REGEN_TICKS = 100

# ClientboundUpdateMobEffectPacket's flag bits (src/protocol/ids.h).
FLAG_VISIBLE = 2
FLAG_SHOW_ICON = 4
FLAG_BLEND = 8

CB_COOLDOWN = 0x16
CB_CONTAINER_SET_CONTENT = 0x12
CB_CONTAINER_SET_SLOT = 0x14
CB_DAMAGE_EVENT = 0x19
CB_ENTITY_EVENT = 0x22
CB_KEEP_ALIVE = 0x2D
CB_PLAYER_POSITION = 0x49
CB_PONG = 0x3F
CB_REMOVE_MOB_EFFECT = 0x4F
CB_SET_HEALTH = 0x6A
CB_SYSTEM_CHAT = 0x7C
CB_UPDATE_ATTRIBUTES = 0x86
CB_UPDATE_MOB_EFFECT = 0x87

SB_CHAT_COMMAND = 0x07
SB_FLYING = 0x21
SB_KEEP_ALIVE = 0x1C
SB_PING_REQUEST = 0x26
SB_SET_CARRIED_ITEM = 0x36
SB_USE_ITEM = 0x43

# Player menu slots: 36 is hotbar 0, which is the slot a fresh session has
# selected and therefore the slot /give fills first.
SLOT_MENU_HOTBAR_FIRST = 36

# Infinite duration is written as a varint of -1, i.e. the unsigned maximum.
UNSIGNED_MAX = 0xFFFFFFFF


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


def read_cooldown(body):
    """cooldown: [string cooldown group][varint duration in ticks]."""
    group, pos = rd_string(body, 0)
    duration, pos = rd_varint(body, pos)
    assert pos == len(body), f"cooldown: {len(body) - pos} trailing byte(s)"
    return group, duration


def read_set_health(body):
    """: [f32 health][varint food][f32 saturation]."""
    health = struct.unpack(">f", body[0:4])[0]
    food, pos = rd_varint(body, 4)
    saturation = struct.unpack(">f", body[pos:pos + 4])[0]
    assert pos + 4 == len(body), f"set_health: {len(body) - pos - 4} trailing byte(s)"
    return round(health, 4), food, round(saturation, 4)


def read_stack(body, pos):
    """ItemStack.OPTIONAL_STREAM_CODEC: [varint count] and, when non-zero,
    [varint item][varint added][varint removed] then the added values. Nothing
    this test eats carries a component, so any patch entry is a surprise."""
    count, pos = rd_varint(body, pos)
    if count == 0:
        return None, pos
    item, pos = rd_varint(body, pos)
    added, pos = rd_varint(body, pos)
    removed, pos = rd_varint(body, pos)
    assert added == 0 and removed == 0, f"unexpected component patch {added}/{removed}"
    return (item, count), pos


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


def total_of(slots, item_id):
    """How many of `item_id` a whole menu holds. A remainder that merged into an
    existing stack counts exactly like one that took an empty slot, which a
    per-stack comparison would not see."""
    return sum(stack[1] for stack in slots if stack is not None and stack[0] == item_id)


def decode_set_slot(body):
    """: (window, state, slot, stack)."""
    window, pos = rd_varint(body, 0)
    state, pos = rd_varint(body, pos)
    slot = struct.unpack(">h", body[pos:pos + 2])[0]
    stack, pos = read_stack(body, pos + 2)
    assert pos == len(body), f"container_set_slot: {len(body) - pos} trailing byte(s)"
    return window, state, slot, stack


def read_position(body):
    """synchronize_player_position: [varint teleport id][f64 x][f64 y][f64 z]..."""
    _, pos = rd_varint(body, 0)
    x, y, z = struct.unpack(">ddd", body[pos:pos + 24])
    return x, y, z


def read_entity_event(body):
    """entity_event: [i32 entity id][u8 event]."""
    eid = struct.unpack(">i", body[0:4])[0]
    return eid, body[4]


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
        # The last synchronize_player_position the server sent, which is the
        # only place the client is told where the server thinks it is: the join
        # spawn teleport, a /tp, and a chorus fruit all arrive this way. Recorded
        # in `recv` so no caller can forget to look for it.
        self.position = None

    def send(self, pid, body=b""):
        self.reader.send(pid, body)

    def recv(self):
        pid, body = read_packet(self.sock, self.threshold)
        if pid == CB_PLAYER_POSITION:
            self.position = read_position(body)
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
def drain_quiet(bot, quiet=0.15, limit=2.0):
    """Drain until the server has been quiet for `quiet` seconds.

    The fixed window `drain` above waits out its whole window even when the
    server has already said everything it is going to say, and wall time is not
    free here: the packet-driven survival clock turns idle seconds into game
    ticks, which is exactly the length of a chorus fruit's cooldown.
    """
    return wire_client.drain_quiet(bot, quiet, limit)


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


def latest_attributes(packets):
    bodies = all_of(packets, CB_UPDATE_ATTRIBUTES)
    return read_attributes(bodies[-1]) if bodies else (None, {})


def speed_modifiers(packets):
    """The `minecraft:effect.speed` modifiers on movement_speed (attribute 26) in
    the last attribute snapshot of the drain."""
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
    """(health before the hit, health after) from a drain around one hit: the
    tick batch first, then the damage_event, then the health packet the hit
    raised."""
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


def last_health(packets, fallback):
    """The health bar the drain ended at (a float), or `fallback` if the server
    never synced one."""
    healths = [read_set_health(b) for b in all_of(packets, CB_SET_HEALTH)]
    return healths[-1][0] if healths else fallback


def tick(bot, count):
    """Advance the survival clock by exactly `count` game ticks.

    The server runs with `PLUSMC_SURVIVAL_CLOCK=packet`, so the clock *is* these
    packets: one tick each, and nothing at all while the test is not sending.
    That is what makes the durations in this file measurements rather than
    guesses. With the wall clock, every second spent waiting on a packet was
    another twenty ticks, so a loaded machine shortened the effect under test —
    which is why this test used to fail intermittently on a busy one.
    """
    for _ in range(count):
        bot.send(SB_FLYING, bytes([1]))


_PING_PAYLOAD = 0x1000


def barrier(bot, ticks, timeout=2.0):
    """Advance the clock exactly `ticks` and hand back everything the server
    sent for them, in order — and nothing generated after them.

    `handle_ping_request` echoes its payload, so a ping is a barrier: the pong
    cannot be written until the server has processed every packet sent before
    it, and TCP keeps the order. That is what lets a duration be asserted as an
    exact tick count rather than "somewhere in this window" — the packets above
    are counted, not timed, so neither a slow machine nor a fast one moves the
    answer. The ping is itself a recognized serverbound packet and so is the
    *last* of the `ticks` counted here (the survival clock's
    `PLUSMC_SURVIVAL_CLOCK=packet` advances on every one of them).
    """
    global _PING_PAYLOAD
    assert ticks >= 1, "a barrier is at least its own ping"
    tick(bot, ticks - 1)
    _PING_PAYLOAD += 1
    payload = _PING_PAYLOAD
    bot.reader.start()  # idempotent: the reader may not have been needed yet
    bot.send(SB_PING_REQUEST, struct.pack(">i", payload))
    out = []
    deadline = time.time() + timeout
    while True:
        try:
            pid, body = bot.reader.pop(max(0.05, deadline - time.time()))
        except TimeoutError:
            raise AssertionError(f"no pong for {payload:#x}: "
                                 f"{[hex(p) for p, _ in out]}") from None
        out.append((pid, body))
        if pid == CB_PONG and struct.unpack(">i", body[:4])[0] == payload:
            return out


def use_item(bot):
    """ServerboundUseItem (0x43): [varint hand][varint sequence][f32 yRot]
    [f32 xRot], all zero for a main-hand use facing east."""
    bot.send(SB_USE_ITEM, varint(0) + varint(0) + struct.pack(">ff", 0.0, 0.0))


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
    """Commands, inventory tracking and the health bar."""

    def __init__(self, bot):
        self.bot = bot
        self.health = None
        self.state = None
        # The last full player menu the server sent, so a caller can find where
        # a `/give` actually landed.
        self.slots = None

    def command(self, text, timeout=1.5):
        step(f"-> /{text}")
        send_chat_command(self.bot, text)
        packets = drain(self.bot, timeout)
        self.health = last_health(packets, self.health)
        for body in all_of(packets, CB_CONTAINER_SET_CONTENT):
            _, self.state, self.slots, _ = decode_content(body)
        for body in all_of(packets, CB_CONTAINER_SET_SLOT):
            _, state, _, _ = decode_set_slot(body)
            self.state = state
        return packets

    def select(self, item_id):
        """Select the hotbar slot holding `item_id`, the way a client scrolls to
        an item before using it (ServerboundSetCarriedItem, 0x35), and return its
        menu slot.

        A use acts on the *selected* hotbar slot only, and `/give` fills the
        selected slot when it is empty or already holds the item and the first
        empty slot otherwise — which the use remainder makes the common case: the
        bowl, bucket or bottle the *last* use left behind is still sitting in the
        selected slot, so the next item has to land somewhere else.
        """
        for i, stack in enumerate(self.slots or []):
            if stack is None or stack[0] != item_id:
                continue
            if not (SLOT_MENU_HOTBAR_FIRST <= i < SLOT_MENU_HOTBAR_FIRST + 9):
                raise AssertionError(f"menu slot {i} is outside the hotbar")
            self.bot.send(SB_SET_CARRIED_ITEM,
                          struct.pack(">h", i - SLOT_MENU_HOTBAR_FIRST))
            drain(self.bot, 0.25)
            return i
        raise AssertionError(f"no {item_id} in the inventory: {self.slots}")

    def eat(self, item_name, item_id, ticks, remainder=None):
        """`/give` the item, select the hotbar slot it landed in, use it, and
        tick the use out. The slot must end up holding `remainder` — nothing for
        an item with no use_remainder, and the bowl/bucket/bottle for one that
        has it. Returns (give packets, use packets)."""
        give = self.command(f"give {self.bot.name} {item_name} 1")
        assert f"Gave 1 [minecraft:{item_name.split(':')[-1]}]" in feedback(give), \
            feedback(give)
        contents = all_of(give, CB_CONTAINER_SET_CONTENT)
        assert contents, "a /give must resync the inventory"
        _, _, slots, _ = decode_content(contents[-1])
        slot = 0
        for i, stack in enumerate(slots):
            if stack is not None and stack[0] == item_id:
                slot = i
                break
        else:
            raise AssertionError(f"{item_name} is nowhere in the menu: {slots}")
        assert slots[slot] == (item_id, 1), \
            f"{item_name} must land as one item: {slots[slot]}"
        if slot != SLOT_MENU_HOTBAR_FIRST:
            assert self.select(item_id) == slot, "the item must be selectable"
        use_item(self.bot)
        tick(self.bot, ticks)
        packets = drain(self.bot, 1.2)
        sets = all_of(packets, CB_CONTAINER_SET_SLOT)
        assert sets, f"completing a use must resync the slot: {[hex(p) for p, _ in packets]}"
        window, _, used_slot, stack = decode_set_slot(sets[-1])
        assert window == 0 and used_slot == slot, (window, used_slot, slot)
        assert stack == remainder, \
            f"the used slot must hold {remainder} (use_remainder): {stack}"
        self.health = last_health(packets, self.health)
        return give, packets

    def hit(self, damage=10, type_name="player_attack"):
        """One /damage and the delta it took off the health bar, measured across
        the damage_event."""
        prior = self.health
        packets = self.command(f"damage {self.bot.name} {damage} {type_name}")
        assert "Applied" in feedback(packets), f"the hit was refused: {feedback(packets)}"
        before, after = health_around_damage(packets)
        assert after is not None, f"the hit must report health: {[hex(p) for p, _ in packets]}"
        from_health = before[0] if before is not None else prior
        self.health = after[0]
        return round(from_health - after[0], 4), packets


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-consume-")
    # One fixture chunk of real terrain is copied into the world, because the
    # chorus fruit needs ground to land on and a fresh world is all air:
    # AnvilWorld answers air for a chunk it has no region data for. Copied rather
    # than symlinked so nothing the server saves can touch the repository.
    shutil.copytree(os.path.join(os.path.dirname(__file__), "fixtures", "ow3region"),
                    os.path.join(world, "region"))
    env = dict(os.environ)
    # Only this test's own packets may advance the survival clock.
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    # One tick per packet: this test measures tick durations (an effect's 100
    # ticks, a 20-tick cooldown), so idle wall time must not be able to move the
    # clock behind its assertions.
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"

    proc = None
    try:
        step(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", "127.0.0.1", "--port", "0", "--motd", "consume test",
             "--offline-mode", "--gamemode", "0", "--data", data_root, "--world", world,
             "--no-flat-world"], wait_listening, env=env)

        bot = Bot("EatBot", port)
        wire_client.record(bot, note="consume-wire: on_consume_effects, remainder, cooldown")
        bot.join()
        bot.start_reader()
        h = Harness(bot)
        join_packets = drain(bot, 1.2)
        healths = [read_set_health(b) for b in all_of(join_packets, CB_SET_HEALTH)]
        assert healths and healths[-1] == (20.0, 20, 5.0), f"join health: {healths}"
        h.health = healths[-1][0]
        assert not effects_of(join_packets), "a fresh join has no effects"
        attrs = all_of(join_packets, CB_UPDATE_ATTRIBUTES)
        assert attrs, "the join must push the attribute set (which carries the entity id)"
        bot.entity_id, _ = read_attributes(attrs[-1])
        assert bot.position is not None, "the join must teleport the player to the spawn"
        spawn_x, spawn_y, spawn_z = bot.position
        step(f"  OK join: 20/20/5, no effects, spawn at "
             f"({spawn_x:.2f}, {spawn_y:.2f}, {spawn_z:.2f})")

        # --- 1. the golden apple: Absorption and Regeneration ---------------
        # The food bar is drained first, so that the eat step is observable at
        # all: with a full bar and full health the apple's nutrition and
        # saturation move numbers that ServerPlayer.doTick's guard does not watch
        # (health, food, or saturation crossing zero), so it syncs nothing.
        step("-> /effect give EatBot hunger 30 255, then let it drain the bar")
        h.command("effect give EatBot hunger 30 255")
        # Forty ticks of amplifier-255 hunger: the effect's 1.28 exhaustion a
        # tick has to burn the bar's five saturation points (4 exhaustion each)
        # before the food number itself moves at all.
        tick(bot, 40)
        # The ticks above are what drain the bar, so their packets are part of
        # this measurement; clearing the effect is what *stops* the drain. Which
        # of the two drains carries the set_health is a question of the clock:
        # with the packet clock the clear banks nothing of its own, and with the
        # wall clock it used to bank a whole window of catch-up.
        packets = drain(bot, 1.0)
        packets += h.command("effect clear EatBot hunger")
        healths = [read_set_health(b) for b in all_of(packets, CB_SET_HEALTH)]
        assert healths, f"the drained bar must be synced: {[hex(p) for p, _ in packets]}"
        _, food_before, saturation_before = healths[-1]
        assert 0 < food_before < 20, f"hunger must drain the bar: {healths[-1]}"
        step(f"  OK drained to food {food_before}/20, saturation {saturation_before}")

        step("-> /give EatBot minecraft:golden_apple 1, then use_item")
        _, packets = h.eat("minecraft:golden_apple", ITEM_GOLDEN_APPLE, TICKS_DEFAULT)
        given = effects_of(packets)
        assert len(given) == 2, f"two effects, got {[hex(p) for p, _ in packets]}"
        # The data's own order: Regeneration (index 9) then Absorption (21).
        regen, absorption = given  # noqa: the two names are the assertion subjects
        assert regen == (bot.entity_id, EFFECT_REGENERATION, 1, 100,
                         FLAG_VISIBLE | FLAG_SHOW_ICON | FLAG_BLEND), regen
        assert absorption == (bot.entity_id, EFFECT_ABSORPTION, 0, 2400,
                              FLAG_VISIBLE | FLAG_SHOW_ICON | FLAG_BLEND), absorption
        step(f"  OK update_mob_effect: regeneration amp 1 for {regen[3]} ticks, "
             f"absorption amp 0 for {absorption[3]}")
        # ...and the same drain ate: Consumable.onConsume runs the item's
        # ConsumableListener components (FoodData.add for food) at step 1 and
        # on_consume_effects at step 2, so the apple's 4 nutrition and its 9.6
        # saturation are already on the bar when its effects arrive.
        healths = [read_set_health(b) for b in all_of(packets, CB_SET_HEALTH)]
        assert healths, f"eating must sync the food bar: {[hex(p) for p, _ in packets]}"
        expected_food = min(20, food_before + 4)
        expected_saturation = min(float(expected_food), saturation_before + 9.6)
        assert healths[-1][0] == 20.0 and healths[-1][1] == expected_food, healths[-1]
        assert abs(healths[-1][2] - expected_saturation) < 0.05, \
            f"the apple's 9.6 saturation must land: {healths[-1]}"
        step(f"  OK ate in the same drain: food {healths[-1][1]}, saturation "
             f"{healths[-1][2]}")

        # --- 2. Absorption eats real damage ----------------------------------
        step("-> /damage EatBot 10 player_attack under 4 absorption points")
        landed, packets = h.hit(10)
        # Player.actuallyHurt: the absorption floor takes min(damage, absorption)
        # before the health does, so 10 damage costs 6 health.
        assert abs(landed - 6.0) < 0.001, f"4 absorption must eat 4 damage: {landed}"
        step(f"  OK 10 damage landed as {landed} (4 absorbed)")

        # --- 3. both effects report themselves, on their own clocks ----------
        # Regeneration was granted with exactly 100 ticks, and the clock is the
        # packet count (`PLUSMC_SURVIVAL_CLOCK=packet`), so "100 ticks" is a
        # number this test can land on instead of a window it hopes to see the
        # packet in: 99 packets must leave it running and the 100th must end it.
        # The barrier says which side of the count each removal fell on.
        #
        # The count starts at the tick the eat granted it (the last packet of
        # `h.eat`'s own batch); the /damage in step 2 is the one packet sent
        # since, so the barrier below has to leave one tick for the removal.
        since_grant = 1
        step(f"-> tick {REGEN_TICKS - 1}: Absorption gone, Regeneration alive")
        packets = barrier(bot, REGEN_TICKS - since_grant - 1)
        removed = removals(packets)
        # Absorption is not on a countdown at all: AbsorptionMobEffect's tick
        # removes it the tick it finds no absorption hearts left, and the hit
        # above spent all four — so it is gone on the *first* tick of the batch,
        # a full 2400 ticks before the duration it was granted with, and
        # Regeneration is still running 99 ticks in.
        assert removed == [(bot.entity_id, EFFECT_ABSORPTION)], \
            f"only Absorption may be gone at tick {REGEN_TICKS - 1} of " \
            f"{REGEN_TICKS}: {removed}"
        h.health = last_health(packets, h.health)

        step(f"-> tick {REGEN_TICKS}: Regeneration comes off, not a tick earlier")
        packets = barrier(bot, 1)
        removed = removals(packets)
        # One packet after the last: the duration reaching zero is what removes
        # it (MobEffectInstance.tickDownDuration), so the 100th tick after the
        # grant is the one that reports the removal and the 99th does not.
        assert removed == [(bot.entity_id, EFFECT_REGENERATION)], \
            f"Regeneration must expire on exactly its {REGEN_TICKS}th tick, " \
            f"never before: {removed}"
        h.health = last_health(packets, h.health)
        step("  OK Absorption removed itself where the hearts ran out, Regeneration "
             f"on exactly the {REGEN_TICKS}th tick")

        # --- 4. a milk bucket clears everything ------------------------------
        step("-> /effect give EatBot speed 30 1")
        packets = h.command("effect give EatBot speed 30 1")
        assert effects_of(packets)[-1][1] == EFFECT_SPEED, effects_of(packets)
        assert speed_modifiers(packets), "Speed must push its attribute modifier"
        step("-> /effect give EatBot regeneration 30 0")
        packets = h.command("effect give EatBot regeneration 30 0")
        assert effects_of(packets)[-1][1] == EFFECT_REGENERATION, effects_of(packets)

        step("-> /give EatBot minecraft:milk_bucket 1 and drink it (it leaves a bucket)")
        _, packets = h.eat("minecraft:milk_bucket", ITEM_MILK_BUCKET, TICKS_DEFAULT,
                           remainder=(ITEM_BUCKET, 1))
        removed = removals(packets)
        # RemoveAllEffects reports every effect it takes, in the set's own order
        # (by effect id: Speed is 0, Regeneration 9) — one packet each.
        assert removed == [(bot.entity_id, EFFECT_SPEED),
                           (bot.entity_id, EFFECT_REGENERATION)], removed
        assert speed_modifiers(packets) == [], \
            "clearing Speed must take its attribute modifier with it"
        step(f"  OK remove_mob_effect x{len(removed)} and the modifier is gone")

        step("-> /effect clear EatBot (nothing left)")
        packets = h.command("effect clear EatBot")
        assert "There are no effects to remove" in feedback(packets), feedback(packets)

        # --- 5. the honey bottle removes poison, and only poison -------------
        step("-> /effect give EatBot poison 30 0, /effect give EatBot hunger 30 0")
        assert h.command("effect give EatBot poison 30 0")
        assert h.command("effect give EatBot hunger 30 0")
        step("-> /give EatBot minecraft:honey_bottle 1 and drink it (it leaves a bottle)")
        _, packets = h.eat("minecraft:honey_bottle", ITEM_HONEY_BOTTLE, TICKS_HONEY_BOTTLE,
                           remainder=(ITEM_GLASS_BOTTLE, 1))
        removed = removals(packets)
        assert removed == [(bot.entity_id, EFFECT_POISON)], \
            f"honey removes poison and nothing else: {removed}"
        step("  OK remove_mob_effect(poison) alone, hunger untouched")
        h.command("effect clear EatBot")

        # --- 6. the use remainder: a stew hands the bowl back ----------------
        # `ItemStack.finishUsingItem` runs `applyAfterUseComponentSideEffects`
        # once Consumable's three steps have returned, so the remainder is
        # decided *after* the stack shrank — which is what separates the two
        # destinations `UseRemainder.convertIntoRemainder` has. The bar is
        # drained first because a stew is not `can_always_eat`: at a full one the
        # use is refused outright and nothing below would happen at all.
        step("-> drain the bar again (a stew is refused at a full one)")
        h.command("effect give EatBot hunger 30 255")
        tick(bot, 40)
        drain(bot, 1.0)
        h.command("effect clear EatBot hunger")

        # The emptied stack hands the remainder to the *hand*. A stew stacks to
        # one, so one stew really is the whole stack — which is why the bowl ends
        # up exactly where the stew was.
        step("-> /give EatBot minecraft:mushroom_stew 1 and eat it")
        _, packets = h.eat("minecraft:mushroom_stew", ITEM_MUSHROOM_STEW, TICKS_DEFAULT,
                           remainder=(ITEM_BOWL, 1))
        assert not all_of(packets, CB_CONTAINER_SET_CONTENT), \
            "an emptied stack needs no inventory resync"
        step("  OK the emptied slot now holds minecraft:bowl (920)")

        # A stack that *survives* cannot hand the remainder to the hand, so it
        # goes to the inventory instead (ServerPlayer.handleExtraItemsCreatedOnUse)
        # — and because the slot it lands in is whatever `Inventory.add` found, the
        # server resyncs the whole menu rather than guessing. A honey bottle is
        # the only item in 26.2 that can even reach this branch: a stew, a milk
        # bucket and a potion all stack to one, and the honey bottle stacks to 16.
        # No draining needed for these two: a honey bottle (like a chorus fruit)
        # carries can_always_eat, so the bar's state is irrelevant to them — only
        # the stew above, which is an ordinary food, had to have room.
        step("-> /give EatBot minecraft:honey_bottle 2 and drink one")
        h.command("give EatBot minecraft:honey_bottle 1")
        h.command("give EatBot minecraft:honey_bottle 1")
        slot = h.select(ITEM_HONEY_BOTTLE)
        before = h.slots
        assert before[slot] == (ITEM_HONEY_BOTTLE, 2), before[slot]
        bottles_before = total_of(before, ITEM_GLASS_BOTTLE)
        use_item(bot)
        tick(bot, TICKS_HONEY_BOTTLE)
        packets = drain(bot, 1.2)
        sets = all_of(packets, CB_CONTAINER_SET_SLOT)
        assert sets, f"the shrunk stack must resync: {[hex(p) for p, _ in packets]}"
        _, _, used_slot, stack = decode_set_slot(sets[-1])
        assert used_slot == slot and stack == (ITEM_HONEY_BOTTLE, 1), (used_slot, stack, slot)
        contents = all_of(packets, CB_CONTAINER_SET_CONTENT)
        assert contents, "the bottle that could not take the slot needs the menu resynced"
        _, _, after, _ = decode_content(contents[-1])
        assert total_of(after, ITEM_GLASS_BOTTLE) == bottles_before + 1, \
            f"exactly one bottle must appear: {total_of(after, ITEM_GLASS_BOTTLE)} " \
            f"vs {bottles_before}"
        assert after[slot] == (ITEM_HONEY_BOTTLE, 1), \
            f"the stack keeps the slot: {after[slot]}"
        step("  OK the honey stayed in hand and the empty bottle went to the inventory")

        # --- 7. a chorus fruit hops the player, and locks itself out ---------
        # Chunk (-2, -1) of the copied fixture is real dark-forest terrain — stone
        # under a grass surface — so the landing search has ground to find. The
        # rest of this test runs in air, where the search would rightly refuse.
        step("-> /tp -24 100 -8, inside that chunk")
        h.command("tp -24 100 -8")
        assert bot.position == (-24.0, 100.0, -8.0), bot.position
        # Two of them, because the cooldown this checks gates the *next* use:
        # `UseCooldown.apply` adds the range to the player when the first use
        # finishes, and ServerItemCooldowns sends the client the full duration.
        step("-> /give EatBot minecraft:chorus_fruit 2 and eat one")
        h.command("give EatBot minecraft:chorus_fruit 1")
        h.command("give EatBot minecraft:chorus_fruit 1")
        slot = h.select(ITEM_CHORUS_FRUIT)
        assert h.slots[slot] == (ITEM_CHORUS_FRUIT, 2), h.slots[slot]
        here = bot.position
        use_item(bot)
        tick(bot, TICKS_DEFAULT)
        # A quiet drain, not a fixed one: the cooldown's 20 ticks are measured on
        # the same clock the idle wall time feeds, so the refusal below has to
        # arrive before they are used up.
        packets = drain_quiet(bot)
        cooldowns = [read_cooldown(b) for b in all_of(packets, CB_COOLDOWN)]
        assert cooldowns == [(CHORUS_FRUIT_GROUP, COOLDOWN_CHORUS_FRUIT)], cooldowns
        assert find(packets, CB_PLAYER_POSITION) is not None, \
            f"the chorus fruit must teleport: {[hex(p) for p, _ in packets]}"
        # The fruit has no use_remainder, so the stack simply dropped to one.
        sets = all_of(packets, CB_CONTAINER_SET_SLOT)
        assert sets, f"the slot must resync: {[hex(p) for p, _ in packets]}"
        _, _, used_slot, stack = decode_set_slot(sets[-1])
        assert used_slot == slot and stack == (ITEM_CHORUS_FRUIT, 1), (used_slot, stack)
        step(f"  OK cooldown ({CHORUS_FRUIT_GROUP}, {COOLDOWN_CHORUS_FRUIT}) and the "
             "stack went 2 -> 1")
        x, y, z = bot.position
        # TeleportRandomlyConsumeEffect rolls across a diameter of 16, so the
        # candidate is within 8 blocks of the player on each axis.
        assert abs(x - here[0]) <= 8.0 and abs(z - here[2]) <= 8.0, \
            f"({x}, {z}) is outside the 16-block diameter of ({here[0]}, {here[2]})"
        # ...and the landing search only ever walks *down* from the candidate, so
        # the landing y can never be above the candidate's own (at most 8 up) —
        # and here the terrain is below the teleport, so it really did descend to
        # a surface rather than land in the air at the player's height.
        assert y <= here[1] + 8.0, f"y {y} is above the candidate bound {here[1] + 8.0}"
        assert y < here[1], f"the search must have descended: y {y} vs {here[1]}"
        assert bot.position != here, "the fruit must actually move"
        events = [read_entity_event(b) for b in all_of(packets, CB_ENTITY_EVENT)]
        assert (bot.entity_id, 46) in events, f"the teleport particles: {events}"
        step(f"  OK moved ({here[0]:.2f}, {here[1]:.2f}, {here[2]:.2f}) -> "
             f"({x:.2f}, {y:.2f}, {z:.2f}) with entity_event 46")

        # A second use inside the window is refused, and refused *silently*: this
        # is ServerPlayerGameMode.useItem's gate, which sits ahead of everything
        # the item itself would do, so nothing at all comes back — no teleport,
        # no second cooldown, and not even a slot resync. No ticks are sent
        # between the two uses, so the 20 ticks really are still running.
        step("-> use_item again inside the 20-tick window (must be refused)")
        locked_at = bot.position
        use_item(bot)
        refused = drain_quiet(bot)
        locked = [read_cooldown(b) for b in all_of(refused, CB_COOLDOWN)]
        assert not locked, f"a locked item must not start a cooldown: {locked}"
        assert find(refused, CB_PLAYER_POSITION) is None, "and must not be used again"
        assert not all_of(refused, CB_CONTAINER_SET_SLOT), "and must not consume the stack"
        assert bot.position == locked_at, bot.position
        step("  OK the use was refused outright")

        # ...and the server clears it on its own clock: the range ends `ticks`
        # ticks after it started, and the client is told with a duration of 0
        # (ServerItemCooldowns.onCooldownEnded). Both halves of that are exact
        # here: `ItemCooldowns.add` sets `end = tickCount + 20` and `tick`
        # reports the group the tick it reaches it, so tick 19 must still be
        # silent and tick 20 must clear it. The refused use above is the one
        # packet sent since it started, which is the count the barrier is
        # measured against.
        since_start = 1
        step(f"-> tick {COOLDOWN_CHORUS_FRUIT - 1}: still on cooldown")
        packets = barrier(bot, COOLDOWN_CHORUS_FRUIT - 1 - since_start)
        ends = [read_cooldown(b) for b in all_of(packets, CB_COOLDOWN)]
        assert ends == [], \
            f"the cooldown must still be running at {COOLDOWN_CHORUS_FRUIT - 1} " \
            f"of {COOLDOWN_CHORUS_FRUIT} ticks: {ends}"
        step(f"-> tick {COOLDOWN_CHORUS_FRUIT}: the client is told it is over")
        packets = barrier(bot, 1)
        ends = [read_cooldown(b) for b in all_of(packets, CB_COOLDOWN)]
        assert ends == [(CHORUS_FRUIT_GROUP, 0)], ends
        # Now the fruit works again, which is the other half of the gate — and
        # the second use is `consume_ticks` long on the same clock, so one tick
        # short of it the fruit must still be un-eaten.
        step("-> use the second fruit, now that the cooldown is over")
        use_item(bot)
        packets = barrier(bot, TICKS_DEFAULT - 1)
        assert find(packets, CB_PLAYER_POSITION) is None, \
            f"a {TICKS_DEFAULT}-tick use must not finish one tick early: " \
            f"{[hex(p) for p, _ in packets]}"
        assert not all_of(packets, CB_COOLDOWN), "and must not have re-locked yet"
        packets = barrier(bot, 1)
        # What the gate guarantees is that the use *ran*, and that is what is
        # asserted: `UseCooldown.apply` re-locks the item for its full duration
        # and `ItemStack.consume(1, user)` empties the hand. The hop itself is
        # deliberately not asserted here, because it is not deterministic in
        # this world. `TeleportRandomlyConsumeEffect` rolls sixteen candidates
        # and `LivingEntity.randomTeleport` refuses every one of them with no
        # floor under it or a box that is not free — and this fixture is a single
        # chunk of dark forest, so two thirds of a *second* hop's candidates fall
        # in the air beside it, and most of the rest land inside a hill, a canopy
        # or a pool. The first fruit above already proves the hop lands when the
        # world has ground under the roll; here the fruit can legitimately be
        # eaten without going anywhere, which is exactly what vanilla does (the
        # effect's return value does not undo the consume).
        assert [read_cooldown(b) for b in all_of(packets, CB_COOLDOWN)] == \
            [(CHORUS_FRUIT_GROUP, COOLDOWN_CHORUS_FRUIT)], \
            f"the second use must run and re-lock: {[hex(p) for p, _ in packets]}"
        sets = all_of(packets, CB_CONTAINER_SET_SLOT)
        assert sets, f"and consume the stack: {[hex(p) for p, _ in packets]}"
        _, _, emptied_slot, stack = decode_set_slot(sets[-1])
        assert emptied_slot == slot and stack is None, \
            f"the last fruit must empty the hand: {emptied_slot} {stack}"
        step(f"  OK cleared on exactly tick {COOLDOWN_CHORUS_FRUIT}, and the second "
             f"fruit was eaten on its {TICKS_DEFAULT}th")

        # --- 8. an ominous bottle is consumed and silent ---------------------
        step("-> /give EatBot minecraft:ominous_bottle 1 (play_sound only)")
        _, packets = h.eat("minecraft:ominous_bottle", ITEM_OMINOUS_BOTTLE, TICKS_DEFAULT)
        assert not effects_of(packets), f"no effect, only a sound: {effects_of(packets)}"
        assert not removals(packets), removals(packets)
        assert find(packets, CB_PLAYER_POSITION) is None, "and no teleport"
        # ...and no cooldown of its own either: in 26.2 an ominous bottle carries
        # no use_cooldown component at all, so nothing but its 1.6 s drink time
        # paces it. The only cooldown packet here is the chorus fruit's range
        # running out (a duration of 0), which is exactly the component being
        # absent rather than a limitation of this server.
        cooldowns = [read_cooldown(b) for b in all_of(packets, CB_COOLDOWN)]
        assert all(group == CHORUS_FRUIT_GROUP for group, _ in cooldowns), cooldowns
        assert not any(duration == COOLDOWN_CHORUS_FRUIT for _, duration in cooldowns), \
            f"the ominous bottle must not start one: {cooldowns}"
        step("  OK consumed, and the one thing it asks for is a sound")

        bot.close()
    finally:
        proc.terminate()
        proc.wait(timeout=15)
    print("\nPASS: consume test")
    return 0


if __name__ == "__main__":
    sys.exit(main())
