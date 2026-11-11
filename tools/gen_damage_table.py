#!/usr/bin/env python3
"""Generates src/world/damage_table.h from the extracted vanilla data.

Sources, all of them data rather than code:

  data/minecraft/damage_type/*.json        the 51 damage types: message_id,
                                           scaling, exhaustion, effects,
                                           death_message_type
  data/minecraft/tags/damage_type/*.json   the tags the damage pipeline gates
                                           on (bypasses_armor, is_fire, ...),
                                           flattened to one bit per tag
  clientconfig/registries/damage_type.txt  the wire order the client is told
                                           (`minecraft:damage_type` is a
                                           *synchronized* registry, so the
                                           packet's holder id is this index)

Unlike items and attributes the ids are NOT a built-in registration order:
damage types are a datapack registry, loaded from the files above. The
captured order file is what the client was actually told, and the generator
refuses to run if it and the json directory disagree — a mismatch would put a
damage type on the wire with the wrong id, which reads as a different type.

Usage:
  tools/gen_damage_table.py \
      --data data/vanilla/26.2/data \
      --order data/vanilla/26.2/clientconfig/registries/damage_type.txt
"""

import argparse
import json
import pathlib
import sys

_NAMESPACE = "minecraft:"


def load_jsons(directory: pathlib.Path) -> dict:
    """{name: parsed json} for every file in `directory`."""
    out = {}
    for path in sorted(directory.glob("*.json")):
        name = _NAMESPACE + path.stem
        out[name] = json.loads(path.read_text())
    return out


def expand_tag(tag: str, tags: dict, seen: set) -> set:
    """A tag's direct members, with `#other_tag` references resolved."""
    if tag in seen:
        return set()
    seen.add(tag)
    values = tags[tag]["values"]
    out = set()
    for value in values:
        if value.startswith("#"):
            out |= expand_tag(value[1:], tags, seen)
        else:
            out.add(value)
    return out


def rows(damage_types: dict, tags: dict, order: list) -> list:
    if sorted(damage_types) != sorted(order):
        missing = sorted(set(damage_types) - set(order))
        extra = sorted(set(order) - set(damage_types))
        raise SystemExit(f"damage_type wire order disagrees with the data: "
                         f"missing={missing} extra={extra}")
    tag_bits = {tag: i for i, tag in enumerate(sorted(tags))}
    expanded = {tag: expand_tag(tag, tags, set()) for tag in tags}
    rows = []
    for name in order:
        d = damage_types[name]
        bits = 0
        for tag, bit in tag_bits.items():
            if name in expanded[tag]:
                bits |= 1 << bit
        rows.append((name,
                     d["message_id"],
                     d.get("scaling", "when_caused_by_living_non_player"),
                     float(d.get("exhaustion", 0.0)),
                     d.get("effects", "hurt"),
                     d.get("death_message_type", "default"),
                     bits))
    return rows, tag_bits


def render(rows, tag_bits: dict) -> str:
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "//",
        f"// The {len(rows)} 26.2 vanilla damage types, in wire order (generated — do not",
        "// hand-edit).",
        "//",
        "// `id` is the index this type has in the minecraft:damage_type registry, which",
        "// is a *synchronized* registry: the client is sent the same order during the",
        "// configuration phase (clientconfig/registries/damage_type.txt), so the id is",
        "// both a wire holder id (damage_event) and the lookup key here.",
        "//",
        "// `tags` is a bitmask over the minecraft:damage_type tags — the vanilla damage",
        "// pipeline gates on tags, not on names (getDamageAfterArmorAbsorb checks",
        "// BYPASSES_ARMOR, is_fire gates igniting, and so on). Sixty-four bits, not",
        "// thirty-two: vanilla ships 34 tags and `1u << 32` would be undefined.",
        "//",
        "// Regenerate with tools/gen_damage_table.py.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace mc::world {",
        "",
        "// DamageScaling: whether the (difficulty-scaled) damage amount applies.",
        "constexpr uint8_t kDamageScalingNever = 0;",
        "constexpr uint8_t kDamageScalingWhenCausedByLivingNonPlayer = 1;",
        "constexpr uint8_t kDamageScalingAlways = 2;",
        "",
        "// DeathMessageType: which lang key the death message is built from.",
        "constexpr uint8_t kDeathMessageDefault = 0;",
        "constexpr uint8_t kDeathMessageFallVariants = 1;",
        "constexpr uint8_t kDeathMessageIntentionalGameDesign = 2;",
        "",
        "// minecraft:damage_type tag bits, in sorted-tag order.",
    ]
    for tag, bit in sorted(tag_bits.items(), key=lambda kv: kv[1]):
        camel = "".join(p.capitalize() for p in tag.split(":", 1)[-1].split("_"))
        lines.append(f"constexpr uint64_t kDamageTag{camel} = 1ull << {bit};")
    lines += [
        "",
        "struct GeneratedDamageType {",
        "  const char* name;               // minecraft:fall",
        "  const char* message_id;         // fall (death.attack.<message_id>)",
        "  uint8_t scaling;                // kDamageScaling*",
        "  uint8_t death_message_type;     // kDeathMessage*",
        "  const char* effects;            // hurt|drowning|burning|poking|freezing",
        "  float exhaustion;               // Player.causeFoodExhaustion per hit",
        "  uint64_t tags;                  // kDamageTag* bits",
        "};",
        "",
        "// clang-format off",
        "inline constexpr GeneratedDamageType kDamageTypes[] = {",
    ]
    scaling = {
        "never": "kDamageScalingNever",
        "when_caused_by_living_non_player": "kDamageScalingWhenCausedByLivingNonPlayer",
        "always": "kDamageScalingAlways",
    }
    death = {
        "default": "kDeathMessageDefault",
        "fall_variants": "kDeathMessageFallVariants",
        "intentional_game_design": "kDeathMessageIntentionalGameDesign",
    }
    for name, msg, scale, exhaustion, effects, death_type, bits in rows:
        # `0f` is not a float literal (it reads as an octal constant), so keep a
        # decimal point on whole numbers.
        amount = f"{exhaustion:.7g}"
        if "." not in amount and "e" not in amount and "E" not in amount:
            amount += ".0"
        lines.append(f'    {{"{name}", "{msg}", {scaling[scale]}, {death[death_type]}, '
                     f'"{effects}", {amount}f, 0x{bits:016x}ull}},')
    lines += [
        "};",
        f"inline constexpr int kDamageTypeCount = {len(rows)};",
        "// clang-format on",
        "",
        "} // namespace mc::world",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data", type=pathlib.Path,
                        default=pathlib.Path("data/vanilla/26.2/data"),
                        help="extracted data tree (holds minecraft/damage_type)")
    parser.add_argument("--order", type=pathlib.Path,
                        default=pathlib.Path(
                            "data/vanilla/26.2/clientconfig/registries/damage_type.txt"),
                        help="captured wire order of the synchronized registry")
    parser.add_argument("--header", type=pathlib.Path,
                        default=pathlib.Path("src/world/damage_table.h"))
    args = parser.parse_args()

    mc = args.data / "minecraft"
    damage_types = load_jsons(mc / "damage_type")
    tags = load_jsons(mc / "tags" / "damage_type")
    order = [line.strip() for line in args.order.read_text().splitlines() if line.strip()]
    rows_out, tag_bits = rows(damage_types, tags, order)

    args.header.parent.mkdir(parents=True, exist_ok=True)
    args.header.write_text(render(rows_out, tag_bits))
    print(f"wrote {args.header} ({len(rows_out)} damage types, {len(tag_bits)} tags)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
