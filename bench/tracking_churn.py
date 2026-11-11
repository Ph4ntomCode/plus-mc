#!/usr/bin/env python3
"""Churn probe for the interest-management tracking boundary.

A move-only client's relay is re-evaluated on every movement packet it sends, so
a player hovering on the 48-block tracking line makes the pairing transition fire
once per packet unless the gate has a drop margin. This probe measures that:

* two real clients join; the observer sits at spawn and never moves again
* the hoverer announces positions that straddle the line by `--amplitude` blocks
  (47.5 and 48.5 by default, i.e. +/- 0.5 blocks around vanilla's 48), one
  position packet every `--period` seconds, for `--seconds`
* the observer's clientbound stream is classified by packet id, so the pairing
  traffic is counted directly: Spawn Entity (0x01) + synched data (0x63) per
  re-entry, Remove Entities (0x4d) per exit, rel moves (0x35) while tracked

The same binary is both arms: `PLUSMC_PLAYER_TRACKING_HYSTERESIS=0` is vanilla's
single threshold, the shipped default is a drop margin (8 blocks). The record is
JSON, like bench/soak.py's.

Usage: tracking_churn.py [--hysteresis N] [--seconds S] [--amplitude B] [--json F]
"""

import argparse
import json
import os
import pathlib
import re
import socket
import struct
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests"))

import wire_client  # noqa: E402
from join_test import (Client, PLAYER_A, PLAYER_B,  # noqa: E402
                       unpack_keepalive)

HOST = "127.0.0.1"
# The observer's own position, announced explicitly rather than assumed: the
# relay measures the distance between the entries' feet positions, so the probe
# needs both of them fixed to know where the line is.
OBS_X, OBS_Y, OBS_Z = 8.0, 100.0, 8.0
TRACKING_RANGE = 48.0

# Every entity packet a viewer of a player can receive, by packet id.
BY_ID = {0x01: "spawns", 0x4D: "removes", 0x63: "synched_data",
         0x35: "rel_moves", 0x23: "teleports", 0x38: "looks",
         0x36: "move_looks"}


def drain(client, seconds):
    """Collects (pid, body) for a window, acking keep-alives as they arrive."""
    out, deadline = [], time.time() + seconds
    while time.time() < deadline:
        try:
            pid, body = client.read_pkt(max(0.02, deadline - time.time()))
        except (socket.timeout, EOFError):
            break
        if pid == 0x2C:
            unpack_keepalive(client, body)
            continue
        out.append((pid, body))
    return out


def classify(packets):
    counts = {k: 0 for k in BY_ID.values()}
    counts.update({"other_packets": 0, "payload_bytes": 0})
    for pid, body in packets:
        key = BY_ID.get(pid)
        if key is None:
            counts["other_packets"] += 1
        else:
            counts[key] += 1
        counts["payload_bytes"] += len(body)
    return counts


def wait_listen(proc, deadline):
    while time.time() < deadline:
        line = proc.stdout.readline()
        if not line:
            if proc.poll() is not None:
                raise AssertionError("plusmc exited before listening")
            continue
        m = re.search(rb"listening on [\d.]+:(\d+)", line)
        if m:
            return int(m.group(1))
    raise AssertionError("plusmc did not report a listening port in time")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary", nargs="?", default=str(ROOT / "build/src/plusmc"))
    ap.add_argument("--seconds", type=float, default=5.0,
                    help="how long the hoverer walks the line")
    ap.add_argument("--period", type=float, default=0.05,
                    help="seconds between the hoverer's position packets (0.05 = 20 Hz)")
    ap.add_argument("--amplitude", type=float, default=0.5,
                    help="blocks either side of the tracking range")
    ap.add_argument("--range", type=float, default=TRACKING_RANGE,
                    help="the server's entry range in blocks "
                         "(PLUSMC_PLAYER_TRACKING_RANGE; the hover straddles this line)")
    ap.add_argument("--hysteresis", type=int, default=None,
                    help="PLUSMC_PLAYER_TRACKING_HYSTERESIS for the server; omit "
                         "for the shipped default, 0 for vanilla's single threshold")
    ap.add_argument("--view-distance", type=int, default=1)
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    data_root = ROOT / "data" / "vanilla"
    assert data_root.is_dir(), "no data/vanilla extraction present; run tools/vanilla-data"

    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "1200"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "4000"
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
    env.pop("PLUSMC_PLAYER_TRACKING_HYSTERESIS", None)
    if args.hysteresis is not None:
        env["PLUSMC_PLAYER_TRACKING_HYSTERESIS"] = str(args.hysteresis)
    if args.range != TRACKING_RANGE:
        env["PLUSMC_PLAYER_TRACKING_RANGE"] = str(int(args.range))

    load_before = os.getloadavg()[0] if hasattr(os, "getloadavg") else None
    proc, port = wire_client.start_server(
        [args.binary, "--host", HOST, "--port", "0", "--motd", "tracking churn",
         "--offline-mode", "--view-distance", str(args.view_distance),
         "--data", str(data_root)],
        lambda p: wait_listen(p, time.time() + 15), env=env)

    observer = hoverer = None
    rc = None
    try:
        deadline = time.time() + 10
        while True:
            try:
                s = socket.create_connection((HOST, port), timeout=3)
                s.close()
                break
            except OSError:
                if proc.poll() is not None:
                    raise AssertionError("plusmc exited early")
                if time.time() > deadline:
                    raise AssertionError("could not reach plusmc")
                time.sleep(0.05)

        observer = Client(PLAYER_B, port)
        hoverer = Client(PLAYER_A, port)
        observer.join()
        hoverer.join()
        # The observer announces where it is, so the hoverer's distance to it is
        # exact (the relay measures the two entries' feet positions).
        observer.send_pkt(0x1E, struct.pack(">ddd", OBS_X, OBS_Y, OBS_Z) + b"\x01")
        drain(observer, 0.4)
        drain(hoverer, 0.4)
        drain(observer, 0.3)  # count from quiet: the join's pairing burst is not churn

        in_x = OBS_X + args.range - args.amplitude   # inside the line
        out_x = OBS_X + args.range + args.amplitude  # outside it
        steps = int(round(args.seconds / args.period))
        started = time.time()
        for i in range(steps):
            x = out_x if i % 2 == 0 else in_x
            hoverer.send_pkt(0x1E, struct.pack(">ddd", x, OBS_Y, OBS_Z) + b"\x01")
            time.sleep(args.period)
        elapsed = time.time() - started

        counts = classify(drain(observer, 1.5))
    finally:
        for c in (observer, hoverer):
            if c is not None:
                try:
                    c.close()
                except Exception:
                    pass
        proc.terminate()
        try:
            rc = proc.wait(timeout=10)
        except Exception:
            proc.kill()

    transitions = counts["spawns"] + counts["removes"]
    record = {
        "note": "interest-management churn probe: a player hovering on the "
                "48-block tracking line, one observer, one binary",
        "argv": sys.argv,
        "binary": args.binary,
        "seconds": args.seconds,
        "period_s": args.period,
        "amplitude_blocks": args.amplitude,
        "tracking_range": args.range,
        "hysteresis": args.hysteresis if args.hysteresis is not None else "default(8)",
        "view_distance": args.view_distance,
        "steps": steps,
        "hover_seconds": round(elapsed, 3),
        "server_rc": rc,
        "observer": counts,
        "transitions": transitions,
        "transitions_per_s": round(transitions / elapsed, 2),
        "pairing_packets": counts["spawns"] + counts["synched_data"] + counts["removes"],
        "load_avg_1m": {"before": load_before,
                        "after": os.getloadavg()[0] if hasattr(os, "getloadavg") else None},
    }
    print(json.dumps(record, indent=2, sort_keys=True))
    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
