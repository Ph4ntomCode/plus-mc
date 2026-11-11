#!/usr/bin/env python3
"""Phase-4 milestone-K integration test: --worldgen streams real overworld
terrain to a joining client.

The server serves the post-carver overworld column (noise, biomes, the surface
rules and `applyCarvers` — the stage `NoiseBasedChunkGenerator.applyCarvers`
leaves behind); a raw 26.2 client joins and the spawn chunk batch is decoded
with the independent Python decoder (tools/chunk_decode.py). Assertions:

  1. login + configuration proceed as in the other join tests, the batch is
     the advertised view square (--view-distance 1 -> 9 chunks), and Join
     Game's `hashedSeed` is `obfuscateSeed(42)` (the hash, not the seed, which
     is what vanilla advertises and what the client's biome manager samples by)
  2. every body is real terrain (not the 1104-byte void fallback) and parses
  3. chunks (0,0) and (-1,-1) are *cell for cell* the committed vanilla dumps
     (tests/fixtures/worldgen/carved_seed42_*.bin.gz): 16 x 16 x 384 cells
     each, every network block-state id resolved through the server's own
     table (src/world/block_states.h) back to the canonical state name the
     dump recorded — same block, same properties, block for block
  4. their three client heightmaps are the heights the blocks imply, column for
     column (the derived store is current on the wire, not a stale zero array)
  5. every section's 4 x 4 x 4 biome cells are the biome source's own answers
     (tests/fixtures/worldgen/biomes_seed42_*.tsv), cell for cell, with the
     network ids looked up in the same advertised registry the server reads —
     same biome, cell for cell
  6. light is computed for the streamed terrain: sky light fills the open air
     above the ocean, and no block light is emitted (the only emissive state
     the carve stage writes is lava, below the carvers' lava level, and
     neither fixture chunk carries any)
  7. keep-alives flow and SIGTERM -> clean exit 0

The parity boundary is vanilla's post-carver chunk, not its final decorated
one: the server's default world is the decorated one (ore, trees, lakes), and
this test asks the same binary for the carved arm (`PLUSMC_WORLDGEN_STAGE=carved`)
because this port's decoration stream is its own and has no vanilla dump to
compare against. `docs/WORLDGEN_PARITY.md` records that gap; the blocks the
carved arm serves — caves included — are byte-for-byte the game's own.

Usage: worldgen_wire_test.py /path/to/plusmc /path/to/tests/fixtures
"""
import gzip
import hashlib
import json
import os
import pathlib
import re
import struct
import subprocess
import sys
import time

import join_test as JT  # helper client, framing + login/config flow (same dir)
import wire_client  # noqa: E402

FIXTURES = pathlib.Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else \
    pathlib.Path(__file__).resolve().parent / "fixtures"
REPO = FIXTURES.parent.parent
sys.path.insert(0, str(REPO / "tools"))
import chunk_decode as CD  # independent decoder

BIN = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
HOST = "127.0.0.1"
PLAYER = "GenBob"
SEED = "42"
VIEW_DISTANCE = 1
HEIGHT = 384  # the column height; fixture layout below

# The canonical state name -> clientbound network id, read from the table the
# server itself encodes with (src/world/block_states.h, generated from the 26.2
# server jar).  Reading it instead of listing the surface states by hand keeps
# the comparison honest when the surface stage gains a material: a state with no
# entry fails the test rather than being silently skipped.
def block_state_ids() -> dict:
    text = (REPO / "src" / "world" / "block_states.h").read_text()
    entries = re.findall(r'\{"([^"]+)", "([^"]*)", (\d+)\}', text)
    assert len(entries) == 35723, f"block state table parse: {len(entries)} entries"
    table = {}
    for name, properties, state_id in entries:
        key = name if not properties else f"{name}[{properties}]"
        table[key] = int(state_id)
    return table


def read_fixture(name: str) -> bytes:
    with gzip.open(FIXTURES / "worldgen" / name, "rb") as f:
        return f.read()


def fixture_code(data: bytes, x: int, y: int, z: int) -> int:
    """`[(x*16 + z)*HEIGHT + (y - -64)]`, the dump's own layout."""
    return data[(x * 16 + z) * HEIGHT + (y + 64)]


def stage_fixture(stage: str, chunk_x: int, chunk_z: int):
    """The committed dump for one stage and chunk, plus its own code ->
    canonical state name map (`<stage>_meta.tsv` row, code assigned in first
    appearance order so it only decodes its own file)."""
    with open(FIXTURES / "worldgen" / f"{stage}_meta.tsv", encoding="utf-8") as f:
        for line in f:
            if not line or line.startswith("#"):
                continue
            fields = line.rstrip("\n").split("\t")
            if (int(fields[1]), int(fields[2])) != (chunk_x, chunk_z):
                continue
            code_names = {}
            for pair in fields[8].split(","):
                code, name = pair.split("=", 1)
                code_names[int(code)] = name
            data = read_fixture(f"{stage}_seed42_c{chunk_x}_{chunk_z}.bin.gz")
            assert len(data) == 16 * 16 * HEIGHT, len(data)
            return data, code_names
    raise AssertionError(f"no {stage}_meta.tsv row for chunk ({chunk_x},{chunk_z})")


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


def decode_heightmap(longs: list, bits: int) -> list:
    """The 256 stored heightmap entries, `(z*16 + x)` order (SimpleBitStorage
    packing, the same one `Chunk::height_at` reads)."""
    per_long = 64 // bits
    mask = (1 << bits) - 1
    return [(longs[i // per_long] >> ((i % per_long) * bits)) & mask
            for i in range(256)]


def biome_registry(data_root: pathlib.Path) -> dict:
    """The advertised biome registry: name -> network id, read the way the
    server reads its client config (newest `world_version` first, one name per
    non-empty line)."""
    best = None
    for version_json in sorted(data_root.glob("*/version.json")):
        try:
            data_version = json.loads(version_json.read_text()).get("world_version", -1)
        except (OSError, ValueError):
            continue
        if best is None or data_version > best[0]:
            best = (data_version, version_json.parent)
    assert best is not None, "no data/vanilla extraction present; run tools/vanilla-data"
    listing = best[1] / "clientconfig" / "registries" / "worldgen_biome.txt"
    names = [line for line in listing.read_text().splitlines() if line]
    return {name: i for i, name in enumerate(names)}


def load_biome_fixture(name: str) -> dict:
    """(quartX, quartY, quartZ) -> the game's own biome answer, from
    tools/gen_worldgen_dump.py's biome fill dump."""
    table = {}
    with open(FIXTURES / "worldgen" / name, encoding="utf-8") as f:
        for line in f:
            if not line or line.startswith("#"):
                continue
            fields = line.rstrip("\n").split("\t")
            table[(int(fields[3]), int(fields[4]), int(fields[5]))] = fields[12]
    return table


def decode_biome_cells(section: dict) -> list:
    """A section's 64 biome ids in cell order (`(y*4 + z)*4 + x`)."""
    bits = section["biomes_bits"]
    if bits == 0:
        return [section["biomes"][0]] * 64
    per_long = 64 // bits
    mask = (1 << bits) - 1
    raw = section["biomes_raw"]
    entries = section["biomes"]
    out = []
    for i in range(64):
        value = (raw[i // per_long] >> ((i % per_long) * bits)) & mask
        # The biome container lists palettes up to 8 entries; above that the
        # longs carry global ids directly (world/chunk.cpp's encoding).
        out.append(entries[value] if bits <= 3 else value)
    return out


def compare_biomes(dec: dict, biomes: dict, ids: dict, chunk_x: int, chunk_z: int,
                   label: str) -> None:
    mismatch = 0
    for sy, section in enumerate(dec["sections"]):
        cells = decode_biome_cells(section)
        section_y = -4 + sy  # overworld min_section_y
        for y in range(4):
            for z in range(4):
                for x in range(4):
                    want_name = biomes[(chunk_x * 4 + x, section_y * 4 + y, chunk_z * 4 + z)]
                    want = ids[want_name]
                    got = cells[(y * 4 + z) * 4 + x]
                    if got != want:
                        mismatch += 1
                        if mismatch <= 5:
                            print(f"  {label} section {section_y} cell ({x},{y},{z}): "
                                  f"id {got}, want {want} ({want_name})")
    assert mismatch == 0, f"{label}: {mismatch}/1536 biome cells differ"
    print(f"{label}: 1536 biome cells match the vanilla dump")


def compare_cells(dec: dict, fixture: bytes, code_names: dict, ids: dict,
                  label: str) -> None:
    """Every one of the chunk's 98304 cells, as canonical state names.  The
    wire's palette carries network ids; the fixture carries its own codes.  Both
    sides name a state, and the names are what must agree: same seed, same
    block, same properties."""
    id_names = {state_id: name for name, state_id in ids.items()}
    assert len(id_names) == len(ids), "block state table has duplicate ids"
    mismatches = 0
    unknown = 0
    first = []
    for sy, section in enumerate(dec["sections"]):
        cells = decode_cells(section)
        for i, block_id in enumerate(cells):
            got = id_names.get(block_id)
            if got is None:
                unknown += 1
                if len(first) < 5:
                    first.append(f"state {block_id}")
                continue
            local_y, rem = divmod(i, 256)
            z, x = divmod(rem, 16)
            y = -64 + sy * 16 + local_y
            want = code_names[fixture_code(fixture, x, y, z)]
            if got != want:
                mismatches += 1
                if len(first) < 5:
                    first.append(f"({x},{y},{z}) got {got} want {want}")
    assert unknown == 0, f"{label}: unknown block states {first}"
    assert mismatches == 0, f"{label}: {mismatches}/98304 cells differ: {first}"
    print(f"{label}: 98304 blocks match the vanilla dump, state for state")


def compare_heightmaps(dec: dict, fixture: bytes, code_names: dict, label: str) -> None:
    # The encoder's committed order: MOTION_BLOCKING_NO_LEAVES, MOTION_BLOCKING,
    # WORLD_SURFACE (types 5, 4, 1).
    assert [t for t, _ in dec["heightmaps"]] == [5, 4, 1], [t for t, _ in dec["heightmaps"]]
    # 384 cells -> 9 bits per entry (Heightmap(<height+1>)).
    values = {t: decode_heightmap(longs, 9) for t, longs in dec["heightmaps"]}
    # Every non-air state the surface and carve stages write either blocks
    # motion or is a fluid (water, lava), so all three maps agree with each
    # other here and each expected value is the column's top non-air cell + 1.
    checked = 0
    def is_air(x: int, y: int, z: int) -> bool:
        # A code's meaning is per-file (0 is air in the noise dumps, bedrock in
        # the surface and carved ones), so the name, not the code, decides.
        return code_names[fixture_code(fixture, x, y, z)] == "minecraft:air"

    for z in range(16):
        for x in range(16):
            top = max((y for y in range(-64, 320) if not is_air(x, y, z)), default=None)
            want = 0 if top is None else top + 1 + 64  # relative to min_y
            for t in (5, 4, 1):
                got = values[t][z * 16 + x]
                assert got == want, (label, f"heightmap {t}", (x, z), got, want)
                checked += 1
    print(f"{label}: {checked} heightmap entries match the blocks")


def main() -> None:
    data_root = REPO / "data" / "vanilla"
    assert data_root.is_dir(), "no data/vanilla extraction present; run tools/vanilla-data"
    biome_ids = biome_registry(data_root)
    block_ids = block_state_ids()

    # The post-carver dumps: the same stage the server serves.  Comparing
    # against the raw noise dumps would pass only if the surface rules were
    # skipped, so the fixture choice is itself the "not bare noise" assertion;
    # comparing against the *surface* dumps would pass only if the carve stage
    # were skipped, which is why the served boundary moved with the fixtures.
    ocean_fixture, ocean_names = stage_fixture("carved", 0, 0)
    land_fixture, land_names = stage_fixture("carved", -1, -1)
    assert len(ocean_fixture) == len(land_fixture) == 16 * 16 * HEIGHT
    # Both fixture chunks must actually exercise the surface pass, or the
    # comparison below could be satisfied by the noise stage alone, and at least
    # one must have cells the carve rewrote, or it could be satisfied by the
    # surface stage alone.
    assert set(ocean_names.values()) >= {"minecraft:water[level=0]",
                                         "minecraft:dirt"}, ocean_names
    assert "minecraft:grass_block[snowy=false]" in set(land_names.values()), land_names
    surface_ocean, surface_ocean_names = stage_fixture("surface", 0, 0)
    surface_land, surface_land_names = stage_fixture("surface", -1, -1)
    # Each carved dump must differ from its post-surface dump, or this test
    # would silently accept a carve that rewrote nothing (the failure mode
    # `docs/WORLDGEN_PARITY.md` records from the uncommitted tag).  The counts
    # are pinned per chunk so a carve that stops halfway also fails.
    rewritten = {}
    for (chunk_x, chunk_z), carved, carved_names, surface, surface_names in (
            ((0, 0), ocean_fixture, ocean_names, surface_ocean, surface_ocean_names),
            ((-1, -1), land_fixture, land_names, surface_land, surface_land_names)):
        rewritten[(chunk_x, chunk_z)] = sum(
            1 for i, code in enumerate(carved)
            if carved_names[code] != surface_names[surface[i]])
        assert rewritten[(chunk_x, chunk_z)] > 0, \
            f"chunk ({chunk_x},{chunk_z}): carved dump equals the surface dump"
        print(f"carved fixture ({chunk_x},{chunk_z}): "
              f"{rewritten[(chunk_x, chunk_z)]} cells rewritten by the carvers")
    assert rewritten[(0, 0)] == 3060 and rewritten[(-1, -1)] == 962, rewritten
    print(f"fixtures: {sum(rewritten.values())} cells differ between the surface "
          f"and carved dumps")

    env = dict(os.environ)
    # The served world is the decorated one; the only stage this engine can
    # compare against a vanilla dump is the post-carver one (its decoration
    # stream is its own), so this test asks the same binary for that arm.
    env["PLUSMC_WORLDGEN_STAGE"] = "carved"
    env["PLUSMC_KEEPALIVE_MS"] = "1200"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "4000"
    # The survival tick counts packets (no wall-time catch-up), so a drain
    # banks no ticks while the generated chunks stream.
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
    proc, port = wire_client.start_server(
        [str(BIN), "--host", HOST, "--port", "0", "--worldgen", SEED,
         "--offline-mode", "--view-distance", str(VIEW_DISTANCE),
         "--data", str(data_root)],
        JT.wait_for_listen_line, env=env)
    try:
        a = JT.Client(PLAYER, port)
        wire_client.record(a, note="worldgen-wire: generated chunks for a join")
        join_body, flags, comp = a.join()
        eid, dims = JT.decode_join_game(join_body, expect_online=0)
        assert flags["brand"] and flags["registry"] >= 20 and flags["tags"], flags
        assert comp and dims[:1] == ["minecraft:overworld"], dims
        assert eid > 0
        # Join Game's `hashedSeed`: vanilla sends `BiomeManager.obfuscateSeed(
        # level.getSeed())`, not the seed -- SHA-256 of the seed's little-endian
        # bytes, the first eight read back little-endian. Recomputed here rather
        # than imported from the server's own helper, so the field is pinned
        # against the game's formula and not against the code that sends it.
        login = JT.decode_login_body(join_body)
        expected_seed = int.from_bytes(
            hashlib.sha256(struct.pack("<q", int(SEED))).digest()[:8], "little",
            signed=True)
        assert login["seed"] == expected_seed, (login["seed"], expected_seed)
        print(f"join game: hashed seed {login['seed']} == obfuscateSeed({SEED})")

        seen = {"chunks": {}, "batch_size": None, "ka": 0}
        for _ in range(400):
            pid, body = a.read_pkt()
            if pid == 0x2E:  # level chunk with light
                x, z = struct.unpack(">ii", body[:8])
                seen["chunks"][(x, z)] = body
            elif pid == 0x0B:  # chunk batch finished
                n, _ = JT.rd_varint(body)
                seen["batch_size"] = n
                break
            elif pid == 0x2D:  # keep-alive: ack
                JT.unpack_keepalive(a, body)
                seen["ka"] += 1

        radius = VIEW_DISTANCE
        assert seen["batch_size"] == (2 * radius + 1) ** 2, seen["batch_size"]
        assert len(seen["chunks"]) == (2 * radius + 1) ** 2, sorted(seen["chunks"])
        # The spawn search puts the player near the shore around chunk (-1,-1),
        # so both committed fixture chunks are inside the first batch.
        assert (0, 0) in seen["chunks"] and (-1, -1) in seen["chunks"], \
            sorted(seen["chunks"])
        for (x, z), body in seen["chunks"].items():
            assert len(body) > 1104, (x, z, len(body))  # not the void fallback
        print(f"spawn batch: {(2 * radius + 1) ** 2} chunks, all real terrain")

        for (x, z), fixture, code_names, biome_file in (
                ((0, 0), ocean_fixture, ocean_names, "biomes_seed42_c0_0.tsv"),
                ((-1, -1), land_fixture, land_names, "biomes_seed42_c-1_-1.tsv")):
            label = f"chunk ({x},{z})"
            dec = CD.decode(seen["chunks"][(x, z)])
            assert len(dec["sections"]) == 24, len(dec["sections"])
            compare_cells(dec, fixture, code_names, block_ids, label)
            compare_heightmaps(dec, fixture, code_names, label)
            compare_biomes(dec, load_biome_fixture(biome_file), biome_ids, x, z, label)

            # The ocean is open to the sky, so a fully-lit sky array must be
            # present; neither fixture chunk carries lava (the only emissive
            # state the carve stage can write, below its lava level), so no
            # block light is emitted at all (all-zero arrays are omitted by
            # the encoder).
            assert any(arr and all(b == 0xFF for b in arr) for arr in dec["sky"]), \
                f"{label}: no fully-lit sky section"
            for arr in dec["block"]:
                assert not any(arr), f"{label}: unexpected block light"
            print(f"{label}: sky light computed, no block light emitted")

        # Keep-alives flow (server considered us alive through the batch).  The
        # join leaves a burst of setup packets queued, so this is a time-bounded
        # drain rather than a packet-count one: wait out the first ack.
        deadline = time.time() + 10
        while seen["ka"] == 0 and time.time() < deadline:
            pid, body = a.read_pkt()
            if pid == 0x2D:
                JT.unpack_keepalive(a, body)
                seen["ka"] += 1
            else:
                assert pid not in (0x0B, 0x0C, 0x2D), f"late chunk packet 0x{pid:02x}"
        assert seen["ka"] >= 1, "no keep-alive within 10 s"
        print("keep-alive: OK")

        a.close()
        proc.terminate()
        try:
            rc = proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            raise AssertionError("plusmc did not exit after SIGTERM")
        assert rc == 0, f"expected clean exit on SIGTERM, got {rc}"
        print("graceful shutdown: OK")
    except BaseException:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        raise
    print("PASS")


if __name__ == "__main__":
    main()
