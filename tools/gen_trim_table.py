#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates src/world/trim_table.h — the 26.2 armor trim registries.

Two outputs, one per input mode (the same shape as gen_item_table.py):

  --reports DIR   the authoritative mode: reads the vanilla data generator's
                  reports directory to learn which item *provides* each trim
                  material (`minecraft:provides_trim_material`, a component no
                  data file carries — Items.java sets it with
                  `Item.Properties.trimMaterial`, so the report is the only
                  place it exists), and writes BOTH the flattened extraction
                  (data/vanilla/26.2/trims.tsv) and the header.
  --tsv FILE      rebuilds only the header from the committed extraction.

Sources:

  clientconfig/registries/trim_material.txt   the wire order of the 11
  clientconfig/registries/trim_pattern.txt    minecraft:trim_material and 18
                                              minecraft:trim_pattern entries.
                                              Both are *datapack* registries, so
                                              unlike the item registry there is no
                                              built-in registration order; the id
                                              in the `minecraft:trim` item
                                              component is this index (the wire
                                              carries id + 1: `ArmorTrim` stores
                                              Holders, and `ByteBufCodecs.holder`
                                              writes id + 1 with 0 reserved for a
                                              direct holder).
  data/minecraft/trim_material/*.json         cross-checks the captured order
  data/minecraft/trim_pattern/*.json          against the data tree.
  data/minecraft/recipe/*_smithing_trim.json  each pattern's template item. The
                                              recipe is the only place that link
                                              exists in 26.2 — `TrimPattern` is
                                              `(asset_id, description, decal)` and
                                              the template is the recipe's
                                              ingredient — so it is resolved here
                                              and pinned 1:1: a pattern must have
                                              exactly one trim recipe, that
                                              recipe's `template` must be a plain
                                              item (not a tag or a list), and no
                                              two patterns may share a template.
  data/minecraft/tags/item/trim_materials.json the trim materials tag, which the
                                              recipes' `addition` cell names. Every
                                              item in it must carry the component
                                              and vice versa: the engine derives
                                              the material from the *addition
                                              stack*, and refuses loudly here if
                                              the two sets ever disagree.
  reports/registries.json                     the `minecraft:data_component_type`
                                              id of `minecraft:trim` itself, for
                                              the header's comment.
  data/vanilla/26.2/item_components.tsv       item ids (the generated column is
                                              an id, the registry index a client
                                              reads).

The generator refuses to run on any disagreement between these sources rather
than emitting a table that would silently blend the wrong armor.

Usage:
    tools/gen_trim_table.py --reports build/_dl/boot/generated/reports
    tools/gen_trim_table.py --tsv data/vanilla/26.2/trims.tsv
"""

import argparse
import json
import pathlib
import sys

_NS = "minecraft:"


def _fail(message):
    print(f"error: {message}", file=sys.stderr)
    return 1


def read_order(path):
    """One `minecraft:<name>` per line, in wire order. Returns [(name, index)]."""
    names = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if not line.startswith(_NS):
            raise ValueError(f"{path}: entry {line!r} is not in the minecraft namespace")
        if line in names:
            raise ValueError(f"{path}: {line} appears twice")
        names.append(line)
    if not names:
        raise ValueError(f"{path}: no entries")
    return [(name, i) for i, name in enumerate(names)]


def load_item_ids(tsv_path):
    """id -> name and name -> id from the committed item extraction."""
    ids = {}
    by_name = {}
    for line in tsv_path.read_text().splitlines():
        # `#` lines are scalar comments (a component id); the first row is the
        # TSV's own `id\tname\t...` header.
        cols = line.split("\t")
        if line.startswith("#") or len(cols) < 2 or not cols[0].isdigit():
            continue
        ids[int(cols[0])] = cols[1]
        by_name[cols[1]] = int(cols[0])
    return ids, by_name


def item_component_items(reports_dir):
    """The item that provides each trim material, from the components report.

    `RegistryComponentsReport` writes one JSON per item with its full
    DataComponentPatch, and a Holder-shaped component is the referenced id.
    """
    provides = {}
    where = reports_dir / "minecraft" / "components" / "item"
    if not where.is_dir():
        raise ValueError(f"{where}: no item component reports")
    for path in sorted(where.glob("*.json")):
        patch = json.loads(path.read_text())["components"]
        material = patch.get(_NS + "provides_trim_material")
        if material is None:
            continue
        name = _NS + path.stem  # the report's file name is the item's path
        if material in provides:
            raise ValueError(
                f"{material}: provided by both {provides[material]} and {name}")
        provides[material] = name
    if not provides:
        raise ValueError(f"{where}: no item provides a trim material")
    return provides


def trim_component_id(reports_dir):
    registry = json.loads((reports_dir / "registries.json").read_text())
    entry = registry[_NS + "data_component_type"]["entries"].get(_NS + "trim")
    if entry is None:
        raise ValueError("registries.json does not know minecraft:trim")
    return entry["protocol_id"]


def load_tags(tag_dir):
    """name -> set of item ids-ish names, nested `#tag` resolved."""
    loaded = {}

    def resolve(name, seen):
        if name in loaded:
            return loaded[name]
        if name in seen:
            raise ValueError(f"{name}: cyclic tag")
        seen.add(name)
        path = tag_dir / (name.split(":", 1)[1] + ".json")
        values = set()
        for entry in json.loads(path.read_text())["values"]:
            if entry.startswith("#"):
                values |= resolve(entry[1:], seen)
            else:
                values.add(entry)
        seen.discard(name)
        loaded[name] = values
        return values

    return lambda name: resolve(name, set())


def expand_ingredient(spec, tags):
    """An Ingredient is `HolderSet<Item>`: an item, a `#tag`, or a list of either."""
    entries = spec if isinstance(spec, list) else [spec]
    out = set()
    plain = []
    for entry in entries:
        if isinstance(entry, str) and entry.startswith("#"):
            out |= tags(entry[1:])
        else:
            out.add(entry)
            plain.append(entry)
    return out, plain


def load_trim_recipes(recipe_dir, tags, pattern_names):
    """pattern -> template item, from the 18 `smithing_trim` recipes.

    Also returns the union of every recipe's `addition` cell: the items the
    matcher can ever see there.
    """
    templates = {}
    additions = set()
    for path in sorted(recipe_dir.glob("*.json")):
        recipe = json.loads(path.read_text())
        if recipe.get("type") != _NS + "smithing_trim":
            continue
        pattern = recipe.get("pattern")
        if not isinstance(pattern, str) or pattern not in pattern_names:
            raise ValueError(f"{path.name}: unknown pattern {pattern!r}")
        if pattern in templates:
            raise ValueError(f"{path.name}: {pattern} already has a trim recipe")
        template_items, plain = expand_ingredient(recipe.get("template"), tags)
        if len(template_items) != 1:
            raise ValueError(
                f"{path.name}: the template must name exactly one item, got {sorted(template_items)}")
        templates[pattern] = plain[0]
        args, _ = expand_ingredient(recipe.get("addition"), tags)
        additions |= args
    return templates, additions


def build_tables(reports_dir, items_tsv, order_dir, data_dir):
    """Everything the header needs, with every source cross-checked."""
    materials = read_order(order_dir / "trim_material.txt")
    patterns = read_order(order_dir / "trim_pattern.txt")
    ids, by_name = load_item_ids(items_tsv)
    provides = item_component_items(reports_dir)

    material_names = [name for name, _ in materials]
    if set(provides) != set(material_names):
        raise ValueError(
            "the component's materials and the captured wire order disagree: "
            f"only in the reports {sorted(set(provides) - set(material_names))}, "
            f"only in the order {sorted(set(material_names) - set(provides))}")

    data_material = {_NS + p.stem for p in (data_dir / "minecraft" / "trim_material").glob("*.json")}
    data_pattern = {_NS + p.stem for p in (data_dir / "minecraft" / "trim_pattern").glob("*.json")}
    if data_material != set(material_names):
        raise ValueError(
            f"trim_material/*.json and the wire order disagree: {data_material ^ set(material_names)}")
    if data_pattern != set(name for name, _ in patterns):
        raise ValueError(
            f"trim_pattern/*.json and the wire order disagree: "
            f"{data_pattern ^ set(name for name, _ in patterns)}")

    tags = load_tags(data_dir / "minecraft" / "tags" / "item")
    tag_items = tags("minecraft:trim_materials")
    if tag_items != set(provides.values()):
        raise ValueError(
            f"#minecraft:trim_materials and the `provides_trim_material` items disagree: "
            f"{tag_items ^ set(provides.values())}")

    pattern_names = {name for name, _ in patterns}
    templates, additions = load_trim_recipes(
        data_dir / "minecraft" / "recipe", tags, pattern_names)
    if set(templates) != pattern_names:
        raise ValueError(
            f"patterns without a trim recipe: {sorted(pattern_names - set(templates))}")
    if additions != set(provides.values()):
        raise ValueError(
            "the trim recipes' `addition` cell and the material items disagree: "
            f"{additions ^ set(provides.values())}")
    template_items = list(templates.values())
    if len(set(template_items)) != len(template_items):
        raise ValueError("two trim patterns share a template item")

    def item_id(name, where):
        if name not in by_name:
            raise ValueError(f"{where}: {name} is not in the item table")
        return by_name[name]

    out_materials = []
    for name, index in materials:
        item = provides[name]
        out_materials.append((name, index, item_id(item, name), item))
    out_patterns = []
    for name, index in patterns:
        item = templates[name]
        out_patterns.append((name, index, item_id(item, name), item))

    scalars = {"trim_component_id": trim_component_id(reports_dir)}
    return out_materials, out_patterns, scalars


def write_tsv(path, materials, patterns, scalars):
    lines = [
        "# Generated by tools/gen_trim_table.py --reports ...",
        "#",
        "# The armor trim registries, in the order the client's trim_material and",
        "# trim_pattern registries are indexed (the `minecraft:trim` item",
        "# component carries `id + 1` for each). Each material row names the item",
        "# whose minecraft:provides_trim_material component points at it, and each",
        "# pattern row names the smithing template its recipe is built around.",
        f"#trim_component_id\t{scalars['trim_component_id']}",
        "#material_id\tmaterial_name\titem_id\titem_name",
    ]
    for name, index, item, item_name in materials:
        lines.append(f"{index}\t{name}\t{item}\t{item_name}")
    lines.append("#pattern_id\tpattern_name\ttemplate_item_id\ttemplate_item_name")
    for name, index, item, item_name in patterns:
        lines.append(f"{index}\t{name}\t{item}\t{item_name}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n")


def read_tsv(path):
    """Reads back write_tsv for --tsv mode (the header rebuild needs no reports)."""
    scalars = {}
    materials, patterns = [], []
    section = None
    for line in path.read_text().splitlines():
        if line.startswith("#material_id"):
            section = "material"
            continue
        if line.startswith("#pattern_id"):
            section = "pattern"
            continue
        if line.startswith("#"):
            cols = line[1:].split("\t")
            if len(cols) == 2:
                scalars[cols[0]] = cols[1]
            continue
        cols = line.split("\t")
        if section == "material":
            materials.append((cols[1], int(cols[0]), int(cols[2]), cols[3]))
        elif section == "pattern":
            patterns.append((cols[1], int(cols[0]), int(cols[2]), cols[3]))
        else:
            raise ValueError(f"{path}: data row before any section header")
    return materials, patterns, scalars


def render_header(materials, patterns, scalars, header_name):
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "//",
        f"// The 26.2 armor trim registries (generated — do not hand-edit): the",
        f"// {len(materials)} minecraft:trim_material entries, the {len(patterns)} "
        "minecraft:trim_pattern",
        "// entries, and the two links the smithing table needs to apply a trim.",
        "//",
        "// Both registries are *datapack* registries, so their order comes from the",
        "// client's own registry order (clientconfig/registries/trim_material.txt and",
        "// trim_pattern.txt), cross-checked against the data tree and the trim",
        "// recipes. The `minecraft:trim` item component (data component type id",
        f"// {scalars['trim_component_id']}) stores a Holder<TrimMaterial> and a",
        "// Holder<TrimPattern>, and `ByteBufCodecs.holder` writes each as `id + 1`",
        "// (0 is a direct holder, which this engine never writes).",
        "//",
        "// The pattern -> template link is a *recipe* in 26.2: `TrimPattern` is",
        "// `(asset_id, description, decal)` and carries no template item, so the",
        "// mapping here is resolved from the 18 `smithing_trim` recipes and pinned",
        "// 1:1 by the generator — each pattern has exactly one recipe, its `template`",
        "// names exactly one item, and no two patterns share a template. That is what",
        "// lets the matcher resolve a matched trim recipe's pattern from the template",
        "// stack in the player's slot.",
        "//",
        "// The material -> item link is the item's `minecraft:provides_trim_material`",
        "// component (set in Items.java, so it exists in no data file); the generator",
        "// verifies that exactly these items are `#minecraft:trim_materials`, which is",
        "// the `addition` cell of every trim recipe, so an item the matcher accepts",
        "// there always resolves to the material vanilla would use.",
        "//",
        "// Regenerate with tools/gen_trim_table.py --reports <reports dir>.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace mc::world {",
        "",
        "struct GeneratedTrimMaterial {",
        '  const char* name; // "minecraft:quartz"',
        "  int32_t id;       // the trim_material registry id; the wire carries id + 1",
        "  int32_t item;     // the item whose provides_trim_material component names it",
        "};",
        "",
        "struct GeneratedTrimPattern {",
        '  const char* name;      // "minecraft:bolt"',
        "  int32_t id;            // the trim_pattern registry id; the wire carries id + 1",
        "  int32_t template_item; // the pattern's smithing template item",
        "};",
        "",
        "// clang-format off",
        "inline constexpr GeneratedTrimMaterial kTrimMaterials[] = {",
    ]
    for name, index, item, _item_name in materials:
        lines.append(f'    {{"{name}", {index}, {item}}},')
    lines += [
        "};",
        "inline constexpr GeneratedTrimPattern kTrimPatterns[] = {",
    ]
    for name, index, item, _item_name in patterns:
        lines.append(f'    {{"{name}", {index}, {item}}},')
    lines += [
        "};",
        "// clang-format on",
        f"inline constexpr uint32_t kTrimMaterialCount = {len(materials)};",
        f"inline constexpr uint32_t kTrimPatternCount = {len(patterns)};",
        "",
        "} // namespace mc::world",
        "",
    ]
    text = "\n".join(lines)
    path = pathlib.Path(header_name)
    path.write_text(text)
    return len(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--reports", type=pathlib.Path,
                        help="vanilla data generator reports directory")
    parser.add_argument("--tsv", type=pathlib.Path,
                        default=pathlib.Path("data/vanilla/26.2/trims.tsv"),
                        help="flattened extraction (written in --reports mode)")
    parser.add_argument("--data", type=pathlib.Path,
                        default=pathlib.Path("data/vanilla/26.2/data"),
                        help="the vanilla data tree (recipes, tags, trim registries)")
    parser.add_argument("--order-dir", type=pathlib.Path,
                        default=pathlib.Path("data/vanilla/26.2/clientconfig/registries"),
                        help="captured registry wire orders")
    parser.add_argument("--items", type=pathlib.Path,
                        default=pathlib.Path("data/vanilla/26.2/item_components.tsv"),
                        help="the generated item extraction (id -> name)")
    parser.add_argument("--header", type=pathlib.Path,
                        default=pathlib.Path("src/world/trim_table.h"),
                        help="generated header")
    args = parser.parse_args()

    try:
        if args.reports:
            materials, patterns, scalars = build_tables(
                args.reports, args.items, args.order_dir, args.data)
            write_tsv(args.tsv, materials, patterns, scalars)
            print(f"wrote {args.tsv}: {len(materials)} materials, {len(patterns)} patterns")
        else:
            materials, patterns, scalars = read_tsv(args.tsv)
    except (ValueError, OSError) as error:
        return _fail(str(error))

    size = render_header(materials, patterns, scalars, args.header)
    print(f"wrote {args.header}: {len(materials)} materials, {len(patterns)} patterns, "
          f"{size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
