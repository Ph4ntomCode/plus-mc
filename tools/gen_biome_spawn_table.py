#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generates src/world/biome_spawn_table.h from the vanilla 26.2 worldgen biome
# data pack (`data/vanilla/26.2/data/minecraft/worldgen/biome/*.json`): every
# biome's `spawners` list and its `spawn_costs`, which are the two things
# `NaturalSpawner` reads out of a biome (Phase 4 milestone C).
#
#   * `spawners` is `MobCategory -> [SpawnerData]`, and `SpawnerData` is
#     `{type, weight, minCount, maxCount}`. All eight categories are emitted, in
#     `MobCategory.values()` order, including the ones this engine has no mob for
#     (`NaturalSpawner.SPAWNING_CATEGORIES` is every category but `MISC`); the
#     *runtime* picks from the entries whose type the mob table knows, because a
#     spawn list is renormalized over what can actually be created.
#   * `spawn_costs` is `MobSpawnSettings.MobSpawnCost` (`charge`,
#     `energy_budget`), read by `PotentialCalculator`. Only two biomes in 26.2
#     set one (soul_sand_valley and warped_forest, both Nether), and both are
#     emitted here anyway: a biome with no cost is absent, which is exactly how
#     `getMobSpawnCost` answers null.
#
# Every entity name is cross-checked against the protocol's entity-type id list
# in the decompiled `EntityTypeIds.java` (so a typo or a rename is a data
# mismatch that fails the generation) and *reported* against the mob table
# (src/world/mob_table.h): the mobile set this engine can create is a handful of
# hand-written rows, so the header prints how many of the table's entries are
# reachable today. That number is the honest denominator for what natural
# spawning can produce, and it moves on its own as mobs are added to the table.
#
# Regenerate with:
#   python3 tools/gen_biome_spawn_table.py
#
# Usage: gen_biome_spawn_table.py [--biomes DIR] [--ids FILE] [--mobs FILE]
#                                 [--tsv FILE] [--header FILE]

import argparse
import json
import pathlib
import re
import sys

# `MobCategory.values()` order. The generated enum is this list, and
# `SPAWNING_CATEGORIES` (MISC excluded) is derived from it at runtime.
CATEGORIES = [
    "monster",
    "creature",
    "ambient",
    "axolotls",
    "underground_water_creature",
    "water_creature",
    "water_ambient",
    "misc",
]
# camelCase for the generated enum constants.
def enum_name(category: str) -> str:
    return "kSpawn" + "".join(part.capitalize() for part in category.split("_"))


def entity_registry(ids_java: pathlib.Path) -> set:
    """Every `minecraft:entity_type` name vanilla registers.

    `EntityTypeIds.java` is where the protocol names live (one
    `create("zombie")` per type, in registration order, which is also the id
    order the wire uses), so a name in the biome data pack that is not in this
    set is a typo or a rename — the same provenance check the other generators
    run against their registries.
    """
    text = ids_java.read_text(encoding="utf-8")
    names = set(re.findall(r'EntityTypeIds\.create\("([a-z0-9_]+)"\)', text))
    if len(names) < 100:
        raise SystemExit(f"only {len(names)} entity type ids found in {ids_java}")
    return {"minecraft:" + name for name in names}


def mob_table_names(path: pathlib.Path) -> set:
    return set(re.findall(r'\{"(minecraft:[a-z_]+)"', path.read_text(encoding="utf-8")))


def build(biomes_dir: pathlib.Path):
    rows = []   # (biome, category, entity, weight, min, max)
    costs = []  # (biome, entity, charge, energy_budget)
    for path in sorted(biomes_dir.glob("*.json")):
        biome = "minecraft:" + path.stem
        data = json.loads(path.read_text(encoding="utf-8"))
        spawners = data.get("spawners", {})
        for category in CATEGORIES:
            for entry in spawners.get(category, []):
                rows.append((biome, category, entry["type"], entry["weight"],
                             entry["minCount"], entry["maxCount"]))
        for entity, cost in sorted(data.get("spawn_costs", {}).items()):
            costs.append((biome, entity, cost["charge"], cost["energy_budget"]))
    return rows, costs


def write_tsv(path: pathlib.Path, rows, costs):
    lines = ["biome\tcategory\tentity\tweight\tmin_count\tmax_count"]
    lines += [f"{b}\t{c}\t{e}\t{w}\t{lo}\t{hi}" for b, c, e, w, lo, hi in rows]
    lines.append("")
    lines.append("# spawn_costs: PotentialCalculator's PointCharge (MobSpawnSettings.MobSpawnCost)")
    lines.append("biome\tentity\tcharge\tenergy_budget")
    lines += [f"{b}\t{e}\t{ch}\t{en}" for b, e, ch, en in costs]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n")


def write_header(path: pathlib.Path, rows, costs, known_mobs: set, reachable_rows: int,
                 reachable_biomes: int, biomes: int):
    out = []
    out.append("// SPDX-License-Identifier: GPL-3.0-or-later")
    out.append("// clang-format off")
    out.append("//")
    out.append(f"// Every biome's spawn list and spawn costs ({len(rows)} spawner entries and")
    out.append(f"// {len(costs)} spawn-cost entries over {biomes} biomes, generated — do not")
    out.append("// hand-edit), flattened out of the vanilla 26.2 worldgen biome data pack by")
    out.append("// tools/gen_biome_spawn_table.py. This is the data `NaturalSpawner` picks")
    out.append("// a spawn from (Phase 4 milestone C).")
    out.append("//")
    out.append("//   * `kBiomeSpawns` is sorted by (biome, category), and within a category it")
    out.append("//     keeps the JSON's own order — `WeightedList` does not care about order,")
    out.append("//     so keeping it is what makes a row-by-row diff against the data pack")
    out.append("//     possible. `kBiomeSpawnRanges` is the per-biome slice of it.")
    out.append("//   * All eight `MobCategory`s are carried, including the water ones this")
    out.append("//     engine has no mob for: the *runtime* skips entries whose type the mob")
    out.append("//     table cannot create (world/mob_table.h) and renormalizes the weights of")
    out.append("//     the rest, so a spawn list is weighted over what can actually spawn.")
    out.append(f"//     {reachable_rows} of {len(rows)} entries ({reachable_biomes} biomes) name a mob")
    out.append("//     this engine has today; the rest become reachable as the table grows.")
    out.append("//   * `kBiomeSpawnCosts` is `MobSpawnSettings.MobSpawnCost`, which only two")
    out.append("//     biomes set in 26.2 (soul_sand_valley, warped_forest). A biome with no")
    out.append("//     cost is absent, which is how `getMobSpawnCost` answers null.")
    out.append("//")
    out.append("// There is one entry per (biome, category) even when the list is empty, so")
    out.append("// the range table answers \"what may spawn in this biome\" in one lookup.")
    out.append("//")
    out.append("// Regenerate with tools/gen_biome_spawn_table.py.")
    out.append("#pragma once")
    out.append("")
    out.append("#include <cstdint>")
    out.append("")
    out.append("namespace mc::world {")
    out.append("")
    out.append("// `MobCategory`: the eight categories, in `MobCategory.values()` order.")
    out.append("enum MobSpawnCategory : uint8_t {")
    for category in CATEGORIES:
        out.append(f"  {enum_name(category)} = {CATEGORIES.index(category)},")
    out.append("  kSpawnCategoryCount,")
    out.append("};")
    out.append("")
    out.append("// The category's own name, as the data pack spells it.")
    out.append("inline constexpr const char* kMobSpawnCategoryNames[kSpawnCategoryCount] = {")
    for category in CATEGORIES:
        out.append(f'    "{category}",')
    out.append("};")
    out.append("")
    out.append("// One `MobSpawnSettings.SpawnerData`: what, how likely, and how many.")
    out.append("struct GeneratedBiomeSpawn {")
    out.append("  const char* biome;   // \"minecraft:plains\"")
    out.append("  const char* entity;  // \"minecraft:zombie\"")
    out.append("  uint8_t category;    // MobSpawnCategory")
    out.append("  int weight;          // WeightedList weight")
    out.append("  int min_count;       // SpawnerData.minCount()")
    out.append("  int max_count;       // SpawnerData.maxCount()")
    out.append("};")
    out.append("")
    out.append("// Sorted by (biome, category); binary-searched through kBiomeSpawnRanges.")
    out.append("inline constexpr GeneratedBiomeSpawn kBiomeSpawns[] = {")
    for biome, category, entity, weight, lo, hi in rows:
        out.append(f'    {{"{biome}", "{entity}", {enum_name(category)}, {weight}, {lo}, {hi}}},')
    out.append("};")
    out.append(f"inline constexpr int kBiomeSpawnCount = {len(rows)};")
    out.append("")
    out.append("// The slice of `kBiomeSpawns` belonging to one biome: every category, in")
    out.append("// order, so an empty category is a zero-length slice between two others.")
    out.append("struct GeneratedBiomeSpawnRange {")
    out.append("  const char* biome;")
    out.append("  int first;  // index into kBiomeSpawns")
    out.append("  int count;")
    out.append("};")
    out.append("")
    out.append("// Sorted by biome name, so a lookup is one binary search.")
    out.append("inline constexpr GeneratedBiomeSpawnRange kBiomeSpawnRanges[] = {")
    ranges = []
    for biome in sorted({row[0] for row in rows}):
        indices = [i for i, row in enumerate(rows) if row[0] == biome]
        ranges.append((biome, indices[0], len(indices)))
    for biome, first, count in ranges:
        out.append(f'    {{"{biome}", {first}, {count}}},')
    out.append("};")
    out.append(f"inline constexpr int kBiomeSpawnRangeCount = {len(ranges)};")
    out.append("")
    out.append("// `MobSpawnSettings.MobSpawnCost`: the charge a mob of this type carries")
    out.append("// and the energy budget its biome allows before spawning stops.")
    out.append("struct GeneratedBiomeSpawnCost {")
    out.append("  const char* biome;")
    out.append("  const char* entity;")
    out.append("  double charge;")
    out.append("  double energy_budget;")
    out.append("};")
    out.append("")
    out.append("// Sorted by (biome, entity); a biome with no cost does not appear at all.")
    out.append("inline constexpr GeneratedBiomeSpawnCost kBiomeSpawnCosts[] = {")
    for biome, entity, charge, energy in costs:
        out.append(f'    {{"{biome}", "{entity}", {charge!r}, {energy!r}}},')
    out.append("};")
    out.append(f"inline constexpr int kBiomeSpawnCostCount = {len(costs)};")
    out.append("")
    out.append("} // namespace mc::world")
    out.append("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(out))


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--biomes", type=pathlib.Path,
                        default=pathlib.Path(
                            "data/vanilla/26.2/data/minecraft/worldgen/biome"),
                        help="the worldgen biome data pack directory")
    parser.add_argument("--ids", type=pathlib.Path,
                        default=pathlib.Path(
                            "build/_decomp/net/minecraft/world/entity/EntityTypeIds.java"),
                        help="the decompiled entity-type id list")
    parser.add_argument("--mobs", type=pathlib.Path,
                        default=pathlib.Path("src/world/mob_table.h"),
                        help="the hand-written mob stat table")
    parser.add_argument("--tsv", type=pathlib.Path,
                        default=pathlib.Path("data/vanilla/26.2/biome_spawns.tsv"))
    parser.add_argument("--header", type=pathlib.Path,
                        default=pathlib.Path("src/world/biome_spawn_table.h"))
    args = parser.parse_args()

    if not args.biomes.is_dir():
        raise SystemExit(f"no biome data pack at {args.biomes}")
    rows, costs = build(args.biomes)
    if not rows:
        raise SystemExit("no spawner entries found: is the data pack present?")

    registry = entity_registry(args.ids)
    referenced = {row[2] for row in rows} | {cost[1] for cost in costs}
    unknown = sorted(referenced - registry)
    if unknown:
        raise SystemExit("entities not in the protocol registry: " + ", ".join(unknown))

    known = mob_table_names(args.mobs)
    reachable = [row for row in rows if row[2] in known]
    reachable_biomes = len({row[0] for row in reachable})
    biomes = len(list(args.biomes.glob("*.json")))

    write_tsv(args.tsv, rows, costs)
    write_header(args.header, rows, costs, known, len(reachable), reachable_biomes, biomes)

    print(f"{len(rows)} spawner entries over {biomes} biomes, {len(costs)} spawn costs")
    print(f"{len(reachable)} entries ({reachable_biomes} biomes) name a mob in "
          f"{args.mobs.name}: {', '.join(sorted({row[2] for row in reachable}))}")
    print(f"wrote {args.tsv} and {args.header}")


if __name__ == "__main__":
    main()
