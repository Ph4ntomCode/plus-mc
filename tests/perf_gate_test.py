#!/usr/bin/env python3
"""Phase 5 (ordered item 5) — perf as a CI gate: the idle server's own numbers.

`docs/PHASE5.md` §5 recorded why this did not exist yet:

    **The harness is not a ctest.** Deliberately, for now: a load test that
    shares the machine with the suite is a flake generator, and the CI gate
    should be designed with the region-parallel sim (item 3) rather than before
    it. Until then the numbers live in this file and in `build/soak-*.json`.

Item 3 landed (`src/sim/region_scheduler.{h,cpp}`, ctest `region-scheduler`), so
this is that gate. It is deliberately the *idle* half of the perf story: the
loaded curve (60/100 movers, latency percentiles, the per-chunk memory ladder)
needs the bot driver and a quiet box, and stays `bench/soak.py`'s job. What a
shared CI runner can hold is the accounting itself — the server starts inside
its budget, an idle tick costs what it costs, boot memory stays bounded — and,
because the floors are *read from a checked-in record* instead of hard-coded,
tightening or loosening one is a reviewed change to that record rather than a
silent edit to a threshold inside a test.

What is asserted (every floor comes from `bench/baseline/…-perf-gate-idle.json`;
a missing or malformed record fails the gate loudly rather than skipping it):

  A. **startup.** Spawn to the `listening on` line, against the plan's < 5 s
     (`docs/PLAN.md` §8: "startup < 5 s").
  B. **the tick cost.** Every reported window's `work_avg_ms` under the
     ceiling. §8's target is "mspt never sustained above ~45 ms"; the gate's
     ceiling is a regression detector well below that, not a restatement of it.
  C. **the rate.** At least `tps_ok_fraction` of the windows at or above
     `tps_min`. A loaded runner legitimately loses a window; it should not lose
     half of them, and an idle server on a healthy box holds 20.0 TPS.
  D. **boot memory.** The largest RSS any window reports, under the ceiling.
  E. **it is the accounting.** Every window carries the metrics export's keys,
     the run reports at least `WINDOWS` loaded-window records, no client ever
     joined, and the process exits 0.

Printed, never gated: the peak work per tick, the resync count and the ambient
load average. A busy box spikes those legitimately, and a gate that fails on
them is the flake generator §5 warned about.

The server runs with an *absent* data root on purpose, exactly like
`tests/metrics_file_test.py`: CI does not extract the Mojang data before the
suite (`.github/workflows/ci.yml`), so a gate that needed it would be a gate
that never runs. The record says so, and the measured reference it carries is
the same shape of run.

Run:  perf_gate_test.py /path/to/plusmc [--record PATH]

`--record PATH` is how the baseline record is produced and re-cut: it runs the
same measurement, prints the same table, and writes the record with the floors
below plus the run's reference numbers. CI never passes it. The environment
override `PLUSMC_PERF_GATE_BASELINE` is the test's own red-check hook (point it
at a copy of the record with a mutated floor and watch the gate name the row);
CI leaves it unset.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import wire_client  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
BASELINE_REL = os.path.join("bench", "baseline", "2026-10-15-i5-4570-perf-gate-idle.json")
BASELINE_PATH = os.path.normpath(os.path.join(HERE, "..", BASELINE_REL))
OVERRIDE_ENV = "PLUSMC_PERF_GATE_BASELINE"
STEP_TIMEOUT = 30.0
# Windows the run must report before it stops. One second each (the export's
# implied cadence), so the loaded window is ~6 s on top of the spawn.
WINDOWS = 6
# The floors. They are written into the record by `--record` and read back by
# every gated run; the values are the plan's own target (startup) plus
# deliberately generous regression ceilings (the measured idle reference is
# ~0.05 ms/tick and ~5 MB RSS, against 10 ms and 256 MiB).
FLOORS = {
    "startup_s_max": 5.0,
    "work_avg_ms_max": 10.0,
    "tps_min": 15.0,
    "tps_ok_fraction": 0.8,
    "rss_bytes_max": 268435456,
}
# The export's fields the gate reads (docs/metrics.schema.json is the published
# contract; the gate checks the keys it needs rather than an exact key set, so a
# future additive field does not fail it).
KEYS = ["tps", "work_avg_ms", "work_peak_ms", "resync", "players", "rss_bytes", "uptime_s", "ticks"]
LISTENING_RE = re.compile(r"listening on .*:(\d+)")
FAILURES = []


def fail(message):
    FAILURES.append(message)
    print(f"FAIL: {message}", file=sys.stderr, flush=True)


def read_log(path):
    with open(path, "rb") as fh:
        return fh.read().decode("utf-8", "replace")


def load_record(path):
    """The baseline record's floors. A missing or malformed record is a failure,
    not a skip: the gate's whole argument is that the thresholds are visible."""
    if not os.path.isfile(path):
        raise AssertionError(
            f"the perf-gate baseline record is missing: {path}\n"
            f"  it is checked in; re-cut it with `perf_gate_test.py <binary> --record {path}`")
    with open(path, "r", encoding="utf-8") as fh:
        record = json.load(fh)
    floors = record.get("floors")
    if not isinstance(floors, dict) or set(floors) != set(FLOORS):
        raise AssertionError(
            f"{path}: `floors` must carry exactly {sorted(FLOORS)}, got "
            f"{sorted(floors) if isinstance(floors, dict) else floors!r}")
    return floors, record


def measurement(binary):
    """One idle run: `(result dict)` with the startup time, every window, the
    process's exit status and the server log."""
    root = tempfile.mkdtemp(prefix="plusmc-perf-gate-")
    metrics_path = os.path.join(root, "metrics.jsonl")
    log_path = os.path.join(root, "server.log")
    data_root = os.path.join(root, "vanilla-absent")  # box- and data-independent
    env = dict(os.environ)
    env["PLUSMC_AUTOSAVE_SECS"] = "0"  # an idle server should not save on a cadence
    args = [binary, "--host", "127.0.0.1", "--port", "0", "--motd", "perf gate",
            "--offline-mode", "--data", data_root, "--metrics-file", metrics_path]
    try:
        started = time.monotonic()
        with open(log_path, "wb") as log:
            proc = wire_client.start_process(args, stdout=log, stderr=subprocess.STDOUT,
                                    env=wire_client.server_env(env))
        startup_s = None
        deadline = time.monotonic() + STEP_TIMEOUT
        while time.monotonic() < deadline:
            match = LISTENING_RE.search(read_log(log_path))
            if match:
                startup_s = time.monotonic() - started
                # The line is written before accept() is called, so also prove
                # the port answers: the gate is about a serving server.
                port = int(match.group(1))
                for _ in range(50):
                    try:
                        probe = socket.create_connection(("127.0.0.1", port), timeout=1)
                        probe.close()
                        break
                    except OSError:
                        time.sleep(0.05)
                break
            if proc.poll() is not None:
                raise AssertionError(
                    f"the server exited before listening: {read_log(log_path)[-800:]}")
            time.sleep(0.02)
        if startup_s is None:
            raise AssertionError(
                f"the server did not listen within {STEP_TIMEOUT:g} s: {read_log(log_path)[-800:]}")

        def windows():
            if not os.path.isfile(metrics_path):
                return []
            with open(metrics_path, "r", encoding="utf-8") as fh:
                return [json.loads(line) for line in fh if line.strip()]

        deadline = time.monotonic() + STEP_TIMEOUT
        while time.monotonic() < deadline and len(windows()) < WINDOWS + 1:
            if proc.poll() is not None:
                break
            time.sleep(0.1)
        # Read the windows *before* the stop: the shutdown report appends the
        # partial window in flight, which is a different shape of window and is
        # deliberately not part of the gate's sample.
        loaded = windows()
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        return {"startup_s": startup_s, "windows": loaded, "rc": proc.returncode,
                "log": read_log(log_path), "binary": binary, "args": args, "root": root}
    finally:
        shutil.rmtree(root, ignore_errors=True)


def check(result, floors, source):
    """The floors, applied to one run. Returns the derived summary."""
    del source  # only used in messages by the caller
    windows = result["windows"]
    if len(windows) < WINDOWS:
        fail(f"only {len(windows)} window(s) were reported; the run needs {WINDOWS}")
    if result["rc"] != 0:
        fail(f"the server exited with status {result['rc']}, not 0")
    for i, rec in enumerate(windows):
        missing = [k for k in KEYS if k not in rec]
        if missing:
            fail(f"window {i} is missing {missing}: {sorted(rec)}")
            continue
        if rec["players"] != 0:
            fail(f"window {i} has {rec['players']} player(s); the gate is an idle server")

    if result["startup_s"] > floors["startup_s_max"]:
        fail(f"startup {result['startup_s']:.3f} s exceeds the floor "
             f"{floors['startup_s_max']:g} s (PLAN §8: startup < 5 s)")
    if windows:
        worst_work = max(rec.get("work_avg_ms", 0.0) for rec in windows)
        if worst_work > floors["work_avg_ms_max"]:
            worst = max(windows, key=lambda r: r.get("work_avg_ms", 0.0))
            fail(f"work_avg_ms {worst_work:.3f} exceeds the floor "
                 f"{floors['work_avg_ms_max']:g} ms (window uptime_s={worst.get('uptime_s')})")
        peak_rss = max(rec.get("rss_bytes", 0) for rec in windows)
        if peak_rss > floors["rss_bytes_max"]:
            fail(f"rss_bytes {peak_rss} ({peak_rss / 1048576:.1f} MiB) exceeds the floor "
                 f"{floors['rss_bytes_max']} ({floors['rss_bytes_max'] / 1048576:.0f} MiB)")
        ok_tps = sum(1 for rec in windows if rec.get("tps", 0.0) >= floors["tps_min"])
        need = int(floors["tps_ok_fraction"] * len(windows) + 0.9999)
        if ok_tps < need:
            fail(f"{ok_tps} of {len(windows)} windows reached {floors['tps_min']:g} TPS; "
                 f"the floor keeps {need}")
        summary = {
            "startup_s": result["startup_s"],
            "windows": len(windows),
            "work_avg_ms_max": worst_work,
            "work_peak_ms_max": max(rec.get("work_peak_ms", 0.0) for rec in windows),
            "tps_min": min(rec.get("tps", 0.0) for rec in windows),
            "tps_mean": sum(rec.get("tps", 0.0) for rec in windows) / len(windows),
            "tps_windows_ok": ok_tps,
            "rss_bytes_max": peak_rss,
            "resync_total": sum(int(rec.get("resync", 0)) for rec in windows),
        }
    else:
        summary = {"startup_s": result["startup_s"], "windows": 0}
    print(f"  startup {result['startup_s']:.3f} s  |  "
          f"work avg/peak {summary.get('work_avg_ms_max', 0):.3f}/"
          f"{summary.get('work_peak_ms_max', 0):.3f} ms  |  "
          f"tps min/mean {summary.get('tps_min', 0):.2f}/"
          f"{summary.get('tps_mean', 0):.2f}  |  "
          f"rss {summary.get('rss_bytes_max', 0) / 1048576:.1f} MiB  |  "
          f"resync {summary.get('resync_total', 0)}", flush=True)
    return summary


def load_average():
    try:
        return round(os.getloadavg()[0], 2)
    except (AttributeError, OSError):
        return None


def binary_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def write_record(path, result, summary, floors):
    # The argv is recorded for reproducibility, but the run's own temp tree is
    # gone the moment the test exits, so it is written as a placeholder rather
    # than as a path that will never exist again (docs/PHASE5.md §5 records the
    # same convention for the bench records).
    argv = [arg.replace(result["root"], "<run-tmp>") for arg in result["args"]]
    record = {
        "name": "perf-gate-idle",
        "kind": "perf-gate-baseline",
        "machine": "i5-4570 (4 cores, 3.2 GHz), 24 GB RAM, macOS 11.7.11",
        "binary_sha256": binary_sha256(result["binary"]),
        "argv": argv,
        "note": ("the Phase-5 item 5 CI gate's own baseline: one idle server, no vanilla data, "
                 "no bots, void world, one-second metrics windows. The floors are the gate's "
                 "thresholds (read from this record on every run); the reference is what this "
                 "run measured."),
        "load_avg_1m": load_average(),
        "floors": floors,
        "reference": summary,
    }
    directory = os.path.dirname(os.path.abspath(path))
    os.makedirs(directory, exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(record, fh, indent=1, sort_keys=True)
        fh.write("\n")
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("binary", help="path to the plusmc binary under test")
    parser.add_argument("--record", metavar="PATH",
                        help="measure, then write the baseline record to PATH (the gate's "
                             "re-cut path; CI never passes it)")
    args = parser.parse_args()

    binary = os.path.realpath(args.binary)
    if not os.path.isfile(binary):
        print(f"FAIL: no such binary: {binary}", file=sys.stderr)
        return 1

    source = os.environ.get(OVERRIDE_ENV) or BASELINE_PATH
    if args.record:
        floors = dict(FLOORS)
        source = args.record
    else:
        floors, _record = load_record(source)
    print(f"perf gate: {binary}\n  floors {source}", flush=True)

    result = measurement(binary)
    summary = check(result, floors, source)

    if args.record:
        # A record is only written from a run that passes its own floors, so a
        # broken measurement is never frozen into the baseline.
        if FAILURES:
            print("FAIL: refusing to write a baseline record from a failing run",
                  file=sys.stderr)
            return 1
        record = write_record(args.record, result, summary, floors)
        print(f"wrote {args.record} (binary sha256 {record['binary_sha256'][:16]}…)", flush=True)

    if FAILURES:
        print(f"\nFAIL: perf gate — {len(FAILURES)} floor(s) violated "
              f"(floors from {source})", file=sys.stderr)
        return 1
    print(f"\nPASS: perf gate — idle startup, tick cost, rate and boot memory inside "
          f"{source}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
