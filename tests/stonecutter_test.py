#!/usr/bin/env python3
"""Integration test: the stonecutter menu (Phase 3 milestone D).

The stonecutter is the one menu whose *buttons* matter: the client builds the
list of recipes the input makes, the player picks one, and the client sends
`container_button_click` (0x11) carrying the index. StonecutterMenu
.clickMenuButton then stores it in a DataSlot (which the server pushes back as
`container_set_data` 0x13) and re-derives the result slot.

A bot places a stonecutter, opens it (menu 24), puts a stone in the input, and:
  * picks the first recipe in the list -> the result slot must hold that
    recipe's output, the selection must be echoed as data slot 0, and the input
    must be untouched,
  * picks an index past the end of the list -> nothing moves (the range check),
  * takes the result -> the input is consumed *and* the selection is dropped
    (a different input item is a different list), which the client learns from a
    data slot of -1 — the reset that keeps a stale index from paying out a
    recipe the new input does not make.

Layout notes (26.2 codecs, see docs/PHASE3.md):
  * open_screen (0x3b): [varint container id][varint menu id][title component]
    — MenuType.stonecutter is 24, the last of the 25 menu-registry entries.
  * stonecutter menu slots: 0 input, 1 result, 2..28 main, 29..37 hotbar (38).
  * serverbound container_button_click (0x11): [varint container id][varint button id]
"""
import os, socket, struct, sys, time, subprocess, re, tempfile, shutil

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nbt_component import chat_text  # noqa: E402
import wire_client
from wire_client import make_frame, read_packet

PROTO = 777
HOST = "127.0.0.1"

# Registry ids (generated item table, src/world/item_registry.h).
ITEM_STONE = 1
ITEM_CHISELED_STONE_BRICKS = 452
ITEM_STONECUTTER = 1513

SB_SET_CREATIVE_SLOT = 0x39
SB_USE_ITEM_ON = 0x42
SB_CONTAINER_CLICK = 0x12
SB_CONTAINER_BUTTON_CLICK = 0x11
CB_BLOCK_UPDATE = 0x08
CB_CONTAINER_SET_CONTENT = 0x12
CB_CONTAINER_SET_DATA = 0x13
CB_CONTAINER_SET_SLOT = 0x14
CB_OPEN_SCREEN = 0x3C
CB_SET_CURSOR_ITEM = 0x62
CB_KEEP_ALIVE = 0x2D
SB_KEEP_ALIVE = 0x1C

MENU_STONECUTTER = 24
MENU_SLOTS = 38
SLOT_INPUT, SLOT_RESULT = 0, 1
SLOT_HOTBAR_FIRST = 29
# The one DataSlot StonecutterMenu declares.
DATA_SELECTED_RECIPE = 0


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
    return wire_client.pack_block_pos(x, y, z)


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


def hashed_stack(item, count):
    if count <= 0:
        return b"\x00"
    return bytes([1]) + varint(item) + varint(count) + varint(0) + varint(0)


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
    seconds."""
    return wire_client.drain(bot, timeout)
def find(packets, pid):
    for p, body in packets:
        if p == pid:
            return body
    return None


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


def decode_content(body):
    window, i = rd_varint(body, 0)
    state, i = rd_varint(body, i)
    n, i = rd_varint(body, i)
    stacks = []
    for _ in range(n):
        stack, i = read_stack(body, i)
        stacks.append(stack)
    cursor, i = read_stack(body, i)
    return window, state, stacks, cursor


def click_body(window, state, slot, button, mode, carried=(0, 0)):
    return (varint(window) + varint(state) + struct.pack(">h", slot) + bytes([button])
            + varint(mode) + varint(0) + hashed_stack(*carried))


def place(bot, x, y, z, face, seq):
    body = (varint(0) + position(x, y, z) + varint(face)
            + struct.pack(">f", 0.5) + struct.pack(">f", 0.0) + struct.pack(">f", 0.5)
            + bytes([0]) + bytes([0]) + varint(seq))
    bot.send(SB_USE_ITEM_ON, body)


def give(bot, menu_slot, item, count):
    bot.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", menu_slot) + write_stack(item, count))


def slot_updates(packets):
    """[(slot, (item, count))] out of the container_set_slot packets."""
    out = []
    for p, b in packets:
        if p != CB_CONTAINER_SET_SLOT:
            continue
        _, i = rd_varint(b, 0)
        _, i = rd_varint(b, i)
        slot = struct.unpack(">h", b[i:i + 2])[0]
        stack, _ = read_stack(b, i + 2)
        out.append((slot, stack))
    return out


def state_of(packets, fallback):
    """The container revision the client would hold afterwards: the newest one
    the server tagged a window packet with (a resync carries one too)."""
    state = fallback
    content = find(packets, CB_CONTAINER_SET_CONTENT)
    if content is not None:
        _, state, _, _ = decode_content(content)
    for p, b in packets:
        if p != CB_CONTAINER_SET_SLOT:
            continue
        _, i = rd_varint(b, 0)
        s, _ = rd_varint(b, i)
        state = s
    return state


def cursor_updates(packets):
    """The stack each set_cursor_item (0x60) carried."""
    out = []
    for p, b in packets:
        if p != CB_SET_CURSOR_ITEM:
            continue
        stack, _ = read_stack(b, 0)
        out.append(stack)
    return out


def data_updates(packets):
    """{data slot: value} out of the container_set_data packets."""
    out = {}
    for p, b in packets:
        if p != CB_CONTAINER_SET_DATA:
            continue
        _, i = rd_varint(b, 0)
        ds, val = struct.unpack(">hh", b[i:i + 4])
        out[ds] = val
    return out


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-stonecutter-")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "1200"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "4000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    # The survival tick counts packets (one tick each, no wall-time catch-up), so
    # a drain banks no ticks. A stonecutter asserts its menu's recipes and the
    # result stack they produce — states and writes, not durations.
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
    STONECUTTER_Y = 70

    proc = None
    try:
        print(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", "127.0.0.1", "--port", "0", "--motd", "stonecutter test",
             "--offline-mode", "--gamemode", "1", "--data", data_root,
             "--world", world, "--no-flat-world"], wait_listening, env=env)

        bot = Bot("CutterBot", port)
        wire_client.record(bot, note="stonecutter: the menu, its recipe list and the result")
        bot.join()
        drain(bot, 1.0)
        print("  joined")
        # Stand on top of the stonecutter this bot places: feet at
        # STONECUTTER_Y + 1, so both the block below it and the block itself are
        # inside the interaction range.
        bot.move_to(0.5, STONECUTTER_Y + 1.0, 0.5)
        time.sleep(0.2)

        # Creative writes drop any open window, so the input stack goes into the
        # hotbar *before* the menu is opened.
        give(bot, 36, ITEM_STONECUTTER, 1)  # hotbar 0: what we place
        give(bot, 37, ITEM_STONE, 4)        # hotbar 1
        drain(bot, 0.5)
        place(bot, 0, STONECUTTER_Y - 1, 0, 1, 1)
        packets = drain(bot, 2.0)
        assert find(packets, CB_BLOCK_UPDATE) is not None, "placing the stonecutter produced no block update"
        print("  stonecutter placed")

        place(bot, 0, STONECUTTER_Y, 0, 1, 2)
        packets = drain(bot, 2.0)
        opened = find(packets, CB_OPEN_SCREEN)
        assert opened is not None, f"no open_screen: {[hex(p) for p, _ in packets]}"
        window, i = rd_varint(opened, 0)
        menu_id, _ = rd_varint(opened, i)
        assert menu_id == MENU_STONECUTTER, f"expected the stonecutter menu (24), got {menu_id}"
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "open_screen without container_set_content"
        cw, state, slots, cursor = decode_content(content)
        assert cw == window == 1, f"first window must be 1, got {cw}/{window}"
        assert len(slots) == MENU_SLOTS, f"stonecutter menu is 38 slots, got {len(slots)}"
        assert slots[SLOT_INPUT] == (0, 0) and slots[SLOT_RESULT] == (0, 0), "a fresh stonecutter is empty"
        assert slots[SLOT_HOTBAR_FIRST + 1] == (ITEM_STONE, 4), f"hotbar 1: {slots[SLOT_HOTBAR_FIRST + 1]}"
        print(f"  opened window {window} (menu {menu_id}), stone in hotbar 1")

        # Put the stone in the input: PICKUP the hotbar stack, then PICKUP it
        # into slot 0. The client reports the cursor it believes it holds.
        # Click helper: send, drain, and follow the container revision the
        # server moved to (a real client does the same with every packet).
        def click(slot, button=0, mode=0, carried=(0, 0)):
            nonlocal state
            bot.send(SB_CONTAINER_CLICK, click_body(window, state, slot, button, mode, carried))
            packets = drain(bot, 0.6)
            state = state_of(packets, state)
            return packets

        packets = click(SLOT_HOTBAR_FIRST + 1)
        ups = slot_updates(packets)
        assert (SLOT_HOTBAR_FIRST + 1, (0, 0)) in ups, f"the hotbar stack should be on the cursor: {ups}"
        packets = click(SLOT_INPUT, carried=(ITEM_STONE, 4))
        ups = slot_updates(packets)
        assert (SLOT_INPUT, (ITEM_STONE, 4)) in ups, (
            f"the stone should be in the input, and the client told about it: {ups}")
        print("  4 stone in the input")

        # --- picking a recipe: the first one the list offers ------------------
        bot.send(SB_CONTAINER_BUTTON_CLICK, varint(window) + varint(0))
        packets = drain(bot, 1.0)
        state = state_of(packets, state)
        ups = dict(slot_updates(packets))
        data = data_updates(packets)
        assert data.get(DATA_SELECTED_RECIPE) == 0, (
            f"the picked recipe index must be echoed in the menu's data slot: {data}")
        assert ups.get(SLOT_RESULT) == (ITEM_CHISELED_STONE_BRICKS, 1), (
            f"recipe 0 for stone is chiseled stone bricks: {ups}")
        assert SLOT_INPUT not in ups or ups[SLOT_INPUT] == (ITEM_STONE, 4), (
            f"selecting a recipe does not consume the input: {ups}")
        print("  picked recipe 0 -> chiseled stone bricks in the result slot")

        # An index past the end of the list is ignored (clickMenuButton's range
        # check) and the result stays where it was.
        bot.send(SB_CONTAINER_BUTTON_CLICK, varint(window) + varint(999))
        packets = drain(bot, 1.0)
        assert state_of(packets, state) == state, "an ignored button must not move the revision"
        assert SLOT_RESULT not in dict(slot_updates(packets)), (
            f"an out-of-range button must change nothing: {slot_updates(packets)}")
        assert DATA_SELECTED_RECIPE not in data_updates(packets), (
            f"an out-of-range button must not move the selection: {data_updates(packets)}")
        print("  button 999 ignored")

        # --- taking the result -------------------------------------------------
        packets = click(SLOT_RESULT)
        ups = dict(slot_updates(packets))
        data = data_updates(packets)
        # The take puts the output on the cursor, consumes one input item and
        # re-derives the result — the input still holds the *same* item, so the
        # selection survives and the client is shown the output again.
        assert cursor_updates(packets) == [(ITEM_CHISELED_STONE_BRICKS, 1)], (
            f"the taken item must be on the cursor: {cursor_updates(packets)}")
        assert ups.get(SLOT_INPUT) == (ITEM_STONE, 3), (
            f"taking the result consumes one input item and the client must be told: {ups}")
        assert ups.get(SLOT_RESULT) == (ITEM_CHISELED_STONE_BRICKS, 1), (
            f"three stone left still make the selected recipe: {ups}")
        assert DATA_SELECTED_RECIPE not in data, (
            f"the same input item keeps the selection: {data}")
        print("  took the result; input down to 3 stone, result re-derived")

        # Keep taking: the cursor collects the bricks and each take carries it,
        # until the input runs out. Emptying the input is a *different* item
        # (air), so StonecutterMenu.setupRecipeList drops the selection and the
        # client is told (data slot -1) — otherwise it would keep showing a
        # recipe an empty stonecutter no longer makes.
        dropped = False
        for taken in range(2, 6):
            packets = click(SLOT_RESULT, carried=(ITEM_CHISELED_STONE_BRICKS, taken - 1))
            ups = dict(slot_updates(packets))
            data = data_updates(packets)
            if ups.get(SLOT_INPUT) == (0, 0):
                assert data.get(DATA_SELECTED_RECIPE) == -1, (
                    f"an emptied input must drop the selection: {data} (slots {ups})")
                assert ups.get(SLOT_RESULT) == (0, 0), f"nothing left to make: {ups}"
                assert cursor_updates(packets) == [(ITEM_CHISELED_STONE_BRICKS, taken)], (
                    f"the cursor keeps collecting the output: {cursor_updates(packets)}")
                dropped = True
                break
            assert ups.get(SLOT_INPUT) == (ITEM_STONE, 4 - taken), (
                f"each take consumes one input item: {ups}")
        assert dropped, "the input should have been emptied by the takes"
        print("  emptied the input: selection dropped (data slot -1)")

        bot.close()
        print("PASS stonecutter")
        return 0
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(world, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
