#!/usr/bin/env python3
"""Phase 5 (ordered item 0) — the *loaded* half of the perf gate.

`tests/perf_gate_test.py` holds an idle server against startup, tick cost, rate
and boot memory. It exists because those are the numbers a shared CI runner can
hold. It is also, by construction, blind to the failure that mattered most: a
crowded server that stays at 20 TPS standing still and collapses when its
players walk around together. That regression lives in the relay fan-out (the
write path and the chunk stream), and an idle run never touches it.

This is that arm. Sixteen real protocol clients join one flat world, cluster on
the same spot (the worst case for fan-out), and keep walking for a fixed window
while the server's own metrics export is sampled. What is asserted:

  A. **the run happened.** All sixteen bots joined, none failed, the server
     exited 0, and at least `windows_min` loaded windows were reported.
  B. **the rate.** At least `tps_ok_fraction` of the loaded windows are at or
     above `tps_min`. This is the floor that a saturated sim breaks first.
  C. **the tick cost.** Every loaded window's `work_avg_ms` is under
     `work_avg_ms_max`. The default is PLAN §8's own "mspt never sustained
     above ~45 ms" — the only absolute ceiling that is honest without a quiet
     box to calibrate a tighter one on.
  D. **the sharing.** At shutdown the server reports how many chunk sends were
     served from bytes another connection already built; at least `reuse_min`
     of them must be, and at least `frame_share_min` of them must be *frames*
     (the compressed bytes, written verbatim) rather than bodies. This is the
     *load-independent* half of the write path: it is a ratio of sends, not a
     time, so ambient load cannot move it, and the two floors answer different
     regressions. The total catches a cache that stopped serving; the frame
     share catches the one that matters — compression back in the recipient
     loop. With `PLUSMC_CHUNK_FRAME_CACHE=0` the total stays high (89.6 % in the
     red check below, because a body hit takes over) while the frame share is
     exactly zero, so gating the total alone would miss it. That asymmetry is
     why both rows exist.

Printed, never gated: peak work per tick, the minimum TPS window, the ambient
load before and after, and the split between frame and body reuse. A busy box
moves those legitimately, and a gate that fails on them is a flake generator.

What this deliberately does **not** claim: a calibrated mover curve. Turning
"118.9 ms at 200 clustered movers" into a threshold needs a quiet box and a
baseline cut on it (`docs/WRITE_PATH.md` records the attempt on a box at load
10-79 and why it was not frozen into a floor). Until that exists, this gate
holds the *structure* of the write path and the plan's own sustained-mspt
ceiling, and prints the rest for a human.

The server needs the vanilla data root (`data/vanilla`, from
`tools/vanilla-data`): a client cannot join without the registry and client
config assets — an absent root accepts the socket and then refuses the login,
which is what the first version of this gate measured. The idle gate can run
without it because it never joins a client; this one cannot, so it **skips with
exit 77** when the data root is absent, and says so. `ctest` reports that as
skipped (`SKIP_RETURN_CODE`), and the skip is a missing input, not a green.

If the box cannot even carry sixteen bots, the gate fails — a server that will
not let sixteen clients in is a regression, and a gate that skipped it would be
swallowing the error it exists to catch. That includes a box whose ambient load
is above `JOIN_SKIP_LOAD`: the fleet's join window is 30 s of Python, so such a
run cannot make the claim, and it says so as a failure instead of exiting 77.
The only skip left is the missing data root above — a skip code now means a
missing input, not a loaded box.

Run:  perf_gate_loaded_test.py /path/to/plusmc [--record PATH]

`--record PATH` re-cuts the baseline record; CI never passes it. The environment
override `PLUSMC_LOADED_GATE_BASELINE` is the red-check hook (point it at a copy
with a mutated floor).
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

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(REPO, "bench"))
sys.path.insert(0, HERE)

import botnet  # noqa: E402
import wire_client  # noqa: E402  (start_process: graceful Ctrl-Break on Windows)

BASELINE_REL = os.path.join("bench", "baseline", "2026-10-17-i5-4570-perf-gate-loaded.json")
BASELINE_PATH = os.path.normpath(os.path.join(REPO, BASELINE_REL))
OVERRIDE_ENV = "PLUSMC_LOADED_GATE_BASELINE"
SKIP_RC = 77
HOST = "127.0.0.1"
LISTENING_RE = re.compile(r"listening on .*:(\d+)")
SHARING_RE = re.compile(
    r"chunk sharing: (\d+) sends, (\d+) bodies reused \(([\d.]+)%\), "
    r"(\d+) frames reused \(([\d.]+)%\)")
KEYS = ["tps", "work_avg_ms", "work_peak_ms", "resync", "players", "rss_bytes"]
BOTS = 16
LOADED_SECONDS = 12.0
JOIN_TIMEOUT = 30.0
# Only a box this far into overload may skip instead of failing on join failures
# (4 cores: 4x oversubscribed). Everything below it must carry sixteen bots.
JOIN_SKIP_LOAD = 16.0
# See the docstring: C is PLAN §8's sustained ceiling, D is the structural floor.
FLOORS = {
    "bots": BOTS,
    "windows_min": 8,
    "tps_min": 12.0,
    "tps_ok_fraction": 0.7,
    "work_avg_ms_max": 45.0,
    "reuse_min": 0.25,
    "frame_share_min": 0.40,
}
FAILURES = []


def fail(message):
    FAILURES.append(message)
    print(f"FAIL: {message}", file=sys.stderr, flush=True)


def skip(message):
    print(f"SKIP: {message}", file=sys.stderr, flush=True)
    sys.exit(SKIP_RC)


def read_text(path):
    try:
        with open(path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


def load_record(path):
    if not os.path.isfile(path):
        raise AssertionError(
            f"the loaded perf-gate baseline record is missing: {path}\n"
            f"  it is checked in; re-cut it with `perf_gate_loaded_test.py <binary> --record {path}`")
    with open(path, "r", encoding="utf-8") as fh:
        record = json.load(fh)
    floors = record.get("floors")
    if not isinstance(floors, dict) or set(floors) != set(FLOORS):
        raise AssertionError(
            f"{path}: `floors` must carry exactly {sorted(FLOORS)}, got "
            f"{sorted(floors) if isinstance(floors, dict) else floors!r}")
    return floors, record


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


def parse_sharing(log):
    match = None
    for match in SHARING_RE.finditer(log):
        pass
    if match is None:
        return None
    sends, bodies, body_pct, frames, frame_pct = match.groups()
    return {
        "sends": int(sends),
        "bodies": int(bodies),
        "body_pct": float(body_pct),
        "frames": int(frames),
        "frame_pct": float(frame_pct),
    }


def measurement(binary):
    """One loaded run. Returns the windows from the metric export, the server's
    own shutdown report, and the process status."""
    root = tempfile.mkdtemp(prefix="plusmc-loaded-gate-")
    metrics_path = os.path.join(root, "metrics.jsonl")
    log_path = os.path.join(root, "server.log")
    script_path = os.path.join(root, "bots.py")
    data_root = os.path.join(REPO, "data", "vanilla")
    # The bot driver runs in its own process: the gate's own sampling and this
    # harness's selector loop must not compete for the GIL while the server is
    # under test. It reports the fleet snapshot as JSON and exits.
    with open(script_path, "w", encoding="utf-8") as fh:
        fh.write(
            "import json, sys, time\n"
            "sys.path.insert(0, %r)\n"
            "import botnet\n"
            "port, count, seconds, out = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3]), sys.argv[4]\n"
            "fleet = botnet.Fleet(%r, port, count, spread=0.0)\n"
            "fleet.connect_all()\n"
            "deadline = time.monotonic() + %r\n"
            "while time.monotonic() < deadline:\n"
            "    joined = sum(1 for b in fleet.bots if b.joined_at is not None)\n"
            "    if joined + len(fleet.failures) >= count:\n"
            "        break\n"
            "    fleet.run(min(time.monotonic() + 0.05, deadline))\n"
            "end = time.monotonic() + seconds\n"
            "while time.monotonic() < end:\n"
            "    fleet.run(min(time.monotonic() + 0.05, end))\n"
            "snap = fleet.snapshot()\n"
            "fleet.close_all()\n"
            "json.dump(snap, open(out, 'w'))\n" %
            (os.path.join(REPO, "bench"), HOST, JOIN_TIMEOUT))
    bots_json = os.path.join(root, "bots.json")
    env = dict(os.environ)
    env["PLUSMC_AUTOSAVE_SECS"] = "0"
    env["PLUSMC_MOB_SPAWNING"] = "0"
    args = [binary, "--host", HOST, "--port", "0", "--motd", "loaded gate",
            "--offline-mode", "--data", data_root, "--metrics-file", metrics_path,
            "--view-distance", "4", "--max-players", "64"]
    proc = None
    bots = None
    load_before = load_average()
    try:
        with open(log_path, "wb") as log:
            proc = wire_client.start_process(args, stdout=log, stderr=subprocess.STDOUT, env=env)
        port = None
        deadline = time.monotonic() + JOIN_TIMEOUT
        while time.monotonic() < deadline:
            match = LISTENING_RE.search(read_text(log_path))
            if match:
                port = int(match.group(1))
                for _ in range(50):
                    try:
                        socket.create_connection((HOST, port), timeout=1).close()
                        break
                    except OSError:
                        time.sleep(0.05)
                break
            if proc.poll() is not None:
                raise AssertionError(
                    f"the server exited before listening: {read_text(log_path)[-800:]}")
            time.sleep(0.02)
        if port is None:
            raise AssertionError(
                f"the server did not listen within {JOIN_TIMEOUT:g} s: {read_text(log_path)[-800:]}")

        bot_proc = subprocess.run(
            [sys.executable, script_path, str(port), str(BOTS), str(LOADED_SECONDS), bots_json],
            capture_output=True, text=True, timeout=JOIN_TIMEOUT + LOADED_SECONDS + 30)
        if os.path.isfile(bots_json):
            with open(bots_json, "r", encoding="utf-8") as fh:
                bots = json.load(fh)
        if bot_proc.returncode != 0:
            fail(f"the bot driver exited {bot_proc.returncode}: "
                 f"{(bot_proc.stderr or bot_proc.stdout)[-800:]}")

        # Let the last window land, then stop the server so its shutdown report
        # is written (that report carries the sharing counters).
        time.sleep(1.5)
        windows = []
        if os.path.isfile(metrics_path):
            with open(metrics_path, "r", encoding="utf-8") as fh:
                windows = [json.loads(line) for line in fh if line.strip()]
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
        log = read_text(log_path)
        return {"windows": windows, "sharing": parse_sharing(log), "rc": proc.returncode,
                "log": log, "bots": bots, "load_before": load_before, "binary": binary,
                "args": args, "root": root}
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
        shutil.rmtree(root, ignore_errors=True)


def check(result, floors):
    bots = result["bots"] or {}
    if bots.get("connected") != floors["bots"] or bots.get("joined") != floors["bots"]:
        load_before = result.get("load_before")
        if load_before is not None and load_before > JOIN_SKIP_LOAD:
            # Not a skip: the data root was present, so this run had everything it
            # needed except a quiet box, and a skip would read as "nothing to see".
            fail(f"only {bots.get('joined')} of {floors['bots']} bots joined at ambient load "
                 f"{load_before:g} (> {JOIN_SKIP_LOAD:g}); the join window is a harness "
                 f"limit, and a loaded box cannot make this claim — re-run on a quieter one")
            return {"windows": 0, "joined": bots.get("joined")}
        fail(f"only {bots.get('joined')} of {floors['bots']} bots joined "
             f"({bots.get('connected')} connected, failures {bots.get('failures')}) at ambient "
             f"load {load_before}; sixteen clients must join")
        return {"windows": 0, "joined": bots.get("joined")}
    if bots.get("failures"):
        fail(f"bot failures: {bots['failures'][:3]}")
    if result["rc"] != 0:
        fail(f"the server exited with status {result['rc']}, not 0")

    loaded = [w for w in result["windows"] if w.get("players") == floors["bots"]]
    if len(loaded) < floors["windows_min"]:
        fail(f"only {len(loaded)} loaded window(s) were reported; the gate needs "
             f"{floors['windows_min']}")
    for i, rec in enumerate(loaded):
        missing = [k for k in KEYS if k not in rec]
        if missing:
            fail(f"loaded window {i} is missing {missing}")

    summary = {"windows": len(loaded)}
    if loaded:
        worst_work = max(rec.get("work_avg_ms", 0.0) for rec in loaded)
        worst = max(loaded, key=lambda r: r.get("work_avg_ms", 0.0))
        if worst_work > floors["work_avg_ms_max"]:
            fail(f"work_avg_ms {worst_work:.3f} exceeds the floor "
                 f"{floors['work_avg_ms_max']:g} ms under {floors['bots']} clustered movers "
                 f"(window uptime_s={worst.get('uptime_s')})")
        ok_tps = sum(1 for rec in loaded if rec.get("tps", 0.0) >= floors["tps_min"])
        need = int(floors["tps_ok_fraction"] * len(loaded) + 0.9999)
        if ok_tps < need:
            fail(f"{ok_tps} of {len(loaded)} loaded windows reached {floors['tps_min']:g} TPS; "
                 f"the floor keeps {need}")
        summary.update({
            "work_avg_ms_max": worst_work,
            "work_peak_ms_max": max(rec.get("work_peak_ms", 0.0) for rec in loaded),
            "tps_min": min(rec.get("tps", 0.0) for rec in loaded),
            "tps_mean": sum(rec.get("tps", 0.0) for rec in loaded) / len(loaded),
            "tps_windows_ok": ok_tps,
            "rss_bytes_max": max(rec.get("rss_bytes", 0) for rec in loaded),
            "resync_total": sum(int(rec.get("resync", 0)) for rec in loaded),
        })

    sharing = result["sharing"]
    if sharing is None:
        fail("the server's shutdown report has no 'chunk sharing' line; the gate cannot "
             "check that one compression still serves many connections")
    elif sharing["sends"] > 0:
        reuse = (sharing["bodies"] + sharing["frames"]) / sharing["sends"]
        if reuse < floors["reuse_min"]:
            fail(f"chunk reuse {reuse * 100:.1f}% ({sharing['bodies']} bodies + "
                 f"{sharing['frames']} frames of {sharing['sends']} sends) is below the "
                 f"floor {floors['reuse_min'] * 100:.0f}%")
        frame_share = sharing["frame_pct"] / 100.0
        if frame_share < floors["frame_share_min"]:
            fail(f"only {sharing['frame_pct']:.1f}% of chunk sends were served from a shared "
                 f"compressed frame; the floor is {floors['frame_share_min'] * 100:.0f}% "
                 f"(compression is happening per recipient again)")
        summary.update({"chunk_sends": sharing["sends"], "reuse": round(reuse, 4),
                        "frame_pct": sharing["frame_pct"], "body_pct": sharing["body_pct"]})

    print(f"  bots {floors['bots']} clustered movers  |  "
          f"work avg/peak {summary.get('work_avg_ms_max', 0):.2f}/"
          f"{summary.get('work_peak_ms_max', 0):.2f} ms  |  "
          f"tps min/mean {summary.get('tps_min', 0):.2f}/{summary.get('tps_mean', 0):.2f}  |  "
          f"chunk reuse {summary.get('reuse', 0) * 100:.1f}% "
          f"(frames {summary.get('frame_pct', 0):.1f}% of sends)  |  "
          f"rss {summary.get('rss_bytes_max', 0) / 1048576:.1f} MiB  |  "
          f"load {result.get('load_before')} -> {load_average()}", flush=True)
    return summary


def write_record(path, result, summary, floors):
    record = {
        "name": "perf-gate-loaded",
        "kind": "perf-gate-baseline",
        "machine": "i5-4570 (4 cores, 3.2 GHz), 24 GB RAM, macOS 11.7.11",
        "binary_sha256": binary_sha256(result["binary"]),
        "note": ("the loaded half of the Phase-5 CI gate: 16 clustered movers on one flat "
                 "server, absent vanilla data, a 12 s window. The floors are the gate's "
                 "thresholds; C is PLAN section 8's sustained-mspt ceiling and D is the "
                 "load-independent sharing ratio. The reference was measured on a shared "
                 "box and is recorded for shape, not as a calibration."),
        "load_avg_1m": load_average(),
        "floors": floors,
        "reference": summary,
    }
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(record, fh, indent=1, sort_keys=True)
        fh.write("\n")
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("binary", help="path to the plusmc binary under test")
    parser.add_argument("--record", metavar="PATH",
                        help="measure, then write the baseline record to PATH")
    args = parser.parse_args()

    binary = os.path.realpath(args.binary)
    if not os.path.isfile(binary):
        print(f"FAIL: no such binary: {binary}", file=sys.stderr)
        return 1

    # The server resolves the newest extract under this root (`--data`'s help:
    # "newest version.json wins"), and a client cannot join without the
    # `clientconfig` assets inside that extract.
    vanilla_root = os.path.join(REPO, "data", "vanilla")
    has_config = any(
        os.path.isdir(os.path.join(vanilla_root, name, "clientconfig"))
        for name in (os.listdir(vanilla_root) if os.path.isdir(vanilla_root) else []))
    if not has_config:
        skip(f"no extracted vanilla data with client assets under {vanilla_root}; "
             f"a client cannot join without it (run tools/vanilla-data). "
             f"This gate is skipped, not passed.")

    source = os.environ.get(OVERRIDE_ENV) or BASELINE_PATH
    if args.record:
        floors = dict(FLOORS)
        source = args.record
    else:
        floors, _record = load_record(source)
    print(f"loaded perf gate: {binary}\n  floors {source}", flush=True)

    result = measurement(binary)
    summary = check(result, floors)

    if args.record:
        if FAILURES:
            print("FAIL: refusing to write a baseline record from a failing run",
                  file=sys.stderr)
            return 1
        record = write_record(args.record, result, summary, floors)
        print(f"wrote {args.record} (binary sha256 {record['binary_sha256'][:16]}…)",
              flush=True)

    if FAILURES:
        print(f"\nFAIL: loaded perf gate — {len(FAILURES)} floor(s) violated "
              f"(floors from {source})", file=sys.stderr)
        return 1
    print(f"\nPASS: loaded perf gate — clustered movers inside {source}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
