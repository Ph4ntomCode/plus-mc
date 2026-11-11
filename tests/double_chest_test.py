#!/usr/bin/env python3
"""Integration test: double chests (Phase 3 milestone C follow-up).

Bot1 places a chest, and the server must write the block's *default* state —
`facing=north,type=single,waterlogged=false` (id 5544) for a player looking
south — rather than the first state of that block in the id table, which is the
waterlogged variant (3987) and renders as a chest full of water.

Bot1 then moves 64 stone into that chest, closes the window, and places a second
chest directly beside it. Both blocks must be re-typed into one double chest
(`type=right` at the new block, `type=left` at the old one), a reopen must be
served as menu `generic_9x6` (5) with 90 menu slots, and the first chest's items
must show up in the *second* half of that menu (vanilla's FIRST = `type=right`,
so the older chest — re-typed to LEFT — owns menu slots 27..53).

Breaking the right half must drop only *its* contents (the dirt) and turn the
left half back into a single chest that still holds its own stone: vanilla drops
the removed block entity's items and re-singles the neighbour.

Layout notes (all verified against the 26.2 codecs, see docs/PHASE3.md):
  * open_screen (0x3b): [varint container id][varint menu id][title component]
  * container_set_content (0x12): [varint id][varint state id][varint count][stacks][cursor]
  * container_set_slot (0x14): [varint id][varint state id][i16 slot][stack]
  * container_close (0x11) / serverbound close_window (0x13): [varint id]
  * block_update (0x08): [i64 position][varint state id]
  * serverbound container_click (0x12): [u8 id][varint state][i16 slot][i8 button]
        [varint ContainerInput][varint changed count][HashedStack carried]
  * serverbound player_action (0x29): [varint action][i64 pos][u8 face][varint seq]
"""
import os, socket, struct, sys, time, tempfile, signal, subprocess, re
import wire_client
from wire_client import make_frame, read_packet

PROTO = 777
HOST = "127.0.0.1"

# Registry ids (generated item table, src/world/item_registry.h).
ITEM_STONE = 1
ITEM_DIRT = 55
ITEM_CHEST = 404

# Packet ids.
SB_SET_CREATIVE_SLOT = 0x39
SB_USE_ITEM_ON = 0x42
SB_CONTAINER_CLICK = 0x12
SB_CLOSE_WINDOW = 0x13
SB_PLAYER_ACTION = 0x29
SB_SET_CARRIED_ITEM = 0x36
CB_KEEP_ALIVE = 0x2D
SB_KEEP_ALIVE = 0x1C
CB_ADD_ENTITY = 0x01
CB_BLOCK_UPDATE = 0x08
CB_CONTAINER_SET_CONTENT = 0x12
CB_CONTAINER_SET_SLOT = 0x14
CB_SET_CURSOR_ITEM = 0x62
CB_SET_ENTITY_DATA = 0x65
CB_CHUNK_DATA = 0x2E
CB_OPEN_SCREEN = 0x3C

# MenuType ids (static registration order). The registry has 25 entries
# (generic_9x1 .. stonecutter, ids 0..24), so any larger id on the wire makes the
# client's open_screen decode throw and drops the connection.
MENU_REGISTRY_SIZE = 25
MENU_GENERIC_9X3 = 2
MENU_GENERIC_9X6 = 5

# Block states of interest (src/world/block_states.h, generated).
CHEST_NORTH_SINGLE = 5544
CHEST_NORTH_LEFT = 5546
CHEST_NORTH_RIGHT = 5548
CHEST_WATERLOGGED_SINGLE = 5543

CHEST_Y = 70


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
    """Decodes open_screen the way ClientboundOpenScreenPacket does.

    Layout: [container id: varint][menu type: varint menu-registry id][title].
    The container id comes FIRST (ByteBufCodecs.CONTAINER_ID). A test that
    mirrors the *server's* field order instead of the client's cannot see a
    swap: every assertion stays self-consistent while a real client reads the
    menu id as its window id (so all its clicks go to a window the server does
    not have) and the window id as its menu type (so it draws whatever screen
    the registry holds at that index, and throws once the id runs past the end
    of the 25-entry registry). Returns (window, menu_id, next_offset).
    """
    window, i = rd_varint(body, 0)
    assert 1 <= window <= 100, (
        f"the container id must stay in vanilla's 1..100 range, got {window}")
    menu, j = rd_varint(body, i)
    assert menu < MENU_REGISTRY_SIZE, (
        f"menu type {menu} is past the end of the {MENU_REGISTRY_SIZE}-entry "
        "menu registry — the client throws while decoding and disconnects")
    return window, menu, j


def position(x, y, z):
    return wire_client.pack_block_pos(x, y, z)


def parse_position(raw):
    v = struct.unpack(">q", raw[:8])[0]
    x = v >> 38
    y = v & 0xFFF
    z = (v >> 12) & 0x3FFFFFF
    if x >= 0x2000000:
        x -= 0x4000000
    if z >= 0x2000000:
        z -= 0x4000000
    if y >= 0x800:
        y -= 0x1000
    return x, y, z


def read_stack(body, pos):
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


def write_stack(item, count):
    if count <= 0:
        return varint(0)
    return varint(count) + varint(item) + varint(0) + varint(0)


def decode_content(body):
    window = body[0]
    state, i = rd_varint(body, 1)
    n, i = rd_varint(body, i)
    stacks = []
    for _ in range(n):
        stack, i = read_stack(body, i)
        stacks.append(stack)
    cursor, i = read_stack(body, i)
    return window, state, stacks, cursor


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
        return read_packet(self.sock, self.threshold)

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
        """Runs the handshake; returns the number of chunk packets consumed.

        The spawn burst is the paced part: the server writes
        PLUSMC_CHUNKS_PER_TICK chunks per 50 ms tick before the batch-finished
        marker, so a caller can time this call to see the pacing.
        """
        chunks = 0
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
        for _ in range(500):
            pid, body = self.recv()
            if pid == CB_KEEP_ALIVE:
                self.send(SB_KEEP_ALIVE, body[:8])
            elif pid == CB_CHUNK_DATA:
                chunks += 1
            elif pid == 0x0B:
                b, _ = rd_varint(body)
                self.send(0x0B, varint(b))
                break
        return chunks


def drain(bot, timeout=1.5):
    """The packets the server sends before it goes quiet, up to `timeout`.

    A container reply is synchronous, so a window here ends when the server has
    been quiet for `quiet` seconds rather than always burning the whole window;
    `timeout` stays the bound a missing reply is judged against. Requires the
    reader (see `bot.start_reader()` after the join).
    """
    return wire_client.drain_quiet(bot, quiet=0.3, limit=timeout)
def find(packets, pid):
    for p, body in packets:
        if p == pid:
            return body
    return None


def block_updates(packets):
    """All block_update packets as {(x, y, z): state_id}."""
    out = {}
    for pid, body in packets:
        if pid != CB_BLOCK_UPDATE:
            continue
        x, y, z = parse_position(body)
        state, _ = rd_varint(body, 8)
        out[(x, y, z)] = state
    return out


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


def click(window, state, slot, button, mode):
    return (varint(window) + varint(state) + struct.pack(">h", slot) + bytes([button])
            + varint(mode) + varint(0) + b"\x00")


def place(bot, x, y, z, face, seq):
    body = (varint(0) + position(x, y, z) + varint(face)
            + struct.pack(">f", 0.5) + struct.pack(">f", 0.0) + struct.pack(">f", 0.5)
            + bytes([0]) + bytes([0]) + varint(seq))
    bot.send(SB_USE_ITEM_ON, body)


def give(bot, menu_slot, item, count):
    bot.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", menu_slot) + write_stack(item, count))


def reopen(bot, x, y, z, seq):
    """Right-clicks a container and returns (window, state, slots).

    A creative slot write can only come from the creative inventory screen, so
    it answers on the *player menu* and drops the server's view of any open
    container — exactly what a player does by opening their inventory. A bot
    that uses give() to fill its hotbar therefore has to open the container
    again before clicking in it.
    """
    place(bot, x, y, z, 1, seq)
    packets = drain(bot, 2.0)
    opened = find(packets, CB_OPEN_SCREEN)
    assert opened is not None, "the container did not reopen"
    window, _, _ = parse_open_screen(opened)
    content = find(packets, CB_CONTAINER_SET_CONTENT)
    assert content is not None, "open_screen without content"
    cw, state, slots, _ = decode_content(content)
    assert cw == window, f"content window {cw} != open window {window}"
    return window, state, slots


def dig(bot, x, y, z, seq=99):
    bot.send(SB_PLAYER_ACTION, varint(2) + position(x, y, z) + varint(0) + varint(seq))


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-double-chest-")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "1200"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "4000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_AUTOSAVE_SECS"] = "0"
    # The survival tick counts packets (one tick each, no wall-time catch-up), so
    # a drain banks no ticks. A double chest's rules are all states and writes —
    # the two halves, the shared inventory, the slot count — and none of them is
    # a survival-tick duration.
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"

    proc = None
    bot = None
    try:
        proc, port = wire_client.start_server(
            [binary, "--host", "127.0.0.1", "--port", "0", "--motd", "double chest test",
             "--offline-mode", "--gamemode", "1", "--data", data_root,
             "--world", world, "--no-flat-world"], wait_listening, env=env)
        print(f"world dir: {world}")

        # The spawn burst is paced (PLUSMC_CHUNKS_PER_TICK chunks per 50 ms
        # tick), so the whole view distance must arrive in more than one tick
        # but still be complete: 17x17 columns at the default 9 per tick is 32
        # ticks. A burst that lands instantly would be the old behaviour, where
        # the client has to parse, light and mesh all 289 chunks in one go.
        bot = Bot("ChestBot", port)
        wire_client.record(bot, note="double-chest: two halves, one inventory, the join burst")
        t0 = time.time()
        chunks = bot.join()
        bot.start_reader()
        burst_secs = time.time() - t0
        assert chunks == 17 * 17, f"the spawn burst must cover the view distance, got {chunks}"
        assert burst_secs >= 1.3, (
            f"the 289-chunk burst must be paced, arrived in {burst_secs:.2f}s")
        print(f"  spawn burst: {chunks} chunks paced over {burst_secs:.1f}s")
        drain(bot, 1.0)
        # Stand on top of the chest pair this bot places: feet at CHEST_Y + 1,
        # between the two cells (0, CHEST_Y, 0) and (1, CHEST_Y, 0).
        bot.move_to(0.5, CHEST_Y + 1.0, 0.5)
        time.sleep(0.2)

        # --- 1. one chest goes down on its vanilla default state -------------
        give(bot, 36, ITEM_CHEST, 4)
        time.sleep(0.2)
        place(bot, 0, CHEST_Y - 1, 0, 1, 1)  # target (0, CHEST_Y, 0)
        packets = drain(bot, 2.0)
        updates = block_updates(packets)
        assert (0, CHEST_Y, 0) in updates, f"no block update for the chest: {updates}"
        got = updates[(0, CHEST_Y, 0)]
        assert got == CHEST_NORTH_SINGLE, (
            f"a placed chest must be facing=north,type=single,waterlogged=false "
            f"({CHEST_NORTH_SINGLE}), got {got}"
            + (" (the waterlogged variant!)" if got == CHEST_WATERLOGGED_SINGLE else ""))
        print(f"  chest placed at (0,{CHEST_Y},0) with state {got} (not waterlogged)")

        # --- 2. put 64 stone into it ---------------------------------------
        place(bot, 0, CHEST_Y, 0, 1, 2)
        packets = drain(bot, 2.0)
        opened = find(packets, CB_OPEN_SCREEN)
        assert opened is not None, "the chest did not open"
        _, menu_id, _ = parse_open_screen(opened)
        assert menu_id == MENU_GENERIC_9X3, f"a single chest must be 9x3 (2), got {menu_id}"
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "open_screen without content"
        _, state, slots, _ = decode_content(content)
        assert len(slots) == 27 + 36, f"a single chest menu has 63 slots, got {len(slots)}"
        give(bot, 36, ITEM_STONE, 64)
        packets = drain(bot, 1.5)
        upd = find(packets, CB_CONTAINER_SET_SLOT)
        assert upd is not None, "the stone give produced no slot update"
        assert upd[0] == 0, "a creative slot write always answers on window 0"
        window, state, _ = reopen(bot, 0, CHEST_Y, 0, 2)  # the give dropped our window
        bot.send(SB_CONTAINER_CLICK, click(window, state, 54, 0, 1))  # QUICK_MOVE hotbar 0
        packets = drain(bot, 2.0)
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "shift-click must resync the menu"
        _, state, slots, _ = decode_content(content)
        assert slots[0] == (ITEM_STONE, 64), f"the stone must land in slot 0, got {slots[0]}"
        print("  64 stone shift-clicked into slot 0")

        # --- 3. closing with an item on the cursor returns it to the player ---
        # (creative clone: mode 3 puts a full stack on the cursor)
        bot.send(SB_CONTAINER_CLICK, click(window, state , 0, 0, 0))  # PICKUP slot 0
        packets = drain(bot, 2.0)
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is None, "a pickup is answered with slot updates, not a full resync"
        cursor = None
        for pid, body in packets:
            if pid == CB_SET_CURSOR_ITEM:
                cursor, _ = read_stack(body, 0)
        assert cursor == (ITEM_STONE, 64), f"the stone must be on the cursor, got {cursor}"
        bot.send(SB_CLOSE_WINDOW, varint(window))
        packets = drain(bot, 2.0)
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "closing with a carried stack must resync the player menu"
        cw, _, slots, cursor = decode_content(content)
        assert cw == 0, f"the resync after closing goes to the player menu (0), got {cw}"
        assert cursor == (0, 0), f"the cursor must be empty after closing, got {cursor}"
        assert (ITEM_STONE, 64) in slots, "the carried stone must be back in the inventory"
        print("  closing with a carried stack returned it to the inventory")
        # Put it back in the chest so the double chest has contents in its first
        # half (the *older* chest becomes the second half of the pair).
        place(bot, 0, CHEST_Y, 0, 1, 3)
        packets = drain(bot, 2.0)
        opened = find(packets, CB_OPEN_SCREEN)
        assert opened is not None, "the chest did not reopen"
        window, _, _ = parse_open_screen(opened)
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        _, state, slots, _ = decode_content(content)
        stone_slot = slots.index((ITEM_STONE, 64))
        bot.send(SB_CONTAINER_CLICK, click(window, state, stone_slot, 0, 1))  # QUICK_MOVE
        packets = drain(bot, 2.0)
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "moving the stone back must resync"
        _, state, slots, _ = decode_content(content)
        assert slots[0] == (ITEM_STONE, 64), f"stone back in chest slot 0, got {slots[0]}"

        # --- 4. place the second chest beside it: both halves re-type -------
        # (hotbar 0 is empty again after the stone dance: refill it)
        give(bot, 36, ITEM_CHEST, 4)
        time.sleep(0.2)
        place(bot, 1, CHEST_Y - 1, 0, 1, 4)  # target (1, CHEST_Y, 0)
        packets = drain(bot, 2.0)
        updates = block_updates(packets)
        assert (1, CHEST_Y, 0) in updates, f"no block update for the second chest: {updates}"
        assert updates[(1, CHEST_Y, 0)] == CHEST_NORTH_RIGHT, (
            f"the new chest must become type=right ({CHEST_NORTH_RIGHT}), "
            f"got {updates[(1, CHEST_Y, 0)]}")
        assert (0, CHEST_Y, 0) in updates, (
            f"the first chest must be re-typed into the pair: {updates}")
        assert updates[(0, CHEST_Y, 0)] == CHEST_NORTH_LEFT, (
            f"the first chest must become type=left ({CHEST_NORTH_LEFT}), "
            f"got {updates[(0, CHEST_Y, 0)]}")
        print("  both chests joined into one double chest (right + left)")

        # --- 5. either half opens the combined 9x6 menu ---------------------
        place(bot, 0, CHEST_Y, 0, 1, 5)
        packets = drain(bot, 2.0)
        opened = find(packets, CB_OPEN_SCREEN)
        assert opened is not None, "the double chest did not open"
        window, menu_id, _ = parse_open_screen(opened)
        assert menu_id == MENU_GENERIC_9X6, (
            f"a double chest must be generic_9x6 (5), got {menu_id}")
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "open_screen without content"
        _, state, slots, _ = decode_content(content)
        assert len(slots) == 54 + 36, f"a double chest menu has 90 slots, got {len(slots)}"
        # The older chest became type=left = the *second* half (menu 27..53).
        assert slots[27] == (ITEM_STONE, 64), (
            f"the first chest's stone must show at menu slot 27, got {slots[27]}")
        assert slots[0] == (0, 0), "the new (right) half starts empty"
        print(f"  double chest opened: menu {menu_id}, {len(slots)} slots, stone at 27")

        # --- 5b. the other half is the *same* window, not a new one ----------
        # Both halves are one container, so right-clicking the far side must not
        # close and reopen anything: the client's menu already is this chest.
        # A close here is not cosmetic — the client applies it to whatever
        # screen it has, and a close+open pair also widens the gap in which the
        # client has no screen at all.
        place(bot, 1, CHEST_Y, 0, 1, 5)
        packets = drain(bot, 2.0)
        assert find(packets, CB_OPEN_SCREEN) is None, (
            "the other half of a double chest must not open a new window: "
            f"{[(hex(p)) for p, _ in packets]}")
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "the other half must re-state the same menu"
        cw, _, slots, _ = decode_content(content)
        assert cw == window, f"the other half must re-state window {window}, got {cw}"
        assert slots[27] == (ITEM_STONE, 64), (
            f"the contents must not move when switching halves, got {slots[27]}")
        print("  other half re-states the same window (no close, no new open)")

        # And after a close, reopening at the other half is still the *same*
        # container: one 9x6 menu, the stone on the same menu slot, neither half
        # empty and neither half showing the other's items.
        bot.send(SB_CLOSE_WINDOW, varint(window))
        drain(bot, 1.0)
        window, state, slots = reopen(bot, 1, CHEST_Y, 0, 6)
        assert len(slots) == 54 + 36, f"still one double chest menu, got {len(slots)}"
        assert slots[27] == (ITEM_STONE, 64), (
            f"the stone must stay at menu slot 27 when opened from the other half, "
            f"got {slots[27]}")
        assert slots[0] == (0, 0), f"the right half is still empty, got {slots[0]}"
        print("  reopening at the other half keeps one container and one layout")

        # A shift-click fills the *first* half (the right/new chest).
        give(bot, 36, ITEM_DIRT, 64)
        packets = drain(bot, 1.5)
        upd = find(packets, CB_CONTAINER_SET_SLOT)
        assert upd is not None, "the dirt give produced no slot update"
        assert upd[0] == 0, "a creative slot write always answers on window 0"
        window, state, _ = reopen(bot, 0, CHEST_Y, 0, 7)
        hotbar_menu = 54 + 27  # the double chest's hotbar starts at menu slot 81
        bot.send(SB_CONTAINER_CLICK, click(window, state, hotbar_menu, 0, 1))
        packets = drain(bot, 2.0)
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "the dirt shift-click must resync"
        _, state, slots, _ = decode_content(content)
        assert slots[0] == (ITEM_DIRT, 64), (
            f"shift-click must fill the first half (menu slot 0), got {slots[0]}")
        assert slots[27] == (ITEM_STONE, 64), "the stone must stay in slot 27"
        print("  64 dirt shift-clicked into the first (right) half")

        # --- 6. break the right half: only its contents drop -----------------
        bot.send(SB_CLOSE_WINDOW, varint(window))
        drain(bot, 1.0)
        dig(bot, 1, CHEST_Y, 0)
        packets = drain(bot, 2.5)
        updates = block_updates(packets)
        assert updates.get((1, CHEST_Y, 0)) == 0, (
            f"the broken half must be air, got {updates.get((1, CHEST_Y, 0))}")
        assert updates.get((0, CHEST_Y, 0)) == CHEST_NORTH_SINGLE, (
            f"the surviving chest must become single ({CHEST_NORTH_SINGLE}), "
            f"got {updates.get((0, CHEST_Y, 0))}")
        drops = []
        for pid, body in packets:
            if pid != CB_ADD_ENTITY:
                continue
            _, i = rd_varint(body, 0)   # entity id
            i += 16                     # uuid
            etype, i = rd_varint(body, i)
            x, y, z = struct.unpack(">ddd", body[i:i + 24])
            drops.append((etype, x, y, z))
        assert drops, "breaking a non-empty half must drop its contents"
        assert len(drops) == 1, f"only the broken half's stack drops, got {len(drops)}"
        etype, x, y, z = drops[0]
        assert etype == 72, f"the drop must be an item entity (72), got {etype}"
        assert abs(x - 1.5) < 0.6 and abs(z - 0.5) < 0.6, (
            f"the drop must land at the broken half, got ({x},{y},{z})")
        # ...and it must be the dirt that lived in that half, not the stone.
        md = find(packets, CB_SET_ENTITY_DATA)
        assert md is not None, "the drop must carry its item stack"
        _, i = rd_varint(md, 0)         # entity id
        assert md[i] == 8, f"DATA_ITEM is synched index 8, got {md[i]}"
        i += 1
        ser, i = rd_varint(md, i)
        assert ser == 7, f"the item stack uses serializer 7, got {ser}"
        stack, _ = read_stack(md, i)
        assert stack == (ITEM_DIRT, 64), f"the broken half held dirt, got {stack}"
        print("  right half broke: 64 dirt dropped, left half is single")

        # --- 7. the surviving chest kept its own stone ------------------------
        place(bot, 0, CHEST_Y, 0, 1, 6)
        packets = drain(bot, 2.0)
        opened = find(packets, CB_OPEN_SCREEN)
        assert opened is not None, "the surviving chest did not open"
        _, menu_id, _ = parse_open_screen(opened)
        assert menu_id == MENU_GENERIC_9X3, (
            f"the surviving chest is single again: expect 9x3 (2), got {menu_id}")
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "open_screen without content"
        _, _, slots, _ = decode_content(content)
        assert len(slots) == 63, f"a single chest menu has 63 slots, got {len(slots)}"
        assert slots[0] == (ITEM_STONE, 64), (
            f"the surviving half must keep its 64 stone, got {slots[0]}")
        print("  surviving chest still holds its own 64 stone")

        bot.close()
        bot = None
        proc.send_signal(signal.SIGTERM)
        proc.wait(timeout=15)
        proc = None
    finally:
        if bot is not None:
            bot.close()
        if proc is not None:
            try:
                proc.send_signal(signal.SIGTERM)
                proc.wait(timeout=10)
            except (subprocess.TimeoutExpired, OSError):
                proc.kill()

    print("\nPASS: double chest test")
    return 0


if __name__ == "__main__":
    sys.exit(main())
