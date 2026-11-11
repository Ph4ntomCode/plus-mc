#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates src/world/brewing_table.h — every 26.2 brewing stand mix.

Brewing is *code*, not a datapack in this version: `PotionBrewing.addVanillaMixes`
builds three lists — the allowed containers, the container mixes (a potion plus
gunpowder becomes a splash potion) and the potion mixes — and each entry is an
item or a `Potions` field, never a resource path. So the authoritative source is
that one Java method, transcribed here in its exact order (a lookup takes the
first matching entry, and `addStartMix` inserts *two* mixes, one of which is
always `water -> mundane`; the order is preserved verbatim rather than sorted).

Two outputs, one per input mode (the same shape as gen_potion_table.py):

  --reports DIR --java-dir DIR --item-header FILE --fuel-tag FILE
                  the authoritative mode. `DIR` is the vanilla data generator's
                  reports directory (registries.json carries every potion's
                  protocol_id, which is what a potion_contents component
                  writes). `DIR` for the Java source is the decompiled
                  net/minecraft/world/item/alchemy directory (PotionBrewing.java
                  for the mixes, PotionIds.java for the registry key of each
                  `Potions` field, because a field name and its key are not
                  always the same string). `--item-header` is the generated item
                  table the item ids come from, and `--fuel-tag` the vanilla
                  `minecraft:brewing_fuel` item tag (blaze powder), which is
                  what BrewingStandBlockEntity accepts in its fuel slot and
                  burns 20 uses out of. Writes both the flattened extraction
                  (data/vanilla/26.2/brewing.tsv) and the header.
  --tsv FILE      rebuilds only the header from the committed extraction, so
                  anyone can regenerate the header without the reports or the
                  decompiled sources.

The parser is deliberately strict, like gen_potion_table.py's: it *refuses to
run* on a `builder.` line it cannot account for, on a potion or item the
registries do not know, and on a fuel tag whose values are not plain items. A
future version whose lines change shape fails the generator instead of silently
dropping a mix.

Usage:
    tools/gen_brewing_table.py --reports build/_dl/boot/generated/reports \\
        --java-dir build/_decomp/net/minecraft/world/item/alchemy \\
        --item-header src/world/item_registry.h \\
        --fuel-tag data/vanilla/26.2/data/minecraft/tags/item/brewing_fuel.json
    tools/gen_brewing_table.py --tsv data/vanilla/26.2/brewing.tsv
"""

import argparse
import json
import pathlib
import re
import sys

_NS = "minecraft:"
_POTION_REGISTRY = "minecraft:potion"

_TSV_HEADER = "kind\ta\tb\tc"
_KIND_CONTAINER = "container"
_KIND_CONTAINER_MIX = "container_mix"
_KIND_POTION_MIX = "potion_mix"
_KIND_FUEL = "fuel"

# `builder.addContainer(Items.POTION);`
_CONTAINER = re.compile(r'^\s*builder\.addContainer\(Items\.(\w+)\);$')
# `builder.addContainerRecipe(Items.POTION, Items.GUNPOWDER, Items.SPLASH_POTION);`
_CONTAINER_MIX = re.compile(
    r'^\s*builder\.addContainerRecipe\(Items\.(\w+), Items\.(\w+), Items\.(\w+)\);$')
# `builder.addMix(Potions.WATER, Items.REDSTONE, Potions.MUNDANE);`
_POTION_MIX = re.compile(
    r'^\s*builder\.addMix\(Potions\.(\w+), Items\.(\w+), Potions\.(\w+)\);$')
# `builder.addStartMix(Items.SUGAR, Potions.SWIFTNESS);` — two mixes, see below.
_START_MIX = re.compile(r'^\s*builder\.addStartMix\(Items\.(\w+), Potions\.(\w+)\);$')

# `public static final ResourceKey<Potion> LONG_NIGHT_VISION = PotionIds
# .register("long_night_vision");` — the field name PotionBrewing.java writes and
# the registry key are not always the same string.
_JAVA_ID = re.compile(
    r'^\s*public static final ResourceKey<Potion> (\w+) = PotionIds\.register\("([a-z_]+)"\);')


def snake(java_constant: str) -> str:
    """`Items.GLISTERING_MELON_SLICE` -> "glistering_melon_slice": every item and
    potion registry key is the lowercased Java constant."""
    return java_constant.lower()


def parse_potion_keys(text: str) -> dict:
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


def parse_mixes(text: str, keys: dict) -> list:
    """PotionBrewing.java's addVanillaMixes -> the four row kinds, in order."""
    rows = []
    for line in text.splitlines():
        # Only the body of addVanillaMixes registers anything; its `return
        # builder.build();` is the one other line that mentions the builder.
        if not line.strip().startswith("builder."):
            continue
        match = _CONTAINER.match(line)
        if match:
            rows.append((_KIND_CONTAINER, _NS + snake(match.group(1)), "-", "-"))
            continue
        match = _CONTAINER_MIX.match(line)
        if match:
            rows.append((_KIND_CONTAINER_MIX, _NS + snake(match.group(1)),
                         _NS + snake(match.group(2)), _NS + snake(match.group(3))))
            continue
        match = _POTION_MIX.match(line)
        if match:
            rows.append((_KIND_POTION_MIX, _potion(keys, match.group(1)),
                         _NS + snake(match.group(2)), _potion(keys, match.group(3))))
            continue
        match = _START_MIX.match(line)
        if match:
            # `Builder.addStartMix`: water plus the ingredient is always mundane,
            # and awkward plus it is the named potion. Two mixes, in this order.
            ingredient = _NS + snake(match.group(1))
            rows.append((_KIND_POTION_MIX, _potion(keys, "WATER"), ingredient,
                         _potion(keys, "MUNDANE")))
            rows.append((_KIND_POTION_MIX, _potion(keys, "AWKWARD"), ingredient,
                         _potion(keys, match.group(2))))
            continue
        raise SystemExit(f"unparsed PotionBrewing line: {line.strip()}")
    if not rows:
        raise SystemExit("no addVanillaMixes registrations found")
    return rows


def _potion(keys: dict, java_field: str) -> str:
    if java_field not in keys:
        raise SystemExit(f"Potions.{java_field} is not declared in PotionIds.java")
    return _NS + keys[java_field]


def parse_fuel_tag(path: pathlib.Path) -> list:
    """The `minecraft:brewing_fuel` item tag -> its item names.

    BrewingStandBlockEntity tests `stack.is(ItemTags.BREWING_FUEL)`, so the tag
    is the fuel list. Vanilla's contains one item; a tag that referenced another
    tag (or held no items at all) would mean this table is stale, not that the
    entry should be dropped.
    """
    values = json.loads(path.read_text()).get("values", [])
    items = []
    for value in values:
        if isinstance(value, dict):
            value = value.get("id", "")
        if not isinstance(value, str) or not value.startswith(_NS):
            raise SystemExit(f"{path}: tag value {value!r} is not a plain item")
        items.append(value)
    if not items:
        raise SystemExit(f"{path}: the brewing fuel tag is empty")
    return items


def load_item_ids(header_path: pathlib.Path) -> dict:
    """{name: item id} from the generated item table's kItems[] array."""
    text = header_path.read_text()
    body = text.split("kItems[]", 1)[1].split("};", 1)[0]
    pairs = re.findall(r'\{"([^"]+)", (\d+),', body)
    if not pairs:
        raise SystemExit(f"{header_path}: no kItems[] entries found")
    return {name: int(i) for name, i in pairs}


def load_potion_ids_tsv(path: pathlib.Path) -> dict:
    """{name: potion id} from the committed potions extraction."""
    ids = {}
    for line in path.read_text().splitlines():
        if not line or line.startswith("#") or line == "name\tid\teffect\tduration\tamplifier":
            continue
        name, ident = line.split("\t")[:2]
        ids[name] = int(ident)
    if not ids:
        raise SystemExit(f"{path}: no potions found")
    return ids


def rows_from_reports(reports: pathlib.Path, alchemy: pathlib.Path,
                      item_header: pathlib.Path, fuel_tag: pathlib.Path) -> list:
    registries = json.loads((reports / "registries.json").read_text())
    potion_ids = {name: meta["protocol_id"]
                  for name, meta in registries[_POTION_REGISTRY]["entries"].items()}
    item_ids = load_item_ids(item_header)

    keys = parse_potion_keys((alchemy / "PotionIds.java").read_text())
    rows = parse_mixes((alchemy / "PotionBrewing.java").read_text(), keys)
    # A name the matching registry does not know is a version mismatch between
    # the jar and the reports, not something to guess at. `a` and `c` are
    # potions for a potion mix and items otherwise; `b` (the ingredient) is
    # always an item and '-' for the two rows that have none.
    for kind, a, b, c in rows:
        if kind == _KIND_POTION_MIX:
            names = [(a, potion_ids), (c, potion_ids), (b, item_ids)]
        elif kind == _KIND_CONTAINER_MIX:
            names = [(a, item_ids), (b, item_ids), (c, item_ids)]
        else:
            names = [(a, item_ids)]
        for name, table in names:
            if name not in table:
                raise SystemExit(f"{name} is not in the table of its kind")

    for item in parse_fuel_tag(fuel_tag):
        if item not in item_ids:
            raise SystemExit(f"fuel {item} is not in the item table")
        rows.append((_KIND_FUEL, item, "-", "-"))
    return rows


def read_tsv(path: pathlib.Path) -> list:
    rows = []
    for line in path.read_text().splitlines():
        if not line or line.startswith("#") or line == _TSV_HEADER:
            continue
        parts = line.split("\t")
        if len(parts) != 4:
            raise SystemExit(f"{path}: expected four columns, got {line!r}")
        if parts[0] not in (_KIND_CONTAINER, _KIND_CONTAINER_MIX, _KIND_POTION_MIX,
                            _KIND_FUEL):
            raise SystemExit(f"{path}: unknown kind {parts[0]!r}")
        rows.append(tuple(parts))
    if not rows:
        raise SystemExit(f"{path}: no rows")
    return rows


def render_tsv(rows: list) -> str:
    lines = [
        "# Generated by tools/gen_brewing_table.py --reports ... --java-dir ...",
        "#",
        "# Brewing is code rather than data in 26.2: PotionBrewing.addVanillaMixes",
        "# builds the container list, the container mixes and the potion mixes from",
        "# item and Potions fields, and BrewingStandBlockEntity pulls its fuel from",
        "# the minecraft:brewing_fuel item tag. Each row is one of:",
        "#",
        "#   container      a -> the item that may sit in a bottle slot (b and c '-')",
        "#   container_mix  a -> b becomes c  (a and c are items)",
        "#   potion_mix     a -> b becomes c  (a and c are potions)",
        "#   fuel           a -> the items the fuel slot accepts (b and c '-')",
        "#",
        "# Order is the order the mixes were registered in, which is the order a",
        "# lookup scans them in.",
        _TSV_HEADER,
    ]
    for kind, a, b, c in rows:
        lines.append("\t".join([kind, a, b, c]))
    return "\n".join(lines) + "\n"


def render(rows: list, item_ids: dict, potion_ids: dict) -> str:
    def item(name):
        if name not in item_ids:
            raise SystemExit(f"item {name} is not in the item table")
        return item_ids[name]

    def potion(name):
        if name not in potion_ids:
            raise SystemExit(f"potion {name} is not in the potion table")
        return potion_ids[name]

    containers = [a for kind, a, _b, _c in rows if kind == _KIND_CONTAINER]
    container_mixes = [(a, b, c) for kind, a, b, c in rows if kind == _KIND_CONTAINER_MIX]
    potion_mixes = [(a, b, c) for kind, a, b, c in rows if kind == _KIND_POTION_MIX]
    fuels = [a for kind, a, _b, _c in rows if kind == _KIND_FUEL]
    if not containers or not potion_mixes or not fuels:
        raise SystemExit("the brewing table is missing a required row kind")

    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "//",
        f"// The 26.2 brewing table ({len(containers)} containers, {len(container_mixes)} item mixes,",
        f"// {len(potion_mixes)} potion mixes, {len(fuels)} fuel — generated, do not hand-edit).",
        "//",
        "// Brewing is code rather than data in this version, so the mixes come from",
        "// the decompiled net/minecraft/world/item/alchemy/PotionBrewing.java and the",
        "// fuel from the `minecraft:brewing_fuel` item tag that",
        "// BrewingStandBlockEntity tests its fuel slot against. Ids are registry ids",
        "// (world/item_registry.h and world/potion_table.h), because they are what a",
        "// stack and a potion_contents component carry at runtime.",
        "//",
        "// Order is registration order and therefore lookup order. `Builder.addStartMix`",
        "// contributes two entries — water plus the ingredient is mundane, awkward plus",
        "// it is the named potion — which is why water appears more than once as a",
        "// `from`. `mix` and `hasMix` take the *first* entry whose `from` and",
        "// `ingredient` both match, so the order is preserved verbatim.",
        "//",
        "// Regenerate with tools/gen_brewing_table.py.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace mc::world {",
        "",
        "// PotionBrewing.BREWING_TIME_SECONDS = 20, in ticks: the data slot the client",
        "// counts down from when a brew starts.",
        "inline constexpr int kBrewingTimeTicks = 400;",
        "// BrewingStandBlockEntity.FUEL_USES: the `Fuel` one unit of the fuel tag is",
        "// worth. One use is spent to start a brew, not per tick.",
        "inline constexpr int kBrewingFuelUses = 20;",
        "",
        "// PotionBrewing.hasMix: the item a mix may start from (the three potion",
        "// items, `Builder.addContainer`).",
        "inline constexpr int32_t kBrewingContainers[] = {",
    ]
    for name in containers:
        lines.append(f"    {item(name)},  // {name}")
    lines += [
        "};",
        f"inline constexpr int kBrewingContainerCount = {len(containers)};",
        "",
        "// The `minecraft:brewing_fuel` tag, expanded (FuelSlot.mayPlaceItem).",
        "inline constexpr int32_t kBrewingFuels[] = {",
    ]
    for name in fuels:
        lines.append(f"    {item(name)},  // {name}")
    lines += [
        "};",
        f"inline constexpr int kBrewingFuelCount = {len(fuels)};",
        "",
        "// PotionBrewing.hasContainerMix / Mix<Item>: changing the container rather",
        "// than the potion — `source` is the item, the potion it holds is untouched.",
        "struct GeneratedContainerMix {",
        "  int32_t from;",
        "  int32_t ingredient;",
        "  int32_t to;",
        "};",
        "",
        "inline constexpr GeneratedContainerMix kBrewingContainerMixes[] = {",
    ]
    for a, b, c in container_mixes:
        lines.append(f"    {{{item(a)}, {item(b)}, {item(c)}}},  // {_short(a)} + {_short(b)}"
                     f" -> {_short(c)}")
    lines += [
        "};",
        f"inline constexpr int kBrewingContainerMixCount = {len(container_mixes)};",
        "",
        "// PotionBrewing.hasPotionMix / Mix<Potion>: changing the potion, in the",
        "// container the stack already is.",
        "struct GeneratedPotionMix {",
        "  int32_t from;",
        "  int32_t ingredient;",
        "  int32_t to;",
        "};",
        "",
        "inline constexpr GeneratedPotionMix kBrewingPotionMixes[] = {",
    ]
    for a, b, c in potion_mixes:
        lines.append(f"    {{{potion(a)}, {item(b)}, {potion(c)}}},  // {_short(a)} + {_short(b)}"
                     f" -> {_short(c)}")
    lines += [
        "};",
        f"inline constexpr int kBrewingPotionMixCount = {len(potion_mixes)};",
        "",
        "} // namespace mc::world",
        "",
    ]
    return "\n".join(lines)


def _short(name: str) -> str:
    return name[len(_NS):] if name.startswith(_NS) else name


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--reports", type=pathlib.Path,
                        help="vanilla data generator reports directory")
    parser.add_argument("--java-dir", type=pathlib.Path,
                        help="decompiled net/minecraft/world/item/alchemy directory")
    parser.add_argument("--item-header", type=pathlib.Path,
                        default=pathlib.Path("src/world/item_registry.h"))
    parser.add_argument("--fuel-tag", type=pathlib.Path,
                        default=pathlib.Path(
                            "data/vanilla/26.2/data/minecraft/tags/item/brewing_fuel.json"))
    parser.add_argument("--tsv", type=pathlib.Path,
                        default=pathlib.Path("data/vanilla/26.2/brewing.tsv"),
                        help="the flattened extraction (written in --reports mode)")
    parser.add_argument("--potions", type=pathlib.Path,
                        default=pathlib.Path("data/vanilla/26.2/potions.tsv"),
                        help="the potion extraction the ids come from in --tsv mode")
    parser.add_argument("--header", type=pathlib.Path,
                        default=pathlib.Path("src/world/brewing_table.h"))
    args = parser.parse_args()

    if args.reports:
        if not args.java_dir:
            raise SystemExit("--reports needs --java-dir (the mixes are code-only)")
        rows = rows_from_reports(args.reports, args.java_dir, args.item_header,
                                 args.fuel_tag)
        args.tsv.parent.mkdir(parents=True, exist_ok=True)
        args.tsv.write_text(render_tsv(rows))
        potion_ids = {name: meta["protocol_id"] for name, meta in
                      json.loads((args.reports / "registries.json")
                                 .read_text())[_POTION_REGISTRY]["entries"].items()}
        print(f"wrote {args.tsv} ({len(rows)} rows)")
    else:
        rows = read_tsv(args.tsv)
        potion_ids = load_potion_ids_tsv(args.potions)

    args.header.parent.mkdir(parents=True, exist_ok=True)
    args.header.write_text(render(rows, load_item_ids(args.item_header), potion_ids))
    print(f"wrote {args.header} ({len(rows)} rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
