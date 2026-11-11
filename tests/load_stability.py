#!/usr/bin/env python3
"""Load-stability proof: run every wire test *at the same time*, repeatedly.

`ctest -j4` runs four servers at once and is the suite's normal gate. This is the
stress version of that question — "if the machine is busy, does anything fail?" —
and it is asked the way the answer is useful: **all** of the socket tests at
once, several times, with every run's output kept so a failure can be read
instead of guessed at. `ctest -j4` on a loaded box has produced flakes in several
different files over this project's life (four in the milestone-G record, then
`mobs`, `consume-wire`, `tnt-wire` and `thrown-potion-wire`), and each one needed
the *evidence* to decide whether it was the feature or the harness. The
natural-spawn class among them is closed at the source now: every wire test's
server is started mob-free by default (`wire_client.server_env`), so a run this
script starts has no natural mobs in it either — the population a test sees is
the population it created.

Two things are measured, not just one:

1. **Exit status**, per test per repeat. A non-zero rc is a failure and the
   script's own exit status says so — a proof that never fails anything proves
   nothing.
2. **The step lines each test printed**, compared across repeats
   (`--show-diffs` to see them). `rc == 0` alone cannot tell a test whose
   assertions held from one whose heavy steps silently skipped; identical step
   lines across repeats is the stronger statement, and a *difference* locates
   where nondeterminism actually lives. Lines are compared with their `[ 12.40s]`
   timing prefix, the temporary world's random directory name and the ephemeral
   port stripped — a slower machine and a different temp dir are not differences,
   while any number a step printed that moved between repeats is.

The summary's last column is that verdict per test: `same` when every repeat
printed the same steps in the same order, `Δn` when n step lines moved (with the
values printed below the table), `STEP-COUNT` when the number of steps itself
moved, which means a step ran in one repeat and skipped in another.

What it does NOT do is pass/fail-ify a loaded batch on the moved numbers: a
machine that cannot start 29 servers is not a bug in the server. The summary says
which tests failed, how long each took, and the first assertion line of each
failure's log, so "slow but correct" and "nondeterministic assertion" can be told
apart.

Usage:
    tests/load_stability.py <build-dir> [--repeats N] [--filter RE]
                            [--timeout SECS] [--keep-dir DIR] [--show-diffs N]
                            [--summary FILE] [--quiet]

The test list comes from `ctest --show-only=json-v1`, so it is whatever CMake
registers — no second copy of the suite to keep in sync. `--filter` narrows it by
name (a regex), which is how a single suspicious test is hammered while the rest
of the machine stays quiet.

Result rows are also written as JSON to <build-dir>/load-stability.json, and a
later batch merges into it, so a proof across several calls is one record.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

# Two reporting styles live in the suite: the newer tests prefix every line with
# "[ 12.40s]", the older ones print plain lines and end with PASS. Both are
# stripped to the step text, so a slower machine is not a difference but a number
# a step printed that moved between repeats is.
TIMED_LINE = re.compile(r"^\[\s*[\d.]+s\]\s+(.*)$")
TIMING_ONLY = re.compile(r"^\[\s*[\d.]+s\]\s*$")
# The server's own log, which shares stdout: it carries the ephemeral port, so it
# must not be compared between repeats.
SERVER_LOG = re.compile(r"^\[(info|warn|warning|error|debug|trace)\s*\]")
# What a step line cannot be compared on: `tempfile.mkdtemp`'s random suffix and
# a port the kernel picked. Both are per-run identity, not evidence.
TMP_SUFFIX = re.compile(r"(plusmc-[a-z0-9-]+-)[A-Za-z0-9_]{6,12}")
EPHEMERAL = re.compile(r"listening on (?:127\.0\.0\.1:)?\d+")

# `<test>#<repeat>@<batch>` — the row key, so batches do not overwrite each other.
ROW_KEY = re.compile(r"@(\d+)$")





def normalize(text):
    """A step line with per-run identity removed."""
    return EPHEMERAL.sub("listening on <ephemeral port>",
                         TMP_SUFFIX.sub(r"\1<random>", text))


def wire_tests(build_dir):
    """Every registered test whose command drives a test script over a socket.

    Those are the ones that start a server, which is what a load-stability run
    multiplies; a native unit test is microseconds and would just be noise.
    """
    proc = subprocess.run(["ctest", "--test-dir", build_dir, "--show-only=json-v1"],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr.decode(errors="replace"))
        raise SystemExit(f"ctest --show-only failed in {build_dir}")
    tests = []
    for t in json.loads(proc.stdout.decode()).get("tests", []):
        cmd = [str(c) for c in t.get("command", [])]
        if any(c.endswith("_test.py") for c in cmd):
            tests.append((t["name"], cmd))
    return tests


def run_batch(batch, cwd, logdir, timeout, results):
    """Start every test in `batch` at once, then poll until all have exited.

    The times have to come from a poll loop rather than from waiting on each
    process in turn: waiting in start order stamps every test with the elapsed
    time of the slowest test started before it, which is how a 15-second `hosting`
    came out as 36 s — the length of `commands`, which was started first and so
    waited on first. Here each test's own exit is observed, and its timeout is
    counted from its own start.
    """
    pending = []
    for name, cmd in batch:
        path = os.path.join(logdir, name + ".log")
        handle = open(path, "wb")
        started = time.time()
        proc = subprocess.Popen(cmd, cwd=cwd, stdout=handle, stderr=subprocess.STDOUT)
        pending.append({"name": name, "proc": proc, "handle": handle, "started": started})
        print(f"    started {name}", flush=True)
    while pending:
        for entry in list(pending):
            rc = entry["proc"].poll()
            if rc is None and time.time() - entry["started"] > timeout:
                entry["proc"].kill()
                entry["proc"].wait()
                rc = "timeout"
            if rc is None:
                continue
            entry["handle"].close()
            results[entry["name"]] = {
                "rc": rc, "seconds": round(time.time() - entry["started"], 1)}
            pending.remove(entry)
        if pending:
            time.sleep(0.2)


def read_log(logfile):
    try:
        with open(logfile, "r", errors="replace") as fh:
            return fh.read().splitlines()
    except OSError:
        return []


def step_lines(logfile):
    """What the test itself reported, as the fingerprint of its run.

    Server logs (they carry an ephemeral port), the `[ 12.40s]` prefixes and
    traceback noise are dropped; everything else a test printed is kept in order,
    step lines and context lines alike, because a *count* that moves between
    repeats is how a step that ran in one run and skipped in another shows up.
    """
    out = []
    in_traceback = False
    for line in read_log(logfile):
        if line.startswith("Traceback (most recent call last)") \
                or line.startswith("Exception in thread"):
            in_traceback = True
            continue
        if in_traceback:
            # The frame lines are indented; the exception's own `Name: message`
            # line is not, and ends the block.
            if not line.strip() or line[:1] in (" ", "\t"):
                continue
            in_traceback = False
            continue
        if SERVER_LOG.match(line):
            continue
        m = TIMED_LINE.match(line)
        if m:
            out.append(normalize(m.group(1).rstrip()))
        elif TIMING_ONLY.match(line):
            continue
        elif line.strip():
            out.append(normalize(line.rstrip()))
    return out


def step_diffs(row):
    """How one test's repeats differ: None, or the list of step lines that moved.

    A moved *count* is reported first, and as a tuple, because it means something
    different from a moved value: a step that ran in some repeats and not others.
    """
    counts = {len(r["steps"]) for r in row}
    if len(counts) > 1:
        return ("count", row)
    diffs = []
    for step in range(len(row[0]["steps"])):
        values = {r["steps"][step] for r in row}
        if len(values) > 1:
            diffs.append(sorted(values))
    return diffs or None


def report(results, batch_no, batch, repeats, show_diffs):
    """The table and the step-line verdicts for one batch. Returns failures."""
    def rows(name):
        return [results[f"{name}#{r}@{batch_no}"] for r in range(1, repeats + 1)]

    verdicts = {name: step_diffs(rows(name)) for name, _cmd in batch}
    failed = 0

    print("  summary")
    for name, _cmd in batch:
        row = rows(name)
        marks = " ".join("ok" if r["rc"] == 0 else f"FAIL({r['rc']})" for r in row)
        secs = " ".join(f"{r['seconds']:6.1f}" for r in row)
        steps = " ".join(f"{len(r['steps']):>3}" for r in row)
        v = verdicts[name]
        if v is None:
            mark = "same"
        elif isinstance(v, tuple):
            mark = "STEP-COUNT"
        else:
            mark = f"moved {len(v)}"
        print(f"    {name:<20} {marks:<24} {secs}s  steps {steps:<14} {mark}")
        failed += sum(1 for r in row if r["rc"] != 0)

    if failed:
        print("\n  failures")
        for name, _cmd in batch:
            for r in rows(name):
                if r["rc"] == 0:
                    continue
                print(f"    {name} (repeat {r['repeat']}, rc={r['rc']}, {r['seconds']}s)")
                print(f"      {first_failure_line(read_log(r['log']))}")
                print(f"      log: {r['log']}")

    print("\n  step lines across repeats")
    if not any(verdicts.values()):
        print("    every test printed identical steps in every repeat")
    else:
        for name, _cmd in batch:
            v = verdicts[name]
            if v is None:
                continue
            row = rows(name)
            if isinstance(v, tuple):
                counts = [len(r["steps"]) for r in row]
                print(f"    {name:<20} DIFFERENT STEP COUNTS {counts} — a step ran "
                      f"in some repeats and not others")
                shown = {}
                for r in row:
                    for line in r["steps"]:
                        shown[line] = shown.get(line, 0) + 1
                for line, seen in shown.items():
                    if seen != len(row):
                        print(f"        {seen}x: {line[:150]}")
            else:
                print(f"    {name:<20} {len(v)} step line(s) moved between repeats")
                for values in v[:show_diffs]:
                    for value in values:
                        print(f"        {value[:150]}")
    return failed


def first_failure_line(lines):
    """The line that says why, for the summary table."""
    for line in lines:
        if line.startswith(("AssertionError", "Traceback", "Error", "FAIL")):
            return line[:160]
    for line in reversed(lines):
        if line.strip():
            return line.strip()[:160]
    return "(empty log)"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build_dir", help="the build directory, e.g. build")
    ap.add_argument("--repeats", type=int, default=3,
                    help="how many times to run the whole batch (default 3)")
    ap.add_argument("--filter", default=None, help="only tests whose name matches")
    ap.add_argument("--timeout", type=float, default=900.0,
                    help="per-test seconds before it is killed (default 900)")
    ap.add_argument("--keep-dir", default=None,
                    help="where the per-test logs go (default <build>/load-stability-logs)")
    ap.add_argument("--show-diffs", type=int, default=4,
                    help="how many differing step lines to print per test (default 4)")
    ap.add_argument("--summary", default=None,
                    help="JSON to write (default <build>/load-stability.json); rows from an "
                         "earlier batch in that file are kept, so several calls build one "
                         "record")
    ap.add_argument("--report-only", action="store_true",
                    help="print the table and verdicts for a batch already in the "
                         "summary file instead of running anything")
    ap.add_argument("--batch", type=int, default=None,
                    help="which batch to report (default: the newest, or the one "
                         "about to run)")
    ap.add_argument("--quiet", action="store_true", help="only the summary")
    args = ap.parse_args()

    build_dir = os.path.abspath(args.build_dir)
    batch = wire_tests(build_dir)
    if args.filter:
        rx = re.compile(args.filter)
        batch = [(n, c) for n, c in batch if rx.search(n)]
    if not batch:
        raise SystemExit("no wire tests matched")

    logdir = args.keep_dir or os.path.join(build_dir, "load-stability-logs")
    os.makedirs(logdir, exist_ok=True)
    cwd = os.path.dirname(os.path.abspath(__file__))

    summary_path = args.summary or os.path.join(build_dir, "load-stability.json")
    # Rows already on disk are kept and rewritten: one file is the record of every
    # batch run against this build. Each row is keyed `<test>#<repeat>@<batch>`,
    # so a later batch supersedes only the rows it re-runs and a batch's repeats
    # stay distinguishable from the batch before it.
    results = {}
    if os.path.exists(summary_path):
        try:
            with open(summary_path) as fh:
                results = json.load(fh).get("results", {})
        except (OSError, ValueError):
            results = {}
        # Rows written before the normaliser existed kept a run's temp dir in
        # their step lines. Re-normalising on load is idempotent, so a batch's
        # record stays comparable whichever version wrote it.
        for row in results.values():
            row["steps"] = [normalize(s) for s in row.get("steps", [])]
    batches = [int(m.group(1)) for k in results for m in [ROW_KEY.search(k)] if m]
    if args.report_only:
        batch_no = args.batch or (max(batches) if batches else 1)
        names = sorted({k.split("#")[0] for k in results
                        if k.endswith(f"@{batch_no}")})
        batch = [(name, None) for name in names]
        if not batch:
            raise SystemExit(f"no rows for batch {batch_no} in {summary_path}")
        print(f"load stability: batch {batch_no} from {summary_path}")
        report(results, batch_no, batch, args.repeats, args.show_diffs)
        return 0
    batch_no = args.batch or ((max(batches) if batches else 0) + 1)

    print(f"load stability: {len(batch)} test(s) at once, {args.repeats} repeat(s)"
          f" (batch {batch_no})")
    print(f"  logs: {logdir}")
    failures = []
    for repeat in range(1, args.repeats + 1):
        print(f"  repeat {repeat}/{args.repeats}", flush=True)
        # A batch gets its own directory: batches are separate runs of the same
        # suite, and a later one must not overwrite the earlier one's logs.
        run_dir = os.path.join(logdir, f"batch{batch_no}", f"repeat{repeat}")
        os.makedirs(run_dir, exist_ok=True)
        this = {}
        batch_started = time.time()
        run_batch(batch, cwd, run_dir, args.timeout, this)
        wall = round(time.time() - batch_started, 1)
        for name, row in this.items():
            row["repeat"] = repeat
            row["log"] = os.path.join(run_dir, name + ".log")
            row["steps"] = step_lines(row["log"])
            results[f"{name}#{repeat}@{batch_no}"] = row
            if row["rc"] != 0:
                failures.append((name, repeat, row))
        # Checkpoint after every repeat: a batch of this size is minutes long, and
        # a run that is interrupted halfway should still leave the repeats that
        # did finish on disk rather than nothing at all.
        with open(summary_path, "w") as fh:
            json.dump({"tests": len(batch), "repeats": args.repeats,
                       "batch": batch_no, "batch_repeats": args.repeats,
                       "batch_wall_seconds": wall, "results": results},
                      fh, indent=2, sort_keys=True)
        print(f"  repeat {repeat} done ({len(this)} run(s), "
              f"{sum(1 for r in this.values() if r['rc'] != 0)} failed, "
              f"{wall}s wall — the slowest test)", flush=True)

    failed = report(results, batch_no, batch, args.repeats, args.show_diffs)
    print(f"\n  {failed} failure(s) in {len(batch) * args.repeats} run(s); "
          f"results in {summary_path}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
