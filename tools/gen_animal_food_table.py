#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates src/world/animal_food_table.h from the vanilla 26.2 item tag data
pack: what `Animal.isFood(ItemStack)` answers for every `Animal` in the mob
table (world/mob_table.h's `kMobAnimal` rows).

The table used to carry four animals (Phase 3 milestone I). The per-class rule
is one line of Java each — `AbstractCow.isFood` is `stack.is(ItemTags.COW_FOOD)`,
`Wolf`'s is `ItemTags.WOLF_FOOD` — and the tags themselves are data. This tool
finds each animal's `isFood` override in the decompiled entity classes, reads
the `ItemTags.*` it tests, and flattens every referenced tag (nested `#tag`
references resolved) into one (mob, item) row per membership.

The mob list comes from `src/world/mob_table.h` itself (the generated table's
`kMobAnimal` rows), so the two tables cannot drift: a mob that loses the flag
loses its food rows, and a name that appears in only one of them fails the
generation.

A tag is a list of item names and of *other tags* (`"#minecraft:..."`), so the
resolution is the recursive one `TagLoader` performs. A mob whose `isFood` has
no `ItemTags` reference at all simply gets no rows — `Animal.isFood`'s default
is false, and an animal that can never be fed can never fall in love.

Regenerate with: python3 tools/gen_animal_food_table.py
"""

import argparse
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def read(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def resolve_tag(name: str, tag_dir: pathlib.Path, cache: dict) -> set:
    """The flattened item names of `minecraft:<name>`, tags expanded."""
    if name in cache:
        return cache[name]
    path = tag_dir / (name + ".json")
    if not path.exists():
        raise SystemExit(f"missing item tag file: {path}")
    values = json.loads(read(path)).get("values", [])
    out = set()
    for value in values:
        if isinstance(value, dict):
            value = value.get("id", "")
        if value.startswith("#"):
            out |= resolve_tag(value[1:].split(":")[-1], tag_dir, cache)
        else:
            out.add(value if ":" in value else "minecraft:" + value)
    cache[name] = out
    return out


def class_index(decomp: pathlib.Path) -> dict:
    """class name -> {base, path} for the whole decompiled tree."""
    out = {}
    pattern = re.compile(r"(?:^|\n)\s*(?:public |private |protected |static |abstract |final )*"
                         r"(class|interface)\s+(\w+)(?:<[^>]*>)?\s*"
                         r"(?:extends\s+([\w.]+))?\s*(?:implements\s+([^{]+))?")
    for path in sorted(decomp.rglob("*.java")):
        for match in pattern.finditer(read(path)):
            name, base = match.group(2), match.group(3)
            base = base.rsplit(".", 1)[-1] if base else None
            if name in out and out[name]["base"] is not None and base is None:
                continue
            out[name] = {"base": base, "path": path}
    return out


def animal_classes(mob_header: pathlib.Path) -> list:
    """(mob name, class name) for every `kMobAnimal` row, in table order."""
    java = read(ROOT / "build/_decomp/net/minecraft/world/entity/EntityTypes.java")
    builder = re.compile(
        r"EntityTypes\.register\(EntityTypeIds\.([A-Z0-9_]+),\s*EntityType\.Builder\.of\("
        r"([A-Za-z0-9_$.]+)::new,\s*MobCategory\.([A-Z_]+)\)")
    class_for = {("minecraft:" + m.group(1).lower()): m.group(2).rsplit(".", 1)[-1]
                 for m in builder.finditer(java)}
    out = []
    for line in read(mob_header).splitlines():
        if "kMobAnimal" not in line or not line.lstrip().startswith("{"):
            continue  # only the kMobs rows, not the flag's own definition
        match = re.search(r'\{"(minecraft:[a-z_]+)"', line)
        if not match:
            continue
        name = match.group(1)
        cls = class_for.get(name)
        if cls is None:
            raise SystemExit(f"no EntityTypes factory for animal {name}")
        out.append((name, cls))
    if not out:
        raise SystemExit("no kMobAnimal rows found in the mob table")
    return out


def is_food_tags(cls: str, decls: dict) -> list:
    """The `ItemTags.*` names the nearest `isFood` override tests, in order."""
    seen, chain = set(), []
    while cls and cls in decls and cls not in seen:
        seen.add(cls)
        chain.append(cls)
        cls = decls[cls]["base"]
    for name in chain:
        text = read(decls[name]["path"])
        match = re.search(r"public boolean isFood\([^)]*\)\s*\{(.*?)\n    \}", text, re.S)
        if not match:
            continue
        return re.findall(r"ItemTags\.([A-Z0-9_]+)", match.group(1))
    return []


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--decomp", type=pathlib.Path, default=ROOT / "build/_decomp")
    parser.add_argument("--tags", type=pathlib.Path,
                        default=ROOT / "data/vanilla/26.2/data/minecraft/tags/item")
    parser.add_argument("--mob-header", type=pathlib.Path, default=ROOT / "src/world/mob_table.h")
    parser.add_argument("--item-header", type=pathlib.Path, default=ROOT / "src/world/item_registry.h")
    parser.add_argument("--out", type=pathlib.Path, default=ROOT / "src/world/animal_food_table.h")
    args = parser.parse_args()

    decls = class_index(args.decomp)
    animals = animal_classes(args.mob_header)

    cache = {}
    rows = []       # (mob, item)
    sources = []    # (mob, [tag names])
    for mob, cls in animals:
        tags = is_food_tags(cls, decls)
        names = set()
        for tag in tags:
            names |= resolve_tag(tag.lower(), args.tags, cache)
        if not names:
            print(f"note: {mob} ({cls}) has no ItemTags food reference", file=sys.stderr)
            continue
        sources.append((mob, tags))
        for name in names:
            rows.append((mob, name))

    items = set(re.findall(r'\{"(minecraft:[a-z0-9_]+)"', read(args.item_header)))
    mobs = set(re.findall(r'\{"(minecraft:[a-z_]+)", "', read(args.mob_header)))
    unknown_items = sorted({item for _, item in rows} - items)
    unknown_mobs = sorted({mob for mob, _ in rows} - mobs)
    if unknown_items:
        raise SystemExit(f"ITEM NOT IN THE PROJECT REGISTRY: {unknown_items}")
    if unknown_mobs:
        raise SystemExit(f"MOB NOT IN THE PROJECT REGISTRY: {unknown_mobs}")

    rows.sort()
    lines = []
    lines.append("// SPDX-License-Identifier: GPL-3.0-or-later")
    lines.append("// clang-format off")
    lines.append("//")
    lines.append(f"// What `Animal.isFood(ItemStack)` answers for the animals this engine")
    lines.append(f"// simulates ({len(rows)} rows, generated — do not hand-edit), flattened out of")
    lines.append("// the vanilla 26.2 item tag data pack by tools/gen_animal_food_table.py. The")
    lines.append("// mob list is world/mob_table.h's own `kMobAnimal` rows, so the two tables")
    lines.append("// cannot drift; the per-class Java is one line each:")
    lines.append("//")
    for mob, tags in sources:
        tag_text = ", ".join("#minecraft:" + t.lower() for t in tags)
        lines.append(f"//   * {mob:32s} -> {tag_text}")
    lines.append("//")
    lines.append("// Sorted by (mob, item), so `mob_eats` (world/mob.cpp) can walk it in one pass.")
    lines.append("//")
    lines.append("#pragma once")
    lines.append("")
    lines.append("namespace mc::world {")
    lines.append("")
    lines.append("struct GeneratedAnimalFood {")
    lines.append("  const char* mob;")
    lines.append("  const char* item;")
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr GeneratedAnimalFood kAnimalFoods[] = {")
    for mob, item in rows:
        lines.append(f'    {{"{mob}", "{item}"}},')
    lines.append("};")
    lines.append(f"inline constexpr int kAnimalFoodCount = {len(rows)};")
    lines.append("")
    lines.append("} // namespace mc::world")
    lines.append("")
    args.out.write_text("\n".join(lines) + "\n")
    print(f"wrote {args.out}: {len(rows)} (mob, item) row(s) for {len(sources)} animals "
          f"out of {len(animals)} kMobAnimal rows")
    return 0


if __name__ == "__main__":
    sys.exit(main())
