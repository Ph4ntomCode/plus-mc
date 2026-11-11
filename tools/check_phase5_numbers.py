#!/usr/bin/env python3
"""The docs' quoted numbers against the baseline records they came from.

docs/PHASE5.md is this project's measurement log, and a table that was true when
it was written is the easiest thing in it to leave stale: the record files get
re-measured, the prose does not. Each table that quotes records therefore carries
a machine-readable anchor right above it:

    <!-- check:numbers
    bench/baseline/<record>.json
    ...
    -->

listing one record per data row, in row order. This tool reads each anchor, walks
the table under it, and fails if any number in a row is not derivable from its
record — identity, or the decimal KB/MB scaling a byte count is quoted in, at the
precision the table prints.

It is a staleness detector, not a proof: a number that happens to coincide with
something else in the record passes. What it does catch is the case that matters
here — a table whose row was copied from an earlier run, a different arm, or a
typo.

Usage: check_phase5_numbers.py [docs/PHASE5.md]
"""

import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
ANCHOR = "<!-- check:numbers"
TOLERANCE_NOTE = "identity, /1e3 (KB), /1e6 (MB), /1024 (KiB), /1048576 (MiB)"
SCALINGS = [("", 1.0), ("KB", 1e3), ("MB", 1e6), ("KiB", 1024.0), ("MiB", 1048576.0)]


def numbers(value, out):
    """Every numeric leaf, depth-first, skipping booleans."""
    if isinstance(value, bool):
        return
    if isinstance(value, (int, float)):
        out.append(value)
    elif isinstance(value, dict):
        for v in value.values():
            numbers(v, out)
    elif isinstance(value, list):
        for v in value:
            numbers(v, out)
    return out


def matches(token: str, values):
    """The record value this token prints as, or None."""
    digits = len(token.split(".")[1]) if "." in token else 0
    want = token.replace(",", "")
    for v in values:
        for unit, factor in SCALINGS:
            try:
                scaled = v / factor
            except ZeroDivisionError:
                continue
            if f"{scaled:.{digits}f}" == want:
                return (v, unit or "identity")
    return None


def rows_after(lines, start):
    """The data rows of the first markdown table at or after `start`."""
    i = start
    while i < len(lines) and not lines[i].lstrip().startswith("|"):
        i += 1
    if i >= len(lines):
        return []
    table = []
    while i < len(lines) and lines[i].lstrip().startswith("|"):
        table.append(lines[i])
        i += 1
    rows = []
    for line in table:
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if not cells:
            continue
        if all(re.fullmatch(r":?-{2,}:?", c) for c in cells if c):
            continue  # the header separator
        rows.append(cells)
    return rows[1:]  # the header row itself


def check(path: pathlib.Path):
    lines = path.read_text().splitlines()
    failures, checked, tables = [], 0, 0
    for n, line in enumerate(lines):
        if not line.lstrip().startswith(ANCHOR):
            continue
        end = n + 1
        while end < len(lines) and "-->" not in lines[end]:
            end += 1
        records = [l.strip() for l in lines[n + 1:end] if l.strip() and not l.strip().startswith("-->")]
        rows = rows_after(lines, end + 1)
        if len(rows) != len(records):
            failures.append(f"line {n + 1}: {len(records)} record(s) but {len(rows)} row(s)")
            continue
        tables += 1
        for record, cells in zip(records, rows):
            where = f"line {n + 1} table row {cells[0]!r} vs {record}"
            try:
                data = json.loads((ROOT / record).read_text())
            except OSError as exc:
                failures.append(f"{where}: {exc}")
                continue
            values = numbers(data, [])
            for cell in cells[1:]:
                for token in re.findall(r"\d[\d,]*\.?\d*", cell):
                    checked += 1
                    if matches(token, values) is None:
                        failures.append(
                            f"{where}: cell {cell!r} quotes {token}, which is not in the "
                            f"record ({TOLERANCE_NOTE})")
    return failures, checked, tables


def main() -> int:
    path = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "docs/PHASE5.md"
    failures, checked, tables = check(path)
    for f in failures:
        print(f"FAIL {f}", file=sys.stderr)
    if failures:
        print(f"{path}: {len(failures)} mismatched number(s) in {tables} table(s)", file=sys.stderr)
        return 1
    print(f"{path}: {checked} quoted number(s) in {tables} table(s) all match their records")
    return 0


if __name__ == "__main__":
    sys.exit(main())
