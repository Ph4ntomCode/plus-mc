#!/usr/bin/env python3
"""Integration test: block containers (Phase 3 milestone C).

Bot1 places a chest, right-clicks it, and the server must answer with
open_screen (menu generic_9x3 = 2) followed by the whole 63-slot menu content.
A shift-click (QUICK_MOVE) moves a stack from the player's hotbar into the
chest. The server is then stopped and restarted on the same world directory;
a second bot reopens the same chest and must be shown the items that were put
there — i.e. the container round-tripped through the Anvil block-entity list.

Layout notes (all verified against the 26.2 codecs, see docs/PHASE3.md):
  * open_screen (0x3b): [varint container id][varint menu id][title component]
  * container_set_content (0x12): [varint id][varint state id][varint count][stacks][cursor]
  * container_set_slot (0x14): [varint id][varint state id][i16 slot][stack]
  * set_cursor_item (0x60): [stack]
  * container_close (0x11): [varint id]
  * serverbound container_click (0x12): [varint id][varint state][i16 slot][i8 button]
        [varint ContainerInput][varint changed count]{[i16 slot][HashedStack]}
        [HashedStack carried]
  * serverbound close_window (0x13): [varint id]
  * a stack is [varint count]; when count > 0, [varint item id][DataComponentPatch];
    a patch entry is [varint type id][the component's own stream codec value].
    A *clientbound* value carries no length; the untrusted form a client sends is
    length-prefixed (`DataComponentPatch.DELIMITED_STREAM_CODEC`), which is what
    lets the server skip a component it does not model.

The test also puts a stack's *components* through the whole path: an enchanted,
damaged and trimmed piece and a potion with a custom effect that hides another
go into the chest and have to come back out of the reloaded region file with
every one of them (`read_stack_full` decodes, `write_stack_components` writes
the delimited form the creative slot carries).
"""
import os, socket, struct, sys, time, zlib, pathlib, shutil, signal, subprocess, re, tempfile
import wire_client
from wire_client import make_frame, read_packet

PROTO = 777
HOST = "127.0.0.1"

# Registry ids (generated item table, src/world/item_registry.h).
ITEM_STONE = 1
ITEM_DIRT = 55
ITEM_CHEST = 404
ITEM_IRON_CHESTPLATE = 1081
ITEM_POTION = 1271
# ...and the component-carrying registries the ids below come from, each named
# with the lookup that produced it.
ENCH_PROTECTION = 28          # enchantment_by_name("minecraft:protection")
ENCH_UNBREAKING = 40          # enchantment_by_name("minecraft:unbreaking")
POTION_WATER = 0              # potion_by_name("minecraft:water")
EFFECT_POISON = 18            # mob_effect_by_name("minecraft:poison")
TRIM_MATERIAL_NETHERITE = 7   # trim_material_for_name("minecraft:netherite")
TRIM_PATTERN_EYE = 3          # trim_pattern_for_name("minecraft:eye")

# DataComponentType ids (src/world/item_registry.h).
COMPONENT_DAMAGE = 3
COMPONENT_ENCHANTMENTS = 13
COMPONENT_POTION_CONTENTS = 53
COMPONENT_TRIM = 58

# Packet ids.
SB_SET_CREATIVE_SLOT = 0x39
SB_USE_ITEM_ON = 0x42
SB_CONTAINER_CLICK = 0x12
SB_CLOSE_WINDOW = 0x13
CB_BLOCK_UPDATE = 0x08
CB_BLOCK_CHANGED_ACK = 0x04
CB_CONTAINER_CLOSE = 0x11
CB_CONTAINER_SET_CONTENT = 0x12
CB_CONTAINER_SET_SLOT = 0x14
CB_OPEN_SCREEN = 0x3C
CB_SET_CURSOR_ITEM = 0x62
CB_KEEP_ALIVE = 0x2D
SB_KEEP_ALIVE = 0x1C

# MenuType.generic_9x3, from the static registry registration order. The menu
# registry has 25 entries (generic_9x1 .. stonecutter, ids 0..24); a larger id
# on the wire makes the client's open_screen decode throw and disconnect.
MENU_REGISTRY_SIZE = 25
MENU_GENERIC_9X3 = 2
CHEST_SLOTS = 27
MENU_SLOTS = CHEST_SLOTS + 27 + 9  # container grid + player main + hotbar


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
    The container id comes FIRST (ByteBufCodecs.CONTAINER_ID). This is worth
    reading off the client rather than off our writer: with the two ids the
    other way round every assertion here still passes while a real client reads
    the menu id as its window id (so its clicks all land on a window the server
    does not have) and the window id as its menu type (so it draws whatever
    screen the registry holds at that index — window 5 showed a single chest as
    a 54-slot double chest). Returns (window, menu_id, next_offset).
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


def rd_bool(body, pos):
    return body[pos] != 0, pos + 1


def rd_u32(body, pos):
    return struct.unpack(">I", body[pos:pos + 4])[0], pos + 4


def rd_effect_details(body, pos):
    """MobEffectInstance.Details.STREAM_CODEC -> ({...}, new_pos).

    [varint amplifier][varint duration][bool ambient][bool show_particles]
    [bool show_icon][bool has_hidden][nested Details]. The nested layer carries
    no effect id of its own: a hidden effect is always the same effect as the
    instance hiding it.
    """
    details = {}
    details["amplifier"], pos = rd_varint(body, pos)
    details["duration"], pos = rd_varint(body, pos)
    details["ambient"], pos = rd_bool(body, pos)
    details["show_particles"], pos = rd_bool(body, pos)
    details["show_icon"], pos = rd_bool(body, pos)
    has_hidden, pos = rd_bool(body, pos)
    if has_hidden:
        details["hidden_effect"], pos = rd_effect_details(body, pos)
    return details, pos


def rd_effect_instance(body, pos):
    """MobEffectInstance.STREAM_CODEC -> ({...}, new_pos): the id, then Details."""
    effect, pos = rd_varint(body, pos)
    details, pos = rd_effect_details(body, pos)
    details["effect"] = effect
    return details, pos


def read_potion_contents(body, pos):
    """PotionContents.STREAM_CODEC -> ({...}, new_pos).

    A presence flag and a potion id, the display-only custom colour, the effects
    list, then the custom name — the fields are in that order, so a decoder that
    skipped the middle ones would lose its place.
    """
    contents = {}
    has_potion, pos = rd_bool(body, pos)
    if has_potion:
        contents["potion"], pos = rd_varint(body, pos)
    has_color, pos = rd_bool(body, pos)
    if has_color:
        contents["custom_color"], pos = rd_u32(body, pos)
    count, pos = rd_varint(body, pos)
    contents["custom_effects"] = []
    for _ in range(count):
        effect, pos = rd_effect_instance(body, pos)
        contents["custom_effects"].append(effect)
    has_name, pos = rd_bool(body, pos)
    if has_name:
        contents["custom_name"], pos = rd_string(body, pos)
    return contents, pos


def read_stack_full(body, pos):
    """ItemStack.STREAM_CODEC -> ({...}, new_pos); count 0 = an empty stack.

    Every component a stack can carry is decoded, not skipped, because skipping
    is exactly what a clientbound patch does not allow: a value has no length in
    front of it, so an unknown type id is a hard failure here rather than a
    silently misread stack.
    """
    count, pos = rd_varint(body, pos)
    if count <= 0:
        return {"count": 0}, pos
    item, pos = rd_varint(body, pos)
    stack = {"item": item, "count": count}
    # Both counts come first, then the added entries, then the removed type ids.
    added, pos = rd_varint(body, pos)
    removed, pos = rd_varint(body, pos)
    for _ in range(added):
        type_id, pos = rd_varint(body, pos)
        if type_id == COMPONENT_DAMAGE:
            stack["damage"], pos = rd_varint(body, pos)
        elif type_id == COMPONENT_ENCHANTMENTS:
            n, pos = rd_varint(body, pos)
            levels = {}
            for _ in range(n):
                enchantment, pos = rd_varint(body, pos)
                levels[enchantment], pos = rd_varint(body, pos)
            stack["enchantments"] = levels
        elif type_id == COMPONENT_POTION_CONTENTS:
            stack["potion_contents"], pos = read_potion_contents(body, pos)
        elif type_id == COMPONENT_TRIM:
            # Both holders are written as id + 1; 0 would be a *direct* holder,
            # which this server never writes, so it is a decode error here.
            material, pos = rd_varint(body, pos)
            pattern, pos = rd_varint(body, pos)
            assert material != 0 and pattern != 0, "a trim holder id is id + 1"
            stack["trim"] = (material - 1, pattern - 1)
        else:
            raise AssertionError(
                f"component type {type_id}: a clientbound patch has no value "
                "length, so this decoder cannot skip what it does not know")
    for _ in range(removed):
        _, pos = rd_varint(body, pos)
    return stack, pos


def read_stack(body, pos):
    """Returns ((item_id, count), new_pos). count 0 = empty stack."""
    stack, pos = read_stack_full(body, pos)
    return (stack.get("item", 0), stack.get("count", 0)), pos


def write_effect_details(amplifier=0, duration=0, ambient=False,
                         show_particles=True, show_icon=True, hidden=None):
    out = varint(amplifier) + varint(duration)
    out += bytes([1 if ambient else 0, 1 if show_particles else 0,
                  1 if show_icon else 0])
    out += bytes([1 if hidden else 0])
    if hidden:
        out += write_effect_details(**hidden)
    return out


def write_effect_instance(effect, amplifier=0, duration=0, ambient=False,
                          show_particles=True, show_icon=True, hidden=None):
    """MobEffectInstance.STREAM_CODEC bytes: the effect id, then Details."""
    return varint(effect) + write_effect_details(
        amplifier, duration, ambient, show_particles, show_icon, hidden)


def component_damage(damage):
    return COMPONENT_DAMAGE, varint(damage)


def component_enchantments(entries):
    value = varint(len(entries))
    for enchantment, level in entries:
        value += varint(enchantment) + varint(level)
    return COMPONENT_ENCHANTMENTS, value


def component_potion_contents(potion=None, effects=()):
    value = bytes([1 if potion is not None else 0])
    if potion is not None:
        value += varint(potion)
    value += b"\x00"                        # custom_color, absent
    value += varint(len(effects))
    for effect in effects:
        value += write_effect_instance(
            effect["effect"], effect.get("amplifier", 0), effect.get("duration", 0),
            effect.get("ambient", False), effect.get("show_particles", True),
            effect.get("show_icon", True), effect.get("hidden"))
    value += b"\x00"                        # custom_name, absent
    return COMPONENT_POTION_CONTENTS, value


def component_trim(material, pattern):
    return COMPONENT_TRIM, varint(material + 1) + varint(pattern + 1)


def write_stack_components(item, count, *components):
    """ItemStack.OPTIONAL_UNTRUSTED_STREAM_CODEC: what a client may send.

    The same [added][removed] patch layout, but each added value is
    length-prefixed (`DataComponentPatch.DELIMITED_STREAM_CODEC`), which is how
    a server can skip a component it does not model.
    """
    out = varint(count) + varint(item)
    out += varint(len(components)) + varint(0)
    for type_id, value in components:
        out += varint(type_id) + varint(len(value)) + value
    return out


def write_stack(item, count, *components):
    """A bare stack, or one with the given (type id, value) components."""
    if count <= 0:
        return varint(0)
    return write_stack_components(item, count, *components)


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
    """The packets the server sends before it goes quiet, up to `timeout`.

    A container reply is synchronous, so a window here is a *reply* window: it
    ends when the server has been quiet for `quiet` seconds (the reader's own
    queue makes that observable) with `timeout` kept as the upper bound a
    missing reply is judged against. The bots below start their readers so this
    is the queue path; without one, `drain_quiet` falls back to a full window.
    """
    return wire_client.drain_quiet(bot, quiet=0.5, limit=timeout)


def drain_reply(bot, pid, limit=8.0):
    """Packets until `pid` arrives, plus the quiet window that follows it.

    `drain`'s quiet gap is what ends a reply, and on a loaded machine the gap
    between a container's open_screen and its content can be longer than that.
    When the reply's first packet is known, waiting for *it* is the boundary;
    the extra window after it collects the rest of the same reply (content,
    slot updates) even when they trail by more than one quiet gap.
    """
    packets = []
    deadline = time.time() + limit
    while time.time() < deadline:
        packets.extend(drain(bot, timeout=min(2.0, max(0.3, deadline - time.time()))))
        if any(p == pid for p, _ in packets):
            break
    packets.extend(drain(bot, timeout=0.5))
    return packets


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
    for _ in range(30):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=1)
            s.close()
            break
        except OSError:
            time.sleep(0.1)
    return port


def decode_content_full(body):
    """container_set_content -> (window, state, [stack dicts], cursor dict)."""
    window = body[0]
    state, i = rd_varint(body, 1)
    n, i = rd_varint(body, i)
    stacks = []
    for _ in range(n):
        stack, i = read_stack_full(body, i)
        stacks.append(stack)
    cursor, i = read_stack_full(body, i)
    return window, state, stacks, cursor


def decode_content(body):
    """The same, with each stack as an (item id, count) pair."""
    window, state, stacks, cursor = decode_content_full(body)
    return (window, state, [(s.get("item", 0), s.get("count", 0)) for s in stacks],
            (cursor.get("item", 0), cursor.get("count", 0)))


def click(window, state, slot, button, mode):
    body = (varint(window) + varint(state) + struct.pack(">h", slot) + bytes([button])
            + varint(mode) + varint(0)          # changed slots: none
            + b"\x00")                          # cursor: absent
    return body


def place(bot, x, y, z, face, seq):
    body = (varint(0) + position(x, y, z) + varint(face)
            + struct.pack(">f", 0.5) + struct.pack(">f", 0.0) + struct.pack(">f", 0.5)
            + bytes([0]) + bytes([0]) + varint(seq))
    bot.send(SB_USE_ITEM_ON, body)


def give(bot, menu_slot, item, count, *components):
    body = struct.pack(">h", menu_slot) + write_stack(item, count, *components)
    bot.send(SB_SET_CREATIVE_SLOT, body)


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-containers-")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "1200"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "4000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    # Short autosave so the test can drive the "chunk already clean" path.
    env["PLUSMC_AUTOSAVE_SECS"] = "1"
    # The survival tick counts packets, one tick each, with no wall-time
    # catch-up, so a drain banks no ticks. What this file asserts is a menu's
    # slots, its data slots and the block updates behind them — never a
    # survival-tick duration (the bot is creative).
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
    # The chest goes on top of this block; the chunk is generated fresh on the
    # first placement, so both the block and the block entity are written.
    CHEST_Y = 70

    proc = None
    try:
        print(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", "127.0.0.1", "--port", "0", "--motd", "container test",
             "--offline-mode", "--gamemode", "1", "--data", data_root,
             "--world", world, "--no-flat-world"], wait_listening, env=env)

        print("Connecting bot1...")
        bot = Bot("ChestBot1", port)
        wire_client.record(bot, note="container-gui: a chest over the wire, reopened after a restart")
        bot.join()
        bot.start_reader()
        drain(bot, 1.0)
        # Stand on top of the chest this bot is about to place: feet at
        # CHEST_Y + 1 (the chest occupies CHEST_Y), which puts the eye within
        # reach of both the block below it and the chest itself.
        bot.move_to(0.5, CHEST_Y + 1.0, 0.5)
        time.sleep(0.2)

        # Put a chest in hotbar slot 0 (creative slot 36 == menu hotbar 0) and
        # place it on top of (0, CHEST_Y-1, 0).
        give(bot, 36, ITEM_CHEST, 1)
        time.sleep(0.2)
        place(bot, 0, CHEST_Y - 1, 0, 1, 1)
        packets = drain(bot, 2.0)
        upd = find(packets, CB_BLOCK_UPDATE)
        assert upd is not None, f"chest place produced no block update: {[(hex(p)) for p, _ in packets]}"
        state_id, _ = rd_varint(upd, 8)
        assert state_id != 0, "the placed chest must not be air"
        print(f"  chest placed (state {state_id})")

        # Right-click the chest: open_screen + the whole menu content.
        place(bot, 0, CHEST_Y, 0, 1, 2)
        packets = drain_reply(bot, CB_OPEN_SCREEN)
        opened = find(packets, CB_OPEN_SCREEN)
        assert opened is not None, f"no open_screen: {[(hex(p)) for p, _ in packets]}"
        window, menu_id, _ = parse_open_screen(opened)
        assert menu_id == MENU_GENERIC_9X3, f"expected generic_9x3 (2), got menu {menu_id}"
        assert window == 1, f"first container window must be id 1, got {window}"
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "open_screen without container_set_content"
        cw, state, slots, cursor = decode_content(content)
        assert cw == window, f"content window {cw} != open window {window}"
        assert len(slots) == MENU_SLOTS, f"expected {MENU_SLOTS} slots, got {len(slots)}"
        assert all(s == (0, 0) for s in slots[:CHEST_SLOTS]), "a fresh chest must be empty"
        # The chest item the player just placed is still in hotbar 0, which the
        # container menu shows at 27 (grid) + 27 (main) + 0 = slot 54.
        assert slots[54] == (ITEM_CHEST, 1), f"hotbar 0 should still hold the chest, got {slots[54]}"
        assert cursor == (0, 0), "cursor must start empty"
        print(f"  opened window {window} (menu {menu_id}, {len(slots)} slots, state {state})")

        # The player closing a window is *not* answered with a clientbound close:
        # vanilla's handleContainerClose runs ServerPlayer.doCloseContainer(),
        # and only the server-initiated closeContainer() sends the packet. An
        # echo here is not harmless — the client applies any close to whatever
        # screen it currently has, so the echo closed the inventory screen the
        # client opens next (vanilla: pressing E on a chest closes the chest and
        # opens your inventory).
        bot.send(SB_CLOSE_WINDOW, varint(window))
        packets = drain(bot, 2.0)
        assert find(packets, CB_CONTAINER_CLOSE) is None, (
            "a player-initiated close must not be echoed: "
            f"{[(hex(p)) for p, _ in packets]}")
        place(bot, 0, CHEST_Y, 0, 1, 3)
        packets = drain_reply(bot, CB_OPEN_SCREEN)
        opened = find(packets, CB_OPEN_SCREEN)
        assert opened is not None, "reopening the chest produced no open_screen"
        window, menu_id, _ = parse_open_screen(opened)
        assert window == 2, f"the second window must be id 2, got {window}"
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "reopening the chest produced no content"
        cw, state, slots, _ = decode_content(content)
        assert cw == window, f"content window {cw} != open window {window}"
        print(f"  reopened as window {window} (state {state})")

        # Give 64 stone in hotbar slot 0 by writing the creative slot (menu slot
        # 36), exactly as the creative inventory screen does. The echo must land
        # on the *player menu* (window 0) at the InventoryMenu slot, never on the
        # open chest window at 54: the creative inventory screen is the only
        # sender of set_creative_mode_slot, and a window-0 packet is applied by
        # the client to its own 46-slot inventoryMenu, so a 54 there is an
        # IndexOutOfBoundsException inside the packet handler that drops the
        # connection.
        give(bot, 36, ITEM_STONE, 64)
        packets = drain_reply(bot, CB_CONTAINER_SET_SLOT, limit=5.0)
        slot_upd = find(packets, CB_CONTAINER_SET_SLOT)
        assert slot_upd is not None, f"stone give produced no set_slot: {[(hex(p)) for p, _ in packets]}"
        uw, slot = slot_upd[0], None
        _, i = rd_varint(slot_upd, 1)
        slot = struct.unpack(">h", slot_upd[i:i + 2])[0]
        stack, _ = read_stack(slot_upd, i + 2)
        assert uw == 0, f"a creative slot write must sync on the player menu (0), got window {uw}"
        assert slot == 36, f"hotbar 0 is player menu slot 36, got {slot}"
        assert stack == (ITEM_STONE, 64), f"expected 64 stone on the client, got {stack}"
        print(f"  stone synced to the player menu at slot {slot}")

        # That write also means the creative screen was the client's screen, so
        # our window is stale and was dropped: a click aimed at it is ignored
        # (no resync of a window the client no longer shows) and the chest has
        # to be opened again.
        bot.send(SB_CONTAINER_CLICK, click(window, 1, 54, 0, 1))
        packets = drain(bot, 1.0)
        assert find(packets, CB_CONTAINER_SET_CONTENT) is None, (
            "a click on a window we already dropped must be ignored: "
            f"{[(hex(p)) for p, _ in packets]}")
        place(bot, 0, CHEST_Y, 0, 1, 4)
        packets = drain_reply(bot, CB_OPEN_SCREEN)
        opened = find(packets, CB_OPEN_SCREEN)
        assert opened is not None, "the chest must reopen after the creative write"
        window, _, _ = parse_open_screen(opened)
        assert window == 3, f"the third window must be id 3, got {window}"
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "reopening the chest produced no content"
        cw, state, slots, _ = decode_content(content)
        assert cw == window, f"content window {cw} != open window {window}"
        print(f"  reopened as window {window} after the creative write")

        # Shift-click it into the chest: mode 1 (QUICK_MOVE) on menu slot 54.
        bot.send(SB_CONTAINER_CLICK, click(window, state, 54, 0, 1))
        packets = drain(bot, 2.0)
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, ("shift-click must resync the whole menu: "
                                     f"{[(hex(p)) for p, _ in packets]}")
        _, _, slots, cursor = decode_content(content)
        assert slots[0] == (ITEM_STONE, 64), f"chest slot 0 should hold 64 stone, got {slots[0]}"
        assert slots[54] == (0, 0), "the source hotbar slot must be empty"
        assert cursor == (0, 0), "the cursor must not hold anything after a shift-click"
        print("  shift-click moved 64 stone into chest slot 0")

        # --- the window id is its own field, and cycles 1..100 ---------------
        # This is the regression that kicked a real client. open_screen used to
        # write the *menu* id first and the window id second, so the client read
        # the window id as a menu-registry id. The registry has 25 entries, so
        # the 26th window opened in a session threw inside the client's packet
        # handler ("Failed to decode packet 'clientbound/minecraft:open_screen'")
        # and dropped the connection. Push the counter past that boundary and
        # check both ids on every open: the window id stays in 1..100 (vanilla's
        # nextContainerCounter) and the menu id is generic_9x3 (2) no matter how
        # many windows have been opened.
        # The chest is still open from the shift-click. A right-click on a
        # container we already have open deliberately re-states it *without* an
        # open_screen, so close it first and every iteration below is a fresh
        # open.
        bot.send(SB_CLOSE_WINDOW, varint(window))
        drain(bot, 0.5)
        print("  cycling the window counter past the menu registry...")
        seen_windows = []
        for n in range(30):
            place(bot, 0, CHEST_Y, 0, 1, 10 + n)
            packets = drain_reply(bot, CB_OPEN_SCREEN, limit=5.0)
            opened = find(packets, CB_OPEN_SCREEN)
            assert opened is not None, f"open #{n} produced no open_screen"
            window, menu_id, _ = parse_open_screen(opened)
            assert menu_id == MENU_GENERIC_9X3, (
                f"open #{n} must still be generic_9x3 (2), got {menu_id}")
            content = find(packets, CB_CONTAINER_SET_CONTENT)
            assert content is not None, f"open #{n} produced no content"
            cw, _, _, _ = decode_content(content)
            assert cw == window, (
                f"open #{n}: content window {cw} != open window {window} — the "
                "client addresses its clicks with the window id it was given")
            seen_windows.append(window)
            bot.send(SB_CLOSE_WINDOW, varint(window))
            drain(bot, 0.4)
        assert max(seen_windows) <= 100, f"window ids must stay <= 100: {seen_windows}"
        assert len(set(seen_windows)) > 1, "the window counter never advanced"
        print(f"  30 opens all decoded cleanly; window ids {seen_windows[0]}.."
              f"{seen_windows[-1]} (max {max(seen_windows)})")

        # Let the autosave flush the column so it is *clean*, then make a second
        # change that touches only the container, and shut the server down with
        # the window still open. That is the case the write-back has to handle on
        # its own: with no block dirty, nothing else will flush the chunk, so a
        # missing mark loses the items.
        print("  waiting for an autosave so the column goes clean...")
        time.sleep(2.0)
        give(bot, 36, ITEM_DIRT, 64)
        packets = drain_reply(bot, CB_CONTAINER_SET_SLOT, limit=5.0)
        slot_upd = find(packets, CB_CONTAINER_SET_SLOT)
        assert slot_upd is not None, "second give produced no set_slot"
        assert slot_upd[0] == 0, "a creative slot write always answers on window 0"
        # Same as above: the write dropped our window, so reopen and click.
        place(bot, 0, CHEST_Y, 0, 1, 5)
        packets = drain_reply(bot, CB_OPEN_SCREEN)
        opened = find(packets, CB_OPEN_SCREEN)
        assert opened is not None, "the chest must reopen before the second click"
        window, _, _ = parse_open_screen(opened)
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "reopening the chest produced no content"
        _, state, _, _ = decode_content(content)
        bot.send(SB_CONTAINER_CLICK, click(window, state, 54, 0, 1))
        packets = drain(bot, 2.0)
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "the second shift-click must resync the menu"
        _, _, slots, _ = decode_content(content)
        assert slots[0] == (ITEM_STONE, 64), f"stone must stay in slot 0, got {slots[0]}"
        assert slots[1] == (ITEM_DIRT, 64), f"dirt should land in slot 1, got {slots[1]}"
        print("  shift-click moved 64 dirt into chest slot 1 (container-only change)")

        # --- every component a stack can carry ---------------------------------
        # A stack is more than an id and a count: its damage, its enchantments,
        # its potion contents and its trim all ride in the DataComponentPatch,
        # and the container codec has to keep each one. The creative slot write
        # is the lever here because its patch values are length-prefixed, which
        # is the shape a *client* sends (see write_stack_components).
        print("-> an enchanted, damaged, trimmed piece must persist with its trim")
        piece = (component_damage(42),
                 component_enchantments([(ENCH_PROTECTION, 4), (ENCH_UNBREAKING, 3)]),
                 component_trim(TRIM_MATERIAL_NETHERITE, TRIM_PATTERN_EYE))
        give(bot, 36, ITEM_IRON_CHESTPLATE, 1, *piece)
        packets = drain_reply(bot, CB_CONTAINER_SET_SLOT, limit=5.0)
        slot_upd = find(packets, CB_CONTAINER_SET_SLOT)
        assert slot_upd is not None, "the enchanted give produced no set_slot"
        # The echo is the *clientbound* form of the very same patch, so decoding
        # it here checks the writing side of each component against what this
        # test chose, before any of it reaches the disk.
        _, i = rd_varint(slot_upd, 1)
        assert struct.unpack(">h", slot_upd[i:i + 2])[0] == 36, "hotbar 0 is menu slot 36"
        echoed, _ = read_stack_full(slot_upd, i + 2)
        assert echoed == {
            "item": ITEM_IRON_CHESTPLATE, "count": 1, "damage": 42,
            "enchantments": {ENCH_PROTECTION: 4, ENCH_UNBREAKING: 3},
            "trim": (TRIM_MATERIAL_NETHERITE, TRIM_PATTERN_EYE),
        }, f"the enchanted give echoed {echoed}"
        print("  the piece echoed with its damage, both enchantments and its trim")

        # The write dropped our window, so reopen the chest and shift-click the
        # piece in: the chest takes it in its first empty slot, which is 2.
        place(bot, 0, CHEST_Y, 0, 1, 6)
        packets = drain_reply(bot, CB_OPEN_SCREEN)
        opened = find(packets, CB_OPEN_SCREEN)
        assert opened is not None, "the chest must reopen after the enchanted give"
        window, _, _ = parse_open_screen(opened)
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "reopening the chest produced no content"
        _, state, slots, _ = decode_content_full(content)
        assert slots[2].get("count", 0) == 0, f"chest slot 2 must be free, got {slots[2]}"
        bot.send(SB_CONTAINER_CLICK, click(window, state, 54, 0, 1))
        packets = drain(bot, 2.0)
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "the enchanted shift-click must resync the menu"
        _, _, slots, _ = decode_content_full(content)
        assert slots[2].get("damage") == 42, f"the piece lost its damage: {slots[2]}"
        assert slots[2].get("trim") == (TRIM_MATERIAL_NETHERITE, TRIM_PATTERN_EYE), (
            f"chest slot 2 must hold the trimmed piece, got {slots[2]}")
        print("  shift-click moved the piece into chest slot 2, trim and all")

        # A potion with a custom effect that hides a shorter one: the nested
        # Details is the part of the component that a shallow codec drops.
        print("-> a potion's custom effects (and the layer one hides) must persist")
        brew = (component_potion_contents(
            potion=POTION_WATER,
            effects=[{"effect": EFFECT_POISON, "amplifier": 1, "duration": 200,
                      "ambient": True, "show_particles": False, "show_icon": True,
                      "hidden": {"amplifier": 0, "duration": 60}}]),)
        give(bot, 36, ITEM_POTION, 1, *brew)
        packets = drain_reply(bot, CB_CONTAINER_SET_SLOT, limit=5.0)
        slot_upd = find(packets, CB_CONTAINER_SET_SLOT)
        assert slot_upd is not None, "the potion give produced no set_slot"
        _, i = rd_varint(slot_upd, 1)
        echoed, _ = read_stack_full(slot_upd, i + 2)
        assert echoed.get("potion_contents") == {
            "potion": POTION_WATER,
            "custom_effects": [{
                "effect": EFFECT_POISON, "amplifier": 1, "duration": 200,
                "ambient": True, "show_particles": False, "show_icon": True,
                "hidden_effect": {"amplifier": 0, "duration": 60, "ambient": False,
                                  "show_particles": True, "show_icon": True},
            }],
        }, f"the potion give echoed {echoed}"
        print("  the potion echoed with its custom effect and its hidden layer")
        place(bot, 0, CHEST_Y, 0, 1, 7)
        packets = drain_reply(bot, CB_OPEN_SCREEN)
        opened = find(packets, CB_OPEN_SCREEN)
        assert opened is not None, "the chest must reopen after the potion give"
        window, _, _ = parse_open_screen(opened)
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "reopening the chest produced no content"
        _, state, slots, _ = decode_content_full(content)
        assert slots[3].get("count", 0) == 0, f"chest slot 3 must be free, got {slots[3]}"
        bot.send(SB_CONTAINER_CLICK, click(window, state, 54, 0, 1))
        packets = drain(bot, 2.0)
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "the potion shift-click must resync the menu"
        _, _, slots, _ = decode_content_full(content)
        assert slots[3].get("potion_contents") is not None, (
            f"chest slot 3 must hold the potion, got {slots[3]}")
        print("  shift-click moved the potion into chest slot 3")

        # --- AbstractContainerMenu.stillValid --------------------------------
        # A menu is a *place*, and vanilla re-checks it on every player tick:
        # `if (!this.containerMenu.stillValid(this)) closeContainer()`
        # (ServerPlayer.doTick line 700), where stillValid is
        # `isWithinBlockInteractionRange(pos, 4.0)` (AbstractContainerMenu line
        # 119) — 8.5 blocks from the eye to the block. `handle_window_click`
        # checks the window id and the state id instead, which says nothing about
        # where the player is, so without this the chest stays clickable from
        # anywhere in the world.
        print("-> walking away from the chest must close it")
        far_x = 40.5
        for _ in range(4):
            bot.move_to(far_x, CHEST_Y + 1.0, 0.5)
            time.sleep(0.06)
        packets = drain(bot, 1.0)
        assert find(packets, CB_CONTAINER_CLOSE) is not None, (
            f"walking 40 blocks away must close the chest: {[hex(p) for p, _ in packets]}")
        print("  OK the chest closed once the player was out of range")
        # ...and a click on the window we have dropped is ignored outright: no
        # slot update, no content resync, no cursor. `handle_window_click`'s
        # window-id check is what drops it (the window the client names is not the
        # one we have open), and the resync the *stale*-revision path sends is
        # deliberately not reached, so nothing at all comes back.
        bot.send(SB_CONTAINER_CLICK, click(1, 0, 0, 0, 0))
        packets = drain(bot, 1.0)
        dropped = [b for p, b in packets
                   if p in (CB_CONTAINER_SET_CONTENT, CB_CONTAINER_SET_SLOT)
                   and rd_varint(b, 0)[0] == 1]
        assert not dropped, f"a click on the closed window must be dropped: {dropped}"
        print("  OK a click on the closed window was dropped (no answer for window 1)")
        # Walk back for the persistence phase below, and reopen the chest.
        for _ in range(3):
            bot.move_to(0.5, CHEST_Y + 1.0, 0.5)
            time.sleep(0.06)
        drain(bot, 0.5)
        place(bot, 0, CHEST_Y, 0, 1, 3)
        packets = drain_reply(bot, CB_OPEN_SCREEN)
        assert find(packets, CB_OPEN_SCREEN) is not None, "the chest must reopen"
        print("  OK the chest reopened at home")

        # No close: the shutdown flush must persist the click on its own.
        bot.close()
        time.sleep(0.5)
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            print("FAIL: server did not exit on SIGTERM", file=sys.stderr)
            return 1
        proc = None

        # The chest contents must be in the region file's block-entity list.
        region = pathlib.Path(world) / "dimensions" / "minecraft" / "overworld" / "region" / "r.0.0.mca"
        if not region.exists():
            region = pathlib.Path(world) / "region" / "r.0.0.mca"
        assert region.exists(), f"no region file written under {world}"
        data = region.read_bytes()
        found_chunk = False
        for i in range(1024):
            loc = struct.unpack(">I", data[i * 4:i * 4 + 4])[0]
            sector, count = loc >> 8, loc & 0xFF
            if sector == 0 or count == 0:
                continue
            off = sector * 4096
            length = struct.unpack(">I", data[off:off + 4])[0]
            payload = data[off + 5:off + 4 + length]
            try:
                nbt_bytes = zlib.decompress(payload)
            except zlib.error:
                continue
            if b"minecraft:chest" not in nbt_bytes:
                continue
            found_chunk = True
            assert b"Items" in nbt_bytes, "the chest's block entity must carry its Items list"
            assert b"minecraft:stone" in nbt_bytes, "the stone must be in the block entity"
            # The components are in the block entity in vanilla's own NBT shapes,
            # by resource location — which is also what ties the registry ids
            # written over the wire to the names they actually mean.
            for needle in (b"minecraft:enchantments", b"minecraft:protection",
                           b"minecraft:unbreaking", b"minecraft:trim",
                           b"minecraft:netherite", b"minecraft:eye",
                           b"minecraft:potion_contents", b"hidden_effect"):
                assert needle in nbt_bytes, (
                    f"the block entity must carry {needle.decode()} by name")
            break
        assert found_chunk, "no chunk in the region file holds a chest block entity"
        print("  region file carries the chest block entity with its items and components")

        # Second boot on the same world: the chest must still hold the stone.
        print("Restarting the server on the same world...")
        proc2, port2 = wire_client.start_server(
            [binary, "--host", "127.0.0.1", "--port", "0", "--motd", "container test",
             "--offline-mode", "--gamemode", "1", "--data", data_root,
             "--world", world, "--no-flat-world"], wait_listening, env=env)
        try:
            bot2 = Bot("ChestBot2", port2)
            wire_client.record(bot2, note="container-gui: the client that rejoins after the restart")
            bot2.join()
            bot2.start_reader()
            drain(bot2, 1.0)
            bot2.move_to(0.5, CHEST_Y + 1.0, 0.5)
            time.sleep(0.2)
            # Place the *same* block state check: the chest is already there, so
            # just right-click it.
            place(bot2, 0, CHEST_Y, 0, 1, 1)
            packets = drain_reply(bot2, CB_OPEN_SCREEN)
            opened = find(packets, CB_OPEN_SCREEN)
            assert opened is not None, "reopening the chest produced no open_screen"
            content = find(packets, CB_CONTAINER_SET_CONTENT)
            assert content is not None, "reopening the chest produced no content"
            _, _, slots, cursor = decode_content(content)
            assert slots[0] == (ITEM_STONE, 64), (
                f"the chest must survive a restart with 64 stone, got {slots[0]}")
            assert slots[1] == (ITEM_DIRT, 64), (
                f"the container-only change must also survive, got {slots[1]}")
            # ...and the component-carrying stacks came back whole: the same
            # bytes, decoded one level deeper (each component, not just the id
            # and the count).
            _, _, slots, _ = decode_content_full(content)
            assert slots[2].get("damage") == 42, (
                f"the damage must survive the restart, got {slots[2]}")
            assert slots[2].get("enchantments") == {ENCH_PROTECTION: 4, ENCH_UNBREAKING: 3}, (
                f"the enchantments must survive the restart, got {slots[2]}")
            assert slots[2].get("trim") == (TRIM_MATERIAL_NETHERITE, TRIM_PATTERN_EYE), (
                f"the trim must survive the restart, got {slots[2]}")
            assert slots[3].get("item") == ITEM_POTION, (
                f"the potion must survive the restart, got {slots[3]}")
            assert slots[3].get("potion_contents") == {
                "potion": POTION_WATER,
                "custom_effects": [{
                    "effect": EFFECT_POISON, "amplifier": 1, "duration": 200,
                    "ambient": True, "show_particles": False, "show_icon": True,
                    "hidden_effect": {"amplifier": 0, "duration": 60, "ambient": False,
                                      "show_particles": True, "show_icon": True},
                }],
            }, f"the potion contents must survive the restart, got {slots[3]}"
            print("  chest contents survived the restart, components included")
            bot2.close()
        finally:
            proc2.terminate()
            try:
                proc2.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc2.kill()
                proc2.wait()
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        shutil.rmtree(world, ignore_errors=True)

    print("\nPASS: container (open, click, components, persist across restart)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
