#!/usr/bin/env python3
"""Integration test: crafting (Phase 3 milestone D).

Bot1 joins, is given a crafting table and oak logs, places the table and opens
it. The test then drives the *real* menu protocol — the same packets a client
sends — to craft, so an engine that only works in unit tests cannot pass:

  * 3x3 crafting table (menu 12): a log is moved into the grid with PICKUP
    clicks, the derived result slot must become 4 oak planks, the log is taken
    out with an empty cursor, and exactly one log is consumed.
  * QUICK_MOVE (shift-click) on the result slot crafts *repeatedly* while the
    grid still matches — that repeat loop lives on the server in vanilla's
    AbstractContainerMenu.doClick, and the crafted output has to land in the
    player's inventory and reach the client.
  * the player menu's own 2x2 (window 0, grid at menu slots 1..4): two planks
    stacked vertically make 4 sticks, proving the 2x2 is a real crafting menu
    and not just the table's.
  * closing the crafting table hands its grid back to the inventory.
  * the smithing table (menu 21): a netherite upgrade derives the netherite
    sword and the take consumes all three inputs; an armor trim derives the
    *base* piece carrying a `minecraft:trim` component (decoded from the
    packet's bytes), the take consumes one of each input and completes the
    trim's own `recipe_crafted` criterion. All 18 trim patterns are swept, with
    the material cycled through the 11 registry entries, so the whole
    trim_pattern and trim_material registries reach the wire; an identical
    re-trim derives no result at all (vanilla's `applyTrim` no-op) and a
    different material replaces the trim.

The container_click packet carries the client's cursor as a HashedStack and the
server rejects a click whose cursor disagrees with its own (a stale client, see
handle_window_click). So the bot tracks the cursor from the server's
set_cursor_item replies and sends it back; a bot that always sent an empty
cursor could never pick a stack up.

Layout notes (verified against the 26.2 codecs, see docs/PHASE3.md):
  * open_screen (0x3b): [varint container id][varint menu id][title component]
  * container_set_content (0x12): [varint id][varint state][varint count][stacks][cursor]
  * container_set_slot (0x14): [varint id][varint state][i16 slot][stack]
  * set_cursor_item (0x60): [stack]
  * serverbound container_click (0x12): [varint id][varint state][i16 slot][i8 button]
        [varint ContainerInput][varint changed count]{[i16 slot][HashedStack]}
        [HashedStack carried]
  * serverbound close_window (0x13): [varint id]
  * a stack is [varint count]; when count > 0, [varint item id][DataComponentPatch]
  * a HashedStack is [u8 present]; when present, [varint item][varint count]
        [varint added]{[varint type][u32 hash]}[varint removed]{[varint type]}
"""

import os, socket, struct, sys, time, re, subprocess, tempfile
import wire_client
from wire_client import make_frame, read_packet

PROTO = 777
HOST = "127.0.0.1"

# Registry ids (generated item table, src/world/item_registry.h).
ITEM_OAK_LOG = 163
ITEM_OAK_PLANKS = 63
ITEM_STICK = 1060
ITEM_CRAFTING_TABLE = 405
ITEM_SMITHING_TABLE = 1512
ITEM_NETHERITE_TEMPLATE = 1579
ITEM_DIAMOND_SWORD = 1050
ITEM_NETHERITE_INGOT = 1023
ITEM_NETHERITE_SWORD = 1055
ITEM_IRON_CHESTPLATE = 1081
ITEM_COAL = 1010

# The 11 trim materials and 18 trim patterns in registry order, from
# src/world/trim_table.h (cross-checked against item_registry.h):
#   material = (registry name, trim_material id, item id, item registry name)
#   pattern  = (registry name, trim_pattern id, template item id)
# The `minecraft:trim` component on the wire carries `id + 1` for the material
# and for the pattern (ArmorTrim stores Holders, and ByteBufCodecs.holder
# reserves 0 for a direct holder). A pattern's template registry name is
# `minecraft:<name>_armor_trim_smithing_template`.
TRIM_MATERIALS = [
    ("amethyst", 0, 1016, "minecraft:amethyst_shard"),
    ("copper", 1, 1020, "minecraft:copper_ingot"),
    ("diamond", 2, 1012, "minecraft:diamond"),
    ("emerald", 3, 1013, "minecraft:emerald"),
    ("gold", 4, 1022, "minecraft:gold_ingot"),
    ("iron", 5, 1018, "minecraft:iron_ingot"),
    ("lapis", 6, 1014, "minecraft:lapis_lazuli"),
    ("netherite", 7, 1023, "minecraft:netherite_ingot"),
    ("quartz", 8, 1015, "minecraft:quartz"),
    ("redstone", 9, 824, "minecraft:redstone"),
    ("resin", 10, 1397, "minecraft:resin_brick"),
]
TRIM_PATTERNS = [
    ("bolt", 0, 1597),
    ("coast", 1, 1582),
    ("dune", 2, 1581),
    ("eye", 3, 1585),
    ("flow", 4, 1596),
    ("host", 5, 1595),
    ("raiser", 6, 1594),
    ("rib", 7, 1589),
    ("sentry", 8, 1580),
    ("shaper", 9, 1592),
    ("silence", 10, 1593),
    ("snout", 11, 1588),
    ("spire", 12, 1590),
    ("tide", 13, 1587),
    ("vex", 14, 1586),
    ("ward", 15, 1584),
    ("wayfinder", 16, 1591),
    ("wild", 17, 1583),
]
# The eight patterns `trim_with_all_exclusive_armor_patterns` names, in the
# order the sweep reaches them (its criteria are one AND group each).
EXCLUSIVE_TRIM_PATTERNS = ["rib", "silence", "snout", "spire",
                           "tide", "vex", "ward", "wayfinder"]
COMPONENT_TRIM = 58
ADV_ANY_TRIM = "minecraft:adventure/trim_with_any_armor_pattern"
ADV_ALL_TRIMS = "minecraft:adventure/trim_with_all_exclusive_armor_patterns"

# Packet ids.
SB_SET_CREATIVE_SLOT = 0x39
SB_CHAT_COMMAND = 0x07
SB_USE_ITEM_ON = 0x42
SB_CONTAINER_CLICK = 0x12
SB_CLOSE_WINDOW = 0x13
SB_KEEP_ALIVE = 0x1C
SB_FLYING = 0x21
CB_BLOCK_UPDATE = 0x08
CB_CONTAINER_SET_CONTENT = 0x12
CB_CONTAINER_SET_SLOT = 0x14
CB_OPEN_SCREEN = 0x3C
CB_SET_CURSOR_ITEM = 0x62
CB_KEEP_ALIVE = 0x2D
CB_UPDATE_ADVANCEMENTS = 0x85

# MenuType.crafting = 12, MenuType.smithing = 21 (SmithingMenu). The registry
# has 25 entries, so an id past 24 makes the client's open_screen decode throw.
MENU_CRAFTING = 12
MENU_SMITHING = 21
MENU_REGISTRY_SIZE = 25
CRAFTING_MENU_SIZE = 46
PLAYER_MENU_SIZE = 46
# SmithingMenu's own four slots come first: template, base, addition, result.
SM_TEMPLATE = 0
SM_BASE = 1
SM_ADDITION = 2
SM_RESULT = 3
SM_MAIN_FIRST = 4
SM_HOTBAR_FIRST = 31
SMITHING_MENU_SIZE = 40
# CraftingMenu layout: result 0, 3x3 grid 1..9, main 10..36, hotbar 37..45.
CT_GRID_FIRST = 1
CT_MAIN_FIRST = 10
CT_HOTBAR_FIRST = 37
# InventoryMenu layout: result 0, 2x2 grid 1..4, armour 5..8, main 9..35,
# hotbar 36..44, offhand 45.
PM_RESULT = 0
PM_GRID_FIRST = 1
PM_MAIN_FIRST = 9
PM_HOTBAR_FIRST = 36

# ContainerInput mode ids (the 26.2 ContainerInput enum order).
PICKUP = 0
QUICK_MOVE = 1

# Creative slot writes address the player *menu*: hotbar slot 0 is menu 36.
CREATIVE_HOTBAR0 = 36


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


def parse_open_screen(body):
    """Decodes open_screen the way ClientboundOpenScreenPacket does:
    [container id][menu type][title]. The container id comes FIRST."""
    window, i = rd_varint(body, 0)
    assert 1 <= window <= 100, (
        f"the container id must stay in vanilla's 1..100 range, got {window}")
    menu, j = rd_varint(body, i)
    assert menu < MENU_REGISTRY_SIZE, (
        f"menu type {menu} is past the end of the {MENU_REGISTRY_SIZE}-entry "
        "menu registry — the client throws while decoding and disconnects")
    return window, menu, j


TAG_STRING = 0x08
TAG_LIST = 0x09
TAG_COMPOUND = 0x0A


def rd_nbt_string(body, pos):
    """An NBT string: [u16 big-endian length][utf8] — *not* the varint-prefixed
    form `rd_string` reads (that is the packet layer's)."""
    ln = struct.unpack(">H", body[pos:pos + 2])[0]
    pos += 2
    return body[pos:pos + ln].decode("utf-8"), pos + ln


def read_title(body, pos):
    """The open_screen title component: either a literal string (TAG_String, as
    `Writer::component_text` writes, and what `parse_open_screen` leaves behind)
    or a compound carrying a `translate` key (`component_translate`). Vanilla's
    block menus are translatable components, so a title that decodes to the wrong
    shape is a client-visible wrong string."""
    tag = body[pos]
    pos += 1
    if tag == TAG_STRING:
        text, pos = rd_nbt_string(body, pos)
        return {"type": "text", "text": text}
    assert tag == TAG_COMPOUND, (
        f"a title component is a string or a compound, got tag {tag:#x}")
    title = {"type": "translate", "key": None}
    while body[pos] != 0x00:  # TAG_End
        kind = body[pos]
        pos += 1
        field, pos = rd_nbt_string(body, pos)
        if kind == TAG_STRING:
            value, pos = rd_nbt_string(body, pos)
            if field == "translate":
                title["key"] = value
        elif kind == TAG_LIST:  # `with`: [element tag][i32 count][elements]
            pos += 1  # the list's element tag
            count = struct.unpack(">i", body[pos:pos + 4])[0]
            pos += 4
            for _ in range(count):
                assert body[pos] == TAG_COMPOUND, "a `with` argument is a component"
                pos += 1
                while body[pos] != 0x00:
                    nested = body[pos]
                    pos += 1
                    _, pos = rd_nbt_string(body, pos)
                    assert nested == TAG_STRING, "only a nested text argument exists here"
                    _, pos = rd_nbt_string(body, pos)
                pos += 1
        else:
            raise AssertionError(f"unexpected title field tag {kind:#x}")
    return title


def position(x, y, z):
    """BlockPos.asLong, the 8 wire bytes (see `wire_client.pack_block_pos`).

    It goes through the shared codec because this one packed `">q"` without the
    64-bit mask: a *negative* x masks to a value above 2**63 and `struct.error`s
    out, so no caller of this helper could ever click a cell at x < 0."""
    return wire_client.pack_block_pos(x, y, z)


def rd_component_patch(body, pos):
    """Decodes a *trusted* DataComponentPatch -> ({type id: value}, new_pos).

    This is the serverbound-to-client form (container_set_content,
    container_set_slot): each value is written inline by its own codec and has no
    length prefix, so a component whose value this decoder does not know cannot
    be skipped — it is an assertion failure rather than a guess about where the
    next field starts (brewing_test's decoder makes the same choice). Only the
    components a crafting result can carry are accepted.
    """
    # DataComponentPatch.STREAM_CODEC: both counts, then the added entries and
    # finally the removed type ids.
    added, pos = rd_varint(body, pos)
    removed, pos = rd_varint(body, pos)
    components = {}
    for _ in range(added):
        cid, pos = rd_varint(body, pos)
        if cid == 3:                                    # minecraft:damage
            value, pos = rd_varint(body, pos)
            components[cid] = value
        elif cid == 13:                                 # minecraft:enchantments
            n, pos = rd_varint(body, pos)
            entries = []
            for _ in range(n):
                ench, pos = rd_varint(body, pos)
                level, pos = rd_varint(body, pos)
                entries.append((ench, level))
            components[cid] = entries
        elif cid == COMPONENT_TRIM:                     # minecraft:trim
            material, pos = rd_varint(body, pos)
            pattern, pos = rd_varint(body, pos)
            components[cid] = (material, pattern)
        else:
            raise AssertionError(f"unmodelled component {cid} in a stack patch")
    for _ in range(removed):
        _, pos = rd_varint(body, pos)                   # a removed component type
    assert removed == 0, f"a removed component is not modelled: {removed}"
    return components, pos


def read_stack(body, pos):
    """Returns ((item_id, count), components, new_pos). count 0 = empty stack."""
    count, pos = rd_varint(body, pos)
    if count <= 0:
        return (0, 0), {}, pos
    item, pos = rd_varint(body, pos)
    components, pos = rd_component_patch(body, pos)
    return (item, count), components, pos


def write_stack(item, count):
    if count <= 0:
        return varint(0)
    return varint(count) + varint(item) + varint(0) + varint(0)


def write_hashed_stack(stack):
    """The cursor field of a container_click: [u8 present][item][count][patches]."""
    item, count = stack
    if count <= 0:
        return b"\x00"
    return bytes([1]) + varint(item) + varint(count) + varint(0) + varint(0)


def decode_content(body):
    """Decodes container_set_content ->
    (window, state, [stacks], cursor, [{type id: value}] per slot)."""
    window = body[0]
    state, i = rd_varint(body, 1)
    n, i = rd_varint(body, i)
    stacks = []
    components = []
    for _ in range(n):
        stack, comps, i = read_stack(body, i)
        stacks.append(stack)
        components.append(comps)
    cursor, _, i = read_stack(body, i)
    return window, state, stacks, cursor, components


def advancement_criteria(packets, advancement_id):
    """The criteria map of `advancement_id` in the update_advancements packet
    that carries it, as {name: obtained flag}, or None.

    make_update_advancements writes *every* requirement criterion of an
    advancement whose progress changed — obtained or not — so a bare substring
    match cannot tell a granted criterion from a pending one. The progress entry
    is the advancement id followed by its criteria map, and it comes after the
    added definitions (which also name their ids and parents), so the *last*
    occurrence of the id is the progress entry."""
    needle = advancement_id.encode()
    for p, body in packets:
        if p != CB_UPDATE_ADVANCEMENTS:
            continue
        pos = body.rfind(needle)
        if pos < 0:
            continue
        i = pos + len(needle)
        count, i = rd_varint(body, i)
        if count is None or count > 256:
            continue
        criteria = {}
        try:
            for _ in range(count):
                name, i = rd_string(body, i)
                obtained = body[i]
                i += 1
                if obtained:
                    i += 8  # the obtained Instant (FriendlyByteBuf.writeInstant)
                criteria[name] = obtained
        except (IndexError, TypeError, UnicodeDecodeError):
            continue  # not a criteria map after all: keep looking
        if criteria and all("armor_trimmed_minecraft:" in n for n in criteria):
            return criteria
    return None


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

    def send(self, pid, body=b""):
        self.reader.send(pid, body)

    def recv(self):
        pid, body = read_packet(self.sock, self.threshold)
        # Optional per-test capture. The join consumes most of the initial
        # burst before `start_reader`, so a test that needs to see one of those
        # packets (the translated-player proxy, for example) can install a hook
        # before `join()` and keep every read, including the reader thread's.
        hook = getattr(self, "packet_hook", None)
        if hook is not None:
            hook(pid, body)
        return pid, body

    def start_reader(self):
        self.reader.start()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def move_to(self, x, y, z, ground=1):
        """ServerboundMovePlayerPos (0x1e): feet x/y/z + on-ground flag.

        The server measures its block interaction range (4.5 + a 1.0 lag buffer,
        from the eye to the block's box) from where the client says it is, so a
        bot has to stand next to what it clicks — as a real client always does.
        """
        self.send(0x1e, struct.pack(">ddd", x, y, z) + bytes([ground]))

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
            if pid == 0x32:
                break
        for _ in range(400):
            pid, body = self.recv()
            if pid == CB_KEEP_ALIVE:
                self.send(SB_KEEP_ALIVE, body[:8])
            elif pid == 0x0B:
                b, _ = rd_varint(body)
                self.send(0x0B, varint(b))
                break


def drain(bot, timeout=1.5):
    """Every packet the reader thread queued in the next `timeout`
    seconds.

    A reply window ends when the server has been quiet for `quiet` seconds
    rather than always burning the whole window; `timeout` stays the bound a
    missing reply is judged against. This is the shared drain the tier gates
    and the recipe book import, so it is quiet for all of them; before a reader
    is started (`Reader.start`, after each join) `wire_client.drain_quiet`
    falls back to reading the socket directly for the whole window, which is
    what the join's own reads need.
    """
    return wire_client.drain_quiet(bot, quiet=0.2, limit=timeout)
def find(packets, pid):
    for p, body in packets:
        if p == pid:
            return body
    return None


def wait_listening(proc, timeout=None):
    """Wait for the bind line, then for the socket to accept connections.

    Startup is event-driven: a loaded full-suite run can take longer than any
    fixed allowance to open the world before binding. EOF is the deterministic
    failure, and CTest's overall timeout is the guard against a true hang.
    """
    deadline = None if timeout is None else time.time() + timeout
    port = None
    while deadline is None or time.time() < deadline:
        raw = proc.stdout.readline()
        if not raw:
            raise RuntimeError("server exited before listening")
        line = raw.decode(errors="replace") if isinstance(raw, bytes) else raw
        sys.stderr.write(line)
        m = re.search(r"listening on.*:(\d+)", line)
        if m:
            port = int(m.group(1))
            break
    if port is None:
        raise RuntimeError("server did not report a listening port in time")
    while True:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=1)
            s.close()
            return port
        except OSError:
            if proc.poll() is not None:
                raise RuntimeError("server exited before accepting connections")
            time.sleep(0.05)


def place(bot, x, y, z, face, seq):
    body = (varint(0) + position(x, y, z) + varint(face)
            + struct.pack(">f", 0.5) + struct.pack(">f", 0.0) + struct.pack(">f", 0.5)
            + bytes([0]) + bytes([0]) + varint(seq))
    bot.send(SB_USE_ITEM_ON, body)


def give(bot, menu_slot, item, count):
    bot.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", menu_slot) + write_stack(item, count))


def slash_give(bot, player, item, count=1):
    """`/give <player> <item> <count>` over the chat-command packet. Unlike a
    creative slot write this does not close an open menu — it only adds to the
    inventory and resyncs the content — so the smithing window's sweep can
    restock between takes."""
    cmd = f"give {player} {item} {count}"
    bot.send(SB_CHAT_COMMAND, varint(len(cmd)) + cmd.encode())


def find_item(menu, item):
    """The first menu slot (its own or the player's) that holds `item`."""
    for i, (held, count) in enumerate(menu.slots):
        if held == item and count > 0:
            return i
    raise AssertionError(f"no slot holds item {item}: {menu.slots}")


class Menu:
    """Tracks the server's view of an open menu: window id, revision, slots and
    cursor. Every click must name the *current* revision and cursor or the
    server treats it as a stale client and just resyncs, so the bot has to keep
    them in step with the packets it receives."""

    def __init__(self, bot):
        self.bot = bot
        self.window = None
        self.menu_id = None
        self.state = 0
        self.slots = None
        self.cursor = (0, 0)
        # The decoded component patch of every slot and of the cursor, so a test
        # can assert what the client would *draw* (a trim), not just the item.
        self.components = None
        self.cursor_components = {}

    def _apply(self, pid, body):
        if pid == CB_KEEP_ALIVE:
            self.bot.send(SB_KEEP_ALIVE, body[:8])
        elif pid == CB_OPEN_SCREEN:
            self.window, self.menu_id, _ = parse_open_screen(body)
        elif pid == CB_CONTAINER_SET_CONTENT:
            w, state, slots, cursor, components = decode_content(body)
            self.window, self.state, self.slots, self.cursor = w, state, slots, cursor
            self.components = components
        elif pid == CB_CONTAINER_SET_SLOT:
            w, i = rd_varint(body, 0)
            state, i = rd_varint(body, i)
            slot = struct.unpack(">h", body[i:i + 2])[0]
            stack, comps, _ = read_stack(body, i + 2)
            self.window, self.state = w, state
            if self.components is not None and 0 <= slot < len(self.slots):
                self.slots[slot] = stack
                self.components[slot] = comps
        elif pid == CB_SET_CURSOR_ITEM:
            self.cursor, self.cursor_components, _ = read_stack(body, 0)

    def pump(self, timeout=0.8):
        """The reply to whatever the bot just sent, applied to the menu's own
        view of window, state, slots and cursor.

        A menu reply is synchronous — the session answers a click inside its own
        handler — so this returns as soon as the server stops talking instead of
        waiting `timeout` out (`drain_quiet`), with `timeout` kept as the upper
        bound a *missing* reply is judged against. A click therefore costs the
        reply, not the window: the 31 clicks below each saved most of their
        window.
        """
        packets = wire_client.drain_quiet(self.bot, quiet=0.15, limit=timeout)
        for pid, body in packets:
            self._apply(pid, body)
        return packets

    def click(self, slot, button, mode):
        assert self.window is not None, "no menu open"
        body = (varint(self.window) + varint(self.state) + struct.pack(">h", slot)
                + bytes([button]) + varint(mode) + varint(0)
                + write_hashed_stack(self.cursor))
        self.bot.send(SB_CONTAINER_CLICK, body)
        return self.pump()

    def slot(self, index):
        return self.slots[index]

    def trim(self, index):
        """The `minecraft:trim` component of a slot as the wire carries it
        ([material + 1][pattern + 1]), or None for an untrimmed stack."""
        return self.components[index].get(COMPONENT_TRIM)


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-craft-")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "1200"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "4000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    # The survival tick counts packets (one tick each, no wall-time catch-up), so
    # a drain banks no ticks. The subject here is the crafting menu's slots and
    # the recipe book's results — states and writes, not durations.
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
    # The flat world the block-interact test uses: the surface block is at y=3,
    # so a block placed on top of it lands at y=4.
    GROUND_Y = 3

    proc = None
    try:
        print(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", "127.0.0.1", "--port", "0", "--motd", "craft test",
             "--offline-mode", "--gamemode", "1", "--data", data_root,
             "--world", world], wait_listening, env=env)

        print("Connecting bot...")
        bot = Bot("CraftBot", port)
        wire_client.record(bot, note="craft: the grid, the recipe book, the result slot")
        bot.join()
        # Hand the read side to the reader thread now that the join is over, the
        # way the other socket tests do: `drain`/`drain_quiet` then pull from its
        # queue, so a reply-window can end the moment the server goes quiet
        # instead of always waiting its window out (see `Menu.pump`).
        bot.start_reader()
        drain(bot, 1.0)
        # Stand where the table is placed (its target is (0, GROUND_Y, 0) and the
        # table lands above it, so the bot's feet go one higher still).
        bot.move_to(0.5, GROUND_Y + 2.0, 0.5)
        time.sleep(0.2)

        # Put a crafting table in hotbar 0 and place it on the ground.
        give(bot, CREATIVE_HOTBAR0, ITEM_CRAFTING_TABLE, 1)
        time.sleep(0.2)
        drain(bot, 0.5)
        place(bot, 0, GROUND_Y, 0, 1, 1)
        packets = drain(bot, 2.0)
        assert find(packets, CB_BLOCK_UPDATE) is not None, (
            f"crafting table place produced no block update: {[(hex(p)) for p, _ in packets]}")
        print("  crafting table placed at (0, 4, 0)")

        # Give logs *before* opening: a creative slot write is a creative-screen
        # packet, so the server treats it as "the client left the container" and
        # drops the window. Doing it now keeps the table open below.
        give(bot, CREATIVE_HOTBAR0, ITEM_OAK_LOG, 4)
        time.sleep(0.2)
        drain(bot, 0.5)

        # --- open the crafting table -----------------------------------------
        menu = Menu(bot)
        place(bot, 0, GROUND_Y + 1, 0, 1, 2)
        packets = menu.pump(2.0)
        opened = find(packets, CB_OPEN_SCREEN)
        assert opened is not None, (
            f"right-clicking the table produced no open_screen: "
            f"{[(hex(p)) for p, _ in packets]}")
        os_window, os_menu, title_pos = parse_open_screen(opened)
        assert os_menu == MENU_CRAFTING, (
            f"expected the crafting menu (12), got menu {os_menu}")
        title = read_title(opened, title_pos)
        assert title == {"type": "text", "text": "Crafting"}, (
            "the crafting table's screen titles itself Crafting (vanilla's "
            f"container.crafting renders the same), got {title}")
        # The content must name the *same* window open_screen just opened. A
        # crafting table has no block container behind it, so it is easy to fall
        # back to "no container => window 0" when writing the content — and a
        # real client ignores content whose container id is not the one
        # open_screen gave it, then drops every click as a stale window.
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "open_screen without container_set_content"
        cw, _, _, _, _ = decode_content(content)
        assert cw == os_window, (
            f"container_set_content names window {cw} but open_screen opened "
            f"{os_window} — the client will ignore the content and its clicks")
        assert menu.window == os_window, (
            f"the tracked window must stay {os_window}, got {menu.window}")
        assert menu.slots is not None, "open_screen without container_set_content"
        assert len(menu.slots) == CRAFTING_MENU_SIZE, (
            f"a crafting table has {CRAFTING_MENU_SIZE} slots, got {len(menu.slots)}")
        assert menu.slots[PM_RESULT] == (0, 0), "a fresh table has an empty result"
        assert menu.slots[CT_HOTBAR_FIRST] == (ITEM_OAK_LOG, 4), (
            f"hotbar 0 is menu slot {CT_HOTBAR_FIRST} and should hold 4 logs, got "
            f"{menu.slots[CT_HOTBAR_FIRST]}")
        assert all(s == (0, 0) for i, s in enumerate(menu.slots) if i != CT_HOTBAR_FIRST), (
            "a fresh crafting table must be otherwise empty")
        print(f"  opened window {menu.window} (menu {menu.menu_id}, "
              f"{len(menu.slots)} slots, state {menu.state})")

        # --- craft one output by hand ----------------------------------------
        # Pick the logs up, right-click one into grid cell 0, put the rest back.
        menu.click(CT_HOTBAR_FIRST, 0, PICKUP)
        assert menu.cursor == (ITEM_OAK_LOG, 4), (
            f"picking up hotbar 0 should fill the cursor with 4 logs, got {menu.cursor}")
        assert menu.slot(CT_HOTBAR_FIRST) == (0, 0), "the source slot must be empty"
        menu.click(CT_GRID_FIRST, 1, PICKUP)  # right-click: one item into the grid
        assert menu.slot(CT_GRID_FIRST) == (ITEM_OAK_LOG, 1), (
            f"the grid cell should hold one log, got {menu.slot(CT_GRID_FIRST)}")
        assert menu.cursor == (ITEM_OAK_LOG, 3), (
            f"the cursor should have 3 logs left, got {menu.cursor}")
        assert menu.slot(PM_RESULT) == (ITEM_OAK_PLANKS, 4), (
            "one oak log must derive 4 oak planks in the result slot, got "
            f"{menu.slot(PM_RESULT)}")
        print("  one log in the grid derived 4 oak planks")
        menu.click(CT_HOTBAR_FIRST, 0, PICKUP)  # put the 3 logs back
        assert menu.cursor == (0, 0), f"the cursor should be empty, got {menu.cursor}"
        assert menu.slot(CT_HOTBAR_FIRST) == (ITEM_OAK_LOG, 3)

        # Take the result with an empty cursor. ResultSlot.onTake consumes one
        # item from every non-empty grid cell, so the log must go.
        menu.click(PM_RESULT, 0, PICKUP)
        assert menu.cursor == (ITEM_OAK_PLANKS, 4), (
            f"taking the result should put 4 planks on the cursor, got {menu.cursor}")
        assert menu.slot(CT_GRID_FIRST) == (0, 0), (
            f"the log must be consumed by the craft, got {menu.slot(CT_GRID_FIRST)}")
        assert menu.slot(PM_RESULT) == (0, 0), "the result must be re-derived to empty"
        print("  took the 4 planks; the grid cell was consumed")
        menu.click(CT_MAIN_FIRST, 0, PICKUP)  # deposit into the empty first main slot
        assert menu.cursor == (0, 0)
        assert menu.slot(CT_MAIN_FIRST) == (ITEM_OAK_PLANKS, 4), (
            f"the crafted planks should be in the inventory, got {menu.slot(CT_MAIN_FIRST)}")

        # --- shift-clicking an ingredient fills the grid ----------------------
        # CraftingMenu.quickMoveStack tries the 3x3 grid *before* it swaps
        # main <-> hotbar, so a shift-click on a log in the hotbar must land in
        # the grid rather than the main inventory and derive the result. (The
        # player menu's own 2x2 deliberately does not do this — vanilla only
        # auto-fills a crafting table's grid.)
        assert menu.slot(CT_HOTBAR_FIRST) == (ITEM_OAK_LOG, 3)
        menu.click(CT_HOTBAR_FIRST, 0, QUICK_MOVE)
        assert menu.slot(CT_HOTBAR_FIRST) == (0, 0), (
            f"the shift-clicked logs must leave the hotbar, got {menu.slot(CT_HOTBAR_FIRST)}")
        assert menu.slot(CT_GRID_FIRST) == (ITEM_OAK_LOG, 3), (
            "a shift-click on an ingredient must fill the crafting grid, got "
            f"{menu.slot(CT_GRID_FIRST)}")
        assert menu.slot(PM_RESULT) == (ITEM_OAK_PLANKS, 4), (
            "the auto-filled grid must derive the result, got "
            f"{menu.slot(PM_RESULT)}")
        assert menu.cursor == (0, 0), "a shift-click never touches the cursor"
        print("  shift-click filled the grid with 3 logs")

        # --- shift-click the result crafts repeatedly -------------------------
        # The server must loop, consuming one log and adding 4 planks each time.
        menu.click(PM_RESULT, 0, QUICK_MOVE)
        # 3 crafts happen (one log each) -> 12 planks, merged onto the 4 already
        # in the first main slot.
        assert menu.slot(CT_MAIN_FIRST) == (ITEM_OAK_PLANKS, 16), (
            "a shift-click on the output must craft repeatedly; the first main "
            f"slot should hold 4 + 12 planks, got {menu.slot(CT_MAIN_FIRST)}")
        assert menu.slot(CT_GRID_FIRST) == (0, 0), (
            f"all 3 logs must be consumed, got {menu.slot(CT_GRID_FIRST)}")
        assert menu.slot(PM_RESULT) == (0, 0), "the grid is empty, so the result is too"
        assert menu.cursor == (0, 0), "a shift-click never touches the cursor"
        print("  shift-click crafted 3x (12 planks) into the inventory")

        # --- the 2x2 player menu crafts too ----------------------------------
        # Closing the table hands its (empty) grid back and re-states the player
        # menu, window 0.
        bot.send(SB_CLOSE_WINDOW, varint(menu.window))
        menu.pump(1.5)
        assert menu.window == 0, f"after closing, window 0 is the player menu, got {menu.window}"
        assert menu.slots is not None and len(menu.slots) == PLAYER_MENU_SIZE, (
            f"the player menu has {PLAYER_MENU_SIZE} slots")
        # The 16 planks are main inventory slot 0, which the player menu shows at
        # slot 9 (armour 5..8 pushes main down).
        assert menu.slot(PM_MAIN_FIRST) == (ITEM_OAK_PLANKS, 16), (
            f"the planks should be in player menu slot {PM_MAIN_FIRST}, got "
            f"{menu.slot(PM_MAIN_FIRST)}")
        print("  closed the table; the player menu shows 16 planks in slot 9")

        # Two planks stacked vertically (grid cells 0 and 2 = menu slots 1 and 3)
        # make 4 sticks.
        menu.click(PM_MAIN_FIRST, 0, PICKUP)          # pick up all 16 planks
        menu.click(PM_GRID_FIRST, 1, PICKUP)          # one into cell 0
        menu.click(PM_GRID_FIRST + 2, 1, PICKUP)      # one into cell 2
        assert menu.slot(PM_GRID_FIRST) == (ITEM_OAK_PLANKS, 1)
        assert menu.slot(PM_GRID_FIRST + 2) == (ITEM_OAK_PLANKS, 1)
        assert menu.slot(PM_RESULT) == (ITEM_STICK, 4), (
            f"two stacked planks in the 2x2 must derive 4 sticks, got {menu.slot(PM_RESULT)}")
        print("  the player's 2x2 derived 4 sticks")
        menu.click(PM_MAIN_FIRST, 0, PICKUP)          # put the 14 planks back
        assert menu.slot(PM_MAIN_FIRST) == (ITEM_OAK_PLANKS, 14)
        assert menu.cursor == (0, 0)
        menu.click(PM_RESULT, 0, PICKUP)              # take the sticks
        assert menu.cursor == (ITEM_STICK, 4), (
            f"taking the sticks should fill the cursor, got {menu.cursor}")
        assert menu.slot(PM_GRID_FIRST) == (0, 0), "both planks must be consumed"
        assert menu.slot(PM_GRID_FIRST + 2) == (0, 0), "both planks must be consumed"
        menu.click(PM_MAIN_FIRST + 1, 0, PICKUP)      # deposit into the next empty main slot
        assert menu.slot(PM_MAIN_FIRST + 1) == (ITEM_STICK, 4), (
            f"the sticks should be in the inventory, got {menu.slot(PM_MAIN_FIRST + 1)}")
        assert menu.cursor == (0, 0)
        print("  crafted 4 sticks in the player's own 2x2")

        # --- smithing table ---------------------------------------------------
        # A netherite upgrade is a smithing_transform: the upgrade template in
        # slot 0, the diamond sword in slot 1, a netherite ingot in slot 2, and
        # the result at slot 3 (SmithingMenu's four own slots come *first*, so
        # the player slots shift to 4..39).
        give(bot, CREATIVE_HOTBAR0, ITEM_SMITHING_TABLE, 1)
        time.sleep(0.2)
        drain(bot, 0.5)
        place(bot, 1, GROUND_Y, 0, 1, 20)
        packets = drain(bot, 2.0)
        assert find(packets, CB_BLOCK_UPDATE) is not None, "smithing table place failed"
        # All three ingredients into the *hotbar* before opening (a creative slot
        # write drops the window): menu slots 31, 32 and 33 once the menu opens.
        # The armour-trim sweep is restocked later by /give, which does not close
        # the window, so nothing for it is given here.
        give(bot, CREATIVE_HOTBAR0, ITEM_NETHERITE_TEMPLATE, 1)
        give(bot, CREATIVE_HOTBAR0 + 1, ITEM_DIAMOND_SWORD, 1)
        give(bot, CREATIVE_HOTBAR0 + 2, ITEM_NETHERITE_INGOT, 1)
        give(bot, CREATIVE_HOTBAR0 + 3, ITEM_COAL, 5)
        time.sleep(0.3)
        drain(bot, 0.8)

        place(bot, 1, GROUND_Y + 1, 0, 1, 21)
        packets = menu.pump(2.0)
        opened = find(packets, CB_OPEN_SCREEN)
        assert opened is not None, (
            f"right-clicking the smithing table produced no open_screen: "
            f"{[(hex(p)) for p, _ in packets]}")
        sm_window, sm_menu, title_pos = parse_open_screen(opened)
        assert sm_menu == MENU_SMITHING, (
            f"expected the smithing menu (21), got menu {sm_menu}")
        # SmithingTableBlock.CONTAINER_TITLE is `container.upgrade` — "Upgrade
        # Gear", not "Smithing" — and it is a *translatable* component, so a
        # non-English client shows its own wording.
        title = read_title(opened, title_pos)
        assert title == {"type": "translate", "key": "container.upgrade"}, (
            "the smithing table's screen must send vanilla's container.upgrade, "
            f"got {title}")
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "open_screen without container_set_content"
        cw, _, _, _, _ = decode_content(content)
        assert cw == sm_window, (
            f"container_set_content names window {cw} but open_screen opened {sm_window}")
        assert menu.slots is not None and len(menu.slots) == SMITHING_MENU_SIZE, (
            f"a smithing table has {SMITHING_MENU_SIZE} slots, got "
            f"{len(menu.slots) if menu.slots else None}")
        print(f"  opened window {sm_window} (menu {sm_menu}, {len(menu.slots)} slots)")

        # mayPlace: the template slot only takes a template. Put coal on the
        # cursor and click it there — the click must be a no-op, exactly as the
        # client's own Slot predicts.
        menu.click(SM_HOTBAR_FIRST + 3, 0, PICKUP)  # pick up the coal
        assert menu.cursor == (ITEM_COAL, 5), f"cursor should hold coal, got {menu.cursor}"
        menu.click(SM_TEMPLATE, 0, PICKUP)         # try to drop it in the template slot
        assert menu.slot(SM_TEMPLATE) == (0, 0), (
            "a smithing template slot must refuse a non-template item, got "
            f"{menu.slot(SM_TEMPLATE)}")
        assert menu.cursor == (ITEM_COAL, 5), (
            f"the refused click must leave the cursor alone, got {menu.cursor}")
        menu.click(SM_HOTBAR_FIRST + 3, 0, PICKUP)  # put the coal back
        assert menu.cursor == (0, 0)
        print("  the template slot refused a non-template item (mayPlace)")

        # Shift-clicking each ingredient routes it to the *right* input slot.
        menu.click(SM_HOTBAR_FIRST, 0, QUICK_MOVE)      # template -> slot 0
        assert menu.slot(SM_TEMPLATE) == (ITEM_NETHERITE_TEMPLATE, 1), (
            f"the template should be in input 0, got {menu.slot(SM_TEMPLATE)}")
        assert menu.slot(SM_HOTBAR_FIRST) == (0, 0), "the template must leave the hotbar"
        assert menu.slot(SM_RESULT) == (0, 0), "one input is not a smithing recipe"
        menu.click(SM_HOTBAR_FIRST + 1, 0, QUICK_MOVE)  # diamond sword -> slot 1
        assert menu.slot(SM_BASE) == (ITEM_DIAMOND_SWORD, 1), (
            f"the sword should be in input 1, got {menu.slot(SM_BASE)}")
        menu.click(SM_HOTBAR_FIRST + 2, 0, QUICK_MOVE)  # netherite ingot -> slot 2
        assert menu.slot(SM_ADDITION) == (ITEM_NETHERITE_INGOT, 1), (
            f"the ingot should be in input 2, got {menu.slot(SM_ADDITION)}")
        assert menu.slot(SM_RESULT) == (ITEM_NETHERITE_SWORD, 1), (
            "template + diamond sword + netherite ingot must derive a netherite "
            f"sword, got {menu.slot(SM_RESULT)}")
        print("  the three inputs derived a netherite sword")

        # Taking the result consumes one of each input (SmithingMenu.onTake).
        menu.click(SM_RESULT, 0, PICKUP)
        assert menu.cursor == (ITEM_NETHERITE_SWORD, 1), (
            f"taking the result should fill the cursor, got {menu.cursor}")
        assert menu.slot(SM_TEMPLATE) == (0, 0), "the template must be consumed"
        assert menu.slot(SM_BASE) == (0, 0), "the base must be consumed"
        assert menu.slot(SM_ADDITION) == (0, 0), "the addition must be consumed"
        assert menu.slot(SM_RESULT) == (0, 0), "the result must be re-derived to empty"
        # The first two main slots already hold the planks and sticks from the
        # 2x2 section, so deposit into the next free one.
        deposit = SM_MAIN_FIRST + 2
        assert menu.slot(deposit) == (0, 0), "expected a free inventory slot"
        menu.click(deposit, 0, PICKUP)
        assert menu.slot(deposit) == (ITEM_NETHERITE_SWORD, 1), (
            f"the sword should be in the inventory, got {menu.slot(deposit)}")
        assert menu.cursor == (0, 0)
        print("  taking the netherite sword consumed all three inputs")

        # --- armor trim: all eighteen patterns --------------------------------
        # A smithing_trim: the pattern's template in slot 0, a trimmable armor
        # piece in slot 1, and a *material* in slot 2. The material is not in the
        # recipe — it is the addition stack's own minecraft:provides_trim_material
        # — and the result is the base stack with a minecraft:trim component, so
        # the client draws the piece trimmed and its tooltip names the pattern.
        #
        # Every one of the 18 patterns is swept, with the material cycled through
        # the 11 registry entries, so the run drives both registries whole. One
        # chestplate carries the sweep: once it wears a trim, a different
        # (material, pattern) replaces it, so each result is checked against the
        # pair that was just placed.
        #
        # The restock arrives by /give rather than a creative slot write: a
        # creative write is a creative-screen packet, which the server reads as
        # "the client left the container" and answers by dropping the window
        # (handle_set_creative_slot). /give only adds to the inventory and
        # resyncs this menu's content, so the window survives between takes.
        slash_give(bot, "CraftBot", "minecraft:iron_chestplate", 1)
        time.sleep(0.3)
        menu.pump(0.6)
        menu.click(find_item(menu, ITEM_IRON_CHESTPLATE), 0, QUICK_MOVE)
        assert menu.slot(SM_BASE) == (ITEM_IRON_CHESTPLATE, 1), (
            f"the chestplate should be in the base slot, got {menu.slot(SM_BASE)}")
        print("  the plain chestplate is in the base slot")

        for pattern, pattern_id, template_item in TRIM_PATTERNS:
            mat, mat_id, mat_item, mat_arg = TRIM_MATERIALS[pattern_id % len(TRIM_MATERIALS)]
            slash_give(bot, "CraftBot", f"minecraft:{pattern}_armor_trim_smithing_template", 1)
            slash_give(bot, "CraftBot", mat_arg, 1)
            time.sleep(0.25)
            menu.pump(0.6)
            menu.click(find_item(menu, template_item), 0, QUICK_MOVE)
            assert menu.slot(SM_TEMPLATE) == (template_item, 1), (
                f"the {pattern} template should be in input 0, got {menu.slot(SM_TEMPLATE)}")
            menu.click(find_item(menu, mat_item), 0, QUICK_MOVE)
            assert menu.slot(SM_ADDITION) == (mat_item, 1), (
                f"the {mat} material should be in input 2, got {menu.slot(SM_ADDITION)}")
            # The wire is where the trim lives: the result slot's minecraft:trim
            # component must name this material and this pattern, each as Holder
            # id + 1.
            expect = (mat_id + 1, pattern_id + 1)
            assert menu.slot(SM_RESULT) == (ITEM_IRON_CHESTPLATE, 1), (
                f"{pattern} + {mat} must derive the *chestplate*, got {menu.slot(SM_RESULT)}")
            assert menu.trim(SM_RESULT) == expect, (
                f"the {pattern}-trimmed chestplate must carry minecraft:trim {expect}, "
                f"got {menu.trim(SM_RESULT)}")

            # Taking it consumes one of each input and completes this pattern's
            # own recipe_crafted criterion: the *recipe* the result was derived
            # from is the trim's, which is what makes the advancement reachable
            # in play.
            packets = menu.click(SM_RESULT, 0, PICKUP)
            assert menu.slot(SM_TEMPLATE) == (0, 0), "the take must consume the template"
            assert menu.slot(SM_BASE) == (0, 0), "the take must consume the base"
            assert menu.slot(SM_ADDITION) == (0, 0), "the take must consume the material"
            assert menu.cursor == (ITEM_IRON_CHESTPLATE, 1), (
                f"taking the trimmed chestplate should fill the cursor, got {menu.cursor}")
            assert menu.cursor_components.get(COMPONENT_TRIM) == expect, (
                "the cursor must carry the trimmed, not the plain, chestplate: "
                f"{menu.cursor_components}")
            # The advancement flush rides the survival clock, and that clock is a
            # *packet*: the take's own click ticks the session before its handler
            # runs, so the flush lands on the next packet the client sends (its
            # keep-alive reply) rather than in the take's own drain. Poll for it,
            # and read the criterion's *obtained* flag rather than a substring:
            # the packet names every criterion of the advancement either way.
            criterion = (f"armor_trimmed_minecraft:{pattern}"
                         "_armor_trim_smithing_template_smithing_trim")
            deadline = time.time() + 4.0
            criteria = advancement_criteria(packets, ADV_ANY_TRIM)
            while time.time() < deadline and (criteria is None or criteria.get(criterion) != 1):
                # The flush rides the *survival* clock, and that clock is a
                # packet: send one so the session ticks and flushes now, rather
                # than idling until the next keep-alive reply carries it (1.2 s
                # away, across eighteen patterns).
                bot.send(SB_FLYING, bytes([1]))
                packets += menu.pump(0.25)
                criteria = advancement_criteria(packets, ADV_ANY_TRIM)
            assert criteria is not None, "taking a trim must flush update_advancements"
            assert criteria.get(criterion) == 1, (
                f"the {pattern} trim's own criterion must be the one granted, got "
                f"{sorted(k for k, v in criteria.items() if v)}")

            # The eight exclusive patterns also complete
            # trim_with_all_exclusive_armor_patterns, whose criteria are one AND
            # group each; when the last of them lands its progress entry must
            # show all eight obtained.
            if pattern == EXCLUSIVE_TRIM_PATTERNS[-1]:
                challenge = advancement_criteria(packets, ADV_ALL_TRIMS)
                assert challenge is not None, (
                    "the last exclusive pattern must flush the challenge's progress")
                granted = sorted(k for k, v in challenge.items() if v)
                assert len(granted) == len(EXCLUSIVE_TRIM_PATTERNS), (
                    f"the challenge's eight patterns must all be granted, got {granted}")
                print("  the eighth exclusive pattern completed "
                      "adventure/trim_with_all_exclusive_armor_patterns")

            # Put the (re)trimmed chestplate back in the base slot for the next
            # pattern.
            menu.click(SM_BASE, 0, PICKUP)
            assert menu.cursor == (0, 0), f"the cursor should be empty, got {menu.cursor}"
            assert menu.slot(SM_BASE) == (ITEM_IRON_CHESTPLATE, 1), (
                "the chestplate must go back into the base slot for the next trim")
            print(f"  [{pattern_id + 1:2d}/18] {pattern} + {mat} -> trim {expect}")

        # The identical re-trim is a no-op: applyTrim compares the old and the new
        # ArmorTrim and returns an empty stack, so the result slot must stay empty
        # even though the chestplate already wears exactly the template/material
        # pair in the other two slots.
        pattern, pattern_id, template_item = TRIM_PATTERNS[-1]
        mat, mat_id, mat_item, mat_arg = TRIM_MATERIALS[pattern_id % len(TRIM_MATERIALS)]
        slash_give(bot, "CraftBot", f"minecraft:{pattern}_armor_trim_smithing_template", 1)
        slash_give(bot, "CraftBot", mat_arg, 1)
        time.sleep(0.25)
        menu.pump(0.6)
        menu.click(find_item(menu, template_item), 0, QUICK_MOVE)
        menu.click(find_item(menu, mat_item), 0, QUICK_MOVE)
        assert menu.trim(SM_BASE) == (mat_id + 1, pattern_id + 1), (
            f"the base should still wear the {pattern} trim, got {menu.trim(SM_BASE)}")
        assert menu.slot(SM_RESULT) == (0, 0), (
            "re-trimming a piece with the trim it already wears must derive no "
            f"result, got {menu.slot(SM_RESULT)}")
        print(f"  the same {pattern}/{mat} trim on the already-trimmed piece derived "
              "nothing (applyTrim)")

        # Swap the material for another: a different trim is a different craft,
        # so the result comes back with the new material and the same pattern.
        other, other_id, other_item, other_arg = next(
            m for m in TRIM_MATERIALS if m[0] != mat)
        menu.click(SM_ADDITION, 0, PICKUP)              # the leftover material
        assert menu.cursor == (mat_item, 1)
        free = next(i for i in range(SM_MAIN_FIRST, SM_HOTBAR_FIRST)
                    if menu.slot(i) == (0, 0))
        menu.click(free, 0, PICKUP)
        assert menu.cursor == (0, 0)
        slash_give(bot, "CraftBot", other_arg, 1)
        time.sleep(0.25)
        menu.pump(0.6)
        menu.click(find_item(menu, other_item), 0, QUICK_MOVE)
        assert menu.slot(SM_ADDITION) == (other_item, 1), (
            f"the {other} material should be in input 2, got {menu.slot(SM_ADDITION)}")
        assert menu.trim(SM_RESULT) == (other_id + 1, pattern_id + 1), (
            f"a {other} material must re-derive the {pattern} trim, got "
            f"{menu.trim(SM_RESULT)}")
        print(f"  the {other} material replaced the {mat} one "
              f"(trim component {menu.trim(SM_RESULT)})")

        print("OK — crafting and smithing work end to end")
        return 0
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    sys.exit(main())
