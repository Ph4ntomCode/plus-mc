#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Play the server as a named player and render what that player sees.

The premium server rejects unauthenticated clients, so this starts its own
loopback instance of the same binary (``--offline-mode``, creative, seed 1) and
drives it through the real serverbound play packets: join, chat, walk, break a
block, place it back.  Everything reported below is what the *server* sent in
answer -- the chunk stream, the block updates, the chat echo -- not a reading of
the generator.

The identity is the local player's name.  On an offline instance the server
derives the UUID from that name (``OfflinePlayer:<name>``), which is exactly
what a premium login would replace with the account's Mojang UUID; the real
profile UUID is printed for reference only.

First-person frames are ray-cast from the client's own received chunk data, so
they show the same world the client's mesher would: one ray per pixel, DDA
through the block grid, face-shaded, with distance fog. When prepared client assets are available
(``PLUSMC_ASSETS`` or ``data/vanilla/<version>/assets``), block faces are
sampled from the real block models and PNG textures; a cached client jar
(``PLUSMC_CLIENT_JAR`` or ``data/vanilla/_cache/client-*.jar``) is the
fallback, and only when neither exists is the old flat-colour fallback used.

Usage:
    player_session.py /path/to/plusmc out_dir [player-name]
                      [--dimension overworld|nether|end] [--container]
"""

from __future__ import annotations

import argparse
import math
import pathlib
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
import uuid as uuidlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tests"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import join_test as JT  # noqa: E402
import nbt_component as NBT  # noqa: E402
import chunk_decode as CD  # noqa: E402
from block_textures import BlockTextures, find_texture_source  # noqa: E402
from item_textures import ItemTextures, item_table  # noqa: E402

from PIL import Image  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parent.parent
SEED = 1
VIEW_DISTANCE = 4
FOV = 70.0
EYE_HEIGHT = 1.62
WHITE = (255, 255, 255)

# 26.2 wire ids (protocol/packet_ids.generated.h, 776).
SB_CHAT = 0x09
SB_POSITION = 0x1E
SB_POSITION_LOOK = 0x1F
SB_LOOK = 0x20
SB_PLAYER_ACTION = 0x29
SB_SWING = 0x2E  # "punch"
SB_USE_ITEM_ON = 0x42
SB_SET_CREATIVE_SLOT = 0x39
SB_SET_CARRIED = 0x36
SB_CONTAINER_CLICK = 0x12
CB_KEEP_ALIVE = 0x2D
CB_CHUNK = 0x2E
CB_BATCH_FINISHED = 0x0B
CB_BLOCK_UPDATE = 0x08
CB_SYSTEM_CHAT = 0x7C
CB_ENTITY_TELEPORT = 0x80
CB_CONTAINER_SET_CONTENT = 0x12
CB_CONTAINER_SET_SLOT = 0x14
CB_OPEN_SCREEN = 0x3C
SB_KEEP_ALIVE = 0x1C


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def block_table() -> dict:
    text = (REPO / "src/world/block_states.h").read_text()
    entries = re.findall(r'\{"([^"]+)", "([^"]*)", (\d+)\}', text)
    return {int(sid): (name if not props else f"{name}[{props}]")
            for name, props, sid in entries}


IDS = block_table()


def base_name(state: str) -> str:
    return state.split("[")[0]


def is_liquid(state: str) -> bool:
    return base_name(state) in ("minecraft:water", "minecraft:lava")


TEXTURES = None
ITEM_TEXTURES = None
ITEM_IDS = {name: item_id for name, item_id in item_table(REPO)}
ITEM_NAMES = {item_id: name for name, item_id in ITEM_IDS.items()}
SKY_TOP = (102, 153, 255)
SKY_BOTTOM = (200, 220, 255)
# The ray-cast scene is deliberately coded in Python for the debug preview. A
# half-resolution render is upscaled to the requested frame size; it keeps the
# full smoke run bounded without changing the published preview dimensions.
RENDER_SCALE = 0.5
SAMPLE_ITEMS = (
    "minecraft:potion", "minecraft:skeleton_skull", "minecraft:red_banner",
    "minecraft:shield", "minecraft:copper_golem_statue", "minecraft:stone",
    "minecraft:oak_stairs", "minecraft:diamond_sword", "minecraft:apple",
)
SAMPLE_COUNTS = (1, 1, 1, 1, 1, 64, 8, 1, 3)
SAMPLE_DISPLAY_NAMES = {0: "Healing Draught"}


def _nbt_text(text: str) -> bytes:
    raw = text.encode()
    return b"\x08" + struct.pack(">H", len(raw)) + raw


def _potion_contents() -> bytes:
    # PotionContents: present potion 0, no colour, no custom effects, no name.
    return b"\x01" + JT.varint(0) + b"\x00" + JT.varint(0) + b"\x00"


def _custom_model_data() -> bytes:
    # CustomModelData: font/flags/strings/colors lists, each count-prefixed.
    return (JT.varint(1) + struct.pack(">f", 123.0) +
            JT.varint(0) + JT.varint(0) + JT.varint(0))


def _food() -> bytes:
    # FoodProperties: nutrition, saturation, can_always_eat.
    return (JT.varint(4) + struct.pack(">f", 2.4) + b"\x00")


def _tool() -> bytes:
    # Tool: one rule over a block holder, optional speed/correctness, default
    # mining speed, damage per block and the creative flag.
    return (JT.varint(1) + JT.varint(2) + JT.varint(10) +
            b"\x01" + struct.pack(">f", 8.0) + b"\x01\x01" +
            struct.pack(">f", 1.0) + JT.varint(1) + b"\x01")


def _equippable() -> bytes:
    # Equippable: offhand slot, sound holder, three absent optionals, five
    # flags, then the shearing-sound holder.
    return (JT.varint(1) + JT.varint(1) + b"\x00\x00\x00" +
            b"\x00\x01\x00\x01\x00" + JT.varint(1))


# Serverbound creative-slot values are delimited. The server stores the
# components it does not interpret as opaque bytes and echoes them back, so the
# preview's HUD and item frames see the same custom name and component-bearing
# stacks that were seeded here.
SAMPLE_COMPONENTS = {
    0: ((6, _nbt_text(SAMPLE_DISPLAY_NAMES[0])),
        (53, _potion_contents())),
    3: ((32, _equippable()),),       # shield: equippable
    5: ((17, _custom_model_data()),),  # stone: custom_model_data
    7: ((28, _tool()),),             # diamond sword: tool
    8: ((23, _food()),),             # apple: food
}


def creative_stack(item_name: str, count: int, hotbar: int) -> bytes:
    """A serverbound ItemStack, including the delimited component patch."""
    item_id = ITEM_IDS.get(item_name)
    if item_id is None:
        raise ValueError(f"unknown sample item {item_name}")
    components = SAMPLE_COMPONENTS.get(hotbar, ())
    out = JT.varint(count) + JT.varint(item_id)
    out += JT.varint(len(components)) + JT.varint(0)
    for component, value in components:
        out += JT.varint(component) + JT.varint(len(value)) + value
    return out


def block_tint(state: str):
    """The plains-biome tint vanilla applies to the common cutout blocks."""
    name = base_name(state)
    if name.startswith("minecraft:"):
        name = name[len("minecraft:"):]
    if name in ("grass_block", "grass", "short_grass", "tall_grass", "fern",
                "large_fern", "potted_fern"):
        return (145, 189, 89)
    if name.endswith("_leaves") or name in ("vine", "potted_oak_sapling",
                                            "oak_sapling", "spruce_sapling",
                                            "birch_sapling", "jungle_sapling",
                                            "acacia_sapling", "dark_oak_sapling",
                                            "mangrove_propagule"):
        return (89, 174, 48)
    if name in ("water", "bubble_column"):
        return (63, 118, 228)
    return None


def face_uv(axis: int, dx: float, dy: float, dz: float,
            hit: tuple[float, float, float]) -> tuple[str, float, float]:
    """Map the entered face and its hit point to a Minecraft texture face."""
    x, y, z = hit
    fx, fy, fz = x - math.floor(x), y - math.floor(y), z - math.floor(z)
    if axis == 1:
        if dy < 0:
            return "up", fx, 1.0 - fz
        return "down", fx, fz
    if axis == 2:
        if dz < 0:
            return "south", 1.0 - fx, 1.0 - fy
        return "north", fx, 1.0 - fy
    if dx < 0:
        return "east", 1.0 - fz, 1.0 - fy
    return "west", fz, 1.0 - fy


def decode_cells(section: dict) -> list:
    bits = section["states_bits"]
    if bits == 0:
        return [section["states"][0]] * 4096
    per, mask = 64 // bits, (1 << bits) - 1
    raw, entries = section["states_raw"], section["states"]
    return [entries[(raw[i // per] >> ((i % per) * bits)) & mask] for i in range(4096)]


class VoxelWorld:
    """The joined client's view of the world: only chunks it was actually sent."""

    def __init__(self, bodies: dict, min_y: int = -64):
        self.bodies = bodies
        self.min_y = min_y
        self.max_y = None
        self.decoded = {}
        self.cells = {}
        self.overrides = {}  # (x, y, z) -> state name, from block updates

    def chunk(self, cx: int, cz: int):
        if (cx, cz) in self.decoded:
            return self.decoded[(cx, cz)]
        body = self.bodies.get((cx, cz))
        if body is None:
            return None
        dec = CD.decode(body)
        self.decoded[(cx, cz)] = dec
        if self.max_y is None:
            self.max_y = self.min_y + len(dec["sections"]) * 16 - 1
        return dec

    def apply_update(self, x: int, y: int, z: int, state: str) -> None:
        """A clientbound block update: the client replaces that cell."""
        self.overrides[(x, y, z)] = state

    def state(self, x: int, y: int, z: int):
        if y < self.min_y:
            return "minecraft:air"
        override = self.overrides.get((x, y, z))
        if override is not None:
            return override
        dec = self.chunk(x >> 4, z >> 4)
        if dec is None:
            return None  # outside what the server streamed
        sy = (y - self.min_y) // 16
        if sy < 0 or sy >= len(dec["sections"]):
            return "minecraft:air"
        key = (x >> 4, z >> 4, sy)
        cells = self.cells.get(key)
        if cells is None:
            cells = decode_cells(dec["sections"][sy])
            self.cells[key] = cells
        return IDS.get(cells[((y - self.min_y) % 16 * 16 + (z & 15)) * 16 + (x & 15)], "?")

    def ground(self, x: int, z: int, start_y: int | None = None):
        """First non-air block at or below `start_y` (water counts)."""
        max_y = self.max_y if self.max_y is not None else 319
        start = max_y if start_y is None else min(max_y, start_y)
        for y in range(start, self.min_y - 1, -1):
            if self.state(x, y, z) not in ("minecraft:air", None, "?"):
                return y
        return None

    def solid(self, x: int, y: int, z: int) -> bool:
        name = self.state(x, y, z)
        if name is None or name in ("minecraft:air", "?"):
            return False
        return not is_liquid(name)


# --- block colours ---------------------------------------------------------
COLORS = {
    "minecraft:grass_block": (95, 159, 53),
    "minecraft:dirt": (134, 96, 67),
    "minecraft:stone": (125, 125, 125),
    "minecraft:gravel": (128, 122, 118),
    "minecraft:sand": (219, 207, 163),
    "minecraft:water": (48, 95, 190),
    "minecraft:bedrock": (85, 85, 85),
    "minecraft:deepslate": (80, 80, 84),
    "minecraft:snow_block": (240, 245, 250),
    "minecraft:packed_ice": (141, 180, 250),
    "minecraft:mycelium": (111, 99, 105),
    "minecraft:podzol": (91, 63, 24),
    "minecraft:terracotta": (152, 94, 67),
    "minecraft:tuff": (108, 109, 102),
}


def color_for(state: str):
    name = base_name(state)
    if name in COLORS:
        return COLORS[name]
    h = 0
    for ch in name:
        h = (h * 131 + ord(ch)) & 0xFFFFFFFF
    return (70 + (h & 0x3F), 70 + ((h >> 6) & 0x3F), 70 + ((h >> 12) & 0x3F))


def sky_color(dy: float):
    t = max(0.0, min(1.0, (dy + 0.35) / 1.35))
    return tuple(int(SKY_BOTTOM[i] + (SKY_TOP[i] - SKY_BOTTOM[i]) * t)
                 for i in range(3))


def ray_dir(yaw: float, pitch: float, nx: float, ny: float, aspect: float):
    """Camera ray for a screen point in [-1, 1] x [-1, 1].

    Minecraft yaw 0 faces +Z and yaw increases clockwise seen from above, so
    forward = (-sin, 0, cos) and right = (-cos, 0, -sin). Pitch is positive
    downward, as on the wire.
    """
    yr, pr = math.radians(yaw), math.radians(pitch)
    half = math.tan(math.radians(FOV) / 2.0)
    vx = math.cos(pr) * -math.sin(yr) + (-math.cos(yr)) * (nx * half * aspect)
    vy = -math.sin(pr) + ny * half
    vz = math.cos(pr) * math.cos(yr) + (-math.sin(yr)) * (nx * half * aspect)
    n = (vx * vx + vy * vy + vz * vz) ** 0.5
    return vx / n, vy / n, vz / n


def cast(world: VoxelWorld, ox: float, oy: float, oz: float, dx: float, dy: float, dz: float,
         max_dist: float = 96.0):
    """DDA. Returns (state, face_axis, distance) of the first solid block."""
    import math
    x, y, z = math.floor(ox), math.floor(oy), math.floor(oz)
    step = (1 if dx > 0 else -1, 1 if dy > 0 else -1, 1 if dz > 0 else -1)
    t_max, t_delta = [0.0, 0.0, 0.0], [0.0, 0.0, 0.0]
    o = (ox, oy, oz)
    d = (dx, dy, dz)
    for i in range(3):
        if abs(d[i]) < 1e-9:
            t_max[i] = 1e30
            t_delta[i] = 1e30
        else:
            nxt = (x, y, z)[i] + (1 if step[i] > 0 else 0)
            t_max[i] = (nxt - o[i]) / d[i]
            t_delta[i] = abs(1.0 / d[i])
    t = 0.0
    last_axis = 1
    for _ in range(512):
        axis = 0 if t_max[0] < t_max[1] else 1
        if t_max[2] < t_max[axis]:
            axis = 2
        t = t_max[axis]
        if t > max_dist:
            return None
        pos = [x, y, z]
        pos[axis] += step[axis]
        x, y, z = pos
        t_max[axis] += t_delta[axis]
        state = world.state(x, y, z)
        if state is None:
            return None
        name = base_name(state)
        if is_liquid(name):
            last_axis = axis
            continue
        if state != "minecraft:air" and state != "?":
            return state, axis, t
        last_axis = axis
    return None


def cast_visible(world: VoxelWorld, ox: float, oy: float, oz: float,
                 dx: float, dy: float, dz: float, max_dist: float = 96.0):
    """Walk the block grid and intersect each block's real model boxes."""
    if TEXTURES is None:
        return cast(world, ox, oy, oz, dx, dy, dz, max_dist)
    x, y, z = math.floor(ox), math.floor(oy), math.floor(oz)
    step = (1 if dx > 0 else -1, 1 if dy > 0 else -1, 1 if dz > 0 else -1)
    t_max, t_delta = [0.0, 0.0, 0.0], [0.0, 0.0, 0.0]
    o, d = (ox, oy, oz), (dx, dy, dz)
    for i in range(3):
        if abs(d[i]) < 1e-9:
            t_max[i] = t_delta[i] = 1e30
        else:
            nxt = (x, y, z)[i] + (1 if step[i] > 0 else 0)
            t_max[i] = (nxt - o[i]) / d[i]
            t_delta[i] = abs(1.0 / d[i])
    t = 0.0
    for _ in range(512):
        axis = 0 if t_max[0] < t_max[1] else 1
        if t_max[2] < t_max[axis]:
            axis = 2
        t = t_max[axis]
        if t > max_dist:
            return None
        pos = [x, y, z]
        pos[axis] += step[axis]
        x, y, z = pos
        t_max[axis] += t_delta[axis]
        state = world.state(x, y, z)
        if state is None:
            return None
        name = base_name(state)
        if name == "minecraft:water" or state in ("minecraft:air", "?"):
            continue
        hit = TEXTURES.surface_hit(
            state, ((ox - x) * 16.0, (oy - y) * 16.0, (oz - z) * 16.0),
            (dx, dy, dz), block_tint(state))
        if hit is not None:
            return state, hit, t + hit.distance
    return None


def render(world: VoxelWorld, cam_x: float, cam_y: float, cam_z: float,
           yaw: float, pitch: float, width: int = 400, height: int = 225,
           scale: float | None = None):
    """Render a frame, optionally at a lower internal resolution.

    The ray caster is pure Python, so a full 400x225 frame is too slow for a
    complete smoke run. The default half-resolution pass is upscaled to the
    requested output size; HUD composition still happens at full size.
    """
    if scale is None:
        scale = RENDER_SCALE
    scale = max(0.1, min(1.0, scale))
    rw = max(1, int(width * scale))
    rh = max(1, int(height * scale))
    img = Image.new("RGB", (rw, rh))
    px = img.load()
    aspect = width / height
    for j in range(rh):
        sy = (0.5 - (j + 0.5) / rh) * 2.0
        for i in range(rw):
            sx = ((i + 0.5) / rw - 0.5) * 2.0
            dx, dy, dz = ray_dir(yaw, pitch, sx, sy, aspect)
            hit = cast_visible(world, cam_x, cam_y, cam_z, dx, dy, dz)
            sky = sky_color(dy)
            if hit is None:
                px[i, j] = sky
                continue
            if TEXTURES is not None:
                state, surface, dist = hit
                face, texel = surface.face, surface.texel
                r, g, b, alpha = texel
                if alpha < 255:
                    # Cutout/translucent texels blend against the sky instead
                    # of turning black; a second ray is beyond this preview.
                    r = (r * alpha + sky[0] * (255 - alpha)) // 255
                    g = (g * alpha + sky[1] * (255 - alpha)) // 255
                    b = (b * alpha + sky[2] * (255 - alpha)) // 255
            else:
                state, axis, dist = hit
                base = base_name(state)
                r, g, b = color_for(state)
                face, u, v = face_uv(axis, dx, dy, dz,
                                     (cam_x + dx * dist, cam_y + dy * dist, cam_z + dz * dist))
                if base.startswith("minecraft:grass"):
                    # Fallback only: the top face is grass, the sides are dirt.
                    r, g, b = ((95, 159, 53) if axis == 1 and dy < 0 else (134, 96, 67))
            shade = (1.0 if face == "up" else 0.5 if face == "down"
                     else 0.80 if face in ("north", "south") else 0.66)
            fog = min(1.0, max(0.0, (dist - 40.0) / 56.0))
            px[i, j] = tuple(int((c * shade) * (1 - fog) + s * fog) for c, s in zip((r, g, b), sky))
    if (rw, rh) != (width, height):
        img = img.resize((width, height), getattr(Image, "Resampling", Image).NEAREST)
    return img


# --- server / player -------------------------------------------------------
def start_server(binary: pathlib.Path, log_path: pathlib.Path, dimension: str = "overworld"):
    port = free_port()
    cfg = pathlib.Path(tempfile.mkdtemp(prefix="plusmc-play-"))
    log = log_path.open("wb")
    proc = subprocess.Popen(
        [str(binary), "--host", "127.0.0.1", "--port", str(port),
         "--motd", "+MC playtest", "--offline-mode", "--gamemode", "1",
         "--view-distance", str(VIEW_DISTANCE), "--worldgen", str(SEED),
         "--dimension", dimension,
         "--data", str(REPO / "data" / "vanilla"), "--accept-eula",
         "--config-dir", str(cfg)],
        stdout=log, stderr=subprocess.STDOUT, env={**__import__("os").environ})
    spawn = None
    deadline = time.time() + 180
    while time.time() < deadline:
        text = log_path.read_text(errors="replace") if log_path.exists() else ""
        m = re.search(r"worldgen: (?:[^;\n]+; )?seed -?\d+; spawn (-?\d+),(-?\d+),(-?\d+)",
                      text)
        if m:
            spawn = tuple(int(v) for v in m.groups())
        if f"listening on 127.0.0.1:{port}" in text:
            break
        if proc.poll() is not None:
            raise AssertionError(f"server exited:\n{text[-4000:]}")
        time.sleep(0.2)
    assert spawn is not None, (
        f"no worldgen spawn line in the server log:\n{log_path.read_text(errors='replace')[-2000:]}")
    return proc, port, spawn, log


def drain(client, seconds: float, ids=None, live: LiveInventory | None = None):
    """Reads for `seconds`; returns {packet id: [bodies]} and acks keep-alives."""
    out = {}
    end = time.time() + seconds
    while time.time() < end:
        try:
            pid, body = client.read_pkt(timeout=max(0.01, end - time.time()))
        except socket.timeout:
            break
        if pid == CB_KEEP_ALIVE:
            JT.unpack_keepalive(client, body)
        if live is not None:
            if pid == CB_CONTAINER_SET_CONTENT:
                live.apply_content(body)
            elif pid == CB_CONTAINER_SET_SLOT:
                live.apply_slot(body)
        if ids is None or pid in ids:
            out.setdefault(pid, []).append(body)
    return out


def chat(client, text: str):
    payload = JT.varint(len(text)) + text.encode()
    client.send_pkt(SB_CHAT, payload)


def step_to(client, x, y, z, yaw, pitch, ground=True):
    body = struct.pack(">ddd", x, y, z) + struct.pack(">ff", yaw, pitch) + bytes([1 if ground else 0])
    client.send_pkt(SB_POSITION_LOOK, body)


def dig(client, x, y, z, face, seq):
    pos = ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF)
    client.send_pkt(SB_PLAYER_ACTION, JT.varint(0) + struct.pack(">q", pos) + bytes([face]) +
                    JT.varint(seq))
    client.send_pkt(SB_SWING, JT.varint(0))
    client.send_pkt(SB_PLAYER_ACTION, JT.varint(2) + struct.pack(">q", pos) + bytes([face]) +
                    JT.varint(seq + 1))


def decode_block_update(body: bytes):
    pos = struct.unpack(">q", body[:8])[0]
    x = pos >> 38
    z = (pos >> 12) & 0x3FFFFFF
    y = pos & 0xFFF
    if x >= 1 << 25:
        x -= 1 << 26
    if z >= 1 << 25:
        z -= 1 << 26
    if y >= 1 << 11:
        y -= 1 << 12
    sid, off = JT.rd_varint(body, 8)
    return x, y, z, sid


def pixels_differing(a: Image.Image, b: Image.Image) -> int:
    pa, pb = a.tobytes(), b.tobytes()
    return sum(1 for i in range(0, len(pa), 3) if pa[i:i + 3] != pb[i:i + 3])


def with_hud(frame: Image.Image, inventory: dict, selected: int,
             enabled: bool = True) -> Image.Image:
    """Composite the Java hotbar and selected-item name onto a frame."""
    if not enabled or ITEM_TEXTURES is None or not inventory:
        return frame
    hotbar = ITEM_TEXTURES.render_hotbar(inventory, selected)
    out = frame.convert("RGBA")
    out.alpha_composite(hotbar, ((out.width - hotbar.width) // 2,
                                 out.height - hotbar.height))
    return out.convert("RGB")


def use_item_on(client, x: int, y: int, z: int, face: int, seq: int) -> None:
    """Right-click one block with the selected hand."""
    pos = ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF)
    body = (JT.varint(0) + struct.pack(">q", pos) + JT.varint(face) +
            struct.pack(">fff", 0.5, 1.0, 0.5) + b"\x00\x00" + JT.varint(seq))
    client.send_pkt(SB_USE_ITEM_ON, body)


def give_creative(client, item_name: str, hotbar: int = 0) -> bool:
    item_id = ITEM_IDS.get(item_name)
    if item_id is None:
        return False
    stack = JT.varint(1) + JT.varint(item_id) + JT.varint(0) + JT.varint(0)
    client.send_pkt(SB_SET_CREATIVE_SLOT, struct.pack(">h", 36 + hotbar) + stack)
    client.send_pkt(SB_SET_CARRIED, struct.pack(">h", hotbar))
    drain(client, 0.1)
    return True


def decode_open_screen(body: bytes):
    window, pos = JT.rd_varint(body, 0)
    menu, pos = JT.rd_varint(body, pos)
    return window, menu


def read_effect_details(body: bytes, pos: int, depth: int = 0):
    """MobEffectInstance.Details, including any nested hidden layer."""
    if depth > 8:
        raise ValueError("potion effect hidden-layer nesting is too deep")
    amplifier, pos = JT.rd_varint(body, pos)
    duration, pos = JT.rd_varint(body, pos)
    if pos + 4 > len(body):
        raise ValueError("potion effect details truncated")
    ambient, visible, show_icon, has_hidden = body[pos:pos + 4]
    pos += 4
    out = {"amplifier": amplifier, "duration": duration,
           "ambient": bool(ambient), "visible": bool(visible),
           "show_icon": bool(show_icon)}
    if has_hidden:
        hidden, pos = read_effect_details(body, pos, depth + 1)
        out["hidden"] = hidden
    return out, pos


def read_potion_contents(body: bytes, pos: int):
    """PotionContents.STREAM_CODEC in a clientbound item stack.

    The order matters: the potion holder, display colour, custom-effect list and
    display name are all optional/separate fields, so a decoder cannot skip the
    middle ones and remain aligned.
    """
    has_potion = body[pos]
    pos += 1
    potion = None
    if has_potion:
        potion, pos = JT.rd_varint(body, pos)
    has_color = body[pos]
    pos += 1
    color = None
    if has_color:
        color, pos = JT.rd_varint(body, pos)
    count, pos = JT.rd_varint(body, pos)
    effects = []
    for _ in range(count):
        effect, pos = JT.rd_varint(body, pos)
        details, pos = read_effect_details(body, pos)
        effects.append({"effect": effect, **details})
    has_name = body[pos]
    pos += 1
    custom_name = None
    if has_name:
        custom_name, pos = JT.rd_string(body, pos)
    return {"potion": potion, "custom_color": color,
            "custom_effects": effects, "custom_name": custom_name}, pos


def read_custom_name(body: bytes, pos: int):
    """The custom_name component: a network-NBT text component.

    Component values in a clientbound patch have no per-value length, so this
    uses the shared NBT reader to consume the complete tag tree. A plain UTF
    string is accepted as a fallback for older/simplified encoders.
    """
    if body[pos] not in (0x08, 0x0A):
        return JT.rd_string(body, pos)
    node, pos = NBT.rd_nbt_tree(body, pos)
    text = node.get("text") or node.get("translate")
    if not isinstance(text, str):
        raise ValueError(f"custom_name component has no text: {node}")
    return text, pos


def _read_bool(body: bytes, pos: int):
    if pos >= len(body):
        raise ValueError("boolean component is truncated")
    return bool(body[pos]), pos + 1


def _read_float(body: bytes, pos: int):
    if pos + 4 > len(body):
        raise ValueError("float component is truncated")
    return struct.unpack(">f", body[pos:pos + 4])[0], pos + 4


def _read_int(body: bytes, pos: int):
    if pos + 4 > len(body):
        raise ValueError("int component is truncated")
    return struct.unpack(">i", body[pos:pos + 4])[0], pos + 4


def _read_optional(body: bytes, pos: int, reader):
    present, pos = _read_bool(body, pos)
    if not present:
        return None, pos
    return reader(body, pos)


def _read_holder(body: bytes, pos: int):
    """A registry Holder: id + 1, or 0 followed by a direct value."""
    holder, pos = JT.rd_varint(body, pos)
    if holder == 0:
        raise ValueError("direct registry holders are not modelled")
    return holder - 1, pos


def _read_holder_set(body: bytes, pos: int):
    """HolderSet: 0 + tag name, otherwise count + direct Holder references."""
    count, pos = JT.rd_varint(body, pos)
    if count == 0:
        tag, pos = JT.rd_string(body, pos)
        return {"tag": tag}, pos
    holders = []
    for _ in range(count - 1):
        holder, pos = _read_holder(body, pos)
        holders.append(holder)
    return {"holders": holders}, pos


def _read_sound_event(body: bytes, pos: int):
    """Holder<SoundEvent>; direct sounds include an identifier and range."""
    holder, pos = JT.rd_varint(body, pos)
    if holder:
        return {"sound": holder - 1}, pos
    location, pos = JT.rd_string(body, pos)
    fixed_range, pos = _read_optional(body, pos, _read_float)
    return {"direct": {"location": location, "fixed_range": fixed_range}}, pos


def read_custom_model_data(body: bytes, pos: int):
    """CustomModelData.STREAM_CODEC: four separately counted lists."""
    out = {}
    for key, reader in (("floats", _read_float), ("flags", _read_bool),
                        ("strings", JT.rd_string), ("colors", _read_int)):
        count, pos = JT.rd_varint(body, pos)
        values = []
        for _ in range(count):
            value, pos = reader(body, pos)
            values.append(value)
        out[key] = values
    return out, pos


def read_food(body: bytes, pos: int):
    """FoodProperties.STREAM_CODEC: nutrition, saturation, always-edible."""
    nutrition, pos = JT.rd_varint(body, pos)
    saturation, pos = _read_float(body, pos)
    can_always_eat, pos = _read_bool(body, pos)
    return {"nutrition": nutrition, "saturation": saturation,
            "can_always_eat": can_always_eat}, pos


def read_tool(body: bytes, pos: int):
    """Tool.STREAM_CODEC, including each rule's holder set and optionals."""
    count, pos = JT.rd_varint(body, pos)
    rules = []
    for _ in range(count):
        blocks, pos = _read_holder_set(body, pos)
        speed, pos = _read_optional(body, pos, _read_float)
        correct, pos = _read_optional(body, pos, _read_bool)
        rules.append({"blocks": blocks, "speed": speed,
                      "correct_for_drops": correct})
    default_speed, pos = _read_float(body, pos)
    damage_per_block, pos = JT.rd_varint(body, pos)
    can_destroy, pos = _read_bool(body, pos)
    return {"rules": rules, "default_mining_speed": default_speed,
            "damage_per_block": damage_per_block,
            "can_destroy_blocks_in_creative": can_destroy}, pos


def read_equippable(body: bytes, pos: int):
    """Equippable.STREAM_CODEC: slot, sounds, optionals, then five flags."""
    slot, pos = JT.rd_varint(body, pos)
    equip_sound, pos = _read_sound_event(body, pos)
    asset, pos = _read_optional(body, pos, JT.rd_string)
    overlay, pos = _read_optional(body, pos, JT.rd_string)
    allowed, pos = _read_optional(body, pos, _read_holder_set)
    flags = []
    for _ in range(5):
        flag, pos = _read_bool(body, pos)
        flags.append(flag)
    shearing_sound, pos = _read_sound_event(body, pos)
    return {"slot": slot, "equip_sound": equip_sound, "asset_id": asset,
            "camera_overlay": overlay, "allowed_entities": allowed,
            "dispensable": flags[0], "swappable": flags[1],
            "damage_on_hurt": flags[2], "equip_on_interact": flags[3],
            "can_be_sheared": flags[4],
            "shearing_sound": shearing_sound}, pos


def read_stack(body: bytes, pos: int):
    """An ItemStack, including the clientbound DataComponentPatch.

    Clientbound component values are not length-prefixed, so every codec the
    item registry can put on the wire has to consume its own bytes. All eleven
    component ids in data/vanilla/26.3/item_components.tsv are decoded here;
    a removed component is a bare type id.
    """
    count, pos = JT.rd_varint(body, pos)
    if count <= 0:
        return None, pos
    item, pos = JT.rd_varint(body, pos)
    added, pos = JT.rd_varint(body, pos)
    removed, pos = JT.rd_varint(body, pos)
    stack = {"item": item, "count": count}
    for _ in range(added):
        cid, pos = JT.rd_varint(body, pos)
        if cid == 1:  # minecraft:max_stack_size
            stack["max_stack_size"], pos = JT.rd_varint(body, pos)
        elif cid == 2:  # minecraft:max_damage
            stack["max_damage"], pos = JT.rd_varint(body, pos)
        elif cid == 3:  # minecraft:damage
            stack["damage"], pos = JT.rd_varint(body, pos)
        elif cid == 6:  # minecraft:custom_name
            stack["custom_name"], pos = read_custom_name(body, pos)
        elif cid == 13:  # minecraft:enchantments
            n, pos = JT.rd_varint(body, pos)
            levels = {}
            for _ in range(n):
                enchantment, pos = JT.rd_varint(body, pos)
                level, pos = JT.rd_varint(body, pos)
                levels[enchantment] = level
            stack["enchantments"] = levels
        elif cid == 17:  # minecraft:custom_model_data
            stack["custom_model_data"], pos = read_custom_model_data(body, pos)
        elif cid == 23:  # minecraft:food
            stack["food"], pos = read_food(body, pos)
        elif cid == 28:  # minecraft:tool
            stack["tool"], pos = read_tool(body, pos)
        elif cid == 32:  # minecraft:equippable
            stack["equippable"], pos = read_equippable(body, pos)
        elif cid == 53:  # minecraft:potion_contents
            stack["potion_contents"], pos = read_potion_contents(body, pos)
        elif cid == 58:  # minecraft:trim
            material, pos = JT.rd_varint(body, pos)
            pattern, pos = JT.rd_varint(body, pos)
            stack["trim"] = (material - 1, pattern - 1)
        else:
            raise ValueError(f"clientbound stack component {cid} is not modelled")
    for _ in range(removed):
        _, pos = JT.rd_varint(body, pos)
    return stack, pos


def decode_content(body: bytes):
    window, pos = JT.rd_varint(body, 0)
    state, pos = JT.rd_varint(body, pos)
    count, pos = JT.rd_varint(body, pos)
    slots = []
    for _ in range(count):
        stack, pos = read_stack(body, pos)
        slots.append(stack)
    read_stack(body, pos)  # cursor, not needed for the screenshot
    return window, state, slots


class LiveInventory:
    """The player menu as the server describes it: container 0, menu slots.

    The server sends the full 46-slot menu once at join and a single menu-slot
    update for every mutation. Applying both packets in receive order gives the
    preview the same hotbar the real client would show (36..44 are the hotbar).
    """

    def __init__(self):
        self.window = 0
        self.state = 0
        self.slots: list[dict | None] = []
        self.syncs = 0
        self.updates = 0

    def apply_content(self, body: bytes) -> bool:
        window, state, slots = decode_content(body)
        if window != 0:
            return False
        self.window, self.state, self.slots = window, state, slots
        self.syncs += 1
        return True

    def apply_slot(self, body: bytes) -> bool:
        window, state, slot, stack = decode_set_slot(body)
        if window != 0 or not 0 <= slot < len(self.slots):
            return False
        self.slots[slot] = stack
        self.state = state
        self.updates += 1
        return True

    def named(self) -> dict[int, dict | None]:
        out = {}
        for slot, stack in enumerate(self.slots):
            if stack:
                entry = {
                    "name": ITEM_NAMES.get(stack["item"],
                                          f"minecraft:item_{stack['item']}"),
                    "count": stack["count"],
                }
                custom = stack.get("custom_name")
                contents = stack.get("potion_contents")
                if not custom and contents:
                    custom = contents.get("custom_name")
                if custom:
                    entry["custom_name"] = custom
                if contents:
                    entry["potion_contents"] = contents
                out[slot] = entry
        return out

    def hotbar_count(self) -> int:
        return sum(1 for slot in range(36, 45) if slot < len(self.slots) and self.slots[slot])


def hashed_stack(item: int, count: int) -> bytes:
    if count <= 0:
        return b"\x00"
    return b"\x01" + JT.varint(item) + JT.varint(count) + JT.varint(0) + JT.varint(0)


def click_container(client, window: int, state: int, slot: int,
                    carried: tuple[int, int] = (0, 0), mode: int = 0) -> None:
    body = (JT.varint(window) + JT.varint(state) + struct.pack(">h", slot) +
            b"\x00" + JT.varint(mode) + JT.varint(0) + hashed_stack(*carried))
    client.send_pkt(SB_CONTAINER_CLICK, body)


def decode_set_slot(body: bytes):
    window, pos = JT.rd_varint(body, 0)
    state, pos = JT.rd_varint(body, pos)
    slot = struct.unpack(">h", body[pos:pos + 2])[0]
    stack, pos = read_stack(body, pos + 2)
    return window, state, slot, stack


def capture_container(client, world: VoxelWorld, px_: float, feet_y: float,
                      pz_: float, yaw: float, out_dir: pathlib.Path,
                      target: tuple[int, int, int]) -> None:
    """Place and open a chest, then render its real container screen."""
    if ITEM_TEXTURES is None:
        print("container: no texture renderer")
        return
    bx, by, bz = target
    # The restored block can be inside a tree canopy, so try every face of it
    # rather than assuming the cell above is free.
    candidates = (
        (1, (bx, by + 1, bz)),
        (2, (bx, by, bz - 1)),
        (3, (bx, by, bz + 1)),
        (4, (bx - 1, by, bz)),
        (5, (bx + 1, by, bz)),
        (0, (bx, by - 1, bz)),
    )
    cell = next(((face, pos) for face, pos in candidates
                 if world.state(*pos) in ("minecraft:air", "?")), None)
    if cell is None:
        print(f"container: no free cell beside ({bx}, {by}, {bz})")
        return
    face, (cx, cy, cz) = cell
    if not give_creative(client, "minecraft:chest", 0):
        print("container: no chest item id")
        return
    use_item_on(client, bx, by, bz, face, 10)
    got = drain(client, 1.0, ids={CB_BLOCK_UPDATE})
    placed = False
    for ux, uy, uz, sid_ in (decode_block_update(b) for b in got.get(CB_BLOCK_UPDATE, [])):
        world.apply_update(ux, uy, uz, IDS.get(sid_, "?"))
        if (ux, uy, uz) == (cx, cy, cz):
            placed = True
    if not placed:
        print(f"container: chest placement produced no update at ({cx}, {cy}, {cz})")
        return
    use_item_on(client, cx, cy, cz, 1, 11)
    got = drain(client, 1.5, ids={CB_OPEN_SCREEN, CB_CONTAINER_SET_CONTENT})
    opened = got.get(CB_OPEN_SCREEN, [])
    if not opened:
        print("container: no open_screen")
        return
    window, menu = decode_open_screen(opened[-1])
    slots: list = []
    state = 0
    contents = got.get(CB_CONTAINER_SET_CONTENT, [])
    if contents:
        try:
            _, state, slots = decode_content(contents[-1])
        except ValueError as exc:
            print(f"container: {exc}; rendering empty slots")
    # Move the sample diamond sword from the chest menu hotbar into chest slot 0.
    # A shift-click is one click and the server answers it with the whole menu,
    # so the state id used for the screenshot is the one it just sent. (Two
    # pickup clicks would get a container_set_slot reply to the first one and
    # the second would be rejected as stale unless that reply were decoded.)
    if len(slots) >= 61 and slots[60]:
        click_container(client, window, state, 60, mode=1)
        got = drain(client, 0.6, ids={CB_CONTAINER_SET_CONTENT, CB_CONTAINER_SET_SLOT})
        if got.get(CB_CONTAINER_SET_CONTENT):
            _, state, slots = decode_content(got[CB_CONTAINER_SET_CONTENT][-1])
        else:
            for body in got.get(CB_CONTAINER_SET_SLOT, []):
                cw, cstate, slot, stack = decode_set_slot(body)
                if cw == window and 0 <= slot < len(slots):
                    slots[slot] = stack
                    state = cstate
    named = {}
    for slot, stack in enumerate(slots):
        if stack:
            name = ITEM_NAMES.get(stack["item"], f"minecraft:item_{stack['item']}")
            named[slot] = {"name": name, "count": stack["count"]}
    gui = ITEM_TEXTURES.render_container(named, rows=3)
    base = render(world, px_, feet_y + EYE_HEIGHT, pz_, yaw, 0.0)
    base.paste(gui, ((base.width - gui.width) // 2, (base.height - gui.height) // 2), gui)
    base.save(out_dir / "container.png")
    print(f"container: window {window}, menu {menu}, {len(slots)} slots, "
          f"{len(named)} occupied -> container.png")


def main() -> int:
    global TEXTURES, ITEM_TEXTURES, SKY_TOP, SKY_BOTTOM, RENDER_SCALE
    parser = argparse.ArgumentParser(description="Play the server and render what it sends")
    parser.add_argument("binary", type=pathlib.Path)
    parser.add_argument("out_dir", type=pathlib.Path)
    parser.add_argument("name", nargs="?", default="Zaidrivaz")
    parser.add_argument("--dimension", choices=("overworld", "nether", "end"),
                        default="overworld")
    parser.add_argument("--container", action="store_true")
    parser.add_argument("--skip-place-back", action="store_true",
                        help="leave the broken block in the air (negative smoke)")
    parser.add_argument("--selected", type=int, default=0, choices=range(9),
                        help="hotbar slot to highlight (default 0)")
    parser.add_argument("--no-hud", action="store_true",
                        help="write frames without the hotbar/name overlay")
    parser.add_argument("--render-scale", type=float, default=RENDER_SCALE,
                        help="internal ray-cast scale, upscaled to 400x225 "
                             "(default: %(default)s)")
    args = parser.parse_args()
    if not 0.1 <= args.render_scale <= 1.0:
        parser.error("--render-scale must be between 0.1 and 1.0")
    binary = args.binary.resolve()
    out_dir = args.out_dir.resolve()
    name = args.name
    dimension = args.dimension
    out_dir.mkdir(parents=True, exist_ok=True)
    log_path = out_dir / "server.log"

    RENDER_SCALE = args.render_scale
    if dimension == "nether":
        SKY_TOP, SKY_BOTTOM = (74, 22, 16), (34, 8, 6)
    elif dimension == "end":
        SKY_TOP, SKY_BOTTOM = (18, 12, 30), (54, 42, 72)
    source = find_texture_source(REPO)
    if source is not None:
        TEXTURES = BlockTextures(source)
        ITEM_TEXTURES = ItemTextures(TEXTURES)
        print(f"textures: {source} ({len(TEXTURES.corpus('block'))} block PNGs, "
              f"{len(TEXTURES.corpus('item'))} item PNGs)")
    else:
        print("textures: no client assets; using flat-colour fallback "
              "(run tools/vanilla_assets.py or set PLUSMC_ASSETS)")
    print(f"player_session: {name} joining seed {SEED} in the {dimension} on a loopback "
          f"instance (view distance {VIEW_DISTANCE})")
    proc, port, spawn, log = start_server(binary, log_path, dimension)
    sx, sy, sz = spawn
    client = None
    try:
        client = JT.Client(name, port)
        join_body, flags, comp = client.join()
        info = JT.decode_login_body(join_body)
        print(f"joined: entity {client.entity_id}, dimension {info['dimension']}, "
              f"game type {info['game_type']}, view distance {info['chunk_radius']}, "
              f"seed {info['seed']}, online-mode {info['online_mode']}, compression {comp}")

        live = LiveInventory()

        def drain_live(seconds: float, ids=None):
            return drain(client, seconds, ids=ids, live=live)

        chunks = {}
        expected = (2 * VIEW_DISTANCE + 1) ** 2
        deadline = time.time() + 120
        while time.time() < deadline and len(chunks) < expected:
            pid, body = client.read_pkt(timeout=10)
            if pid == CB_CHUNK:
                cx, cz = struct.unpack(">ii", body[:8])
                chunks[(cx, cz)] = body
            elif pid == CB_BATCH_FINISHED:
                break
            elif pid == CB_CONTAINER_SET_CONTENT:
                live.apply_content(body)
            elif pid == CB_CONTAINER_SET_SLOT:
                live.apply_slot(body)
            elif pid == CB_KEEP_ALIVE:
                JT.unpack_keepalive(client, body)
        print(f"chunks: {len(chunks)}/{expected} streamed")
        assert len(chunks) >= expected, sorted(chunks)

        # The player-menu sync is normally in the join burst. If a server sends
        # it after the chunk batch, give it a short window before seeding.
        if not live.slots:
            drain_live(0.5, ids={CB_CONTAINER_SET_CONTENT})
        if not live.slots:
            print("inventory: no player-menu content packet; hotbar will be empty")

        # Seed a useful creative hotbar through real serverbound slot writes.
        # The preview below is nevertheless the server's tracked menu, not the
        # local values written here: each echo is applied by LiveInventory.
        sent = 0
        for hotbar, item_name in enumerate(SAMPLE_ITEMS):
            try:
                stack = creative_stack(item_name, SAMPLE_COUNTS[hotbar], hotbar)
            except ValueError:
                continue
            client.send_pkt(SB_SET_CREATIVE_SLOT,
                            struct.pack(">h", 36 + hotbar) + stack)
            sent += 1
        deadline = time.time() + 1.5
        while live.updates < sent and time.time() < deadline:
            drain_live(0.1, ids={CB_CONTAINER_SET_CONTENT, CB_CONTAINER_SET_SLOT})
        inventory = live.named()
        if ITEM_TEXTURES is not None:
            ITEM_TEXTURES.render_inventory(inventory, selected=args.selected).save(
                out_dir / "inventory.png")
        component_keys = sorted({key for stack in live.slots if stack
                                 for key in stack
                                 if key not in ("item", "count")})
        selected_stack = inventory.get(36 + args.selected) or {}
        selected_label = selected_stack.get("custom_name")
        label_suffix = f", selected label {selected_label!r}" if selected_label else ""
        print(f"inventory: window {live.window}, state {live.state}, "
              f"syncs {live.syncs}, slot updates {live.updates}, "
              f"{live.hotbar_count()} hotbar stacks{label_suffix}, "
              f"components {component_keys} -> inventory.png")
        print(f"hud: {'off' if args.no_hud else 'hotbar + selected name'}; "
              f"selected slot {args.selected}")

        min_y = -64 if dimension == "overworld" else 0
        world = VoxelWorld(chunks, min_y)
        feet_y = sy
        if world.state(sx, sy - 1, sz) in (None, "minecraft:air", "?"):
            floor = world.ground(sx, sz, sy + 4)
            if floor is not None:
                feet_y = floor + 1
        if feet_y != sy:
            print(f"note: spawn line said y={sy}; terrain top puts feet at y={feet_y}")
        ground_name = world.state(sx, feet_y - 1, sz)
        print(f"standing on {ground_name} at ({sx}, {feet_y}, {sz})")

        # Look around from the spawn column: four cardinal frames. The one with
        # the most open sky becomes spawn.png (a view of the spawn, not the inside
        # of whatever hill faces one of the four ways).
        views = []
        for yaw in (0.0, 90.0, 180.0, 270.0):
            step_to(client, sx + 0.5, feet_y, sz + 0.5, yaw, 15.0)
            time.sleep(0.15)
            drain_live(0.05)
            img = with_hud(render(world, sx + 0.5, feet_y + EYE_HEIGHT, sz + 0.5,
                                  yaw, 15.0), inventory, args.selected, not args.no_hud)
            img.save(out_dir / f"view_yaw{int(yaw)}.png")
            top = img.crop((0, 0, img.width, img.height // 4)).tobytes()
            sky = sum(1 for p in range(0, len(top), 3) if top[p + 2] > top[p] + 40)
            views.append((sky, img, yaw))
        cam = max(views, key=lambda v: v[0])[1]
        cam.save(out_dir / "spawn.png")  # already has the HUD
        print(f"frames: view_yaw0/90/180/270.png + spawn.png ({cam.width}x{cam.height}, "
              f"openest heading yaw {max(views, key=lambda v: v[0])[2]:.0f})")

        # A direction to walk: pick the heading whose path stays on dry land at a
        # walkable gradient for the next 24 blocks (a player picks a way they can
        # actually go, rather than walking into a cliff).
        best = None
        for yaw in range(0, 360, 45):
            dx, dz = -math.sin(math.radians(yaw)), math.cos(math.radians(yaw))
            last = feet_y - 1
            score, run = 0.0, 0
            for d in range(1, 25):
                tx, tz = int(math.floor(sx + 0.5 + dx * d)), int(math.floor(sz + 0.5 + dz * d))
                # In the Nether the first solid block above the cave is its roof:
                # scanning above the feet would climb to the bedrock ceiling.
                g = world.ground(tx, tz, feet_y if dimension == "nether" else feet_y + 4)
                if g is None or is_liquid(world.state(tx, g, tz)):
                    break
                if (dimension == "nether" and g == feet_y
                        and world.state(tx, feet_y + 1, tz) not in ("minecraft:air", "?")):
                    break  # a wall, not a one-block step: do not climb it
                run = d
                if abs(g - last) > 1:
                    score += abs(g - last)
                last = g
            if best is None or (run, -score) > (best[0], -best[1]):
                best = (run, score, yaw, dx, dz)
        assert best is not None and best[0] > 0, "no dry heading at spawn"
        run, score, yaw, dx, dz = best
        print(f"walking yaw {yaw} (dry run {run} blocks, path score {score:.0f})")

        start_cx, start_cz = int(math.floor((sx + 0.5) / 16)), int(math.floor((sz + 0.5) / 16))
        px_, pz_, feet_y = sx + 0.5, sz + 0.5, feet_y
        new_chunks = 0
        ticks, walked = min(90, max(1, int(run / 0.28))), 0.0
        for _ in range(ticks):
            nx, nz = px_ + dx * 0.28, pz_ + dz * 0.28
            gx, gz = int(math.floor(nx)), int(math.floor(nz))
            g = world.ground(gx, gz, feet_y if dimension == "nether" else feet_y + 4)
            if g is None or is_liquid(world.state(gx, g, gz)):
                break
            if (dimension == "nether" and g == feet_y
                    and world.state(gx, feet_y + 1, gz) not in ("minecraft:air", "?")):
                break
            # Step up/down like a walking player: feet follow the terrain top.
            feet_y = g + 1
            step_to(client, nx, feet_y, nz, yaw, 0.0)
            px_, pz_ = nx, nz
            walked += 0.28
            got = drain_live(0.02, ids={CB_CHUNK})
            new_chunks += len(got.get(CB_CHUNK, []))
        hero_chunk = (int(math.floor(px_ / 16)), int(math.floor(pz_ / 16)))
        print(f"walked {walked:.1f} blocks to ({px_:.1f}, {feet_y:.0f}, {pz_:.1f}); "
              f"chunks streamed while moving: {new_chunks}; "
              f"left spawn chunk: {hero_chunk != (start_cx, start_cz)}")
        step_to(client, px_, feet_y, pz_, yaw, 0.0)
        time.sleep(0.1)
        with_hud(render(world, px_, feet_y + EYE_HEIGHT, pz_, yaw, 0.0),
                 inventory, args.selected, not args.no_hud).save(
                     out_dir / "walked_horizon.png")

        # Chat: the server echoes it to every player (itself included).
        chat(client, f"hello from {name}'s stand-in")
        got = drain_live(1.2, ids={CB_SYSTEM_CHAT})
        echoed = got.get(CB_SYSTEM_CHAT, [])
        print(f"chat: sent 1 message, server echoed {len(echoed)} system_chat packet(s)")

        # Break the first block the camera can actually see: level, then a
        # little down. The camera is aimed at it, so the before/after frames
        # always contain the target (the terrain here is not flat).
        inventory = live.named()
        target, pitch = None, 0.0
        for aim in (0.0, 20.0, 35.0):
            for turn in (0, 45, -45, 90, -90, 135, -135, 180):
                aim_yaw = yaw + turn
                vx, vy, vz = ray_dir(aim_yaw, aim, 0.0, 0.0, 1.0)
                hit = cast(world, px_, feet_y + EYE_HEIGHT, pz_, vx, vy, vz, max_dist=8.0)
                if hit is None:
                    continue
                _, _, dist = hit
                hx = px_ + vx * (dist + 1e-3)
                hy = feet_y + EYE_HEIGHT + vy * (dist + 1e-3)
                hz = pz_ + vz * (dist + 1e-3)
                bx, by, bz = int(math.floor(hx)), int(math.floor(hy)), int(math.floor(hz))
                bstate = world.state(bx, by, bz)
                # Only pick blocks the creative place-back path can restore:
                # lava (and other non-item states) have no hotbar item id.
                if world.solid(bx, by, bz) and ITEM_IDS.get(base_name(bstate)) is not None:
                    target, pitch, yaw = (bx, by, bz), aim, aim_yaw
                    break
            if target is not None:
                break
        assert target is not None, "no solid block in view to break"
        bx, by, bz = target
        step_to(client, px_, feet_y, pz_, yaw, pitch)
        time.sleep(0.15)
        drain_live(0.1)
        inventory = live.named()
        before = render(world, px_, feet_y + EYE_HEIGHT, pz_, yaw, pitch)
        with_hud(before, inventory, args.selected, not args.no_hud).save(
            out_dir / "before_break.png")
        broke_state = world.state(bx, by, bz)
        print(f"breaking {broke_state} at ({bx}, {by}, {bz})")
        dig(client, bx, by, bz, 1, 1)
        got = drain_live(1.0, ids={CB_BLOCK_UPDATE, CB_KEEP_ALIVE})
        updates = [decode_block_update(b) for b in got.get(CB_BLOCK_UPDATE, [])]
        changed = [u for u in updates if u[:3] == (bx, by, bz)]
        for ux, uy, uz, sid_ in updates:
            world.apply_update(ux, uy, uz, IDS.get(sid_, "?"))
        print(f"block updates received: {len(updates)}; for the dug block: {len(changed)}"
              + (f" -> {IDS.get(changed[0][3])}" if changed else ""))

        after = render(world, px_, feet_y + EYE_HEIGHT, pz_, yaw, pitch)
        with_hud(after, inventory, args.selected, not args.no_hud).save(
            out_dir / "after_break.png")
        diff = pixels_differing(before, after)
        print(f"first-person frame changed by {diff} of {before.width * before.height} pixels "
              f"after the break")

        # Put it back (creative): fill hotbar slot 0 with the broken *item* id
        # (not the block-state id), then right-click the layer below it.
        item_name = base_name(broke_state)
        item_id = ITEM_IDS.get(item_name)
        placed = False
        if args.skip_place_back:
            print("place back skipped: --skip-place-back")
        elif item_id is not None and changed:
            stack = JT.varint(1) + JT.varint(item_id) + JT.varint(0) + JT.varint(0)
            client.send_pkt(SB_SET_CREATIVE_SLOT, struct.pack(">h", 36) + stack)
            client.send_pkt(SB_SET_CARRIED, struct.pack(">h", 0))
            use_item_on(client, bx, by - 1, bz, 1, 5)
            got = drain_live(1.0, ids={CB_BLOCK_UPDATE})
            inventory = live.named()
            for ux, uy, uz, sid_ in (decode_block_update(b) for b in got.get(CB_BLOCK_UPDATE, [])):
                world.apply_update(ux, uy, uz, IDS.get(sid_, "?"))
                if (ux, uy, uz) == (bx, by, bz):
                    placed = True
            print(f"place back on top of ({bx}, {by - 1}, {bz}): "
                  f"{'block update received' if placed else 'no block update'}")
            # The place-back consumed the selected slot. Put the custom-named
            # potion sample back so the HUD label is present in restored.png too.
            updates_before_restore = live.updates
            client.send_pkt(SB_SET_CREATIVE_SLOT,
                            struct.pack(">h", 36) +
                            creative_stack(SAMPLE_ITEMS[0], SAMPLE_COUNTS[0], 0))
            deadline = time.time() + 1.5
            while live.updates <= updates_before_restore and time.time() < deadline:
                drain_live(0.1, ids={CB_CONTAINER_SET_CONTENT, CB_CONTAINER_SET_SLOT})
            inventory = live.named()
        elif item_id is None:
            print(f"place back: no item id for {item_name}")
        restored = render(world, px_, feet_y + EYE_HEIGHT, pz_, yaw, pitch)
        with_hud(restored, inventory, args.selected, not args.no_hud).save(
            out_dir / "restored.png")
        diff2 = pixels_differing(before, restored)
        action = "skipping place-back" if args.skip_place_back else "placing"
        print(f"frame vs the pre-break frame after {action}: {diff2} pixels differ")
        if args.container:
            capture_container(client, world, px_, feet_y, pz_, yaw, out_dir, target)
        print(f"frames: before_break.png, after_break.png, restored.png, walked_horizon.png")

        offline = str(uuidlib.UUID(bytes=JT.offline_uuid(name)))
        print(f"identity: {name} on an offline instance -> uuid {offline}; "
              f"a premium login would carry d604dd63-7d97-42ed-b1bc-07c7caa9d08b instead")
        drain_live(0.3)
        return 0
    finally:
        if client is not None:
            client.close()
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
