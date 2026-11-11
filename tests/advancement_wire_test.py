#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Integration test: advancements over the real wire protocol (Phase 4
milestone N).

`src/world/advancement.{h,cpp}` is the registry, the progress model and the
visibility walk; `tests/advancement_test.cpp` pins them in isolation and
`tests/advancement_packets_test.cpp` pins the two packet builders byte for
byte. This test pins the half neither of them can: that the session's award
sites really reach the model, that the flush's decisions turn into
`update_advancements` a client can read, and that `seen_advancements` drives
`select_advancements_tab`. Everything below is decoded off the wire, and every
`update_advancements` is walked from its first byte to its last — a field-order
or length slip anywhere in the tree is a hard failure rather than a silent
misparse.

What the sequence pins:

  * nothing is earned at join, so nothing is sent. The file is *not* pushed at
    login the way the recipe book is: the client's tree only appears once
    something in it becomes visible, and the very first packet that has
    anything to say carries `reset`, which is what drops the client's stale
    copy (`PlayerAdvancements.flushDirty`'s `isFirstPacket`).

  * the TICK trigger fires once per player tick, and exactly one advancement in
    the 26.2 pack answers it: `recipes/decorations/crafting_table`, whose one
    requirement set is the OR of `has_the_recipe` and `unlock_right_away`, the
    latter being `minecraft:tick`. Completing it makes its display-less parent
    `recipes/root` visible too (a node is visible when a descendant is done),
    and nothing else — no story, no nether: a branch with nothing done in it
    reveals nothing. Both added nodes arrive with no DisplayInfo at all, which
    is the one shape the milestone-N packet builder has to get right.

  * `/give <player> <item>` is the inventory_changed trigger's wire path
    (`ItemStack` added to the player's inventory -> the trigger fires against
    the changed stack). A cobblestone is in `#minecraft:stone_tool_materials`,
    so `story/mine_stone` completes and reveals `story/root` with it. Both
    nodes' DisplayInfo is decoded field by field — title/description
    translation keys, the 26.2 icon stack template (item id *then* count, and
    an empty component patch), the frame ordinal, the background flag and the
    laid-out coordinates — and their progress carries the obtained criterion
    with a real epoch-millisecond Instant.

  * the same item again changes nothing (a grant is idempotent), and giving a
    crafting table completes the already-visible `story/root`: the flush then
    has `added` empty and carries *only* progress, which is the distinguishing
    case for the packet's three lists.

  * `husbandry/obtain_netherite_hoe` is a challenge reached through a
    display-less-free path (`husbandry/root` -> `plant_seed` ->
    `obtain_netherite_hoe`) whose unreached ancestors are revealed by the done
    descendant. Its `rewards.experience` of 100 is paid on the session's tick,
    and the `xp` scoreboard criterion — milestone M's own `totalExperience`
    observable — is what reads it back.

  * the screen's tab click (0x32, `advancement_tab` / 26.2 `seen_advancements`)
    with OPENED_TAB answers `select_advancements_tab`
    (0x55) only when the selection *changed*, only for a displayed root, and
    CLOSED_SCREEN clears it; a non-root or an unknown id selects nothing.

  * the `location` trigger needs a *world*, so it has its own server
    (`--worldgen 42`) and its own phase: `/kill` completes `adventure/root`,
    whose done SHOW rule reveals `adventure/adventuring_time` with its 55
    criteria; a `/tp` to one of the committed dump's own cells then makes the
    trigger complete that biome's criterion, on a progress-only flush. The
    target is picked against the flush that follows a *staged* teleport, so the
    criterion the world's spawn cell may have earned on the way cannot be
    mistaken for the target's, and the target's window asserts the exact delta.

  * the `enter_block` and `effects_changed` triggers ride the same worldgen
    server, because ENTER_BLOCK reads the block under the player and a void
    session has no world to read: `/setblock ~ ~1 ~ end_gateway` writes the one
    block no generated world ever carries into the player's own bounding box
    (the box is 1.8 tall, so the cell one above the feet is always one of its
    cells) and the flush adds exactly the six nodes that completion reveals —
    the node, its two ancestors, and the children and grandchildren its SHOW
    rule reaches; `/setblock ~ ~1 ~ water` then earns the ten `in_water`
    criteria the boat recipe advancements each carry — the ten advancements
    that could not be completed at all before this slice. The 17 effects
    `nether/all_potions` names are then given one `/effect give ... infinite` at
    a time, and the advancement only appears on the flush of the *last* one: the
    criterion is an "every effect named" test, so sixteen of them earn nothing.

The server runs with `PLUSMC_SURVIVAL_CLOCK=packet`, so one recognized packet
is one survival tick. A command's own effect is written by the *next* packet's
tick (`tick_survival_on_packet` runs before the handler), so every step below
sends a `set_carried_item` after the command that moves an advancement.

Run: advancement_wire_test.py /path/to/plusmc
   or: PLUSMC_REPLAY=transcript.jsonl advancement_wire_test.py  (no binary)
"""
import os
import re
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nbt_component import chat_tree, rd_nbt_translate  # noqa: E402
import wire_client  # noqa: E402
from wire_client import make_frame, read_packet  # noqa: E402

PROTO = 777
HOST = "127.0.0.1"

# Clientbound packet ids (src/protocol/packet_ids.generated.h).
CB_UPDATE_ADVANCEMENTS = 0x85
CB_SELECT_ADVANCEMENTS_TAB = 0x57
CB_SET_SCORE = 0x70
CB_SYSTEM_CHAT = 0x7C
CB_KEEP_ALIVE = 0x2D

# Serverbound packet ids. 0x32 is the dispatch table's `advancement_tab`
# (26.2's own name for it is `seen_advancements`; tests/protocol_ids_test.cpp
# pins the crosswalk).
SB_CHAT_COMMAND = 0x07
SB_CLIENT_COMMAND = 0x0C  # `client_command`; action 0 is "perform respawn"
SB_SET_CARRIED_ITEM = 0x36
SB_SEEN_ADVANCEMENTS = 0x33

# The generated world the location, enter-block and effects phases run in, and
# the vanilla dump the location phase's biomes are checked against
# (tools/gen_worldgen_dump.py's biome fill).
SEED = "42"

# The ten advancements `minecraft:enter_block` completes with `minecraft:water`:
# each boat recipe's own advancement carries that one `in_water` criterion (the
# table's eleven enter_block criteria are these ten and the end gateway's), so
# one tick in water earns all ten at once.
BOAT_ADVANCEMENTS = [
    "minecraft:recipes/transportation/acacia_boat",
    "minecraft:recipes/transportation/bamboo_raft",
    "minecraft:recipes/transportation/birch_boat",
    "minecraft:recipes/transportation/cherry_boat",
    "minecraft:recipes/transportation/dark_oak_boat",
    "minecraft:recipes/transportation/jungle_boat",
    "minecraft:recipes/transportation/mangrove_boat",
    "minecraft:recipes/transportation/oak_boat",
    "minecraft:recipes/transportation/pale_oak_boat",
    "minecraft:recipes/transportation/spruce_boat",
]

# The 17 effects `nether/all_potions`' one criterion names. The unit test derives
# them from the generated table (so a regeneration that changes the list fails
# there first); this list is what the wire phase *gives*, and the assertion that
# the seventeenth is the one that completes it is what keeps the two in step —
# a name the model does not expect simply never completes the advancement.
ALL_POTION_EFFECTS = [
    "minecraft:fire_resistance", "minecraft:infested", "minecraft:invisibility",
    "minecraft:jump_boost", "minecraft:night_vision", "minecraft:oozing",
    "minecraft:poison", "minecraft:regeneration", "minecraft:resistance",
    "minecraft:slow_falling", "minecraft:slowness", "minecraft:speed",
    "minecraft:strength", "minecraft:water_breathing", "minecraft:weakness",
    "minecraft:weaving", "minecraft:wind_charged",
]
FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures", "worldgen")

# `seen_advancements` actions (ServerboundSeenAdvancementsPacket.Action).
ACTION_OPENED_TAB = 0
ACTION_CLOSED_SCREEN = 1

# AdvancementType ordinals (server/advancement_packets.h).
FRAME_TASK = 0
FRAME_CHALLENGE = 1

# The icon items this test reads back (src/world/item_registry.h).
ITEM_GRASS_BLOCK = 54
ITEM_WOODEN_PICKAXE = 1027
ITEM_NETHERITE_HOE = 1059

CB_DISPLAY_BACKGROUND = 1
CB_DISPLAY_TOAST = 2
CB_DISPLAY_HIDDEN = 4


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


def rd_f32(buf, pos):
    return struct.unpack(">f", buf[pos:pos + 4])[0], pos + 4


def read_display(buf, pos):
    """DisplayInfo.serializeToNetwork: [Component title][Component description]
    [ItemStackTemplate icon: varint item, varint count, varint added, varint
    removed][varint frame][u32 flags][utf background when bit 0].

    26.3 moved x/y out of DisplayInfo into the packet's PositionedAdvancement
    wrapper; the icon is the *template* codec — item id before count, the
    reverse of the menu ItemStack — and always carries an empty patch.
    """
    title, _, pos = rd_nbt_translate(buf, pos)
    description, _, pos = rd_nbt_translate(buf, pos)
    icon, pos = rd_varint(buf, pos)
    icon_count, pos = rd_varint(buf, pos)
    added, pos = rd_varint(buf, pos)
    removed, pos = rd_varint(buf, pos)
    assert added == 0 and removed == 0, f"DisplayInfo icon patch {added}/{removed}"
    frame, pos = rd_varint(buf, pos)
    flags = struct.unpack(">I", buf[pos:pos + 4])[0]
    pos += 4
    background = None
    if flags & CB_DISPLAY_BACKGROUND:
        background, pos = rd_string(buf, pos)
    return {
        "title": title,
        "description": description,
        "icon": icon,
        "icon_count": icon_count,
        "frame": frame,
        "background": background,
        "show_toast": bool(flags & CB_DISPLAY_TOAST),
        "hidden": bool(flags & CB_DISPLAY_HIDDEN),
    }, pos


def read_advancement(buf, pos):
    """Advancement.write: [bool + utf parent][bool + DisplayInfo]
    [varint requirement sets]{varint count}{utf criterion}[bool telemetry].

    The criteria and the rewards are *not* on the wire — the client needs the
    tree's shape and the display, nothing else.
    """
    parent = None
    if buf[pos]:
        parent, pos = rd_string(buf, pos + 1)
    else:
        pos += 1
    display = None
    if buf[pos]:
        display, pos = read_display(buf, pos + 1)
    else:
        pos += 1
    set_count, pos = rd_varint(buf, pos)
    requirements = []
    for _ in range(set_count):
        size, pos = rd_varint(buf, pos)
        one = []
        for _ in range(size):
            name, pos = rd_string(buf, pos)
            one.append(name)
        requirements.append(one)
    telemetry = bool(buf[pos])
    pos += 1
    return {"parent": parent, "display": display, "requirements": requirements,
            "telemetry": telemetry}, pos


def read_progress(buf, pos):
    """`writeMap(criteria, writeUtf, CriterionProgress::serializeToNetwork)`:
    [varint count]{[utf name][bool obtained][i64 epoch ms when obtained]}."""
    count, pos = rd_varint(buf, pos)
    out = {}
    for _ in range(count):
        name, pos = rd_string(buf, pos)
        obtained = bool(buf[pos])
        pos += 1
        ms = None
        if obtained:
            ms = struct.unpack(">q", buf[pos:pos + 8])[0]
            pos += 8
        out[name] = (obtained, ms)
    return out, pos


def read_update_advancements(body):
    """update_advancements (0x85): [bool reset]
    [varint added]{[utf id][Advancement][f32 x][f32 y]}
    [varint removed]{[utf id]}
    [varint progress]{[utf id][AdvancementProgress]}[bool show].

    26.3's added entry is a PositionedAdvancement: the coordinates follow the
    AdvancementHolder instead of living in DisplayInfo. The decoder folds them
    back onto the display so callers can keep asking display["x"].

    The whole body must be consumed, so a field that moved or a length that is
    wrong fails here rather than leaving the rest of the test reading garbage.
    """
    reset = bool(body[0])
    pos = 1
    count, pos = rd_varint(body, pos)
    added = []
    for _ in range(count):
        adv_id, pos = rd_string(body, pos)
        advancement, pos = read_advancement(body, pos)
        x, pos = rd_f32(body, pos)
        y, pos = rd_f32(body, pos)
        advancement["x"], advancement["y"] = x, y
        if advancement["display"] is not None:
            advancement["display"]["x"], advancement["display"]["y"] = x, y
        added.append((adv_id, advancement))
    count, pos = rd_varint(body, pos)
    removed = []
    for _ in range(count):
        adv_id, pos = rd_string(body, pos)
        removed.append(adv_id)
    count, pos = rd_varint(body, pos)
    progress = []
    for _ in range(count):
        adv_id, pos = rd_string(body, pos)
        entry, pos = read_progress(body, pos)
        progress.append((adv_id, entry))
    show = bool(body[pos])
    pos += 1
    assert pos == len(body), (
        f"update_advancements: {len(body) - pos} trailing byte(s) of {len(body)}")
    return {"reset": reset, "added": added, "removed": removed,
            "progress": progress, "show": show}


def read_select_tab(body):
    """select_advancements_tab (0x55): [bool + utf tab]; absence = no tab."""
    if not body[0]:
        assert len(body) == 1, "a closed screen's tab packet carries no id"
        return None
    tab, pos = rd_string(body, 1)
    assert pos == len(body), "select_advancements_tab: trailing bytes"
    return tab


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
    # A reply window ends when the server has been quiet for `quiet` seconds
    # rather than always burning the whole window; `timeout` stays the bound a
    # missing reply is judged against. Requires the reader (started after each
    # join below).
    return wire_client.drain_quiet(bot, quiet=0.5, limit=timeout)


def drain_until(bot, pid, limit=40.0):
    """Packets until `pid` arrives, or `limit` seconds pass.

    A `--worldgen` join streams its whole view square first — thousands of
    chunk packets, tens of seconds on a loaded box — and only then gets to the
    queued play packets, so the tick that earns this flush can be far outside
    `drain`'s fixed window. The packet is still exactly one; only the wait for
    it has to outlast the join burst.
    """
    collected = []
    deadline = time.time() + limit
    while time.time() < deadline:
        packets = drain(bot, timeout=min(4.0, max(0.5, deadline - time.time())))
        collected.extend(packets)
        if any(p == pid for p, _ in packets):
            break
    return collected


def all_of(packets, pid):
    return [b for p, b in packets if p == pid]


def send_command(bot, cmd_minus_slash):
    bot.send(SB_CHAT_COMMAND, varint(len(cmd_minus_slash)) + cmd_minus_slash.encode())


def tick(bot, times=1):
    """Advance the survival clock by `times` packets (one tick each)."""
    for _ in range(times):
        bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", 0))


def scores_of(packets, objective):
    """Every set_score for `objective`, as {owner: value} (last one wins)."""
    out = {}
    for body in all_of(packets, CB_SET_SCORE):
        owner, pos = rd_string(body, 0)
        obj, pos = rd_string(body, pos)
        value, _ = rd_varint(body, pos)
        if obj == objective:
            out[owner] = value
    return out


def biome_dump(chunk_x, chunk_z):
    """`(quartX, quartY, quartZ) -> biome` from the committed vanilla dump of one
    chunk at seed 42 -- the same fixture `ctest -R worldgen-wire` compares the
    streamed chunk against, so it is the *game's* own answer. An empty table for
    a chunk the dump does not carry.
    """
    path = os.path.join(FIXTURES, f"biomes_seed42_c{chunk_x}_{chunk_z}.tsv")
    if not os.path.exists(path):
        return {}
    table = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            if not line or line.startswith("#"):
                continue
            fields = line.rstrip("\n").split("\t")
            table[(int(fields[3]), int(fields[4]), int(fields[5]))] = fields[12]
    return table


def named_adventure_biomes(data_root):
    """The biome keys the pack's `adventuring_time` criteria name."""
    import json
    import pathlib

    pack = None
    for version_json in sorted(pathlib.Path(data_root).glob("*/version.json")):
        try:
            data_version = json.loads(version_json.read_text()).get("world_version", -1)
        except (OSError, ValueError):
            continue
        if pack is None or data_version > pack[0]:
            pack = (data_version, version_json.parent)
    assert pack is not None, "no data/vanilla extraction present; run tools/vanilla-data"
    named = set()
    for criterion in json.loads((pack[1] / "data" / "minecraft" / "advancement" / "adventure" /
                                "adventuring_time.json").read_text())["criteria"].values():
        player = criterion["conditions"]["player"]
        entries = player if isinstance(player, list) else [player]
        for entry in entries:
            biomes = entry["predicate"]["minecraft:location"]["biomes"]
            named.update([biomes] if isinstance(biomes, str) else biomes)
    return named


def adventure_biome_cell(data_root, exclude=()):
    """`(biome, x, y, z)` for a block the generated world answers with a biome
    `adventuring_time` names and that is not in `exclude` — the biomes a flush
    has already shown as obtained, which the phase must not mistake for the cell
    it teleports to.

    The cell is a dump row, so the expected biome is the game's own answer, and
    the advancement's own criteria decide which cells are usable: a biome the
    pack does not name would complete nothing. The row is picked
    deterministically (lowest quart y, then z, then x) so the test cannot drift
    from one run to the next — and so a replay, whose only input is the recorded
    stream, picks the same cell the recorded run did.
    """
    excluded = set(exclude)
    named = named_adventure_biomes(data_root)
    best = None
    for (quart_x, quart_y, quart_z), biome in biome_dump(0, 0).items():
        y = quart_y * 4
        if biome not in named or biome in excluded or not 64 <= y <= 120:
            continue  # near the surface, where a player would stand
        candidate = (quart_y, quart_z, quart_x, biome)
        if best is None or candidate < best:
            best = candidate
    assert best is not None, (
        "the dump has no chunk-(0,0) cell with a named biome left "
        f"(excluded {sorted(excluded)})")
    quart_y, quart_z, quart_x, biome = best
    return biome, quart_x * 4 + 0.5, quart_y * 4, quart_z * 4 + 0.5


def flush_of(packets, what):
    """The single update_advancements in `packets`, decoded — with the count
    asserted, so a flush that sent twice (or not at all) is the failure."""
    bodies = all_of(packets, CB_UPDATE_ADVANCEMENTS)
    assert len(bodies) == 1, f"{what}: expected one update_advancements, got {len(bodies)}"
    return read_update_advancements(bodies[0])


def added_map(update):
    return dict(update["added"])


def progress_of(update, advancement_id):
    for adv_id, entry in update["progress"]:
        if adv_id == advancement_id:
            return entry
    return None


def obtained_of(updates, advancement_id, what):
    """`(entry, obtained)` for the *last* progress row of `advancement_id` across
    `updates`, `obtained` being the sorted criterion names that row has done.
    The last row is the newest one — each flush carries the whole row, not a
    diff — so it is the state the window ended in.
    """
    entry = None
    for update in updates:
        found = progress_of(update, advancement_id)
        if found is not None:
            entry = found
    assert entry is not None, (
        f"{what}: no progress row for {advancement_id} in "
        f"{[i for update in updates for i, _ in update['progress']]}")
    return entry, sorted(name for name, (done, _) in entry.items() if done)


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

    def wait_listening(proc):
        # Read the startup pipe until the bind line. No elapsed-time ceiling is
        # imposed here: full-suite load can delay generation before bind, while
        # EOF is the deterministic failure (the process exited). CTest's overall
        # timeout remains the guard against a genuinely hung process.
        while True:
            raw = proc.stdout.readline()
            if not raw:
                print("FAIL: server exited before listening", file=sys.stderr)
                return None
            line = raw.decode(errors="replace")
            sys.stderr.write(line)
            m = re.search(r"listening on.*:(\d+)", line)
            if m:
                return int(m.group(1))

    proc, port = wire_client.start_server(
        [binary, "--host", HOST, "--port", "0", "--motd", "advancement test",
         "--offline-mode", "--gamemode", "1", "--data", data_root],
        wait_listening, env=env)
    if port is None:
        proc.kill()
        proc.wait()
        return 1
    if not wire_client.replaying():
        while True:
            try:
                s = socket.create_connection((HOST, port), timeout=1)
                s.close()
                break
            except OSError:
                if proc.poll() is not None:
                    print("FAIL: server exited before accepting connections", file=sys.stderr)
                    return 1
                time.sleep(0.02)

    try:
        bot = Bot("AdvBot", port)
        wire_client.record(bot, note="advancements: triggers, flush, tab selection")
        bot.join()
        bot.start_reader()
        bot.start_reader()

        # --- 1. the first flush: the TICK trigger, and only what it completes -
        # The tree is earned into existence, not pushed at join: the only
        # advancement in the pack that answers `minecraft:tick` is the recipe
        # helper, and the first play packet's tick is what completes it. The
        # join's own last play packet ticks the clock too (the clock is
        # packet-driven), so take the first flush from wherever it lands.
        print("-> the first flush is a reset carrying the one TICK'ed advancement")
        packets = drain(bot, 1.5)
        updates = all_of(packets, CB_UPDATE_ADVANCEMENTS)
        if not updates:
            tick(bot, 1)
            packets = drain(bot, 1.5)
            updates = all_of(packets, CB_UPDATE_ADVANCEMENTS)
        assert len(updates) == 1, (
            f"the first flush is one packet, and nothing is earned before it: {len(updates)}")
        update = read_update_advancements(updates[0])
        assert update["reset"] and update["show"], (
            "the first packet that says anything is a reset, and asks for toasts")
        added = added_map(update)
        assert set(added) == {"minecraft:recipes/root",
                              "minecraft:recipes/decorations/crafting_table"}, (
            f"the TICK trigger completes exactly one advancement: {sorted(added)}")
        # Both are display-less helpers: the packet carries the tree shape and
        # no DisplayInfo at all, which is the shape the builder must get right.
        table = added["minecraft:recipes/decorations/crafting_table"]
        assert table["parent"] == "minecraft:recipes/root"
        assert table["display"] is None, "a recipe helper has no display"
        assert table["requirements"] == [["has_the_recipe", "unlock_right_away"]] or \
            [set(one) for one in table["requirements"]] == [
                {"has_the_recipe", "unlock_right_away"}], table["requirements"]
        assert table["telemetry"] is False
        assert added["minecraft:recipes/root"]["parent"] is None
        assert added["minecraft:recipes/root"]["display"] is None
        assert [set(one) for one in added["minecraft:recipes/root"]["requirements"]] == [
            {"impossible"}]
        entry = progress_of(update, "minecraft:recipes/decorations/crafting_table")
        assert entry is not None, "the completed helper's progress is sent"
        assert entry["unlock_right_away"][0] is True
        assert entry["unlock_right_away"][1] > 1_600_000_000_000, (
            "an obtained criterion carries its epoch-millisecond Instant")
        # ...and the completion pays its reward in the same breath: `award`
        # calls `AdvancementRewards.grant`, whose `recipes` half goes
        # `ServerPlayer.awardRecipesByKey` -> `ServerRecipeBook.addRecipes` ->
        # `CriteriaTriggers.RECIPE_UNLOCKED` for `minecraft:crafting_table` —
        # which is this node's *other* criterion, so both are obtained in the
        # one flush. (The client's own recipe book is not told: the reward's
        # unlock pushes no recipe_book_add, only the join's empty one.)
        assert entry["has_the_recipe"][0] is True, (
            f"the reward unlocks the recipe, and so this node's other criterion: {entry}")
        assert entry["has_the_recipe"][1] > 1_600_000_000_000
        assert progress_of(update, "minecraft:recipes/root") == {"impossible": (False, None)}
        # A branch with nothing done in it reveals nothing: the story and nether
        # roots are not sent just because they exist.
        assert not [i for i in added if i.startswith(("minecraft:story/", "minecraft:nether/"))], (
            f"only the recipes branch is done: {sorted(added)}")
        print("  OK reset, 2 display-less nodes, and no story/nether branch")

        # Neither completed node has a display, and vanilla has nothing to
        # announce without one (`holder.value().display().ifPresent(...)`): the
        # chat stays silent however many helpers the tick completes.
        announced = [c for c in (chat_tree(b) for p, b in packets if p == CB_SYSTEM_CHAT)
                     if str(c.get("translate", "")).startswith("chat.type.advancement.")]
        assert not announced, f"a display-less advancement is not announced: {announced}"

        print("-> a tick with nothing new sends no packet")
        tick(bot, 3)
        packets = drain(bot, 1.0)
        assert not all_of(packets, CB_UPDATE_ADVANCEMENTS), (
            "a grant is idempotent and an empty flush writes nothing")
        print("  OK an empty flush is silence")

        # --- 2. the inventory trigger reveals the story branch ----------------
        print("-> /give cobblestone completes story/mine_stone")
        send_command(bot, "give AdvBot cobblestone 1")
        tick(bot, 1)
        packets = drain(bot, 1.5)
        update = flush_of(packets, "the cobblestone's flush")
        assert not update["reset"], "only the first packet is a reset"
        added = added_map(update)
        assert {"minecraft:story/root", "minecraft:story/mine_stone"} <= set(added), (
            f"a completed node reveals itself and its ancestors: {sorted(added)}")
        # The same change completes every recipe advancement whose OR member
        # `has_cobblestone` fires — the cascade the requirements sets describe.
        # Their rewards then raise `has_the_recipe` on the same nodes (see the
        # crafting-table flush above), which is server-side only: the recipe
        # book the *client* sees is still the empty one the join sent.
        assert "minecraft:recipes/building_blocks/cobblestone_stairs" in added, (
            f"an inventory change unlocks the recipes that wanted it: {sorted(added)}")
        assert not ({"minecraft:recipes/root",
                     "minecraft:recipes/decorations/crafting_table"} & set(added)), (
            "an already-sent node is not added again: the flush is a diff")
        mine_stone = added["minecraft:story/mine_stone"]
        assert mine_stone["parent"] == "minecraft:story/root"
        display = mine_stone["display"]
        assert display is not None, "a displayed advancement carries its DisplayInfo"
        assert display["title"] == "advancements.story.mine_stone.title"
        assert display["description"] == "advancements.story.mine_stone.description"
        assert display["icon"] == ITEM_WOODEN_PICKAXE and display["icon_count"] == 1
        assert display["frame"] == FRAME_TASK
        assert display["background"] is None
        assert display["show_toast"] is True and display["hidden"] is False
        assert (display["x"], display["y"]) == (1.0, 1.75)
        assert [set(one) for one in mine_stone["requirements"]] == [{"get_stone"}]
        assert mine_stone["telemetry"] is True
        root = added["minecraft:story/root"]["display"]
        assert root["title"] == "advancements.story.root.title"
        assert root["icon"] == ITEM_GRASS_BLOCK
        assert root["background"] == "minecraft:gui/advancements/backgrounds/stone", (
            "the background flag is what the client paints the tab with")
        assert root["show_toast"] is False, "Vanilla's story root asks for no toast"
        entry = progress_of(update, "minecraft:story/mine_stone")
        assert entry is not None and entry["get_stone"][0] is True, (
            f"the changed node's progress is sent: {entry}")
        assert progress_of(update, "minecraft:story/root") == {"crafting_table": (False, None)}
        # ...and the completion is announced in chat, to *every* player
        # (`PlayerList.broadcastSystemMessage(message, false)`). The recipes that
        # also completed in this flush are display-less and stay silent, so the
        # one line here is mine_stone's: a task, its title in green brackets.
        announced = [c for c in (chat_tree(b) for p, b in packets if p == CB_SYSTEM_CHAT)
                     if str(c.get("translate", "")).startswith("chat.type.advancement.")]
        assert len(announced) == 1, f"exactly one completion is announced: {announced}"
        message = announced[0]
        assert message["translate"] == "chat.type.advancement.task", message
        who, name = message["with"]
        assert who == {"text": "AdvBot"}, who
        assert name["text"] == "[" and name["color"] == "green", name
        assert name["extra"][0] == {"translate": "advancements.story.mine_stone.title"}, name
        assert name["extra"][1] == {"text": "]"}, name
        print("  OK story/root + story/mine_stone, decoded field by field")
        print("  OK the completion was announced: chat.type.advancement.task"
              " with the title bracketed in green")

        # --- 3. an already-visible node's completion is progress only ---------
        print("-> /give crafting_table completes the already-visible root")
        send_command(bot, "give AdvBot crafting_table 1")
        tick(bot, 1)
        packets = drain(bot, 1.5)
        update = flush_of(packets, "the crafting table's flush")
        assert update["added"] == [], (
            f"nothing new becomes visible: {[i for i, _ in update['added']]}")
        entry = progress_of(update, "minecraft:story/root")
        assert entry is not None and entry["crafting_table"][0] is True, (
            f"a completed root is a progress-only packet: {update['progress']}")
        print("  OK progress-only flush (added empty, progress carries the root)")

        # --- 4. a challenge behind an unreached ancestor, and its reward ------
        # The `xp` criterion is milestone M's `totalExperience` observable: the
        # reward is paid on the session's tick, so the score moves a tick later.
        print("-> the xp objective reads the reward back")
        send_command(bot, "scoreboard objectives add xpt xp")
        drain(bot, 0.6)
        send_command(bot, "scoreboard objectives setdisplay sidebar xpt")
        drain(bot, 0.8)
        tick(bot, 2)
        packets = drain(bot, 1.0)
        # The criterion is written when its value moves; a player who has never
        # earned a point has no score to send yet, so an absent one is zero.
        before = scores_of(packets, "xpt").get("AdvBot", 0)
        print(f"  OK xp before = {before}")

        print("-> /give netherite_hoe completes husbandry/obtain_netherite_hoe")
        send_command(bot, "give AdvBot netherite_hoe 1")
        tick(bot, 1)
        packets = drain(bot, 1.5)
        update = flush_of(packets, "the netherite hoe's flush")
        added = added_map(update)
        assert "minecraft:husbandry/obtain_netherite_hoe" in added, (
            f"the challenge itself is revealed: {sorted(added)}")
        assert set(added) == {"minecraft:husbandry/root", "minecraft:husbandry/plant_seed",
                             "minecraft:husbandry/obtain_netherite_hoe"}, (
            f"the done node reveals its ancestors and nothing beside them: {sorted(added)}")
        hoe = added["minecraft:husbandry/obtain_netherite_hoe"]
        assert hoe["parent"] == "minecraft:husbandry/plant_seed"
        assert hoe["display"]["frame"] == FRAME_CHALLENGE, "Serious Dedication is a challenge"
        assert hoe["display"]["icon"] == ITEM_NETHERITE_HOE
        assert progress_of(update, "minecraft:husbandry/root") is not None
        tick(bot, 2)
        packets = drain(bot, 1.5)
        after = scores_of(packets, "xpt").get("AdvBot")
        assert after == before + 100, (
            f"rewards.experience 100 must reach totalExperience: {before} -> {after}")
        print(f"  OK xp score {before} -> {after} (+100)")

        # --- 5. seen_advancements -> select_advancements_tab ------------------
        def seen_tab(tab_id, action=ACTION_OPENED_TAB):
            body = varint(action)
            if tab_id is not None:
                body += varint(len(tab_id)) + tab_id.encode()
            bot.send(SB_SEEN_ADVANCEMENTS, body)
            return [read_select_tab(b) for b in all_of(drain(bot, 0.8),
                                                      CB_SELECT_ADVANCEMENTS_TAB)]

        print("-> OPENED_TAB on a displayed root answers with that tab")
        assert seen_tab("minecraft:story/root") == ["minecraft:story/root"]
        print("  OK select_advancements_tab = minecraft:story/root")

        print("-> the same tab again changes nothing")
        assert seen_tab("minecraft:story/root") == [], (
            "vanilla sends the selection only when it changed")
        print("  OK no packet for an unchanged selection")

        print("-> a non-root selects nothing")
        assert seen_tab("minecraft:story/mine_stone") == [None], (
            "only a root with a display is a tab")
        print("  OK the non-root cleared the selection")

        print("-> an unknown id selects nothing either")
        assert seen_tab("minecraft:story/root") == ["minecraft:story/root"]
        assert seen_tab("minecraft:nope/root") == [None]
        print("  OK an unknown id is not a tab")

        print("-> CLOSED_SCREEN clears the selection")
        assert seen_tab("minecraft:story/root") == ["minecraft:story/root"]
        assert seen_tab(None, ACTION_CLOSED_SCREEN) == [None]
        print("  OK closing the screen sends the absent form")
        assert seen_tab(None, ACTION_CLOSED_SCREEN) == [], (
            "closing an already-closed screen is not a change")

        bot.close()

        # --- 6. the location trigger, in a world that has biomes -------------
        # Nothing above exercises `minecraft:location`: a void session has no
        # world, so `advancement_location` hands the model an empty biome and
        # no criterion can match. This phase restarts the binary over a
        # `--worldgen` world, where the biome under the player comes from the
        # generator's own biome stage (the stage `ctest -R worldgen-wire`
        # compares to the same dumps this test reads).
        print("-> restarting over --worldgen: a location predicate needs a biome")
        if proc is not None:
            proc.kill()
            proc.wait()
            proc = None
        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--motd", "advancement location",
             "--offline-mode", "--gamemode", "1", "--data", data_root,
             "--worldgen", SEED],
            wait_listening, env=env)
        if port is None:
            proc.kill()
            proc.wait()
            return 1

        loc = Bot("LocBot", port)
        wire_client.record(loc, note="advancements: the location trigger's biome")
        loc.join()
        loc.start_reader()
        # The join's own tick earns the recipe helper, so its flush (a reset) is
        # taken first — the phases below assert on one flush per window.
        tick(loc, 2)
        join_flush = flush_of(drain_until(loc, CB_UPDATE_ADVANCEMENTS),
                              "the worldgen join's flush")
        assert join_flush["reset"] and set(added_map(join_flush)) == {
            "minecraft:recipes/root",
            "minecraft:recipes/decorations/crafting_table"}, sorted(added_map(join_flush))

        # The trigger's completion is only *sent* for a node the tree considers
        # visible (`PlayerAdvancements.flushDirty` writes progress for
        # `this.visible`), and `adventure/adventuring_time` hangs off
        # `adventure/root`: killing the player is the cheap way to complete that
        # root (its `killed_by_something` is `entity_killed_player`). The death
        # is a corpse's tick — no further packet is recognized until the respawn
        # click — so the root's completion rides the respawn's own tick, and its
        # own SHOW rule is what reveals the branch: three deep, so the challenge
        # arrives as an *added* node carrying its whole row, one criterion per
        # biome.
        print("-> /kill completes adventure/root, which reveals the challenge")
        send_command(loc, "kill LocBot")
        tick(loc, 3)
        drain(loc, 1.5)
        loc.send(SB_CLIENT_COMMAND, varint(0))
        tick(loc, 2)
        reveal = flush_of(drain_until(loc, CB_UPDATE_ADVANCEMENTS, limit=15.0),
                          "the respawn's flush")
        assert not reveal["reset"], "the respawn's flush is not the connection's first"
        revealed = added_map(reveal)
        assert "minecraft:adventure/root" in revealed, sorted(revealed)
        assert "minecraft:adventure/adventuring_time" in revealed, (
            f"a done parent reveals its children: {sorted(revealed)}")
        entry, _ = obtained_of([reveal], "minecraft:adventure/adventuring_time",
                               "the respawn's flush")
        assert len(entry) == 55, (
            f"the whole challenge rides the reveal, one criterion per biome: {len(entry)}")

        # Stand in a biome the pack names, at the *game's own* answer for that
        # cell: the position is a dump row, and the advancement's own criteria
        # say which rows are usable. The cell the player *starts* on is not this
        # phase's business — the world's spawn is the server's own choice, not a
        # dump row — and it can be worth a criterion, because the trigger tick
        # runs before the packet's handler does: the tick a teleport itself
        # rides still reads the pre-teleport position. That cell is *staged*
        # instead: the teleport below goes to the dump's first row, and the
        # window is waited out to a flush, so the flush's own obtained set is
        # what the target row is picked against — and the delta across the
        # second teleport can be asserted exactly.
        staging, x, y, z = adventure_biome_cell(data_root)
        print(f"-> /tp into the dump's first named cell, {staging} at ({x}, {y}, {z})")
        send_command(loc, f"tp {x} {y} {z}")
        # `ServerPlayer.tick` fires the trigger every twentieth tick, and the
        # clock is the packet clock, so the test owns the count: 40 recognized
        # packets carry *two* `tickCount % 20 == 0`, and only the teleport's own
        # packet — the first of them — can still read the pre-teleport position,
        # so one of the two ticks is guaranteed to read the staged cell. A
        # barrier rather than a plain `tick` because this teleport crosses into
        # a new chunk, and a crossing re-streams a chunk batch synchronously in
        # the handler — paced, and generated on demand — so everything sent
        # behind it (these ticks, and the trigger among them) waits for that
        # stream, and a quiet gap would close the window inside one of its
        # stalls. The ping is the last tick, and the pong cannot be written
        # until the ticks before it have run; the flush is written by then too.
        packets = wire_client.barrier(loc, ticks=40, timeout=30.0)
        updates = [read_update_advancements(b)
                   for b in all_of(packets, CB_UPDATE_ADVANCEMENTS)]
        assert updates, "the location trigger's completion must flush"
        assert not any(update["reset"] for update in updates), (
            "no flush in this window is the connection's first")
        assert not any(added_map(update) for update in updates), (
            "the challenge is already visible, so these are progress-only flushes: "
            f"{[sorted(added_map(u)) for u in updates]}")
        _, before = obtained_of(updates, "minecraft:adventure/adventuring_time",
                                "the staged teleport's flush")
        assert staging in before, (
            f"the trigger answered the dump's own cell: {before} !∋ {staging}")

        # The target is the dump's first row whose biome no flush has shown as
        # obtained yet: whatever the spawn's own cell earned is in `before` by
        # now, so the second teleport's delta is exactly its own criterion. The
        # target is in the staging cell's chunk, so this crossing streams no
        # chunks (the session only re-sends when the chunk changes) and the
        # window is a plain tick count.
        expected, x, y, z = adventure_biome_cell(data_root, exclude=before)
        print(f"-> /tp into {expected} at ({x}, {y}, {z}) and let the trigger run")
        send_command(loc, f"tp {x} {y} {z}")
        # The same 40-tick window, for the same reason: this teleport's own tick
        # still reads the staged cell (already obtained, so silent), and the
        # other trigger tick is the one that reads the target.
        packets = wire_client.barrier(loc, ticks=40, timeout=30.0)
        updates = [read_update_advancements(b)
                   for b in all_of(packets, CB_UPDATE_ADVANCEMENTS)]
        assert updates, "the location trigger's completion must flush"
        assert not any(update["reset"] for update in updates), (
            "no flush in this window is the connection's first")
        assert not any(added_map(update) for update in updates), (
            "still progress-only: "
            f"{[sorted(added_map(u)) for u in updates]}")
        entry, after = obtained_of(updates, "minecraft:adventure/adventuring_time",
                                   "the trigger's flush")
        assert expected in after, (
            f"the biome under the player is the dump's answer: {after} !∋ {expected}")
        assert set(after) - set(before) == {expected}, (
            "the teleport adds the row's own criterion and nothing else: "
            f"{before} -> {after}")
        assert set(before) <= set(after), (
            f"progress only grows: {before} -> {after}")
        assert len(entry) == 55, (
            f"the whole challenge rides the packet, one criterion per biome: {len(entry)}")
        print(f"  OK the location trigger completed {expected} "
              f"({len(after)} of {len(entry)} criteria)")
        loc.close()

        # --- 7. the enter-block trigger --------------------------------------
        # `CriteriaTriggers.ENTER_BLOCK` reads the blocks the player's own
        # bounding box overlaps, so it needs a world (a void session's `world`
        # is null and `/setblock` refuses). The cell is named relative to the
        # player (`~ ~1 ~` — the server's own position, which the recipe is not
        # told), and one above the feet is always inside the box: it is 1.8
        # tall. An end gateway is the one named block a generated world cannot
        # contain, so the advancement it earns is this phase's and no other's.
        print("-> /setblock ~ ~1 ~ end_gateway: the box enters a gateway")
        swim = Bot("WaveBot", port)
        wire_client.record(swim, note="advancements: enter_block and effects_changed")
        swim.join()
        swim.start_reader()
        tick(swim, 2)
        join_flush = flush_of(drain_until(swim, CB_UPDATE_ADVANCEMENTS),
                              "the worldgen join's flush")
        assert join_flush["reset"], "a fresh connection's first flush is the reset"
        send_command(swim, "setblock ~ ~1 ~ end_gateway")
        tick(swim, 3)
        # The tick is packet-driven, so the flush can land after any fixed
        # window on a loaded box; wait for the packet itself.
        packets = drain_until(swim, CB_UPDATE_ADVANCEMENTS, limit=15.0)
        updates = [read_update_advancements(b)
                   for b in all_of(packets, CB_UPDATE_ADVANCEMENTS)]
        assert updates, "entering a gateway must flush"
        assert not any(update["reset"] for update in updates), (
            "the join's own flush was the reset")
        entered = {}
        for update in updates:
            entered.update(added_map(update))
        # Six nodes and no others. The done gateway's own SHOW rule reveals its
        # children and grandchildren — `end/find_end_city`, and its two children
        # `end/elytra` and `end/levitate` — and its ancestors are visible as a
        # done descendant's (`end/kill_dragon`, then `end/root`). Everything else
        # in `end` is a *sibling* of one of those (`end/dragon_egg` and the
        # other children of the kill), and no branch outside `end` has anything
        # done.
        assert set(entered) == {
            "minecraft:end/root", "minecraft:end/kill_dragon",
            "minecraft:end/enter_end_gateway", "minecraft:end/find_end_city",
            "minecraft:end/elytra", "minecraft:end/levitate"}, sorted(entered)
        entry, obtained = obtained_of(updates, "minecraft:end/enter_end_gateway",
                                      "the gateway's flush")
        assert obtained == ["entered_end_gateway"], obtained
        print("  OK entered_end_gateway completed; the flush added its six-node window")

        # ...and water for the boat recipe advancements: the ten `in_water`
        # criteria (each boat's advancement carries that one criterion and
        # nothing else) are granted by the same probe. The water is written over
        # the gateway cell, so the box carries water whichever spawn this world
        # chose — but a spawn box that already overlapped water would have
        # earned them on the join's own tick, so the assertion is over both
        # windows and says which one carried them.
        print("-> /setblock ~ ~1 ~ water: the ten in_water criteria")
        send_command(swim, "setblock ~ ~1 ~ water")
        tick(swim, 3)
        # Wait for the boats themselves rather than any fixed window: the flush
        # is one tick's worth of packets, but the tick that produces it can be
        # arbitrarily late behind the join burst on a loaded box.
        before = set(added_map(join_flush))
        deadline = time.time() + 20.0
        packets = []
        updates = []
        waded = {}
        while time.time() < deadline:
            packets += drain(swim, 1.5)
            updates = [read_update_advancements(b)
                       for b in all_of(packets, CB_UPDATE_ADVANCEMENTS)]
            waded = {}
            for update in updates:
                waded.update(added_map(update))
            if set(BOAT_ADVANCEMENTS) <= before | set(waded):
                break
        assert not any(update["reset"] for update in updates), (
            "the join's own flush was the reset")
        after = before | set(waded)
        assert set(BOAT_ADVANCEMENTS) <= after, (
            f"the box overlaps water, so every in_water criterion is earned: "
            f"{sorted(set(BOAT_ADVANCEMENTS) - after)}")
        carried = "the setblock's flush" if set(BOAT_ADVANCEMENTS) - before else (
            "the join's own flush (the spawn box was already water)")
        print(f"  OK the ten boat recipe advancements completed on {carried}")
        # Nothing else moved with them: the tree's other branches have nothing
        # done, and the ten nodes are display-less recipe advancements.
        assert set(waded) <= set(BOAT_ADVANCEMENTS) | set(before), sorted(set(waded) - before)

        # --- 8. the effects-changed trigger ----------------------------------
        # `nether/all_potions` is one criterion whose `effects` map names 17
        # effects; `MobEffectsPredicate.matches` wants every one of them, so the
        # advancement appears on the flush of the last `/effect give` and on no
        # earlier one. Each command's own window is drained, which is what makes
        # the "one short of the list" half an assertion rather than a story.
        print("-> /effect give ... one at a time: sixteen are not the whole set")
        earned_early = []
        for index, effect in enumerate(ALL_POTION_EFFECTS):
            send_command(swim, f"effect give WaveBot {effect} infinite 0 true")
            tick(swim, 2)
            # 1.5 s, not a token 0.4: the batch's quiet window still ends 0.3 s
            # after the last packet, so an update that arrives at 1.2 s is caught
            # rather than judged absent. On a decorated `--worldgen` world the
            # session thread shares the box with chunk generation, and the
            # seventeenth effect's flush was the one that fell outside 0.4 s.
            # The last one waits for its packet instead of guessing a window:
            # under a loaded box the flush can land seconds later, and waiting
            # for it is still an assertion that it arrives (`drain_until`
            # raises the bound a missing packet is judged against, not the
            # expectation that one comes).
            if index + 1 < len(ALL_POTION_EFFECTS):
                packets = drain(swim, 1.5)
            else:
                packets = drain_until(swim, CB_UPDATE_ADVANCEMENTS, limit=60.0)
            adds = {}
            for update in [read_update_advancements(b)
                           for b in all_of(packets, CB_UPDATE_ADVANCEMENTS)]:
                assert not update["reset"]
                adds.update(added_map(update))
            if index + 1 < len(ALL_POTION_EFFECTS):
                if "minecraft:nether/all_potions" in adds:
                    earned_early.append(effect)
            else:
                assert "minecraft:nether/all_potions" in adds, (
                    f"the seventeenth effect completes it: {sorted(adds)}")
                # ...and the 34-effect `nether/all_effects` is still short.
                assert "minecraft:nether/all_effects" not in adds, sorted(adds)
        assert not earned_early, (
            f"a subset of the list must not complete it (after {earned_early})")
        print(f"  OK nether/all_potions completed on the {len(ALL_POTION_EFFECTS)}th effect")
        swim.close()
    finally:
        if proc is not None:
            proc.kill()
            proc.wait()

    print("\nadvancement wire test: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
