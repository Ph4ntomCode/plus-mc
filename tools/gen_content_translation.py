#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates canonical 26.3 -> 26.2 / 26.1.2 translated-content id maps.

The registries fall into two groups:

* Built-in registries (items, entity types, data component types, potions and
  mob effects) are not synchronized in the configuration registry set, so
  their ids come from each release's own data-generator
  ``reports/registries.json``.
* Datapack registries (enchantments and the two trim registries) are
  synchronized, and their wire ids are the index in each release's captured
  client-config registry list (``clientconfig/registries/<name>.txt``).

The arrays are indexed by the canonical 26.3 id and store the target release's
wire id. ``-1`` means the entry does not exist in that release; callers must
drop that item/component/effect rather than send an id the client would resolve
as something else.

Usage:
    gen_content_translation.py SOURCE_REPORT TARGET_26_2_REPORT \
        TARGET_26_1_2_REPORT SOURCE_CLIENTCONFIG TARGET_26_2_CLIENTCONFIG \
        TARGET_26_1_2_CLIENTCONFIG OUT_HEADER
"""

import json
import os
import sys


REPORT_REGISTRIES = (
    "minecraft:item",
    "minecraft:entity_type",
    "minecraft:data_component_type",
    "minecraft:potion",
    "minecraft:mob_effect",
)

CONFIG_REGISTRIES = (
    "enchantment",
    "trim_material",
    "trim_pattern",
)


def load_report(path):
    report = json.load(open(path))
    out = {}
    for registry in REPORT_REGISTRIES:
        entries = report[registry]["entries"]
        by_name = {name: int(entry["protocol_id"]) for name, entry in entries.items()}
        if sorted(by_name.values()) != list(range(len(by_name))):
            raise SystemExit(f"{path}: {registry} ids are not contiguous")
        out[registry] = by_name
    return out


def load_clientconfig(path):
    out = {}
    for registry in CONFIG_REGISTRIES:
        names = []
        with open(os.path.join(path, "registries", registry + ".txt")) as handle:
            for line in handle:
                name = line.strip()
                if name:
                    names.append(name)
        if len(set(names)) != len(names):
            raise SystemExit(f"{path}: {registry} has duplicate entries")
        by_name = {name: i for i, name in enumerate(names)}
        if sorted(by_name.values()) != list(range(len(by_name))):
            raise SystemExit(f"{path}: {registry} ids are not contiguous")
        out[registry] = by_name
    return out


def load(report_path, config_path):
    report = load_report(report_path)
    config = load_clientconfig(config_path)
    return {**report, **config}


def names_by_id(registry):
    """The report's entries are a JSON object in alphabetical order; the
    protocol id is the only thing that defines the index, so rebuild the list
    by id rather than trusting insertion order. Config registries are already
    indexed by their capture order."""
    names = [None] * len(registry)
    for name, registry_id in registry.items():
        names[registry_id] = name
    if any(name is None for name in names):
        raise SystemExit("registry ids are not contiguous")
    return names


def map_registry(source, target, registry):
    target_ids = target[registry]
    out = [target_ids.get(name, -1) for name in names_by_id(source[registry])]
    missing = sum(1 for value in out if value < 0)
    exact = len(out) - missing
    print(f"{registry}: {exact} exact, {missing} missing")
    return out


def emit_array(name, values):
    out = [f"inline constexpr int32_t {name}[] = {{"]
    for i in range(0, len(values), 16):
        row = ", ".join(str(v) for v in values[i:i + 16])
        out.append(f"    {row},")
    out.append("};")
    return out


def main():
    if len(sys.argv) != 8:
        print(__doc__)
        return 2
    source_report, target_262_report, target_2612_report, source_config, \
        target_262_config, target_2612_config, out_path = sys.argv[1:]

    source = load(source_report, source_config)
    target_262 = load(target_262_report, target_262_config)
    target_2612 = load(target_2612_report, target_2612_config)

    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// clang-format off",
        "//",
        "// 26.3 -> 26.2 / 26.1.2 translated-content registry id maps. Generated",
        "// by tools/gen_content_translation.py from each release's own",
        "// reports/registries.json and client-config registry captures; do not",
        "// hand-edit.",
        "//",
        "// The arrays are indexed by the canonical 26.3 id and store the target",
        "// id, or -1 when the target release has no entry with that name.",
        "//",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace mc::translation::t0 {",
    ]
    for registry, prefix in (
            ("minecraft:item", "Item"),
            ("minecraft:entity_type", "EntityType"),
            ("minecraft:data_component_type", "DataComponent"),
            ("minecraft:potion", "Potion"),
            ("minecraft:mob_effect", "MobEffect"),
            ("enchantment", "Enchantment"),
            ("trim_material", "TrimMaterial"),
            ("trim_pattern", "TrimPattern")):
        count = len(source[registry])
        lines.append(f"inline constexpr int32_t kCanonical{prefix}Count = {count};")
        lines.append("")
        for target, suffix in ((target_262, "262"), (target_2612, "2612")):
            values = map_registry(source, target, registry)
            if len(values) != count:
                raise SystemExit(f"{registry}: source count changed")
            lines += emit_array(f"kCanonical{prefix}To{suffix}", values)
            lines.append("")
    lines += [
        "} // namespace mc::translation::t0",
        "",
    ]
    open(out_path, "w").write("\n".join(lines))
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
