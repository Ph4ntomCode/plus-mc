#!/usr/bin/env python3
"""Attribute the packet/streaming path on a live plusmc server with several players.

`prof_plusmc.py` answers "what does one *tick* spend on the world" with a single
player. It cannot answer the multi-player question — what this server spends per
joined player and per packet — because every earlier profile parks the encode
thread in recvfrom and no run ever had more than one player joined.

This boots the real binary against a fixture world, joins `--bots` players, gives
them distinct work (bot 0 stays home and writes blocks; the others walk away
across chunk borders so they end up outside the view distance they spawned in),
samples the whole process with `/usr/bin/sample`, and counts, per player, what
that player actually *received* off its own socket — chunk data, block updates,
light updates, and the bytes of each. The counts are what make a broadcast defect
visible: a packet a player
should never have been sent shows up as a count on that player, and the light
updates a player genuinely needs still show up on the player who needed them.

The world is the shipped fixture (one real overworld column, `r.0.0.mca`) by
default, so chunk bodies are representative only near spawn; everything else is
the compact air fallback. The per-player *multiplicity* (the same chunk encoded
once per player) and the per-recipient *fan-out* of broadcasts are independent of
that and are what this measures — but the cost of *building* a body is not: with
the fixture, 99.85% of streamed chunks take the air fallback and the encoder is
barely entered, so anything about the body path (a shared body cache, say) must
be measured with `--world-mode memory`, where the world materialises a column for
every chunk and a body is a real ~45 KB encode.

The `writes` phase is the broadcast fan-out measurement: bot 0 writes blocks at
spawn while the other bots have walked away (the `stream` phase is what moves
them), so a block update delivered to a distant player is a packet that player
had no chunk to apply it to. `--write-burst` raises the write rate so that cost
is visible above the command path that issues the writes; run with
`--only stream,writes` in an A/B so the walk still happens, and read the
`__sendto` line, which is the per-recipient half alone.

Usage:
    python3 tools/prof_multiplayer.py . --bots 8 --secs 10 --write-burst 8 \
        --only stream,writes --binary /tmp/ab/plusmc_A
    python3 tools/prof_multiplayer.py . --bots 1 --tag _one   # the A/B's other side
"""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import threading
import time

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tests"))
sys.path.insert(0, str(REPO / "tools"))

import prof_tick  # noqa: E402
import spawn_wire_test as sw  # noqa: E402
import wire_client  # noqa: E402

# The load harness and the sampling helper are the single-player profiler's; this
# is the same instrument with more than one client on the other end.
from prof_plusmc import ENV, Load, sample  # noqa: E402

PHASES = ("join", "stream", "writes", "mixed")
FLOOR_Y = -60
SPAWN_X = 8.5
SPAWN_Z = 8.5

# Clientbound play ids, from src/protocol/packet_ids.generated.h (the same table
# `ids.h` builds its constants from). Counted per player so "who received what"
# is a number, not an inference.
CB_ADD_ENTITY = 0x01
CB_BLOCK_UPDATE = 0x08
CB_CHUNK_DATA = 0x2D  # level_chunk_with_light
CB_LIGHT_UPDATE = 0x30
WATCHED = {
    "chunk_data": CB_CHUNK_DATA,
    "block_update": CB_BLOCK_UPDATE,
    "light_update": CB_LIGHT_UPDATE,
    "add_entity": CB_ADD_ENTITY,
}


def received(seen, start):
    """Counts, by name, of the watched packets a bot's reader collected.

    Bytes are counted too: the per-packet question ("what does one packet cost
    this server") needs a size as well as a rate, and the body the reader
    already kept is that size.
    """
    counts = {name: 0 for name in WATCHED}
    counts["packets"] = 0
    counts["bytes"] = 0
    # The chunk coordinates each `chunk_data` carries (body starts u32 x, u32 z —
    # `encode_chunk_body`): the *distinct* chunks behind those packets are what a
    # per-chunk body build is repeated for, so the ratio is the duplication the
    # server side paid for.
    counts["chunks"] = set()
    for pid, body in seen[start:]:
        counts["packets"] += 1
        counts["bytes"] += len(body)
        if pid == CB_CHUNK_DATA and len(body) >= 8:
            counts["chunks"].add((int.from_bytes(body[0:4], "big", signed=True),
                                  int.from_bytes(body[4:8], "big", signed=True)))
        for name, want in WATCHED.items():
            if pid == want:
                counts[name] += 1
    return counts


def busy_report(path, top=12):
    """Whole-process CPU per 50 ms tick, not just the tick thread.

    `prof_tick.report` answers "what is the tick loop doing"; a multi-player
    server also spends CPU on every session thread — the join burst, chunk
    encoding and each socket write all run there — and that is exactly the
    per-player cost this instrument exists to see. Self time is a node's samples
    minus its children's, and a sample under an idle symbol (`recvfrom`, the
    keep-alive sleep) is the thread parked, so it is excluded.
    """
    threads, window = prof_tick.parse(str(path))
    ticks = max(window, 1) / 50.0
    per_sym: dict[str, float] = {}
    rows = []
    total = 0.0
    for t in threads:
        busy = 0.0

        def visit(node, idle):
            nonlocal busy
            # `__recvfrom` is where every session's reader thread parks; the
            # `recvfrom` in prof_tick's list is the un-prefixed spelling sample
            # uses on some builds.
            parked = idle or node.sym in prof_tick.IDLE_SYMBOLS or node.sym.endswith("recvfrom")
            if not parked:
                self_ms = node.count - sum(c.count for c in node.children)
                if self_ms > 0:
                    per_sym[node.sym] = per_sym.get(node.sym, 0.0) + self_ms
                    busy += self_ms
            for child in node.children:
                visit(child, parked)

        for root in t.roots:
            visit(root, False)
        total += busy
        rows.append((t.name, busy / ticks))
    rows.sort(key=lambda r: -r[1])
    print(f"   whole process: {total / ticks:.2f} ms/tick of CPU across {len(threads)} threads"
          f"  (window {window} ms, {ticks:.0f} ticks)", flush=True)
    for name, ms in rows[:6]:
        if ms > 0.02:
            print(f"      {ms:7.2f} ms/tick  {name}", flush=True)
    print("   -- top self, all threads --", flush=True)
    for sym, ms in sorted(per_sym.items(), key=lambda kv: -kv[1])[:top]:
        print(f"      {ms / ticks:7.2f} ms/tick  {sym}", flush=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("repo", nargs="?", default=".")
    ap.add_argument("--bots", type=int, default=4)
    ap.add_argument("--secs", type=int, default=10)
    ap.add_argument("--only", default=",".join(PHASES))
    ap.add_argument("--out", default="/tmp/multi")
    ap.add_argument("--binary", default=None)
    ap.add_argument("--world-mode", choices=("fixture", "memory"), default="fixture",
                    help="fixture = the shipped one-chunk region file (a *streamed* "
                         "chunk outside it has no region data and takes the air "
                         "fallback, so the chunk-body path is barely exercised); "
                         "memory = no --world, so the in-memory world materialises "
                         "a column for every chunk and the body path is the real one")
    ap.add_argument("--tag", default="")
    ap.add_argument("--walk-blocks", type=float, default=4.0,
                    help="blocks per movement packet (crossing chunks fast enough "
                         "to leave the view distance inside one window)")
    ap.add_argument("--walk-period", type=float, default=0.1)
    ap.add_argument("--write-period", type=float, default=0.02)
    ap.add_argument("--write-burst", type=int, default=1,
                    help="setblocks per write tick, at distinct cells, so every "
                         "one is a real change and the fan-out is what is measured")
    args = ap.parse_args()

    repo = pathlib.Path(args.repo).resolve()
    binary = pathlib.Path(args.binary).resolve() if args.binary else repo / "build" / "src" / "plusmc"
    want = [p.strip() for p in args.only.split(",") if p.strip()]

    fixtures = repo / "tests" / "fixtures"
    tmp = tempfile.mkdtemp(prefix="plusmc-multi-")
    world = pathlib.Path(tmp) / "world"
    launch = [str(binary), "--host", "127.0.0.1", "--port", "0", "--motd", "multi",
              "--offline-mode", "--gamemode", "1", "--data", str(repo / "data" / "vanilla"),
              "--view-distance", "8"]
    if args.world_mode == "fixture":
        region = world / "dimensions" / "minecraft" / "overworld" / "region"
        region.mkdir(parents=True)
        shutil.copyfile(fixtures / "r.0.0.mca", region / "r.0.0.mca")
        launch += ["--world", str(world), "--no-flat-world"]

    env = dict(os.environ)
    env.update(ENV)
    proc = None
    bots: list = []
    results = []
    try:
        proc, port = wire_client.start_server(launch, sw.wait_listening, env=env)
        print(f"[multi] server pid {proc.pid} on :{port}, {args.bots} bots, "
              f"world={args.world_mode}", flush=True)

        # Each bot walks its own lane so they never stack. Bot 0 is the writer and
        # stays home; the rest drift along +x, so by the time the writes phase
        # runs they are far outside its view.
        def spawn_bot(i):
            b = sw.Bot(f"Multi{i}", port)
            b.join()
            b.lane_z = SPAWN_Z + 2.0 * i
            b.x = SPAWN_X
            b.move_to(SPAWN_X, FLOOR_Y, b.lane_z)
            return b

        def join_rest(stagger=0.0):
            while len(bots) < args.bots:
                bots.append(spawn_bot(len(bots)))
                if stagger:
                    time.sleep(stagger)

        def walk(i):
            for b in bots[1:]:
                b.x += args.walk_blocks
                b.move_to(b.x, FLOOR_Y, b.lane_z)

        def writes(i):
            # One writer at home, toggling an 8x8 floor patch. Every one of those
            # writes broadcasts a block update to every player (the fan-out this
            # instrument exists to measure); distinct cells keep each write a
            # real change, so the packet count is the write count times the
            # recipient count and nothing else.
            for k in range(args.write_burst):
                n = i * args.write_burst + k
                x = 4 + (n % 8)
                z = 4 + ((n // 8) % 8)
                block = "minecraft:stone" if (n // 64) % 2 == 0 else "minecraft:air"
                sw.command(bots[0], f"setblock {x} {FLOOR_Y} {z} {block}", timeout=0.0)

        if "join" not in want:
            join_rest()

        for phase in want:
            layer = layer2 = joiner = None
            if phase == "join":
                mark = None
                joiner = threading.Thread(target=join_rest, args=(0.5,), daemon=True)
                joiner.start()
            else:
                # Flush the readers first, so a packet still queued from the
                # previous phase is not billed to this one.
                for b in bots:
                    sw.drain(b, 0.2)
                mark = [len(b.seen) for b in bots]
                if phase == "stream":
                    layer = Load("stream", walk, args.walk_period).start()
                elif phase == "writes":
                    layer = Load("writes", writes, args.write_period).start()
                elif phase == "mixed":
                    layer = Load("mixed", writes, args.write_period).start()
                    layer2 = Load("mixed-walk", walk, args.walk_period).start()
            out = pathlib.Path(f"{args.out}_{phase}{args.tag}.txt")
            wall = sample(proc.pid, args.secs, out)
            if joiner is not None:
                joiner.join(timeout=30)
                for b in bots:
                    sw.drain(b, 0.2)
                # The handshake consumed the spawn burst off the socket directly;
                # count from here on (the handshake is not what is being ranked).
                mark = [len(b.seen) for b in bots]
            if layer is not None:
                layer.stop()
            if layer2 is not None:
                layer2.stop()
            # Pull each player's arriving packets into its own log before counting.
            for b in bots:
                sw.drain(b, 0.5)
            ops = "" if layer is None else f", {layer.ops} ops issued"
            if layer is not None and layer.errors:
                ops += f" ({layer.errors} errored)"
            # Snapshot each player's receipts *now*: the logs keep growing, so
            # counting them at report time would bill every later phase's packets
            # to this one.
            per_bot = []
            for i, b in enumerate(bots):
                with b.lock:
                    seen = list(b.seen)
                per_bot.append((b.x, b.lane_z, received(seen, mark[i] if mark else 0)))
            print(f"[multi] {phase}: sampled {wall:.1f}s -> {out}{ops}", flush=True)
            results.append((phase, str(out), per_bot))
    finally:
        for b in bots:
            try:
                b.close()
            except OSError:
                pass
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)

    # What each player received, and whether the writer's updates reached the
    # players that were nowhere near it.
    print()
    for phase, path, per_bot in results:
        print(f"-- {phase}: CPU --", flush=True)
        busy_report(path)
        print(f"-- {phase}: received per player --")
        tot = {name: 0 for name in WATCHED}
        tot["packets"] = 0
        tot["bytes"] = 0
        all_chunks: set = set()
        for i, (x, z, got) in enumerate(per_bot):
            for name in WATCHED:
                tot[name] += got[name]
            tot["packets"] += got["packets"]
            tot["bytes"] += got["bytes"]
            print(f"   bot{i:<2} x~{x:7.1f} z~{z:6.1f}  "
                  f"chunk_data {got['chunk_data']:6d} ({len(got['chunks']):5d} distinct)  "
                  f"block_update {got['block_update']:6d}  "
                  f"light_update {got['light_update']:5d}  add_entity {got['add_entity']:4d}  "
                  f"packets {got['packets']:7d}  bytes {got['bytes']:9d}", flush=True)
            all_chunks.update(got["chunks"])
        print(f"   total: chunk_data {tot['chunk_data']} over {len(all_chunks)} distinct chunks, "
              f"block_update {tot['block_update']}, light_update {tot['light_update']}, "
              f"add_entity {tot['add_entity']}, packets {tot['packets']}, "
              f"bytes {tot['bytes']}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
