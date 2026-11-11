#!/usr/bin/env python3
"""26.1/26.2 (protocol 775/776) login, configuration and translated play entry.

Both releases share the 775/776 packet-id table but differ in two wire shapes:
26.1.2 Login Success has no session id and Join Game ends at the secure-chat
byte, while 26.2 appends the session id and the authentication flag.  Each
client gets its own captured configuration registry set and Update Tags body.
Every chunk is decoded and checked for non-air terrain whose block and biome ids
belong to that release's registries. The play phase checks the translated
inventory, block placement/breaking, item-entity spawn/metadata, pickup,
translated peer player tracking, and canonical-player visibility.

Usage: t0_config_test.py /path/to/plusmc
"""
import collections
import os
import pathlib
import re
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tools"))
import chunk_decode  # noqa: E402
import craft_test  # noqa: E402
import wire_client  # noqa: E402
from nbt_component import chat_text, rd_nbt_component, rd_nbt_tree  # noqa: E402

BIN = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
HOST = "127.0.0.1"
SOCKET_TIMEOUT = 30.0

LOGIN_SUCCESS = 0x02
SET_COMPRESSION = 0x03
CONFIG_BRAND = 0x01
CONFIG_FINISH = 0x03
CONFIG_REGISTRY = 0x07
CONFIG_FEATURE_FLAGS = 0x0C
CONFIG_TAGS = 0x0D
CONFIG_KNOWN_PACKS = 0x0E
PLAY_DISCONNECT = 0x20
PLAY_JOIN = 0x31
PLAY_COMMANDS = 0x10
PLAY_GAME_EVENT = 0x26
PLAY_CHUNK = 0x2D
PLAY_BATCH_START = 0x0C
PLAY_BATCH_FINISHED = 0x0B
PLAY_POSITION = 0x48
PLAY_KEEPALIVE = 0x2C
PLAY_SYSTEM_CHAT = 0x79
PLAY_MOVE_POS = 0x1E
PLAY_CHAT = 0x09
PLAY_CHAT_COMMAND = 0x07
PLAY_PLAYER_INFO_UPDATE = 0x46
PLAY_PLAYER_INFO_REMOVE = 0x45
PLAY_ENTITY_POSITION_SYNC = 0x23
# The canonical 26.3 client speaks protocol 777, whose play-state ids are not
# the same as the translated 26.1/26.2 ids above.
CANONICAL_ADD_ENTITY = 0x01
CANONICAL_ENTITY_POSITION_SYNC = 0x23
CANONICAL_PLAYER_INFO_UPDATE = 0x47
CANONICAL_PLAYER_INFO_REMOVE = 0x46
CANONICAL_REMOVE_ENTITIES = 0x4E
CANONICAL_SET_ENTITY_DATA = 0x65
CANONICAL_SET_EQUIPMENT = 0x68
PLAY_CONTAINER_SET_CONTENT = 0x12
PLAY_BLOCK_ACK = 0x04
PLAY_BLOCK_UPDATE = 0x08
PLAY_ADD_ENTITY = 0x01
PLAY_SET_ENTITY_DATA = 0x63
PLAY_SET_EQUIPMENT = 0x66
PLAY_REMOVE_ENTITIES = 0x4D
PLAY_TAKE_ITEM = 0x7C
PLAY_SET_CARRIED = 0x35
PLAY_USE_ITEM_ON = 0x42
PLAY_PLAYER_ACTION = 0x29
PLAY_OPEN_SCREEN = 0x3B
PLAY_CONTAINER_SET_SLOT = 0x14
PLAY_CONTAINER_SET_DATA = 0x13
PLAY_MERCHANT_OFFERS = 0x34
PLAY_SET_CURSOR_ITEM = 0x60
PLAY_SET_EXPERIENCE = 0x67
SB_CONTAINER_BUTTON_CLICK = 0x11
SB_CONTAINER_CLICK = 0x12
SB_CONTAINER_CLOSE = 0x13
SB_INTERACT = 0x1A
SB_SELECT_TRADE = 0x33
SB_PLAYER_INPUT = 0x2B
SB_SET_CARRIED = 0x36
SB_KEEP_ALIVE = 0x1C
CANONICAL_PLAYER_ENTITY_TYPE = 159

# Target-release data component ids. Damage, enchantments and the opaque
# enchantment_glint_override kept their 26.3 ids; potion_contents and trim moved
# down by two.
COMPONENT_DAMAGE = 3
COMPONENT_ENCHANTMENTS = 13
COMPONENT_ENCHANTMENT_GLINT_OVERRIDE = 21
COMPONENT_POTION_CONTENTS = 51
COMPONENT_TRIM = 56

DATA_ROOT = pathlib.Path(__file__).resolve().parent.parent / "data" / "vanilla"
CASES = [
    {
        "protocol": 775,
        "name": "T0Cap261",
        "pack": "26.1",
        "packs": ["26.1", "26.1.1", "26.1.2"],
        "registries": 28,
        "biomes": 65,
        "block_max": 29872,
        "item_max": 1505,
        "entity_max": 156,
        "item_entity": 71,
        "player": 155,
        "villager": 139,
        "join_tail": b"\x00",
    },
    {
        "protocol": 775,
        "name": "T0Cap2611",
        "pack": "26.1.1",
        "packs": ["26.1", "26.1.1", "26.1.2"],
        "registries": 28,
        "biomes": 65,
        "block_max": 29872,
        "item_max": 1505,
        "entity_max": 156,
        "item_entity": 71,
        "player": 155,
        "villager": 139,
        "join_tail": b"\x00",
    },
    {
        "protocol": 775,
        "name": "T0Cap2612",
        "pack": "26.1.2",
        "packs": ["26.1", "26.1.1", "26.1.2"],
        "registries": 28,
        "biomes": 65,
        "block_max": 29872,
        "item_max": 1505,
        "entity_max": 156,
        "item_entity": 71,
        "player": 155,
        "villager": 139,
        "join_tail": b"\x00",
    },
    {
        "protocol": 776,
        "name": "T0Cap262",
        "pack": "26.2",
        "packs": ["26.2"],
        "registries": 29,
        "biomes": 66,
        "block_max": 32365,
        "item_max": 1536,
        "entity_max": 157,
        "item_entity": 71,
        "player": 156,
        "villager": 140,
        "join_tail": b"\x00\x00",
    },
]


def varint(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            b |= 0x80
        out.append(b)
        if not n:
            return bytes(out)


def rd_varint(b: bytes, i: int = 0):
    value = 0
    for k in range(5):
        byte = b[i]
        i += 1
        value |= (byte & 0x7F) << (7 * k)
        if not (byte & 0x80):
            return value, i
    raise ValueError("bad varint")


def rd_u32(b: bytes, i: int = 0):
    return struct.unpack(">I", b[i:i + 4])[0], i + 4


def rd_string(b: bytes, i: int = 0):
    n, i = rd_varint(b, i)
    return b[i:i + n].decode(), i + n


def recvn(sock, n: int) -> bytes:
    out = b""
    while len(out) < n:
        chunk = sock.recv(n - len(out))
        if not chunk:
            raise EOFError(f"unexpected EOF (wanted {n}, got {len(out)})")
        out += chunk
    return out


def read_varint_sock(sock):
    out = bytearray()
    while True:
        c = recvn(sock, 1)
        out += c
        if not c[0] & 0x80:
            return bytes(out), len(out)


def frame(payload: bytes, threshold: int) -> bytes:
    if threshold < 0:
        return varint(len(payload)) + payload
    if len(payload) < threshold:
        body = varint(0) + payload
    else:
        body = varint(len(payload)) + zlib.compress(payload)
    return varint(len(body)) + body


def read_packet(sock, threshold: int):
    raw, n = read_varint_sock(sock)
    length, _ = rd_varint(raw)
    if threshold < 0:
        data = recvn(sock, length)
    else:
        dlen_raw, dn = read_varint_sock(sock)
        dlen, _ = rd_varint(dlen_raw)
        data = recvn(sock, length - dn)
        if dlen:
            data = zlib.decompress(data)
    pid, off = rd_varint(data)
    if pid == PLAY_KEEPALIVE:
        # The server drops a client that never answers its keep-alive; echo the
        # i64 back so a long menu run does not outlive its timeout.
        send(sock, threshold, SB_KEEP_ALIVE, data[off:off + 8])
    return pid, data[off:]


_SEND_LOCK = threading.Lock()


def send(sock, threshold: int, pid: int, body: bytes = b""):
    # A keep-alive heartbeat runs beside the test's own sends; one frame at a
    # time, or two sendall calls can interleave their bytes on the socket.
    with _SEND_LOCK:
        sock.sendall(frame(varint(pid) + body, threshold))


SERVER_LOG = collections.deque(maxlen=400)


def wait_listening(proc: subprocess.Popen) -> int:
    # Startup opens the world before it binds, so full-suite load can delay the
    # line; wait for the event rather than a wall-clock ceiling. EOF is the
    # deterministic failure, and CTest's overall timeout catches a true hang.
    assert proc.stdout is not None
    port = None
    while port is None:
        line = proc.stdout.readline()
        if not line:
            raise AssertionError("plusmc exited before listening")
        SERVER_LOG.append(line)
        match = re.search(rb"listening on [\d.]+:(\d+)", line)
        if match:
            port = int(match.group(1))

    # The server logs to stderr, merged into this pipe. A run this long fills
    # the pipe buffer, and a server blocked in a log write stops answering
    # every client, so keep draining it for the rest of the run.
    def pump():
        for raw in iter(proc.stdout.readline, b""):
            SERVER_LOG.append(raw)

    threading.Thread(target=pump, daemon=True).start()
    return port


def parse_join(body: bytes):
    i = 0
    entity_id = struct.unpack(">i", body[i:i + 4])[0]
    i += 4
    hardcore = body[i]
    i += 1
    dim_count, i = rd_varint(body, i)
    dimensions = []
    for _ in range(dim_count):
        dimension, i = rd_string(body, i)
        dimensions.append(dimension)
    max_players, i = rd_varint(body, i)
    view_distance, i = rd_varint(body, i)
    simulation_distance, i = rd_varint(body, i)
    reduced_debug, respawn_screen, limited_crafting = body[i], body[i + 1], body[i + 2]
    i += 3
    dim_type, i = rd_varint(body, i)
    dimension, i = rd_string(body, i)
    hashed_seed = struct.unpack(">q", body[i:i + 8])[0]
    i += 8
    gamemode, previous_gamemode, is_debug, is_flat, has_death = body[i:i + 5]
    i += 5
    portal_cooldown, i = rd_varint(body, i)
    sea_level, i = rd_varint(body, i)
    tail = body[i:]
    return {
        "entity_id": entity_id,
        "hardcore": hardcore,
        "dimensions": dimensions,
        "max_players": max_players,
        "view_distance": view_distance,
        "simulation_distance": simulation_distance,
        "reduced_debug": reduced_debug,
        "respawn_screen": respawn_screen,
        "limited_crafting": limited_crafting,
        "dim_type": dim_type,
        "dimension": dimension,
        "hashed_seed": hashed_seed,
        "gamemode": gamemode,
        "previous_gamemode": previous_gamemode,
        "is_debug": is_debug,
        "is_flat": is_flat,
        "has_death": has_death,
        "portal_cooldown": portal_cooldown,
        "sea_level": sea_level,
        "tail": tail,
    }


def pack_block_pos(x: int, y: int, z: int) -> int:
    return (((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF)) & 0xFFFFFFFFFFFFFFFF


def decode_effect_details(body: bytes, i: int):
    amplifier, i = rd_varint(body, i)
    duration, i = rd_varint(body, i)
    ambient, i = body[i], i + 1
    visible, i = body[i], i + 1
    show_icon, i = body[i], i + 1
    has_hidden, i = body[i], i + 1
    hidden = None
    if has_hidden:
        hidden, i = decode_effect_details(body, i)
    return {
        "amplifier": amplifier,
        "duration": duration,
        "ambient": bool(ambient),
        "visible": bool(visible),
        "show_icon": bool(show_icon),
        "hidden": hidden,
    }, i


def decode_effect_instance(body: bytes, i: int):
    effect, i = rd_varint(body, i)
    details, i = decode_effect_details(body, i)
    return {"effect": effect, **details}, i


def decode_potion_contents(body: bytes, i: int):
    has_potion, i = body[i], i + 1
    potion = None
    if has_potion:
        potion, i = rd_varint(body, i)
    has_color, i = body[i], i + 1
    color = None
    if has_color:
        color, i = rd_u32(body, i)
    count, i = rd_varint(body, i)
    effects = []
    for _ in range(count):
        effect, i = decode_effect_instance(body, i)
        effects.append(effect)
    has_name, i = body[i], i + 1
    name = None
    if has_name:
        name, i = rd_string(body, i)
    return {"potion": potion, "color": color, "effects": effects, "name": name}, i


def decode_item_stack(body: bytes, i: int):
    count, i = rd_varint(body, i)
    if count == 0:
        return None, i
    item, i = rd_varint(body, i)
    added, i = rd_varint(body, i)
    removed, i = rd_varint(body, i)
    assert removed == 0, removed
    components = []
    for _ in range(added):
        component_id, i = rd_varint(body, i)
        if component_id == COMPONENT_DAMAGE:
            value, i = rd_varint(body, i)
            components.append({"type": "damage", "value": value})
        elif component_id == COMPONENT_ENCHANTMENTS:
            entries, i = rd_varint(body, i)
            enchants = []
            for _ in range(entries):
                enchantment, i = rd_varint(body, i)
                level, i = rd_varint(body, i)
                enchants.append((enchantment, level))
            components.append({"type": "enchantments", "entries": enchants})
        elif component_id == COMPONENT_POTION_CONTENTS:
            contents, i = decode_potion_contents(body, i)
            components.append({"type": "potion_contents", **contents})
        elif component_id == COMPONENT_TRIM:
            material, i = rd_varint(body, i)
            pattern, i = rd_varint(body, i)
            components.append({"type": "trim", "material": material - 1,
                               "pattern": pattern - 1})
        elif component_id == COMPONENT_ENCHANTMENT_GLINT_OVERRIDE:
            value = body[i]
            i += 1
            components.append({"type": "enchantment_glint_override",
                               "value": bool(value)})
        else:
            raise AssertionError(f"unexpected translated component id {component_id}")
    return {"item": item, "count": count, "components": components}, i


def decode_container(body: bytes):
    window, i = rd_varint(body)
    state, i = rd_varint(body, i)
    count, i = rd_varint(body, i)
    slots = []
    for _ in range(count):
        stack, i = decode_item_stack(body, i)
        slots.append(stack)
    cursor, i = decode_item_stack(body, i)
    assert i == len(body), (i, len(body))
    return {"window": window, "state": state, "slots": slots, "cursor": cursor}


def decode_open_screen(body: bytes):
    window, i = rd_varint(body)
    menu, i = rd_varint(body, i)
    node, i = rd_nbt_tree(body, i)
    assert i == len(body), (i, len(body))
    # Most menus use a literal title; the smithing table uses vanilla's
    # `container.upgrade` translation key.
    title = node.get("text", node.get("translate"))
    assert title is not None, node
    return window, menu, title


def decode_set_slot(body: bytes):
    window, i = rd_varint(body)
    state, i = rd_varint(body, i)
    slot = struct.unpack(">h", body[i:i + 2])[0]
    i += 2
    stack, i = decode_item_stack(body, i)
    assert i == len(body), (i, len(body))
    return window, state, slot, stack


def send_menu_click(sock, threshold, window, state, slot, button, click_type, carried=None):
    if carried is None:
        hashed = b"\x00"
    else:
        hashed = (b"\x01" + varint(carried["item"]) + varint(carried["count"]) +
                  varint(0) + varint(0))
    body = (varint(window) + varint(state) + struct.pack(">h", slot) +
            bytes([button & 0xFF]) + varint(click_type) + varint(0) + hashed)
    send(sock, threshold, SB_CONTAINER_CLICK, body)


def send_use_item_on(sock, threshold, x, y, z, face, sequence):
    body = (varint(0) + struct.pack(">q", pack_block_pos(x, y, z)) + varint(face) +
            struct.pack(">fff", 0.5, 0.5, 0.5) + b"\x00\x00" + varint(sequence))
    send(sock, threshold, PLAY_USE_ITEM_ON, body)


def read_menu_content(sock, threshold, window=None):
    while True:
        pid, body = read_packet(sock, threshold)
        if pid == PLAY_CONTAINER_SET_CONTENT:
            content = decode_container(body)
            if window is None or content["window"] == window:
                return content
        elif pid == PLAY_DISCONNECT:
            reason, _ = rd_nbt_component(body)
            raise AssertionError(f"disconnected while waiting for menu content: {reason}")


def read_open_menu(sock, threshold, menu_id):
    window = None
    while True:
        pid, body = read_packet(sock, threshold)
        if pid == PLAY_OPEN_SCREEN:
            open_window, menu, title = decode_open_screen(body)
            assert menu == menu_id, (menu, menu_id, title)
            window = open_window
        elif pid == PLAY_CONTAINER_SET_CONTENT and window is not None:
            content = decode_container(body)
            if content["window"] == window:
                return window, content
        elif pid == PLAY_DISCONNECT:
            reason, _ = rd_nbt_component(body)
            raise AssertionError(f"disconnected while opening menu {menu_id}: {reason}")


def read_slots(sock, threshold, wanted):
    got = {}
    state = None
    while not wanted.issubset(got):
        pid, body = read_packet(sock, threshold)
        if pid == PLAY_CONTAINER_SET_SLOT:
            window, new_state, slot, stack = decode_set_slot(body)
            assert window == 0, window
            got[slot] = stack
            state = new_state
        elif pid == PLAY_CONTAINER_SET_CONTENT:
            state = decode_container(body)["state"]
        elif pid == PLAY_DISCONNECT:
            reason, _ = rd_nbt_component(body)
            raise AssertionError(f"disconnected while waiting for slots {sorted(wanted)}: {reason}")
    return got, state


def read_cursor(sock, threshold):
    while True:
        pid, body = read_packet(sock, threshold)
        if pid == PLAY_SET_CURSOR_ITEM:
            stack, i = decode_item_stack(body, 0)
            assert i == len(body), (i, len(body))
            # Every click ends with a cursor update, including clicks that do
            # not move the cursor. Skip an empty one from the previous click
            # and wait for the non-empty stack this take puts on it.
            if stack is not None:
                return stack
        elif pid == PLAY_CONTAINER_SET_SLOT:
            pass
        elif pid == PLAY_CONTAINER_SET_CONTENT:
            pass
        elif pid == PLAY_DISCONNECT:
            reason, _ = rd_nbt_component(body)
            raise AssertionError(f"disconnected while waiting for cursor: {reason}")


def decode_container_data(body: bytes):
    window, i = rd_varint(body)
    data_slot, value = struct.unpack(">hh", body[i:i + 4])
    i += 4
    assert i == len(body), (i, len(body))
    return window, data_slot, value


def decode_set_experience(body: bytes):
    progress = struct.unpack(">f", body[:4])[0]
    level, i = rd_varint(body, 4)
    total, i = rd_varint(body, i)
    assert i == len(body), (i, len(body))
    return progress, level, total


def read_menu_data(sock, threshold, window, wanted):
    got = {}
    while not wanted.issubset(got):
        pid, body = read_packet(sock, threshold)
        if pid == PLAY_CONTAINER_SET_DATA:
            got_window, data_slot, value = decode_container_data(body)
            if got_window == window and data_slot in wanted:
                got[data_slot] = value
        elif pid == PLAY_DISCONNECT:
            reason, _ = rd_nbt_component(body)
            raise AssertionError(f"disconnected while waiting for menu data: {reason}")
    return got


def read_content_and_data(sock, threshold, window, data_slots):
    content = None
    data = {}
    while content is None or not set(data_slots).issubset(data):
        pid, body = read_packet(sock, threshold)
        if pid == PLAY_CONTAINER_SET_CONTENT:
            got = decode_container(body)
            if got["window"] == window:
                content = got
        elif pid == PLAY_CONTAINER_SET_DATA:
            got_window, data_slot, value = decode_container_data(body)
            if got_window == window and data_slot in data_slots:
                data[data_slot] = value
        elif pid == PLAY_DISCONNECT:
            reason, _ = rd_nbt_component(body)
            raise AssertionError(f"disconnected while waiting for menu content/data: {reason}")
    return content, data


def read_enchant_result(sock, threshold, window):
    slots = {}
    data = {}
    xp = None
    while not (set(slots) >= {0, 1} and set(data) >= {0, 1, 2, 4} and xp is not None):
        pid, body = read_packet(sock, threshold)
        if pid == PLAY_CONTAINER_SET_SLOT:
            got_window, _, slot, stack = decode_set_slot(body)
            if got_window == window:
                slots[slot] = stack
        elif pid == PLAY_CONTAINER_SET_DATA:
            got_window, data_slot, value = decode_container_data(body)
            if got_window == window:
                data[data_slot] = value
        elif pid == PLAY_SET_EXPERIENCE:
            xp = decode_set_experience(body)
        elif pid == PLAY_CONTAINER_SET_CONTENT:
            got = decode_container(body)
            if got["window"] == window:
                slots[0] = got["slots"][0]
                slots[1] = got["slots"][1]
        elif pid == PLAY_DISCONNECT:
            reason, _ = rd_nbt_component(body)
            raise AssertionError(f"disconnected while enchanting: {reason}")
    return slots, data, xp


def read_cursor_and_experience(sock, threshold):
    cursor = None
    xp = None
    while cursor is None or xp is None:
        pid, body = read_packet(sock, threshold)
        if pid == PLAY_SET_CURSOR_ITEM:
            stack, i = decode_item_stack(body, 0)
            assert i == len(body), (i, len(body))
            if stack is not None:
                cursor = stack
        elif pid == PLAY_SET_EXPERIENCE:
            xp = decode_set_experience(body)
        elif pid in (PLAY_CONTAINER_SET_SLOT, PLAY_CONTAINER_SET_CONTENT,
                     PLAY_CONTAINER_SET_DATA):
            pass
        elif pid == PLAY_DISCONNECT:
            reason, _ = rd_nbt_component(body)
            raise AssertionError(f"disconnected while taking anvil result: {reason}")
    return cursor, xp


def send_container_button(sock, threshold, window, button):
    send(sock, threshold, SB_CONTAINER_BUTTON_CLICK, varint(window) + varint(button))


def send_select_trade(sock, threshold, index):
    send(sock, threshold, SB_SELECT_TRADE, varint(index))


def send_interact(sock, threshold, entity_id):
    send(sock, threshold, SB_INTERACT,
         varint(entity_id) + varint(0) + b"\x00" + b"\x00")


def read_button_result(sock, threshold, window):
    selected = None
    result = None
    state = None
    while selected is None or result is None:
        pid, body = read_packet(sock, threshold)
        if pid == PLAY_CONTAINER_SET_DATA:
            got_window, data_slot, value = decode_container_data(body)
            if got_window == window and data_slot == 0 and value >= 0:
                selected = value
        elif pid == PLAY_CONTAINER_SET_SLOT:
            got_window, new_state, slot, stack = decode_set_slot(body)
            assert got_window == window, (got_window, window)
            state = new_state
            if slot == 1 and stack is not None:
                result = stack
        elif pid == PLAY_CONTAINER_SET_CONTENT:
            content = decode_container(body)
            assert content["window"] == window, content["window"]
            state = content["state"]
            if content["slots"][1] is not None:
                result = content["slots"][1]
        elif pid == PLAY_DISCONNECT:
            reason, _ = rd_nbt_component(body)
            raise AssertionError(f"disconnected while selecting a stonecutter recipe: {reason}")
    return selected, result, state


def decode_merchant_offers(body: bytes):
    window, i = rd_varint(body)
    count, i = rd_varint(body, i)
    offers = []
    for _ in range(count):
        cost_a_item, i = rd_varint(body, i)
        cost_a_count, i = rd_varint(body, i)
        predicate, i = rd_varint(body, i)
        assert predicate == 0, predicate
        result, i = decode_item_stack(body, i)
        has_b = body[i]
        i += 1
        cost_b = None
        if has_b:
            cost_b_item, i = rd_varint(body, i)
            cost_b_count, i = rd_varint(body, i)
            predicate, i = rd_varint(body, i)
            assert predicate == 0, predicate
            cost_b = {"item": cost_b_item, "count": cost_b_count}
        out_of_stock = body[i]
        i += 1
        uses, max_uses, xp, special_price = struct.unpack(">iiii", body[i:i + 16])
        i += 16
        multiplier = struct.unpack(">f", body[i:i + 4])[0]
        i += 4
        demand = struct.unpack(">i", body[i:i + 4])[0]
        i += 4
        offers.append({
            "cost_a": {"item": cost_a_item, "count": cost_a_count},
            "cost_b": cost_b,
            "result": result,
            "out_of_stock": bool(out_of_stock),
            "uses": uses,
            "max_uses": max_uses,
            "xp": xp,
            "special_price": special_price,
            "multiplier": multiplier,
            "demand": demand,
        })
    level, i = rd_varint(body, i)
    xp, i = rd_varint(body, i)
    show_progress = body[i]
    i += 1
    can_restock = body[i]
    i += 1
    assert i == len(body), (i, len(body))
    return {"window": window, "offers": offers, "level": level, "xp": xp,
            "show_progress": bool(show_progress), "can_restock": bool(can_restock)}


def read_merchant_window(sock, threshold, menu_id):
    window = None
    content = None
    offers = None
    seen = []
    try:
        while window is None or content is None or offers is None:
            pid, body = read_packet(sock, threshold)
            seen.append(pid)
            if pid == PLAY_OPEN_SCREEN:
                open_window, menu, title = decode_open_screen(body)
                assert menu == menu_id, (menu, menu_id, title)
                window = open_window
            elif pid == PLAY_CONTAINER_SET_CONTENT and window is not None:
                got = decode_container(body)
                if got["window"] == window:
                    content = got
            elif pid == PLAY_MERCHANT_OFFERS and window is not None:
                got = decode_merchant_offers(body)
                if got["window"] == window:
                    offers = got
            elif pid == PLAY_DISCONNECT:
                reason, _ = rd_nbt_component(body)
                raise AssertionError(f"disconnected while opening merchant menu {menu_id}: {reason}")
    except TimeoutError:
        print(f"merchant packets before timeout: {[hex(p) for p in seen]}", file=sys.stderr)
        raise
    return window, content, offers


def spawn_trader(port, x, y, z, index):
    """Summon a toolsmith with a canonical 26.3 client, then leave it behind for
    the translated client to trade with. A level-1 toolsmith's trade set is all
    one-emerald offers with 12 uses, so the exhaustion check can always pay
    every use from the starter inventory's emerald stack."""
    bot = craft_test.Bot(f"T0Trade{index}", port)
    try:
        bot.join()
        bot.start_reader()
        bot.move_to(x, y, z)
        craft_test.drain(bot, 0.5)
        command = "summon villager profession=toolsmith"
        bot.send(craft_test.SB_CHAT_COMMAND, craft_test.varint(len(command)) + command.encode())
        craft_test.drain(bot, 1.0)
        time.sleep(0.05)
    finally:
        bot.close()


def run_canonical_command(port, command, index):
    """Run a chat command as a short-lived canonical client. T0 can run
    /time and /weather itself, but /setblock and /summon still come from here."""
    bot = craft_test.Bot(f"T0Cmd{index}", port)
    try:
        bot.join()
        bot.start_reader()
        bot.move_to(0.5, 64.0, 0.5)
        craft_test.drain(bot, 0.3)
        bot.send(craft_test.SB_CHAT_COMMAND,
                 craft_test.varint(len(command)) + command.encode())
        craft_test.drain(bot, 0.8)
        time.sleep(0.05)
    finally:
        bot.close()


def send_translated_command(sock, threshold, command, context):
    """Send a 775/776 command packet and return its system-chat reply."""
    send(sock, threshold, PLAY_CHAT_COMMAND,
         varint(len(command)) + command.encode())
    while True:
        pid, body = read_packet(sock, threshold)
        if pid == PLAY_SYSTEM_CHAT:
            return chat_text(body)
        if pid == PLAY_DISCONNECT:
            reason, _ = rd_nbt_component(body)
            raise AssertionError(f"{context} disconnected: {reason}")


def place_and_sync(sock, threshold, x, y, z, face, sequence):
    send_use_item_on(sock, threshold, x, y, z, face, sequence)
    placed = None
    content = None
    while placed is None or content is None:
        pid, body = read_packet(sock, threshold)
        if pid == PLAY_BLOCK_UPDATE:
            px, py, pz, state = decode_block_update(body)
            if state != 0:
                placed = (px, py, pz)
        elif pid == PLAY_CONTAINER_SET_CONTENT:
            content = decode_container(body)
            assert content["window"] == 0, content["window"]
        elif pid == PLAY_DISCONNECT:
            reason, _ = rd_nbt_component(body)
            raise AssertionError(f"disconnected while placing at {(x, y, z)}: {reason}")
    return placed, content["state"]


def run_menu_checks(port, sock, threshold, protocol, name, case):
    """Open a chest, crafting table, smithing table, stonecutter and merchant.

    The translated inventory starts with a chest and a crafting table in the
    main inventory (slots 14 and 15). The test swaps each into the hotbar with
    the server's own SWAP click, places it, and then exercises the same window
    packets a canonical client uses: open_screen, container_set_content,
    container_set_slot and set_cursor_item, plus serverbound container_click
    and container_close.
    """
    case_index = CASES.index(case)
    # Keep the menu blocks away from the earlier placement test's column (x=8)
    # and from every other case: the server is shared by all four clients.
    menu_x = 24 + case_index * 8

    # Force a fresh content packet instead of guessing the state id left by
    # placement/pickup above. State 0 is always stale, so the server resyncs
    # without applying the click.
    send_menu_click(sock, threshold, 0, 0, 0, 0, 0)
    inventory = read_menu_content(sock, threshold, 0)
    assert len(inventory["slots"]) == 46, len(inventory["slots"])
    assert inventory["slots"][36] is not None, inventory["slots"]
    stone_id = inventory["slots"][36]["item"]
    state = inventory["state"]

    # Move to a clear column and place a stone support.
    send(sock, threshold, PLAY_MOVE_POS,
         struct.pack(">ddd", menu_x + 0.5, 200.0, 8.5) + b"\x01")
    send(sock, threshold, PLAY_SET_CARRIED, struct.pack(">h", 0))
    support, state = place_and_sync(sock, threshold, menu_x, 199, 8, 1, 100)

    # Main inventory slot 14 is the chest. SWAP moves it to hotbar 0.
    send_menu_click(sock, threshold, 0, state, 14, 0, 2)
    swapped, new_state = read_slots(sock, threshold, {14, 36})
    assert swapped[36] is not None and swapped[36]["count"] == 1, swapped
    state = new_state
    send(sock, threshold, PLAY_SET_CARRIED, struct.pack(">h", 0))
    chest_pos, state = place_and_sync(sock, threshold, support[0], support[1], support[2], 1, 101)

    # Right-click the chest: generic_9x3 is menu 2, with 27 container slots plus
    # the standard 36 player slots.
    send_use_item_on(sock, threshold, chest_pos[0], chest_pos[1], chest_pos[2], 1, 102)
    chest_window, chest = read_open_menu(sock, threshold, 2)
    assert len(chest["slots"]) == 63, len(chest["slots"])
    assert chest["window"] == chest_window and chest_window != 0
    state = chest["state"]

    # Menu 55 is hotbar 1 (the oak planks). Shift-click moves the whole stack
    # into the chest's first slot and the server re-states the full menu.
    send_menu_click(sock, threshold, chest_window, state, 55, 0, 1)
    chest = read_menu_content(sock, threshold, chest_window)
    assert chest["slots"][0] is not None and chest["slots"][0]["count"] == 64, chest["slots"][0]
    state = chest["state"]

    send(sock, threshold, SB_CONTAINER_CLOSE, varint(chest_window))
    inventory = read_menu_content(sock, threshold, 0)
    assert inventory["cursor"] is None
    state = inventory["state"]

    # Reopen the same chest: the stack is still there, so the block entity
    # round-trips through the open/close path rather than only the first view.
    send_use_item_on(sock, threshold, chest_pos[0], chest_pos[1], chest_pos[2], 1, 103)
    chest_window, chest = read_open_menu(sock, threshold, 2)
    assert chest["slots"][0] is not None and chest["slots"][0]["count"] == 64
    send(sock, threshold, SB_CONTAINER_CLOSE, varint(chest_window))
    inventory = read_menu_content(sock, threshold, 0)
    state = inventory["state"]

    # Main inventory slot 15 is the crafting table; swap it into hotbar 1.
    send_menu_click(sock, threshold, 0, state, 15, 1, 2)
    swapped, new_state = read_slots(sock, threshold, {15, 37})
    assert swapped[37] is not None and swapped[37]["count"] == 1, swapped
    state = new_state
    send(sock, threshold, PLAY_SET_CARRIED, struct.pack(">h", 1))

    # Place the table against the chest. Sneaking is what tells the server to
    # place rather than open the chest; the following click opens the table.
    send(sock, threshold, SB_PLAYER_INPUT, b"\x20")
    table_pos, state = place_and_sync(sock, threshold, chest_pos[0], chest_pos[1], chest_pos[2], 1, 104)
    send(sock, threshold, SB_PLAYER_INPUT, b"\x00")
    send_use_item_on(sock, threshold, table_pos[0], table_pos[1], table_pos[2], 1, 105)
    table_window, table = read_open_menu(sock, threshold, 12)
    assert len(table["slots"]) == 46, len(table["slots"])
    state = table["state"]

    # CraftingMenu: result 0, grid 1..9, main 10..36, hotbar 37..45. Hotbar 8
    # is the oak log. Shifting it into the grid derives four oak planks.
    send_menu_click(sock, threshold, table_window, state, 45, 0, 1)
    table = read_menu_content(sock, threshold, table_window)
    assert table["slots"][1] is not None and table["slots"][1]["count"] == 64, table["slots"][1]
    assert table["slots"][0] is not None and table["slots"][0]["count"] == 4, table["slots"][0]
    state = table["state"]

    # Taking the result consumes one log and puts four planks on the cursor.
    send_menu_click(sock, threshold, table_window, state, 0, 0, 0)
    cursor = read_cursor(sock, threshold)
    assert cursor is not None and cursor["count"] == 4, cursor

    send(sock, threshold, SB_CONTAINER_CLOSE, varint(table_window))
    inventory = read_menu_content(sock, threshold, 0)
    assert inventory["cursor"] is None, inventory["cursor"]
    print(f"{protocol}/{case['pack']}: chest and crafting-table menus OK")

    # --- smithing table (menu 21): template, base, addition and result -------
    # Main inventory slot 16 is the smithing table; swap it into hotbar 2.
    send_menu_click(sock, threshold, 0, inventory["state"], 16, 2, 2)
    swapped, state = read_slots(sock, threshold, {16, 38})
    assert swapped[38] is not None and swapped[38]["count"] == 1, swapped
    send(sock, threshold, PLAY_SET_CARRIED, struct.pack(">h", 2))
    send(sock, threshold, SB_PLAYER_INPUT, b"\x20")
    smithing_pos, state = place_and_sync(sock, threshold, table_pos[0], table_pos[1],
                                         table_pos[2], 1, 106)
    send(sock, threshold, SB_PLAYER_INPUT, b"\x00")
    send_use_item_on(sock, threshold, smithing_pos[0], smithing_pos[1], smithing_pos[2],
                     1, 107)
    smith_window, smith = read_open_menu(sock, threshold, 21)
    assert len(smith["slots"]) == 40, len(smith["slots"])
    state = smith["state"]

    # SmithingMenu: inputs 0..2, result 3, main 4..30, hotbar 31..39. Main
    # inventory slots 18/19/20 map to menu slots 13/14/15. Shift-clicking each
    # fills the input slot the item belongs to.
    for menu_slot, input_slot in ((13, 0), (14, 2), (15, 1)):
        send_menu_click(sock, threshold, smith_window, state, menu_slot, 0, 1)
        smith = read_menu_content(sock, threshold, smith_window)
        assert smith["slots"][input_slot] is not None, (input_slot, smith["slots"])
        state = smith["state"]
    assert smith["slots"][3] is not None and smith["slots"][3]["count"] == 1, smith["slots"][3]
    smith_result = smith["slots"][3]

    send_menu_click(sock, threshold, smith_window, state, 3, 0, 0)
    cursor = read_cursor(sock, threshold)
    assert cursor["item"] == smith_result["item"], (cursor, smith_result)
    send(sock, threshold, SB_CONTAINER_CLOSE, varint(smith_window))
    inventory = read_menu_content(sock, threshold, 0)
    assert inventory["cursor"] is None, inventory["cursor"]
    state = inventory["state"]
    print(f"{protocol}/{case['pack']}: smithing-table menu OK")

    # --- stonecutter (menu 24): input, button selection, result --------------+
    # Main inventory slot 17 is the stonecutter; swap it into hotbar 3.
    send_menu_click(sock, threshold, 0, state, 17, 3, 2)
    swapped, state = read_slots(sock, threshold, {17, 39})
    assert swapped[39] is not None and swapped[39]["count"] == 1, swapped
    send(sock, threshold, PLAY_SET_CARRIED, struct.pack(">h", 3))
    send(sock, threshold, SB_PLAYER_INPUT, b"\x20")
    cutter_pos, state = place_and_sync(sock, threshold, smithing_pos[0], smithing_pos[1],
                                       smithing_pos[2], 1, 108)
    send(sock, threshold, SB_PLAYER_INPUT, b"\x00")
    send_use_item_on(sock, threshold, cutter_pos[0], cutter_pos[1], cutter_pos[2], 1, 109)
    cutter_window, cutter = read_open_menu(sock, threshold, 24)
    assert len(cutter["slots"]) == 38, len(cutter["slots"])
    state = cutter["state"]

    # Find the stone stack among the menu's player slots and pick it up. The
    # input slot is 0; a stale click then resyncs the cursor and state after
    # each multi-step move, since the test client does not track revisions.
    stone_slot = next((i for i, stack in enumerate(cutter["slots"])
                       if i >= 2 and stack is not None and stack["item"] == stone_id), None)
    assert stone_slot is not None, cutter["slots"]
    send_menu_click(sock, threshold, cutter_window, state, stone_slot, 0, 0)
    send_menu_click(sock, threshold, cutter_window, 0, 0, 0, 0)
    cutter = read_menu_content(sock, threshold, cutter_window)
    assert cutter["cursor"] is not None and cutter["cursor"]["item"] == stone_id, cutter["cursor"]
    state = cutter["state"]

    send_menu_click(sock, threshold, cutter_window, state, 0, 0, 0, cutter["cursor"])
    send_menu_click(sock, threshold, cutter_window, 0, 0, 0, 0)
    cutter = read_menu_content(sock, threshold, cutter_window)
    assert cutter["slots"][0] is not None and cutter["slots"][0]["item"] == stone_id, cutter["slots"][0]
    assert cutter["cursor"] is None, cutter["cursor"]
    state = cutter["state"]

    send_container_button(sock, threshold, cutter_window, 0)
    selected, cutter_result, state = read_button_result(sock, threshold, cutter_window)
    assert selected == 0, selected
    assert cutter_result is not None and cutter_result["count"] >= 1, cutter_result

    send_menu_click(sock, threshold, cutter_window, state, 1, 0, 0)
    cursor = read_cursor(sock, threshold)
    assert cursor["item"] == cutter_result["item"], (cursor, cutter_result)
    send(sock, threshold, SB_CONTAINER_CLOSE, varint(cutter_window))
    inventory = read_menu_content(sock, threshold, 0)
    assert inventory["cursor"] is None, inventory["cursor"]
    state = inventory["state"]
    print(f"{protocol}/{case['pack']}: stonecutter menu OK")

    # --- enchanting table (menu 13): item, lapis, three offers --------------
    send_menu_click(sock, threshold, 0, state, 26, 4, 2)
    swapped, state = read_slots(sock, threshold, {26, 40})
    assert swapped[40] is not None and swapped[40]["count"] == 1, swapped
    send(sock, threshold, PLAY_SET_CARRIED, struct.pack(">h", 4))
    send(sock, threshold, SB_PLAYER_INPUT, b"\x20")
    enchanting_pos, state = place_and_sync(sock, threshold, cutter_pos[0], cutter_pos[1],
                                           cutter_pos[2], 1, 110)
    send(sock, threshold, SB_PLAYER_INPUT, b"\x00")
    send_use_item_on(sock, threshold, enchanting_pos[0], enchanting_pos[1],
                     enchanting_pos[2], 1, 111)
    enchant_window, enchant = read_open_menu(sock, threshold, 13)
    assert len(enchant["slots"]) == 38, len(enchant["slots"])
    initial = read_menu_data(sock, threshold, enchant_window, {0, 1, 2, 3, 4})
    assert initial[0] == initial[1] == initial[2] == 0, initial
    assert initial[4] == 0, initial
    state = enchant["state"]

    # EnchantmentMenu: item 0, lapis 1, main 2..28, hotbar 29..37. Main
    # inventory 28 (the lapis stack) is menu 21, and main inventory 9 (the
    # damaged diamond pickaxe) is menu 2. The plain sword is not used here: the
    # smithing check above consumes it as its base input and hands it back to
    # whichever inventory slot is free, so it is no longer at a fixed slot.
    send_menu_click(sock, threshold, enchant_window, state, 21, 0, 1)
    enchant, data = read_content_and_data(sock, threshold, enchant_window, {0, 1, 2, 3, 4})
    assert enchant["slots"][1] is not None and enchant["slots"][1]["count"] == 64, enchant["slots"][1]
    state = enchant["state"]

    send_menu_click(sock, threshold, enchant_window, state, 2, 0, 1)
    enchant, data = read_content_and_data(sock, threshold, enchant_window, {0, 1, 2, 3, 4})
    assert enchant["slots"][0] is not None, enchant["slots"]
    assert all(data[i] > 0 for i in (0, 1, 2)), data
    assert data[4] == 64, data
    enchant_cost = data[0]
    state = enchant["state"]

    send_container_button(sock, threshold, enchant_window, 0)
    slots, _, xp = read_enchant_result(sock, threshold, enchant_window)
    assert slots[0] is not None, slots
    components = {component["type"]: component for component in slots[0]["components"]}
    assert "enchantments" in components and components["enchantments"]["entries"], slots[0]
    assert slots[1] is not None and slots[1]["count"] == 63, slots[1]
    assert xp is not None and xp[1] == 30 - enchant_cost, (xp, enchant_cost)
    xp_level = xp[1]

    send(sock, threshold, SB_CONTAINER_CLOSE, varint(enchant_window))
    inventory = read_menu_content(sock, threshold, 0)
    assert inventory["cursor"] is None, inventory["cursor"]
    state = inventory["state"]
    print(f"{protocol}/{case['pack']}: enchanting-table menu OK (cost {enchant_cost})")

    # --- anvil (menu 8): two damaged swords, repair and take ----------------
    send_menu_click(sock, threshold, 0, state, 27, 5, 2)
    swapped, state = read_slots(sock, threshold, {27, 41})
    assert swapped[41] is not None and swapped[41]["count"] == 1, swapped
    send(sock, threshold, PLAY_SET_CARRIED, struct.pack(">h", 5))
    send(sock, threshold, SB_PLAYER_INPUT, b"\x20")
    anvil_pos, state = place_and_sync(sock, threshold, enchanting_pos[0], enchanting_pos[1],
                                      enchanting_pos[2], 1, 112)
    send(sock, threshold, SB_PLAYER_INPUT, b"\x00")
    send_use_item_on(sock, threshold, anvil_pos[0], anvil_pos[1], anvil_pos[2], 1, 113)
    anvil_window, anvil = read_open_menu(sock, threshold, 8)
    assert len(anvil["slots"]) == 39, len(anvil["slots"])
    initial = read_menu_data(sock, threshold, anvil_window, {0})
    assert initial[0] == 0, initial
    state = anvil["state"]

    # AnvilMenu: left 0, right 1, result 2, main 3..29, hotbar 30..38. Main
    # inventory 29 and 30 map to menu slots 23 and 24.
    send_menu_click(sock, threshold, anvil_window, state, 23, 0, 1)
    anvil, _ = read_content_and_data(sock, threshold, anvil_window, {0})
    assert anvil["slots"][0] is not None, anvil["slots"]
    state = anvil["state"]

    send_menu_click(sock, threshold, anvil_window, state, 24, 0, 1)
    anvil, data = read_content_and_data(sock, threshold, anvil_window, {0})
    assert anvil["slots"][1] is not None, anvil["slots"]
    assert anvil["slots"][2] is not None and anvil["slots"][2]["count"] == 1, anvil["slots"][2]
    anvil_cost = data[0]
    assert anvil_cost > 0, data
    anvil_result = anvil["slots"][2]
    result_damage = next((component["value"] for component in anvil_result["components"]
                          if component["type"] == "damage"), 0)
    assert result_damage < 100, anvil_result
    state = anvil["state"]

    send_menu_click(sock, threshold, anvil_window, state, 2, 0, 0)
    cursor, xp = read_cursor_and_experience(sock, threshold)
    assert cursor is not None and cursor["item"] == anvil_result["item"], (cursor, anvil_result)
    assert xp is not None and xp[1] == xp_level - anvil_cost, (xp, xp_level, anvil_cost)

    send(sock, threshold, SB_CONTAINER_CLOSE, varint(anvil_window))
    inventory = read_menu_content(sock, threshold, 0)
    assert inventory["cursor"] is None, inventory["cursor"]
    state = inventory["state"]
    print(f"{protocol}/{case['pack']}: anvil menu OK (cost {anvil_cost})")

    # --- merchant (menu 19): exhaust an offer, then restock on a new day -----
    # T0's own /time set 0 above advances the day; a canonical client still
    # summons the farmer on top of this client. The next movement packet makes
    # track_mobs Add Entity it to the translated connection, which then
    # right-clicks it.
    # Put the world on a known day first: this fixture's save keeps advancing
    # time between runs, and the restock check below needs a 0 -> 1 crossing.
    send_translated_command(sock, threshold, "time set 0",
                            f"{protocol}/{case['pack']} day command")
    # The table placements above stack on the old support block, so the trader
    # gets its own stone floor in a clear column. Both it and the player stand
    # there, which keeps them in reach while the restock check runs.
    # /summon puts the villager on a two-block ring around the summoner, so the
    # floor belongs at trader_x + 2, under the villager's actual spawn point.
    trader_x = menu_x + 4
    run_canonical_command(port, f"setblock {trader_x + 2} 199 8 minecraft:stone", case_index)
    spawn_trader(port, trader_x + 0.5, 200.0, 8.5, case_index)
    send(sock, threshold, PLAY_MOVE_POS,
         struct.pack(">ddd", trader_x + 0.5, 200.0, 8.5) + b"\x01")
    villager = None
    while villager is None:
        pid, body = read_packet(sock, threshold)
        if pid == PLAY_ADD_ENTITY:
            eid, etype, vx, vy, vz = decode_add_entity_pos(body)
            if etype == case["villager"]:
                villager = (eid, vx, vy, vz)
        elif pid == PLAY_DISCONNECT:
            reason, _ = rd_nbt_component(body)
            raise AssertionError(f"{protocol}: disconnected while waiting for a villager: {reason}")
    villager_eid, vx, vy, vz = villager
    send(sock, threshold, PLAY_MOVE_POS, struct.pack(">ddd", vx, vy, vz) + b"\x01")
    send_interact(sock, threshold, villager_eid)
    merchant_window, merchant, merchant_offers = read_merchant_window(sock, threshold, 19)
    assert len(merchant["slots"]) == 39, len(merchant["slots"])
    assert merchant_offers["offers"], merchant_offers
    state = merchant["state"]

    available = {}
    for stack in merchant["slots"][3:]:
        if stack is not None:
            available[stack["item"]] = available.get(stack["item"], 0) + stack["count"]
    pick = None
    for index, offer in enumerate(merchant_offers["offers"]):
        if offer["out_of_stock"] or offer["max_uses"] <= 0:
            continue
        need = {}
        for cost in (offer["cost_a"], offer["cost_b"]):
            if cost is not None and cost["item"] != 0:
                need[cost["item"]] = need.get(cost["item"], 0) + cost["count"]
        if need and all(available.get(item, 0) >= count * offer["max_uses"]
                        for item, count in need.items()):
            pick = index
            break
    assert pick is not None, (merchant_offers["offers"], available)

    send_select_trade(sock, threshold, pick)
    send_menu_click(sock, threshold, merchant_window, 0, 0, 0, 0)
    merchant = read_menu_content(sock, threshold, merchant_window)
    assert merchant["slots"][0] is not None or merchant["slots"][1] is not None, merchant["slots"]
    assert merchant["slots"][2] is not None, merchant["slots"]
    state = merchant["state"]

    # The quick-move loop trades until the offer is out of stock.
    send_menu_click(sock, threshold, merchant_window, state, 2, 0, 1)
    merchant = read_menu_content(sock, threshold, merchant_window)
    assert merchant["slots"][2] is None, merchant["slots"]
    send(sock, threshold, SB_CONTAINER_CLOSE, varint(merchant_window))
    inventory = read_menu_content(sock, threshold, 0)
    assert inventory["cursor"] is None, inventory["cursor"]

    # Same day: reopening must show the exhausted offer as out of stock.
    send_interact(sock, threshold, villager_eid)
    exhausted_window, exhausted, exhausted_offers = read_merchant_window(sock, threshold, 19)
    chosen = exhausted_offers["offers"][pick]
    assert chosen["out_of_stock"], chosen
    assert chosen["uses"] == chosen["max_uses"], chosen
    send(sock, threshold, SB_CONTAINER_CLOSE, varint(exhausted_window))
    read_menu_content(sock, threshold, 0)

    # Crossing a world day is this engine's deterministic restock boundary.
    run_canonical_command(port, "time set 24000", case_index)
    send_interact(sock, threshold, villager_eid)
    restocked_window, restocked, restocked_offers = read_merchant_window(sock, threshold, 19)
    chosen = restocked_offers["offers"][pick]
    assert not chosen["out_of_stock"], chosen
    assert chosen["uses"] == 0, chosen
    send(sock, threshold, SB_CONTAINER_CLOSE, varint(restocked_window))
    inventory = read_menu_content(sock, threshold, 0)
    assert inventory["cursor"] is None, inventory["cursor"]
    print(f"{protocol}/{case['pack']}: merchant menu OK (exhausted offer {pick}, restocked)")


def decode_block_update(body: bytes):
    raw = struct.unpack(">q", body[:8])[0]
    state, i = rd_varint(body, 8)
    assert i == len(body), (i, len(body))
    x = raw >> 38
    if x >= 0x2000000:
        x -= 0x4000000
    z = (raw >> 12) & 0x3FFFFFF
    if z >= 0x2000000:
        z -= 0x4000000
    y = raw & 0xFFF
    if y >= 0x800:
        y -= 0x1000
    return x, y, z, state


def decode_add_entity(body: bytes):
    entity_id, i = rd_varint(body)
    assert i + 16 <= len(body), len(body)
    i += 16
    entity_type, i = rd_varint(body, i)
    return entity_id, entity_type


def decode_add_entity_with_uuid(body: bytes):
    entity_id, i = rd_varint(body)
    assert i + 16 <= len(body), len(body)
    uuid = body[i:i + 16]
    i += 16
    entity_type, i = rd_varint(body, i)
    return entity_id, uuid, entity_type


def decode_add_entity_pos(body: bytes):
    entity_id, i = rd_varint(body)
    assert i + 16 <= len(body), len(body)
    i += 16
    entity_type, i = rd_varint(body, i)
    x, y, z = struct.unpack(">ddd", body[i:i + 24])
    return entity_id, entity_type, x, y, z


def decode_set_entity_data(body: bytes):
    """Decodes the translated entries this suite uses into {index: value}."""
    entity_id, i = rd_varint(body)
    values = {}
    while True:
        index = body[i]
        i += 1
        if index == 0xFF:
            break
        serializer, i = rd_varint(body, i)
        if serializer == 0:  # BYTE
            values[index] = body[i]
            i += 1
        elif serializer == 3:  # FLOAT
            values[index] = struct.unpack(">f", body[i:i + 4])[0]
            i += 4
        elif serializer == 8:  # BOOLEAN
            values[index] = body[i]
            i += 1
        elif serializer == 17:  # PARTICLES
            count, i = rd_varint(body, i)
            for _ in range(count):
                particle_type, i = rd_varint(body, i)
                if particle_type == 28:  # minecraft:entity_effect
                    i += 4
            values[index] = count
        else:
            # The observer sees data for every nearby entity, not just the one
            # under test. A newer release can add a serializer this decoder has
            # no wire layout for; keep the fields already read and let the
            # caller decide whether this packet is the one it needs.
            return entity_id, values
    assert i == len(body), (i, len(body))
    return entity_id, values


def decode_set_equipment(body: bytes):
    entity_id, i = rd_varint(body)
    slots = []
    while True:
        header = body[i]
        i += 1
        slot = header & 0x7F
        stack, i = decode_item_stack(body, i)
        slots.append((slot, stack))
        if not (header & 0x80):
            break
    assert i == len(body), (i, len(body))
    return entity_id, slots


def decode_canonical_player_info_add(body: bytes):
    """Decodes the canonical 26.3 0xFF Player Info Update this suite emits."""
    actions = body[0]
    assert actions == 0xFF, actions
    count, i = rd_varint(body, 1)
    assert count == 1, count
    assert i + 16 <= len(body), len(body)
    uuid = body[i:i + 16]
    i += 16
    player_name, i = rd_string(body, i)
    properties, i = rd_varint(body, i)
    for _ in range(properties):
        _, i = rd_string(body, i)
        _, i = rd_string(body, i)
        has_signature, i = body[i], i + 1
        if has_signature:
            _, i = rd_string(body, i)
    chat, i = body[i], i + 1
    assert chat == 0, chat
    _, i = rd_varint(body, i)  # gamemode
    listed, i = body[i], i + 1
    assert listed == 1, listed
    _, i = rd_varint(body, i)  # latency
    display, i = body[i], i + 1
    assert display == 0, display
    _, i = rd_varint(body, i)  # list priority
    hat, i = body[i], i + 1
    assert hat == 1, hat
    assert i == len(body), (i, len(body))
    return uuid, player_name


def wait_observed(observed, predicate, context, timeout=4.0):
    """Waits for a captured packet predicate, returning its non-None value."""
    deadline = time.time() + timeout
    while True:
        for pid, body in list(observed):
            value = predicate(pid, body)
            if value is not None:
                return value
        if time.time() >= deadline:
            raise AssertionError(f"{context}: packet not observed in {len(observed)} packets")
        time.sleep(0.02)


def wait_observed_player_proxy(observed, player_name, entity_id, context):
    def match_info(pid, body):
        if pid != CANONICAL_PLAYER_INFO_UPDATE:
            return None
        uuid, name = decode_canonical_player_info_add(body)
        return uuid if name == player_name else None

    def match_spawn(pid, body):
        if pid != CANONICAL_ADD_ENTITY:
            return None
        got_eid, uuid, entity_type = decode_add_entity_with_uuid(body)
        if got_eid == entity_id and entity_type == CANONICAL_PLAYER_ENTITY_TYPE:
            return uuid
        return None

    try:
        info = wait_observed(observed, match_info, f"{context} tab entry")
    except AssertionError as exc:
        names = []
        for pid, body in observed:
            if pid == CANONICAL_PLAYER_INFO_UPDATE:
                try:
                    names.append(decode_canonical_player_info_add(body)[1])
                except AssertionError:
                    pass
        raise AssertionError(f"{exc}; captured info names: {names}") from exc
    spawn = wait_observed(observed, match_spawn, f"{context} spawn")
    assert info == spawn, (context, info, spawn)
    return info


def wait_observed_entity_data(observed, entity_id, context):
    def match(pid, body):
        if pid != CANONICAL_SET_ENTITY_DATA:
            return None
        got_eid, values = decode_set_entity_data(body)
        return values if got_eid == entity_id else None
    return wait_observed(observed, match, context)


def wait_observed_entity_sync(observed, entity_id, x, y, z, context):
    def match(pid, body):
        if pid != CANONICAL_ENTITY_POSITION_SYNC:
            return None
        got_eid, i = rd_varint(body)
        if got_eid != entity_id:
            return None
        got_x, got_y, got_z = struct.unpack(">ddd", body[i:i + 24])
        if (abs(got_x - x) < 1e-6 and abs(got_y - y) < 1e-6 and
                abs(got_z - z) < 1e-6):
            return (got_x, got_y, got_z)
        return None
    return wait_observed(observed, match, context)


def wait_observed_canonical_equipment(observed, entity_id, context):
    def match(pid, body):
        if pid != CANONICAL_SET_EQUIPMENT:
            return None
        got_eid, slots = decode_set_equipment(body)
        return slots if got_eid == entity_id else None
    return wait_observed(observed, match, context)


def wait_observed_remove_entity(observed, entity_id, context):
    def match(pid, body):
        if pid != CANONICAL_REMOVE_ENTITIES:
            return None
        count, i = rd_varint(body)
        for _ in range(count):
            got_eid, i = rd_varint(body, i)
            if got_eid == entity_id:
                return True
        return None
    return wait_observed(observed, match, context)


def wait_observed_spawn(observed, entity_id, context):
    def match(pid, body):
        if pid != CANONICAL_ADD_ENTITY:
            return None
        got_eid, _uuid, entity_type = decode_add_entity_with_uuid(body)
        if got_eid == entity_id and entity_type == CANONICAL_PLAYER_ENTITY_TYPE:
            return True
        return None
    return wait_observed(observed, match, context)


def wait_observed_leave(observed, entity_id, uuid, context):
    deadline = time.time() + 4.0
    while True:
        removed = info_removed = False
        for pid, body in list(observed):
            if pid == CANONICAL_REMOVE_ENTITIES:
                count, i = rd_varint(body)
                ids = []
                for _ in range(count):
                    got_eid, i = rd_varint(body, i)
                    ids.append(got_eid)
                if entity_id in ids:
                    removed = True
            elif pid == CANONICAL_PLAYER_INFO_REMOVE:
                if decode_player_info_remove(body) == uuid:
                    info_removed = True
        if removed and info_removed:
            return
        if time.time() >= deadline:
            raise AssertionError(f"{context}: leave not observed")
        time.sleep(0.02)


def decode_player_info_add(body: bytes):
    actions, i = body[0], 1
    assert actions == 0x01, actions
    count, i = rd_varint(body, i)
    assert count == 1, count
    assert i + 16 <= len(body), len(body)
    uuid = body[i:i + 16]
    i += 16
    player_name, i = rd_string(body, i)
    properties, i = rd_varint(body, i)
    for _ in range(properties):
        _, i = rd_string(body, i)
        _, i = rd_string(body, i)
        has_signature, i = body[i], i + 1
        if has_signature:
            _, i = rd_string(body, i)
    assert i == len(body), (i, len(body))
    return uuid, player_name


def decode_player_info_remove(body: bytes):
    count, i = rd_varint(body)
    assert count == 1, count
    assert i + 16 <= len(body), len(body)
    uuid = body[i:i + 16]
    i += 16
    assert i == len(body), (i, len(body))
    return uuid


def read_canonical_pairing(sock, threshold, expected_name, case, expected_eid=None,
                           expected_uuid=None, context="canonical player"):
    """Reads one canonical player's translated Player Info + Add Entity pair."""
    info = None
    spawn = None
    while info is None or spawn is None:
        pid, body = read_packet(sock, threshold)
        if pid == PLAY_PLAYER_INFO_UPDATE:
            uuid, player_name = decode_player_info_add(body)
            if player_name != expected_name:
                continue
            if expected_uuid is not None:
                assert uuid == expected_uuid, (context, uuid, expected_uuid)
            info = (uuid, player_name)
        elif pid == PLAY_ADD_ENTITY:
            entity_id, uuid, entity_type = decode_add_entity_with_uuid(body)
            if entity_type != case["player"]:
                continue
            if expected_eid is not None and entity_id != expected_eid:
                continue
            wanted_uuid = expected_uuid if expected_uuid is not None else \
                (info[0] if info is not None else None)
            if wanted_uuid is not None and uuid != wanted_uuid:
                continue
            spawn = (entity_id, entity_type)
        elif pid == PLAY_DISCONNECT:
            reason, _ = rd_nbt_component(body)
            raise AssertionError(f"{context} disconnected during pairing: {reason}")
    return info, spawn


def read_canonical_entity_data(sock, threshold, entity_id, context):
    while True:
        pid, body = read_packet(sock, threshold)
        if pid == PLAY_SET_ENTITY_DATA:
            got_eid, values = decode_set_entity_data(body)
            if got_eid == entity_id:
                return values
        elif pid == PLAY_DISCONNECT:
            reason, _ = rd_nbt_component(body)
            raise AssertionError(f"{context} disconnected during entity data: {reason}")


def read_canonical_equipment(sock, threshold, entity_id, context):
    while True:
        pid, body = read_packet(sock, threshold)
        if pid == PLAY_SET_EQUIPMENT:
            got_eid, slots = decode_set_equipment(body)
            if got_eid == entity_id:
                return slots
        elif pid == PLAY_DISCONNECT:
            reason, _ = rd_nbt_component(body)
            raise AssertionError(f"{context} disconnected during equipment: {reason}")


def read_entity_sync(sock, threshold, entity_id, x, y, z, context):
    while True:
        pid, body = read_packet(sock, threshold)
        if pid == PLAY_ENTITY_POSITION_SYNC:
            got_eid, i = rd_varint(body)
            if got_eid != entity_id:
                continue
            got_x, got_y, got_z = struct.unpack(">ddd", body[i:i + 24])
            assert abs(got_x - x) < 1e-6 and abs(got_y - y) < 1e-6 and \
                   abs(got_z - z) < 1e-6, (context, got_x, got_y, got_z)
            return
        if pid == PLAY_DISCONNECT:
            reason, _ = rd_nbt_component(body)
            raise AssertionError(f"{context} disconnected during movement: {reason}")


def read_canonical_remove_entity(sock, threshold, entity_id, context):
    while True:
        pid, body = read_packet(sock, threshold)
        if pid == PLAY_REMOVE_ENTITIES:
            count, i = rd_varint(body)
            for _ in range(count):
                got_eid, i = rd_varint(body, i)
                if got_eid == entity_id:
                    return
        elif pid == PLAY_DISCONNECT:
            reason, _ = rd_nbt_component(body)
            raise AssertionError(f"{context} disconnected during range exit: {reason}")


def read_canonical_entity_spawn(sock, threshold, entity_id, entity_type, context):
    while True:
        pid, body = read_packet(sock, threshold)
        if pid == PLAY_ADD_ENTITY:
            got_eid, got_type = decode_add_entity(body)
            if got_eid == entity_id and got_type == entity_type:
                return
        elif pid == PLAY_DISCONNECT:
            reason, _ = rd_nbt_component(body)
            raise AssertionError(f"{context} disconnected during range entry: {reason}")


def read_canonical_leave(sock, threshold, entity_id, uuid, context):
    removed = info_removed = False
    while not (removed and info_removed):
        pid, body = read_packet(sock, threshold)
        if pid == PLAY_REMOVE_ENTITIES:
            count, i = rd_varint(body)
            ids = []
            for _ in range(count):
                got_eid, i = rd_varint(body, i)
                ids.append(got_eid)
            if entity_id in ids:
                removed = True
        elif pid == PLAY_PLAYER_INFO_REMOVE:
            if decode_player_info_remove(body) == uuid:
                info_removed = True
        elif pid == PLAY_DISCONNECT:
            reason, _ = rd_nbt_component(body)
            raise AssertionError(f"{context} disconnected during leave: {reason}")


def join_translated_peer(port: int, protocol: int, case: dict, main_name: str):
    """Logs a second T0 client in and returns it in play.

    The peer has no menus or world interaction; the assertions only need it
    connected long enough for the translated player tracker to pair it with
    the already-running main client.
    """
    peer_name = (case["name"] + "Peer")[:16]
    sock = wire_client.transport(HOST, port, timeout=SOCKET_TIMEOUT)
    sock.settimeout(SOCKET_TIMEOUT)
    threshold = -1
    try:
        handshake = (varint(0) + varint(protocol) + varint(len(HOST)) + HOST.encode() +
                     struct.pack(">H", port) + varint(2))
        sock.sendall(frame(handshake, -1))
        send(sock, -1, 0x00, varint(len(peer_name)) + peer_name.encode() + b"\x00" * 16)

        while True:
            pid, body = read_packet(sock, threshold)
            if pid == SET_COMPRESSION:
                threshold, _ = rd_varint(body)
            elif pid == LOGIN_SUCCESS:
                break
            else:
                raise AssertionError(f"{protocol}: peer unexpected login packet 0x{pid:02x}")
        send(sock, threshold, 0x03)

        while True:
            pid, body = read_packet(sock, threshold)
            if pid == CONFIG_KNOWN_PACKS:
                count, i = rd_varint(body)
                packs = []
                for _ in range(count):
                    ns, i = rd_string(body, i)
                    pack, i = rd_string(body, i)
                    version, i = rd_string(body, i)
                    packs.append((ns, pack, version))
                out = varint(len(packs))
                for ns, pack, version in packs:
                    for text in (ns, pack, version):
                        out += varint(len(text)) + text.encode()
                send(sock, threshold, 0x07, out)
            elif pid == CONFIG_FINISH:
                send(sock, threshold, 0x03)
                break
            elif pid in (CONFIG_BRAND, CONFIG_FEATURE_FLAGS, CONFIG_REGISTRY, CONFIG_TAGS):
                pass
            else:
                raise AssertionError(f"{protocol}: peer unexpected config packet 0x{pid:02x}")

        pid, body = read_packet(sock, threshold)
        assert pid == PLAY_JOIN, f"{protocol}: peer expected Join Game, got 0x{pid:02x}"
        join = parse_join(body)
        peer_entity_id = join["entity_id"]
        assert peer_entity_id > 0

        seen_info = None
        seen_spawn = None
        while seen_info is None or seen_spawn is None:
            pid, body = read_packet(sock, threshold)
            if pid == PLAY_PLAYER_INFO_UPDATE:
                uuid, player_name = decode_player_info_add(body)
                if player_name == main_name:
                    seen_info = (uuid, player_name)
            elif pid == PLAY_ADD_ENTITY:
                entity_id, entity_type = decode_add_entity(body)
                if entity_type == case["player"]:
                    seen_spawn = (entity_id, entity_type)
            elif pid == PLAY_DISCONNECT:
                reason, _ = rd_nbt_component(body)
                raise AssertionError(f"{protocol}: peer disconnected during pairing: {reason}")
        return sock, threshold, peer_entity_id, seen_info, seen_spawn
    except BaseException:
        sock.close()
        raise


def run_player_tracking_checks(port: int, sock, threshold: int, protocol: int,
                               name: str, case: dict, entity_id: int):
    peer = None
    canonical = None
    leave_observer = None
    peer_eid = None
    peer_uuid = None
    observed = []
    try:
        # The menu checks leave this client at the merchant platform (y=200),
        # while the canonical bot joins at the world spawn. Put the main back
        # at spawn first, and use a chat reply as a barrier: packets on one
        # connection are ordered, so the reply proves the server has processed
        # the move before the canonical join computes tracking range.
        reset_message = f"{case['pack']} tracking range reset"
        send(sock, threshold, PLAY_MOVE_POS,
             struct.pack(">ddd", 3.5, 65.0, 3.5) + b"\x01")
        send(sock, threshold, PLAY_CHAT, varint(len(reset_message)) + reset_message.encode())
        while True:
            pid, body = read_packet(sock, threshold)
            if pid == PLAY_SYSTEM_CHAT and chat_text(body) == f"<{name}> {reset_message}":
                break
            if pid == PLAY_DISCONNECT:
                reason, _ = rd_nbt_component(body)
                raise AssertionError(
                    f"{protocol}/{case['pack']} disconnected moving to spawn: {reason}")

        # A canonical 26.3 player joins after this T0 client is already in
        # play. The translated client must receive its tab entry and entity
        # without a canonical Player registration of its own.
        canonical_name = ("T0Canon" + case["pack"].replace(".", "") +
                          str(protocol))[:16]
        canonical = craft_test.Bot(canonical_name, port)
        canonical.packet_hook = lambda pid, body: observed.append((pid, body))
        canonical.join()
        canonical.start_reader()
        canonical_info, canonical_spawn = read_canonical_pairing(
            sock, threshold, canonical_name, case,
            context=f"{protocol}/{case['pack']} canonical join")
        canonical_uuid = canonical_info[0]
        canonical_eid = canonical_spawn[0]
        assert canonical_eid != entity_id

        # The reverse direction: the canonical viewer must have the T0 player
        # in its tab list and an Add Entity proxy for it, captured from the
        # join burst before the reader thread took over.
        reverse_main_uuid = wait_observed_player_proxy(
            observed, name, entity_id,
            f"{protocol}/{case['pack']} canonical sees T0 main")
        reverse_data = wait_observed_entity_data(
            observed, entity_id,
            f"{protocol}/{case['pack']} canonical T0 skin data")
        assert reverse_data.get(16) == 0x7F, reverse_data
        assert reverse_data.get(9) == 20.0, reverse_data

        # Reverse shared flags are live, and clearing the sneak bit must be a
        # real zero write rather than an omitted entry the client would ignore.
        observed.clear()
        send(sock, threshold, SB_PLAYER_INPUT, b"\x20")
        sneak = wait_observed_entity_data(
            observed, entity_id,
            f"{protocol}/{case['pack']} canonical T0 sneak")
        assert sneak.get(0) is not None and sneak[0] & 0x02, sneak
        observed.clear()
        send(sock, threshold, SB_PLAYER_INPUT, b"\x00")
        clear = wait_observed_entity_data(
            observed, entity_id,
            f"{protocol}/{case['pack']} canonical T0 sneak clear")
        assert clear.get(0) == 0, clear

        # A hotbar switch must reach the canonical viewer as Set Equipment.
        # The menu run no longer leaves the starter 64-plank stack in hotbar 1
        # (the chest took it and crafting left a different stack), so read the
        # player's own window and require the mirror to match that stack rather
        # than a count that only held before the menus.
        send_menu_click(sock, threshold, 0, 0, 0, 0, 0)
        current_inventory = read_menu_content(sock, threshold, 0)
        expected_mainhand = current_inventory["slots"][37]
        assert expected_mainhand is not None, current_inventory["slots"]
        observed.clear()
        send(sock, threshold, PLAY_SET_CARRIED, struct.pack(">h", 1))
        reverse_equipment = wait_observed_canonical_equipment(
            observed, entity_id,
            f"{protocol}/{case['pack']} canonical T0 equipment")
        mainhand = next(stack for slot, stack in reverse_equipment if slot == 0)
        assert mainhand is not None and mainhand["item"] != 0, reverse_equipment
        # The player window is translated (target-release item ids) and the
        # reverse Set Equipment is canonical 26.3, so the wire item ids are not
        # comparable across the two; the count identifies the held stack.
        assert mainhand["count"] == expected_mainhand["count"], (
            mainhand, expected_mainhand)

        # Range gating runs in both directions. Moving the canonical player far
        # away must despawn its T0 proxy on this client and despawn this T0
        # player's proxy on the canonical client; moving back must respawn both
        # without resending the global Player Info entries.
        observed.clear()
        canonical.move_to(200.0, 65.0, 3.5)
        read_canonical_remove_entity(
            sock, threshold, canonical_eid,
            f"{protocol}/{case['pack']} canonical range exit")
        wait_observed_remove_entity(
            observed, entity_id,
            f"{protocol}/{case['pack']} canonical client T0 range exit")
        observed.clear()
        canonical.move_to(3.5, 65.0, 3.5)
        read_canonical_entity_spawn(
            sock, threshold, canonical_eid, case["player"],
            f"{protocol}/{case['pack']} canonical range entry")
        wait_observed_spawn(
            observed, entity_id,
            f"{protocol}/{case['pack']} canonical client T0 range entry")

        # The pairing burst must carry the skin-layer mask, not just the name
        # and position; otherwise every canonical player arrives bare.
        canonical_data = read_canonical_entity_data(
            sock, threshold, canonical_eid,
            f"{protocol}/{case['pack']} canonical synched data")
        assert canonical_data.get(16) == 0x7F, canonical_data

        # A held item is the live equipment path: /give fills slot 0, and the
        # carried-slot packet makes the canonical session publish Set Equipment.
        give = f"give {canonical_name} minecraft:stone 1"
        canonical.send(craft_test.SB_CHAT_COMMAND,
                       craft_test.varint(len(give)) + give.encode())
        canonical.send(SB_SET_CARRIED, struct.pack(">h", 0))
        canonical_equipment = read_canonical_equipment(
            sock, threshold, canonical_eid,
            f"{protocol}/{case['pack']} canonical equipment")
        mainhand = next(stack for slot, stack in canonical_equipment if slot == 0)
        assert mainhand is not None and mainhand["count"] == 1, canonical_equipment

        peer, peer_threshold, peer_eid, peer_info, peer_spawn = join_translated_peer(
            port, protocol, case, name)
        assert peer_spawn[0] == entity_id, (peer_spawn, entity_id)
        assert peer_info[1] == name, peer_info

        peer_name = (case["name"] + "Peer")[:16]
        reverse_peer_uuid = wait_observed_player_proxy(
            observed, peer_name, peer_eid,
            f"{protocol}/{case['pack']} canonical sees T0 peer")

        # Consume the main client's peer pair before the canonical movement and
        # leave reads; those reads deliberately ignore unrelated packets and
        # would otherwise swallow this pair first.
        peer_uuid = None
        seen_info = seen_spawn = False
        while not (seen_info and seen_spawn):
            pid, body = read_packet(sock, threshold)
            if pid == PLAY_PLAYER_INFO_UPDATE:
                uuid, player_name = decode_player_info_add(body)
                if player_name == (case["name"] + "Peer")[:16]:
                    peer_uuid = uuid
                    seen_info = True
            elif pid == PLAY_ADD_ENTITY:
                entity_id_seen, entity_type = decode_add_entity(body)
                if entity_id_seen == peer_eid and entity_type == case["player"]:
                    seen_spawn = True
            elif pid == PLAY_DISCONNECT:
                reason, _ = rd_nbt_component(body)
                raise AssertionError(f"{protocol}: disconnected during peer join: {reason}")
        assert peer_uuid is not None

        # The second T0 client joined after the canonical player, so its join
        # snapshot must contain the same canonical pair.
        peer_canonical_info, peer_canonical_spawn = read_canonical_pairing(
            peer, peer_threshold, canonical_name, case,
            expected_eid=canonical_eid, expected_uuid=canonical_uuid,
            context=f"{protocol}/{case['pack']} peer canonical snapshot")
        assert peer_canonical_info[0] == canonical_uuid
        assert peer_canonical_spawn[0] == canonical_eid

        canonical_x, canonical_y, canonical_z = 2.5, 64.0, 2.5
        canonical.move_to(canonical_x, canonical_y, canonical_z)
        read_entity_sync(sock, threshold, canonical_eid, canonical_x, canonical_y,
                         canonical_z, f"{protocol}/{case['pack']} canonical move")
        read_entity_sync(peer, peer_threshold, canonical_eid, canonical_x,
                         canonical_y, canonical_z,
                         f"{protocol}/{case['pack']} peer canonical move")

        # A command teleport takes the separate do_teleport path rather than
        # the movement packet path; translated viewers must see that too.
        teleport_x, teleport_y, teleport_z = 3.5, 65.0, 3.5
        command = f"tp {teleport_x} {teleport_y} {teleport_z}"
        canonical.send(craft_test.SB_CHAT_COMMAND,
                       craft_test.varint(len(command)) + command.encode())
        read_entity_sync(sock, threshold, canonical_eid, teleport_x, teleport_y,
                         teleport_z, f"{protocol}/{case['pack']} canonical teleport")
        read_entity_sync(peer, peer_threshold, canonical_eid, teleport_x,
                         teleport_y, teleport_z,
                         f"{protocol}/{case['pack']} peer canonical teleport")

        move_x, move_y, move_z = 1.5, 64.0, 1.5
        send(peer, peer_threshold, PLAY_MOVE_POS,
             struct.pack(">ddd", move_x, move_y, move_z) + b"\x01")
        moved = False
        while not moved:
            pid, body = read_packet(sock, threshold)
            if pid == PLAY_ENTITY_POSITION_SYNC:
                moved_eid, i = rd_varint(body)
                x, y, z = struct.unpack(">ddd", body[i:i + 24])
                if moved_eid == peer_eid:
                    assert abs(x - move_x) < 1e-6 and abs(y - move_y) < 1e-6 and \
                           abs(z - move_z) < 1e-6, (x, y, z)
                    moved = True
            elif pid == PLAY_DISCONNECT:
                reason, _ = rd_nbt_component(body)
                raise AssertionError(f"{protocol}: disconnected during peer movement: {reason}")
        wait_observed_entity_sync(
            observed, peer_eid, move_x, move_y, move_z,
            f"{protocol}/{case['pack']} canonical sees T0 peer move")

        canonical.close()
        canonical = None
        read_canonical_leave(sock, threshold, canonical_eid, canonical_uuid,
                             f"{protocol}/{case['pack']} canonical leave")
        read_canonical_leave(peer, peer_threshold, canonical_eid, canonical_uuid,
                             f"{protocol}/{case['pack']} peer canonical leave")
        print(f"{protocol}/{case['pack']}: canonical player visibility OK")

        # A fresh canonical observer checks the reverse leave burst. The first
        # canonical viewer is already gone by this point, so this one joins
        # while the T0 peer is still in play and watches it disconnect.
        observer_name = ("T0Sees" + case["pack"].replace(".", "") +
                         str(protocol))[:16]
        leave_observed = []
        leave_observer = craft_test.Bot(observer_name, port)
        leave_observer.packet_hook = lambda pid, body: leave_observed.append((pid, body))
        leave_observer.join()
        leave_observer.start_reader()
        wait_observed_player_proxy(
            leave_observed, peer_name, peer_eid,
            f"{protocol}/{case['pack']} leave observer sees T0 peer")

        peer.close()
        peer = None
        removed = info_removed = False
        while not (removed and info_removed):
            pid, body = read_packet(sock, threshold)
            if pid == PLAY_REMOVE_ENTITIES:
                count, i = rd_varint(body)
                ids = []
                for _ in range(count):
                    removed_eid, i = rd_varint(body, i)
                    ids.append(removed_eid)
                if peer_eid in ids:
                    removed = True
            elif pid == PLAY_PLAYER_INFO_REMOVE:
                if decode_player_info_remove(body) == peer_uuid:
                    info_removed = True
            elif pid == PLAY_DISCONNECT:
                reason, _ = rd_nbt_component(body)
                raise AssertionError(f"{protocol}: disconnected during peer leave: {reason}")
        wait_observed_leave(
            leave_observed, peer_eid, reverse_peer_uuid,
            f"{protocol}/{case['pack']} canonical leave observer")
        leave_observer.close()
        leave_observer = None
        assert reverse_main_uuid != reverse_peer_uuid
        print(f"{protocol}/{case['pack']}: peer player tracking OK")
    finally:
        if leave_observer is not None:
            leave_observer.close()
        if canonical is not None:
            if canonical.reader.error is not None:
                print(f"{protocol}/{case['pack']}: canonical reader error: "
                      f"{canonical.reader.error!r}", file=sys.stderr)
            canonical.close()
        if peer is not None:
            if peer.reader.error is not None:
                print(f"{protocol}/{case['pack']}: peer reader error: "
                      f"{peer.reader.error!r}", file=sys.stderr)
            peer.close()


def run_client(port: int, protocol: int, case: dict):
    name = case["name"]
    sock = wire_client.transport(HOST, port, timeout=SOCKET_TIMEOUT)
    sock.settimeout(SOCKET_TIMEOUT)
    threshold = -1
    try:
        handshake = (varint(0) + varint(protocol) + varint(len(HOST)) + HOST.encode() +
                     struct.pack(">H", port) + varint(2))
        sock.sendall(frame(handshake, -1))
        send(sock, -1, 0x00, varint(len(name)) + name.encode() + b"\x00" * 16)

        while True:
            pid, body = read_packet(sock, threshold)
            if pid == SET_COMPRESSION:
                threshold, _ = rd_varint(body)
                assert threshold >= 0
            elif pid == LOGIN_SUCCESS:
                break
            else:
                raise AssertionError(f"{protocol}: unexpected login packet 0x{pid:02x}")

        expected = 16 + 1 + len(name) + 1 + (16 if protocol == 776 else 0)
        assert len(body) == expected, (
            f"{protocol}: Login Success has {len(body)} bytes, expected {expected}")
        got_name, i = rd_string(body, 16)
        props, i = rd_varint(body, i)
        assert got_name == name and props == 0
        if protocol == 776:
            assert i + 16 == len(body), f"26.2 session id missing: {i}/{len(body)}"
            i += 16
        assert i == len(body), f"{protocol}: login success trailing bytes"
        send(sock, threshold, 0x03)

        registries = 0
        tags = 0
        brand = feature_flags = known_packs = False
        while True:
            pid, body = read_packet(sock, threshold)
            if pid == CONFIG_BRAND:
                channel, i = rd_string(body)
                value, i = rd_string(body, i)
                assert channel == "minecraft:brand" and value and i == len(body)
                brand = True
            elif pid == CONFIG_FEATURE_FLAGS:
                count, i = rd_varint(body)
                flags = []
                for _ in range(count):
                    flag, i = rd_string(body, i)
                    flags.append(flag)
                assert "minecraft:vanilla" in flags and i == len(body), flags
                feature_flags = True
            elif pid == CONFIG_KNOWN_PACKS:
                count, i = rd_varint(body)
                packs = []
                for _ in range(count):
                    ns, i = rd_string(body, i)
                    pack, i = rd_string(body, i)
                    version, i = rd_string(body, i)
                    packs.append((ns, pack, version))
                expected = [("minecraft", "core", v) for v in case["packs"]]
                assert packs == expected, f"server advertised {packs}, expected {expected}"
                assert i == len(body)
                selected = ("minecraft", "core", case["pack"])
                out = varint(1)
                for text in selected:
                    out += varint(len(text)) + text.encode()
                send(sock, threshold, 0x07, out)
                known_packs = True
            elif pid == CONFIG_REGISTRY:
                rid, i = rd_string(body)
                count, i = rd_varint(body, i)
                for _ in range(count):
                    _, i = rd_string(body, i)
                    assert body[i] == 0, f"{rid}: unexpected NBT"
                    i += 1
                assert i == len(body), f"{rid}: trailing bytes"
                registries += 1
            elif pid == CONFIG_TAGS:
                expected = (DATA_ROOT / case["pack"] / "clientconfig" / "tags.bin").read_bytes()
                assert body == expected, (
                    f"{case['pack']}: got the wrong tags payload "
                    f"({len(body)} bytes, expected {len(expected)})")
                tags = len(body)
            elif pid == CONFIG_FINISH:
                send(sock, threshold, 0x03)
                break
            else:
                raise AssertionError(f"{protocol}: unexpected config packet 0x{pid:02x}")

        assert brand and feature_flags and known_packs, (brand, feature_flags, known_packs)
        assert registries == case["registries"], (
            f"{protocol}: {registries} registries, expected {case['registries']}")
        assert tags > 30000, f"{protocol}: tags payload is only {tags} bytes"

        pid, body = read_packet(sock, threshold)
        assert pid == PLAY_JOIN, f"{protocol}: expected Join Game 0x31, got 0x{pid:02x}"
        join_bytes = len(body)
        join = parse_join(body)
        entity_id = join["entity_id"]
        assert join["tail"] == case["join_tail"], (
            f"{protocol}: Join Game tail {join['tail'].hex()}, expected {case['join_tail'].hex()}")
        assert join["entity_id"] > 0 and join["hardcore"] == 0
        assert join["dimensions"] == [
            "minecraft:overworld", "minecraft:the_nether", "minecraft:the_end"]
        assert join["dimension"] == "minecraft:overworld" and join["view_distance"] == 2
        assert join["sea_level"] == 63
        assert join["previous_gamemode"] == 0xFF and join["has_death"] == 0

        chunks = set()
        non_air_sections = 0
        saw_event = saw_position = saw_batch_start = saw_commands = False
        batch_count = -1
        while True:
            pid, body = read_packet(sock, threshold)
            if pid == PLAY_GAME_EVENT:
                assert len(body) == 5 and body[0] == 13, body.hex()
                saw_event = True
            elif pid == PLAY_POSITION:
                saw_position = True
            elif pid == PLAY_BATCH_START:
                saw_batch_start = True
            elif pid == PLAY_CHUNK:
                x, z = struct.unpack(">ii", body[:8])
                chunks.add((x, z))
                decoded = chunk_decode.decode(body, mask_encoding="longs")
                assert len(decoded["sections"]) == 24, len(decoded["sections"])
                for section in decoded["sections"]:
                    if section["non_air"]:
                        non_air_sections += 1
                    if section["states_bits"] == 0:
                        assert section["states"][0] <= case["block_max"], section
                    elif section["states_bits"] <= 8:
                        assert all(v <= case["block_max"] for v in section["states"]), section
                    if section["biomes_bits"] == 0:
                        assert section["biomes"][0] < case["biomes"], section
                    elif section["biomes_bits"] <= 3:
                        assert all(v < case["biomes"] for v in section["biomes"]), section
            elif pid == PLAY_COMMANDS:
                saw_commands = True
            elif pid == PLAY_BATCH_FINISHED:
                batch_count, _ = rd_varint(body)
                break
            elif pid == PLAY_KEEPALIVE:
                pass
            elif pid == PLAY_DISCONNECT:
                reason, _ = rd_nbt_component(body)
                raise AssertionError(f"{protocol}: play entry disconnected: {reason}")
        side = 2 * join["view_distance"] + 1
        assert saw_event and saw_position and saw_batch_start
        assert saw_commands, f"{protocol}: translated client got no command tree"
        assert batch_count == side * side == len(chunks), (batch_count, side, len(chunks))
        assert non_air_sections > 0, "translated chunks are still all air"

        # The translated connection must receive a real player menu, not an
        # empty placeholder. Slot 36 is the first hotbar slot (menu order), and
        # the starter inventory deliberately puts a full stone stack there.
        inventory = None
        experience = None
        while inventory is None:
            pid, body = read_packet(sock, threshold)
            if pid == PLAY_CONTAINER_SET_CONTENT:
                inventory = decode_container(body)
            elif pid == PLAY_SET_EXPERIENCE:
                experience = decode_set_experience(body)
            elif pid in (PLAY_KEEPALIVE, PLAY_ADD_ENTITY, PLAY_SET_ENTITY_DATA):
                pass
            elif pid == PLAY_DISCONNECT:
                reason, _ = rd_nbt_component(body)
                raise AssertionError(f"{protocol}: inventory disconnected: {reason}")
        assert experience is not None and experience[1] == 30, experience
        assert inventory["window"] == 0 and inventory["state"] >= 1
        assert len(inventory["slots"]) == 46, len(inventory["slots"])
        assert inventory["cursor"] is None
        assert inventory["slots"][36] is not None, inventory["slots"]
        stone = inventory["slots"][36]
        stone_id, stone_count = stone["item"], stone["count"]
        assert stone_id <= case["item_max"], (stone_id, case["item_max"])
        assert stone_count == 64, (stone_id, stone_count)
        assert all(stack is None or stack["item"] <= case["item_max"]
                   for stack in inventory["slots"])

        # The five component-bearing starter stacks must arrive with target
        # component ids and re-mapped registry ids, not as plain items.
        damaged = inventory["slots"][9]
        assert damaged is not None and damaged["item"] <= case["item_max"]
        assert damaged["components"] == [{"type": "damage", "value": 100}], damaged
        enchanted_stack = inventory["slots"][10]
        assert enchanted_stack is not None and enchanted_stack["item"] <= case["item_max"]
        assert enchanted_stack["components"] == [
            {"type": "enchantments", "entries": [(33, 3)]}], enchanted_stack
        trimmed_stack = inventory["slots"][11]
        assert trimmed_stack is not None and trimmed_stack["item"] <= case["item_max"]
        assert trimmed_stack["components"] == [
            {"type": "trim", "material": 4, "pattern": 17}], trimmed_stack
        potion_stack = inventory["slots"][12]
        assert potion_stack is not None and potion_stack["item"] <= case["item_max"]
        assert potion_stack["components"] == [{
            "type": "potion_contents",
            "potion": 24,
            "color": 0x123456,
            "effects": [{
                "effect": 15,
                "amplifier": 0,
                "duration": 200,
                "ambient": False,
                "visible": True,
                "show_icon": True,
                "hidden": None,
            }],
            "name": "Starter Tonic",
        }], potion_stack
        glinting_stack = inventory["slots"][13]
        assert glinting_stack is not None and glinting_stack["item"] <= case["item_max"]
        assert glinting_stack["components"] == [{
            "type": "enchantment_glint_override", "value": True,
        }], glinting_stack

        # Move above the terrain and place the starter stone block. Clicking the
        # air cell at y=199 either places into it (replaceable) or on its top
        # face, so the test reads the returned position instead of assuming it.
        send(sock, threshold, PLAY_SET_CARRIED, struct.pack(">h", 0))
        send(sock, threshold, PLAY_MOVE_POS,
             struct.pack(">ddd", 8.5, 200.0, 8.5) + b"\x01")
        place_x, place_y, place_z = 8, 199, 8
        place_seq = 1
        send(sock, threshold, PLAY_USE_ITEM_ON,
             varint(0) + struct.pack(">q", pack_block_pos(place_x, place_y, place_z)) +
             varint(1) + struct.pack(">fff", 0.5, 0.5, 0.5) + b"\x00\x00" +
             varint(place_seq))
        placed = None
        placed_content = None
        placed_ack = False
        while placed is None or placed_content is None:
            pid, body = read_packet(sock, threshold)
            if pid == PLAY_BLOCK_UPDATE:
                placed = decode_block_update(body)
            elif pid == PLAY_BLOCK_ACK:
                placed_ack = True
            elif pid == PLAY_CONTAINER_SET_CONTENT:
                placed_content = decode_container(body)
            elif pid in (PLAY_KEEPALIVE, PLAY_ADD_ENTITY, PLAY_SET_ENTITY_DATA,
                         PLAY_REMOVE_ENTITIES, PLAY_TAKE_ITEM):
                pass
            elif pid == PLAY_DISCONNECT:
                reason, _ = rd_nbt_component(body)
                raise AssertionError(f"{protocol}: placement disconnected: {reason}")
        assert placed_ack, "placement was not acknowledged"
        px, py, pz, placed_state = placed
        assert placed_state != 0 and placed_state <= case["block_max"], placed
        assert (px, py, pz) in ((place_x, place_y, place_z),
                                (place_x, place_y + 1, place_z)), placed
        assert placed_content["slots"][36] is not None
        assert placed_content["slots"][36]["count"] == 63, placed_content["slots"][36]

        # Break it with the pickaxe. The target client must see the predicted
        # block corrected to air, then an add_entity of the item type and the
        # item stack metadata, all with target-release ids.
        send(sock, threshold, PLAY_SET_CARRIED, struct.pack(">h", 4))
        break_seq = 2
        send(sock, threshold, PLAY_PLAYER_ACTION,
             varint(2) + struct.pack(">q", pack_block_pos(px, py, pz)) + b"\x01" +
             varint(break_seq))
        broke = False
        item_eid = None
        dropped_item = None
        item_data = False
        while not (broke and item_eid is not None and item_data):
            pid, body = read_packet(sock, threshold)
            if pid == PLAY_BLOCK_UPDATE:
                ux, uy, uz, state = decode_block_update(body)
                if (ux, uy, uz) == (px, py, pz):
                    assert state == 0, state
                    broke = True
            elif pid == PLAY_ADD_ENTITY:
                eid, etype = decode_add_entity(body)
                if etype == case["item_entity"]:
                    item_eid = eid
                else:
                    assert etype <= case["entity_max"], (etype, case["entity_max"])
            elif pid == PLAY_SET_ENTITY_DATA:
                eid, i = rd_varint(body)
                if i < len(body):
                    index = body[i]
                    i += 1
                    serializer, i = rd_varint(body, i)
                    if index == 8 and serializer == 7 and eid == item_eid:
                        stack, i = decode_item_stack(body, i)
                        assert stack is not None and stack["item"] <= case["item_max"], stack
                        assert stack["count"] == 1, stack
                        assert i == len(body) - 1 and body[i] == 0xFF
                        dropped_item = stack["item"]
                        item_data = True
            elif pid in (PLAY_KEEPALIVE, PLAY_BLOCK_ACK, PLAY_TAKE_ITEM,
                         PLAY_REMOVE_ENTITIES):
                pass
            elif pid == PLAY_DISCONNECT:
                reason, _ = rd_nbt_component(body)
                raise AssertionError(f"{protocol}: break disconnected: {reason}")

        # Walking onto the drop must produce Take Item + Remove Entities and put
        # the item back into the hotbar through a fresh content packet.
        send(sock, threshold, PLAY_MOVE_POS,
             struct.pack(">ddd", px + 0.5, py + 1.0, pz + 0.5) + b"\x01")
        took = removed = False
        picked_content = None
        while not (took and removed and picked_content is not None):
            pid, body = read_packet(sock, threshold)
            if pid == PLAY_TAKE_ITEM:
                taken_eid, i = rd_varint(body)
                player_eid, i = rd_varint(body, i)
                amount, i = rd_varint(body, i)
                assert amount == 1 and player_eid > 0, (amount, player_eid)
                if taken_eid == item_eid:
                    took = True
            elif pid == PLAY_REMOVE_ENTITIES:
                count, i = rd_varint(body)
                ids = []
                for _ in range(count):
                    eid, i = rd_varint(body, i)
                    ids.append(eid)
                assert i == len(body)
                if item_eid in ids:
                    removed = True
            elif pid == PLAY_CONTAINER_SET_CONTENT:
                picked_content = decode_container(body)
            elif pid in (PLAY_KEEPALIVE, PLAY_BLOCK_ACK, PLAY_ADD_ENTITY,
                         PLAY_SET_ENTITY_DATA):
                pass
            elif pid == PLAY_DISCONNECT:
                reason, _ = rd_nbt_component(body)
                raise AssertionError(f"{protocol}: pickup disconnected: {reason}")
        assert dropped_item is not None
        assert any(stack is not None and stack["item"] == dropped_item and
                   stack["count"] == 1 for stack in picked_content["slots"]), (
            dropped_item, picked_content["slots"])
        assert picked_content["slots"][36] is not None
        expected_stone = 64 if dropped_item == stone_id else 63
        assert picked_content["slots"][36]["count"] == expected_stone, (
            dropped_item, picked_content["slots"][36])

        send(sock, threshold, PLAY_MOVE_POS,
             struct.pack(">ddd", 40.5, 64.0, 7.5) + b"\x01")
        moved_chunks = set()
        moved_batch = -1
        while moved_batch < 0:
            pid, body = read_packet(sock, threshold)
            if pid == PLAY_CHUNK:
                x, z = struct.unpack(">ii", body[:8])
                moved_chunks.add((x, z))
            elif pid == PLAY_BATCH_FINISHED:
                moved_batch, _ = rd_varint(body)
            elif pid == PLAY_DISCONNECT:
                reason, _ = rd_nbt_component(body)
                raise AssertionError(f"{protocol}: movement disconnected: {reason}")
        assert moved_batch == side * side, (moved_batch, side)
        assert len(moved_chunks) == side * side, (len(moved_chunks), side)
        assert (2, 0) in moved_chunks, sorted(moved_chunks)[:5]

        message = f"hello from {protocol}"
        send(sock, threshold, PLAY_CHAT, varint(len(message)) + message.encode())
        while True:
            pid, body = read_packet(sock, threshold)
            if pid == PLAY_SYSTEM_CHAT:
                text = chat_text(body)
                assert text == f"<{name}> {message}", text
                break
            if pid == PLAY_DISCONNECT:
                reason, _ = rd_nbt_component(body)
                raise AssertionError(f"{protocol}: chat disconnected: {reason}")

        command_reply = send_translated_command(
            sock, threshold, "time query daytime",
            f"{protocol}/{case['pack']} time command")
        assert command_reply.startswith("daytime is "), command_reply
        print(f"{protocol}/{case['pack']}: command {command_reply!r}")
        for command, expected in [
                ("weather clear", "Set the weather to clear"),
                ("gamemode creative",
                 "Only /time and /weather are available on a translated connection yet.")]:
            command_reply = send_translated_command(
                sock, threshold, command,
                f"{protocol}/{case['pack']} command {command!r}")
            assert command_reply == expected, (command, command_reply)
            print(f"{protocol}/{case['pack']}: command {command_reply!r}")

        # This raw T0 client has no reader thread; when a check delegates to a
        # second bot, or the canonical join blocks, its socket can be silent for
        # longer than the server's 30 s read backstop. A real client is still
        # reading and answering keep-alives, so keep the connection live with
        # the same packet. A no-op keep-alive does not touch play state, unlike
        # a synthetic player-input packet.
        stop_heartbeat = threading.Event()

        def keep_t0_alive():
            while not stop_heartbeat.wait(5.0):
                try:
                    send(sock, threshold, SB_KEEP_ALIVE, b"\x00" * 8)
                except OSError:
                    return

        heartbeat = threading.Thread(target=keep_t0_alive, daemon=True)
        heartbeat.start()
        try:
            run_menu_checks(port, sock, threshold, protocol, name, case)
            run_player_tracking_checks(port, sock, threshold, protocol, name, case, entity_id)
            print(f"{protocol}/{case['pack']}: {registries} registries, {tags} tag bytes, "
                  f"{len(chunks)} chunks, Join {join_bytes}B, inventory/place/break/pickup, "
                  f"menus, chat {text!r}")
        finally:
            stop_heartbeat.set()
            heartbeat.join(timeout=2.0)
    finally:
        sock.close()


def main() -> None:
    if BIN is None:
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        return 1
    data_root = DATA_ROOT
    for version in ("26.1", "26.1.1", "26.1.2", "26.2"):
        assert (data_root / version / "clientconfig" / "tags.bin").is_file(), \
            f"the {version} client-config capture is missing"

    proc = None
    try:
        proc, port = wire_client.start_server(
            [str(BIN), "--host", HOST, "--port", "0", "--protocol", "777",
             "--version-name", "26.3-test", "--offline-mode", "--worldgen", "42",
             "--view-distance", "2",
             "--data", str(data_root)],
            wait_listening)
        for case in CASES:
            run_client(port, case["protocol"], case)

        if not wire_client.skip_server_check(
                "the SIGTERM exit code (this run has no server process)"):
            proc.terminate()
            try:
                rc = proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
                raise AssertionError("plusmc did not exit after SIGTERM")
            assert rc == 0, f"expected clean exit on SIGTERM, got {rc}"

        print("26.1/26.2 login/configuration/play entry: OK")
        print("PASS")
        return 0
    except BaseException:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        if SERVER_LOG:
            print("--- last server log lines ---", file=sys.stderr)
            for raw in list(SERVER_LOG)[-60:]:
                sys.stderr.write(raw.decode(errors="replace"))
        raise


if __name__ == "__main__":
    sys.exit(main() or 0)
