#!/usr/bin/env python3
"""Compare decorated-column digests from different build runners.

`plusmc-column-digest` writes one record per build (seed, chunk and the digest
of every block state in that decorated column). CI runs it once on each compiler
in the build matrix and uploads the records; this tool diffs them against each
other, so a generator that only diverges under one compiler fails the push
instead of passing every runner on its own. The failure it exists for is a value
that depends on what the stack held or on an evaluation order the standard
leaves unspecified: stable on the machine that recorded the golden, different on
another (`docs/PHASE5.md`'s geode).

    python3 tools/compare_column_digests.py digests/*/column-digest.json

Two or more records are required. Every record must cover the same seeds, grid
radius, cell count and digest format; each column is identified by
`(seed, cx, cz)`. The check fails on a missing or extra column, a duplicate, or
a digest that differs — and prints the columns that differ, not just a count.

Agreement between runners is not the whole contract: three runners that all
changed together still agree with each other. `--baseline` pins one recorded
record as well, so every record must match it too:

    python3 tools/compare_column_digests.py --baseline bench/baseline/<record>.json \\
        digests/*/column-digest.json

Drift from the pinned record fails on its own, even when every record agrees.
A real generator change therefore re-records the baseline deliberately
(`./build/plusmc-column-digest --json <path>`) instead of moving every runner at
once and passing. With `--baseline` a single record is enough, which is the
local form: one fresh run against the pin.
"""

import json
import pathlib
import sys

KIND = "column-digest"
SHAPE_FIELDS = ("format", "seeds", "grid_radius", "cells_per_column")


def load(path):
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        sys.exit(f"{path}: {exc}")
    if data.get("kind") != KIND:
        sys.exit(f"{path}: not a {KIND} record (kind {data.get('kind')!r})")
    rows = data.get("columns")
    if not isinstance(rows, list) or not rows:
        sys.exit(f"{path}: no columns")
    return data


def column_key(row):
    try:
        return (int(row["seed"]), int(row["cx"]), int(row["cz"]))
    except (KeyError, TypeError, ValueError) as exc:
        sys.exit(f"malformed column row {row!r}: {exc}")


def columns_by_key(path, data):
    columns = {}
    for row in data["columns"]:
        key = column_key(row)
        if key in columns:
            sys.exit(f"{path}: duplicate column {key}")
        digest = row.get("digest")
        if not isinstance(digest, str) or len(digest) != 16:
            sys.exit(f"{path}: column {key} has a malformed digest {digest!r}")
        columns[key] = digest
    return columns


def describe(label, data):
    compiler = data.get("compiler")
    line = label
    if compiler:
        line += f" ({compiler})"
    print(f"{line}: {len(data['columns'])} columns")


def check_shape(reference_label, reference, path, data):
    for field in SHAPE_FIELDS:
        if data.get(field) != reference.get(field):
            sys.exit(f"{path} and {reference_label} cover different {field}: "
                     f"{data.get(field)!r} against {reference.get(field)!r}")


def report_difference(against, expected, actual, show, failures):
    """The columns of one pair that moved, as FAIL lines and one summary line.

    `expected` is a `(seed, cx, cz)` -> digest map, `actual` the record being
    checked, `show` the verb the summary line ends in ("differ from X").
    """
    columns = actual.columns
    missing = sorted(set(expected) - set(columns))
    extra = sorted(set(columns) - set(expected))
    differing = sorted(key for key in set(expected) & set(columns)
                       if expected[key] != columns[key])
    for key in missing:
        failures.append(f"{actual.label}: missing column {key}, which {against} has")
    for key in extra:
        failures.append(f"{actual.label}: extra column {key}, which {against} does not have")
    for key in differing:
        failures.append(f"{actual.label}: column {key} digest {columns[key]} "
                        f"against {against}'s {expected[key]}")
    if differing:
        shown = ", ".join(f"{key}" for key in differing[:10])
        suffix = "" if len(differing) <= 10 else f" and {len(differing) - 10} more"
        print(f"  {actual.label}: {len(differing)} column(s) {show}: {shown}{suffix}")
    return len(missing) + len(extra) + len(differing)


class Record:
    def __init__(self, label, data):
        self.label = label
        self.data = data
        self.columns = columns_by_key(label, data)


def compare(paths, baseline_path=None):
    if baseline_path is None and len(paths) < 2:
        sys.exit("comparison needs at least two records")
    if not paths:
        sys.exit("no records given")

    records = [Record(str(path), load(path)) for path in paths]
    baseline = None
    if baseline_path is not None:
        label = f"the pinned baseline {baseline_path}"
        baseline = Record(label, load(baseline_path))
        print(f"{label}: {len(baseline.data['columns'])} columns")

    reference = records[0]
    for record in records:
        describe(record.label, record.data)
        check_shape(reference.label, reference.data, record.label, record.data)
    if baseline is not None:
        check_shape(baseline.label, baseline.data, reference.label, reference.data)

    failures, disagreements, drift = [], 0, 0
    for record in records[1:]:
        disagreements += report_difference(f"the record {reference.label}", reference.columns,
                                           record, f"differ from the record {reference.label}",
                                           failures)
    if baseline is not None:
        for record in records:
            drift += report_difference(baseline.label, baseline.columns, record,
                                       f"differ from {baseline.label}", failures)

    if failures:
        for failure in failures[:50]:
            print(f"FAIL {failure}", file=sys.stderr)
        if len(failures) > 50:
            print(f"FAIL ... and {len(failures) - 50} more", file=sys.stderr)
        records_word = "record" if len(records) == 1 else "records"
        if disagreements:
            print(f"{len(records)} {records_word} disagree on the decorated columns",
                  file=sys.stderr)
        if drift:
            print(f"{len(records)} {records_word} drift from {baseline.label}", file=sys.stderr)
            print("re-record the baseline only when the generator change is intended: "
                  f"./build/plusmc-column-digest --json {baseline_path}", file=sys.stderr)
        return 1

    if baseline is not None and len(records) == 1:
        print(f"1 record, {len(reference.columns)} columns: matches {baseline.label}")
    elif baseline is not None:
        print(f"{len(records)} records, {len(reference.columns)} columns each: all identical, "
              f"and match {baseline.label}")
    else:
        print(f"{len(records)} records, {len(reference.columns)} columns each: all identical")
    return 0


def main(argv):
    paths, baseline = [], None
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--baseline":
            if i + 1 >= len(argv):
                print("--baseline takes a record path", file=sys.stderr)
                return 2
            baseline = pathlib.Path(argv[i + 1])
            i += 2
        elif arg.startswith("--baseline="):
            baseline = pathlib.Path(arg.split("=", 1)[1])
            i += 1
        elif arg in ("-h", "--help"):
            print(__doc__.strip())
            return 0
        elif arg.startswith("-"):
            print(f"unknown argument {arg} (try --help)", file=sys.stderr)
            return 2
        else:
            paths.append(pathlib.Path(arg))
            i += 1
    return compare(paths, baseline)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
