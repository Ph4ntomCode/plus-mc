#!/usr/bin/env python3
"""The light pool's post-leave return, asserted instead of described.

`PLUSMC_LIGHT_ALLOC` is a choice between two storages for the light engine's
2 KiB arrays: a 2 KiB slot pool over dedicated pages that are unmapped when both
slots are free, or the ordinary heap. The claim the knob is worth is a
measurement, not a shape: after a streamed population leaves, the pool's pages
come back. That number depends on two things — the store releasing the columns
(and with them the arrays), and the pool unmapping a page once its last slot is
freed — and only the second is the pool's own.

So this test runs both arms of one cell through bench/soak.py and reads the
pool's *own accounting* across the leave window. The server prints, after every
release pass, `light pool: N MB in P pages (2 arrays per page; B before the
release)`: `B` is what the pool had booked when the pass started, `P` what it
still holds after it. Both are the pool's counters, so they move only when a slot
is freed or handed out — not when the allocator trims, not when a worker faults
pages back in, not when the box's load changes between the two arms. That is the
quantity this cell gates: releasing 192 of 256 columns must give back a real
share of the pool's booked pages, and what stays must be the kept columns'
arrays.

The cell is deliberately small — 4 stationary bots, the store capped at 256
columns so both arms release the same 192 — and it fails rather than skips when
the store never filled (the comparison needs the occupancy, and a loaded box can
keep the bots from streaming it) or when the pooled arm never logged a release
pass. Those runs cannot make the claim, and a skip code here would let the cell
drop out of the suite without saying so; the failure names the box as the likely
reason. The test has no skip path left.

RSS is printed, not gated. An earlier revision compared the two arms' returns
across the leave window, and then their post-leave levels; both were rejected by
the same measurement. On this tree the pool holds ~1 MB of a ~45 MB process
(docs/PHASE5.md §4's 18 Oct re-measurement: the send-time relight stores
ring-warmed neighbours unlit, so a released column carries ~3 arrays, not ~9),
while what an arm gives back across the window is dominated by the page
allocator's column buffers and the malloc relief a pass runs — several MB that
swing between identical runs of one binary (0.0, 5.2 and 12.9 MB returned were
all measured on this binary). A band on that number either passes for the wrong
reason or fails a pool that has done nothing wrong; the booked pages above are
the pool's own claim, and they are what is gated.

Usage: light_pool_leave_test.py <path-to-plusmc>
"""

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOAK = ROOT / "bench" / "soak.py"
CAP = 256
BOTS = 4
SECONDS = 90.0
LEAVE = 45.0
# `light pool: 1 MB in 287 pages (2 arrays per page; 358 before the release)`.
# The element size is kLightArrayBytes (`world/light.h`), so the page size the
# pool maps is `arrays_per_page * 2048`, whatever the host's page is.
POOL_RE = re.compile(
    r"light pool: \d+ MB in (\d+) pages \((\d+) arrays per page; (\d+) before the release\)")
ARRAY_BYTES = 2048
# Releasing 192 of 256 columns (75%) must give back at least this fraction of the
# pool's booked pages. The measured cell returns 234 -> 91 pages (61%); a quarter
# is the floor under the mechanism, not a restatement of the measurement.
MIN_POOL_RETURN_FRACTION = 0.25


def run_arm(binary, arm, alloc, out_dir):
    """One arm of the cell; returns its record plus the log's release and pool lines."""
    record = out_dir / f"{arm}.json"
    log = out_dir / f"{arm}.log"
    env = {
        "PLUSMC_CHUNK_BUDGET_COLUMNS": str(CAP),
        "PLUSMC_MOB_SPAWNING": "0",
        # The cell asks what the *light pool* gives back when the store releases
        # columns, so its terrain has to stay evictable. A decorated column
        # carries leaves and grass, the random-tick pass writes to them, and
        # `release_idle_columns` may not drop a column that carries writes — so
        # on the default (decorated) arm almost every resident column is
        # irreplaceable and the release is 0 for both arms, which says nothing
        # about the pool. The carved arm keeps the cell's premise (see
        # docs/PHASE5.md §4, and `docs/WORLDGEN_PARITY.md` on the decorated
        # store's own release limit).
        "PLUSMC_WORLDGEN_STAGE": "carved",
    }
    if alloc:
        env["PLUSMC_LIGHT_ALLOC"] = "0"
    argv = [sys.executable, str(SOAK),
            "--bin", str(binary),
            "--generated", "42", "--bots", str(BOTS), "--stationary",
            "--spread", "256", "--view-distance", "8",
            "--seconds", f"{SECONDS:g}", "--leave", f"{LEAVE:g}",
            "--max-rss", "2000", "--quiet",
            "--log", str(log), "--json", str(record)]
    child_env = dict(os.environ, **env)
    result = subprocess.run(argv, cwd=ROOT, env=child_env, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"light_pool_leave: the {arm} arm exited {result.returncode}", file=sys.stderr)
        print(result.stdout[-2000:], file=sys.stderr)
        print(result.stderr[-2000:], file=sys.stderr)
        return None
    data = json.loads(record.read_text())
    # The idle release runs on a 10 s cadence and may give columns back over
    # more than one pass when a loaded box interrupts the first one: the log
    # reports what that pass released, not a running total. Summing the passes
    # is what the cell is asking about; taking the last line measured one pass
    # (e.g. 36 of 192) and called the rest a failure.
    #
    # The sum stops once the 192 the cell asked about are back. A worker that
    # was already inside a column when the bots left finishes it and stores it
    # afterwards (world/chunk_generation.h's `discard_pending` drops the queue,
    # not the in-flight job), so a later pass honestly reports those few
    # columns; charging them to the release made a 192-column release read as
    # 195. They are not part of what the store gave back from the bots' view.
    expected = CAP - 64
    releases = []
    released = 0
    pool_pages = []     # (booked before the pass, still booked after it)
    arrays_per_page = None
    for line in log.read_text().splitlines():
        pool = POOL_RE.search(line)
        if pool:
            arrays_per_page = int(pool.group(2))
            pool_pages.append((int(pool.group(3)), int(pool.group(1))))
        if "world store: released " not in line:
            continue
        count = int(line.split("world store: released ")[1].split()[0])
        releases.append(count)
        if released < expected:
            released = min(expected, released + count)
    return {"record": data, "released": released, "release_passes": releases,
            "pool_pages": pool_pages, "arrays_per_page": arrays_per_page}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary")
    args = ap.parse_args()

    binary = pathlib.Path(args.binary).resolve()
    if not binary.exists():
        print(f"light_pool_leave: no binary at {binary}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="plusmc-light-pool-") as tmp:
        out = pathlib.Path(tmp)
        heap = run_arm(binary, "heap", True, out)
        pooled = run_arm(binary, "pooled", False, out)
        if heap is None or pooled is None:
            return 1

        for arm, got in (("heap", heap), ("pooled", pooled)):
            world = got["record"]["world"]["world_chunks"]
            if world.get("max") != CAP:
                print(f"light_pool_leave: FAIL — the {arm} arm filled to "
                      f"{world.get('max')} of {CAP} columns, so the release is not the "
                      f"same cell; a loaded box can starve the stream — re-run on a "
                      f"quieter one", file=sys.stderr)
                return 1
            if got["released"] != CAP - 64:
                print(f"light_pool_leave: the {arm} arm released {got['released']} of "
                      f"{CAP - 64} columns", file=sys.stderr)
                return 1

        # The gate: the pool's own pages across the release. The heap arm must
        # not log them at all — a pooled line there means PLUSMC_LIGHT_ALLOC=0
        # did not take effect and the two arms were the same storage.
        if heap["pool_pages"]:
            print("light_pool_leave: FAIL — the heap arm logged light-pool pages, so "
                  f"PLUSMC_LIGHT_ALLOC=0 was not in force ({len(heap['pool_pages'])} "
                  "release passes reported the pool)", file=sys.stderr)
            return 1
        if not pooled["pool_pages"] or pooled["arrays_per_page"] is None:
            print("light_pool_leave: FAIL — the pooled arm never logged a release pass "
                  "with the pool's booked pages, so the cell cannot say whether the "
                  "release returned them; re-run on a quieter box", file=sys.stderr)
            return 1
        pool_before = pooled["pool_pages"][0][0]
        pool_after = pooled["pool_pages"][-1][1]
        returned = pool_before - pool_after
        need = max(1, int(pool_before * MIN_POOL_RETURN_FRACTION))
        page_bytes = pooled["arrays_per_page"] * ARRAY_BYTES

        print(f"light_pool_leave: {CAP} columns resident, {CAP - 64} released in both arms")
        # The leave window's RSS is context, not a gate: the sample at the close is
        # the last metrics line the reader happened to see, and it races the drop
        # the close itself causes (the bots' session teardown sits in the same
        # second as the store's release). An arm whose reader missed it is a reason
        # to print less, not to fail a cell whose claim is the pool's counters.
        for arm, got in (("heap", heap), ("pooled", pooled)):
            leave = got["record"].get("rss_after_leave") or {}
            peak = got["record"]["rss_bytes"]["peak"] / 1e6
            if leave.get("at_close") is None or leave.get("after") is None:
                print(f"  {arm:6} RSS not sampled across the leave (peak {peak:.1f} MB)"
                      f" — reported, not gated")
                continue
            before, after = leave["at_close"] / 1e6, leave["after"] / 1e6
            print(f"  {arm:6} {after:.1f} MB after the leave, gave back "
                  f"{before - after:.1f} MB (peak {peak:.1f} MB) — reported, not gated")
        print(f"  light pool: {pool_before} pages booked before the release, "
              f"{pool_after} after ({returned} returned, "
              f"{returned * page_bytes / 1e6:.2f} MB at {page_bytes} B pages; "
              f"floor {need} pages)")
        if returned < need:
            print(f"light_pool_leave: FAIL — the pool returned {returned} of the "
                  f"{pool_before} pages it had booked, under the {need}-page floor: "
                  f"releasing {CAP - 64} columns did not give the arrays back",
                  file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
