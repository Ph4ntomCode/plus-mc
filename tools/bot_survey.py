#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Survey a generated world the way a joining client sees it.

The premium server rejects cracked clients, so this tool starts its own
**offline** server for the same seed (an ephemeral loopback port, never the
running one), joins with the raw 26.2 bot from `tests/join_test.py`, collects
the chunk batch, walks and looks around the spawn, then:

  * renders a top-down map of every chunk the bot received (one pixel per
    block, shaded by height) to a PNG;
  * censuses the surface block and biome the bot actually stands on/under;
  * if given a `tools/gen_worldgen_dump.py` output directory, compares every
    overlapping chunk block-for-block and biome-cell-for-biome-cell against the
    real 26.2 server's post-carver dumps for the same seed (a directory with
    only post-surface dumps is compared at that stage and says so).

Usage:
    bot_survey.py /path/to/plusmc 1 /tmp/vanilla-seed1 out.png [view-distance]

The comparison is the same boundary as `docs/WORLDGEN_PARITY.md`: vanilla's
post-carver chunk, not its final decorated one (no ores/trees/structures).
"""
import gzip
import json
import pathlib
import re
import socket
import struct
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tests"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import join_test as JT  # noqa: E402
import chunk_decode as CD  # noqa: E402

from PIL import Image, ImageDraw  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parent.parent
HEIGHT = 384
SEED = 0
VIEW_DISTANCE = 3


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def block_table() -> dict:
    text = (REPO / "src/world/block_states.h").read_text()
    entries = re.findall(r'\{"([^"]+)", "([^"]*)", (\d+)\}', text)
    assert len(entries) == 32366, len(entries)
    return {int(sid): name if not props else f"{name}[{props}]"
            for name, props, sid in entries}


def biome_registry() -> list:
    best = None
    root = REPO / "data/vanilla"
    for version_json in root.glob("*/version.json"):
        try:
            dv = json.loads(version_json.read_text())["world_version"]
        except Exception:
            continue
        if best is None or dv > best[0]:
            best = (dv, version_json.parent)
    listing = best[1] / "clientconfig/registries/worldgen_biome.txt"
    return [line for line in listing.read_text().splitlines() if line]


def parse_state_codes(text: str) -> dict:
    """The dump's own code -> canonical state name.  Commas inside `[...]`
    properties do not start a new entry, so split only on `,<digits>=`."""
    parts = re.split(r",(?=\d+=)", text)
    out = {}
    for part in parts:
        code, name = part.split("=", 1)
        out[int(code)] = name
    return out


def load_stage_dumps(directory: pathlib.Path) -> tuple[str, dict]:
    """The dumps to compare against, at the stage the server serves.

    The served column is vanilla's post-carver one, so `carved_meta.tsv` (the
    newer dump) is preferred; a directory carrying only `surface_meta.tsv` is
    an older dump of one stage earlier and is compared as such, with the stage
    named, rather than silently skipped.
    """
    for stage in ("carved", "surface"):
        meta = directory / f"{stage}_meta.tsv"
        if not meta.is_file():
            continue
        dumps = {}
        for line in meta.read_text().splitlines():
            if not line or line.startswith("#"):
                continue
            f = line.split("\t")
            cx, cz = int(f[1]), int(f[2])
            with gzip.open(directory / f"{stage}_seed{f[0]}_c{cx}_{cz}.bin.gz", "rb") as fh:
                dumps[(cx, cz)] = (fh.read(), parse_state_codes(f[8]))
        return stage, dumps
    return "none", {}


def load_biome_dump(directory: pathlib.Path, cx: int, cz: int, seed: int) -> dict:
    path = directory / f"biomes_seed{seed}_c{cx}_{cz}.tsv"
    if not path.is_file():
        return {}
    out = {}
    for line in path.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        f = line.split("\t")
        out[(int(f[3]), int(f[4]), int(f[5]))] = f[12]
    return out


def decode_cells(section: dict) -> list:
    bits = section["states_bits"]
    if bits == 0:
        return [section["states"][0]] * 4096
    per, mask = 64 // bits, (1 << bits) - 1
    raw, entries = section["states_raw"], section["states"]
    return [entries[(raw[i // per] >> ((i % per) * bits)) & mask] for i in range(4096)]


def decode_biomes(section: dict, bits: int) -> list:
    raw, entries = section["biomes_raw"], section["biomes"]
    if bits == 0:
        return [entries[0]] * 64
    per, mask = 64 // bits, (1 << bits) - 1
    return [(entries[(raw[i // per] >> ((i % per) * bits)) & mask] if bits <= 3
             else (raw[i // per] >> ((i % per) * bits)) & mask) for i in range(64)]


def section_names(dec: dict, ids: dict) -> list:
    names = []
    for sec in dec["sections"]:
        vals = decode_cells(sec)
        names.append([ids.get(v, f"<id {v}>") for v in vals])
    return names


COLORS = {
    "minecraft:water[level=0]": (46, 92, 190),
    "minecraft:grass_block[snowy=false]": (92, 154, 62),
    "minecraft:grass_block[snowy=true]": (200, 214, 210),
    "minecraft:dirt": (122, 86, 55),
    "minecraft:coarse_dirt": (110, 80, 52),
    "minecraft:podzol[snowy=false]": (94, 70, 40),
    "minecraft:mycelium[snowy=false]": (130, 112, 122),
    "minecraft:sand": (219, 208, 158),
    "minecraft:red_sand": (188, 104, 52),
    "minecraft:sandstone": (214, 199, 144),
    "minecraft:gravel": (136, 130, 127),
    "minecraft:stone": (124, 124, 124),
    "minecraft:deepslate[axis=y]": (78, 78, 84),
    "minecraft:bedrock": (42, 42, 46),
    "minecraft:snow_block": (242, 246, 252),
    "minecraft:packed_ice": (154, 196, 232),
    "minecraft:ice": (172, 210, 240),
    "minecraft:clay": (160, 168, 176),
    "minecraft:mud": (62, 52, 42),
    "minecraft:terracotta": (152, 94, 68),
    "minecraft:orange_terracotta": (162, 84, 38),
    "minecraft:tuff": (108, 110, 94),
    "minecraft:moss_block": (88, 122, 60),
    "minecraft:lava[level=0]": (207, 92, 20),
}


def color_for(name: str) -> tuple:
    if name in COLORS:
        return COLORS[name]
    base = name.split("[")[0]
    if base in COLORS:
        return COLORS[base]
    h = 0
    for ch in name:
        h = (h * 131 + ord(ch)) & 0xFFFFFFFF
    return (70 + h % 150, 70 + (h >> 8) % 150, 70 + (h >> 16) % 150)


def shade(rgb: tuple, y: int, lo: int, hi: int) -> tuple:
    t = 0.78 + 0.22 * ((y - lo) / max(1, hi - lo))
    return tuple(min(255, int(c * t)) for c in rgb)


def render_map(chunks: dict, names_by_chunk: dict, spawn: tuple, out: pathlib.Path) -> None:
    xs = [cx for cx, _ in chunks]
    zs = [cz for _, cz in chunks]
    min_x, max_x, min_z, max_z = min(xs), max(xs), min(zs), max(zs)
    scale, margin = 4, 26
    width = (max_x - min_x + 1) * 16 * scale
    height = (max_z - min_z + 1) * 16 * scale
    image = Image.new("RGB", (width + 2 * margin, height + 2 * margin), (18, 18, 22))
    draw = ImageDraw.Draw(image)

    tops = []
    for (cx, cz), names in names_by_chunk.items():
        for z in range(16):
            for x in range(16):
                for y in range(319, -65, -1):
                    if names[(y + 64) // 16][((y + 64) % 16 * 16 + z) * 16 + x] != "minecraft:air":
                        tops.append(((cx, cz, x, z), y,
                                     names[(y + 64) // 16][((y + 64) % 16 * 16 + z) * 16 + x]))
                        break
    if not tops:
        raise AssertionError("the bot received no non-air blocks")
    lo = min(t[1] for t in tops)
    hi = max(t[1] for t in tops)
    for (cx, cz, x, z), y, name in tops:
        rgb = shade(color_for(name), y, lo, hi)
        px = margin + ((cx - min_x) * 16 + x) * scale
        py = margin + ((cz - min_z) * 16 + z) * scale
        draw.rectangle([px, py, px + scale - 1, py + scale - 1], fill=rgb)

    sx, sy, sz = spawn
    px = margin + ((sx // 16 - min_x) * 16 + sx % 16) * scale
    py = margin + ((sz // 16 - min_z) * 16 + sz % 16) * scale
    draw.line([px - 8, py, px + 8, py], fill=(255, 40, 40), width=2)
    draw.line([px, py - 8, px, py + 8], fill=(255, 40, 40), width=2)
    draw.text((4, 4), f"seed {SEED}  spawn {sx},{sy},{sz}  view {VIEW_DISTANCE}  "
                      f"y {lo}..{hi}  north=up", fill=(230, 230, 230))
    out.parent.mkdir(parents=True, exist_ok=True)
    image.save(out)
    print(f"view: {image.size[0]}x{image.size[1]} px, {len(tops)} columns, "
          f"{min_x}..{max_x} x {min_z}..{max_z} chunks -> {out}")


def compare(dumps: dict, seed: int, dump_dir: pathlib.Path, decoded: dict, ids: dict,
            biome_ids: list) -> None:
    total_cells = total_biomes = bad_cells = bad_biomes = 0
    compared_chunks = 0
    for (cx, cz), (fixture, code_names) in sorted(dumps.items()):
        if (cx, cz) not in decoded:
            print(f"  chunk ({cx},{cz}): not in the bot's view, skipped")
            continue
        dec = decoded[(cx, cz)]
        chunk_bad_cells = chunk_bad_biomes = 0
        for sy, sec in enumerate(dec["sections"]):
            cells = decode_cells(sec)
            for i, block_id in enumerate(cells):
                ly, rem = divmod(i, 256)
                z, x = divmod(rem, 16)
                y = -64 + sy * 16 + ly
                got = ids.get(block_id, f"<id {block_id}>")
                want = code_names[fixture[(x * 16 + z) * HEIGHT + (y + 64)]]
                total_cells += 1
                if got != want:
                    bad_cells += 1
                    chunk_bad_cells += 1
                    if bad_cells <= 5:
                        print(f"    ({cx},{cz}) ({x},{y},{z}): {got} != {want}")
        biomes = load_biome_dump(dump_dir, cx, cz, seed)
        for sy, sec in enumerate(dec["sections"]):
            cells = decode_biomes(sec, sec["biomes_bits"])
            section_y = -4 + sy
            for y in range(4):
                for z in range(4):
                    for x in range(4):
                        key = (cx * 4 + x, section_y * 4 + y, cz * 4 + z)
                        want_name = biomes.get(key)
                        if want_name is None:
                            continue
                        got_id = cells[(y * 4 + z) * 4 + x]
                        got = biome_ids[got_id] if got_id < len(biome_ids) else f"<id {got_id}>"
                        total_biomes += 1
                        if got != want_name:
                            bad_biomes += 1
                            chunk_bad_biomes += 1
                            if bad_biomes <= 5:
                                print(f"    biome ({cx},{cz}) {key}: {got} != {want_name}")
        compared_chunks += 1
        print(f"  chunk ({cx},{cz}): "
              f"{'MATCH' if chunk_bad_cells == 0 and chunk_bad_biomes == 0 else 'DIFFERS'}")
    print(f"comparison: {compared_chunks} chunks, {total_cells} blocks, {total_biomes} biomes; "
          f"{bad_cells} block and {bad_biomes} biome mismatches")


def main() -> int:
    binary = pathlib.Path(sys.argv[1]).resolve()
    global SEED, VIEW_DISTANCE
    SEED = int(sys.argv[2])
    dump_dir = pathlib.Path(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3] else None
    out_png = pathlib.Path(sys.argv[4]) if len(sys.argv) > 4 else REPO / "premium-server" / "seed1-bot-view.png"
    VIEW_DISTANCE = int(sys.argv[5]) if len(sys.argv) > 5 else 3

    ids = block_table()
    biome_ids = biome_registry()
    port = free_port()
    log_path = pathlib.Path("/tmp") / f"plusmc-bot-survey-{port}.log"
    log = open(log_path, "w")
    proc = subprocess.Popen(
        [str(binary), "--host", "127.0.0.1", "--port", str(port),
         "--worldgen", str(SEED), "--offline-mode", "--view-distance", str(VIEW_DISTANCE),
         "--data", str(REPO / "data" / "vanilla"), "--motd", "bot survey"],
        stdout=log, stderr=subprocess.STDOUT, cwd=str(REPO))
    try:
        spawn = None
        deadline = time.time() + 180
        while time.time() < deadline:
            text = log_path.read_text() if log_path.exists() else ""
            m = re.search(r"worldgen: seed -?\d+; spawn (-?\d+),(-?\d+),(-?\d+)", text)
            if m:
                spawn = tuple(int(v) for v in m.groups())
            if f"listening on 127.0.0.1:{port}" in text:
                break
            if proc.poll() is not None:
                raise AssertionError(f"server exited: {text}")
            time.sleep(0.2)
        assert spawn is not None, log_path.read_text()
        print(f"bot: joining seed {SEED} at {spawn} on 127.0.0.1:{port} (offline twin)")

        client = JT.Client("SurveyBot", port)
        join_body, flags, comp = client.join()
        assert flags["brand"] and flags["registry"] >= 20, flags
        assert comp, "streamed chunks must use compression"

        chunks = {}
        expected = (2 * VIEW_DISTANCE + 1) ** 2
        deadline = time.time() + 180
        while time.time() < deadline and len(chunks) < expected:
            pid, body = client.read_pkt(timeout=10)
            if pid == 0x2D:
                cx, cz = struct.unpack(">ii", body[:8])
                chunks[(cx, cz)] = body
            elif pid == 0x0B:
                if len(chunks) >= expected:
                    break
            elif pid == 0x2C:
                JT.unpack_keepalive(client, body)
        print(f"bot: received {len(chunks)}/{expected} chunks (view distance {VIEW_DISTANCE})")
        assert len(chunks) >= expected, sorted(chunks)

        # Look around and take a few steps on the spot, like a joining player.
        sx, sy, sz = spawn
        for yaw in (0.0, 90.0, 180.0, 270.0):
            client.send_pkt(0x20, struct.pack(">ff", yaw, 0.0) + b"\x01")
            time.sleep(0.05)
        for step in (0.4, 0.8, 1.2):
            client.send_pkt(0x1F, struct.pack(">ddd", sx + step, sy, sz + 0.3)
                            + struct.pack(">ff", 45.0, 10.0) + b"\x01")
            time.sleep(0.1)
        print("bot: walked/looked at spawn without a disconnect")

        decoded = {key: CD.decode(body) for key, body in chunks.items()}
        names_by_chunk = {key: section_names(dec, ids) for key, dec in decoded.items()}

        surface, biomes = {}, {}
        for (cx, cz), names in names_by_chunk.items():
            for z in range(16):
                for x in range(16):
                    for y in range(319, -65, -1):
                        name = names[(y + 64) // 16][((y + 64) % 16 * 16 + z) * 16 + x]
                        if name != "minecraft:air":
                            surface[name] = surface.get(name, 0) + 1
                            break
            dec = decoded[(cx, cz)]
            sec = dec["sections"][8]  # y 64..79, the surface band at sea level
            cells = decode_biomes(sec, sec["biomes_bits"])
            for cell in cells:
                name = biome_ids[cell] if cell < len(biome_ids) else f"<id {cell}>"
                biomes[name] = biomes.get(name, 0) + 1

        print("bot sees surface blocks:", ", ".join(
            f"{k.split(':')[-1].split('[')[0]} {v}" for k, v in
            sorted(surface.items(), key=lambda kv: -kv[1])[:8]))
        print("bot sees biomes (surface band):", ", ".join(
            f"{k.split(':')[-1]} {v}" for k, v in
            sorted(biomes.items(), key=lambda kv: -kv[1])[:8]))
        sp_x, sp_z = sx % 16, sz % 16
        under_y = sy - 1
        sp_sec = (under_y + 64) // 16
        sp_name = names_by_chunk[(sx // 16, sz // 16)][sp_sec][
            ((under_y + 64) % 16 * 16 + sp_z) * 16 + sp_x]
        print(f"spawn column: feet at y={sy}, block under feet={sp_name}")

        render_map(chunks, names_by_chunk, spawn, out_png)

        if dump_dir is not None and dump_dir.is_dir():
            stage, dumps = load_stage_dumps(dump_dir)
            print(f"comparing against vanilla 26.2 post-{stage} dumps in {dump_dir}:")
            if stage == "surface":
                print("  note: this dump is one stage earlier than the served "
                      "column (no carvers); differences are expected")
            compare(dumps, SEED, dump_dir, decoded, ids, biome_ids)
        else:
            print("no vanilla dump directory given; map and census only")

        client.close()
        return 0
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
