#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Phase 4 — the Tier-B gate.

`docs/PLAN.md` §3.1 defines Tier B as the *mechanics depth* a server must have
before Phase 4 is done, and §11 makes it the phase's gate:

    combat/damage/armor, hunger, status effects, enchantments, redstone,
    fluids, pistons, TNT, projectiles, mobs (hostile + passive), breeding,
    villagers/trading, raid/boss basics, advancements, statistics, scoreboards

`tier-a-gate` is the Tier-A twin: one client, one world, one session, and the
whole list walked in order, because a regression that only appears when two
features meet — a window left open, a revision counter gone stale, a gamemode
that did not stick — passes every per-feature test and fails only there. This is
that gate for Tier B, labelled `tier-b` so CI can require it as one named gate.

The rest of the suite proves each area in depth, one feature at a time. What
this adds is *composition*: the areas are driven in one session, and every step
asserts the **wire consequence** of the feature, not a log line. Each step is
command-driven where it can be (`/effect`, `/enchant`, `/setblock`, `/summon`,
`/scoreboard`, `/give`, `/damage`, `/kill`), because a command's effect is
deterministic and cheap to assert, and the deep behaviour is the per-feature
tests' job; only combat, breeding and trading need a real interaction packet.

**What it does not walk, and why.** Tier B's list names *raid/boss basics*. The
wither, dragon and raid encounters have their own wire tests (`wither-fight`,
`dragon-fight` and `raid`); this gate walks only the bar it can assert in a
command-driven session — a summoned wither adds a UUID-keyed bar and `/mobs
kill` removes it — and leaves the encounter timing to those tests. The
half-health armour and the dragon's ground-start climb are pinned by
`plusmc-mob-test`. The gate does not re-walk the raid or the two fights; that is
a test-scope choice, not a missing feature. The Tier-C ledger
(`ctest -R tier-coverage`) is what measures the rest of the content surface.
Everything else on the §3.1 Tier-B list is walked below.

**How it starts a world.** An empty temp world (no generator, no `--worldgen`),
exactly like `tier-a-gate`: there is no surface, so a step that needs ground
lays its own with `/setblock` (the fluid, piston, redstone and TNT steps do).
The survival clock is packet-driven and the day cycle is off, so the only ticks
that assert are the packets this test sends.

Run: tier_b_gate_test.py /path/to/plusmc
"""

import os
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import craft_test as ct  # noqa: E402
import wire_client  # noqa: E402
from craft_test import (Bot, decode_content, drain, find, give,  # noqa: E402
                        parse_open_screen, rd_varint, wait_listening)
from nbt_component import chat_text, rd_nbt_component  # noqa: E402

# --- clientbound packet ids (src/protocol/ids.h / the generated table) --------
CB_ADD_ENTITY = 0x01
CB_AWARD_STATS = 0x03
CB_BLOCK_UPDATE = 0x08
CB_BOSS_EVENT = 0x09
CB_CONTAINER_SET_CONTENT = 0x12
CB_CONTAINER_SET_SLOT = 0x14
CB_DAMAGE_EVENT = 0x19
CB_ENTITY_EVENT = 0x22
CB_ENTITY_POSITION_SYNC = 0x23
CB_JOIN_GAME = 0x32
CB_MERCHANT_OFFERS = 0x35
CB_OPEN_SCREEN = 0x3C
CB_REMOVE_MOB_EFFECT = 0x4F
CB_SET_ENTITY_DATA = 0x65
CB_SET_EQUIPMENT = 0x68
CB_SET_HEALTH = 0x6A
CB_SET_OBJECTIVE = 0x6C
CB_SET_SCORE = 0x70
CB_SYNC_POS = 0x49
CB_SYSTEM_CHAT = 0x7C
CB_UPDATE_ADVANCEMENTS = 0x85
CB_UPDATE_ATTRIBUTES = 0x86
CB_UPDATE_MOB_EFFECT = 0x87

# --- serverbound packet ids ---------------------------------------------------
SB_ATTACK = 0x01
SB_CHAT_COMMAND = 0x07
SB_CLOSE_WINDOW = 0x13
SB_INTERACT = 0x1A
SB_FLYING = 0x21
SB_PLAYER_ACTION = 0x29
SB_SET_CARRIED_ITEM = 0x36
SB_CLIENT_COMMAND = 0x0C
SB_USE_ITEM = 0x43
SB_USE_ITEM_ON = 0x42
ACTION_STOP_DESTROY = 2
ACTION_RELEASE_USE_ITEM = 5
CLIENT_COMMAND_REQUEST_STATS = 1
# entity_event (0x22) event ids (src/protocol/ids.h). 18 is `kEntityEventLove`:
# the heart burst `Animal.setInLove` raises over the animal it just fed, and
# `finalizeSpawnChildFromBreeding` raises over each parent.
ENTITY_EVENT_LOVE = 18

# --- registry ids (generated tables) -----------------------------------------
ET_COW = 30
ET_ARROW = 6
ET_VILLAGER = 143
ET_ZOMBIE = 154
ET_PRIMED_TNT = 136
ITEM_DIAMOND_SWORD = 1050
ITEM_DIAMOND_CHESTPLATE = 1085
ITEM_FLINT_AND_STEEL = 1005
ITEM_BUCKET = 1128
ITEM_WATER_BUCKET = 1129
ITEM_BOW = 1008
ITEM_ARROW = 1009
ITEM_WHEAT = 1066
# The player menu: 0 result, 1-4 crafting grid, 5-8 armour, 9-35 main, 36-44
# hotbar, 45 offhand (`src/server/session.cpp`).
PM_ARMOR_CHEST = 6
PM_HOTBAR_FIRST = 36
CREATIVE_HOTBAR0 = 36
MENU_MERCHANT = 19
PLAYER_MENU_SIZE = 46

BOT_NAME = "TBot"
# An empty world has no floor; steps that need ground lay this layer and stand
# on top of it.
GROUND_Y = 3
STAND = (0.5, GROUND_Y + 2.0, 0.5)
# A poll bound, never a success delay: a healthy server answers a step in well
# under these, and the deadline only bounds a *failure*.
STEP_TIMEOUT = 20.0
PROGRESS = [0]


class GateBot(Bot):
    """`Bot` that keeps the packets its own `join()` consumed (Join Game, the
    spawn sync), which no later drain can see."""

    def __init__(self, name, port):
        self.join_log = []
        super().__init__(name, port)

    def recv(self):
        packet = super().recv()
        self.join_log.append(packet)
        return packet


def step(msg):
    PROGRESS[0] += 1
    print(f"[{PROGRESS[0]:2d}] {msg}", flush=True)


def has(packets, pid):
    return any(p == pid for p, _ in packets)


def bodies(packets, pid):
    return [b for p, b in packets if p == pid]


def unpack_pos(raw):
    return wire_client.unpack_block_pos(raw)


def block_updates(packets):
    return {unpack_pos(struct.unpack(">q", b[0:8])[0]): rd_varint(b, 8)[0]
            for b in bodies(packets, CB_BLOCK_UPDATE)}


def parse_boss_event(body):
    """One boss_event as {uuid, op, title?, progress?, color?, overlay?, flags?}.

    The UUID comes first, then the operation; each operation writes only its
    own fields (src/server/entity_packets.h).
    """
    uuid = body[0:16]
    op, i = rd_varint(body, 16)
    out = {"uuid": uuid, "op": op}
    if op in (0, 3):
        out["title"], i = rd_nbt_component(body, i)
    if op in (0, 2):
        out["progress"] = struct.unpack(">f", body[i:i + 4])[0]
        i += 4
    if op in (0, 4):
        out["color"], i = rd_varint(body, i)
        out["overlay"], i = rd_varint(body, i)
    if op in (0, 5):
        out["flags"] = body[i]
        i += 1
    assert i == len(body), f"boss_event op {op}: {len(body) - i} trailing byte(s)"
    return out


def spawns(packets):
    """Add Entity decoded as far as the entity type: [varint eid][uuid 16]
    [varint type]."""
    out = []
    for body in bodies(packets, CB_ADD_ENTITY):
        eid, i = rd_varint(body, 0)
        i += 16
        type_id, i = rd_varint(body, i)
        out.append((eid, type_id))
    return out


def spawn_pos(packets, etype):
    """The (eid, x, y, z) of the first Add Entity of `etype`: [varint eid]
    [uuid 16][varint type][f64 x y z]…"""
    for body in bodies(packets, CB_ADD_ENTITY):
        eid, i = rd_varint(body, 0)
        i += 16
        type_id, i = rd_varint(body, i)
        if type_id == etype:
            x, y, z = struct.unpack(">ddd", body[i:i + 24])
            return eid, x, y, z
    return None


def spawn_list(packets, etype):
    """Every Add Entity of `etype` as (eid, x, y, z)."""
    out = []
    for body in bodies(packets, CB_ADD_ENTITY):
        eid, i = rd_varint(body, 0)
        i += 16
        type_id, i = rd_varint(body, i)
        if type_id == etype:
            x, y, z = struct.unpack(">ddd", body[i:i + 24])
            out.append((eid, x, y, z))
    return out


def read_damage_event(body):
    """damage_event (0x19): [varint entity][varint type][varint cause]
    [varint direct][bool hasPosition][f64 x y z]."""
    eid, i = rd_varint(body, 0)
    type_id, i = rd_varint(body, i)
    cause, i = rd_varint(body, i)
    direct, i = rd_varint(body, i)
    return {"entity": eid, "type": type_id, "cause": cause, "direct": direct}


def read_entity_event(body):
    """entity_event (0x22): [i32 entity id][u8 event id]. The id is a *fixed*
    4-byte int here, the one packet in this family that is not varint-prefixed."""
    return struct.unpack(">i", body[0:4])[0], body[4]


def entity_events(packets):
    """Every entity_event as (entity id, event id)."""
    return [read_entity_event(b) for b in bodies(packets, CB_ENTITY_EVENT)]


def read_set_health(body):
    """set_health (0x68): [f32 health][varint food][f32 saturation]."""
    health = struct.unpack(">f", body[0:4])[0]
    food, i = rd_varint(body, 4)
    sat = struct.unpack(">f", body[i:i + 4])[0]
    return round(health, 4), food, round(sat, 4)


def read_position_sync(body):
    """entity_position_sync (0x23, 26.3): [varint eid][varint PositionPath
    type = LINEAR 0][f64 x y z][f32 yRot][f32 xRot][bool onGround]."""
    eid, pos = rd_varint(body, 0)
    path_type, pos = rd_varint(body, pos)
    assert path_type == 0, f"position_sync path type {path_type}"
    x, y, z = struct.unpack(">ddd", body[pos:pos + 24])
    return {"entity": eid, "x": x, "y": y, "z": z}


def command(bot, text, timeout=0.0):
    """Send a `/command` and return whatever `timeout` seconds of reply it
    produced. The default reads nothing: a step that polls afterwards must not
    have the reply swallowed here (a command's `add_entity`/effect packet lands
    in the very drain the poll is waiting for)."""
    assert text.startswith("/")
    bot.send(SB_CHAT_COMMAND, ct.varint(len(text) - 1) + text[1:].encode())
    return drain(bot, timeout) if timeout else []


def feedback_text(packets):
    return [chat_text(b) for b in bodies(packets, CB_SYSTEM_CHAT)]


def poll(bot, predicate, timeout=STEP_TIMEOUT, tick=False, initial=None):
    """Drain until `predicate(packets)` is truthy or the deadline passes.

    `tick` sends one `flying` per slice — the packet clock a real client drives
    — which is what advances a bow's draw and a survival tick. `initial` seeds
    the collected list, for a reply a command's own drain already read.

    The slice is 0.05 s so that clock runs at 20 Hz, which is the rate a real
    client's one-packet-per-tick gives it: the survival clock counts packets,
    exactly one tick each and no catch-up (`session.h`). At a 0.2 s slice the
    *test* was the bottleneck, not the server — an event thirty ticks away (a
    pair of cows walking together, an advancement flush) cost six wall seconds,
    while the world clock it was waiting on runs 20x compressed
    (`PLUSMC_TICK_SCALE`). A slice that comes back empty is not a failure — the
    deadline is what ends a poll — so a shorter one only makes the poll finer.
    """
    packets = list(initial) if initial else []
    deadline = time.time() + timeout
    while True:
        if tick:
            bot.send(SB_FLYING, bytes([1]))
        packets += drain(bot, 0.05)
        result = predicate(packets)
        if result or time.time() >= deadline:
            return result, packets


def select_hotbar(bot, hotbar_slot):
    bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", hotbar_slot))
    drain(bot, 0.3)


def slot_items(packets):
    """The player menu's stacks as {menu slot: item id} from either window
    packet: a one-slot change (`container_set_slot`) or the whole menu
    (`container_set_content`, which a multi-slot write like a bucket swap
    sends). An empty stack is 0."""
    out = {}
    for body in bodies(packets, CB_CONTAINER_SET_SLOT):
        _, i = rd_varint(body, 1)          # container id, then state id
        slot = struct.unpack(">h", body[i:i + 2])[0]
        count, i = rd_varint(body, i + 2)
        item, i = rd_varint(body, i)
        out[slot] = item if count else 0
    for body in bodies(packets, CB_CONTAINER_SET_CONTENT):
        window, _state, stacks, _carried, _components = decode_content(body)
        if window != 0:
            continue  # a container window's slots are not the player menu's
        for slot, (item, count) in enumerate(stacks):
            out[slot] = item if count else 0
    return out


def setblock(bot, x, y, z, state):
    """`/setblock` and wait for that exact cell's own block update; returns the
    state id that landed."""
    cell = (x, y, z)
    command(bot, f"/setblock {x} {y} {z} {state}", timeout=0.0)
    result, packets = poll(bot, lambda p: cell in block_updates(p))
    return block_updates(packets).get(cell), packets


def build_floor(bot, cells, y=GROUND_Y):
    for x, z in cells:
        command(bot, f"/setblock {x} {y} {z} minecraft:stone", timeout=0.0)
    wanted = {(x, y, z) for x, z in cells}
    poll(bot, lambda p: {c for c in block_updates(p) if c in wanted} == wanted)


def use_item_on(bot, x, y, z, face=1, seq=1):
    """ServerboundUseItemOn: [varint hand][i64 pos][varint face][3 f32 cursor]
    [bool inside][bool border][varint sequence]."""
    bot.send(SB_USE_ITEM_ON, ct.varint(0) + ct.position(x, y, z) + ct.varint(face)
             + struct.pack(">fff", 0.5, 0.5, 0.5) + bytes([0, 0]) + ct.varint(seq))


def use_item(bot, yaw=0.0, pitch=0.0):
    """ServerboundUseItem: [varint hand][varint sequence][f32 yRot][f32 xRot]."""
    bot.send(SB_USE_ITEM, ct.varint(0) + ct.varint(0) + struct.pack(">ff", yaw, pitch))


def release_use_item(bot):
    """RELEASE_USE_ITEM (action 5) on the player-action packet."""
    bot.send(SB_PLAYER_ACTION, ct.varint(ACTION_RELEASE_USE_ITEM) + struct.pack(">q", 0)
             + bytes([1]) + ct.varint(0))


def attack_until_damage(bot, eid):
    """Attack `eid` (charging the swing with the packet clock) until a
    damage_event names it, standing on the mob's own newest sync so the swing
    stays in reach — the same stand-off the per-feature tests take."""
    def got(packets):
        return any(read_damage_event(b)["entity"] == eid
                   for b in bodies(packets, CB_DAMAGE_EVENT))
    packets = []
    deadline = time.time() + STEP_TIMEOUT
    while time.time() < deadline:
        where = [p for p in (read_position_sync(b) for b in bodies(packets, CB_ENTITY_POSITION_SYNC))
                 if p["entity"] == eid]
        if where:
            bot.move_to(round(where[-1]["x"]), round(where[-1]["y"]), round(where[-1]["z"]))
        bot.send(SB_ATTACK, ct.varint(eid))
        for _ in range(14):
            bot.send(SB_FLYING, bytes([1]))
            packets += drain(bot, 0.05)
        packets += drain(bot, 0.2)
        if got(packets):
            return packets
    return packets


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-tier-b-")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "1200"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "6000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_TICK_SCALE"] = "20"
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"

    proc = None
    try:
        print(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", "127.0.0.1", "--port", "0", "--motd", "tier b gate",
             "--offline-mode", "--gamemode", "1", "--data", data_root,
             "--world", world], wait_listening, env=env)

        bot = GateBot(BOT_NAME, port)
        wire_client.record(bot, note="tier-b-gate: the whole Tier-B list in one session")
        bot.join()
        # The read side goes to the reader thread: every window below is a quiet
        # one (`drain`, imported from craft_test), so a poll slice ends when the
        # server has been quiet for 0.2 s instead of always burning its whole
        # window, and the thread answers the keep-alives while it waits.
        bot.start_reader()

        # --- 1. join/spawn ----------------------------------------------------
        assert has(bot.join_log, CB_JOIN_GAME), "a join must send Join Game"
        assert has(bot.join_log, CB_SYNC_POS), "a join must spawn the player"
        packets = []
        deadline = time.time() + 5.0
        while time.time() < deadline and find(packets, CB_CONTAINER_SET_CONTENT) is None:
            packets += drain(bot, 0.5)
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "a join must sync the inventory menu"
        window, _state, slots, _carried, _c = decode_content(content)
        assert window == 0 and len(slots) == PLAYER_MENU_SIZE, \
            f"the player menu is {PLAYER_MENU_SIZE} slots, got {len(slots)}"
        attrs = None
        deadline = time.time() + 5.0
        while time.time() < deadline and attrs is None:
            packets += drain(bot, 0.3)
            attrs = find(packets, CB_UPDATE_ATTRIBUTES)
        assert attrs is not None, "the join must send update_attributes"
        player_eid, _ = rd_varint(attrs, 0)
        bot.move_to(*STAND)
        drain(bot, 0.4)
        build_floor(bot, [(x, z) for x in range(-3, 4) for z in range(-3, 4)])
        step(f"join/spawn: menu {window}, {len(slots)} slots, entity {player_eid}, "
             f"floor laid")

        # --- 2. combat: a sword swing damages a mob ---------------------------
        give(bot, CREATIVE_HOTBAR0, ITEM_DIAMOND_SWORD, 1)
        select_hotbar(bot, 0)
        command(bot, "/gamemode survival")
        command(bot, "/summon cow 1")
        ok, packets = poll(bot, lambda p: any(t == ET_COW for _e, t in spawns(p)))
        cow = next((e for e, t in spawns(packets) if t == ET_COW), None)
        assert ok and cow is not None, (
            f"/summon cow must Add Entity a cow; feedback: {feedback_text(packets)}")
        drain(bot, 0.8)
        attacked = attack_until_damage(bot, cow)
        events = [read_damage_event(b) for b in bodies(attacked, CB_DAMAGE_EVENT)]
        assert any(e["entity"] == cow for e in events), \
            f"a sword swing must damage cow {cow}; events: {events}"
        step(f"combat: a sword swing landed a damage_event on cow {cow}")
        command(bot, "/gamemode creative")

        # --- 3. damage: the player's own health drops -------------------------
        command(bot, "/gamemode survival")
        command(bot, "/damage " + BOT_NAME + " 6")
        def hurt(packets):
            return any(read_set_health(b)[0] < 20.0 for b in bodies(packets, CB_SET_HEALTH))
        ok, _ = poll(bot, hurt)
        assert ok, "taking 6 damage must lower the player's set_health"
        step("damage: 6 damage to the player lowered set_health")

        # --- 4. armor: a chestplate equips ------------------------------------
        # A creative slot write is gated on `abilities.instabuild`, so the mode
        # has to be creative (step 3 left it in survival).
        command(bot, "/gamemode creative")
        give(bot, PM_ARMOR_CHEST, ITEM_DIAMOND_CHESTPLATE, 1)
        ok, packets = poll(bot, lambda p: (has(p, CB_SET_EQUIPMENT)
                           or has(p, CB_SET_ENTITY_DATA)
                           or has(p, CB_CONTAINER_SET_SLOT)
                           or has(p, CB_CONTAINER_SET_CONTENT)))
        assert ok, ("a chestplate in the armour slot must sync "
                    f"(set_equipment / entity_data / slot): "
                    f"{[hex(p) for p, _ in packets]}")
        step("armor: a diamond chestplate equipped into the armour slot")

        # --- 5. hunger: the hunger effect drains the food bar -----------------
        # Survival: a creative player's food does not drain, and the hunger
        # effect's exhaustion has nothing to spend.
        command(bot, "/gamemode survival")
        reply = command(bot, "/effect give " + BOT_NAME + " hunger 60 9", timeout=1.0)
        assert has(reply, CB_UPDATE_MOB_EFFECT), (
            f"/effect give hunger must apply: {feedback_text(reply)}")
        def hungry(packets):
            return any(read_set_health(b)[1] < 20 for b in bodies(packets, CB_SET_HEALTH))
        packets = list(reply)
        deadline = time.time() + 30.0
        drained = False
        while time.time() < deadline and not drained:
            for _ in range(5):
                bot.send(SB_FLYING, bytes([1]))
            packets += drain(bot, 0.1)
            drained = hungry(packets)
        assert drained, ("the hunger effect must drain the food bar; "
                         f"feedback: {feedback_text(reply)}")
        step("hunger: the hunger effect drained the food bar")

        # --- 6. status effects: give, then clear ------------------------------
        command(bot, "/effect clear " + BOT_NAME)
        drain(bot, 0.4)
        command(bot, "/effect give " + BOT_NAME + " minecraft:speed 20 1")
        ok, _ = poll(bot, lambda p: has(p, CB_UPDATE_MOB_EFFECT))
        assert ok, "an effect must send update_mob_effect"
        command(bot, "/effect clear " + BOT_NAME)
        ok, _ = poll(bot, lambda p: has(p, CB_REMOVE_MOB_EFFECT))
        assert ok, "clearing an effect must send remove_mob_effect"
        step("status effects: give -> update_mob_effect, clear -> remove_mob_effect")

        # --- 7. enchantments: /enchant writes the held item -------------------
        command(bot, "/gamemode creative")   # the creative write needs instabuild
        give(bot, CREATIVE_HOTBAR0, ITEM_DIAMOND_SWORD, 1)
        select_hotbar(bot, 0)
        first = command(bot, "/enchant " + BOT_NAME + " minecraft:sharpness 3", timeout=1.0)
        said = feedback_text(first)
        assert any("sharpness" in t.lower() for t in said), \
            f"/enchant must answer with its feedback: {said}"
        ok, _ = poll(bot, lambda p: has(p, CB_CONTAINER_SET_SLOT), initial=first)
        assert ok, "enchanting must re-sync the held slot"
        step("enchantments: /enchant sharpness 3 enchanted the held sword")

        # --- 8. redstone: a source lights a lamp ------------------------------
        lamp = (6, GROUND_Y, 0)
        dark, _ = setblock(bot, *lamp, "minecraft:redstone_lamp")
        assert dark is not None, "the lamp must land"
        setblock(bot, 5, GROUND_Y, 0, "minecraft:redstone_wire")
        command(bot, f"/setblock 4 {GROUND_Y} 0 minecraft:redstone_block", timeout=0.0)
        ok, packets = poll(bot, lambda p: lamp in block_updates(p)
                          and block_updates(p)[lamp] != dark)
        assert ok, ("a source down a wire must light the lamp; lamp updates: "
                    f"{block_updates(packets).get(lamp)} (dark {dark}); "
                    f"ids: {[hex(p) for p, _ in packets][-12:]}")
        step("redstone: a source down a wire lit the lamp")

        # --- 9. fluids: a water source spreads --------------------------------
        # A lone placed source does not schedule its own tick; a fluid written
        # *beside* it does (the neighbor pass reschedules the neighbour, exactly
        # as the fluid tests rely on). Two adjacent sources scatters water into
        # the cells below and beside them.
        #
        # The two sources go in a walled basin *on* the platform, one cell deep,
        # with exactly one open cell for them to reach. They used to go in the
        # open air beside the slab, and that slab is a 7x7 platform over a flat
        # world's void: two sources in mid-air fall ~60 blocks and pool on the
        # floor of the world. The run this was fixed against sent 2,682 block
        # updates in the step's own second and 1,000-8,700 in every second after
        # it — 45,416 cells in all, right through to shutdown — and steps 10+
        # then assert inside that torrent. It is why the piston failed here and
        # not on the host: its `/setblock` went out at t=10.2 s and the server
        # did not answer it until t=29.8 s, past the step's 20 s poll. The basin
        # still spreads into a neighbour; it just stops there.
        basin_y = GROUND_Y + 1
        for wall_x, wall_z in ((-3, 0), (-2, 1), (-2, -1), (-1, 1), (-1, -1), (1, 0),
                               (0, 1), (0, -1)):
            command(bot, f"/setblock {wall_x} {basin_y} {wall_z} minecraft:stone",
                    timeout=0.0)
        # The *first* source goes next to the basin's one open cell and the second
        # beside it: a write reschedules its neighbours, so it is the second
        # source that makes the first one tick. The other order puts the flow off
        # the first source's to-do list entirely and nothing spreads.
        reply = command(bot, f"/setblock -1 {basin_y} 0 minecraft:water", timeout=0.0)
        reply += command(bot, f"/setblock -2 {basin_y} 0 minecraft:water", timeout=1.0)
        # (0, basin_y, 0) is the only cell either source can reach: the floor is
        # under them and the walls close the other three sides.
        neighbours = {(0, basin_y, 0)}
        ok, packets = poll(bot, lambda p: neighbours & set(block_updates(p)), initial=reply)
        assert ok, ("placing water beside water must spread it into a neighbour; "
                    f"feedback: {feedback_text(reply)}; "
                    f"updates: {sorted(block_updates(packets))[:24]}")
        step("fluids: a water source spread to a neighbour cell")

        # --- 9b. buckets: pick a source up, put it back -----------------------
        # `BucketItem.use`, the fluid follow-up docs/PHASE4.md names. The
        # source is a lone one (a placed source does not schedule its own
        # tick), so it sits still until the bucket reaches it. Survival is
        # needed for the swap half: a creative player keeps the bucket they
        # used.
        bucket_floor = (4, GROUND_Y, 8)
        source_cell = (4, GROUND_Y + 1, 8)
        setblock(bot, *bucket_floor, "minecraft:stone")
        # A one-cell basin: a source placed in the open would spread off the
        # platform and rain block updates into every later step (the flood the
        # fluid step's own comment describes).
        for wall in ((3, GROUND_Y + 1, 8), (5, GROUND_Y + 1, 8),
                     (4, GROUND_Y + 1, 7), (4, GROUND_Y + 1, 9)):
            setblock(bot, *wall, "minecraft:stone")
        source_id, _ = setblock(bot, *source_cell, "minecraft:water")
        assert source_id not in (None, 0), "the bucket test needs a water source"
        bot.move_to(4.5, float(GROUND_Y + 1), 6.5)
        drain(bot, 0.3)
        give(bot, CREATIVE_HOTBAR0, ITEM_BUCKET, 1)
        select_hotbar(bot, 0)
        command(bot, "/gamemode survival", timeout=0.5)
        use_item_on(bot, *source_cell, seq=11)
        ok, picked = poll(bot, lambda p: (source_cell in block_updates(p)
                                          and block_updates(p)[source_cell] == 0
                                          and slot_items(p).get(CREATIVE_HOTBAR0) == ITEM_WATER_BUCKET))
        assert ok, ("an empty bucket on a source must empty the cell and fill the "
                    f"bucket: updates {block_updates(picked)}, slots {slot_items(picked)}")
        step("buckets: an empty bucket picked a water source up")
        use_item_on(bot, *bucket_floor, face=1, seq=12)
        ok, emptied = poll(bot, lambda p: (source_cell in block_updates(p)
                                           and block_updates(p)[source_cell] not in (None, 0)
                                           and slot_items(p).get(CREATIVE_HOTBAR0) == ITEM_BUCKET))
        assert ok, ("a water bucket on a block must place a source and empty itself: "
                    f"updates {block_updates(emptied)}, slots {slot_items(emptied)}")
        step("buckets: a water bucket placed its source and became empty")
        # `ItemUtils.createFilledResult`: a player with infinite materials keeps
        # the bucket they used, so the creative arm places the source and the
        # held stack does *not* move.
        command(bot, "/gamemode creative", timeout=0.2)
        command(bot, f"/setblock {source_cell[0]} {source_cell[1]} {source_cell[2]} minecraft:air",
                timeout=0.0)
        give(bot, CREATIVE_HOTBAR0, ITEM_WATER_BUCKET, 1)
        select_hotbar(bot, 0)
        use_item_on(bot, *bucket_floor, face=1, seq=13)
        ok, creative = poll(bot, lambda p: (source_cell in block_updates(p)
                                            and block_updates(p)[source_cell] not in (None, 0)))
        assert ok, f"the creative arm must still place a source: {block_updates(creative)}"
        assert slot_items(creative).get(CREATIVE_HOTBAR0) in (None, ITEM_WATER_BUCKET), \
            ("a creative player keeps the filled bucket: "
             f"slots {slot_items(creative)}")
        step("buckets: a creative water bucket placed and was kept")
        # `ItemUtils.createFilledResult` with a non-empty used stack: buckets
        # stack to sixteen, so a pickup consumes one bucket and *adds* the
        # filled one beside it instead of replacing the slot.
        command(bot, f"/setblock {source_cell[0]} {source_cell[1]} {source_cell[2]} minecraft:air",
                timeout=0.0)
        give(bot, CREATIVE_HOTBAR0, ITEM_BUCKET, 2)
        select_hotbar(bot, 0)
        command(bot, "/gamemode survival", timeout=0.3)
        setblock(bot, *source_cell, "minecraft:water")
        use_item_on(bot, *source_cell, seq=14)
        ok, stacked = poll(bot, lambda p: (source_cell in block_updates(p)
                                           and slot_items(p).get(CREATIVE_HOTBAR0) == ITEM_BUCKET
                                           and ITEM_WATER_BUCKET in slot_items(p).values()))
        assert ok, ("a stack of two buckets must keep one and add the filled one: "
                    f"slots {slot_items(stacked)}")
        step("buckets: a stacked bucket pick split instead of replacing")
        command(bot, "/gamemode creative", timeout=0.2)
        command(bot, f"/setblock {source_cell[0]} {source_cell[1]} {source_cell[2]} minecraft:air",
                timeout=0.0)

        # --- 10. pistons: power extends the piston ----------------------------
        piston = (2, GROUND_Y + 1, 5)
        base, _ = setblock(bot, *piston, "minecraft:piston")
        assert base is not None, "the piston must land"
        command(bot, f"/setblock 2 {GROUND_Y + 1} 4 minecraft:redstone_block", timeout=0.0)
        near = {(2, GROUND_Y + 2, 5), (2, GROUND_Y + 1, 5), (2, GROUND_Y, 5)}
        def extended(packets):
            posted = block_updates(packets)
            moved = posted.get(piston)
            return (moved is not None and moved != base) or bool(near & set(posted))
        ok, _ = poll(bot, extended)
        assert ok, "a powered piston must extend (the base state changes / a head appears)"
        step("pistons: a powered piston extended")

        # --- 11. TNT: flint and steel primes a TNT block ----------------------
        tnt = (7, GROUND_Y + 1, 0)
        setblock(bot, *tnt, "minecraft:tnt")
        give(bot, CREATIVE_HOTBAR0, ITEM_FLINT_AND_STEEL, 1)
        select_hotbar(bot, 0)
        # Interaction range: the flint and steel only reaches a block the player
        # is standing within ~4.5 of.
        bot.move_to(6.5, 4.0, 0.5)
        drain(bot, 0.4)
        use_item_on(bot, *tnt)
        ok, _ = poll(bot, lambda p: any(t == ET_PRIMED_TNT for _e, t in spawns(p)))
        assert ok, "flint and steel on a TNT block must spawn a primed TNT entity"
        step("TNT: flint and steel on a TNT block spawned a primed TNT")

        # --- 12. projectiles: a bow shot spawns an arrow ----------------------
        give(bot, CREATIVE_HOTBAR0, ITEM_BOW, 1)
        give(bot, PM_HOTBAR_FIRST + 1, ITEM_ARROW, 8)
        select_hotbar(bot, 0)
        use_item(bot)
        drain(bot, 0.25)
        for _ in range(20):
            bot.send(SB_FLYING, bytes([1]))
            drain(bot, 0.03)
        release_use_item(bot)
        ok, _ = poll(bot, lambda p: any(t == ET_ARROW for _e, t in spawns(p)))
        assert ok, "a drawn and released bow must spawn an arrow entity"
        step("projectiles: a bow shot spawned an arrow")

        # --- 13. mobs: hostile and passive both summon ------------------------
        # Back over the floor: an empty world has none anywhere else, and a mob
        # summoned over the void falls out of the world (which would make the
        # breeding step below unable to reach its cows).
        bot.move_to(*STAND)
        drain(bot, 0.4)
        command(bot, "/summon cow 1")
        ok, _ = poll(bot, lambda p: any(t == ET_COW for _e, t in spawns(p)))
        assert ok, "/summon cow must Add Entity a passive cow"
        command(bot, "/summon zombie 1")
        ok, _ = poll(bot, lambda p: any(t == ET_ZOMBIE for _e, t in spawns(p)))
        assert ok, "/summon zombie must Add Entity a hostile zombie"
        step("mobs: a passive cow and a hostile zombie were both summoned")

        # --- 13b. boss bars: the HUD entry Tier B's raid/boss basics needs ----
        # The bar is what this gate can assert: a boss entity adds a UUID-keyed
        # bar and its removal follows the entity's. The wither's shield and
        # skull volley are `wither-fight`'s subject; the dragon and raid
        # encounters are `dragon-fight` and `raid`, so this step stays the HUD.
        command(bot, "/summon minecraft:wither 1")
        ok, packets = poll(bot, lambda p: any(
            e["op"] == 0 and e["title"] == "Wither"
            for e in (parse_boss_event(b) for b in bodies(p, CB_BOSS_EVENT))))
        assert ok, (
            f"/summon wither must add its boss bar; feedback: {feedback_text(packets)}")
        wither = next(e for e in (parse_boss_event(b) for b in bodies(packets, CB_BOSS_EVENT))
                      if e["op"] == 0 and e["title"] == "Wither")
        # This session runs the world clock twentyfold (`PLUSMC_TICK_SCALE`),
        # so the 220-tick shield can finish inside the poll; the bar's *add* and
        # its colour are the gate's subject, and progress is a legal float.
        assert 0.0 <= wither["progress"] <= 1.0, f"the bar's progress is a fraction: {wither}"
        assert wither["color"] == 5, f"the wither bar is PURPLE: {wither}"
        command(bot, "/mobs kill")
        ok, packets = poll(bot, lambda p: any(
            e["op"] == 1 and e["uuid"] == wither["uuid"]
            for e in (parse_boss_event(b) for b in bodies(p, CB_BOSS_EVENT))))
        assert ok, (
            f"/mobs kill must remove the wither's bar; feedback: {feedback_text(packets)}")
        step("boss bars: /summon wither added the bar and /mobs kill removed it")

        # --- 14. breeding: two fed cows produce a baby ------------------------
        known = {e for e, t in spawns(bot.join_log)}  # (ids the join carried)
        command(bot, "/summon cow 2")
        ok, packets = poll(bot, lambda p: len(spawn_list(p, ET_COW)) >= 2)
        placed = spawn_list(packets, ET_COW)[:2]
        assert ok and len(placed) >= 2, (
            f"/summon cow 2 must Add Entity two cows: {placed}; "
            f"feedback: {feedback_text(packets)}")
        cows = [e for e, _x, _y, _z in placed]
        give(bot, CREATIVE_HOTBAR0, ITEM_WHEAT, 8)
        select_hotbar(bot, 0)
        # Feeding is a *reach* interaction (`mob_within_player_reach`: three
        # blocks plus the three-block lag buffer), and the world clock this test
        # runs on is compressed twentyfold (`PLUSMC_TICK_SCALE`) — so a cow
        # covers blocks between the Add Entity that named it and the click aimed
        # at it, and standing where a cow *spawned* can be refused as out of
        # reach. Each cow's own position syncs are therefore tracked and re-read
        # immediately before the click, with the player moved into the cow's own
        # cell.
        #
        # The clicks go in a cycle, not once each. `setInLove` starts a
        # 600-*world*-tick countdown, which at this clock is about 1.5 s of real
        # time, and `BreedGoal` only breeds while *both* parents are in love at
        # once — so a pair fed a beat apart silently loses its window. That is
        # this step's load-sensitive failure: the old code clicked each cow once
        # and then waited thirty seconds for the birth, and a stretched drain
        # was enough for the first cow's love to run out before the second was
        # fed. Nothing asserted the feeds had landed, so the run simply timed
        # out. A click on a cow that is already in love is refused and spends
        # nothing, so the cycle is free, and what it asserts is stronger than
        # the old "two clicks were sent": every cow has to answer its own heart
        # burst (`entity_event` 18, `Animal.setInLove`) and the child has to
        # appear.
        live = {e: (x, y, z) for e, x, y, z in placed}
        love = set()
        last_click = {e: 0.0 for e in cows}
        existing = set(cows) | known

        def baby_spawned(heard):
            return any(t == ET_COW and e not in existing for e, t in spawns(heard))

        packets = []
        deadline = time.time() + 30.0
        while time.time() < deadline and not baby_spawned(packets):
            for eid in cows:
                if time.time() - last_click[eid] < 0.4:
                    continue
                last_click[eid] = time.time()
                # A short drain first: while the pair walks, the server streams
                # each cow's position sync, so the newest one is the freshest
                # place that cow has told us it is.
                batch = drain(bot, 0.05)
                packets += batch
                for sync in (read_position_sync(b)
                             for b in bodies(batch, CB_ENTITY_POSITION_SYNC)):
                    if sync["entity"] in live:
                        live[sync["entity"]] = (sync["x"], sync["y"], sync["z"])
                x, y, z = live[eid]
                bot.move_to(x, y, z)
                bot.send(SB_INTERACT, ct.varint(eid) + ct.varint(0) + b"\x00" + bytes([0]))
                packets += drain(bot, 0.25)
                # A burst can land a batch later than the click that raised it,
                # so this reads everything heard since the step began.
                if (eid, ENTITY_EVENT_LOVE) in entity_events(packets):
                    love.add(eid)
        assert love == set(cows), (
            f"both cows must fall in love from the wheat, one heart burst "
            f"(entity_event {ENTITY_EVENT_LOVE}) each: bursts "
            f"{entity_events(packets)} name {sorted(love)} of {cows}, last "
            f"positions {live}, feedback: {feedback_text(packets)}")
        assert baby_spawned(packets), (
            f"two in-love cows must breed a baby cow: cows {cows}, last "
            f"positions {live}, spawns {spawns(packets)}, "
            f"bursts {entity_events(packets)}")
        step("breeding: two fed cows bred a baby")

        # --- 15. villagers/trading: a farmer opens the trade screen -----------
        bot.move_to(*STAND)
        drain(bot, 0.4)
        command(bot, "/summon villager profession=farmer")
        ok, packets = poll(bot, lambda p: any(t == ET_VILLAGER for _e, t in spawns(p)))
        villager = next((e for e, t in spawns(packets) if t == ET_VILLAGER), None)
        assert ok and villager is not None, (
            f"/summon villager must Add Entity a villager; feedback: {feedback_text(packets)}")
        # Stand where the villager spawned (the Add Entity carries its position,
        # and a villager that never moves sends no position sync to follow).
        at = spawn_pos(packets, ET_VILLAGER)
        if at is not None:
            bot.move_to(round(at[1]), at[2], round(at[3]))
            drain(bot, 0.3)
        deadline = time.time() + STEP_TIMEOUT
        screen = None
        heard = []
        while time.time() < deadline and screen is None:
            bot.send(SB_INTERACT, ct.varint(villager) + ct.varint(0) + b"\x00" + bytes([0]))
            heard += drain(bot, 0.3)
            screen = find(heard, CB_OPEN_SCREEN)
        assert screen is not None, "right-clicking a farmer must open the trade screen"
        win, menu_id, _title = parse_open_screen(screen)
        assert menu_id == MENU_MERCHANT, f"a trade screen is menu {MENU_MERCHANT}, got {menu_id}"
        # The offers ride in the same burst as the open screen, so check the
        # whole hearing and not just the packet after the last drain.
        ok, _ = poll(bot, lambda p: has(p, CB_MERCHANT_OFFERS), initial=heard)
        assert ok, "opening a trade screen must send merchant_offers"
        bot.send(SB_CLOSE_WINDOW, ct.varint(win))
        drain(bot, 0.5)
        step(f"villagers/trading: a farmer opened the merchant screen (menu {menu_id})")

        # --- 16. advancements: an inventory_changed trigger completes one -----
        # `story/mine_stone`'s criterion is the `#stone_tool_materials` tag, and
        # cobblestone is in it (plain stone is not) — the advancement test's own
        # note. So cobblestone is what completes it.
        reply = command(bot, "/give " + BOT_NAME + " minecraft:cobblestone 1", timeout=1.0)
        def advance(packets):
            return any(b"minecraft:story/mine_stone" in b
                       for b in bodies(packets, CB_UPDATE_ADVANCEMENTS))
        # The flush rides the survival tick, which is packet-driven here.
        ok, packets = poll(bot, advance, tick=True, initial=reply)
        assert ok, ("being given a stone must complete story/mine_stone; "
                    f"feedback: {feedback_text(reply)}; "
                    f"update_advancements: "
                    f"{[b[:40] for b in bodies(packets, CB_UPDATE_ADVANCEMENTS)]}")
        step("advancements: /give stone completed story/mine_stone")

        # --- 17. statistics: mining a block awards a stat ---------------------
        command(bot, "/gamemode survival")
        command(bot, f"/setblock 9 {GROUND_Y} 0 minecraft:stone", timeout=0.0)
        command(bot, f"/setblock 8 {GROUND_Y} 0 minecraft:stone", timeout=1.0)
        bot.move_to(8.5, GROUND_Y + 1.0, 0.5)   # stand within reach of (9, 3, 0)
        drain(bot, 0.4)
        bot.send(SB_PLAYER_ACTION, ct.varint(ACTION_STOP_DESTROY) + ct.position(9, GROUND_Y, 0)
                 + ct.varint(0) + ct.varint(1))
        broke, packets = poll(bot, lambda p: block_updates(p).get((9, GROUND_Y, 0)) == 0)
        assert broke, "the block must break (a write to air)"
        # The stats screen is answered to a REQUEST_STATS client command (the
        # scoreboard test's own path); mining itself only marks the counter dirty.
        bot.send(SB_CLIENT_COMMAND, ct.varint(CLIENT_COMMAND_REQUEST_STATS))
        ok, _ = poll(bot, lambda p: has(p, CB_AWARD_STATS))
        assert ok, "mining a block must award_stat (REQUEST_STATS must answer it)"
        step("statistics: mining a block awarded a statistic")

        # --- 18. scoreboards: an objective and a score ------------------------
        # `objectives add` alone is silent — an objective no display slot shows
        # is not tracked, so vanilla's onObjectiveAdded has nothing to send. The
        # tracking gate is `setdisplay`, which sends the objective and its scores
        # (the scoreboard test's own distinction).
        command(bot, "/scoreboard objectives add gate dummy Gate")
        reply = command(bot, "/scoreboard objectives setdisplay sidebar gate", timeout=1.0)
        ok, _ = poll(bot, lambda p: has(p, CB_SET_OBJECTIVE), initial=reply)
        assert ok, "setdisplay must start tracking and send set_objective"
        command(bot, f"/scoreboard players set {BOT_NAME} gate 5")
        ok, _ = poll(bot, lambda p: has(p, CB_SET_SCORE))
        assert ok, "a tracked objective's score must send set_score"
        step("scoreboards: setdisplay tracked the objective, and its score reached the wire")

        bot.close()
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:  # pragma: no cover
                proc.kill()
        subprocess.run(["rm", "-rf", world], check=False)
    print("\nPASS: the Tier-B gate — every item on the §3.1 Tier-B list this engine "
          "claims, in one session")
    return 0


if __name__ == "__main__":
    sys.exit(main())
