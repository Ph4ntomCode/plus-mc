#!/usr/bin/env python3
"""Attribute tick cost on a *live* plusmc server, one load at a time.

The component benchmarks (`tools/bench_world_reads.cpp`) measure the pieces the
code is made of. They cannot say which piece a running server actually spends a
tick on — that ordering has twice differed from what the microbenchmarks
suggested, so the ranking has to come from the server itself.

This boots the real binary against a fixture world, joins a bot, and then runs
one named load at a time while `/usr/bin/sample` samples the process every
millisecond. `tools/prof_tick.py` reads those samples back as ms-of-tick-CPU per
function (one sample is one millisecond of one thread; a thread's sample count is
the window, so a tick is `samples / (window/50)`).

The loads are the steady states a player-session server actually has to survive:
idle (one bot, natural spawning), a moving player, a block-write burst, the light
drain with its queue saturated, mob load, and fluid. Each load is issued by the
same wire client the test suite uses, and each reports how many operations it
actually managed to send, so a slow box cannot silently turn a load into a no-op.

Usage:
    python3 tools/prof_plusmc.py .                      # every phase, 8 s each
    python3 tools/prof_plusmc.py . --secs 12
    python3 tools/prof_plusmc.py . --only drain,mobs    # subset
    python3 tools/prof_plusmc.py . --no-sample          # parse what a run left
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

import wire_client  # noqa: E402
import spawn_wire_test as sw  # noqa: E402
import prof_tick  # noqa: E402

PHASES = ("idle", "move", "writes", "drain", "mobs", "fluid")
ENV = {
    "PLUSMC_KEEPALIVE_MS": "1000",
    "PLUSMC_KEEPALIVE_TIMEOUT_MS": "60000",
    "PLUSMC_DAY_CYCLE": "0",
    "PLUSMC_MOB_SPAWNING": "1",
    "PLUSMC_SURVIVAL_CLOCK": "packet",
    "PLUSMC_RANDOM_TICK_SPEED": "3",
}

# The fixture world's chunk (0,0); the neighbouring chunks are generated on join.
FLOOR_Y = -60
PLAYER = (8.5, -60.0, 8.5)
# One cell per chunk, so the light drain has more dirty chunks than it can clear:
# `kLightChunksPerTick` is 2, and these four sit in four different chunks.
CHUNK_CELLS = ((8, FLOOR_Y, 8), (24, FLOOR_Y, 8), (8, FLOOR_Y, 24), (24, FLOOR_Y, 24))


class Load:
    """A background thread issuing operations at a fixed pace until stopped."""

    def __init__(self, name: str, body, period: float):
        self.name = name
        self.body = body
        self.period = period
        self.ops = 0
        self.errors = 0
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def _run(self) -> None:
        i = 0
        while not self._stop.is_set():
            try:
                self.body(i)
                self.ops += 1
            except Exception:
                self.errors += 1
            i += 1
            self._stop.wait(self.period)

    def start(self) -> "Load":
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return self

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=5)


def sample(pid: int, secs: int, path: pathlib.Path) -> float:
    t0 = time.time()
    subprocess.run(["/usr/bin/sample", str(pid), str(secs), "-file", str(path)],
                   check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return time.time() - t0


def setblock(bot, cell, block, timeout=0.0) -> None:
    """`/setblock` one cell, without waiting for the command's packet window.

    The default window (`sw.command`'s 0.6 s) would pace the load at ~3 ops/s and
    quietly turn a "burst" into a trickle; the load's own period is the pace that
    matters here, so the feedback is left for the reader thread.
    """
    x, y, z = cell
    sw.command(bot, f"setblock {x} {y} {z} {block}", timeout=timeout)


def make_loads(bot):
    """One load per phase. Each is a function of the operation counter `i`."""
    toggle = lambda i: "minecraft:stone" if (i // 2) % 2 == 0 else "minecraft:air"

    def move(i):
        # A player walking a small circle: position packets drive chunk streaming,
        # entity tracking and the packet path, with no block changes at all.
        a = i * 0.6
        bot.move_to(PLAYER[0] + 6.0 * (1 if int(a) % 2 == 0 else -1), PLAYER[1],
                    PLAYER[2] + 6.0 * (1 if int(a / 2) % 2 == 0 else -1))

    def writes(i):
        cell = (i % 4, FLOOR_Y, 1)
        setblock(bot, cell, toggle(i))

    def drain(i):
        setblock(bot, CHUNK_CELLS[i % 4], "minecraft:glowstone" if i % 2 == 0 else "minecraft:air")

    def fluid(i):
        # Pour a source on the platform edge every few ops; the fluid ticks then
        # spread it across the floor, which is a per-tick scheduled work queue.
        cell = (10 + (i % 3), FLOOR_Y + 1, 10 + (i // 3) % 3)
        setblock(bot, cell, "minecraft:water")

    return {
        "idle": None,
        "move": Load("move", move, 0.05),   # 20 position updates/s
        "writes": Load("writes", writes, 0.05),  # 2 block writes/tick, one chunk
        "drain": Load("drain", drain, 0.012),   # 4 writes/tick over 4 chunks
        "mobs": None,  # a fixed population, summoned once before the window
        "fluid": Load("fluid", fluid, 0.10),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("repo", nargs="?", default=".")
    ap.add_argument("--secs", type=int, default=8)
    ap.add_argument("--only", default=",".join(PHASES))
    ap.add_argument("--out", default="/tmp/prof")
    ap.add_argument("--binary", default=None,
                    help="server binary to profile (default <repo>/build/src/plusmc)")
    ap.add_argument("--tag", default="", help="suffix for --out, to keep both sides of an A/B")
    ap.add_argument("--no-sample", action="store_true", help="just parse --out files")
    args = ap.parse_args()

    repo = pathlib.Path(args.repo).resolve()
    binary = pathlib.Path(args.binary).resolve() if args.binary else repo / "build" / "src" / "plusmc"
    want = [p.strip() for p in args.only.split(",") if p.strip()]

    if args.no_sample:
        for p in want:
            prof_tick.report(f"{args.out}_{p}{args.tag}.txt", 12, p,
                             ["memcmp", "strlen", "block_blocks_motion"])
        return 0

    fixtures = repo / "tests" / "fixtures"
    data_root = repo / "data" / "vanilla"
    tmp = tempfile.mkdtemp(prefix="plusmc-prof-")
    world = pathlib.Path(tmp) / "world"
    region = world / "dimensions" / "minecraft" / "overworld" / "region"
    region.mkdir(parents=True)
    shutil.copyfile(fixtures / "r.0.0.mca", region / "r.0.0.mca")

    env = dict(os.environ)
    env.update(ENV)
    proc = None
    bot = None
    loads = {}
    results = []
    try:
        proc, port = wire_client.start_server(
            [str(binary), "--host", "127.0.0.1", "--port", "0", "--motd", "prof",
             "--offline-mode", "--gamemode", "1", "--data", str(data_root),
             "--world", str(world), "--no-flat-world", "--view-distance", "8"],
            sw.wait_listening, env=env)
        print(f"[prof] server pid {proc.pid} on :{port}", flush=True)
        bot = sw.Bot("ProfBot", port)
        bot.join()
        sw.drain(bot, 1.5)
        bot.move_to(*PLAYER)
        sw.command(bot, "time set midnight")
        sw.drain(bot, 1.5)
        print("[prof] joined, midnight", flush=True)

        loads = make_loads(bot)
        # The mob load is a fixed population, not a rate: summon it once, before
        # its own sample window, so the window measures steady state and not the
        # spawn burst.
        for phase in want:
            if phase == "mobs":
                for _ in range(4):
                    sw.command(bot, "summon zombie 16", timeout=0.3)
                sw.drain(bot, 1.5)
                print("[prof] 64 zombies summoned", flush=True)

            load = loads.get(phase)
            if load is not None:
                load.start()
            out = pathlib.Path(f"{args.out}_{phase}{args.tag}.txt")
            wall = sample(proc.pid, args.secs, out)
            if load is not None:
                load.stop()
            ops = "" if load is None else f", {load.ops} ops issued"
            if load is not None and load.errors:
                ops += f" ({load.errors} errored)"
            print(f"[prof] {phase}: sampled {wall:.1f}s -> {out}{ops}", flush=True)
            results.append((phase, str(out)))
    finally:
        for load in loads.values():
            if load is not None:
                load.stop()
        if bot is not None:
            bot.close()
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)

    print()
    for phase, path in results:
        prof_tick.report(path, 12, phase, ["memcmp", "strlen", "block_blocks_motion"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
