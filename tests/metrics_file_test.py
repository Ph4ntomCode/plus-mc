#!/usr/bin/env python3
"""Phase 5 (ops) — `--metrics-file`: the metrics export a host can tail.

`docs/PHASE5.md` §6 item 8 leaves two things to the ops item: config validation
(`tests/config_validate_test.py`) and "a metrics export (the line already
exists; this is about a consumer)". This is the consumer: each reported window is
also appended to the file as one JSON object per line.

The contract the test pins, and why each half of it matters:

  A. **a run shorter than one interval still exports.** The window in flight is
     reported on the way out (the same shutdown report the stderr line already
     did), so a host that starts and stops a server gets a record rather than an
     empty file.
  B. **append, not truncate.** Restarting against the same path adds records and
     leaves the earlier ones byte-identical — that is what makes the file
     tail-able across restarts.
  C. **the file and the line agree.** Every record is the same numbers as the
     `metrics: tps=...` line for the same window: identical field names,
     identical rounding. One window, two views; a forwarder's numbers and a
     `bench/soak.py` run's numbers must be comparable without a conversion
     table. The strongest form of the check is that the two *counts* match too.
  D. **the implied cadence.** `--metrics-file` with no `--metrics-interval`
     writes at the documented one-second window; consecutive records are one
     second apart (with the rounding and scheduling slack two windows can be).
  E. **the refusals.** A path whose directory does not exist, and a path that is
     a directory, fail the start rather than silently exporting nothing.
  F. **the published schema.** The records carry a format version (`v`, the
     first key) and `tests/fixtures/metrics.schema.json` describes exactly the keys that
     are written, with all of them required — so a consumer that validates a
     line is validating the format this server actually emits. The check is a
     key-set comparison rather than a validator dependency: the schema's
     `properties` and `required` must both be the record's own key set, and the
     version constant must be the one the schema names.

     The four `world_*` fields arrived after `v` was published and are the
     documented kind of addition: a consumer that ignores unknown keys keeps
     working, so `v` does not move. Three are the in-memory store's own gauges
     (`WorldInterface::resident_columns`/`resident_bytes`/
     `columns_with_writes`), not an estimate: `rss_bytes` says the process is
     2 GB, `world_bytes` says whether that is a streamed view set, world state
     the store may not drop, or a leak. The fourth, `world_chunks_stored`, is
     cumulative (`columns_stored`): a budget that evicts holds the gauges at
     their ceiling while the store keeps building, so the fill's throughput has
     to be read from this one. A disk-backed world reports zeros.

Run: metrics_file_test.py /path/to/plusmc
"""
import json
import os
import re
import shutil
import subprocess
import socket
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import wire_client  # noqa: E402

STEP_TIMEOUT = 30.0
# The JSON object's fields, and the line's, in one place: the parity check
# compares them key by key, so a renamed field fails here rather than drifting.
KEYS = ["tps", "work_avg_ms", "work_peak_ms", "busy_pct", "tick_pct", "resync", "players",
        "rss_bytes", "uptime_s", "ticks", "world_chunks", "world_bytes",
        "world_written_chunks", "world_chunks_stored"]
# The file's own two fields: the wall clock, and the format version
# (mc::metrics::kJsonFormatVersion). Bumping VERSION is a deliberate format
# change; the schema below has to move with it.
FILE_KEYS = ["v", "ts_unix"]
VERSION = 1
SCHEMA_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures",
                           "metrics.schema.json")
LINE_RE = re.compile(r"metrics: (\w+=[-\d.]+(?: \w+=[-\d.]+)*)")
KV_RE = re.compile(r"(\w+)=([-\d.]+)")


def read_log(path):
    with open(path, "rb") as fh:
        return fh.read().decode("utf-8", "replace")


def records(path):
    """The file's lines as parsed JSON objects."""
    if not os.path.isfile(path):
        return []
    with open(path, "r", encoding="utf-8") as fh:
        return [json.loads(line) for line in fh if line.strip()]


def metric_lines(log):
    """Every `metrics: ...` line in a server log, as key -> value dicts."""
    out = []
    for match in LINE_RE.finditer(log):
        parsed = {k: float(v) for k, v in KV_RE.findall(match.group(1))}
        assert len(parsed) == len(KEYS), f"the line must carry all keys: {match.group(0)}"
        out.append(parsed)
    return out


def server_args(binary, data_root, extra=()):
    return [binary, "--host", "127.0.0.1", "--port", "0", "--motd", "metrics test",
            "--offline-mode", "--data", data_root] + list(extra)


def wait_for_port(log_path, proc, timeout=STEP_TIMEOUT):
    deadline = time.time() + timeout
    while time.time() < deadline:
        m = re.search(r"listening on .*:(\d+)", read_log(log_path))
        if m:
            port = int(m.group(1))
            for _ in range(30):
                try:
                    sock = socket.create_connection(("127.0.0.1", port), timeout=1)
                    sock.close()
                    return port
                except OSError:
                    time.sleep(0.1)
        if proc.poll() is not None:
            raise AssertionError(f"the server exited before listening: {read_log(log_path)[-800:]}")
        time.sleep(0.05)
    raise AssertionError(f"the server did not listen in {timeout:g} s: {read_log(log_path)[-800:]}")


def start(binary, data_root, extra, log_path, env=None):
    """`(proc, port)`, with the whole log kept in a file this test can read."""
    with open(log_path, "wb") as log:
        proc = wire_client.start_process(server_args(binary, data_root, extra), stdout=log,
                                stderr=subprocess.STDOUT, env=wire_client.server_env(env))
    return proc, wait_for_port(log_path, proc)


def stop(proc):
    proc.terminate()
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def wait_for_records(path, count, timeout=STEP_TIMEOUT):
    deadline = time.time() + timeout
    while time.time() < deadline:
        found = records(path)
        if len(found) >= count:
            return found
        time.sleep(0.1)
    raise AssertionError(f"only {len(records(path))} record(s) appeared in {timeout:g} s")


def run_expecting_failure(binary, data_root, extra):
    """A start that must be refused before the listener opens."""
    fd, log_path = tempfile.mkstemp(prefix="plusmc-metrics-log-", suffix=".txt")
    os.close(fd)
    with open(log_path, "wb") as log:
        proc = wire_client.start_process(server_args(binary, data_root, extra), stdout=log,
                                stderr=subprocess.STDOUT, env=wire_client.server_env(None))
    try:
        code = proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
        raise AssertionError(f"the server started despite {extra}")
    return code, read_log(log_path)


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1])
    root = tempfile.mkdtemp(prefix="plusmc-metrics-")
    # A data root with no vanilla data: this test is about the reporting path,
    # so it keeps itself independent of whatever was extracted on the box.
    data_root = os.path.join(root, "vanilla-absent")
    path = os.path.join(root, "metrics.jsonl")
    env = dict(os.environ)
    env["PLUSMC_AUTOSAVE_SECS"] = "0"
    try:
        # --- A. a run shorter than one window still exports --------------------
        proc, _port = start(binary, data_root, ["--metrics-file", path],
                            os.path.join(root, "short.log"), env=env)
        stop(proc)
        short = records(path)
        assert len(short) >= 1, "the shutdown report must write the window in flight"
        assert all(isinstance(r, dict) for r in short)
        print(f"[1] a sub-interval run exported {len(short)} record(s)", flush=True)

        # --- B/C/D. append, cadence, and parity with the stderr line ----------
        first_len = len(short)
        with open(path, "rb") as fh:
            prefix = fh.read()
        proc, _port = start(binary, data_root, ["--metrics-file", path],
                            os.path.join(root, "long.log"), env=env)
        try:
            wait_for_records(path, first_len + 3)
        finally:
            stop(proc)
        # Read the file *after* the stop: the shutdown report appends the window
        # in flight, and that last window is the one the last log line matches.
        grown = records(path)
        log = read_log(os.path.join(root, "long.log"))
        with open(path, "rb") as fh:
            whole = fh.read()
        assert whole.startswith(prefix), "a restart must append, not truncate the file"
        assert len(grown) >= first_len + 3, f"expected new records, got {len(grown)}"

        # The second run's log holds the second run's lines; grown holds both
        # runs' records, so the counts line up only after subtracting the first
        # run's — which is itself the point (the file kept them, the log did not).
        lines = metric_lines(log)
        assert len(grown) - first_len == len(lines), \
            f"one line per record: {len(lines)} line(s) for {len(grown) - first_len} new record(s)"

        for i, rec in enumerate(grown):
            assert set(rec) == set(KEYS) | set(FILE_KEYS), f"record {i} keys: {sorted(rec)}"
            assert rec["v"] == VERSION, f"record {i} format version: {rec['v']!r}"
            assert isinstance(rec["ts_unix"], int) and rec["ts_unix"] > 1_600_000_000, \
                f"record {i} needs a wall-clock stamp: {rec['ts_unix']}"
            for key in ("resync", "players", "rss_bytes", "ticks"):
                assert isinstance(rec[key], int) and not isinstance(rec[key], bool), \
                    f"record {i} {key} must be an integer"
            for key in ("tps", "work_avg_ms", "work_peak_ms", "busy_pct", "tick_pct",
                        "uptime_s"):
                # uptime_s prints as `%.0f`, so an early window is "15" and JSON
                # reads it as an int; every field just has to be a number.
                assert isinstance(rec[key], (int, float)) and not isinstance(rec[key], bool), \
                    f"record {i} {key} must be a number"
            assert 0 < rec["tps"] <= 100, f"record {i} tps {rec['tps']} is not a tick rate"
            assert rec["players"] == 0, "no client joined this test's servers"
            assert rec["rss_bytes"] > 0, f"record {i} must carry the process's RSS"

        # The windows are ordered and advancing, and the implied interval is the
        # documented one second (rounded: two records can be 1 or 2 s apart).
        for i in range(1, len(grown)):
            assert grown[i]["ticks"] >= grown[i - 1]["ticks"], "ticks must not go backwards"
            assert grown[i]["uptime_s"] >= grown[i - 1]["uptime_s"], \
                "uptime_s must not go backwards"
            assert grown[i]["ts_unix"] >= grown[i - 1]["ts_unix"], \
                "the wall-clock stamp must not go backwards"
            assert grown[i]["tps"] > 0 and grown[i]["uptime_s"] - grown[i - 1]["uptime_s"] <= 3, \
                f"records {i - 1}..{i} are more than three seconds apart"
        assert grown[-1]["ticks"] > grown[0]["ticks"], "a multi-second run must tick"

        # The parity check: the last record and the last line describe one window,
        # to the precision each prints.
        tail = grown[-1]
        line = lines[-1]
        for key in KEYS:
            if key in ("resync", "players", "rss_bytes", "ticks"):
                assert tail[key] == int(line[key]), \
                    f"{key}: file {tail[key]} against line {line[key]}"
            elif key == "uptime_s":
                assert abs(tail[key] - line[key]) <= 0.5, f"{key}: {tail[key]} vs {line[key]}"
            else:
                assert abs(tail[key] - line[key]) < 0.001, f"{key}: {tail[key]} vs {line[key]}"
        summary = ' '.join(f'{k}={tail[k]}' for k in ('tps', 'ticks', 'uptime_s'))
        print(f"[2] append + parity: {len(grown)} record(s), {len(lines)} line(s), "
              f"last window {summary}", flush=True)
        print(f"[3] implied cadence: uptime_s {[g['uptime_s'] for g in grown]}", flush=True)

        # --- F. the published schema describes what is written -----------------
        # No validator dependency: the schema's own key set is compared with the
        # records'. That is the drift a hand-written schema gets wrong first.
        with open(SCHEMA_PATH, "r", encoding="utf-8") as fh:
            schema = json.load(fh)
        written = set(KEYS) | set(FILE_KEYS)
        assert set(schema["properties"]) == written, \
            f"tests/fixtures/metrics.schema.json must describe exactly the records: " \
            f"{sorted(set(schema['properties']) ^ written)}"
        assert set(schema["required"]) == written, \
            f"every published field is required: {sorted(set(schema['required']) ^ written)}"
        assert schema.get("additionalProperties") is False, \
            "the schema publishes a closed field set (unknown keys are not part of the format)"
        assert schema["properties"]["v"].get("const") == VERSION, \
            f"the schema must name version {VERSION}: {schema['properties']['v']}"
        print(f"[4] schema: {os.path.relpath(SCHEMA_PATH)} describes v{VERSION}, "
              f"{len(written)} fields, all required", flush=True)

        # --- E. the refusals ---------------------------------------------------
        code, log = run_expecting_failure(binary, data_root,
                                          ["--metrics-file", os.path.join(root, "nope", "m.jsonl")])
        assert code != 0, "a metrics path in a missing directory must not start the server"
        assert "cannot open --metrics-file" in log, f"the refusal must say why: {log[-600:]}"
        code, log = run_expecting_failure(binary, data_root, ["--metrics-file", root])
        assert code != 0, "a metrics *directory* must not start the server"
        assert "cannot open --metrics-file" in log, f"the refusal must say why: {log[-600:]}"
        print("[5] refusals: a missing directory and a directory both fail the start", flush=True)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print("\nPASS: --metrics-file — JSON-lines export, append, parity with the metrics line")
    return 0


if __name__ == "__main__":
    sys.exit(main())
