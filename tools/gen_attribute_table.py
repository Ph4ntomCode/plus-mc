#!/usr/bin/env python3
"""Generates src/world/attribute_table.h from the vanilla generator's reports.

`minecraft:attribute` is a *static* registry — it is registered in code
(`Attributes`' static initialisers) and is NOT part of the synchronized
registries the client is sent during configuration. So the id in
ClientboundUpdateAttributesPacket's AttributeSnapshot is the index in the
report's protocol_id order, which is also the order the client's own copy has.
That is fragile enough to be worth generating rather than hand-copying: a wrong
id makes the client apply the value to a different attribute.

Usage:
  tools/gen_attribute_table.py --reports build/_dl/boot/generated/reports
"""

import argparse
import json
import pathlib
import sys

_REGISTRY = "minecraft:attribute"


def rows(reports: pathlib.Path):
    entries = json.loads((reports / "registries.json").read_text())[_REGISTRY]["entries"]
    return sorted((meta["protocol_id"], name) for name, meta in entries.items())


def render(rows_out) -> str:
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "//",
        f"// The {len(rows_out)} 26.2 minecraft:attribute registry ids (generated — do not",
        "// hand-edit).",
        "//",
        "// Source: reports/registries.json (RegistryDumpReport). Unlike damage types",
        "// this registry is *static*: it is registered in code and the client is never",
        "// sent it, so the id in an update_attributes AttributeSnapshot is this index",
        "// and nothing else. Regenerate with tools/gen_attribute_table.py.",
        "#pragma once",
        "",
        "namespace mc::world {",
        "",
        "struct GeneratedAttribute {",
        "  const char* name;  // minecraft:max_health",
        "  int32_t id;        // registry index == the wire id",
        "};",
        "",
        "// clang-format off",
        "inline constexpr GeneratedAttribute kAttributes[] = {",
    ]
    for protocol_id, name in rows_out:
        lines.append(f'    {{"{name}", {protocol_id}}},')
    lines += [
        "};",
        f"inline constexpr int kAttributeCount = {len(rows_out)};",
        "// clang-format on",
        "",
        "} // namespace mc::world",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--reports", type=pathlib.Path,
                        default=pathlib.Path("build/_dl/boot/generated/reports"),
                        help="vanilla data generator reports directory")
    parser.add_argument("--header", type=pathlib.Path,
                        default=pathlib.Path("src/world/attribute_table.h"))
    args = parser.parse_args()

    rows_out = rows(args.reports)
    args.header.parent.mkdir(parents=True, exist_ok=True)
    args.header.write_text(render(rows_out))
    print(f"wrote {args.header} ({len(rows_out)} attributes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
