#!/usr/bin/env python3
"""Integration test: the arm-swing relay over the real wire protocol.

This server never relayed an arm swing. Its own client animates its arm when it
attacks or uses an item, so a single player sees a working game; a *second*
player saw a statue. Vanilla's `LivingEntity.swing(hand)` broadcasts
`ClientboundAnimatePacket` (0 for the main hand, 3 for the off hand) to the
swinger's tracking players through `ChunkMap.broadcast`, which deliberately
excludes the swinger (its own client already played the animation).

Two clients on the committed flat capture, natural spawning off and the clock
packet-driven. A and B stand two blocks apart, in range of each other's player
trackers:

  * both report the same stance with real Move Player Position packets, and B
    receives A's Add Entity — the pairing burst a tracked player gets;
  * A's serverbound `swing` (main hand) reaches B as `animate` with A's entity
    id and animation 0; A itself receives no `animate` (the exclusion above);
  * the off hand reaches B as animation 3;
  * a hand byte the protocol's `InteractionHand` decoder cannot produce (2)
    sends nothing at all, which is the malformed-packet half;
  * B walks 80 blocks east and gets exactly one Remove Entities for A; the next
    swing reaches nobody, because the audience is the tracked set and not
    "every connection in the world".

Run: arm_swing_relay_test.py /path/to/plusmc /path/to/tests/fixtures
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

BIN = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
HOST = "127.0.0.1"
PLAYER_A = "SwingA"
PLAYER_B = "SwingB"

# Clientbound packet ids.
CB_ADD_ENTITY = 0x01
CB_ANIMATE = 0x02
CB_KEEP_ALIVE = 0x2D
CB_REMOVE_ENTITIES = 0x4E

# Serverbound packet ids. 26.3 removed the old `swing` row (0x3F) and moved
# the arm animation to `punch` (0x2E), which the server dispatches to its
# `handle_arm_animation`.
SB_SWING = 0x2E


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


def rd_varint(buf: bytes, pos: int = 0):
    val, shift = 0, 0
    while pos < len(buf):
        b = buf[pos]
        pos += 1
        val |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return val, pos
    raise AssertionError("truncated varint")


def collect(bot, seconds: float):
    """Every packet the client's reader queue hands over in a window."""
    out = []
    deadline = time.time() + seconds
    while True:
        left = deadline - time.time()
        if left <= 0:
            return out
        try:
            out.append(bot.read_pkt(timeout=left))
        except socket.timeout:
            return out


def move(bot, x: float, z: float, steps: int = 3) -> None:
    """Real Move Player Position packets, which is what the tracker reads."""
    for _ in range(steps):
        bot.send_pkt(0x1E, struct.pack(">ddd", x, -60.0, z) + b"\x01")
        time.sleep(0.12)


def animates(packets, eid: int):
    out = []
    for pid, body in packets:
        if pid != CB_ANIMATE:
            continue
        got, i = rd_varint(body)
        if got == eid and len(body) == i + 1:
            out.append(body[i])
    return out


def spawn_ids(packets):
    return {JT.decode_spawn_entity(body)[0] for pid, body in packets
            if pid == CB_ADD_ENTITY}


def removed_ids(packets):
    out = set()
    for pid, body in packets:
        if pid != CB_REMOVE_ENTITIES:
            continue
        count, i = rd_varint(body)
        for _ in range(count):
            eid, i = rd_varint(body, i)
            out.add(eid)
    return out


def wait_for(bot, predicate, seconds: float, label: str):
    """Reads until `predicate(packets)` is true, returning all packets seen."""
    seen = []
    deadline = time.time() + seconds
    while time.time() < deadline:
        seen += collect(bot, min(0.2, max(0.01, deadline - time.time())))
        if predicate(seen):
            return seen
    raise AssertionError(f"{label}: not seen within {seconds}s")


def main() -> None:
    data_root = REPO / "data" / "vanilla"
    assert data_root.is_dir(), "no data/vanilla extraction present; run tools/vanilla-data"

    with tempfile.TemporaryDirectory(prefix="plusmc-swing-") as td:
        world = pathlib.Path(td) / "world"
        region_dir = world / "dimensions" / "minecraft" / "overworld" / "region"
        region_dir.mkdir(parents=True)
        shutil.copyfile(FIXTURES / "r.0.0.mca", region_dir / "r.0.0.mca")

        env = dict(os.environ)
        env["PLUSMC_KEEPALIVE_MS"] = "1200"
        env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "6000"
        env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
        env["PLUSMC_MOB_SPAWNING"] = "0"
        env["PLUSMC_DAY_CYCLE"] = "0"
        proc, port = wire_client.start_server(
            [str(BIN), "--host", HOST, "--port", "0", "--world", str(world),
             "--offline-mode", "--gamemode", "1", "--view-distance", "2",
             "--data", str(data_root)],
            lambda p: JT.wait_for_listen_line(p, time.time() + 20), env=env)
        a = b = None
        try:
            a = JT.Client(PLAYER_A, port)
            wire_client.record(a, note="arm-swing-relay: the swinger")
            join_body, flags, comp = a.join()
            a_eid, dims = JT.decode_join_game(join_body, expect_online=0)
            assert flags["brand"] and flags["registry"] >= 20 and flags["tags"], flags
            assert comp and dims[:1] == ["minecraft:overworld"], dims
            assert a_eid > 0

            b = JT.Client(PLAYER_B, port)
            wire_client.record(b, note="arm-swing-relay: the viewer")
            join_body, flags, comp = b.join()
            b_eid, dims = JT.decode_join_game(join_body, expect_online=0)
            assert b_eid > 0 and b_eid != a_eid

            # The spawn batch and the join's own burst: neither player is in the
            # other's stream yet, because tracking starts from movement.
            collect(a, 1.5)
            collect(b, 1.5)

            # Both report a stance two blocks apart. A's move is what pairs A to
            # B (B's own move pairs B to A), and the pair is what the relay's
            # audience is built from.
            move(a, 8.5, 8.5)
            move(b, 10.5, 8.5)
            seen = wait_for(b, lambda pkts: a_eid in spawn_ids(pkts), 5.0,
                            "B's pairing burst for A")
            print(f"tracking: B was paired with A (entity {a_eid})")
            assert a_eid not in spawn_ids(collect(a, 0.2)), \
                "A must not spawn itself"

            # --- the main hand ------------------------------------------------
            a.send_pkt(SB_SWING, varint(0))
            seen = wait_for(b, lambda pkts: animates(pkts, a_eid), 3.0,
                            "the main-hand animate at B")
            assert animates(seen, a_eid) == [0], animates(seen, a_eid)
            print(f"main hand: B saw animate({a_eid}, 0)")
            mine = collect(a, 0.4)
            assert animates(mine, a_eid) == [], \
                "the swinger must not be sent its own animate packet"
            print("main hand: the swinger received no echo")

            # --- the off hand -------------------------------------------------
            a.send_pkt(SB_SWING, varint(1))
            seen = wait_for(b, lambda pkts: animates(pkts, a_eid), 3.0,
                            "the off-hand animate at B")
            assert animates(seen, a_eid) == [3], animates(seen, a_eid)
            print(f"off hand: B saw animate({a_eid}, 3)")

            # --- a hand the protocol cannot produce ---------------------------
            a.send_pkt(SB_SWING, varint(2))
            quiet = collect(b, 0.8)
            assert animates(quiet, a_eid) == [], (
                f"a hand byte of 2 reached the wire: {animates(quiet, a_eid)}")
            print("malformed hand: no animate packet")

            # --- the audience is the tracked set ------------------------------
            # 80 blocks east is past the 48-block tracking range and its drop
            # line; the move itself is what ends the relationship.
            move(b, 88.5, 8.5, steps=4)
            seen = wait_for(b, lambda pkts: a_eid in removed_ids(pkts), 5.0,
                            "Remove Entities for A after walking out")
            assert removed_ids(seen) == {a_eid}, removed_ids(seen)
            print("tracking: B was told to remove A after walking out")
            a.send_pkt(SB_SWING, varint(0))
            far = collect(b, 1.0)
            assert animates(far, a_eid) == [], (
                f"an untracked viewer received the swing: {animates(far, a_eid)}")
            print("out of range: the swing reached no one")

            a.close()
            b.close()
            a = b = None
            proc.terminate()
            rc = proc.wait(timeout=10)
            assert rc == 0, f"expected clean exit on SIGTERM, got {rc}"
            print("graceful shutdown: OK")
        finally:
            for bot in (a, b):
                if bot is not None:
                    bot.close()
            if proc.poll() is None:
                proc.kill()
                proc.wait()
    print("PASS")


if __name__ == "__main__":
    main()
