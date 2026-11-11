#!/usr/bin/env python3
"""Custom terrain over the real wire protocol: a world this server did not make.

Every world this tree could serve before was one of three things — the void
(`MemoryWorld`'s fabricated air columns), the flat world, or terrain this server
generated itself (`--worldgen <seed>`). None of them is the path a host takes:
point `--world` at a directory someone else made, load the columns off disk, live
in them. That path has its own cost (every column comes off disk and is relit on
the way out, because a hand-authored world carries no light) and its own failure
modes (a state the encoder cannot resolve voids the chunk on the wire; a
heightmap the file does not carry is a client that cannot predict collisions).

What this test does:

1. `plusmc-gen-custom-world` authors an NxN-chunk world into a temp directory —
   a stepped snow-capped pyramid with a cobblestone-and-glass house on its
   summit, a lake with a sand beach, a sandstone mesa, a carved cave tunnel, a
   shaft dropping into a lava pool, an iron vein, a gravel pocket and oak trees —
   and writes the *design* it authored as `custom_world_features.json`.
2. The server is booted on that directory with `--world <dir> --no-flat-world`.
3. A real client joins, is teleported to the middle of the authored region, and
   every authored chunk is decoded with the independent Python decoder
   (`tools/chunk_decode.py`), which knows nothing about this server's C++.
4. Every feature in the manifest is compared, at its own cell, against the block
   state the client received — the design against the bytes, per block.
5. Columns *outside* the authored region must be the void fallback: a custom
   world must not quietly serve generated terrain where its own file stops.
6. The received heightmap of the apex column has to agree with the blocks
   beneath it, which is the round trip "file → parse → store → copy → wire".
7. Sky light must be present on the received chunks: the file has none, so this
   is the server's relight running for a world it did not author.
8. Finally it prints the server's own RAM/CPU report for the run (RSS, work per
   tick, TPS, busy fraction) — "what does a custom world cost to serve" is the
   other half of the question, and `--metrics-file` is where the server already
   answers it (`core/metrics.h`).

Usage: tests/custom_world_test.py /path/to/plusmc /path/to/plusmc-gen-custom-world
"""
import hashlib
import json
import os
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import time

import join_test as JT  # helper client, framing + login/config flow (same dir)
import wire_client  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
import chunk_decode as CD  # independent decoder

BIN = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
GEN = pathlib.Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
HOST = "127.0.0.1"
PLAYER = "CustomBot"
CHUNKS = 7            # the authored world is CHUNKS x CHUNKS columns (112x112 blocks)
VIEW_DISTANCE = 4     # covers 0..(CHUNKS-1) from the centre in one batch
SPAWN = (CHUNKS * 8, 100, CHUNKS * 8)  # the middle of the authored region, in blocks

SB_CHAT_COMMAND = 0x07  # Serverbound Chat Command (26.2)


def step(msg: str) -> None:
    print(msg, flush=True)


def send_command(bot, command: str) -> None:
    bot.send_pkt(SB_CHAT_COMMAND, varint(len(command)) + command.encode())


def varint(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def obfuscate_seed(seed: int) -> int:
    """`BiomeManager.obfuscateSeed` -- SHA-256 of the seed's little-endian bytes,
    the first eight read back little-endian. Recomputed here rather than imported
    from the server's helper, so the wire field is pinned against the game's own
    formula and not against the code that sends it."""
    return int.from_bytes(hashlib.sha256(struct.pack("<q", int(seed))).digest()[:8],
                          "little", signed=True)


def block_state_table() -> dict:
    """Network block-state id -> (name, properties), parsed out of the server's
    own table (src/world/block_states.h).

    The table is the authority the encoder resolves states through, so reading it
    here is the opposite direction of the check: the test does not carry a second
    copy of any state's id, it reads the id back and asks what the server thinks
    that id is. One regex over the compiled-in table, no data files, no registry
    guesswork.
    """
    pattern = re.compile(r'^\s*\{"([^"]+)",\s*"([^"]*)",\s*(\d+)\},')
    table = {}
    for line in (REPO / "src" / "world" / "block_states.h").read_text().splitlines():
        m = pattern.match(line)
        if m:
            table[int(m.group(3))] = (m.group(1), m.group(2))
    assert len(table) > 10000, f"block_states.h parsed to only {len(table)} states"
    return table


def decode_cells(section: dict) -> list:
    """A section's 4096 block ids in cell order (`(y*16 + z)*16 + x`)."""
    bits = section["states_bits"]
    if bits == 0:
        return [section["states"][0]] * 4096
    per_long = 64 // bits
    mask = (1 << bits) - 1
    raw = section["states_raw"]
    entries = section["states"]
    out = []
    for i in range(4096):
        value = (raw[i // per_long] >> ((i % per_long) * bits)) & mask
        out.append(entries[value] if bits <= 8 else value)
    return out


def decode_heightmap(longs: list, bits: int = 9) -> list:
    per_long = 64 // bits
    mask = (1 << bits) - 1
    return [(longs[i // per_long] >> ((i % per_long) * bits)) & mask for i in range(256)]


def cell_of(dec: dict, x: int, y: int, z: int) -> int:
    """One cell of a decoded chunk, by world block coordinates."""
    section = dec["sections"][(y + 64) // 16]
    cells = decode_cells(section)
    return cells[((y & 15) * 16 + (z & 15)) * 16 + (x & 15)]


def main() -> int:
    if BIN is None or GEN is None:
        print(f"usage: {sys.argv[0]} /path/to/plusmc /path/to/plusmc-gen-custom-world",
              file=sys.stderr)
        return 2
    assert BIN.exists(), f"no server binary at {BIN}"
    assert GEN.exists(), f"no world authoring tool at {GEN}"
    data_root = REPO / "data" / "vanilla"
    assert data_root.is_dir(), "no data/vanilla extraction present; run tools/vanilla-data"

    work = pathlib.Path(tempfile.mkdtemp(prefix="plusmc-custom-"))
    world_dir = work / "world"
    metrics_path = work / "metrics.jsonl"
    proc = None
    try:
        # --- 1. author the custom world ------------------------------------
        gen = subprocess.run([str(GEN), "--out", str(world_dir), "--chunks", str(CHUNKS)],
                             capture_output=True, text=True, timeout=120)
        assert gen.returncode == 0, f"gen_custom_world failed: {gen.stdout}{gen.stderr}"
        for line in gen.stdout.strip().splitlines():
            step(f"  author: {line.strip()}")
        manifest = json.loads((world_dir / "custom_world_features.json").read_text())
        features = manifest["features"]
        assert manifest["chunks"] == CHUNKS and len(features) >= 20, manifest
        assert manifest["seed"] == 20261018, manifest
        region = world_dir / "dimensions" / "minecraft" / "overworld" / "region" / manifest["region"]
        assert region.exists(), f"no region file at {region}"
        step(f"authored world: {CHUNKS}x{CHUNKS} columns, {len(features)} design features, "
             f"{region.stat().st_size / 1024:.1f} KiB of region file")

        # --- 2. serve it the way a host would ------------------------------
        env = dict(os.environ)
        env["PLUSMC_MOB_SPAWNING"] = "0"
        env["PLUSMC_KEEPALIVE_MS"] = "1200"
        env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "6000"
        env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
        proc, port = wire_client.start_server(
            [str(BIN), "--host", HOST, "--port", "0", "--offline-mode",
             "--data", str(data_root), "--world", str(world_dir), "--no-flat-world",
             "--gamemode", "1", "--view-distance", str(VIEW_DISTANCE),
             "--metrics-interval", "1", "--metrics-file", str(metrics_path),
             "--motd", "custom world test"],
            JT.wait_for_listen_line, env=env)
        step(f"serving {world_dir} (custom terrain, no light stored in the file)")

        # --- 3. join and pull the authored chunks --------------------------
        a = JT.Client(PLAYER, port)
        wire_client.record(a, note="custom-world: a file-backed world the server did not make")
        join_body, flags, comp = a.join()
        eid, dims = JT.decode_join_game(join_body, expect_online=0)
        assert flags["brand"] and flags["registry"] >= 20 and flags["tags"], flags
        assert comp and dims[:1] == ["minecraft:overworld"], dims
        assert eid > 0

        # Join Game's `hashedSeed` is `obfuscateSeed(level.getSeed())`. This world
        # came off disk with a seed in its level.dat, so the field is read from the
        # file, not from a generator this server ran (that is the `--worldgen` path).
        login = JT.decode_login_body(join_body)
        expected = obfuscate_seed(manifest["seed"])
        assert login["seed"] == expected, (
            f"join game hashed seed {login['seed']}, level.dat seed {manifest['seed']} "
            f"hashes to {expected}")
        step(f"join game: hashed seed {login['seed']} == obfuscateSeed({manifest['seed']})")

        seen = {"chunks": {}}
        deadline = time.time() + 20
        while time.time() < deadline and len(seen["chunks"]) < (2 * VIEW_DISTANCE + 1) ** 2:
            pid, body = a.read_pkt()
            if pid == 0x2E:
                x, z = struct.unpack(">ii", body[:8])
                seen["chunks"][(x, z)] = body
            elif pid == 0x2D:
                JT.unpack_keepalive(a, body)
        # The spawn point is the world's own (8.5, 100, 8.5) -> chunk (0,0); walk
        # the player to the middle of the authored region so the batch that
        # arrives is centred on the whole custom build rather than its corner.
        send_command(a, f"tp {SPAWN[0]}.5 {SPAWN[1]} {SPAWN[2]}.5")
        deadline = time.time() + 45
        authored = {(x, z) for x in range(CHUNKS) for z in range(CHUNKS)}
        while time.time() < deadline and not authored.issubset(seen["chunks"]):
            pid, body = a.read_pkt()
            if pid == 0x2E:
                x, z = struct.unpack(">ii", body[:8])
                seen["chunks"][(x, z)] = body
            elif pid == 0x2D:
                JT.unpack_keepalive(a, body)
        missing = sorted(authored - set(seen["chunks"]))
        assert not missing, f"authored chunks never arrived: {missing}"
        step(f"received all {len(authored)} authored chunks "
             f"({len(seen['chunks'])} chunk positions in all)")

        # --- 4. the design, block by block ---------------------------------
        table = block_state_table()
        decoded = {k: CD.decode(v) for k, v in seen["chunks"].items()}
        verified = 0
        for f in features:
            cx, cz = f["chunk"]
            dec = decoded[(cx, cz)]
            got_id = cell_of(dec, f["x"], f["y"], f["z"])
            name, props = table.get(got_id, (f"<unknown id {got_id}>", ""))
            want = (f["block"], f["properties"])
            assert (name, props) == want, (
                f"{f['name']} at ({f['x']},{f['y']},{f['z']}): the client received "
                f"{name}[{props}], the design authored {want[0]}[{want[1]}]")
            verified += 1
            step(f"  OK {f['name']:<16} ({f['x']:>3},{f['y']:>4},{f['z']:>3}) = {name}"
                 f"{'[' + props + ']' if props else ''}")
        step(f"custom terrain: {verified}/{len(features)} design features match the wire")

        # --- 5. outside the authored region is the void, not generated world -
        outside = [k for k in seen["chunks"] if k[0] < 0 or k[1] < 0 or k[0] >= CHUNKS
                   or k[1] >= CHUNKS]
        assert outside, "no column outside the authored region arrived to check"
        for key in outside[:4]:
            dec = decoded[key]
            for section in dec["sections"]:
                assert all(cell == 0 for cell in decode_cells(section)), \
                    f"chunk {key} is outside the authored region but is not all-air void"
        step(f"chunks outside the region ({len(outside)} of them) are the void fallback")

        # --- 6. the heightmap agrees with the blocks beneath it ------------
        apex = next(f for f in features if f["name"] == "pyramid apex")
        dec = decoded[tuple(apex["chunk"])]
        # The wire carries heightmap types as their numeric ids (world/chunk.h's
        # `HeightmapType`: 1 WORLD_SURFACE, 3 OCEAN_FLOOR, 4 MOTION_BLOCKING,
        # 5 MOTION_BLOCKING_NO_LEAVES), and each entry is `Heightmap(<height+1>)`
        # relative to the world's min_y, i.e. `y_top + 1 - (-64)`.
        surface = next((longs for t, longs in dec["heightmaps"] if t == 1), None)
        assert surface is not None, (
            f"no WORLD_SURFACE heightmap on the apex chunk: {[t for t, _ in dec['heightmaps']]}")
        stored = decode_heightmap(surface)[(apex["z"] & 15) * 16 + (apex["x"] & 15)]
        column = [y for y in range(apex["y"] - 4, apex["y"] + 12)
                  if cell_of(dec, apex["x"], y, apex["z"]) != 0]
        top = max(column)
        assert stored == top + 1 + 64, \
            f"apex column heightmap says {stored}, the blocks say {top + 1 + 64}"
        step(f"apex heightmap: {stored} = the blocks' own top ({top}) + 1 + 64")

        # --- 7. the file had no light: the server relit it -----------------
        centre = decoded[(CHUNKS // 2, CHUNKS // 2)]
        lit = [arr for arr in centre["sky"] if arr and all(b == 0xFF for b in arr)]
        assert lit, "no fully-lit sky section on the apex chunk: relight did not run"
        step(f"sky light: {len(lit)} fully-lit sections on the apex chunk "
             f"(the world file carries none, so the server relit it)")

        # --- 8. what serving it cost ---------------------------------------
        a.close()
        proc.terminate()
        rc = proc.wait(timeout=10)
        proc = None
        assert rc == 0, f"expected clean exit on SIGTERM, got {rc}"
        lines = [json.loads(line) for line in metrics_path.read_text().splitlines() if line.strip()]
        assert lines, "no metrics were written; the RAM/CPU report has nothing to read"
        last = lines[-1]
        step("")
        step("custom world under load, from the server's own metrics line:")
        step(f"  RSS            {last['rss_bytes'] / (1024 * 1024):8.1f} MiB")
        step(f"  work per tick  {last['work_avg_ms']:8.3f} ms avg, "
             f"{last['work_peak_ms']:.3f} ms peak")
        step(f"  TPS            {last['tps']:8.2f}   busy {last['busy_pct']:.1f}%   "
             f"players {last['players']}")
        step(f"  store gauge    world_chunks={last.get('world_chunks')} "
             f"world_bytes={last.get('world_bytes')}  (the resident-column gauge is "
             f"MemoryWorld's; a file-backed world reports 0)")
        assert last["rss_bytes"] > 0 and last["tps"] > 0, last
        step("")
        step("PASS: custom terrain served block-for-block")
        return 0
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    sys.exit(main())
