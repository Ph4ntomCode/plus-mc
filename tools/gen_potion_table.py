#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates src/world/potion_table.h — the 26.2 minecraft:potion registry.

Two outputs, one per input mode (the same shape as gen_effect_table.py):

  --reports DIR --java-dir DIR
                  the authoritative mode. `DIR` is the vanilla data generator's
                  reports directory (it holds registries.json, which carries
                  every potion's protocol_id — the id a potion_contents
                  component carries, because `Potion.STREAM_CODEC` is
                  `ByteBufCodecs.holderRegistry`). `DIR` for the Java sources is
                  the decompiled net/minecraft/world/item/alchemy directory: a
                  potion's effects are `new Potion("healing", new
                  MobEffectInstance(MobEffects.INSTANT_HEALTH, 1))` — constructor
                  arguments, not data — so Potions.java and PotionIds.java are
                  the only places they exist (Potions.java names the *field*
                  whose key PotionIds.java declares, and the two disagree for the
                  long_/strong_ variants: "long_night_vision" and the "night_vision"
                  display name are different strings). Writes both the flattened
                  extraction (data/vanilla/26.2/potions.tsv) and the header.
  --tsv FILE      rebuilds only the header from the committed extraction, so
                  anyone can regenerate the header without the reports or the
                  decompiled sources.

The parser is deliberately strict, like gen_effect_table.py's: it matches one
field declaration per line and *refuses to run* on a registration line it cannot
account for, so a future version whose lines change shape fails the generator
instead of silently dropping a potion's effects. It also refuses a potion that
registries.json knows and Potions.java does not, and vice versa, and insists the
ids are dense 0..N-1 (the header indexes the table by id).

Usage:
    tools/gen_potion_table.py --reports build/_dl/boot/generated/reports \\
        --java-dir build/_decomp/net/minecraft/world/item/alchemy
    tools/gen_potion_table.py --tsv data/vanilla/26.2/potions.tsv
"""

import argparse
import json
import pathlib
import re
import sys

_NS = "minecraft:"
_POTION_REGISTRY = "minecraft:potion"
_EFFECT_REGISTRY = "minecraft:mob_effect"

_TSV_HEADER = "name\tid\teffect\tduration\tamplifier"

# `public static final ResourceKey<Potion> LONG_NIGHT_VISION = PotionIds
# .register("long_night_vision");` — the field name is what Potions.java refers
# to and the string is the registry key, and they are not the same string.
_JAVA_ID = re.compile(
    r'^\s*public static final ResourceKey<Potion> (\w+) = PotionIds\.register\("([a-z_]+)"\);')

# `public static final Holder<Potion> HEALING = Potions.register(PotionIds
# .HEALING, new Potion("healing", new MobEffectInstance(MobEffects
# .INSTANT_HEALTH, 1)));`
_JAVA_REGISTER = re.compile(
    r'^\s*public static final Holder<Potion> (\w+) = Potions\.register\('
    r'PotionIds\.(\w+), new Potion\("([a-z_]+)"(?:, (.+))?\)\);')

# `new MobEffectInstance(MobEffects.SPEED, 3600)` / `..., 1800, 1)` — the two
# optional arguments are the duration in ticks and the amplifier, in that order.
_INSTANCE = re.compile(r'^new MobEffectInstance\(MobEffects\.(\w+), (\d+)(?:, (\d+))?\)$')

# The four potions with no effects are written as the empty *array* rather than
# as no argument at all: `new Potion("water", new MobEffectInstance[0])`.
_NO_EFFECTS = "new MobEffectInstance[0]"


def parse_ids(text: str) -> dict:
    """PotionIds.java -> {java field name: registry key name}."""
    keys = {}
    for line in text.splitlines():
        if "PotionIds.register(" not in line:
            continue
        match = _JAVA_ID.match(line)
        if not match:
            raise SystemExit(f"unparsed PotionIds line: {line.strip()}")
        keys[match.group(1)] = match.group(2)
    if not keys:
        raise SystemExit("no PotionIds registrations found")
    return keys


def parse_potions(text: str, keys: dict, effect_ids: dict) -> list:
    """Potions.java -> [{name, effects: [(effect, duration, amplifier)]}]."""
    potions = []
    for line in text.splitlines():
        if "Potions.register(" not in line:
            continue
        match = _JAVA_REGISTER.match(line)
        if not match:
            raise SystemExit(f"unparsed Potions line: {line.strip()}")
        field, key, display, tail = match.groups()
        if key not in keys:
            raise SystemExit(f"{field}: PotionIds.{key} is not declared in PotionIds.java")
        effects = []
        if tail is not None and tail != _NO_EFFECTS:
            for part in _split_instances(tail):
                instance = _INSTANCE.match(part)
                if not instance:
                    raise SystemExit(f"{field}: unparsed effect argument '{part}'")
                effect, duration, amplifier = instance.groups()
                # The effect is only known to the registry dump, which is also
                # where its wire id comes from; an effect that is not there is a
                # version mismatch, not something to guess at.
                if _NS + _snake(effect) not in effect_ids:
                    raise SystemExit(f"{field}: unknown effect MobEffects.{effect}")
                effects.append((effect_ids[_NS + _snake(effect)], int(duration),
                                int(amplifier or 0)))
        potions.append({"name": _NS + keys[key], "display": _NS + display,
                        "field": field, "effects": effects})
    if not potions:
        raise SystemExit("no Potions registrations found")
    return potions


def _snake(java_constant: str) -> str:
    """MOB_EFFECT constants are already snake case, but a leading kind word can
    differ: MobEffects.INSTANT_HEALTH -> instant_health, MobEffects.JUMP_BOOST ->
    jump_boost. The registry keys are the lowercased constant."""
    return java_constant.lower()


def _split_instances(tail: str) -> list:
    """Splits the effect arguments on the commas *between* instances: every
    `new MobEffectInstance(...)` is one argument, so a depth counter is what
    keeps `..., 1800, 1)` from being split mid-instance."""
    parts, depth, current = [], 0, ""
    for char in tail:
        if char == "," and depth == 0:
            parts.append(current.strip())
            current = ""
            continue
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
        current += char
    if current.strip():
        parts.append(current.strip())
    return parts


def rows_from_reports(reports: pathlib.Path, alchemy: pathlib.Path) -> list:
    registries = json.loads((reports / "registries.json").read_text())
    entries = registries[_POTION_REGISTRY]["entries"]
    effect_ids = {name: meta["protocol_id"]
                  for name, meta in registries[_EFFECT_REGISTRY]["entries"].items()}

    keys = parse_ids((alchemy / "PotionIds.java").read_text())
    potions = parse_potions((alchemy / "Potions.java").read_text(), keys, effect_ids)

    seen = {p["name"] for p in potions}
    missing = sorted(set(entries) - seen)
    extra = sorted(seen - set(entries))
    if missing or extra:
        raise SystemExit(f"registry drift: missing {missing}, unknown {extra}")

    rows = []
    for potion in potions:
        rows.append({**potion, "id": entries[potion["name"]]["protocol_id"]})
    rows.sort(key=lambda r: r["id"])
    # The header is indexed by id, so a gap would silently mis-address every
    # potion after it rather than fail to compile.
    for index, row in enumerate(rows):
        if row["id"] != index:
            raise SystemExit(f"potion ids are not dense: {row['name']} is {row['id']} "
                             f"at index {index}")
    return rows


def read_tsv(path: pathlib.Path) -> list:
    rows, names = [], {}
    for line in path.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        if line == _TSV_HEADER:
            continue
        name, ident, effect, duration, amplifier = line.split("\t")
        if name not in names:
            names[name] = {"name": name, "id": int(ident), "effects": []}
        row = names[name]
        if row["id"] != int(ident):
            raise SystemExit(f"{path}: {name} is listed with two ids")
        if effect == "-":
            continue
        row["effects"].append((int(effect), int(duration), int(amplifier)))
    rows = sorted(names.values(), key=lambda r: r["id"])
    for index, row in enumerate(rows):
        if row["id"] != index:
            raise SystemExit(f"{path}: potion ids are not dense at {row['name']}")
    return rows


def render_tsv(rows: list) -> str:
    lines = [
        "# Generated by tools/gen_potion_table.py --reports ... --java-dir ...",
        "#",
        "# minecraft:potion is a registry whose ids are synchronized to the client",
        "# (`Potion.STREAM_CODEC` is ByteBufCodecs.holderRegistry), so a potion id",
        "# is its protocol_id from the data generator's registries.json. The",
        "# effects themselves are constructor arguments in Potions.java, so each",
        "# row below is one (effect, duration, amplifier) of one potion, and a",
        "# potion with no effects at all (water, mundane, thick, awkward) carries",
        "# a single row with '-' in the three effect columns.",
        _TSV_HEADER,
    ]
    for row in rows:
        if not row["effects"]:
            lines.append("\t".join([row["name"], str(row["id"]), "-", "-", "-"]))
            continue
        for effect, duration, amplifier in row["effects"]:
            lines.append("\t".join([row["name"], str(row["id"]), str(effect),
                                    str(duration), str(amplifier)]))
    return "\n".join(lines) + "\n"


def render(rows: list) -> str:
    effects = sum(len(row["effects"]) for row in rows)
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "//",
        f"// The {len(rows)} 26.2 minecraft:potion entries and their {effects} effects, in",
        "// registry-id order (generated — do not hand-edit).",
        "//",
        "// minecraft:potion is a registry whose ids are synchronized to the client",
        "// (`Potion.STREAM_CODEC` is ByteBufCodecs.holderRegistry, which writes the",
        "// raw registry id), so a potion_contents component's `potion` field is the",
        "// protocol_id the data generator's registries.json assigns. The table",
        "// index *is* that id (the generator checks the ids are dense 0..N-1).",
        "//",
        "// A potion's effects are constructor arguments rather than data —",
        "// `new Potion(\"healing\", new MobEffectInstance(MobEffects.INSTANT_HEALTH, 1))`",
        "// — so the entries come from parsing the decompiled",
        "// net/minecraft/world/item/alchemy/Potions.java, with the registry key read",
        "// from its PotionIds.java (which is where long_/strong_ variants get the",
        "// key that `Potion.name()` does not repeat). The two numeric arguments are",
        "// the duration in ticks and the amplifier; ambient, visible and show_icon",
        "// are `MobEffectInstance`'s own constructor defaults (false/true/true) for",
        "// every potion in the vanilla registry, so they are not columns here.",
        "//",
        "// Regenerate with tools/gen_potion_table.py.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace mc::world {",
        "",
        "struct GeneratedPotionEffect {",
        "  int32_t effect;    // the minecraft:mob_effect registry id",
        "  int32_t duration;  // ticks",
        "  int32_t amplifier;",
        "};",
        "",
        "struct GeneratedPotion {",
        "  const char* name;  // \"minecraft:healing\"",
        "  int32_t id;",
        "  // The potion's effects in the order `Potion.getEffects()` returns them,",
        "  // which is the order a drink applies them.",
        "  int32_t first;",
        "  int32_t count;",
        "};",
        "",
        "// clang-format off",
        "inline constexpr GeneratedPotion kPotions[] = {",
    ]
    for row in rows:
        first = 0
        if row["effects"]:
            first = sum(len(r["effects"]) for r in rows if r["id"] < row["id"])
        lines.append(f'    {{"{row["name"]}", {row["id"]}, {first}, {len(row["effects"])}}},')
    lines += [
        "};",
        f"inline constexpr int kPotionCount = {len(rows)};",
        "",
        "inline constexpr GeneratedPotionEffect kPotionEffects[] = {",
    ]
    for row in rows:
        for effect, duration, amplifier in row["effects"]:
            lines.append(f"    {{{effect}, {duration}, {amplifier}}},  // {row['name']}")
    lines += [
        "};",
        f"inline constexpr int kPotionEffectCount = {effects};",
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
                        help="vanilla data generator reports directory")
    parser.add_argument("--java-dir", type=pathlib.Path,
                        help="decompiled net/minecraft/world/item/alchemy directory")
    parser.add_argument("--tsv", type=pathlib.Path,
                        default=pathlib.Path("data/vanilla/26.2/potions.tsv"),
                        help="the flattened extraction (written in --reports mode)")
    parser.add_argument("--header", type=pathlib.Path,
                        default=pathlib.Path("src/world/potion_table.h"))
    args = parser.parse_args()

    if args.reports:
        if not args.java_dir:
            raise SystemExit("--reports needs --java-dir (the effects are code-only)")
        rows = rows_from_reports(args.reports, args.java_dir)
        args.tsv.parent.mkdir(parents=True, exist_ok=True)
        args.tsv.write_text(render_tsv(rows))
        effects = sum(len(row["effects"]) for row in rows)
        print(f"wrote {args.tsv} ({len(rows)} potions, {effects} effects)")
    else:
        rows = read_tsv(args.tsv)

    args.header.parent.mkdir(parents=True, exist_ok=True)
    args.header.write_text(render(rows))
    print(f"wrote {args.header} ({len(rows)} potions)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
