#!/usr/bin/env python3
"""Phase 3 milestone H — the Tier-A gate.

docs/PLAN.md §3.1 defines Tier A as the *playable core*, and it is the gate
Phase 3 is judged against:

    join/spawn, movement, chat, blocks place/break, day cycle, drops,
    inventories, containers, crafting, furnaces, XP/levels, death/respawn,
    commands

The rest of the suite proves each of those in depth, one feature at a time. What
this test adds is *composition*: one client, one world, one session, and the
whole list walked in order. A change that breaks a feature only when another has
run first — a window left open, a revision counter gone stale, a gamemode that
did not stick, an inventory resynced wrong — passes every per-feature test and
fails here.

It drives the real wire protocol through the shared client (`craft_test`'s Bot
and Menu, which track the container revision and the carried stack the way a
real client does), so an engine that only satisfies unit tests cannot pass.

Every step asserts the *wire* consequence of the feature, not a log line:
  * join/spawn      -> Set Player Position + the 46-slot inventory menu
  * movement        -> a later interaction is measured from where the client says
                       it is (the break below happens from the moved position)
  * chat            -> a SystemChat component that decodes as network NBT
  * commands        -> /gamemode feedback + the gamemode actually changing
  * day cycle       -> /time set day -> SetTime total 1000
  * inventories     -> a creative slot write echoes as container_set_slot
  * place/break     -> Block Update (block) then Block Update (air) + the ack
  * drops           -> a survival break spawns an item entity, and the bot that
                       broke the block *collects* it (take_item_entity + the
                       stack in its own menu)
  * containers      -> open_screen (generic_9x3) + content, a click moves a
                       stack in, close
  * crafting        -> a crafting table's grid derives 4 planks from one log
  * furnaces        -> a fed furnace reports its data slots and produces an ingot
  * XP/levels       -> /xp add -> Set Experience
  * death/respawn   -> /kill -> Player Combat Kill, then respawn -> Respawn
"""

import os, struct, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import craft_test as ct  # noqa: E402
import wire_client  # noqa: E402
from craft_test import (Bot, Menu, decode_content, drain, find, give, place,  # noqa: E402
                        parse_open_screen, rd_varint, wait_listening)
from nbt_component import chat_text  # noqa: E402

# --- packet ids (src/protocol/ids.h, all verified against the 26.2 codecs) ----
CB_ADD_ENTITY = 0x01
CB_BLOCK_UPDATE = 0x08
CB_ENTITY_POSITION_SYNC = 0x23
CB_CONTAINER_SET_CONTENT = 0x12
CB_CONTAINER_SET_DATA = 0x13
CB_CONTAINER_SET_SLOT = 0x14
CB_OPEN_SCREEN = 0x3C
CB_SET_TIME = 0x73
CB_SYNC_POS = 0x49
CB_PLAYER_COMBAT_KILL = 0x45
CB_RESPAWN = 0x54
CB_SET_EXPERIENCE = 0x69
CB_SET_HEALTH = 0x6A
CB_SYSTEM_CHAT = 0x7C
SB_CHAT = 0x09
SB_CHAT_COMMAND = 0x07
SB_CLIENT_COMMAND = 0x0C
SB_CLOSE_WINDOW = 0x13
SB_FLYING = 0x21
SB_PLAYER_ACTION = 0x29
SB_SET_CARRIED_ITEM = 0x36
CB_TAKE_ITEM_ENTITY = 0x7F
CB_UPDATE_ATTRIBUTES = 0x86
# minecraft:entity_type protocol id of a dropped item (src/protocol/ids.h).
ENTITY_ITEM = 72

# Menu layouts (MenuType registration order, docs/PHASE3.md).
MENU_GENERIC_9X3 = 2
MENU_CRAFTING = 12
MENU_FURNACE = 14
PLAYER_MENU_SIZE = 46
CHEST_MENU_SIZE = 63      # 27 container + 27 main + 9 hotbar
CRAFTING_MENU_SIZE = 46   # result + 3x3 grid + 27 main + 9 hotbar
FURNACE_MENU_SIZE = 39    # input, fuel, result + 27 main + 9 hotbar
CT_GRID_FIRST, CT_MAIN_FIRST, CT_HOTBAR_FIRST = 1, 10, 37
PM_RESULT = 0
F_SLOT_INPUT, F_SLOT_FUEL, F_SLOT_RESULT = 0, 1, 2
F_SLOT_HOTBAR_FIRST = 30
PICKUP, QUICK_MOVE = 0, 1

# Registry ids (generated item table, src/world/item_registry.h).
ITEM_STONE = 1
ITEM_OAK_PLANKS = 63
ITEM_COAL_BLOCK = 112
ITEM_OAK_LOG = 163
ITEM_CHEST = 404
ITEM_CRAFTING_TABLE = 405
ITEM_FURNACE = 407
ITEM_RAW_IRON = 1017
ITEM_IRON_INGOT = 1018

CREATIVE_HOTBAR0 = 36     # a creative slot write addresses the player *menu*
# The world this test serves is **empty** (a temp dir and no generator), so
# there is no surface to stand on: a block placed against (x, GROUND_Y, z)'s top
# face lands at GROUND_Y + 1, and that face — y = 4.0 — is the only ground a
# step gets unless it lays one itself. (The step 8 drop needs exactly that; see
# `build_floor`.)
GROUND_Y = 3
STAND = (0.5, GROUND_Y + 2.0, 0.5)
BOT_NAME = "GateBot"

# The world tick that reports an item's touch is the server's own 50 ms clock —
# `PLUSMC_SURVIVAL_CLOCK=packet` moves the *player*, not the world — so the same
# wall-clock wait buys fewer ticks on a loaded box. This bounds a *failure*, not
# a success: a drop resting in the bot's pickup area is reported on the next
# world tick, and the poll below resolves it against an inventory on the next
# packet, so a healthy server answers in well under a second.
PICKUP_TIMEOUT = 30.0
# The same bound for laying ground: a `/setblock` answers with its own block
# update about a tick later, so the wait is on the updates (see `build_floor`).
FLOOR_TIMEOUT = 30.0

CB_JOIN_GAME = 0x32

PROGRESS = [0]


class GateBot(Bot):
    """`Bot` that keeps the packets its own `join()` consumed.

    The join burst — the Join Game packet, the spawn position, the chunk batch —
    is read *sequentially* inside `Bot.join` (the reader thread only takes over
    afterwards), so those packets never reach a later drain. Recording them is
    the only way to assert on the spawn.
    """

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


def seq_bump(seq_holder):
    seq_holder[0] += 1
    return seq_holder[0]


def unpack_pos(raw):
    """BlockPos.asLong: x << 38 | z << 12 | y (a block update's position).

    Delegates to the shared decoder. The local one this step first had masked z
    and y without putting their sign back, so a cell at z = -1 came back as
    z = 67108863: the *packet* was right (the server's own `decode_block_pos`
    sign-extends all three), the reader was not."""
    return wire_client.unpack_block_pos(raw)


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


def read_take_item(body):
    """take_item_entity (0x7c): [varint item][varint player][varint amount]."""
    item, i = rd_varint(body, 0)
    player, i = rd_varint(body, i)
    amount, i = rd_varint(body, i)
    return item, player, amount


def read_position_sync(body):
    """entity_position_sync (0x23, 26.3): [varint eid][varint PositionPath
    type = LINEAR 0][f64 x y z][f32 yRot][f32 xRot][bool onGround]. The drop's
    own position, so a step that loses it can say *where* it went instead of
    listing packet ids."""
    eid, pos = rd_varint(body, 0)
    path_type, pos = rd_varint(body, pos)
    assert path_type == 0, f"position_sync path type {path_type}"
    x, y, z = struct.unpack(">ddd", body[pos:pos + 24])
    pos += 24 + 8
    return {"entity": eid, "x": x, "y": y, "z": z,
            "ground": bool(body[pos])}


def block_updates(packets):
    return {unpack_pos(struct.unpack(">q", b[0:8])[0]): rd_varint(b, 8)[0]
            for b in bodies(packets, CB_BLOCK_UPDATE)}


def rd_varlong(buf, pos=0):
    result, shift = 0, 0
    while True:
        b = buf[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            return result, pos
        shift += 7
        if shift > 63:
            raise ValueError("varlong too long")


def parse_set_time(body):
    """SetTime: [i64 gameTime][varint count]{[varint holder][varlong ticks]}."""
    gametime = struct.unpack(">q", body[0:8])[0]
    count, i = rd_varint(body, 8)
    holder, i = rd_varint(body, i)
    ticks, i = rd_varlong(body, i)
    return gametime, count, holder, ticks


def parse_health(body):
    """Set Health: [f32 health][varint food][f32 saturation]."""
    health = struct.unpack(">f", body[0:4])[0]
    food, i = rd_varint(body, 4)
    sat = struct.unpack(">f", body[i:i + 4])[0]
    return health, food, sat


def parse_experience(body):
    """Set Experience: [f32 progress][varint level][varint total]."""
    progress = struct.unpack(">f", body[0:4])[0]
    level, i = rd_varint(body, 4)
    total, i = rd_varint(body, i)
    return progress, level, total


def command(bot, text, timeout=1.5):
    """Send a `/command` (serverbound chat_command) and drain the reply."""
    assert text.startswith("/")
    bot.send(SB_CHAT_COMMAND, ct.varint(len(text) - 1) + text[1:].encode())
    return drain(bot, timeout)


def chat(bot, text, timeout=1.5):
    bot.send(SB_CHAT, ct.varint(len(text)) + text.encode())
    return drain(bot, timeout)


def break_block(bot, x, y, z, sequence):
    """STOP_DESTROY_BLOCK — finishes a dig. The server breaks instantly (there
    is no dig-time model), so one packet both starts and finishes."""
    bot.send(SB_PLAYER_ACTION, ct.varint(2) + ct.position(x, y, z) + ct.varint(0)
             + ct.varint(sequence))


def clear_hand(bot):
    """Empty the selected hotbar slot so a right-click opens instead of places."""
    give(bot, CREATIVE_HOTBAR0, 0, 0)
    drain(bot, 0.3)


def build_floor(bot, cells):
    """`/setblock` a patch of ground and wait for each cell's own block update.

    The burst goes out unverified (one drain for the lot, not one per cell) and
    the wait is for the updates themselves — never for a number of seconds, the
    wall-clock guess this helper exists to avoid.

    The patch spans negative x and z on purpose. Those columns are the wire
    check for the position decode: `unpack_pos` had no sign extension when this
    step was written, and read the z = -1 row back as z = 67108863 — reporting
    cells still air that the server had in fact set (the broadcast was correct
    all along; see `wire_client.unpack_block_pos`)."""
    for x, y, z in cells:
        command(bot, f"/setblock {x} {y} {z} stone", timeout=0.0)
    wanted = set(cells)
    placed = set()
    deadline = time.time() + FLOOR_TIMEOUT
    while placed != wanted and time.time() < deadline:
        for cell, state in block_updates(drain(bot, 0.5)).items():
            if cell in wanted and state != 0:
                placed.add(cell)
    missing = sorted(wanted - placed)
    assert not missing, f"the floor never landed: {missing} still air"


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-tier-a-")
    env = dict(os.environ)
    # The survival clock is packet-driven — one recognized packet is one tick,
    # with no wall-time catch-up — so keep keep-alives brisk and the day cycle
    # off (so /time's total is deterministic). TICK_SCALE makes the furnace's
    # 200-tick smelt finish inside the test. The gate walks Tier-A's features in
    # order and asserts each one's wire consequence, so the only survival-tick
    # steps in it (/kill's death and the respawn's restored bar) are the packets
    # it sends rather than however long its drains lasted.
    env["PLUSMC_KEEPALIVE_MS"] = "1200"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "4000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_TICK_SCALE"] = "20"
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"

    proc = None
    try:
        print(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", "127.0.0.1", "--port", "0", "--motd", "tier a gate",
             "--offline-mode", "--gamemode", "1", "--data", data_root,
             "--world", world], wait_listening, env=env)

        bot = GateBot(BOT_NAME, port)
        wire_client.record(bot, note="tier-a-gate: the whole Tier-A list in one session")
        bot.join()
        # The read side goes to the reader thread: every window below is a quiet
        # one (`drain`, imported from craft_test), and the thread answers the
        # keep-alives while a poll waits.
        bot.start_reader()
        seq = [0]

        # --- 1. join/spawn -----------------------------------------------------
        # Join Game proves the session reached the play state, and the spawn
        # position proves it was placed in the world. Both are in the packets
        # `join()` itself consumed (see GateBot), which no later drain can see.
        assert has(bot.join_log, CB_JOIN_GAME), \
            f"a join must send Join Game (0x{CB_JOIN_GAME:02x}): {[hex(p) for p, _ in bot.join_log]}"
        assert has(bot.join_log, CB_SYNC_POS), \
            f"a join must spawn the player (0x{CB_SYNC_POS:02x}): {[hex(p) for p, _ in bot.join_log]}"

        packets, deadline = [], time.time() + 5.0
        while time.time() < deadline and find(packets, CB_CONTAINER_SET_CONTENT) is None:
            packets += drain(bot, 0.5)
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "a join must sync the inventory menu"
        window, state, slots, carried, _components = decode_content(content)
        assert window == 0 and len(slots) == PLAYER_MENU_SIZE and carried == (0, 0), \
            f"the player menu (0) is {PLAYER_MENU_SIZE} slots, got window {window}, {len(slots)}"
        # The entity id this session was spawned with, from update_attributes
        # (0x86): the take below names it, the way a real client would see its
        # own id in a pickup animation.
        deadline = time.time() + 5.0
        while time.time() < deadline and find(packets, CB_UPDATE_ATTRIBUTES) is None:
            packets += drain(bot, 0.3)
        attrs = find(packets, CB_UPDATE_ATTRIBUTES)
        assert attrs is not None, "the join must send update_attributes"
        player_eid, _ = rd_varint(attrs, 0)
        step(f"join/spawn: position synced, player menu {window} with {len(slots)} slots, "
             f"entity {player_eid}")

        # --- 2. movement -------------------------------------------------------
        # The server measures block interaction range from where the client says
        # it is, so every interaction below is also a movement assertion: a
        # rejected move makes the next step fail with "no block update".
        bot.move_to(*STAND)
        drain(bot, 0.4)
        step(f"movement: teleported the client to {STAND}")

        # --- 3. chat -----------------------------------------------------------
        packets = chat(bot, "tier a gate")
        chats = [chat_text(b) for b in bodies(packets, CB_SYSTEM_CHAT)]
        assert f"<{BOT_NAME}> tier a gate" in chats, f"chat echo missing: {chats}"
        step("chat: the message was echoed as a decodable system_chat component")

        # --- 4. commands (/gamemode) -------------------------------------------
        packets = command(bot, "/gamemode survival")
        assert any("survival" in t.lower() for t in (chat_text(b) for b in bodies(packets, CB_SYSTEM_CHAT))), \
            "the command must answer with its feedback"
        assert has(packets, 0x27), "a gamemode change is a game event (0x27)"
        step("commands: /gamemode survival answered with feedback + the game event")

        # --- 5. day cycle (/time) ----------------------------------------------
        packets = command(bot, "/time set day")
        times = bodies(packets, CB_SET_TIME)
        assert times, f"/time set day must broadcast SetTime: {[hex(p) for p, _ in packets]}"
        gametime, count, holder, ticks = parse_set_time(times[0])
        assert holder == 0 and ticks == 1000, f"/time set day must be total 1000, got {ticks}"
        step(f"day cycle: /time set day -> SetTime holder {holder}, total {ticks}")

        command(bot, "/gamemode creative")

        # --- 6. inventories (/give -> slot sync) -------------------------------
        give(bot, CREATIVE_HOTBAR0, ITEM_OAK_PLANKS, 8)
        packets = drain(bot, 1.0)
        planks_synced = False
        for b in bodies(packets, CB_CONTAINER_SET_SLOT):
            _, i = rd_varint(b, 0)
            _, i = rd_varint(b, i)
            slot = struct.unpack(">h", b[i:i + 2])[0]
            stack, _comps, _ = ct.read_stack(b, i + 2)
            if slot == CREATIVE_HOTBAR0 and stack == (ITEM_OAK_PLANKS, 8):
                planks_synced = True
        assert planks_synced, \
            f"a creative slot write must echo to menu slot {CREATIVE_HOTBAR0}: {[hex(p) for p, _ in packets]}"
        step(f"inventories: 8 oak planks synced to menu slot {CREATIVE_HOTBAR0}")

        # --- 7. blocks: place --------------------------------------------------
        place(bot, 0, GROUND_Y, 1, 1, seq_bump(seq))
        packets = drain(bot, 1.5)
        updates = block_updates(packets)
        assert (0, GROUND_Y + 1, 1) in updates and updates[(0, GROUND_Y + 1, 1)] != 0, \
            f"placing must write a non-air block: {updates}"
        planks_state = updates[(0, GROUND_Y + 1, 1)]
        step(f"blocks: placed oak planks at (0, {GROUND_Y + 1}, 1) -> state {planks_state}")

        # --- 8. blocks: break + drops (survival) -------------------------------
        command(bot, "/gamemode survival")
        # The drop needs something under it before it is made. An empty world
        # has no floor here, and a drop with nothing to land on falls out of the
        # bot's pickup area within a handful of world ticks — and does not come
        # back, whatever a wait is set to. That is what made this step fail once
        # in three runs on an *idle* box; the wall clock could never have fixed
        # it. The patch is deliberately wider than the cell the drop spawns in:
        # the block-destroy launch throws the drop up to a block sideways (the
        # one measurement of it that matters is the syncs below — the drop rested
        # at x 0.62, 0.71 and 1.45 across three runs, from a spawn at 0.5), and a
        # five-by-five gives that drift a wide margin. A drop off the patch's
        # edge is a drop that falls out of the world again.
        build_floor(bot, [(fx, GROUND_Y, fz) for fx in range(-2, 3) for fz in range(-1, 4)])
        break_block(bot, 0, GROUND_Y + 1, 1, seq_bump(seq))
        packets = drain(bot, 2.0)
        updates = block_updates(packets)
        assert updates.get((0, GROUND_Y + 1, 1)) == 0, \
            f"breaking must write air: {updates}"
        dropped = [eid for eid, type_id in spawns(packets) if type_id == ENTITY_ITEM]
        assert dropped, \
            f"a survival break must drop an item entity: {[hex(p) for p, _ in packets]}"

        # ...and the drop must be *collectable*, not just spawned. That was the
        # hole a real client's acceptance run fell into: the drop never left the
        # ground, so it could never be picked up. The bot steps into the drop's
        # cell and drives the survival clock with the one `flying` packet per
        # tick a real client sends (this test's clock is packets); the world tick
        # reports the touch on its own clock, and the drain resolves it against
        # the inventory.
        drop_eid = dropped[0]
        bot.move_to(0.5, 4.0, 1.5)
        taken = None
        content_after_take = False

        def latest_take(batch):
            """The take this player already got in `batch`, or None. Scanned
            over everything received so far, not just the newest drain: the
            player may already stand in the drop's pickup area when it lands
            (the break's own 2 s wait is enough), and a take that arrived there
            is the same proof."""
            for b in bodies(batch, CB_TAKE_ITEM_ENTITY):
                item, player, amount = read_take_item(b)
                if player == player_eid:
                    return item, amount
            return None

        deadline = time.time() + PICKUP_TIMEOUT
        while time.time() < deadline and taken is None:
            bot.send(SB_FLYING, bytes([1]))
            more = drain(bot, 0.2)
            packets += more
            taken = latest_take(packets)
            if taken is not None:
                content_after_take = find(packets, CB_CONTAINER_SET_CONTENT) is not None
        where = [(s["entity"], round(s["x"], 2), round(s["y"], 2), round(s["z"], 2))
                 for s in (read_position_sync(b) for b in bodies(packets, CB_ENTITY_POSITION_SYNC))
                 if s["entity"] == drop_eid]
        assert taken is not None, \
            (f"standing in the drop must collect it: the drop {drop_eid} was last "
             f"seen at {where[-1] if where else 'nowhere'} ({len(where)} sync(s)); "
             f"packets: {[hex(p) for p, _ in packets[-24:]]}")
        assert taken == (drop_eid, 1), \
            f"the take must name the drop ({drop_eid}) and one item, got {taken}"
        # A drop that fits into a stack already in the inventory is synced as
        # content; either way the take itself is the pickup, and the dropped
        # entity is gone from the world's side of it.
        step(f"drops: the survival break dropped entity {drop_eid} and the bot collected it "
             f"(take_item_entity amount {taken[1]}"
             f"{', inventory resynced' if content_after_take else ''})")
        command(bot, "/gamemode creative")

        # --- 9. containers -----------------------------------------------------
        # Chest at (1, 4, 0); the planks stay in hotbar *1* so the selected hand
        # is empty and the right-click opens the chest instead of placing.
        give(bot, CREATIVE_HOTBAR0, ITEM_CHEST, 1)
        drain(bot, 0.4)
        place(bot, 1, GROUND_Y, 0, 1, seq_bump(seq))
        drain(bot, 1.0)
        give(bot, CT_HOTBAR_FIRST, ITEM_OAK_PLANKS, 8)   # menu slot 37 = hotbar 1
        drain(bot, 0.4)
        clear_hand(bot)

        menu = Menu(bot)
        place(bot, 1, GROUND_Y + 1, 0, 1, seq_bump(seq))
        packets = menu.pump(2.0)
        opened = find(packets, CB_OPEN_SCREEN)
        assert opened is not None, f"right-clicking a chest must open a screen: {[hex(p) for p, _ in packets]}"
        chest_window, chest_menu, _ = parse_open_screen(opened)
        assert chest_menu == MENU_GENERIC_9X3, f"a chest is generic_9x3 (2), got menu {chest_menu}"
        assert menu.slots is not None and len(menu.slots) == CHEST_MENU_SIZE, \
            f"a chest menu is {CHEST_MENU_SIZE} slots, got {len(menu.slots or [])}"
        # Move the planks in with a shift-click, the vanilla "fill the container"
        # gesture, then assert the container's own slot took them.
        planks_slot = next(i for i, s in enumerate(menu.slots) if s == (ITEM_OAK_PLANKS, 8))
        menu.click(planks_slot, 0, QUICK_MOVE)
        assert menu.slot(0) == (ITEM_OAK_PLANKS, 8), \
            f"a shift-click must move the stack into the chest, got {menu.slot(0)}"
        bot.send(SB_CLOSE_WINDOW, ct.varint(chest_window))
        packets = drain(bot, 0.8)
        assert not has(packets, CB_OPEN_SCREEN), "closing must not reopen the chest"
        step(f"containers: chest opened (menu {chest_menu}, window {chest_window}), "
             f"took the planks into slot 0, closed")

        # --- 10. crafting ------------------------------------------------------
        give(bot, CREATIVE_HOTBAR0, ITEM_CRAFTING_TABLE, 1)
        drain(bot, 0.4)
        place(bot, 2, GROUND_Y, 0, 1, seq_bump(seq))
        drain(bot, 1.0)
        give(bot, CREATIVE_HOTBAR0, 0, 0)          # empty hand: open, don't place
        give(bot, CT_HOTBAR_FIRST, ITEM_OAK_LOG, 4)
        drain(bot, 0.4)

        table = Menu(bot)
        place(bot, 2, GROUND_Y + 1, 0, 1, seq_bump(seq))
        packets = table.pump(2.0)
        opened = find(packets, CB_OPEN_SCREEN)
        assert opened is not None, f"right-clicking a table must open a screen: {[hex(p) for p, _ in packets]}"
        _, table_menu, _ = parse_open_screen(opened)
        assert table_menu == MENU_CRAFTING, f"a crafting table is menu 12, got {table_menu}"
        assert table.slots is not None and len(table.slots) == CRAFTING_MENU_SIZE, \
            f"a crafting menu is {CRAFTING_MENU_SIZE} slots, got {len(table.slots or [])}"
        # A shift-click on an ingredient fills the grid and derives the result.
        table.click(CT_HOTBAR_FIRST + 1, 0, QUICK_MOVE)   # hotbar 1 = the logs
        assert table.slot(PM_RESULT) == (ITEM_OAK_PLANKS, 4), \
            f"one log must derive 4 planks, got {table.slot(PM_RESULT)}"
        table.click(PM_RESULT, 0, PICKUP)
        assert table.cursor == (ITEM_OAK_PLANKS, 4), \
            f"taking the result must fill the cursor, got {table.cursor}"
        table.click(CT_MAIN_FIRST, 0, PICKUP)
        assert table.slot(CT_MAIN_FIRST) == (ITEM_OAK_PLANKS, 4), \
            f"the crafted planks must land in the inventory, got {table.slot(CT_MAIN_FIRST)}"
        bot.send(SB_CLOSE_WINDOW, ct.varint(table.window))
        drain(bot, 0.8)
        step("crafting: a shift-clicked log derived 4 planks, which were taken out")

        # --- 11. furnaces ------------------------------------------------------
        give(bot, CREATIVE_HOTBAR0, ITEM_FURNACE, 1)      # hotbar 0 (selected)
        give(bot, CT_HOTBAR_FIRST, ITEM_RAW_IRON, 3)      # hotbar 1 -> menu 37
        give(bot, CT_HOTBAR_FIRST + 1, ITEM_COAL_BLOCK, 1)  # hotbar 2 -> menu 38
        drain(bot, 0.4)
        place(bot, 3, GROUND_Y, 0, 1, seq_bump(seq))
        drain(bot, 1.0)
        clear_hand(bot)

        furnace = Menu(bot)
        place(bot, 3, GROUND_Y + 1, 0, 1, seq_bump(seq))
        packets = furnace.pump(2.0)
        opened = find(packets, CB_OPEN_SCREEN)
        assert opened is not None, f"right-clicking a furnace must open a screen: {[hex(p) for p, _ in packets]}"
        _, furnace_menu, _ = parse_open_screen(opened)
        assert furnace_menu == MENU_FURNACE, f"a furnace is menu 14, got {furnace_menu}"
        assert furnace.slots is not None and len(furnace.slots) == FURNACE_MENU_SIZE, \
            f"a furnace menu is {FURNACE_MENU_SIZE} slots, got {len(furnace.slots or [])}"
        data = bodies(packets, CB_CONTAINER_SET_DATA)
        assert len(data) == 4, f"a cooking window opens with 4 data slots, got {len(data)}"

        # Feed it with ordinary clicks: pick the ore up, put it in the input, and
        # same for the fuel. The result slot then has to become an iron ingot.
        furnace.click(F_SLOT_HOTBAR_FIRST + 1, 0, PICKUP)
        assert furnace.cursor == (ITEM_RAW_IRON, 3), f"cursor: {furnace.cursor}"
        furnace.click(F_SLOT_INPUT, 0, PICKUP)
        assert furnace.slot(F_SLOT_INPUT) == (ITEM_RAW_IRON, 3), \
            f"the ore must feed the input slot, got {furnace.slot(F_SLOT_INPUT)}"
        furnace.click(F_SLOT_HOTBAR_FIRST + 2, 0, PICKUP)
        assert furnace.cursor == (ITEM_COAL_BLOCK, 1), f"cursor: {furnace.cursor}"
        packets = furnace.click(F_SLOT_FUEL, 0, PICKUP)
        # Vanilla consumes the fuel item *as it lights* (`fuel.shrink(1)` in
        # serverTick) and keeps the burn time in the lit-time field, so the fuel
        # slot is empty and the flame is burning — asserting the slot still held
        # the coal would be asserting a divergence from vanilla.
        lit = {}
        for b in bodies(packets, CB_CONTAINER_SET_DATA):
            _, i = rd_varint(b, 0)
            ds, val = struct.unpack(">hh", b[i:i + 4])
            lit[ds] = val
        assert lit.get(0, 0) > 0, \
            f"the fuel must light the furnace: {lit}; {[hex(p) for p, _ in packets]}"
        assert has(packets, CB_BLOCK_UPDATE), "the lit furnace must be re-sent as a block update"

        # 200 cook ticks at TICK_SCALE 20 is 10 real ticks; give the loop room.
        deadline = time.time() + 8.0
        while time.time() < deadline and furnace.slot(F_SLOT_RESULT) != (ITEM_IRON_INGOT, 1):
            furnace.pump(0.5)
        assert furnace.slot(F_SLOT_RESULT) == (ITEM_IRON_INGOT, 1), \
            f"the furnace must smelt the ore, got {furnace.slot(F_SLOT_RESULT)}"
        bot.send(SB_CLOSE_WINDOW, ct.varint(furnace.window))
        drain(bot, 0.8)
        step("furnaces: a fed furnace reported its data slots and produced an iron ingot")

        # --- 12. XP/levels -----------------------------------------------------
        packets = command(bot, "/xp add @s 5")
        xp = bodies(packets, CB_SET_EXPERIENCE)
        assert xp, f"/xp must send Set Experience: {[hex(p) for p, _ in packets]}"
        progress, level, total = parse_experience(xp[-1])
        assert total >= 5, f"the bar must credit the points, got total {total}"
        step(f"XP/levels: /xp add @s 5 -> Set Experience level {level}, total {total}")

        # --- 13. death/respawn -------------------------------------------------
        packets = command(bot, "/kill", timeout=2.0)
        assert has(packets, CB_PLAYER_COMBAT_KILL), \
            f"a death must send the combat kill screen: {[hex(p) for p, _ in packets]}"
        health = [parse_health(b) for b in bodies(packets, CB_SET_HEALTH)]
        assert health and health[-1][0] == 0.0, f"the death must zero health: {health}"
        test_end = time.time() + 2.0
        while time.time() < test_end:
            drain(bot, 0.4)
        bot.send(SB_CLIENT_COMMAND, ct.varint(0))   # perform respawn
        packets = drain(bot, 2.0)
        assert has(packets, CB_RESPAWN), \
            f"the respawn click must send Respawn: {[hex(p) for p, _ in packets]}"
        assert has(packets, CB_CONTAINER_SET_CONTENT), "a respawn must resync the inventory"
        health = [parse_health(b) for b in bodies(packets, CB_SET_HEALTH)]
        assert health and health[-1][0] == 20.0, f"a respawn restores health: {health}"
        step("death/respawn: /kill -> combat kill + health 0, respawn -> Respawn + health 20")

        bot.close()
    finally:
        if proc is not None:
            proc.terminate()
            proc.wait(timeout=5)
    print("\nPASS: the Tier-A gate — every item on the §3.1 list, in one session")
    return 0


if __name__ == "__main__":
    sys.exit(main())
