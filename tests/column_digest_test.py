#!/usr/bin/env python3
"""The cross-compiler column comparison's own test.

`tools/compare_column_digests.py` is the guard CI relies on to fail a push when
one compiler's decorated columns move: each build runner uploads
`plusmc-column-digest`'s record, and the `column-digest` job compares them. A
comparator that stopped noticing differences would leave that guard silently
green, and every runner's own tests would still pass — so the guard needs a test
of its own.

These are synthetic records: no binary, no worldgen, no fixtures. What is pinned
is the comparison contract, not any column:

  * identical records pass, whatever order their rows are in and whichever order
    the paths are given in (the comparison is by `(seed, cx, cz)`, not by list
    position);
  * a changed digest fails, names the column *and* the record it came from;
  * a third record that differs is compared too, not only the second one;
  * a missing column, an extra column, a duplicate column, a different grid,
    seed, cell count or digest format, a malformed row and a file that is not a
    record are all refusals, with a message that says which;
  * one record alone is not a comparison.

`--baseline` adds the second question: agreement between runners is not enough,
because three runners can move together. The tests pin that a record which
matches the pin passes, that drift from the pin fails *even when every record
agrees with every other* (and is reported as drift, not as a disagreement), that
the pin's shape is enforced, and that one record against the pin — the local
form — is a comparison.

Run: column_digest_test.py
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
COMPARE = os.path.join(HERE, "..", "tools", "compare_column_digests.py")
SHAPE = {"format": 1, "seeds": [42, 7], "grid_radius": 1, "cells_per_column": 98304}


def column(seed, cx, cz):
    digest = (seed * 1000003 + (cx + 8) * 1009 + (cz + 8) * 7) & 0xFFFFFFFFFFFFFFFF
    return {"seed": seed, "cx": cx, "cz": cz, "digest": f"{digest:016x}"}


def base_rows():
    return [column(seed, cx, cz) for seed in (42, 7) for cz in (0, 1) for cx in (-1, 0)]


def record(rows=None, **overrides):
    data = {"kind": "column-digest", **SHAPE, "compiler": "unit test",
            "columns": base_rows() if rows is None else rows}
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


def run(*paths):
    return subprocess.run([sys.executable, COMPARE, *paths], capture_output=True, text=True)


def expect_pass(proc, columns):
    assert proc.returncode == 0, f"expected a pass, got {proc.returncode}:\n{proc.stderr}"
    assert f"{columns} columns" in proc.stdout, proc.stdout


def expect_fail(proc, needle):
    assert proc.returncode != 0, f"expected a refusal, got 0:\n{proc.stdout}"
    assert needle in proc.stderr, f"the refusal does not name {needle!r}:\n{proc.stderr}"


def test_identical_records_pass(directory):
    left = write(directory, "gcc.json", record())
    # A record written by a different run may list its rows in any order; the
    # comparison is keyed by (seed, cx, cz), not by position.
    rows = base_rows()
    right = write(directory, "clang.json", record(rows=list(reversed(rows))))
    expect_pass(run(left, right), len(rows))
    assert "all identical" in run(left, right).stdout
    # Argument order is not a property of the answer either.
    expect_pass(run(right, left), len(rows))


def test_one_moved_digest_fails(directory):
    rows = base_rows()
    moved = [dict(row) for row in rows]
    moved[2]["digest"] = "0" * 16
    left = write(directory, "gcc.json", record(rows))
    right = write(directory, "macos.json", record(moved))
    proc = run(left, right)
    key = (moved[2]["seed"], moved[2]["cx"], moved[2]["cz"])
    expect_fail(proc, "macos.json")
    assert f"column {key} digest" in proc.stderr, proc.stderr
    assert "1 column(s)" in proc.stdout, proc.stdout


def test_a_third_record_is_compared(directory):
    # The second record agreeing must not stop the third from being checked:
    # CI compares three runners, and the odd one out can be any of them.
    rows = base_rows()
    moved = [dict(row) for row in rows]
    moved[-1]["digest"] = "f" * 16
    first = write(directory, "gcc.json", record(rows))
    second = write(directory, "clang.json", record(rows))
    third = write(directory, "macos.json", record(moved))
    proc = run(first, second, third)
    expect_fail(proc, "macos.json")
    key = (moved[-1]["seed"], moved[-1]["cx"], moved[-1]["cz"])
    assert f"column {key}" in proc.stderr, proc.stderr


def test_missing_and_extra_columns_fail(directory):
    rows = base_rows()
    left = write(directory, "gcc.json", record(rows))
    missing = write(directory, "missing.json", record(rows[:-1]))
    expect_fail(run(left, missing), "missing column")

    extra_rows = rows + [column(42, 5, 5)]
    extra = write(directory, "extra.json", record(extra_rows))
    expect_fail(run(left, extra), "extra column")


def test_duplicate_column_fails(directory):
    rows = base_rows() + [dict(base_rows()[0])]
    left = write(directory, "gcc.json", record())
    duplicate = write(directory, "duplicate.json", record(rows))
    expect_fail(run(left, duplicate), "duplicate column")


def test_shape_mismatch_fails(directory):
    left = write(directory, "gcc.json", record())
    for field, value in (("format", 2), ("seeds", [42]), ("grid_radius", 2),
                         ("cells_per_column", 98305)):
        other = write(directory, f"{field}.json", record(**{field: value}))
        expect_fail(run(left, other), f"different {field}")


def test_malformed_records_fail(directory):
    left = write(directory, "gcc.json", record())
    wrong_kind = write(directory, "wrong-kind.json", {"kind": "light-seam", "columns": []})
    expect_fail(run(left, wrong_kind), "not a column-digest record")

    bad_digest_rows = [dict(base_rows()[0], digest="xyz")] + base_rows()[1:]
    bad_digest = write(directory, "bad-digest.json", record(bad_digest_rows))
    expect_fail(run(left, bad_digest), "malformed digest")

    not_json = write(directory, "junk.json", "this is not json\n")
    expect_fail(run(left, not_json), "junk.json")

    expect_fail(run(left), "at least two records")


def test_a_record_matching_the_baseline_passes(directory):
    pin = write(directory, "pin.json", record())
    first = write(directory, "gcc.json", record())
    second = write(directory, "clang.json", record())
    proc = run("--baseline", pin, first, second)
    expect_pass(proc, len(base_rows()))
    assert "all identical, and match the pinned baseline" in proc.stdout, proc.stdout
    # The pin is the second thing compared, so one record against it is the
    # local form of the check.
    proc = run("--baseline", pin, first)
    expect_pass(proc, len(base_rows()))
    assert "matches the pinned baseline" in proc.stdout, proc.stdout


def test_drift_from_the_baseline_fails_even_when_records_agree(directory):
    # The case the pin exists for: a generator change that moves every compiler
    # together. Both records are identical to each other and differ from the
    # pin, so cross-runner comparison alone would pass.
    rows = base_rows()
    pinned = [dict(row) for row in rows]
    pinned[1]["digest"] = "a" * 16
    pin = write(directory, "pin.json", record(pinned))
    first = write(directory, "gcc.json", record(rows))
    second = write(directory, "clang.json", record(rows))
    proc = run("--baseline", pin, first, second)
    key = (pinned[1]["seed"], pinned[1]["cx"], pinned[1]["cz"])
    expect_fail(proc, "drift from the pinned baseline")
    assert f"column {key} digest" in proc.stderr, proc.stderr
    assert "gcc.json" in proc.stderr and "clang.json" in proc.stderr, proc.stderr
    # Drift is not a disagreement: the message must not claim the runners differ.
    assert "disagree" not in proc.stderr, proc.stderr
    assert "re-record the baseline" in proc.stderr, proc.stderr


def test_drift_from_the_baseline_fails_for_one_record(directory):
    pin = write(directory, "pin.json", record())
    rows = base_rows()
    moved = [dict(row) for row in rows]
    moved[0]["digest"] = "b" * 16
    fresh = write(directory, "fresh.json", record(moved))
    proc = run("--baseline", pin, fresh)
    expect_fail(proc, "drift from the pinned baseline")
    assert "1 record" in proc.stderr, proc.stderr


def test_the_baseline_shape_is_enforced(directory):
    pin = write(directory, "gcc.json", record())
    for field, value in (("format", 2), ("seeds", [42]), ("grid_radius", 3)):
        other = write(directory, f"pin-{field}.json", record(**{field: value}))
        expect_fail(run("--baseline", other, pin), f"different {field}")
        assert "pinned baseline" in run("--baseline", other, pin).stderr


def test_baseline_errors_are_refused(directory):
    left = write(directory, "gcc.json", record())
    expect_fail(run("--baseline", os.path.join(directory, "no-such-pin.json"), left),
                "no-such-pin.json")
    proc = run(left, "--baseline")
    assert proc.returncode == 2, proc.returncode
    assert "takes a record path" in proc.stderr, proc.stderr
    junk_pin = write(directory, "junk-pin.json", "not json\n")
    expect_fail(run("--baseline", junk_pin, left), "junk-pin")


def main():
    tests = [
        ("identical records pass, row and argument order aside", test_identical_records_pass),
        ("one moved digest fails, naming column and record", test_one_moved_digest_fails),
        ("a third record is compared too", test_a_third_record_is_compared),
        ("a missing or extra column fails", test_missing_and_extra_columns_fail),
        ("a duplicate column fails", test_duplicate_column_fails),
        ("a different format/seed/grid/cell shape fails", test_shape_mismatch_fails),
        ("malformed records and a single record are refused", test_malformed_records_fail),
        ("a record matching the pinned baseline passes", test_a_record_matching_the_baseline_passes),
        ("drift fails even when the records agree", test_drift_from_the_baseline_fails_even_when_records_agree),
        ("drift fails for one record against the pin", test_drift_from_the_baseline_fails_for_one_record),
        ("the pin's shape is enforced", test_the_baseline_shape_is_enforced),
        ("a missing or unreadable pin is refused", test_baseline_errors_are_refused),
    ]
    with tempfile.TemporaryDirectory(prefix="plusmc-column-digest-") as directory:
        for label, fn in tests:
            fn(directory)
            print(f"  OK {label}")
    print("\nPASS: column-digest comparison")
    return 0


if __name__ == "__main__":
    sys.exit(main())
