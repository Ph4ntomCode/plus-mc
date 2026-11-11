#!/usr/bin/env python3
"""Record and compare the generation-time light seam across seeds and chunks.

`tests/world_light_test.cpp` pins the seam on the two columns the worldgen
fixtures describe. This is the many-column version: it runs the probe built from
`bench/light_seam.cpp` (a 5x5 chunk grid per seed, seeds 42, 7 and 2026 by
default), and either saves the measurement as a dated record under
`bench/baseline/` or diffs it field by field against the newest one. The tally
splits the sky disagreements into edge cells (the half a neighbouring chunk's
light can reach) and the column's interior:

    python3 bench/seam_track.py                  # run, compare with the baseline
    python3 bench/seam_track.py --save           # run and keep a new baseline
    python3 bench/seam_track.py --seeds 42,7 --radius 1
    python3 bench/seam_track.py --against bench/baseline/<record>.json
    python3 bench/seam_track.py --measurement /tmp/seam-arm64.json \
        --against bench/baseline/<record>.json --fail-on-change

The last form compares a probe JSON another machine produced instead of running
the binary here — what `tools/check_light_seam_linux.sh` uses for its container
legs, where the probe runs under a Linux gcc (native on x86_64, qemu-emulated on
aarch64) and this tool only has to hold the pin. The file may be the probe's own
output or a saved record (its `measurement` is unwrapped either way); a file that
is not a measurement is refused rather than compared as an empty one.

The probe itself exits 1 on a structural violation (a generated column without
light, a run in which generation and the engine agree on every cell, a
disagreement above the surface, a brighter cell off the border, invented block
light), so a broken approximation still fails here. The counts are *not* held
fixed: a deliberate generator change moves them, and the comparison shows where.
`--fail-on-change` makes any moved total fail the run — the two layer aggregates
and each seed's own split, because a change that moves one seed up and another
down by the same amount leaves the aggregates equal — for a caller that wants the
record treated as a strict pin, which is how `.github/workflows/ci.yml` runs it,
against an explicit record rather than whatever is newest.

The record's counts are machine-independent (they are light levels, not timings);
the `machine`/`generated` fields are provenance, not part of the comparison. That
is measured, not assumed: the 22 Oct record re-ran on Linux x86_64 (gcc 13.3)
field for field once `WgPerlinNoise` owned its amplitudes (docs/PHASE5.md's
platform check).
"""

import argparse
import datetime
import glob
import json
import pathlib
import platform
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASELINE_DIR = ROOT / "bench" / "baseline"

# The tally fields compared, in the order a reader wants them: the answer first,
# then the shape of the error.
TALLY_FIELDS = [
    "agree",
    "brighter",
    "dimmer",
    "max_brighter",
    "max_dimmer",
    "above_surface",
    "border",
    "interior",
    "brighter_border",
    "in_opaque",
]


def find_binary(explicit):
    if explicit:
        path = pathlib.Path(explicit)
        if not path.is_file():
            sys.exit(f"no probe at {path}")
        return path
    candidates = sorted(glob.glob(str(ROOT / "build*" / "plusmc-light-seam")),
                        key=lambda p: pathlib.Path(p).stat().st_mtime, reverse=True)
    if not candidates:
        sys.exit("no plusmc-light-seam built yet; "
                 "build it with `cmake --build build --target plusmc-light-seam`")
    return pathlib.Path(candidates[0])


def machine_label():
    name = ""
    if sys.platform == "darwin":
        try:
            name = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"],
                                  capture_output=True, text=True, check=False).stdout.strip()
        except OSError:
            name = ""
    name = name or platform.processor() or platform.machine()
    match = re.search(r"(i[3579]-\d{4,5}[A-Z]*)", name)
    return match.group(1).lower() if match else re.sub(r"\s+", "-", name.lower())


def newest_baseline(except_path=None):
    records = sorted(BASELINE_DIR.glob("*light-seam*.json"))
    records = [r for r in records if except_path is None or r.resolve() != pathlib.Path(except_path).resolve()]
    return records[-1] if records else None


def load_measurement(path):
    """A measurement written by `plusmc-light-seam --json`, or a saved record.

    The probe writes the measurement itself (`kind: light-seam`); a record wraps
the same object under `measurement`. Anything else — junk, a column-digest
    record, an empty object — is refused here, so the comparison below cannot
    report agreement with a file that never held a tally.
    """
    if not path.is_file():
        sys.exit(f"no measurement at {path}")
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        sys.exit(f"{path} is not readable JSON: {exc}")
    measurement = as_measure(data)
    if not isinstance(measurement, dict) or not isinstance(measurement.get("sky"), dict) \
            or not isinstance(measurement.get("block"), dict) \
            or not isinstance(measurement.get("columns"), int):
        sys.exit(f"{path} is not a light-seam measurement (no sky/block tallies)")
    return measurement


def measure(binary, seeds, radius, chunks):
    with tempfile.TemporaryDirectory(prefix="plusmc-seam-") as tmp:
        out = pathlib.Path(tmp) / "seam.json"
        argv = [str(binary), "--seeds", seeds, "--json", str(out)]
        if chunks:
            argv += ["--chunks", chunks]
        else:
            argv += ["--radius", str(radius)]
        result = subprocess.run(argv, capture_output=True, text=True, check=False)
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        if result.returncode != 0:
            sys.exit(result.returncode if result.returncode > 0 else 1)
        return json.loads(out.read_text())


def record_for(measurement, argv_note):
    return {
        "kind": "light-seam-baseline",
        "format": 1,
        "name": "light-seam",
        "generated": datetime.date.today().isoformat(),
        "machine": machine_label(),
        "note": argv_note,
        "measurement": measurement,
    }


def save(record):
    BASELINE_DIR.mkdir(parents=True, exist_ok=True)
    measure = record["measurement"]
    seeds = len(measure.get("seeds", []))
    radius = measure.get("grid_radius")
    extent = f"r{radius}" if radius is not None else f"{measure['columns']}cols"
    name = (f"{record['generated']}-{record['machine']}-light-seam-"
            f"{seeds}seeds-{extent}.json")
    path = BASELINE_DIR / name
    suffix = 2
    while path.exists():
        path = BASELINE_DIR / name.replace(".json", f"-{suffix}.json")
        suffix += 1
    path.write_text(json.dumps(record, indent=1, sort_keys=True) + "\n")
    return path


def as_measure(record):
    return record.get("measurement", record)


def _num(value):
    return value if isinstance(value, (int, float)) else 0


def display_path(path):
    """The record's path relative to the tree when it is inside it."""
    try:
        return str(path.resolve().relative_to(ROOT))
    except ValueError:
        return str(path)


def compare(new, old, old_path, fail_on_change):
    before, after = as_measure(old), as_measure(new)
    print(f"\ncompared with {display_path(old_path)}")
    print(f"  {before.get('columns', 0)} columns -> {after.get('columns', 0)} columns"
          f"; seeds {before.get('seeds')} -> {after.get('seeds')}")

    # A pin also covers the shape it was measured on: the same seeds and the
    # same number of columns. Otherwise a run measuring fewer cells could agree on
    # every field it shares and pass for the wrong reason.
    shape_moved = (before.get("columns") != after.get("columns") or
                   before.get("seeds") != after.get("seeds"))
    if shape_moved:
        print("  the run covers a different set of columns than the record")

    changed = 0
    for layer in ("sky", "block"):
        prev_layer = before.get(layer, {})
        next_layer = after.get(layer, {})
        print(f"  {layer}:")
        for field in TALLY_FIELDS:
            prev, nxt = _num(prev_layer.get(field)), _num(next_layer.get(field))
            if prev == nxt:
                continue
            changed += 1
            print(f"    {field:<16} {prev:>10,} -> {nxt:>10,}  ({nxt - prev:+,})")
        prev_cells, next_cells = _num(prev_layer.get("cells")), _num(next_layer.get("cells"))
        if prev_cells and next_cells:
            prev_share = 100.0 * (_num(prev_layer.get("brighter")) + _num(prev_layer.get("dimmer"))) / prev_cells
            next_share = 100.0 * (_num(next_layer.get("brighter")) + _num(next_layer.get("dimmer"))) / next_cells
            if abs(next_share - prev_share) >= 0.005:
                print(f"    {'differ %':<16} {prev_share:>9.2f}% -> {next_share:>9.2f}%  "
                      f"({next_share - prev_share:+.2f} pt)")

    # Per-seed only where both records cover the same columns of that seed, so
    # an extra seed (or a different grid) is not reported as a drift.
    prev_seeds = {row.get("seed"): row for row in before.get("per_seed", [])}
    next_seeds = {row.get("seed"): row for row in after.get("per_seed", [])}
    common = [seed for seed in sorted(set(prev_seeds) & set(next_seeds))
              if len(prev_seeds[seed].get("per_column", [])) ==
              len(next_seeds[seed].get("per_column", []))]
    moved_seeds = []
    for seed in common:
        for layer in ("sky", "block"):
            if any(_num(prev_seeds[seed].get(layer, {}).get(f)) !=
                   _num(next_seeds[seed].get(layer, {}).get(f)) for f in TALLY_FIELDS):
                moved_seeds.append(seed)
                break
    if moved_seeds:
        print(f"  seeds whose counts moved: {', '.join(str(s) for s in moved_seeds)}")
    # A per-seed split that moves while the aggregate holds is still drift, and
    # the record stores both: a change that moves one seed up and another down by
    # the same amount leaves every aggregate field equal, so a pin that watched
    # only those would pass it. The strict mode has to cover the seeds too.
    if changed == 0 and not shape_moved and not moved_seeds:
        print("  no total moved")
    elif fail_on_change:
        if changed != 0:
            sys.exit(f"{changed} tally field(s) moved")
        if moved_seeds:
            seeds = ", ".join(str(s) for s in moved_seeds)
            sys.exit(f"{len(moved_seeds)} seed column split(s) moved: {seeds}")
        sys.exit("the run covers a different set of columns than the record")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--binary", help="path to plusmc-light-seam")
    parser.add_argument("--seeds", default="42,7,2026",
                        help="comma-separated seeds (default: 42,7,2026)")
    parser.add_argument("--radius", type=int, default=2,
                        help="chunk grid radius per seed (default: 2, a 5x5 grid)")
    parser.add_argument("--chunks", help="explicit 'x,z;x,z' list instead of a grid")
    parser.add_argument("--measurement",
                        help="compare this probe JSON instead of running the binary")
    parser.add_argument("--save", action="store_true", help="keep this run as a baseline")
    parser.add_argument("--against", help="compare with this record (default: the newest)")
    parser.add_argument("--no-compare", action="store_true", help="only measure")
    parser.add_argument("--fail-on-change", action="store_true",
                        help="exit 1 when any compared total moved, or the run "
                             "covers different columns than the record")
    args = parser.parse_args()

    if args.measurement:
        if args.binary:
            sys.exit("--measurement and --binary are alternatives")
        measurement = load_measurement(pathlib.Path(args.measurement))
        print(f"measurement: {args.measurement}")
    else:
        binary = find_binary(args.binary)
        measurement = measure(binary, args.seeds, args.radius, args.chunks)
    saved_path = None
    if args.save:
        note = (f"measurement from {args.measurement}" if args.measurement
                else f"seam_track.py --seeds {args.seeds} --radius {args.radius}")
        record = record_for(measurement, note)
        saved_path = save(record)
        print(f"baseline: {saved_path}")

    if args.no_compare:
        return
    if args.against:
        old_path = pathlib.Path(args.against)
        if not old_path.is_file():
            sys.exit(f"no record at {old_path}")
    else:
        # When this run was just saved, compare against the record that was
        # newest before it, not against itself.
        old_path = newest_baseline(except_path=saved_path)
        if old_path is None:
            print("\nno earlier light-seam baseline; run with --save to start one")
            return
    compare(record_for(measurement, ""), json.loads(old_path.read_text()), old_path,
            args.fail_on_change)


if __name__ == "__main__":
    main()
