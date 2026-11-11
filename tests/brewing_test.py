#!/usr/bin/env python3
"""Integration test: the brewing stand (Phase 3 milestone G follow-up).

A creative bot places a brewing stand, opens it, and moves three water bottles,
a nether wart and a blaze powder into its five slots with ordinary container
clicks. The server must then actually *brew*, over the real wire protocol:

  * `open_screen` (0x3b) with menu id 11 (BrewingStandMenu) and a
    `container_set_content` (0x12) of 41 slots — the stand's five, then main and
    hotbar with no armour and no offhand,
  * `container_set_data` (0x13) for the two data slots BrewingStandMenu
    declares — the brew ticks remaining and the fuel *uses* remaining — sent
    when the window opens and again as the counters move,
  * `block_update` (0x08) when a `has_bottle_0..2` property flips, which is what
    the client draws the stand's bottle models from,
  * `level_event` (0x2e, type 1035) when a brew finishes, which is the brew
    sound and the bubbles,
  * `container_set_slot` (0x14) when the bottles become awkward potions and the
    ingredient is consumed,
  * the menu's own slot rules: a bottle slot holds one item
    (`PotionSlot.getMaxStackSize`), so a shift-clicked stack of three glass
    bottles fills *three* slots rather than all three going into the first, and
    a slot that already holds one takes no more,
  * `Containers.dropItemStack`: a dragon's breath that is not the last one in
    its stack leaves its glass-bottle remainder in the slot, so a *stack* of two
    has nowhere to put the second brew's remainder and it is dropped as a real
    item entity.

The brew is driven by the server's tick loop, which advances 20 ticks a second;
PLUSMC_TICK_SCALE multiplies that so the test does not have to sit through
twenty seconds of real brewing. One brew is 401 game ticks — the tick that
starts it spends a fuel use and sets the timer, and the next 400 count down.

Layout notes (verified against the 26.2 codecs, see docs/PHASE3.md):
  * open_screen (0x3b): [varint container id][varint menu id][title component]
  * container_set_content (0x12): [varint id][varint state][varint count][stacks][cursor]
  * container_set_slot (0x14): [varint id][varint state][i16 slot][stack]
  * container_set_data (0x13): [varint id][i16 data slot][i16 value]
  * level_event (0x2e): [i32 event][i64 packed position][i32 data][bool global]
  * serverbound container_click (0x12): [varint id][varint state][i16 slot][i8 button]
        [varint ContainerInput][varint changed count][HashedStack carried]
  * BrewingStandMenu slots: 0..2 bottles, 3 ingredient, 4 fuel, 5..31 main,
    32..40 hotbar. window 0 is the player's own menu (0 result, 1..4 grid,
    5..8 armour, 9..35 main, 36..44 hotbar, 45 offhand).
"""
import os, re, shutil, socket, struct, subprocess, sys, tempfile, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nbt_component import chat_text  # noqa: E402
import wire_client
from wire_client import make_frame, read_packet

PROTO = 777
HOST = "127.0.0.1"

# Registry ids (src/world/item_registry.h).
ITEM_GLASS_BOTTLE = 1270
ITEM_POTION = 1271
ITEM_NETHER_WART = 1269
ITEM_BLAZE_POWDER = 1274
ITEM_BREWING_STAND = 1276
ITEM_DRAGON_BREATH = 1441
ITEM_SPLASH_POTION = 1442
ITEM_LINGERING_POTION = 1445

# minecraft:potion ids (src/world/potion_table.h).
POTION_WATER = 0
POTION_AWKWARD = 3

# The minecraft:data_component_type id of potion_contents.
COMPONENT_POTION_CONTENTS = 53

# The two `has_bottle_*` block states of a brewing stand with all three bottle
# slots empty and with all three full (src/world/block_states.h). Every mixture
# in between exists too; the test only needs these two.
STATE_NO_BOTTLES = 11193
STATE_THREE_BOTTLES = 11186

# Packet ids.
CB_SPAWN_ENTITY = 0x01
CB_BLOCK_UPDATE = 0x08
CB_CONTAINER_SET_CONTENT = 0x12
CB_CONTAINER_SET_DATA = 0x13
CB_CONTAINER_SET_SLOT = 0x14
CB_LEVEL_EVENT = 0x2F
CB_OPEN_SCREEN = 0x3C
CB_SET_CURSOR_ITEM = 0x62
CB_SET_ENTITY_DATA = 0x65
CB_SYSTEM_CHAT = 0x7C
CB_KEEP_ALIVE = 0x2D

SB_CHAT_COMMAND = 0x07
SB_CONTAINER_CLICK = 0x12
SB_KEEP_ALIVE = 0x1C
SB_FLYING = 0x21
SB_SET_CARRIED_ITEM = 0x36
SB_USE_ITEM_ON = 0x42

MENU_BREWING_STAND = 11
STAND_SLOTS = 41
SLOT_BOTTLES = (0, 1, 2)
SLOT_INGREDIENT = 3
SLOT_FUEL = 4
SLOT_MAIN_FIRST = 5
SLOT_HOTBAR_FIRST = SLOT_MAIN_FIRST + 27  # 32

# The data slots, in the block entity's ContainerData order.
DATA_BREW_TIME = 0
DATA_FUEL = 1

BREW_TICKS = 400         # PotionBrewing.BREWING_TIME_SECONDS = 20
FUEL_USES = 20           # BrewingStandBlockEntity.FUEL_USES
TICK_SCALE = 20          # PLUSMC_TICK_SCALE: 20 game ticks per real tick


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


def position(x, y, z):
    packed = (((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF)) & 0xFFFFFFFFFFFFFFFF
    return struct.pack(">Q", packed)


# --- decoding ---------------------------------------------------------------

def read_effect_details(buf, pos):
    """MobEffectInstance.Details: [varint amplifier][varint duration][bool
    ambient][bool visible][bool show_icon][bool hidden] and then, when hidden,
    the nested Details."""
    amplifier, pos = rd_varint(buf, pos)
    duration, pos = rd_varint(buf, pos)
    ambient, visible, show_icon = buf[pos], buf[pos + 1], buf[pos + 2]
    pos += 4  # the three flags and the `has hidden` byte
    return {"amplifier": amplifier, "duration": duration, "ambient": ambient,
            "visible": visible, "show_icon": show_icon}, pos


def read_potion_contents(buf, pos):
    """PotionContents.STREAM_CODEC: [bool][varint potion]? [bool][varint color]?
    [varint count][instances] [bool][utf8 name]?."""
    potion = None
    if buf[pos]:
        potion, pos = rd_varint(buf, pos + 1)
    else:
        pos += 1
    if buf[pos]:  # custom_color: a fixed-width INT, display only
        pos += 1
        pos += 4
    else:
        pos += 1
    count, pos = rd_varint(buf, pos)
    effects = []
    for _ in range(count):
        effect, pos = rd_varint(buf, pos)
        details, pos = read_effect_details(buf, pos)
        effects.append((effect, details))
    if buf[pos]:  # custom_name
        pos += 1
        _, pos = rd_string(buf, pos)
    else:
        pos += 1
    return {"potion": potion, "effects": effects}, pos


def read_stack(body, pos=0):
    """The *trusted* (server-sent) ItemStack: [varint count][varint item]
    [varint added][varint removed] and then, per added entry, [varint id][value]
    — with no length. A component's value can only be skipped by knowing it, so
    any component other than potion_contents is an assertion failure here rather
    than something silently walked over."""
    count, pos = rd_varint(body, pos)
    if count <= 0:
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


def decode_set_data(body):
    """container_set_data: (window, data slot, value) — the data slot and the
    value are *shorts*, unlike the varint window id."""
    window, pos = rd_varint(body, 0)
    data_slot, value = struct.unpack(">hh", body[pos:pos + 4])
    assert pos + 4 == len(body), f"container_set_data: {len(body) - pos - 4} trailing byte(s)"
    return window, data_slot, value


def decode_cursor(body):
    """set_cursor_item: [stack] — no window, no slot."""
    stack, pos = read_stack(body, 0)
    assert pos == len(body), f"set_cursor_item: {len(body) - pos} trailing byte(s)"
    return stack


def decode_level_event(body):
    """level_event: [i32 type][i64 packed position][i32 data][bool global]."""
    assert len(body) == 4 + 8 + 4 + 1, f"level_event body is 17 bytes, got {len(body)}"
    event = struct.unpack(">i", body[0:4])[0]
    raw = struct.unpack(">q", body[4:12])[0]
    x = raw >> 38
    z = (raw >> 12) & 0x3FFFFFF
    y = raw & 0xFFF
    if z >= 0x2000000:
        z -= 0x4000000
    if y >= 0x800:
        y -= 0x1000
    data = struct.unpack(">i", body[12:16])[0]
    global_event = body[16]
    return event, (x, y, z), data, global_event


def read_block_update(body):
    """block_update: (packed position, state id)."""
    raw = struct.unpack(">q", body[0:8])[0]
    x = raw >> 38
    z = (raw >> 12) & 0x3FFFFFF
    y = raw & 0xFFF
    if z >= 0x2000000:
        z -= 0x4000000
    if y >= 0x800:
        y -= 0x1000
    state, _ = rd_varint(body, 8)
    return (x, y, z), state


def decode_open_screen(body):
    """open_screen: (window, menu, title)."""
    window, pos = rd_varint(body, 0)
    menu, pos = rd_varint(body, pos)
    assert 1 <= window <= 100, f"window id {window} outside vanilla's 1..100"
    return window, menu


def spawn_entity_type(body):
    """Add Entity: (entity id, entity type id)."""
    eid, pos = rd_varint(body, 0)
    pos += 16  # uuid
    etype, _ = rd_varint(body, pos)
    return eid, etype


def plain(item, count):
    """A stack as `read_stack` reports it, with no components at all."""
    return {"item": item, "count": count, "patch": {}}


def potion_of(stack):
    """(potion, effects) the menu stack's potion_contents holds."""
    assert stack is not None, "no stack to read contents from"
    assert COMPONENT_POTION_CONTENTS in stack["patch"], \
        f"the stack carries no potion_contents: {sorted(stack['patch'])}"
    contents = stack["patch"][COMPONENT_POTION_CONTENTS]
    return contents["potion"], contents["effects"]


# --- the packets the test *sends* -------------------------------------------

def hashed_stack(item, count):
    """A HashedStack (what a container_click carries): [u8 present][varint item]
    [varint count][varint added][varint removed] — the presence flag is a byte,
    not a varint, and the components are hashes rather than values."""
    if count <= 0:
        return b"\x00"
    return bytes([1]) + varint(item) + varint(count) + varint(0) + varint(0)


def click_body(window, state, slot, button, mode, carried=(0, 0)):
    """A serverbound container_click. `carried` is the stack the client believes
    it holds on the cursor: the server compares it against its own and resyncs on
    a mismatch, so a bot that always claimed an empty cursor could never make a
    second click in a row."""
    return (varint(window) + varint(state) + struct.pack(">h", slot) + bytes([button])
            + varint(mode) + varint(0) + hashed_stack(*carried))


def use_item_on_body(x, y, z, face, seq):
    return (varint(0) + position(x, y, z) + varint(face)
            + struct.pack(">f", 0.5) + struct.pack(">f", 0.0) + struct.pack(">f", 0.5)
            + bytes([0]) + bytes([0]) + varint(seq))


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
        self.seq = 1

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

    def place(self, x, y, z, face):
        self.send(SB_USE_ITEM_ON, use_item_on_body(x, y, z, face, self.seq))
        self.seq += 1

    def select(self, hotbar):
        self.send(SB_SET_CARRIED_ITEM, struct.pack(">h", hotbar))


def drain(bot, timeout=1.5):
    """Every packet the reader thread queued in the next `timeout`
    seconds."""
    # A reply window ends when the server has been quiet for `quiet` seconds
    # rather than always burning the whole window; `timeout` stays the bound a
    # missing reply is judged against. Requires the reader (started after each
    # join below).
    return wire_client.drain_quiet(bot, quiet=0.3, limit=timeout)


# (There is deliberately no `tick` helper here: the brew's clock is the *world*
# tick, multiplied by `PLUSMC_TICK_SCALE`, and no assertion in this file measures
# the player's survival tick. A ping/pong barrier would not help either — it
# bounds survival ticks, and the 400 ticks of a brew are not those.)


def all_of(packets, pid):
    return [body for p, body in packets if p == pid]


def find(packets, pid):
    for p, body in packets:
        if p == pid:
            return body
    return None


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

    def pump():
        for raw in iter(proc.stdout.readline, b""):
            sys.stderr.write(raw.decode(errors="replace"))

    threading.Thread(target=pump, daemon=True).start()
    for _ in range(30):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=1)
            s.close()
            return port
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("server did not accept a connection")


# --- menu slot arithmetic ---------------------------------------------------

def inventory_slot_of_player_menu(menu_slot):
    """A slot number in window 0 (the player's own menu, 46 slots) -> the
    player's inventory slot, or None for the slots that back the craft area,
    the armour and the offhand (which a brewing stand's menu does not show)."""
    if 9 <= menu_slot <= 35:
        return menu_slot
    if 36 <= menu_slot <= 44:
        return menu_slot - 36
    return None


def brewing_menu_slot(inv_slot):
    """A player inventory slot -> its slot number in a brewing stand's menu."""
    if 9 <= inv_slot <= 35:
        return SLOT_MAIN_FIRST + (inv_slot - 9)
    if 0 <= inv_slot <= 8:
        return SLOT_HOTBAR_FIRST + inv_slot
    return None


class Screen:
    """The window the bot is looking at, and the revision the server last tagged
    a packet with (which is what a click has to carry)."""

    def __init__(self, bot):
        self.bot = bot
        self.window = 0
        self.state = 0
        self.slots = []
        self.cursor = None

    def observe(self, packets):
        for body in all_of(packets, CB_CONTAINER_SET_CONTENT):
            window, state, slots, cursor = decode_content(body)
            self.window, self.state, self.slots, self.cursor = window, state, slots, cursor
        for body in all_of(packets, CB_CONTAINER_SET_SLOT):
            window, state, slot, stack = decode_set_slot(body)
            self.state = state
            if window == self.window and slot < len(self.slots):
                self.slots[slot] = stack
        for body in all_of(packets, CB_SET_CURSOR_ITEM):
            self.cursor = decode_cursor(body)
        return packets

    def command(self, text, timeout=1.5):
        self.bot.send(SB_CHAT_COMMAND, varint(len(text)) + text.encode())
        return self.observe(drain(self.bot, timeout))

    def click(self, slot, button=0, mode=0, timeout=0.7):
        carried = (self.cursor["item"], self.cursor["count"]) if self.cursor else (0, 0)
        self.bot.send(SB_CONTAINER_CLICK,
                      click_body(self.window, self.state, slot, button, mode, carried))
        return self.observe(drain(self.bot, timeout))

    def give(self, argument, count=1):
        """`/give` and return the *inventory* slots the stacks landed in.

        `/give` resyncs the player's own menu (window 0), so each slot that went
        from empty to something is where a stack landed — an unstackable item
        (a potion) asked for in a count of 3 arrives as three separate stacks in
        three slots, which is why this returns a list.
        """
        before = list(self.slots)
        packets = self.command(f"give {self.bot.name} {argument} {count}")
        assert "Gave " in feedback(packets), feedback(packets)
        assert len(self.slots) == 46, f"window 0 is 46 slots, got {len(self.slots)}"
        changed = []
        for i, stack in enumerate(self.slots):
            if stack is not None and (i >= len(before) or before[i] != stack):
                inv = inventory_slot_of_player_menu(i)
                if inv is not None:
                    changed.append(inv)
        assert changed, f"the stack is nowhere in the inventory: {self.slots}"
        return changed


def step(text):
    print(f"[{time.time() - START:6.2f}s] {text}", flush=True)


START = time.time()


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-brewing-")
    # The same fixture chunk the other survival tests copy: brewing needs no
    # terrain, but a world with real region data is what they all run in.
    shutil.copytree(os.path.join(os.path.dirname(__file__), "fixtures", "ow3region"),
                    os.path.join(world, "region"))
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_TICK_SCALE"] = str(TICK_SCALE)
    # Pinned for consistency with the rest of the suite (and so that no test
    # added here later inherits a wall-clock survival tick). Nothing in this file
    # asserts a survival-tick quantity: the brewing stand brews on the *world*
    # tick, and the bot is in creative, so the player's own clock has no hand in
    # what is checked.
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
    STAND_Y = 70

    proc = None
    try:
        step(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", "127.0.0.1", "--port", "0", "--motd", "brewing test",
             "--offline-mode", "--gamemode", "1", "--data", data_root,
             "--world", world, "--no-flat-world"], wait_listening, env=env)

        bot = Bot("BrewBot", port)
        wire_client.record(bot, note="brewing-wire: the stand, its recipes, the fuel")
        bot.join()
        bot.start_reader()
        screen = Screen(bot)
        screen.observe(drain(bot, 1.2))
        step("  joined")

        # --- 0. the materials, before any window is open ---------------------
        # Commands do not close a window the way a creative slot write does, but
        # they do fill the *selected* hotbar slot first. Only the two blocks the
        # bot has to hold in its hand need to be in the hotbar — everything else
        # is clicked out of wherever it lands.
        stands = screen.give("minecraft:brewing_stand", 2)[0]
        stone = screen.give("minecraft:stone", 64)[0]
        water = []
        for _ in range(3):
            water += screen.give('minecraft:potion[potion_contents={potion:"minecraft:water"}]')
        wart = screen.give("minecraft:nether_wart")[0]
        powder = screen.give("minecraft:blaze_powder")[0]
        gunpowder = screen.give("minecraft:gunpowder")[0]
        breath = screen.give("minecraft:dragon_breath", 2)[0]
        bottles = screen.give("minecraft:glass_bottle", 6)[0]
        assert stands <= 8 and stone <= 8, f"the two blocks must be holdable: {stands}/{stone}"
        step(f"  gave: stand@{stands}, stone@{stone}, water@{water}, wart@{wart}, "
             f"powder@{powder}, gunpowder@{gunpowder}, breath@{breath}, bottles@{bottles}")

        # A floor to build on, and somewhere for the dropped remainder to land.
        bot.send(SB_FLYING, bytes([1]))
        bot.send(0x1E, struct.pack(">ddd", 0.5, float(STAND_Y), 0.5) + bytes([1]))
        bot.select(stone)
        screen.observe(drain(bot, 0.4))
        for dz in range(-2, 3):
            for dx in range(-2, 3):
                bot.place(dx, STAND_Y - 1, dz, 1)
        floor_packets = screen.observe(drain(bot, 1.2))
        assert len(all_of(floor_packets, CB_BLOCK_UPDATE)) >= 20, "the floor did not go down"
        step("  laid a 5x5 floor")

        # --- 1. two stands, placed and opened --------------------------------
        bot.select(stands)
        screen.observe(drain(bot, 0.3))
        # `use_item_on` with face UP places the block *at* the clicked cell, so
        # clicking the cell above the floor puts the stand on the floor.
        bot.place(0, STAND_Y - 1, 0, 1)
        bot.place(2, STAND_Y - 1, 0, 1)
        placed = screen.observe(drain(bot, 1.2))
        states = {}
        for body in all_of(placed, CB_BLOCK_UPDATE):
            pos, state = read_block_update(body)
            if pos[1] == STAND_Y and pos[2] == 0 and pos[0] in (0, 2):
                states[pos[0]] = state
        assert set(states) == {0, 2}, f"both stands must be placed: {states}"
        assert states[0] == STATE_NO_BOTTLES, f"a fresh stand has no bottles: {states}"
        step(f"  placed two brewing stands (has_bottle state {STATE_NO_BOTTLES})")

        bot.place(0, STAND_Y, 0, 1)  # click the stand itself: open, not place
        opened = screen.observe(drain(bot, 1.5))
        body = find(opened, CB_OPEN_SCREEN)
        assert body is not None, f"no open_screen: {[hex(p) for p, _ in opened]}"
        window, menu = decode_open_screen(body)
        assert menu == MENU_BREWING_STAND, f"expected the brewing stand menu (11), got {menu}"
        assert window == 1, f"the first window is 1, got {window}"
        assert screen.window == window
        assert len(screen.slots) == STAND_SLOTS, \
            f"a brewing stand's menu is 41 slots, got {len(screen.slots)}"
        assert all(s is None for s in screen.slots[:5]), "a fresh stand is empty"
        data = {}
        for b in all_of(opened, CB_CONTAINER_SET_DATA):
            cid, data_slot, value = decode_set_data(b)
            assert cid == window
            data[data_slot] = value
        assert data == {DATA_BREW_TIME: 0, DATA_FUEL: 0}, \
            f"a cold stand reports two zero data slots, got {data}"
        assert screen.slots[brewing_menu_slot(wart)] == plain(ITEM_NETHER_WART, 1), screen.slots
        assert screen.cursor is None, "the cursor must start empty"
        step(f"  opened window {window} (menu {menu}), 41 slots, 2 cold data slots")

        def feed(src_inv, dst_slot):
            """The two clicks a client makes — pick the stack up from the player's
            inventory, then put it down in one of the stand's own slots — and the
            packets they produced. The drains in between are long enough (0.7 s
            each, against a 1 s brew) that a brew started by the second click can
            *finish* inside this call, so the packets are handed back rather than
            drained again."""
            packets = screen.click(brewing_menu_slot(src_inv), 0, 0)
            assert screen.cursor is not None, f"nothing came off inventory slot {src_inv}"
            packets += screen.click(dst_slot, 0, 0)
            assert screen.cursor is None, f"the cursor did not empty into slot {dst_slot}"
            return packets

        def bottle_slot_stacks():
            return [screen.slots[i] for i in SLOT_BOTTLES]

        def data_values(packets, data_slot):
            return [v for b in all_of(packets, CB_CONTAINER_SET_DATA)
                    for _, slot, v in [decode_set_data(b)] if slot == data_slot]

        # --- 2. brew one: water bottles plus nether wart become awkward ------
        # Every click's packets are kept, because the `has_bottle_*` state flips
        # with the *bottles* (which the water bottles are) rather than with the
        # brew.
        brew = []
        for i, inv in enumerate(water):
            brew += feed(inv, SLOT_BOTTLES[i])
        brew += feed(wart, SLOT_INGREDIENT)
        # The blaze powder goes in the *fuel* slot, which burns it into 20 fuel
        # *uses* — one of which starts this brew. The slot is empty again by the
        # time the click's own drain ends, because the world tick burns the unit
        # on its very next pass: the fuel is a counter, not a flame.
        brew += feed(powder, SLOT_FUEL)
        # The world tick converts the powder into its 20 uses on its next pass
        # (the note above), so the fuel slot holds nothing again — the uses are
        # a counter, not an item the slot keeps. This is what tick_brewing's
        # `shrink_one(kBrewingFuelSlot)` does, pinned by brewing_test.cpp too.
        assert screen.slots[SLOT_FUEL] is None, \
            f"the powder was consumed into its uses: {screen.slots[SLOT_FUEL]}"
        assert screen.slots[SLOT_INGREDIENT] == plain(ITEM_NETHER_WART, 1), screen.slots
        step("  fed three water bottles, a nether wart and a blaze powder")

        # One brew is 401 game ticks — the starting tick sets the timer and the
        # next 400 count it down — which is 20 real ticks at TICK_SCALE 20.
        brew += screen.observe(drain(bot, 2.5))
        for i, stack in enumerate(bottle_slot_stacks()):
            assert stack is not None and stack["item"] == ITEM_POTION, f"bottle {i}: {stack}"
            assert potion_of(stack)[0] == POTION_AWKWARD, f"bottle {i}: {stack}"
        assert screen.slots[SLOT_INGREDIENT] is None, \
            f"the wart must be consumed: {screen.slots[SLOT_INGREDIENT]}"
        fuel = data_values(brew, DATA_FUEL)
        times = data_values(brew, DATA_BREW_TIME)
        assert fuel and fuel[-1] == FUEL_USES - 1, f"one fuel use, spent: {fuel}"
        # The timer counts 400 down to 0, and 0 is where the brew finishes. The
        # *first* value the client is told is not 400: PLUSMC_TICK_SCALE makes
        # the server advance a whole 20-tick batch per sync, and the timer only
        # reaches the wire at the end of the batch it started in. That the timer
        # really is 400 ticks long, and that the brew takes 401 ticks to finish,
        # is pinned exactly by the unit test's tick loop.
        assert times and times[0] > 300 and times[-1] == 0, \
            f"the timer runs a long way down to 0: {times[0]} ... {times[-1]}"
        assert all(times[i] >= times[i + 1] for i in range(len(times) - 1)), times
        step(f"  brewed three awkward potions (fuel {fuel[-1]}/{FUEL_USES}, "
             f"timer {times[0]} -> {times[-1]})")

        # The `has_bottle_*` block states: the client draws the bottle models
        # from them, so they have to follow the slots. The world tick writes them
        # from the block entity, which is the only thing that knows.
        stand_states = [state for pos, state in
                        (read_block_update(b) for b in all_of(brew, CB_BLOCK_UPDATE))
                        if pos == (0, STAND_Y, 0)]
        assert STATE_THREE_BOTTLES in stand_states, \
            f"three bottles must flip the state to {STATE_THREE_BOTTLES}: {stand_states}"
        level = [decode_level_event(b) for b in all_of(brew, CB_LEVEL_EVENT)]
        assert (1035, (0, STAND_Y, 0), 0, 0) in level, \
            f"a finished brew must raise its sound at the stand: {level}"
        step("  has_bottle state moved (9459 -> 9452) and the brew sound was raised")

        # --- 3. brew two: gunpowder turns them into splash potions ----------
        # A container mix: the *item* changes and the potion inside it does not.
        second = feed(gunpowder, SLOT_INGREDIENT)
        second += screen.observe(drain(bot, 1.5))
        for i, stack in enumerate(bottle_slot_stacks()):
            assert stack is not None and stack["item"] == ITEM_SPLASH_POTION, f"bottle {i}: {stack}"
            assert potion_of(stack)[0] == POTION_AWKWARD, f"bottle {i}: {stack}"
        assert screen.slots[SLOT_INGREDIENT] is None, screen.slots[SLOT_INGREDIENT]
        assert data_values(second, DATA_FUEL)[-1] == FUEL_USES - 2, data_values(second, DATA_FUEL)
        step("  gunpowder brewed the three bottles into splash potions")

        # --- 4. brew three: dragon's breath makes them lingering, and the
        #         second breath's remainder has nowhere to go ------------------
        third = feed(breath, SLOT_INGREDIENT)
        assert screen.slots[SLOT_INGREDIENT]["count"] == 2, screen.slots[SLOT_INGREDIENT]
        third += screen.observe(drain(bot, 1.5))
        for i, stack in enumerate(bottle_slot_stacks()):
            assert stack is not None and stack["item"] == ITEM_LINGERING_POTION, \
                f"bottle {i}: {stack}"
            assert potion_of(stack)[0] == POTION_AWKWARD, f"bottle {i}: {stack}"
        # Two dragon's breath, one brew: the second is still in the slot when the
        # first is consumed, so its glass-bottle remainder is dropped as a real
        # item entity (Containers.dropItemStack) rather than put back in the slot.
        spawns = [spawn_entity_type(b) for b in all_of(third, CB_SPAWN_ENTITY)]
        assert any(etype == 72 for _, etype in spawns), \
            f"the leftover remainder must be dropped as an item entity: {spawns}"
        assert screen.slots[SLOT_INGREDIENT] == plain(ITEM_DRAGON_BREATH, 1), \
            f"the breath that was not consumed stays: {screen.slots[SLOT_INGREDIENT]}"
        step("  dragon's breath brewed them into lingering potions, and the "
             "leftover breath's bottle was dropped")

        # --- 5. the menu's own slot rules, on the second stand ---------------
        bot.place(2, STAND_Y, 0, 1)
        opened = screen.observe(drain(bot, 1.5))
        body = find(opened, CB_OPEN_SCREEN)
        assert body is not None, f"no open_screen for the second stand: {opened}"
        window, menu = decode_open_screen(body)
        assert menu == MENU_BREWING_STAND and window == 2, (window, menu)
        assert all(s is None for s in screen.slots[:5]), "the second stand is fresh"
        # A bottle slot holds *one* item (PotionSlot.getMaxStackSize), so a
        # shift-clicked stack of glass bottles fills the three slots with one
        # each and leaves the rest in the source slot, rather than putting them
        # all into the first slot.
        #
        # Seven, not six: the seventh is the bottle the *first* stand dropped in
        # phase 4 (the surplus dragon's breath's remainder), which is a real item
        # entity now and which this bot walked over on its way to the second
        # stand (`ItemEntity.playerTouch`). The spawn reached the whole stack in
        # hotbar 0, so the pickup merged into it rather than opening a slot.
        source = screen.slots[brewing_menu_slot(bottles)]
        assert source == plain(ITEM_GLASS_BOTTLE, 7), source
        screen.click(brewing_menu_slot(bottles), 0, 1)  # QUICK_MOVE
        for i in SLOT_BOTTLES:
            assert screen.slots[i] == plain(ITEM_GLASS_BOTTLE, 1), \
                f"bottle slot {i} must take exactly one: {screen.slots[i]}"
        assert screen.slots[brewing_menu_slot(bottles)] == plain(ITEM_GLASS_BOTTLE, 4), \
            "three went in, four stayed: " + str(screen.slots[brewing_menu_slot(bottles)])
        # The same shift again changes nothing: the slots are all at their cap,
        # and a potion slot does not merge past it.
        before = [screen.slots[i] for i in SLOT_BOTTLES]
        screen.click(brewing_menu_slot(bottles), 0, 1)
        assert [screen.slots[i] for i in SLOT_BOTTLES] == before, "the cap was exceeded"
        assert screen.slots[brewing_menu_slot(bottles)]["count"] == 4, screen.slots
        step("  the glass bottles filled the three slots one apiece, and no more")

        step("  OK the brewing stand brews what the 26.2 data says it brews")
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
