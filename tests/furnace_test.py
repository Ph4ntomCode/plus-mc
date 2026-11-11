#!/usr/bin/env python3
"""Integration test: the furnace family (Phase 3 milestone E).

A creative bot places a furnace, opens it, and moves three raw iron into the
input slot and two coal into the fuel slot with ordinary container clicks. The
server must then actually *cook*, over the real wire protocol:

  * `container_set_data` (0x13) for the four data slots AbstractFurnaceMenu
    declares — flame ticks remaining / total, cook ticks spent / total — sent
    when the window opens and again as the counters move,
  * `block_update` (0x08) when the furnace's `lit` property flips, so the
    client draws the burning model,
  * `container_set_slot` (0x14) when the input is consumed and the result
    appears, each tagged with a fresh container revision,
  * the experience: taking the output drops experience *orbs* — real entities,
    spawned with their value in synched data — which are pulled to the player,
    collected (`take_item_entity`) and only then move the bar. The bar must
    therefore still read zero at the moment of the take, and `/xp query @s
    points` must report the collected total afterwards.

The cook is driven by the server's tick loop, which advances 20 ticks a second;
PLUSMC_TICK_SCALE multiplies that so the test does not have to sit through ten
seconds of real smelting.

Layout notes (verified against the 26.2 codecs, see docs/PHASE3.md):
  * open_screen (0x3b): [varint container id][varint menu id][title component]
    — MenuType.furnace is 14, blast_furnace 10, smoker 22.
  * container_set_content (0x12): [varint id][varint state][varint count][stacks][cursor]
  * container_set_slot (0x14): [varint id][varint state][i16 slot][stack]
  * container_set_data (0x13): [varint id][i16 data slot][i16 value]
  * serverbound container_click (0x12): [varint id][varint state][i16 slot][i8 button]
        [varint ContainerInput][varint changed count][HashedStack carried]
  * FurnaceMenu slots: 0 input, 1 fuel, 2 result, 3..29 main, 30..38 hotbar.
"""
import os, socket, struct, sys, time, subprocess, re, tempfile, shutil

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nbt_component import chat_text  # noqa: E402
import wire_client
from wire_client import make_frame, read_packet

PROTO = 777
HOST = "127.0.0.1"

# Registry ids (generated item table, src/world/item_registry.h).
ITEM_RAW_IRON = 1017
ITEM_IRON_INGOT = 1018
ITEM_COAL = 1010
ITEM_COAL_BLOCK = 112
ITEM_FURNACE = 407

# Packet ids.
SB_SET_CREATIVE_SLOT = 0x39
SB_USE_ITEM_ON = 0x42
SB_CONTAINER_CLICK = 0x12
CB_BLOCK_UPDATE = 0x08
CB_CONTAINER_SET_CONTENT = 0x12
CB_CONTAINER_SET_DATA = 0x13
CB_CONTAINER_SET_SLOT = 0x14
CB_OPEN_SCREEN = 0x3C
CB_SYSTEM_CHAT = 0x7C
CB_KEEP_ALIVE = 0x2D
SB_KEEP_ALIVE = 0x1C
# Experience orbs (milestone E follow-up): the entities the payout arrives as.
CB_SPAWN_ENTITY = 0x01
CB_ENTITY_POSITION_SYNC = 0x23
CB_SET_ENTITY_DATA = 0x65
CB_SET_EXPERIENCE = 0x69
CB_TAKE_ITEM = 0x7F
ENTITY_TYPE_EXPERIENCE_ORB = 50
SB_MOVE_POS = 0x1E
SB_SET_CARRIED_ITEM = 0x36
ITEM_STONE = 1

MENU_FURNACE = 14
FURNACE_SLOTS = 39
SLOT_INPUT, SLOT_FUEL, SLOT_RESULT = 0, 1, 2
SLOT_MAIN_FIRST = 3
SLOT_HOTBAR_FIRST = SLOT_MAIN_FIRST + 27  # 30

COOK_TICKS = 200       # smelting one raw iron
COAL_BLOCK_TICKS = 16000
TICK_SCALE = 20        # PLUSMC_TICK_SCALE: 20 game ticks per real tick


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
    window, i = rd_varint(body, 0)
    menu, j = rd_varint(body, i)
    assert 1 <= window <= 100, f"window id {window} outside vanilla's 1..100"
    assert menu < 25, f"menu type {menu} is past the end of the menu registry"
    return window, menu, j


def position(x, y, z):
    # x<<38 | z<<12 | y, packed as an *unsigned* 64-bit word: a negative x sets
    # the top bits, which no signed pack would take (the floor the test lays is
    # 5x5, so x and z go negative).
    packed = (((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF)) & 0xFFFFFFFFFFFFFFFF
    return struct.pack(">Q", packed)


def read_stack(body, pos):
    """Returns ((item_id, count), new_pos). count 0 = empty stack."""
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
    """An ItemStack: [varint count][varint item][components]."""
    if count <= 0:
        return varint(0)
    return varint(count) + varint(item) + varint(0) + varint(0)


def hashed_stack(item, count):
    """A HashedStack (what a container_click carries): [u8 present][varint item]
    [varint count][varint added]{[varint type][i32 hash]}[varint removed]...
    — a *different* layout from ItemStack, and the presence flag is a byte, not
    a varint."""
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
        self.entity_id = 0

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
            pid, body = self.recv()
            if pid == 0x32:
                # ClientboundLoginPacket starts with [i32 entity id], which the
                # orb packets name as the collecting player.
                self.entity_id = struct.unpack(">i", body[:4])[0]
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


def all_of(packets, pid):
    return [body for p, body in packets if p == pid]


def orb_spawns(packets):
    """[(entity id, value)] for every experience orb spawned, pairing each Add
    Entity with the Set Entity Data that carries its value (the value arrives in
    a later packet, so the two are collected in two passes)."""
    values = {}
    for body in all_of(packets, CB_SET_ENTITY_DATA):
        eid, i = rd_varint(body, 0)
        index = body[i]
        i += 1
        serializer, i = rd_varint(body, i)
        raw = struct.unpack(">i", body[i:i + 4])[0]
        if index == 8 and serializer == 1:  # DATA_VALUE, the Int serializer
            values[eid] = raw
    out = []
    for body in all_of(packets, CB_SPAWN_ENTITY):
        eid, i = rd_varint(body, 0)
        i += 16  # uuid
        etype, i = rd_varint(body, i)
        if etype == ENTITY_TYPE_EXPERIENCE_ORB:
            out.append((eid, values.get(eid, 0)))
    return out


def take_item_entity(body):
    """take_item_entity (0x7c): [varint entity][varint player][varint amount]."""
    eid, i = rd_varint(body, 0)
    player, i = rd_varint(body, i)
    amount, i = rd_varint(body, i)
    return (eid, player, amount)


def read_set_experience(body):
    """set_experience (0x67): [f32 progress][varint level][varint total]."""
    progress = struct.unpack(">f", body[0:4])[0]
    level, i = rd_varint(body, 4)
    total, i = rd_varint(body, i)
    return (progress, level, total)


def read_position_sync(body):
    """sync_entity_position (0x23, 26.3): [varint entity][varint PositionPath
    type = LINEAR 0][f64 x/y/z][f32 yRot][f32 xRot][u8 onGround]."""
    eid, i = rd_varint(body, 0)
    path_type, i = rd_varint(body, i)
    assert path_type == 0, f"position_sync path type {path_type}"
    x, y, z = struct.unpack(">ddd", body[i:i + 24])
    return (eid, x, y, z)


def read_block_update_pos(body):
    """block_update (0x08): [i64 packed position][varint state], the position
    packed x<<38 | z<<12 | y with each field sign-extended from its width."""
    raw = struct.unpack(">q", body[0:8])[0]
    x = raw >> 38
    z = (raw >> 12) & 0x3FFFFFF
    y = raw & 0xFFF
    if z >= 0x2000000:
        z -= 0x4000000
    if y >= 0x800:
        y -= 0x1000
    return (x, y, z)


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


def decode_content(body):
    """Decodes container_set_content -> (window, state, [stacks], cursor)."""
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
    """A serverbound container_click.

    `carried` is the stack the *client* believes it holds on the cursor. The
    server compares it against its own and resyncs on a mismatch — the same
    check that catches a client whose view is stale — so a bot that always
    claimed an empty cursor could never make a second click in a row.
    """
    return (varint(window) + varint(state) + struct.pack(">h", slot) + bytes([button])
            + varint(mode) + varint(0) + hashed_stack(*carried))


def place(bot, x, y, z, face, seq):
    body = (varint(0) + position(x, y, z) + varint(face)
            + struct.pack(">f", 0.5) + struct.pack(">f", 0.0) + struct.pack(">f", 0.5)
            + bytes([0]) + bytes([0]) + varint(seq))
    bot.send(SB_USE_ITEM_ON, body)


def give(bot, menu_slot, item, count):
    bot.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", menu_slot) + write_stack(item, count))


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-furnace-")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "1200"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "4000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_TICK_SCALE"] = str(TICK_SCALE)
    # The smelt is the *world* clock (`serverTick`, compressed by
    # PLUSMC_TICK_SCALE); the survival tick counts packets instead of wall time
    # (one tick each, no catch-up) so the bot's own clock cannot race the flame
    # and the progress the assertions read. The bot is creative: nothing here is
    # a survival-tick duration.
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
    FURNACE_Y = 70

    proc = None
    try:
        print(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", "127.0.0.1", "--port", "0", "--motd", "furnace test",
             "--offline-mode", "--gamemode", "1", "--data", data_root,
             "--world", world, "--no-flat-world"], wait_listening, env=env)

        bot = Bot("FurnaceBot", port)
        wire_client.record(bot, note="furnace: cook progress, fuel burn and XP")
        bot.join()
        drain(bot, 1.0)
        print("  joined")

        # The ore and the coal go into hotbar 1 and 2 *before* the furnace is
        # opened: a creative slot write means the client is showing the creative
        # screen, so the server drops the window it thought was open.
        give(bot, 36, ITEM_FURNACE, 1)       # hotbar 0: the item we place with
        give(bot, 37, ITEM_RAW_IRON, 3)      # hotbar 1
        give(bot, 38, ITEM_COAL_BLOCK, 1)    # hotbar 2: a long-burning fuel, so
                                             # the flame is still alive at the
                                             # end of the test whatever the
                                             # machine's timing
        give(bot, 39, ITEM_STONE, 64)        # hotbar 3: the floor
        drain(bot, 0.5)
        # This world has no terrain at all — chunks missing on disk fall back to
        # the void emitter — and an orb that leaves the furnace would fall
        # forever. A 9x9 floor at FURNACE_Y - 2 (so its top face is the block
        # below the furnace) catches the launch drift and gives the player
        # somewhere to stand inside the orbs' 8-block pull range.
        bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", 3))
        seq = 1
        # The floor is 9x9 and the server's block interaction range reaches 5.5
        # blocks from the eye to the block's box, so the bot walks to each row
        # and lays that row — which is what a player building a platform does.
        # Standing in one spot would only reach the cells around it.
        for dz in range(-4, 5):
            bot.send(SB_MOVE_POS, struct.pack(">ddd", 0.5, float(FURNACE_Y), dz + 0.5)
                     + bytes([1]))
            for dx in range(-4, 5):
                place(bot, dx, FURNACE_Y - 2, dz, 1, seq)
                seq += 1
        drain(bot, 1.0)
        bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", 0))
        place(bot, 0, FURNACE_Y - 1, 0, 1, seq)
        packets = drain(bot, 2.0)
        furnace_updates = [b for b in all_of(packets, CB_BLOCK_UPDATE)
                           if read_block_update_pos(b) == (0, FURNACE_Y, 0)]
        assert furnace_updates, (
            f"placing the furnace produced no block update: {[hex(p) for p, _ in packets]}")
        cold_state, _ = rd_varint(furnace_updates[0], 8)
        assert cold_state != 0, "the placed furnace must not be air"
        print(f"  furnace placed on a 9x9 floor (state {cold_state})")

        # Right-click it: open_screen (menu 14) + the 39-slot content + the four
        # data slots, which the client has no other way to learn.
        place(bot, 0, FURNACE_Y, 0, 1, 2)
        packets = drain(bot, 2.0)
        opened = find(packets, CB_OPEN_SCREEN)
        assert opened is not None, f"no open_screen: {[hex(p) for p, _ in packets]}"
        window, menu_id, _ = parse_open_screen(opened)
        assert menu_id == MENU_FURNACE, f"expected the furnace menu (14), got {menu_id}"
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "open_screen without container_set_content"
        cw, state, slots, cursor = decode_content(content)
        assert cw == window == 1, f"first window must be 1, got {cw}/{window}"
        assert len(slots) == FURNACE_SLOTS, f"furnace menu is 39 slots, got {len(slots)}"
        assert slots[SLOT_INPUT] == (0, 0) and slots[SLOT_FUEL] == (0, 0)
        assert slots[SLOT_HOTBAR_FIRST + 1] == (ITEM_RAW_IRON, 3), f"hotbar 1: {slots[SLOT_HOTBAR_FIRST + 1]}"
        assert slots[SLOT_HOTBAR_FIRST + 2] == (ITEM_COAL_BLOCK, 1), f"hotbar 2: {slots[SLOT_HOTBAR_FIRST + 2]}"
        assert cursor == (0, 0), "the cursor must start empty"
        data = [b for p, b in packets if p == CB_CONTAINER_SET_DATA]
        assert len(data) == 4, f"a cooking window opens with its four data slots, got {len(data)}"
        dataslots = {}
        for b in data:
            cid, i = rd_varint(b, 0)
            assert cid == window, f"data packet addressed to window {cid}, not {window}"
            ds, val = struct.unpack(">hh", b[i:i + 4])
            dataslots[ds] = val
        assert dataslots == {0: 0, 1: 0, 2: 0, 3: 0}, f"a cold furnace reports zeros, got {dataslots}"
        print(f"  opened window {window} (menu {menu_id}), 4 cold data slots")

        # --- feed it, with ordinary clicks ------------------------------------
        # PICKUP the hotbar stack onto the cursor, then PICKUP it into the slot
        # (a click mode/target pair the client itself produces).
        def latest_state(packets, fallback):
            """The container revision the client would now hold: the newest one
            the server tagged a window packet with."""
            state = fallback
            if find(packets, CB_CONTAINER_SET_CONTENT) is not None:
                _, state, _, _ = decode_content(find(packets, CB_CONTAINER_SET_CONTENT))
            for p, b in packets:
                if p != CB_CONTAINER_SET_SLOT:
                    continue
                _, i = rd_varint(b, 0)
                s, _ = rd_varint(b, i)
                state = s
            return state

        def pickup(src, dst, state, moved):
            """Pick up the stack at `src`, then put it down in `dst` — the two
            clicks a client makes, with `moved` on the cursor in between."""
            bot.send(SB_CONTAINER_CLICK, click_body(window, state, src, 0, 0))
            packets = drain(bot, 0.6)
            state = latest_state(packets, state)
            bot.send(SB_CONTAINER_CLICK, click_body(window, state, dst, 0, 0, moved))
            packets = drain(bot, 0.6)
            state = latest_state(packets, state)
            return packets, state

        fed = []
        packets, state = pickup(SLOT_HOTBAR_FIRST + 1, SLOT_INPUT, state, (ITEM_RAW_IRON, 3))
        assert any(p == CB_CONTAINER_SET_SLOT for p, _ in packets), (
            f"moving the ore produced no slot update: {[hex(p) for p, _ in packets]}")
        fed += packets
        packets, state = pickup(SLOT_HOTBAR_FIRST + 2, SLOT_FUEL, state, (ITEM_COAL_BLOCK, 1))
        assert any(p == CB_CONTAINER_SET_SLOT for p, _ in packets), (
            f"moving the coal produced no slot update: {[hex(p) for p, _ in packets]}")
        fed += packets
        print("  fed 3 raw iron and a coal block")

        # --- it must actually cook -------------------------------------------
        # Three smelts are 600 game ticks = 30 real ticks at TICK_SCALE 20. The
        # lighting happens on the first tick after the fuel lands, so the block
        # update is already in `fed` — collect from both.
        wait = (3 * COOK_TICKS / (20.0 * TICK_SCALE)) + 1.0
        packets = fed + drain(bot, wait)
        progress = {}
        lit_state = None
        max_progress = 0
        for p, b in packets:
            if p == CB_CONTAINER_SET_DATA:
                _, i = rd_varint(b, 0)
                ds, val = struct.unpack(">hh", b[i:i + 4])
                progress[ds] = val
                if ds == 2:
                    max_progress = max(max_progress, val)
            elif p == CB_BLOCK_UPDATE:
                # Block Update: [i64 position][varint state id], with the
                # position packed x<<38 | z<<12 | y (see `position` above).
                if read_block_update_pos(b) == (0, FURNACE_Y, 0):
                    lit_state, _ = rd_varint(b, 8)
        assert progress, f"the furnace never reported a data slot: {[hex(p) for p, _ in packets]}"
        assert progress.get(0, 0) > 0, f"the flame is not burning: {progress}"
        assert progress.get(1, -1) == COAL_BLOCK_TICKS, (
            f"the flame's denominator is the fuel's own burn time: {progress}")
        assert progress.get(3, -1) == COOK_TICKS, (
            f"the progress bar's denominator is the smelting recipe's: {progress}")
        assert max_progress > 0, f"the progress bar never moved: {progress}"
        assert lit_state is not None and lit_state != cold_state, (
            f"the furnace's `lit` property never changed (cold {cold_state}, now {lit_state})")
        print(f"  cooked: flame {progress.get(0)}/{progress.get(1)}, progress {max_progress}/{progress.get(3)}")

        # --- and the output is really there ----------------------------------
        # A deliberately stale click makes the server resync the whole menu, so
        # the assertion is on the furnace's *state*, not on a diff we guessed.
        bot.send(SB_CONTAINER_CLICK, click_body(window, 9999, 0, 0, 0))
        packets = drain(bot, 1.5)
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "a stale click must resync the menu"
        _, state, slots, _ = decode_content(content)
        assert slots[SLOT_INPUT] == (0, 0), f"the ore should be gone, got {slots[SLOT_INPUT]}"
        assert slots[SLOT_RESULT] == (ITEM_IRON_INGOT, 3), f"expected 3 iron ingots, got {slots[SLOT_RESULT]}"
        # The fuel *unit* is consumed when it starts burning, not when it runs
        # out — the flame is still alight at this point (see the data slots).
        assert slots[SLOT_FUEL] == (0, 0), f"the burned fuel unit should be gone, got {slots[SLOT_FUEL]}"
        print("  3 iron ingots smelted, 0 ore, fuel unit consumed, flame still alight")

        # --- taking the output pays the experience the furnace recorded -------
        # It pays it as *orbs*: real entities that fly to the nearest player and
        # are collected there (ExperienceOrb.award -> playerTouch ->
        # giveExperiencePoints). So the bar is still empty at the moment of the
        # take and moves on its own once an orb reaches us.
        #
        # Stand next to the furnace *first* — which is what a player taking an
        # output does, and the only setup that is deterministic: the pull has to
        # be there from the orb's first tick, or the launch velocity carries it
        # off the floor before anything reaches it.
        stand = (0.5, float(FURNACE_Y), 1.5)
        bot.send(SB_MOVE_POS, struct.pack(">ddd", *stand) + bytes([1]))
        drain(bot, 0.5)
        bot.send(0x09, varint(len("/xp query @s points")) + b"/xp query @s points")
        packets = drain(bot, 2.0)
        chats = [chat_text(b) for p, b in packets if p == CB_SYSTEM_CHAT]
        assert any("experience points" in c for c in chats), f"unexpected /xp reply: {chats}"
        assert any(re.search(r"\b0 experience points", c) for c in chats), (
            f"nothing has been collected yet, so the bar must still be empty: {chats}")
        print("  the bar is still empty before the take: the payout is not a credit")

        bot.send(SB_CONTAINER_CLICK, click_body(window, state, SLOT_RESULT, 0, 0))
        packets = drain(bot, 1.0)
        state = latest_state(packets, state)
        # Park the ingots in hotbar 0 (which still holds the furnace item, so
        # the click is a swap), leaving the cursor empty again.
        bot.send(SB_CONTAINER_CLICK,
                 click_body(window, state, SLOT_HOTBAR_FIRST, 0, 0, (ITEM_IRON_INGOT, 3)))
        packets += drain(bot, 1.0)
        # Everything from the take onwards: the orbs are spawned by the click
        # and collected by the tick a few ticks later, so the spawn, the position
        # syncs, the pickup and the set_experience can all land inside the drain
        # windows below. A drain that is thrown away loses them.
        seen = list(packets)

        # The orbs themselves: an experience_orb (entity type 49) spawned at the
        # furnace, carrying its value in synched data. (Their flight — gravity,
        # the pull, the position syncs — is pinned in tests/xp_orb_test.cpp and
        # tests/drop_packet_test.cpp; here the question is what the wire pays.)
        orbs = orb_spawns(packets)
        assert orbs, f"taking the output must drop experience orbs: {[hex(p) for p, _ in packets]}"
        assert all(v >= 1 for _, v in orbs), f"an orb worth nothing: {orbs}"
        orb_ids = {eid for eid, _ in orbs}
        worth = sum(v for _, v in orbs)
        print(f"  the payout arrived as {len(orbs)} orb(s) worth {worth} points: {orbs}")

        # Now wait for them to reach the player, and for the payout to finish.
        #
        # The wait is for the *bar* to settle rather than for a take count,
        # because a split award can merge: ExperienceOrb.tryMergeToExisting
        # takes the rung right after a fresh orb when it lands inside that orb's
        # 1x1x1 box with the same value, and the `count` it adds is not on the
        # wire (only DATA_VALUE is) — so one orb *entity* can take several
        # pickups, two ticks apart (Player.takeXpDelay). Counting takes against
        # spawns would therefore be wrong for exactly the payouts that merged.
        deadline = time.time() + 12.0
        took = []
        bar = []
        settled = -1
        quiet = 0
        while time.time() < deadline:
            seen += drain(bot, 0.4)
            took = [take_item_entity(b) for b in all_of(seen, CB_TAKE_ITEM)]
            bar = [read_set_experience(b) for b in all_of(seen, CB_SET_EXPERIENCE)]
            if took and bar and bar[-1][2] > 0:
                if len(took) == settled:
                    quiet += 1
                    if quiet >= 3:  # three windows with no further pickup
                        break
                else:
                    quiet = 0
            settled = len(took)
        assert took, "the orb was never collected (take_item_entity)"
        assert all(amount == 1 for _e, _p, amount in took), (
            f"an orb is taken one at a time: {took}")
        assert all(player == bot.entity_id for _e, player, _a in took), (
            f"the orbs must be taken by our own player ({bot.entity_id}): {took}")
        assert {e for e, _p, _a in took} == orb_ids, (
            f"every orb that spawned must be collected, and every take must name "
            f"one of them: {took} vs {orbs}")
        moved = [t for t in bar if t[2] > 0]
        assert moved, f"collecting an orb must move the experience bar: {bar}"
        # The bar is worth exactly the orbs, not the orbs plus a credit: this is
        # the whole point of the change. Each take credits the value of the orb
        # it names.
        value_of = dict(orbs)
        credited = sum(value_of[e] for e, _p, _a in took)
        assert moved[-1][2] == credited, (
            f"the bar must hold exactly what was collected ({credited}): {bar}")
        # ...and that number is the furnace's own: three iron ingots at 0.7 xp
        # is floor(2.1) = 2 (createExperience), plus the one the 0.1 fraction
        # beats one time in ten, so 2 or 3 — never a figure that ignores the
        # recipes. This is what makes the equality above a statement about the
        # orbs rather than about the test's own arithmetic.
        assert 2 <= moved[-1][2] <= 3, (
            f"three smelted iron ingots are worth 2 or 3 experience, got {moved[-1][2]}")
        print(f"  collected {len(took)} take(s) of {len(orbs)} orb(s) "
              f"(a merged orb takes once per orb in its stack); the bar is now "
              f"{moved[-1][2]}")

        bot.send(0x09, varint(len("/xp query @s points")) + b"/xp query @s points")
        packets = drain(bot, 2.0)
        chats = [chat_text(b) for p, b in packets if p == CB_SYSTEM_CHAT]
        assert any(re.search(r"\b[1-9]\d* experience points", c) for c in chats), (
            f"the collected orbs must show in /xp query: {chats}")
        print(f"  xp after collecting the orbs: {chats[-1]}")

        bot.close()
        print("PASS furnace")
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
