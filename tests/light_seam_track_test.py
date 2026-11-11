#!/usr/bin/env python3
"""The light seam's comparison and its `--measurement` path, tested.

`bench/seam_track.py` holds the seam pin: CI runs the probe with
`--fail-on-change` against `SEAM_BASELINE`, and `tools/check_light_seam_linux.sh`
compares the record a Linux container produced on the same bounce. A comparison
that stopped noticing a moved tally would leave both silently green, so the
comparison gets a test of its own — and since the container legs hand the tool a
measurement file instead of a running probe, that form is what the tests drive.

These are synthetic measurements: no probe, no worldgen, no fixtures. What is
pinned is the contract, not any seam:

  * a measurement matching the record prints `no total moved` and passes under
    `--fail-on-change`;
  * a moved sky or block field fails under the flag, naming the field and the
    signed delta, and passes without it while still printing the delta;
  * a measurement over a different set of columns is a shape change, which the
    flag fails on its own ("the run covers a different set of columns");
  * drift inside one seed's split is reported by name, and is drift for the
    strict pin too — including the case that leaves every aggregate equal because
    two seeds moved in opposite directions;
  * a saved record's `measurement` unwraps, so the file may be the probe's own
    JSON or a record;
  * a missing file, junk, a record of another kind and a file with no tallies
    are refusals that name the path, not comparisons against an empty tally;
  * `--measurement` and `--binary` together are refused (they are alternatives).

Run: light_seam_track_test.py
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TRACK = os.path.join(HERE, "..", "bench", "seam_track.py")
CELLS = 98304  # one column's cells per layer


def tally(agree, dimmer, border, interior, cells, max_dimmer=3):
    return {
        "agree": agree,
        "brighter": 0,
        "dimmer": dimmer,
        "max_brighter": 0,
        "max_dimmer": max_dimmer,
        "above_surface": 0,
        "border": border,
        "interior": interior,
        "brighter_border": 0,
        "in_opaque": 0,
        "cells": cells,
    }


def seed_row(seed, columns):
    cells = columns * CELLS
    return {
        "seed": seed,
        "sky": tally(cells - 5, 5, 3, 2, cells),
        "block": tally(cells - 4, 4, 2, 2, cells),
        "per_column": [{"cx": cx, "cz": 0, "measured": True} for cx in range(columns)],
    }


def measurement(seeds=(42, 7), columns_per_seed=3):
    columns = len(seeds) * columns_per_seed
    cells = columns * CELLS
    return {
        "kind": "light-seam",
        "format": 1,
        "seeds": list(seeds),
        "grid_radius": 1,
        "columns": columns,
        "cells_per_layer": CELLS,
        "pinned": {"seed": 42, "checked": 2, "mismatches": 0},
        "sky": tally(cells - 10, 10, 6, 4, cells, max_dimmer=5),
        "block": tally(cells - 8, 8, 4, 4, cells, max_dimmer=4),
        "per_seed": [seed_row(seed, columns_per_seed) for seed in seeds],
    }


def record(measure=None, **overrides):
    data = {
        "kind": "light-seam-baseline",
        "format": 1,
        "name": "light-seam",
        "generated": "2026-10-22",
        "machine": "unit-test",
        "note": "synthetic",
        "measurement": measurement() if measure is None else measure,
    }
    data.update(overrides)
    return data


def write(directory, name, data):
    path = os.path.join(directory, name)
    with open(path, "w", encoding="utf-8") as fh:
        if isinstance(data, str):
            fh.write(data)
        else:
            json.dump(data, fh)
    return path


def run(*args):
    return subprocess.run([sys.executable, TRACK, *args], capture_output=True, text=True)


def expect_pass(proc, needle="no total moved"):
    assert proc.returncode == 0, f"expected a pass, got {proc.returncode}:\n{proc.stdout}\n{proc.stderr}"
    assert needle in proc.stdout, proc.stdout


def expect_fail(proc, needle):
    assert proc.returncode != 0, f"expected a refusal, got 0:\n{proc.stdout}"
    assert needle in (proc.stdout + proc.stderr), \
        f"the refusal does not name {needle!r}:\n{proc.stdout}\n{proc.stderr}"


def test_a_matching_measurement_passes(directory):
    pin = write(directory, "pin.json", record())
    same = write(directory, "probe.json", measurement())
    proc = run("--measurement", same, "--against", pin, "--fail-on-change")
    expect_pass(proc)
    assert "6 columns -> 6 columns" in proc.stdout, proc.stdout
    assert "seeds [42, 7] -> [42, 7]" in proc.stdout, proc.stdout


def test_a_moved_sky_field_fails_only_under_the_flag(directory):
    pin = write(directory, "pin.json", record())
    moved = measurement()
    moved["sky"]["dimmer"] += 7
    moved["sky"]["agree"] -= 7
    moved["sky"]["interior"] += 7
    fresh = write(directory, "probe.json", moved)

    proc = run("--measurement", fresh, "--against", pin)
    expect_pass(proc, "")  # without the flag a moved total is reported, not fatal
    assert "dimmer" in proc.stdout and "17  (+7)" in proc.stdout, proc.stdout
    assert "no total moved" not in proc.stdout, proc.stdout

    proc = run("--measurement", fresh, "--against", pin, "--fail-on-change")
    expect_fail(proc, "3 tally field(s) moved")  # agree, dimmer, interior
    assert "interior" in proc.stdout and "(+7)" in proc.stdout, proc.stdout


def test_a_moved_block_field_fails(directory):
    pin = write(directory, "pin.json", record())
    moved = measurement()
    moved["block"]["border"] -= 3
    moved["block"]["dimmer"] += 3
    fresh = write(directory, "probe.json", moved)
    proc = run("--measurement", fresh, "--against", pin, "--fail-on-change")
    expect_fail(proc, "2 tally field(s) moved")
    assert "border" in proc.stdout, proc.stdout


def test_a_different_column_count_is_a_shape_change(directory):
    pin = write(directory, "pin.json", record())
    smaller = write(directory, "probe.json", measurement(seeds=(42,), columns_per_seed=3))
    proc = run("--measurement", smaller, "--against", pin, "--fail-on-change")
    expect_fail(proc, "covers a different set of columns than the record")
    assert "6 columns -> 3 columns" in proc.stdout, proc.stdout


def test_drift_inside_one_seed_is_reported_and_fails_the_pin(directory):
    pin = write(directory, "pin.json", record())
    moved = measurement()
    moved["per_seed"][1]["sky"]["dimmer"] += 2
    moved["per_seed"][1]["sky"]["agree"] -= 2
    fresh = write(directory, "probe.json", moved)

    proc = run("--measurement", fresh, "--against", pin)
    assert "seeds whose counts moved: 7" in proc.stdout, proc.stdout
    assert "no total moved" not in proc.stdout, proc.stdout
    expect_pass(proc, "")  # without the flag it is reported, not fatal

    # The record stores the per-seed splits, so the strict pin holds them too:
    # this is drift, not a note.
    proc = run("--measurement", fresh, "--against", pin, "--fail-on-change")
    expect_fail(proc, "1 seed column split(s) moved: 7")


def test_seed_drift_that_keeps_the_aggregate_fails_the_pin(directory):
    # One seed up, another down by the same amount: every aggregate field is
    # equal, so a pin watching only the totals would pass the change.
    pin = write(directory, "pin.json", record())
    moved = measurement()
    moved["per_seed"][0]["sky"]["dimmer"] -= 2
    moved["per_seed"][0]["sky"]["agree"] += 2
    moved["per_seed"][1]["sky"]["dimmer"] += 2
    moved["per_seed"][1]["sky"]["agree"] -= 2
    fresh = write(directory, "probe.json", moved)
    proc = run("--measurement", fresh, "--against", pin, "--fail-on-change")
    expect_fail(proc, "2 seed column split(s) moved: 7, 42")
    assert "no total moved" not in proc.stdout, proc.stdout


def test_a_saved_record_is_accepted_as_a_measurement(directory):
    # Both directions: the file handed to --measurement may be the probe's own
    # JSON or a saved record, and the record it is compared with may be either.
    bare = write(directory, "probe.json", measurement())
    as_record = write(directory, "pin.json", record())
    expect_pass(run("--measurement", as_record, "--against", bare, "--fail-on-change"))
    expect_pass(run("--measurement", bare, "--against", as_record, "--fail-on-change"))


def test_malformed_measurements_are_refused(directory):
    pin = write(directory, "pin.json", record())
    expect_fail(run("--measurement", os.path.join(directory, "nope.json"), "--against", pin),
                "no measurement at")
    junk = write(directory, "junk.json", "not json at all\n")
    expect_fail(run("--measurement", junk, "--against", pin), "not readable JSON")
    other_kind = write(directory, "digest.json", {"kind": "column-digest", "columns": 6})
    expect_fail(run("--measurement", other_kind, "--against", pin),
                "not a light-seam measurement")
    empty = write(directory, "empty.json", {})
    expect_fail(run("--measurement", empty, "--against", pin), "not a light-seam measurement")
    proc = run("--measurement", junk, "--binary", "somewhere/plusmc-light-seam", "--against", pin)
    expect_fail(proc, "alternatives")


def test_a_missing_pin_is_refused(directory):
    fresh = write(directory, "probe.json", measurement())
    expect_fail(run("--measurement", fresh,
                    "--against", os.path.join(directory, "no-such-record.json")),
                "no record at")


def main():
    tests = [
        ("a measurement matching the pin passes", test_a_matching_measurement_passes),
        ("a moved sky field fails only under the flag", test_a_moved_sky_field_fails_only_under_the_flag),
        ("a moved block field fails", test_a_moved_block_field_fails),
        ("a different column count is a shape change", test_a_different_column_count_is_a_shape_change),
        ("drift inside one seed is reported and fails the pin", test_drift_inside_one_seed_is_reported_and_fails_the_pin),
        ("seed drift that keeps the aggregate fails the pin",
         test_seed_drift_that_keeps_the_aggregate_fails_the_pin),
        ("a saved record is accepted as a measurement", test_a_saved_record_is_accepted_as_a_measurement),
        ("malformed measurements are refused", test_malformed_measurements_are_refused),
        ("a missing record is refused", test_a_missing_pin_is_refused),
    ]
    with tempfile.TemporaryDirectory(prefix="plusmc-light-seam-") as directory:
        for label, fn in tests:
            fn(directory)
            print(f"  OK {label}")
    print("\nPASS: light-seam comparison")
    return 0


if __name__ == "__main__":
    sys.exit(main())
