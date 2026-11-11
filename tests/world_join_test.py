#!/usr/bin/env python3
"""Phase-2 milestone-A integration test: the server streams real Anvil chunks.

Builds a minimal 26.2-layout world directory around the committed fixture
region (tests/fixtures/r.0.0.mca, holding chunk (0,0) of the seed-42 flat
world), starts plusmc with --world pointing at it, and joins as a raw 26.2
client. Asserts:

  1. login + configuration proceed exactly as in the void-world join test
  2. the spawn chunk batch contains the real chunk (0,0): its body is
     byte-identical to the chunk body a live vanilla 26.2 server sent for
     that same chunk (tests/fixtures/chunk000_0_0_body.hex), and the
     independent Python decoder reads terrain (24 sections, Y -4 carries
     non_air=1024 with palette [bedrock,dirt,grass,air] = [85,10,9,0],
     light masks sky {1,2} / empty sky {0} / empty block {0,1,2})
  3. the 8 surrounding chunks are absent from disk and fall back to the
     void emitter (1104-byte all-air bodies), and keep-alives flow
  4. SIGTERM -> clean exit 0

Usage: world_join_test.py /path/to/plusmc /path/to/tests/fixtures
"""
import os
import pathlib
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
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
PLAYER = "WorldBob"

# Spawn chunk coordinates for the default spawn (8.5, 100, 8.5): (0, 0).
# Our encoder emits heightmaps in the committed capture order (5,4,1), so the
# byte comparison below is strict.
def main() -> None:
    data_root = REPO / "data" / "vanilla"
    assert data_root.is_dir(), "no data/vanilla extraction present; run tools/vanilla-data"
    reference_hex = (FIXTURES / "chunk000_0_0_body.hex").read_text().strip()
    assert len(reference_hex) // 2 == 7259

    with tempfile.TemporaryDirectory(prefix="plusmc-world-") as td:
        world = pathlib.Path(td) / "world"
        region_dir = world / "dimensions" / "minecraft" / "overworld" / "region"
        region_dir.mkdir(parents=True)
        shutil.copyfile(FIXTURES / "r.0.0.mca", region_dir / "r.0.0.mca")

        env = dict(os.environ)
        env["PLUSMC_KEEPALIVE_MS"] = "300"
        env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "2000"
        # The survival tick counts packets (one tick each, no wall-time
        # catch-up), so a drain banks no ticks. What this file asserts is the
        # chunk batch a given view distance produces — states, not durations.
        env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
        # A small view distance keeps the streaming assertions tractable.
        proc, port = wire_client.start_server(
            [str(BIN), "--host", HOST, "--port", "0", "--world", str(world),
             "--offline-mode", "--view-distance", "2", "--data", str(data_root)],
            lambda p: JT.wait_for_listen_line(p, time.time() + 15), env=env)
        try:
            a = JT.Client(PLAYER, port)
            wire_client.record(a, note="world-join: the chunk batch a view distance produces")
            join_body, flags, comp = a.join()
            eid, dims = JT.decode_join_game(join_body, expect_online=0)
            assert flags["brand"] and flags["registry"] >= 20 and flags["tags"], flags
            assert comp and dims[:1] == ["minecraft:overworld"], dims
            assert eid > 0

            seen = {"chunks": {}, "batch_size": None, "ka": 0, "info": [], "spawns": [],
                    "game_events": [], "own_info_seq": None, "own_hat": None,
                    "first_chunk_seq": None}
            seq = 0
            for _ in range(400):
                pid, body = a.read_pkt()
                seq += 1
                if pid == 0x2E:  # level chunk with light
                    if seen["first_chunk_seq"] is None:
                        seen["first_chunk_seq"] = seq
                    x, z = struct.unpack(">ii", body[:8])
                    seen["chunks"][(x, z)] = body
                elif pid == 0x0B:  # chunk batch finished
                    n, _ = JT.rd_varint(body)
                    seen["batch_size"] = n
                    break
                elif pid == 0x27:  # game event: [u8 reason][f32 param]
                    assert len(body) == 5, ("game event body", len(body), body.hex())
                    reason = body[0]
                    param = struct.unpack(">f", body[1:5])[0]
                    seen["game_events"].append((reason, param))
                elif pid == 0x2D:  # keep-alive: ack
                    JT.unpack_keepalive(a, body)
                    seen["ka"] += 1
                elif pid == 0x47:
                    for entry in JT.decode_player_info_add_entries(body):
                        seen["info"].append(entry["name"])
                        if entry["name"] == PLAYER:
                            seen["own_info_seq"] = seq
                            seen["own_hat"] = entry["hat"]
                elif pid == 0x01:
                    seen["spawns"].append(JT.decode_spawn_entity(body))
            # The spawn burst covers the whole advertised view distance
            # (--view-distance 2 -> 5x5), because the client cannot move while
            # the loading screen is up and so cannot trigger on-demand
            # streaming to fill the gap.
            # The client's LevelLoadTracker starts in WaitingForServer and only
            # leaves it when ClientPacketListener.handleGameEvent sees
            # LEVEL_CHUNKS_LOAD_START; WaitingForServer has no tick transition,
            # so a join without this packet parks the client on the "Loading
            # terrain" screen until its 30 s timeout drops it. It must land
            # before the chunk batch (we break on batch-finished, so everything
            # recorded here did).
            assert (13, 0.0) in seen["game_events"], (
                "no LEVEL_CHUNKS_LOAD_START game event: the client can never "
                f"leave the loading screen; saw {seen['game_events']}")
            assert 3 in [r for r, _ in seen["game_events"]], seen["game_events"]  # change_game_mode
            print("game events: level_chunks_load_start + change_game_mode before the batch")

            # The local player's tab entry must arrive before the first chunk.
            # AbstractClientPlayer.getPlayerInfo() caches its one-and-only
            # PlayerInfo lookup in a field, so an entry that lands after the
            # world starts rendering pins that player to the default skin — and
            # no default skin carries a cape or the textures property.
            assert seen["own_info_seq"] is not None, (
                "no tab entry for the local player before the chunk batch")
            assert seen["first_chunk_seq"] is not None
            assert seen["own_info_seq"] < seen["first_chunk_seq"], (
                f"local tab entry #{seen['own_info_seq']} must precede the first "
                f"chunk #{seen['first_chunk_seq']}: the client caches a null "
                "PlayerInfo, so a late entry means the default skin and no cape")
            assert seen["own_hat"] == 1, (
                f"hat/outer-skin-layer flag is {seen['own_hat']}, expected 1 "
                "(vanilla writes isModelPartShown(PlayerModelPart.HAT); 0 hides "
                "every player's second skin layer)")
            print("tab entry precedes the chunk batch; hat layer enabled")

            assert seen["batch_size"] == 25, seen["batch_size"]
            assert len(seen["chunks"]) == 25, sorted(seen["chunks"])
            assert (0, 0) in seen["chunks"]
            assert min(x for x, _ in seen["chunks"]) == -2
            assert max(x for x, _ in seen["chunks"]) == 2

            # The real chunk must be byte-identical to the 26.3 encoding of the committed vanilla capture.
            body00 = seen["chunks"][(0, 0)]
            assert body00.hex() == reference_hex, (
                f"chunk (0,0) differs from the vanilla capture: "
                f"{len(body00)} vs {len(reference_hex)//2} bytes")
            print("world chunk (0,0): byte-identical to the 26.3 vanilla fixture encoding")

            # Independent decoder cross-check on the streamed body.
            dec = CD.decode(body00)
            assert len(dec["sections"]) == 24
            s0 = dec["sections"][0]
            assert s0["non_air"] == 1024, s0
            assert s0["states"] == [88, 10, 9, 0], s0  # bedrock/dirt/grass/air
            assert s0["biomes"] == [41], s0  # plains
            assert [t for t, _ in dec["heightmaps"]] == [5, 4, 1]
            assert dec["masks"] == {"sky": [6], "block": [], "empty_sky": [1],
                                    "empty_block": [7]}, dec["masks"]
            assert len(dec["sky"]) == 2 and len(dec["block"]) == 0
            print("decoder: terrain section, palette, light masks all correct")

            # The other 24 spawn chunks are absent from the fixture world and
            # must fall back to the byte-validated void body (1104 bytes).
            for (x, z), body in seen["chunks"].items():
                if (x, z) != (0, 0):
                    assert len(body) == 1104, (x, z, len(body))
            print("void fallback chunks around spawn: OK")

            # Keep-alives flow (server considered us alive through the batch).
            for _ in range(60):
                pid, body = a.read_pkt()
                if pid == 0x2D:
                    JT.unpack_keepalive(a, body)
                    seen["ka"] += 1
                    break
                assert pid not in (0x0B, 0x0C, 0x2E), f"late chunk packet 0x{pid:02x}"
            assert seen["ka"] >= 1, "no keep-alive observed"
            print("keep-alive: OK")

            # Move east across two chunk boundaries (8.5 -> 33.5 is chunk (0,0)
            # -> (2,0)). The server must stream the newly-in-range chunks as one
            # batch: radius 2 around (2,0) = 25 coords, minus the 15 already sent
            # in the spawn batch that are still in range = 10. Chunks beyond the
            # single-chunk fixture fall back to the void body (1104 bytes).
            mv = (struct.pack(">ddd", 33.5, 100.0, 8.5) +
                  struct.pack(">ff", 0.0, 0.0) + b"\x01")
            a.send_pkt(0x1F, mv)  # serverbound position_look
            streamed = {}
            recentered = None
            for _ in range(600):
                pid, body = a.read_pkt()
                if pid == 0x2E:
                    x, z = struct.unpack(">ii", body[:8])
                    streamed[(x, z)] = body
                elif pid == 0x60:  # set_chunk_cache_center
                    cx, i = JT.rd_varint(body)
                    cz, _ = JT.rd_varint(body, i)
                    recentered = (cx, cz)
                elif pid == 0x0B:  # chunk batch finished
                    n, _ = JT.rd_varint(body)
                    break
                elif pid == 0x2D:
                    JT.unpack_keepalive(a, body)
            # The client's chunk cache is centered by this packet and silently
            # drops anything outside center +/- radius, so a walking player that
            # never gets it stops seeing newly streamed chunks.
            assert recentered == (2, 0), (
                f"expected set_chunk_cache_center (2,0) on the chunk crossing, got {recentered}")
            assert len(streamed) == 10, sorted(streamed)
            assert (0, 0) not in streamed, "chunk (0,0) must not be re-sent"
            assert (3, 0) in streamed and (4, 2) in streamed and (4, -2) in streamed
            for (x, z), body in streamed.items():
                assert 0 <= x <= 4 and -2 <= z <= 2, (x, z)
                assert len(body) == 1104, (x, z, len(body))
            print("chunk streaming on movement: OK (10 chunks, correct radius, "
                  "no resends, void fallback)")

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
