#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generation throughput, per second, from a soak's server log.
#
# `bench/soak.py --log FILE` captures the server's stdout, which carries the
# per-second `metrics:` line (`core/metrics.cpp`): tps, work, ticks, and the
# world store's own gauges. The record the harness writes keeps only the loaded
# window's summary (plus the world gauge's first/last/max), which is enough for
# "did the tick survive" and not enough for "how fast did the world generate".
# This reads the raw log and prints:
#
#   * the world store's columns and bytes over time (`world_chunks`,
#     `world_bytes`), with the per-second deltas — during a fresh join the store
#     fills with new columns, so a positive delta IS one generated column;
#   * ticks/s from the server's own counter, which is the tick rate the
#     generation ran beside (a collapsed tick shows up here before anywhere
#     else);
#   * the window's summary: mean and peak columns/s over the monotonic phase,
#     and the bytes a column settled at.
#
# The `metrics:` lines arrive one per `--metrics-interval` second, so the index
# of a line is its second from server start; no wall clock is needed (and the
# log's own timestamps are locale-shaped, which is why they are not parsed).
#
# Usage: tools/soak_throughput.py /tmp/run.log [more.log ...]
import re
import sys


METRICS = re.compile(r"metrics: (.*)$")


def parse(path):
    samples = []
    with open(path, "r", errors="replace") as handle:
        for line in handle:
            match = METRICS.search(line)
            if not match:
                continue
            row = {}
            for pair in match.group(1).split():
                if "=" not in pair:
                    continue
                key, value = pair.split("=", 1)
                try:
                    row[key] = float(value)
                except ValueError:
                    continue
            samples.append(row)
    return samples


def summarise(path):
    samples = parse(path)
    if len(samples) < 3:
        print(f"{path}: {len(samples)} metrics line(s) — not enough to summarise")
        return
    first = samples[0]
    last = samples[-1]
    columns_first = first.get("world_chunks", 0.0)
    columns_last = last.get("world_chunks", 0.0)
    # The generation phase: every second in which the store gained columns. A
    # join storm fills it monotonically; the idle tail (if the run has one)
    # contributes no delta and is left out of the rate.
    gains = []
    for before, after in zip(samples, samples[1:]):
        delta = after.get("world_chunks", 0.0) - before.get("world_chunks", 0.0)
        if delta > 0:
            gains.append(delta)
    seconds = len(samples) - 1
    mean_rate = (columns_last - columns_first) / seconds if seconds > 0 else 0.0
    peak = max(gains) if gains else 0.0
    mean_while_generating = sum(gains) / len(gains) if gains else 0.0
    bytes_per_column = (
        (last.get("world_bytes", 0.0) - first.get("world_bytes", 0.0)) /
        (columns_last - columns_first)
        if columns_last > columns_first else 0.0
    )
    print(f"{path}")
    print(f"  metrics lines       {len(samples)} (1/s)")
    print(f"  columns             {columns_first:.0f} -> {columns_last:.0f}")
    print(f"  columns/s           {mean_rate:.2f} over the whole run, "
          f"{mean_while_generating:.2f} while generating, peak {peak:.0f}")
    print(f"  bytes/column ended  {bytes_per_column:.0f}")
    print(f"  tick rate           {first.get('tps', 0.0):.2f} -> {last.get('tps', 0.0):.2f} "
          f"(min {min(s.get('tps', 0.0) for s in samples):.2f})")
    print(f"  work/tick           avg {max(s.get('work_avg_ms', 0.0) for s in samples):.2f} ms peak-avg, "
          f"{max(s.get('work_peak_ms', 0.0) for s in samples):.2f} ms worst")
    print(f"  generated seconds   {len(gains)} of {seconds}")


def main():
    if len(sys.argv) < 2:
        print("usage: tools/soak_throughput.py LOG [LOG ...]", file=sys.stderr)
        return 2
    for path in sys.argv[1:]:
        summarise(path)
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
