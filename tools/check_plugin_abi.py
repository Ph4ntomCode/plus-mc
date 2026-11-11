#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that pluginapi/plugin.h has not broken ABI 1.

The native plugin C ABI is append-only. A plugin compiled against an older
header must keep loading: fields may be added at the end, enum values may be
added, and the host table may grow, but no existing field, type, order or enum
value may change. `src/pluginapi/abi-v1.json` is the reviewed baseline; this
tool fails when the header stops being a superset of it.

A deliberate break means a new `PMC_PLUGIN_ABI_VERSION`, a new baseline file and
a compatibility fixture. Do not use `--update` to make a red break green.

Run:
    tools/check_plugin_abi.py
    tools/check_plugin_abi.py --update     # only for a reviewed additive change
    tests/plugin_abi_test.py              # the checker's own negative tests
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "pluginapi" / "plugin.h"
BASELINE = ROOT / "src" / "pluginapi" / "abi-v1.json"

_COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
_DEFINE = re.compile(r"^\s*#define\s+(PMC_[A-Z0-9_]+)\s+(\d+)[uUlL]*\s*$", re.M)
_STRUCT = re.compile(r"typedef\s+struct\s+(\w+)\s*\{(.*?)\}\s*\1\s*;", re.S)
_ENUM = re.compile(r"enum\s+(\w+)\s*\{(.*?)\}\s*;", re.S)
_FIELD_NAME = re.compile(r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?$")
_FUNCTION_NAME = re.compile(r"\(\*\s*([A-Za-z_]\w*)\s*\)")


class HeaderError(ValueError):
    pass


def _normalise(declaration: str) -> str:
    """One spelling for comparison; comments are already gone."""
    declaration = " ".join(declaration.split())
    return re.sub(r"\s*([*(),\[\]])\s*", r"\1", declaration)


def _parse_struct(name: str, body: str) -> list[dict[str, str]]:
    fields: list[dict[str, str]] = []
    for raw in body.split(";"):
        declaration = _normalise(raw)
        if not declaration:
            continue
        function = _FUNCTION_NAME.search(declaration)
        if function:
            field_name = function.group(1)
        else:
            match = _FIELD_NAME.search(declaration)
            if match is None:
                raise HeaderError(f"{name}: cannot read field declaration {declaration!r}")
            field_name = match.group(1)
        fields.append({"name": field_name, "decl": declaration})
    return fields


def _parse_enum(name: str, body: str) -> dict[str, int]:
    values: dict[str, int] = {}
    next_value = 0
    for raw in body.split(","):
        item = raw.strip()
        if not item:
            continue
        match = re.match(r"([A-Za-z_]\w*)\s*(?:=\s*(.+))?$", item, re.S)
        if match is None:
            raise HeaderError(f"{name}: cannot read enum item {item!r}")
        item_name = match.group(1)
        if match.group(2) is not None:
            expression = match.group(2).strip()
            try:
                next_value = int(expression, 0)
            except ValueError as exc:
                raise HeaderError(f"{name}.{item_name}: non-integer value {expression!r}") from exc
        values[item_name] = next_value
        next_value += 1
    return values


def parse_header(text: str) -> dict:
    text = _COMMENT.sub("", text)
    defines = {name: int(value) for name, value in _DEFINE.findall(text)}
    structs = {name: _parse_struct(name, body) for name, body in _STRUCT.findall(text)}
    enums = {name: _parse_enum(name, body) for name, body in _ENUM.findall(text)}
    for required in ("PMC_PLUGIN_ABI_VERSION", "PMC_PLUGIN_MANIFEST_API"):
        if required not in defines:
            raise HeaderError(f"missing #define {required}")
    return {
        "abi_version": defines["PMC_PLUGIN_ABI_VERSION"],
        "manifest_api": defines["PMC_PLUGIN_MANIFEST_API"],
        "structs": structs,
        "enums": enums,
    }


def baseline_from(parsed: dict) -> dict:
    return {
        "abi_version": parsed["abi_version"],
        "manifest_api": parsed["manifest_api"],
        "structs": parsed["structs"],
        "enums": parsed["enums"],
    }


def compare(baseline: dict, current: dict) -> list[str]:
    errors: list[str] = []
    if current["abi_version"] != baseline["abi_version"]:
        errors.append(
            f"PMC_PLUGIN_ABI_VERSION is {current['abi_version']} but the baseline is "
            f"{baseline['abi_version']}; a break needs a new baseline and compatibility fixture"
        )
    if current["manifest_api"] != baseline["manifest_api"]:
        errors.append(
            f"PMC_PLUGIN_MANIFEST_API is {current['manifest_api']} but the baseline is "
            f"{baseline['manifest_api']}"
        )
    for struct_name, expected in baseline["structs"].items():
        actual = current["structs"].get(struct_name)
        if actual is None:
            errors.append(f"struct {struct_name} is missing")
            continue
        for index, field in enumerate(expected):
            if index >= len(actual):
                errors.append(f"{struct_name}: field {index} {field['name']!r} is missing")
                continue
            if actual[index] != field:
                found = actual[index]
                errors.append(
                    f"{struct_name}: field {index} was {field['decl']!r} "
                    f"({field['name']}), now {found['decl']!r} ({found['name']})"
                )
    for enum_name, expected in baseline["enums"].items():
        actual = current["enums"].get(enum_name, {})
        for name, value in expected.items():
            if name not in actual:
                errors.append(f"enum {enum_name}.{name} is missing")
            elif actual[name] != value:
                errors.append(
                    f"enum {enum_name}.{name} was {value}, now {actual[name]}"
                )
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header", type=Path, default=HEADER)
    parser.add_argument("--baseline", type=Path, default=BASELINE)
    parser.add_argument("--update", action="store_true",
                        help="rewrite the baseline from the header (reviewed additive changes only)")
    args = parser.parse_args(argv)
    try:
        current = parse_header(args.header.read_text(encoding="utf-8"))
    except OSError as exc:
        print(f"plugin ABI: cannot read {args.header}: {exc}", file=sys.stderr)
        return 2
    except HeaderError as exc:
        print(f"plugin ABI: {args.header}: {exc}", file=sys.stderr)
        return 2

    if args.update:
        args.baseline.write_text(
            json.dumps(baseline_from(current), indent=2) + "\n", encoding="utf-8"
        )
        print(f"plugin ABI: wrote {args.baseline} (ABI {current['abi_version']})")
        return 0

    try:
        baseline = json.loads(args.baseline.read_text(encoding="utf-8"))
    except OSError as exc:
        print(f"plugin ABI: cannot read {args.baseline}: {exc}", file=sys.stderr)
        return 2
    except json.JSONDecodeError as exc:
        print(f"plugin ABI: {args.baseline}: invalid JSON: {exc}", file=sys.stderr)
        return 2

    errors = compare(baseline, current)
    if errors:
        print("plugin ABI: the header broke the frozen baseline:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    host_fields = len(current["structs"].get("pmc_host_api", []))
    struct_fields = sum(len(fields) for fields in current["structs"].values())
    enum_values = sum(len(values) for values in current["enums"].values())
    print(
        f"plugin ABI ok: ABI {current['abi_version']}, {host_fields} host entries, "
        f"{struct_fields} struct fields, {enum_values} enum values"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
