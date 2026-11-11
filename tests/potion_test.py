#!/usr/bin/env python3
"""Integration test: drinking a potion over the wire.

A potion is not a consumable whose effects live in `on_consume_effects`: they
live on the stack, in `minecraft:potion_contents`, and the component applies them
itself (`PotionContents implements ConsumableListener`, and
`Consumable.onConsume` runs the stack's listeners before its own effect list).
This test drives that end to end:

  * `/give PotBot minecraft:potion[potion_contents={potion:"minecraft:healing"}]`
    hands out a bottle that carries the component — the menu resync the server
    sends back is decoded here, component and all, which is the half of the
    feature a server-side-only test could not see;
  * drinking it heals exactly 4 (Healing I is `4 << 0`), *immediately*: an
    instantaneous effect is applied rather than installed, so there is no
    update_mob_effect for it at all — and the emptied slot ends up holding the
    glass bottle the `use_remainder` component leaves behind;
  * Strong Healing heals 8 (`4 << 1`), which is the amplifier reaching the
    instant path;
  * Swiftness *does* install an effect: update_mob_effect with Speed's registry
    id, duration 3600, amplifier 0, the visible+show_icon+blend flags, and the
    movement_speed attribute modifier that comes with it;
  * Harming damages the drinker by 6 with the `indirect_magic` type — not the
    `magic` a ticked instant effect hurts with (`PotionContents` passes the
    drinker as both source and owner: `damageSources().indirectMagic(...)`), with
    a `/damage` control proving the two type ids apart;
  * a water bottle applies nothing at all (its contents name a potion with no
    effects) and still hands the bottle back;
  * a potion whose contents came from a *client* — a custom_effects list written
    with set_creative_slot — is honoured too: its instantaneous entry heals and
    its lasting one lands with the amplifier and duration the client asked for;
  * and the argument form refuses what it does not understand.

Every drink in here is a 32-tick use, and the survival clock *counts packets*
(`PLUSMC_SURVIVAL_CLOCK=packet`): one recognized serverbound packet is exactly
one game tick, and idle wall time contributes none. So `Harness.drink` can
bound the use rather than outlast it — one tick short of 32 nothing has been
applied, and the 32nd tick carries the whole effect — and the food bar's
exhaustion spend (the precondition that keeps regeneration out of the deltas
this test asserts) is the packets the test sends rather than its drains' wall
time.

Run: potion_test.py /path/to/plusmc
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
EFFECT_INSTANT_HEALTH = 5
ITEM_GLASS_BOTTLE = 1270
ITEM_POTION = 1271

# minecraft:potion ids (src/world/potion_table.h).
POTION_WATER = 0
POTION_HEALING = 24

# The minecraft:data_component_type id of potion_contents.
COMPONENT_POTION_CONTENTS = 53

# minecraft:damage_type ids (src/world/damage_table.h): `indirectMagic(source,
# owner)`, which is what a drunk potion uses, and the `magic()` a *ticked* instant
# effect uses.
DMG_INDIRECT_MAGIC = 23
DMG_MAGIC = 27

# minecraft:attribute id of movement_speed (src/world/attribute_table.h).
ATTR_MOVEMENT_SPEED = 26

# Consumable's 1.6 s default: a potion is drunk over 32 ticks.
TICKS_DRINK = 32

# ClientboundUpdateMobEffectPacket's flag bits (src/protocol/ids.h).
FLAG_VISIBLE = 2
FLAG_SHOW_ICON = 4
FLAG_BLEND = 8

CB_CONTAINER_SET_CONTENT = 0x12
CB_CONTAINER_SET_SLOT = 0x14
CB_DAMAGE_EVENT = 0x19
CB_KEEP_ALIVE = 0x2D
CB_PLAYER_POSITION = 0x49
CB_SET_HEALTH = 0x6A
CB_SYSTEM_CHAT = 0x7C
CB_UPDATE_ATTRIBUTES = 0x86
CB_UPDATE_MOB_EFFECT = 0x87

SB_CHAT_COMMAND = 0x07
SB_KEEP_ALIVE = 0x1C
SB_SET_CARRIED_ITEM = 0x36
SB_SET_CREATIVE_SLOT = 0x39
SB_USE_ITEM = 0x43

# Menu slots: 36 is hotbar 0, which a fresh session has selected and therefore
# the slot /give fills first.
SLOT_MENU_HOTBAR_FIRST = 36
MENU_HOTBAR_SLOTS = 9

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
    length, pos = rd_varint(buf, pos)
    return buf[pos:pos + length].decode(), pos + length


# --- decoding ---------------------------------------------------------------

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
        values[attribute] = (round(base, 4), mods)
    assert pos == len(body), f"update_attributes: {len(body) - pos} trailing byte(s)"
    return eid, values


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


def read_stack(body, pos=0):
    """The *trusted* (server-sent) ItemStack: [varint count][varint item]
    [varint added][varint removed] and then, per added entry, [varint id][value]
    — with no length, which is the whole difference from the delimited form a
    client sends (see item_stack_bytes). A component's value can only be skipped
    by knowing it, so any component other than potion_contents is an assertion
    failure here rather than something silently walked over."""
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
    """MobEffectInstance.Details: [varint amplifier][varint duration][bool
    ambient][bool visible][bool show_icon][bool hidden] and then, when hidden,
    the nested Details — which carries no effect id, being the same effect."""
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
    [varint count][instances] [bool][utf8 name]?. Returns ((potion, effects),
    pos)."""
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
    """(potion, custom effects) the menu stack's potion_contents holds."""
    assert stack is not None, "no stack to read contents from"
    assert COMPONENT_POTION_CONTENTS in stack["patch"], \
        f"the stack carries no potion_contents: {sorted(stack['patch'])}"
    return stack["patch"][COMPONENT_POTION_CONTENTS]


# --- the packets the test *sends* -------------------------------------------

def item_stack_bytes(item, count=1, patch=None):
    """The delimited (client-sent) ItemStack: each component's value is
    byte-length prefixed, which is what lets a server skip what it does not
    model."""
    out = varint(count) + varint(item)
    patch = patch or {}
    out += varint(len(patch)) + varint(0)
    for cid, value in patch.items():
        out += varint(cid) + varint(len(value)) + value
    return out


def potion_contents_bytes(potion=None, effects=()):
    """PotionContents.STREAM_CODEC's four fields, with the two display-only ones
    absent: [potion][color][effects][name]."""
    out = b""
    out += b"\x00" if potion is None else b"\x01" + varint(potion)
    out += b"\x00"                    # custom_color
    out += varint(len(effects))
    for effect, amplifier, duration in effects:
        out += varint(effect) + varint(amplifier) + varint(duration)
        out += b"\x00\x01\x01"        # ambient, visible, show_icon
        out += b"\x00"                # no hidden layer
    out += b"\x00"                    # custom_name
    return out


def read_position(body):
    _, pos = rd_varint(body, 0)
    return struct.unpack(">ddd", body[pos:pos + 24])


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


def drain(bot, timeout=1.2):
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


def healths_of(packets):
    return [read_set_health(b) for b in all_of(packets, CB_SET_HEALTH)]


def last_health(packets, fallback):
    healths = healths_of(packets)
    return healths[-1] if healths else fallback


# (There is deliberately no `tick` helper here. Every tick this test wants is
# part of something it is *measuring* — a 32-tick drink, ten ticks of hunger —
# so each one goes through `wire_client.barrier`, which spends the count and
# hands back exactly what it produced. A bare "send N flying packets" helper
# would only invite an assertion that cannot say how many ticks it saw.)


def use_item(bot):
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
    """Commands, the menu, and the health bar."""

    def __init__(self, bot):
        self.bot = bot
        self.health = (20.0, 20, 5.0)
        self.state = None
        self.slots = None

    def command(self, text, timeout=1.2):
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

    def select(self, slot):
        assert SLOT_MENU_HOTBAR_FIRST <= slot < SLOT_MENU_HOTBAR_FIRST + MENU_HOTBAR_SLOTS, \
            f"menu slot {slot} is outside the hotbar"
        self.bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", slot - SLOT_MENU_HOTBAR_FIRST))
        drain(self.bot, 0.2)

    def damage(self, amount, type_name="player_attack"):
        """One /damage, returning (health before, health after)."""
        before = self.health
        packets = self.command(f"damage {self.bot.name} {amount} {type_name}")
        assert "Applied" in feedback(packets), f"the hit was refused: {feedback(packets)}"
        after = last_health(packets, None)
        assert after is not None, f"the hit must report health: {[hex(p) for p, _ in packets]}"
        self.health = after
        return before, after, packets

    def give_potion(self, potion, count=1):
        """`/give` a bottle whose contents name `potion`. Returns (slot, packets):
        the *hotbar* slot the stack landed in, which is where a use has to be
        pointed."""
        packets = self.command(
            f'give {self.bot.name} minecraft:potion[potion_contents={{potion:"{potion}"}}] {count}')
        assert "Gave " in feedback(packets), feedback(packets)
        contents = all_of(packets, CB_CONTAINER_SET_CONTENT)
        assert contents, "a /give must resync the inventory"
        _, _, self.slots, _ = decode_content(contents[-1])
        for i, stack in enumerate(self.slots):
            if stack is not None and stack["item"] == ITEM_POTION:
                return i, packets
        raise AssertionError(f"the potion is nowhere in the menu: {self.slots}")

    def drink(self, slot):
        """Select `slot`, use it, and spend the 32-tick drink, returning the
        packets the *finishing* tick produced.

        A potion's `consumable.consume_seconds` is 1.6 s = 32 ticks and
        `LivingEntity.updateUsingItem` finishes the use on the tick the countdown
        reaches zero, so the duration is bounded rather than outlasted: one tick
        short nothing has happened at all — no slot resync, no effect, no heal —
        and the 32nd tick carries the whole of it. `wire_client.barrier` is what
        makes that two-sided; the old `tick(...)` outlasted the 32 ticks on the
        wall clock and could not have told the difference.
        """
        self.select(slot)
        use_item(self.bot)
        early = wire_client.barrier(self.bot, TICKS_DRINK - 1)
        assert not all_of(early, CB_CONTAINER_SET_SLOT), \
            f"a {TICKS_DRINK}-tick drink must not finish a tick early: " \
            f"{[hex(p) for p, _ in early]}"
        assert effects_of(early) == [] and not all_of(early, CB_DAMAGE_EVENT), \
            f"nothing may be applied before the drink finishes: {early}"
        packets = wire_client.barrier(self.bot, 1)
        self.health = last_health(packets, self.health)
        return packets

    def drained_bottle(self, packets, slot):
        """The stack the drink left in `slot`, asserted to be the glass bottle
        `minecraft:use_remainder` hands back."""
        sets = all_of(packets, CB_CONTAINER_SET_SLOT)
        assert sets, f"the used slot must resync: {[hex(p) for p, _ in packets]}"
        window, _, used_slot, stack = decode_set_slot(sets[-1])
        assert window == 0 and used_slot == slot, (window, used_slot, slot)
        assert stack is not None and stack["item"] == ITEM_GLASS_BOTTLE and stack["count"] == 1, \
            f"the potion must leave its bottle: {stack}"
        assert not stack["patch"], f"the bottle carries no components: {stack}"
        return stack


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-potion-")
    # The same fixture chunk the consume test copies: potions need no terrain,
    # but a world with real region data is what the other survival tests run in.
    shutil.copytree(os.path.join(os.path.dirname(__file__), "fixtures", "ow3region"),
                    os.path.join(world, "region"))
    env = dict(os.environ)
    # Only this test's own packets may advance the survival clock, and they
    # advance it by exactly one tick each (`PLUSMC_SURVIVAL_CLOCK=packet`). That
    # is what makes a 32-tick drink and the food bar's exhaustion spend counts
    # rather than guesses: a keep-alive every 60 s is the clamped maximum, and
    # the day cycle is frozen so nothing else ticks.
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"

    proc = None
    try:
        step(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", "127.0.0.1", "--port", "0", "--motd", "potion test",
             "--offline-mode", "--gamemode", "0", "--data", data_root, "--world", world,
             "--no-flat-world"], wait_listening, env=env)

        bot = Bot("PotBot", port)
        wire_client.record(bot, note="potion-wire: drinking a potion's own effects")
        bot.join()
        bot.start_reader()
        h = Harness(bot)
        join_packets = drain(bot, 1.2)
        healths = healths_of(join_packets)
        assert healths and healths[-1] == (20.0, 20, 5.0), f"join health: {healths}"
        attrs = all_of(join_packets, CB_UPDATE_ATTRIBUTES)
        assert attrs, "the join must push the attribute set (which carries the entity id)"
        bot.entity_id, _ = read_attributes(attrs[-1])
        assert not effects_of(join_packets), "a fresh join has no effects"
        assert bot.position is not None, "the join must teleport the player to the spawn"
        step("  OK join: 20/20/5, no effects")

        # The food bar would walk the health bar under every drink below: with a
        # full bar and any saturation at all it heals every 10 ticks, and a drink
        # takes 32 — so the exact deltas this test asserts are only meaningful
        # with neither regeneration branch running. The bar is drained first
        # (FoodData spends saturation before food) and the precondition is
        # *asserted* rather than assumed: saturation at zero and food at or below
        # 10 leaves only starvation, which needs 80 consecutive ticks at zero
        # food.
        step("-> drain the food bar so no regeneration runs while drinking")
        h.command("effect give PotBot hunger 30 255")
        for _ in range(8):
            # The health syncs the hunger produces arrive on the *tick* packets,
            # not on the command's, so both are collected (the barrier hands back
            # exactly what its ten ticks produced) and folded in order — the
            # command's drain last, because it is the newest.
            packets = wire_client.barrier(bot, 10)
            packets += h.command("effect clear PotBot hunger")
            latest = last_health(packets, h.health)
            h.health = latest
            _, food, saturation = latest
            if 0 < food <= 10 and saturation == 0.0:
                break
            h.command("effect give PotBot hunger 30 255")
        food, saturation = h.health[1], h.health[2]
        assert 0 < food <= 10 and saturation == 0.0, \
            f"the drain must leave food in (0, 10] and no saturation: {h.health}"
        step(f"  OK food {food}/20, saturation {saturation}")

        # --- 1. the component the server hands out ---------------------------
        step("-> /give PotBot minecraft:potion[potion_contents={potion:\"minecraft:healing\"}]")
        slot, give = h.give_potion("minecraft:healing")
        stack = h.slots[slot]
        assert stack["count"] == 1, stack
        potion, custom = potion_of(stack)
        assert potion == POTION_HEALING and custom == [], (potion, custom)
        assert slot == SLOT_MENU_HOTBAR_FIRST, (slot, h.slots)
        step(f"  OK the menu stack carries potion_contents naming potion {potion}")

        # --- 2. Healing I: applied at once, 4 health, and the bottle back ----
        before, after, _ = h.damage(10)               # 20 -> 10
        assert after[0] == 10.0, (before, after)
        packets = h.drink(slot)
        # An instantaneous effect is *applied*, not installed: nothing is added
        # to the effect set, so no update_mob_effect can appear for it.
        assert effects_of(packets) == [], effects_of(packets)
        healths = healths_of(packets)
        assert healths, f"the heal must sync health: {[hex(p) for p, _ in packets]}"
        assert healths[-1][0] == 14.0, f"10 + (4 << 0) = 14: {healths}"
        h.drained_bottle(packets, slot)
        step(f"  OK healed 10.0 -> {healths[-1][0]}, no effect packet, slot holds the bottle")

        # --- 3. the amplifier reaches the instant path: `4 << 1` = 8 ---------
        _, after, _ = h.damage(10)                    # 14 -> 4
        assert after[0] == 4.0, after
        slot, _ = h.give_potion("minecraft:strong_healing")
        packets = h.drink(slot)
        healths = healths_of(packets)
        assert healths and healths[-1][0] == 12.0, f"4 + (4 << 1) = 12: {healths}"
        assert effects_of(packets) == [], effects_of(packets)
        h.drained_bottle(packets, slot)
        step(f"  OK strong healing: 4.0 -> {healths[-1][0]}")

        # --- 4. a lasting effect installs, and reports itself ----------------
        slot, _ = h.give_potion("minecraft:swiftness")
        packets = h.drink(slot)
        given = effects_of(packets)
        assert given == [(bot.entity_id, EFFECT_SPEED, 0, 3600,
                          FLAG_VISIBLE | FLAG_SHOW_ICON | FLAG_BLEND)], given
        attribute_packets = all_of(packets, CB_UPDATE_ATTRIBUTES)
        assert attribute_packets, "an effect landing must refresh the attributes"
        _, values = read_attributes(attribute_packets[-1])
        assert ATTR_MOVEMENT_SPEED in values, sorted(values)
        names = [m[0] for m in values[ATTR_MOVEMENT_SPEED][1]]
        # MobEffect.AttributeTemplate.create's id: `minecraft:effect.<name>`,
        # which is what lets the client replace the modifier rather than stack a
        # second one.
        assert "minecraft:effect.speed" in names, names
        h.drained_bottle(packets, slot)
        step(f"  OK update_mob_effect: speed amp 0 for 3600 ticks, and {names}")

        # --- 5. Harming hurts with indirect_magic, not magic -----------------
        # The control first: `/damage ... magic` names the type a *ticked*
        # instant effect uses, so the two ids can be told apart by the same
        # decoder.
        _, _, control = h.damage(1, "magic")
        events = all_of(control, CB_DAMAGE_EVENT)
        assert events, "the control hit must report a damage_event"
        control_type = read_damage_event(events[-1])["type"]
        assert control_type == DMG_MAGIC, control_type

        slot, _ = h.give_potion("minecraft:harming")
        after = h.health
        packets = h.drink(slot)
        events = all_of(packets, CB_DAMAGE_EVENT)
        assert events, f"harming must damage the drinker: {[hex(p) for p, _ in packets]}"
        event = read_damage_event(events[-1])
        assert event["entity"] == bot.entity_id, event
        assert event["type"] == DMG_INDIRECT_MAGIC, event
        assert event["type"] != control_type, (event, control_type)
        healths = healths_of(packets)
        assert healths and healths[-1][0] == after[0] - 6.0, \
            f"6 << 0 = 6 from {after}: {healths}"
        h.drained_bottle(packets, slot)
        step(f"  OK damage_event type {event['type']} (indirect_magic, not "
             f"{control_type}) and {after[0]} -> {healths[-1][0]} health")

        # --- 6. a water bottle applies nothing, and still leaves the bottle --
        slot, _ = h.give_potion("minecraft:water")
        potion, custom = potion_of(h.slots[slot])
        assert potion == POTION_WATER and custom == [], (potion, custom)
        health_before = h.health[0]
        packets = h.drink(slot)
        assert effects_of(packets) == [], effects_of(packets)
        assert not all_of(packets, CB_DAMAGE_EVENT), "a water bottle is not a harming one"
        assert h.health[0] == health_before, (h.health, health_before)
        h.drained_bottle(packets, slot)
        step(f"  OK nothing applied, {health_before} health unchanged, bottle back")

        # --- 7. a client's own contents are honoured -------------------------
        # A vanilla client in creative sends the whole stack, component and all,
        # with set_creative_slot. What is written here is a bottle whose
        # custom_effects name an instantaneous entry (Instant Health I, which
        # heals 4) and a lasting one at amplifier 2 for 200 ticks — neither of
        # which any /give could spell.
        step("-> /gamemode 1, set_creative_slot with custom_effects, /gamemode 0")
        before_health = h.health[0]
        assert before_health <= 16.0, f"the heal has to have room: {h.health}"
        # The Swiftness drunk above is still running, and Speed II pushed under a
        # running Speed I *merges* (an update, no blend flag) rather than landing
        # fresh — so the effect set is emptied first and this step tests the add.
        cleared = h.command("effect clear PotBot")
        assert effects_of(cleared) == [], effects_of(cleared)
        h.command("gamemode 1")
        slot = SLOT_MENU_HOTBAR_FIRST
        written = potion_contents_bytes(
            None, effects=[(EFFECT_INSTANT_HEALTH, 0, 1), (EFFECT_SPEED, 2, 200)])
        bot.send(SB_SET_CREATIVE_SLOT,
                 struct.pack(">h", slot)
                 + item_stack_bytes(ITEM_POTION, 1, {COMPONENT_POTION_CONTENTS: written}))
        echo = drain(bot, 1.0)
        sets = all_of(echo, CB_CONTAINER_SET_SLOT)
        assert sets, f"the creative write must be echoed back: {[hex(p) for p, _ in echo]}"
        stack = decode_set_slot(sets[-1])[3]
        potion, custom = potion_of(stack)
        assert potion is None and len(custom) == 2, (potion, custom, stack)
        assert custom[0][0] == EFFECT_INSTANT_HEALTH and custom[0][1]["duration"] == 1, custom
        assert custom[1][0] == EFFECT_SPEED and custom[1][1]["amplifier"] == 2, custom
        step("  OK the server kept both of the client's custom effects")
        h.command("gamemode 0")
        packets = h.drink(slot)
        given = effects_of(packets)
        # The instantaneous entry healed, so it has no effect packet at all; the
        # lasting one installed with exactly the amplifier and duration asked
        # for.
        assert given == [(bot.entity_id, EFFECT_SPEED, 2, 200,
                          FLAG_VISIBLE | FLAG_SHOW_ICON | FLAG_BLEND)], given
        healths = healths_of(packets)
        assert healths and healths[-1][0] == before_health + 4.0, \
            f"the custom instant entry heals 4: {healths} from {before_health}"
        h.drained_bottle(packets, slot)
        step(f"  OK custom effects applied: {before_health} -> {healths[-1][0]} health "
             "and speed amp 2 for 200 ticks")

        # --- 8. the argument form refuses what it does not understand --------
        for argument, expected in [
            ("minecraft:potion[foo=1]", "Unsupported component"),
            ('minecraft:potion[potion_contents={potion:"minecraft:nope"}]', "Unknown potion"),
            ("minecraft:potion[potion_contents={custom_color:1}]",
             "Unsupported potion_contents field"),
        ]:
            step(f"-> /give {argument} (must be refused)")
            packets = h.command(f"give {bot.name} {argument} 1")
            text = feedback(packets)
            assert expected in text, f"{argument}: {text!r}"
            assert "Gave" not in text, f"{argument} was given anyway: {text!r}"
            assert not all_of(packets, CB_CONTAINER_SET_CONTENT), "nothing may be written"
        step("  OK every refusal reported itself and gave nothing")

        bot.close()
    finally:
        proc.terminate()
        proc.wait(timeout=15)
    print("\nPASS: potion test")
    return 0


if __name__ == "__main__":
    sys.exit(main())
