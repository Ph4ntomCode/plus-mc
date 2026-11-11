#!/usr/bin/env python3
"""Prove the shared chunk-body cache never hands a viewer stale bytes.

The cache in `src/server/session.cpp` keeps one encoded chunk body and serves it
to every viewer that streams the same chunk while the column is unchanged
(`encode_chunk_body` carries no block entities and the block/biome tables are
fixed, so nothing in a body is per-viewer). Two things could make sharing
*observably* wrong, and this checks both on the real wire:

  * a viewer that joins while a body for its chunk is already cached — it must
    receive the same full body the first watcher received, not a truncation;
  * a chunk *modified* after its body was cached — the next viewer must receive
    the new bytes, never the cached old ones.

Why the assertions are *within* a run, not across two runs: this tool boots the
real binary twice, once per arm, and the in-memory world's light is not
reproducible across boots — the light pass runs once per chunk on first ask, and
for a chunk on the edge of a streaming window its result depends on the order
the 3x3 ring around it was materialised, which two boots with concurrent viewers
do not share. Run this tool with the same binary on both sides (A against A) and
it reports about the same cross-run byte difference count as A against B: the
world, not the cache. The cross-run count is therefore printed as information.

There is a second, sharper reason the "two viewers, one chunk" assertion below is
restricted to *settled* chunks (both viewers asking after the chunk's light was
computed): the light pass is not single-flight. `ensure_chunk_light` asks
`has_chunk_light`, and relights if not — as two separate world operations — so
two viewers asking for the same *never-yet-lit* chunk at almost the same instant
both relight it, and the second relight reads a 3x3 ring whose neighbours have
since been materialised, giving a different body. That is a property of the
uncached server and shows up identically with the cache off (the tool prints both
arms' counts side by side, and they match); it is also what caps how often this
cache can hit, since each of those redundant relights moves the content token.
Fixing it is a single-flight relight, a separate change from this one.

That leaves one gap: a byte comparison alone also passes on a cache that never
hits or one with no staleness guard. Two additions close it. First, run the B
arm against a build with `PLUSMC_BODY_CACHE_STATS` counters (`--stats`) and the
tool asserts the cache really served bodies. Second, the mutation control:
`--expect-fail` is run with a B build that has an effectively unbounded cap *and*
its staleness key removed — with a chunk kept resident across the write, a cache
keyed only by chunk coordinates serves the pre-write body, and the tool must
report exactly that (a clean pass there is the failure).

    # the pair: arm A is a copy of session.cpp with the cache stubbed out
    # (`return std::nullopt;` at the top of cached_chunk_body, `return;` at the
    # top of remember_chunk_body), built in place of the archived object:
    #   cc <compile_commands.json flags for session.cpp> -o /tmp/ab/session_A.o \
    #       -c /tmp/ab/session_A.cpp
    #   cc -O3 -DNDEBUG -Wl,-search_paths_first -Wl,-headerpad_max_install_names \
    #       build/src/CMakeFiles/plusmc.dir/main.cpp.o /tmp/ab/session_A.o \
    #       -o /tmp/ab/plusmc_A build/src/libplusmc-core.a <z, ssl, crypto>
    python3 tools/prove_chunk_body_cache.py . \
        --binary-a /tmp/ab/plusmc_A --binary-b /tmp/ab/plusmc_B

The world is the in-memory one (no `--world`): every chunk materialises a real
column, so the encoder really runs for each body. In the file fixture almost
every streamed body is the compact air fallback, which is never cached at all —
a proof run there would be vacuous.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import shutil
import sys
import tempfile
import threading
import time

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tests"))
sys.path.insert(0, str(REPO / "tools"))

import spawn_wire_test as sw  # noqa: E402
import wire_client  # noqa: E402
from prof_plusmc import ENV  # noqa: E402

PHASES = ("first", "second", "third", "again", "again2")
CB_CHUNK_DATA = 0x2D  # level_chunk_with_light
VIEW_DISTANCE = 8
SPAWN = (8.5, -60.0, 8.5)
# Every write here lands in chunk (0,0), which every bot at SPAWN streams.
#
# The setup write exists so the *modification* can be opaque -> opaque
# (stone -> dirt, `FLOOR` -> `CHANGED`). Both dampen light the same way and
# neither emits, so that write moves the body's blocks without moving any light:
# no relight is scheduled, so the relight sites' `invalidate_chunk_body` never
# runs, and the chunk's **content token** is the only thing that can stop the
# cached pre-write body from being served. A light-changing write would be a
# weaker test — the relight would invalidate the body as well.
#
# The second modification (a glowstone in the same chunk) is the light case:
# light arrays change, the relight runs behind the token's back, and the body
# must not be reused.
FLOOR = "minecraft:stone"
CHANGED = "minecraft:dirt"
GLOW = "minecraft:glowstone"
WRITE_BLOCK = (8, -60, 8)
WRITE_GLOW = (8, -59, 8)
TARGET = (0, 0)


class RecordingBot(sw.Bot):
    """A bot that also keeps the packets `join` reads off the socket itself.

    `Bot.join` consumes the login/configuration exchange and the spawn chunk
    batch *before* the reader thread exists; those are exactly the bodies this
    proof needs (the first, therefore uncached, builds of the spawn chunks).
    Post-join packets arrive through the reader and are appended by `sw.drain`,
    so nothing is recorded twice.
    """

    def recv(self):
        pid, body = super().recv()
        with self.lock:
            self.seen.append((pid, body))
        return pid, body


def chunk_of(body: bytes) -> tuple[int, int]:
    """The chunk a body describes — the first two u32s `encode_chunk_body` writes."""
    return (int.from_bytes(body[0:4], "big", signed=True),
            int.from_bytes(body[4:8], "big", signed=True))


def bodies_of(packets) -> dict[tuple[int, int], tuple[int, str]]:
    """{chunk: (length, sha256)} for the chunk bodies in `packets` (last wins)."""
    out: dict[tuple[int, int], tuple[int, str]] = {}
    for pid, body in packets:
        if pid == CB_CHUNK_DATA and len(body) >= 8:
            out[chunk_of(body)] = (len(body), hashlib.sha256(body).hexdigest())
    return out


def mark(bot) -> int:
    with bot.lock:
        return len(bot.seen)


def snapshot(bot, mark_at: int) -> dict:
    with bot.lock:
        seen = list(bot.seen)
    return bodies_of(seen[mark_at:])


def quiesce(bot, slice_s: float = 0.4, want_quiet: int = 3, limit: float = 30.0) -> None:
    """Drain until `want_quiet` consecutive slices add no chunk body.

    Wall time is not free (the survival clock turns it into ticks), so this stops
    at the first quiet gap rather than waiting a fixed window out.
    """
    deadline = time.time() + limit
    quiet = 0
    while time.time() < deadline:
        window = sw.drain(bot, slice_s)
        if any(pid == CB_CHUNK_DATA for pid, _ in window):
            quiet = 0
        else:
            quiet += 1
            if quiet >= want_quiet:
                return


def walk_together(bots, blocks: float) -> None:
    """Move every bot `blocks` along +x in the same steps, in one loop.

    Stepping them in the same iteration is what makes two viewers ask for the
    same chunk inside one streaming window — the coincidence the cache exists
    for, and the situation in which a stale body would reach a second viewer.
    Profiler-sized steps, so no single position packet is a teleport.
    """
    step = 4.0
    sign = 1.0 if blocks >= 0 else -1.0
    for _ in range(int(abs(blocks) / step)):
        for b in bots:
            b.x += sign * step
            b.move_to(b.x, SPAWN[1], SPAWN[2])
        time.sleep(0.08)


def run_arm(binary: pathlib.Path, world: pathlib.Path,
            stats: pathlib.Path | None = None) -> dict:
    """One arm: three viewers and one modification, in a fixed order."""
    launch = [str(binary), "--host", "127.0.0.1", "--port", "0", "--motd", "proof",
              "--offline-mode", "--gamemode", "1",
              "--data", str(REPO / "data" / "vanilla"),
              "--view-distance", str(VIEW_DISTANCE)]
    env = dict(os.environ)
    env.update(ENV)
    if stats is not None:
        stats.unlink(missing_ok=True)
        env["PLUSMC_BODY_CACHE_STATS"] = str(stats)
    proc = None
    bots = []
    trace: dict = {"notes": []}
    try:
        proc, port = wire_client.start_server(launch, sw.wait_listening, env=env)
        print(f"[proof] {binary.name}: pid {proc.pid} on :{port}, in-memory world",
              flush=True)

        def make_bot(name: str):
            b = RecordingBot(name, port)
            b.x = SPAWN[0]
            bots.append(b)
            return b

        def join_bot(b) -> None:
            b.join()
            b.move_to(*SPAWN)

        # 0. A setup viewer, so the target chunk holds an opaque block before
        #    any of the measured viewers streams it. Its write changes light, so
        #    it relights — which is fine here, because nothing is cached yet.
        setup = make_bot("Proof0")
        join_bot(setup)
        quiesce(setup)
        sw.command(setup, f"setblock {WRITE_BLOCK[0]} {WRITE_BLOCK[1]} "
                          f"{WRITE_BLOCK[2]} {FLOOR}")
        time.sleep(0.6)
        sw.drain(setup, 0.6)

        # 1+2. Two viewers whose spawn batches overlap: the first builds each
        #      body, the second asks for the same chunks 0.4s later, while they
        #      are still cached — the case sharing has to get exactly right.
        b1 = make_bot("Proof1")
        b2 = make_bot("Proof2")
        t1 = threading.Thread(target=join_bot, args=(b1,), daemon=True)
        t2 = threading.Thread(target=join_bot, args=(b2,), daemon=True)
        t1.start()
        time.sleep(0.4)
        t2.start()
        t1.join(timeout=60)
        t2.join(timeout=60)
        quiesce(b1)
        quiesce(b2)
        trace["first"] = snapshot(b1, 0)
        trace["second"] = snapshot(b2, 0)
        trace["notes"].append(
            f"overlapping join (0.4s apart): first viewer "
            f"{len(trace['first'])} chunks, second {len(trace['second'])} chunks")

        # 3. Modification A, opaque -> opaque: the body's blocks move and no
        #    light does, so only the content token stands between the cached
        #    pre-write body and the wire.
        sw.command(b1, f"setblock {WRITE_BLOCK[0]} {WRITE_BLOCK[1]} "
                        f"{WRITE_BLOCK[2]} {CHANGED}")
        time.sleep(0.4)
        sw.drain(b1, 0.4)

        # 4. Both viewers that already hold the chunk leave view together and
        #    come back: the chunk is pruned and re-sent to both in one window, so
        #    both must get the new bytes — never the pre-write body, which is
        #    what a cache without the token serves here. This is the stale-serve
        #    trap: nothing rebuilt the chunk since the write.
        walk_together([b1, b2], 16.0 * (VIEW_DISTANCE + 2))
        time.sleep(0.5)
        sw.drain(b1, 0.4)
        sw.drain(b2, 0.4)
        m1, m2 = mark(b1), mark(b2)
        walk_together([b1, b2], -16.0 * (VIEW_DISTANCE + 2))
        quiesce(b1)
        quiesce(b2)
        trace["again"] = snapshot(b1, m1)
        trace["again2"] = snapshot(b2, m2)

        # 5. Modification B, the light case: a glowstone changes the chunk's
        #    light, which the relight pass writes through the recorder's inner
        #    world.
        sw.command(b1, f"setblock {WRITE_GLOW[0]} {WRITE_GLOW[1]} "
                        f"{WRITE_GLOW[2]} {GLOW}")
        time.sleep(0.6)
        sw.drain(b1, 0.6)

        # 6. A viewer that has never seen the chunk, joining after both writes: a
        #    fresh build of the current column.
        b3 = make_bot("Proof3")
        join_bot(b3)
        quiesce(b3)
        trace["third"] = snapshot(b3, 0)
        return trace
    finally:
        for b in bots:
            try:
                b.close()
            except OSError:
                pass
        if proc is not None:
            proc.terminate()
            try:
                # The counters are written from a static destructor on the clean
                # SIGTERM stop `main.cpp` installs, so read them after the wait.
                proc.wait(timeout=15)
            except Exception:  # noqa: BLE001 - a wedged server must not hang the proof
                proc.kill()
        if stats is not None and stats.exists():
            trace["stats"] = stats.read_text().strip()
        shutil.rmtree(world, ignore_errors=True)


def digest(trace: dict, phase: str, chunk=TARGET) -> str | None:
    got = trace.get(phase, {}).get(chunk)
    return None if got is None else got[1]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("repo", nargs="?", default=".")
    ap.add_argument("--binary-a", required=True, help="cache disabled")
    ap.add_argument("--binary-b", required=True, help="the shipping build")
    ap.add_argument("--stats", default=None,
                    help="file to collect the instrumented B arm's cache counters "
                         "in (PLUSMC_BODY_CACHE_STATS)")
    ap.add_argument("--expect-fail", action="store_true",
                    help="the B build's staleness guard has been removed; the "
                         "modification checks must FAIL (the mutation control)")
    args = ap.parse_args()

    repo = pathlib.Path(args.repo).resolve()
    a = pathlib.Path(args.binary_a).resolve()
    b = pathlib.Path(args.binary_b).resolve()
    started = time.time()
    trace_a = run_arm(a, pathlib.Path(tempfile.mkdtemp(prefix="plusmc-proof-a-")))
    trace_b = run_arm(b, pathlib.Path(tempfile.mkdtemp(prefix="plusmc-proof-b-")),
                      pathlib.Path(args.stats) if args.stats else None)

    failures: list[str] = []
    checks: list[tuple[bool, str, str]] = []
    first_race: dict[str, str] = {}
    race_sets: dict[str, set] = {}

    def check(ok: bool, name: str, detail: str) -> None:
        checks.append((ok, name, detail))
        if not ok:
            failures.append(name)

    # -- within one run: unchanged chunks, seen twice, must be the same bytes ---
    for name, trace in (("A (cache off)", trace_a), ("B (cache on)", trace_b)):
        h_first = digest(trace, "first")
        h_second = digest(trace, "second")
        h_third = digest(trace, "third")
        shared = set(trace.get("first", {})) & set(trace.get("second", {}))
        differ = [c for c in shared
                  if trace["first"][c][1] != trace["second"][c][1]]
        first_race[name] = f"{len(differ)}/{len(shared)}"
        race_sets[name] = set(differ)
        check(h_first is not None,
              f"{name}: the target chunk was streamed at all",
              f"chunk {TARGET} among the first viewer's {len(trace.get('first', {}))} chunks")
        check(h_first is not None and h_second is not None and h_first == h_second,
              f"{name}: the viewer joining while cached got the first viewer's bytes",
              f"{str(h_first)[:12]} vs {str(h_second)[:12]}")

        # Both writes must be visible to a viewer that joins afterwards...
        check(h_first is not None and h_third is not None and h_third != h_first,
              f"{name}: the modifications are visible to a viewer joining after "
              "them", f"before {str(h_first)[:12]} after {str(h_third)[:12]}")
        # ...and to the viewers that held the chunk, re-streamed after the
        # light-neutral write and before anything else rebuilt the chunk.
        for phase, what in (("again", "the first returning viewer"),
                            ("again2", "the second returning viewer")):
            got = digest(trace, phase)
            if got is None:
                continue
            # Post-dirt, pre-glowstone: the one comparison that only the content
            # token can satisfy (an invalidate never ran for that write).
            check(h_first is not None and got != h_first,
                  f"{name}: {what} got the new bytes after the light-neutral write",
                  f"re-streamed {got[:12]} vs pre-write {str(h_first)[:12]}"
                  + ("  <-- THE PRE-WRITE BYTES WERE SERVED STALE"
                     if got == h_first else ""))
        # Both returning viewers must agree with each other on every chunk they
        # share (a large set, streamed in one window right after the write).
        again, again2 = trace.get("again", {}), trace.get("again2", {})
        shared_back = set(again) & set(again2)
        back_differ = [c for c in shared_back if again[c][1] != again2[c][1]]
        check(bool(shared_back) and not back_differ,
              f"{name}: the two returning viewers agree on every re-sent chunk",
              f"{len(shared_back)} chunks shared, {len(back_differ)} differ"
              + (f" (e.g. {back_differ[:3]})" if back_differ else ""))

    # -- the cache arm really served cached bodies -----------------------------
    stats = trace_b.get("stats")
    if stats:
        fields = dict(part.split("=", 1) for part in stats.split() if "=" in part)
        served, calls = int(fields.get("hits", 0)), int(fields.get("calls", 0))
        check(served > 0, "the cache arm really served cached bodies",
              f"{served}/{calls} requests hit, peak "
              f"{int(fields.get('peak_entries', 0))} entries / "
              f"{int(fields.get('peak_bytes', 0))} B, "
              f"miss_token={fields.get('miss_token')}")
        print(f"[proof] B counters: {stats}")

    # -- every body is a full body, not a truncation ---------------------------
    sizes = [n for ph in trace_b.values() if isinstance(ph, dict)
             for n, _ in ph.values()]
    check(bool(sizes) and min(sizes) > 512,
          "no truncated or empty body on the wire",
          f"{len(sizes)} bodies, {min(sizes) if sizes else 0}.."
          f"{max(sizes) if sizes else 0} bytes")

    # -- informational: cross-run byte differences ----------------------------
    # Not asserted: the in-memory world is not byte-reproducible across boots
    # (run this tool with --binary-b == --binary-a to see the same count with no
    # cache anywhere). Inside a run, the checks above cover what matters.
    cross = 0
    for phase in PHASES:
        pa, pb = trace_a.get(phase, {}), trace_b.get(phase, {})
        cross += sum(1 for c in set(pa) & set(pb) if pa[c][1] != pb[c][1])
    chunk_sets_agree = all(
        set(trace_a.get(ph, {})) == set(trace_b.get(ph, {})) for ph in PHASES)

    print(f"\n[proof] {time.time() - started:.0f}s")
    for note in trace_a["notes"] + trace_b["notes"]:
        print(f"[proof] {note}")
    for ok, name, detail in checks:
        print(f"  {'PASS' if ok else 'FAIL'}  {name}: {detail}")
    print(f"  info  chunk sets identical between arms: {chunk_sets_agree}")
    print(f"  info  cross-run bodies that differ: {cross} "
          "(world light order-dependence, not an assertion)")
    shared_race = len(race_sets.get("A (cache off)", set())
                      & race_sets.get("B (cache on)", set()))
    print("  info  first-vs-second viewer byte differences, same run "
          f"(pre-existing relight race, varies run to run): "
          f"A {first_race.get('A (cache off)')}, B {first_race.get('B (cache on)')}"
          f", {shared_race} chunks in both — present with the cache off, so "
          "not the cache")
    if args.expect_fail:
        # The mutation control: a B build with no staleness key (and a cap that
        # keeps the pre-write body resident) must be caught by the modification
        # checks, so a clean pass here is the failure.
        stale = [f for f in failures if "new bytes" in f or "visible" in f]
        if not failures:
            print("\n[proof] FAILED (expected): the staleness mutation was NOT caught")
            return 1
        if not stale:
            print("\n[proof] FAILED (expected): the mutation was caught, but not by a "
                  f"staleness check ({failures[:3]})")
            return 1
        print(f"\n[proof] OK (expected failure): the mutation served stale bytes and "
              f"was caught by {len(stale)} staleness check(s), e.g. {stale[0]!r}")
        return 0
    if failures:
        print(f"\n[proof] FAILED ({len(failures)})")
        return 1
    print("\n[proof] OK: every viewer of an already-lit chunk got the same bytes, "
          "and a chunk modified after its body was cached was re-sent — never "
          "served stale")
    return 0


if __name__ == "__main__":
    sys.exit(main())
