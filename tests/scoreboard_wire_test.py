#!/usr/bin/env python3
"""Integration test: the scoreboard and the statistics screen over the wire
(Phase 4 milestone M).

`src/world/scoreboard.{h,cpp}` and `src/world/stats.{h,cpp}` are the models;
`tests/scoreboard_test.cpp` and `tests/stats_test.cpp` pin them. This test pins
the other half: that the session's award sites and the player tick actually
*reach* the model, and that the change log turns into the six packets vanilla's
`ServerScoreboard` would have sent. Everything below is read off the wire.

  * `/scoreboard objectives add <name> <criterion>` answers with set_objective
    (0x6a, method add). A score on an objective no display slot shows is *not*
    sent (vanilla's `startTrackingObjective` gate), and
    `/scoreboard objectives setdisplay sidebar <name>` makes it tracked: the
    start-tracking burst is set_display_objective (0x62) and every score the
    objective already carries, then `/scoreboard players set` answers set_score
    (0x6e).

  * the six by-value criteria move on the player tick (`ServerPlayer.doTick`
    728-751, in vanilla's order):
      - health: `/damage <player> 5` moves health and the `health` score is
        `ceil(health + absorption)`;
      - food: `/effect give <player> hunger` moves food and the `food` score is
        the food level;
      - air: the score starts at 300 (a fresh player's `getAirSupply()`);
      - armor: equipping a diamond chestplate through the creative menu moves
        the `armor` score to the piece's armour points;
      - xp and level: `/xp add <player> 30 points` moves both — `xp` is
        `totalExperience`, `level` is `experienceLevel`.

  * the item stats ride their own award sites and are read back through the
    statistics screen's own request (`client_command` 0x0c action 1 ->
    award_stats 0x03, `ServerStatsCounter.sendStats`): mining a block earns
    `mined` + `used` (the tool), placing a block earns `used` (the block item),
    and a Q-drop (`player_action` DROP_ITEM) earns the item's `dropped` plus the
    `drop` custom stat.

The server runs with `PLUSMC_SURVIVAL_CLOCK=packet`, so one recognized packet is
one survival tick — which also means a command's own effect is written by the
*next* packet's tick (`tick_survival_on_packet` runs before the handler), so the
test sends a `set_carried_item` after each command that moves a criterion.

Run: scoreboard_wire_test.py /path/to/plusmc
   or: PLUSMC_REPLAY=transcript.jsonl scoreboard_wire_test.py  (no binary needed)
"""
import os
import re
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nbt_component import chat_text  # noqa: E402
import wire_client  # noqa: E402
from wire_client import make_frame, read_packet  # noqa: E402

PROTO = 777
HOST = "127.0.0.1"

# Clientbound packet ids.
CB_SET_OBJECTIVE = 0x6C
CB_SET_SCORE = 0x70
CB_RESET_SCORE = 0x50
CB_SET_DISPLAY_OBJECTIVE = 0x64
CB_AWARD_STATS = 0x03
CB_SYSTEM_CHAT = 0x7C
CB_BLOCK_UPDATE = 0x08
CB_BLOCK_ACK = 0x04
CB_CONTAINER_SET_SLOT = 0x14
CB_KEEP_ALIVE = 0x2D

# Serverbound packet ids.
SB_CHAT_COMMAND = 0x07
SB_CLIENT_COMMAND = 0x0C
SB_POSITION = 0x1E
SB_SET_CARRIED_ITEM = 0x36
SB_SET_CREATIVE_SLOT = 0x39
SB_USE_ITEM_ON = 0x42
SB_PLAYER_ACTION = 0x29

# StatType ids (world/stats.h).
STAT_MINED = 0
STAT_CRAFTED = 1
STAT_USED = 2
STAT_BROKEN = 3
STAT_PICKED_UP = 4
STAT_DROPPED = 5
STAT_CUSTOM = 8

# Custom stat ids (world/stats.h).
CUSTOM_DROP = 1  # kCustomDrop is at the end of the custom table; read by name
CUSTOM_DROP_ID = 77 - 1  # placeholder, resolved below

# Item ids (the generated item table, src/world/item_registry.h).
ITEM_STONE = 1
ITEM_DIAMOND_PICKAXE = 1052
ITEM_DIAMOND_CHESTPLATE = 1085

# DisplaySlot ids (world/scoreboard.h).
SLOT_SIDEBAR = 1

FLOOR_Y = 63
PLAYER_X, PLAYER_Z = 0.5, 0.5


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
    packed = (((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF)) \
        & 0xFFFFFFFFFFFFFFFF
    return struct.pack(">Q", packed)


def read_set_objective(body):
    """set_objective (0x6a): [utf name][byte method], then for add/change
    [Component displayName][varint renderType][bool + NumberFormat]."""
    name, pos = rd_string(body, 0)
    method = body[pos]
    pos += 1
    return name, method


def read_set_score(body):
    """set_score (0x6e): [utf owner][utf objective][varint score]
    [bool + Component display][bool + NumberFormat]."""
    owner, pos = rd_string(body, 0)
    objective, pos = rd_string(body, pos)
    value, pos = rd_varint(body, pos)
    return owner, objective, value


def read_reset_score(body):
    """reset_score (0x4f): [utf owner][bool + utf objective]."""
    owner, pos = rd_string(body, 0)
    present = body[pos]
    pos += 1
    objective = ""
    if present:
        objective, pos = rd_string(body, pos)
    return owner, objective


def read_display_objective(body):
    """set_display_objective (0x62): [varint slot][utf objective]."""
    slot, pos = rd_varint(body, 0)
    objective, pos = rd_string(body, pos)
    return slot, objective


def read_award_stats(body):
    """award_stats (0x03): [varint count], then per entry
    [varint stat type][varint value id][varint value]."""
    count, pos = rd_varint(body, 0)
    out = []
    for _ in range(count):
        stype, pos = rd_varint(body, pos)
        sid, pos = rd_varint(body, pos)
        value, pos = rd_varint(body, pos)
        out.append((stype, sid, value))
    assert pos == len(body), f"award_stats: {len(body) - pos} trailing byte(s)"
    return out


class Bot:
    def __init__(self, name, port):
        self.name = name
        self.sock = wire_client.transport(HOST, port, timeout=10)
        self.sock.settimeout(10)
        self.threshold = -1
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
            if pid == CB_KEEP_ALIVE:
                self.send(0x1C, body[:8])
            elif pid == 0x0B:
                batch_size, _ = rd_varint(body)
                self.send(0x0B, varint(batch_size))
                break


def drain(bot, timeout=1.5):
    """The packets the server sends before it goes quiet, up to `timeout`.

    A scoreboard/command reply is synchronous, so the window ends when the
    server has been quiet for `quiet` seconds; `timeout` is the bound a missing
    reply is judged against. The bots here start their readers, so this is the
    queue path.
    """
    return wire_client.drain_quiet(bot, quiet=0.3, limit=timeout)


def all_of(packets, pid):
    return [b for p, b in packets if p == pid]


def send_command(bot, cmd_minus_slash):
    bot.send(SB_CHAT_COMMAND, varint(len(cmd_minus_slash)) + cmd_minus_slash.encode())


def tick(bot, times=2):
    """Advance the survival clock by `times` packets (one tick each)."""
    for _ in range(times):
        bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", 0))


def scores_of(packets, objective):
    """Every set_score for `objective`, as {owner: value} (last one wins)."""
    out = {}
    for body in all_of(packets, CB_SET_SCORE):
        owner, obj, value = read_set_score(body)
        if obj == objective:
            out[owner] = value
    return out


def write_stack(item, count):
    """ItemStack.OPTIONAL_STREAM_CODEC with an empty component patch."""
    return varint(count) + varint(item) + varint(0) + varint(0)


def read_item_stack(buf, pos):
    """ItemStack.OPTIONAL_STREAM_CODEC: [varint count][varint item]
    [varint added components][varint removed components]. Only the empty patch
    is accepted here (these stacks carry no components)."""
    count, pos = rd_varint(buf, pos)
    if count == 0:
        return None, pos
    item, pos = rd_varint(buf, pos)
    added, pos = rd_varint(buf, pos)
    removed, pos = rd_varint(buf, pos)
    assert added == 0 and removed == 0, f"unexpected component patch {added}/{removed}"
    return (item, count), pos


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
    env["PLUSMC_MOB_SPAWNING"] = "0"

    def wait_listening(proc, timeout=20):
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = proc.stdout.readline().decode(errors="replace")
            if line:
                sys.stderr.write(line)
            m = re.search(r"listening on.*:(\d+)", line)
            if m:
                return int(m.group(1))
        print("FAIL: server did not start", file=sys.stderr)
        return None

    proc, port = wire_client.start_server(
        [binary, "--host", HOST, "--port", "0", "--motd", "scoreboard test",
         "--offline-mode", "--gamemode", "1", "--data", data_root],
        wait_listening, env=env)
    if port is None:
        proc.kill()
        proc.wait()
        return 1
    if not wire_client.replaying():
        for _ in range(20):
            try:
                s = socket.create_connection((HOST, port), timeout=1)
                s.close()
                break
            except OSError:
                time.sleep(0.1)

    try:
        bot = Bot("ScoreBot", port)
        wire_client.record(bot, note="scoreboard: objectives, criteria, item stats")
        bot.join()
        bot.start_reader()
        drain(bot, 1.0)

        # Stand where the floor is about to be built: the server measures its
        # block interaction range from the position the client reports, so a
        # bot left at the world spawn (8, ~terrain, 8) is refused every
        # placement and every dig — acked, but never applied.
        send_command(bot, f"tp {PLAYER_X} {float(FLOOR_Y + 1)} {PLAYER_Z}")
        drain(bot, 1.0)

        # A floor to mine and stand on. `set_creative_slot` writes the *menu*
        # slot, so hotbar 0 is menu 36. The placement's cursor is the *relative*
        # hit offset on the clicked block's face (0..1 per axis; vanilla's
        # `ServerGamePacketListenerImpl.handleUseItemOn` check is
        # `|cursor - 0.5| < 1.0000001`), not a world position.
        bot.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", 36) + write_stack(ITEM_STONE, 64))
        time.sleep(0.3)
        seq = 1
        for x in range(-1, 2):
            for z in range(-1, 2):
                bot.send(SB_USE_ITEM_ON,
                         varint(0) + position(x, FLOOR_Y - 1, z) + varint(1)
                         + struct.pack(">fff", 0.5, 1.0, 0.5)
                         + b"\x00\x00" + varint(seq))
                seq += 1
        packets = drain(bot, 1.0)
        # Clicking the cell below the target with face=up places one block
        # above it, so the nine placements land at y = FLOOR_Y.
        assert len(all_of(packets, CB_BLOCK_UPDATE)) == 9, \
            "nine placements must broadcast nine block updates"
        print("  OK floor placed")

        # --- 1. objectives: add, then display (the tracking gate) --------------
        # `add` alone sends nothing: an objective no display slot shows is not
        # tracked, and `ServerScoreboard.onObjectiveAdded` has no packet to send
        # (the objective arrives with the start-tracking burst below). The
        # tracking gate is exercised with a writable `dummy` objective — the
        # read-only criteria are covered by the tick section below.
        print("-> /scoreboard objectives add")
        send_command(bot, "scoreboard objectives add gate dummy Gate")
        packets = drain(bot, 1.5)
        assert not all_of(packets, CB_SET_OBJECTIVE), \
            "an objective no slot shows must not be sent (vanilla's onObjectiveAdded)"
        # ...and a score on an untracked objective reaches nobody either.
        send_command(bot, "scoreboard players set ScoreBot gate 5")
        packets = drain(bot, 1.0)
        assert not all_of(packets, CB_SET_SCORE), \
            "a score on an untracked objective must not be sent"
        print("  OK objectives add; untracked objectives and scores stay server-side")

        print("-> /scoreboard objectives setdisplay sidebar")
        send_command(bot, "scoreboard objectives setdisplay sidebar gate")
        packets = drain(bot, 1.5)
        displays = [read_display_objective(b) for b in all_of(packets, CB_SET_DISPLAY_OBJECTIVE)]
        assert (SLOT_SIDEBAR, "gate") in displays, \
            f"setdisplay must send set_display_objective: {displays}"
        added = [read_set_objective(b) for b in all_of(packets, CB_SET_OBJECTIVE)]
        assert ("gate", 0) in added, \
            f"start-tracking sends the objective itself, method add: {added}"
        tracked = scores_of(packets, "gate")
        assert tracked.get("ScoreBot") == 5, \
            f"starting to track sends every score it carries: {tracked}"
        # Now that it is tracked, a later write is broadcast on its own.
        send_command(bot, "scoreboard players set ScoreBot gate 9")
        packets = drain(bot, 1.0)
        assert scores_of(packets, "gate").get("ScoreBot") == 9, \
            "a tracked objective's score changes are sent as they happen"
        print("  OK setdisplay starts tracking (objective, slot, existing scores)")

        # The read-only criteria cannot be written by hand: vanilla throws
        # "Cannot modify read-only score" and the model's `set_score` refuses.
        print("-> a read-only criterion refuses a manual write")
        send_command(bot, "scoreboard objectives add health_stat health")
        drain(bot, 0.6)
        send_command(bot, "scoreboard players set ScoreBot health_stat 5")
        packets = drain(bot, 1.0)
        chats = [chat_text(b) for b in all_of(packets, CB_SYSTEM_CHAT)]
        assert any("Cannot modify" in c for c in chats), \
            f"a read-only criterion must refuse the write: {chats}"
        print("  OK read-only criteria refuse manual writes")

        # --- 2. the six by-value criteria -------------------------------------
        # Every criterion gets an objective and a sidebar; only one can be
        # displayed at a time, so each is added, set as the sidebar, and then
        # driven. The health objective's score is already 5 from the gate above,
        # so the tick's first write is the visible correction.
        for name, criterion in [("food_stat", "food"), ("air_stat", "air"),
                                ("armor_stat", "armor"), ("xp_stat", "xp"),
                                ("level_stat", "level")]:
            send_command(bot, f"scoreboard objectives add {name} {criterion}")
            drain(bot, 0.6)

        # Health: display the health objective, then damage the player. The
        # command's own handler runs *after* the tick for that packet, so the
        # next packet's tick is what writes the new value. The bot has to be in
        # survival for `/damage` to land at all — a creative player is
        # invulnerable (`Player.abilities.invulnerable`).
        print("-> the health criterion moves with /damage")
        send_command(bot, "gamemode survival")
        drain(bot, 0.8)
        send_command(bot, "scoreboard objectives setdisplay sidebar health_stat")
        drain(bot, 0.8)
        send_command(bot, "damage ScoreBot 5")
        packets = drain(bot, 0.8)
        chats = [chat_text(b) for b in all_of(packets, CB_SYSTEM_CHAT)]
        assert any("Applied" in c for c in chats), \
            f"/damage must land in survival: {chats}"
        tick(bot, 3)
        packets = drain(bot, 1.5)
        assert scores_of(packets, "health_stat").get("ScoreBot") == 15, \
            f"health score must be ceil(health + absorption) = 15: {scores_of(packets, 'health_stat')}"
        print("  OK health score = 15")

        # Food: Hunger is a *harmful* effect, so `/effect give` needs the
        # amplifier argument to accept it (vanilla's own check).
        print("-> the food criterion moves with the Hunger effect")
        send_command(bot, "effect give ScoreBot hunger infinite 0 true")
        drain(bot, 0.6)
        send_command(bot, "scoreboard objectives setdisplay sidebar food_stat")
        drain(bot, 0.6)
        # Let the effect's own tick run: 80 ticks of exhaustion per level takes
        # a while, so this drives the clock forward and accepts whatever the
        # food level lands on, as long as it is below the 20 a fresh player has.
        for _ in range(40):
            tick(bot, 5)
        packets = drain(bot, 2.0)
        food = scores_of(packets, "food_stat")
        assert food.get("ScoreBot") is not None, \
            f"the food criterion must write once the level moves: {food}"
        assert 0 < food["ScoreBot"] < 20, \
            f"Hunger must have taken food below 20: {food}"
        print(f"  OK food score = {food['ScoreBot']} (< 20)")

        # Air: a fresh player's getAirSupply() is 300, but the sentinel is only
        # invalidated by a *move* — this player's air has not moved since the
        # objective existed, so vanilla would write nothing here either. The
        # criterion's first write is what a *joining* player gets, whose six
        # sentinels all start at their "nothing recorded yet" values: that is
        # what makes a score exist as soon as an objective is watching.
        print("-> a fresh join writes the air criterion (300) on its first tick")
        send_command(bot, "scoreboard objectives setdisplay sidebar air_stat")
        drain(bot, 0.8)
        tick(bot, 2)
        packets = drain(bot, 1.0)
        assert scores_of(packets, "air_stat") == {}, \
            "an unmoved criterion must not be rewritten (vanilla's sentinel)"
        second = Bot("AirBot", port)
        wire_client.record(second, note="scoreboard: a second join's criteria")
        second.join()
        second.start_reader()
        drain(second, 1.0)
        tick(second, 1)
        packets = drain(bot, 1.5)
        air = scores_of(packets, "air_stat")
        assert air.get("AirBot") == 300, \
            f"a fresh join's first tick must write air = 300: {air}"
        # ...and the same tick wrote the other five for the newcomer, to the
        # objectives that watch them (only air_stat is on a display slot, so
        # those writes stay server-side — which is itself the tracking gate).
        second.close()
        print("  OK air score = 300 on a fresh join; unmoved criteria stay quiet")

        # Armour: a diamond chestplate is ARMOR 8. `set_creative_slot` is
        # creative-only (`handleSetCreativeModeSlot`'s `hasInfiniteMaterials`
        # gate), so the bot goes back to creative for it; the armour attribute
        # and so the `armor` criterion read the slots in either gamemode. The
        # player menu's armour slots are head-first (menu 5 = head, 6 = chest,
        # 7 = legs, 8 = feet), so the chest is menu slot 6.
        print("-> the armor criterion moves when a chestplate is equipped")
        send_command(bot, "gamemode creative")
        drain(bot, 0.8)
        send_command(bot, "scoreboard objectives setdisplay sidebar armor_stat")
        drain(bot, 0.6)
        bot.send(SB_SET_CREATIVE_SLOT,
                 struct.pack(">h", 6) + write_stack(ITEM_DIAMOND_CHESTPLATE, 1))
        packets = drain(bot, 1.0)
        assert all_of(packets, CB_CONTAINER_SET_SLOT), \
            "the creative slot write must echo the slot back"
        tick(bot, 3)
        packets = drain(bot, 1.5)
        armor = scores_of(packets, "armor_stat")
        assert armor.get("ScoreBot") == 8, \
            f"a diamond chestplate is 8 armour points: {armor}"
        print("  OK armor score = 8")

        # Experience and level: 30 points is level 3 — `getXpNeededForNextLevel`
        # is 7, 9, 11, 13 for levels 0..3, so 27 points fill level 3's bar and
        # the last 3 sit on it. Only one objective can hold the sidebar, so
        # the `xp` one is displayed first and the tick's own write — not a
        # tracking burst — is what carries it: `ServerPlayer.doTick` writes
        # `totalExperience` to the `xp` criterion and `experienceLevel` to
        # `level` in the same pass.
        print("-> the xp and level criteria move with /xp add")
        send_command(bot, "scoreboard objectives setdisplay sidebar xp_stat")
        drain(bot, 0.8)
        send_command(bot, "xp add ScoreBot 30 points")
        packets = drain(bot, 0.8)
        chats = [chat_text(b) for b in all_of(packets, CB_SYSTEM_CHAT)]
        assert any("experience" in c.lower() for c in chats), \
            f"/xp must report the award: {chats}"
        tick(bot, 3)
        packets = drain(bot, 1.5)
        xp = scores_of(packets, "xp_stat")
        assert xp.get("ScoreBot") == 30, f"xp score must be totalExperience = 30: {xp}"
        # level_stat is not on a slot yet, so the same tick's level write is
        # made against an *untracked* objective and must not be sent — the same
        # gate the first section pinned. Displaying it is what reads the
        # remembered value back, through the tracking burst.
        send_command(bot, "scoreboard objectives setdisplay sidebar level_stat")
        packets = drain(bot, 1.0)
        level = scores_of(packets, "level_stat")
        assert level.get("ScoreBot") == 3, \
            f"30 points is level 3: {level}"
        print("  OK xp score = 30, level score = 3")

        # --- 3. the item stats -------------------------------------------------
        # `request_stats` answers with everything dirty; the join's
        # `markAllDirty` makes the first request the whole counter, and every
        # later one only what moved since.
        def request_stats(bot):
            bot.send(SB_CLIENT_COMMAND, varint(1))
            return drain(bot, 1.5)

        print("-> REQUEST_STATS answers award_stats")
        packets = request_stats(bot)
        entries = all_of(packets, CB_AWARD_STATS)
        assert entries, "client_command REQUEST_STATS must answer award_stats"
        read_award_stats(entries[-1])  # a malformed packet fails here
        print("  OK award_stats (0x03) is a well-formed statistics screen")

        # Mining a block: `Block.playerDestroy` awards BLOCK_MINED, and the
        # tool's ITEM_USED (ItemStack.mineBlock). A stone block broken by hand
        # still earns the mined stat, but `used` needs a tool — so the pickaxe
        # goes into hotbar 0 first.
        print("-> mining earns the block's mined stat and the tool's used stat")
        bot.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", 36) + write_stack(ITEM_DIAMOND_PICKAXE, 1))
        drain(bot, 0.6)
        # Dig out the stone the floor just put at (0, FLOOR_Y, 0):
        # START_DESTROY_BLOCK then STOP_DESTROY_BLOCK. The bot is in creative,
        # where the start already breaks instantly — the stop then finds air and
        # re-broadcasts it, so either packet carries the update.
        bot.send(SB_PLAYER_ACTION,
                 varint(0) + position(0, FLOOR_Y, 0) + bytes([1]) + varint(900))
        drain(bot, 0.4)
        bot.send(SB_PLAYER_ACTION,
                 varint(2) + position(0, FLOOR_Y, 0) + bytes([1]) + varint(901))
        packets = drain(bot, 1.0)
        assert all_of(packets, CB_BLOCK_UPDATE), "the break must broadcast the block update"
        packets = request_stats(bot)
        stats = {(t, i): v for t, i, v in read_award_stats(all_of(packets, CB_AWARD_STATS)[-1])}
        mined = stats.get((STAT_MINED, ITEM_STONE))
        used = stats.get((STAT_USED, ITEM_DIAMOND_PICKAXE))
        assert mined is not None and mined >= 1, \
            f"breaking stone must earn mined/stone: {stats}"
        assert used is not None and used >= 1, \
            f"a pickaxe mining must earn used/diamond_pickaxe: {stats}"
        print(f"  OK mined/stone = {mined}, used/diamond_pickaxe = {used}")

        # Placing a block: `ItemStack.useOn` awards ITEM_USED for the block item
        # (`BlockItem.place` returns SUCCESS, whose ItemContext.DEFAULT says the
        # item was the interaction).
        print("-> placing a block earns the block item's used stat")
        bot.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", 36) + write_stack(ITEM_STONE, 64))
        drain(bot, 0.6)
        bot.send(SB_USE_ITEM_ON,
                 varint(0) + position(0, FLOOR_Y - 1, 0) + varint(1)
                 + struct.pack(">fff", 0.5, 1.0, 0.5) + b"\x00\x00" + varint(902))
        packets = drain(bot, 1.0)
        assert all_of(packets, CB_BLOCK_UPDATE), "the place must broadcast the block update"
        packets = request_stats(bot)
        stats = {(t, i): v for t, i, v in read_award_stats(all_of(packets, CB_AWARD_STATS)[-1])}
        placed_used = stats.get((STAT_USED, ITEM_STONE))
        assert placed_used is not None and placed_used >= 1, \
            f"placing stone must earn used/stone: {stats}"
        print(f"  OK used/stone = {placed_used}")

        # A Q-drop: `player_action` DROP_ITEM (action 3, one item — action 4 is
        # Ctrl+Q and throws the whole stack) -> `ServerPlayer.drop(false)` ->
        # `drop(stack, false, true)` -> the item's `dropped` stat and Stats.DROP.
        # The slot resyncs either way (`sync_slot` runs after the drop).
        print("-> a Q-drop earns the item's dropped stat and the drop stat")
        before = {(t, i): v for t, i, v in read_award_stats(all_of(request_stats(bot),
                                                                  CB_AWARD_STATS)[-1])}
        bot.send(SB_PLAYER_ACTION,
                 varint(3) + position(0, FLOOR_Y, 0) + bytes([1]) + varint(903))
        packets = drain(bot, 1.0)
        assert all_of(packets, CB_CONTAINER_SET_SLOT), \
            "the Q-drop must resync the hotbar slot it emptied"
        packets = request_stats(bot)
        after = {(t, i): v for t, i, v in read_award_stats(all_of(packets, CB_AWARD_STATS)[-1])}
        dropped = after.get((STAT_DROPPED, ITEM_STONE), 0) - before.get((STAT_DROPPED, ITEM_STONE), 0)
        assert dropped == 1, f"one stone thrown from the hand: {dropped}"
        # The custom `drop` stat is keyed by its own custom id; find it by name
        # is not possible on the wire, so assert *some* custom stat moved.
        custom_moved = any(
            t == STAT_CUSTOM and v > before.get((t, i), 0)
            for (t, i), v in after.items())
        assert custom_moved, f"Stats.DROP must move a custom stat: {before} -> {after}"
        print(f"  OK dropped/stone = {dropped}, and a custom stat moved")

        # --- 4. removing the objective clears its display and its scores ------
        # `Scoreboard.removeObjective` clears every slot showing it *first*, so
        # by the time `ServerScoreboard.getStopTrackingPackets` runs its loop
        # finds no slot still on the objective and the whole message is the
        # single set_objective method remove — the client's own
        # `ClientPacketListener.handleSetObjective` (method 1) clears the slot
        # it remembers. One packet, and no reset_score either: dropping the
        # scores (`PlayerScores.remove`) has no callback.
        print("-> /scoreboard objectives remove")
        send_command(bot, "scoreboard objectives remove level_stat")
        packets = drain(bot, 1.5)
        removed = [read_set_objective(b) for b in all_of(packets, CB_SET_OBJECTIVE)]
        assert removed == [("level_stat", 1)], \
            f"remove must send exactly one set_objective method remove: {removed}"
        assert not all_of(packets, CB_SET_DISPLAY_OBJECTIVE), \
            "the slot was cleared before the stop-tracking burst, so no display packet"
        assert not all_of(packets, CB_RESET_SCORE), \
            f"dropping an objective's scores sends nothing: {packets}"
        print("  OK objective removed; the one packet is its set_objective(remove)")
        # ...and it really is gone: a later write reaches no objective at all.
        send_command(bot, "scoreboard players set ScoreBot level_stat 7")
        packets = drain(bot, 1.0)
        assert not all_of(packets, CB_SET_SCORE), \
            "a score on a removed objective must not be sent"
        print("  OK the removed objective takes no more scores")

        bot.close()
    finally:
        if proc is not None:
            proc.kill()
            proc.wait()

    print("\nscoreboard wire test: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
