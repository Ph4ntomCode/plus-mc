#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generates src/world/spawn_rule_table.h from the decompiled 26.2 `Blocks.java`:
# the per-block overrides of `BlockBehaviour.BlockStateBase.isValidSpawn`, which
# is the *ground* question a mob's spawn position asks (`SpawnPlacementTypes
# .ON_GROUND` -> `belowState.isValidSpawn(level, below, type)`, called through
# `Mob.checkMobSpawnRules`).
#
# The default is `isFaceSturdy(level, pos, Direction.UP)` — a full top face —
# and 26.2 overrides it for 32 registrations (Phase 4 milestone C). Each
# override is one of:
#
#   * `Blocks::never`   (23 registrations: glass, the trapdoors, ice-free
#     decorative cubes) — no mob may spawn on it;
#   * `Blocks::always`  (5: soul_sand, carved_pumpkin, jack_o_lantern,
#     redstone_lamp, mud);
#   * a lambda, which is *per entity type*: `entityType == POLAR_BEAR` (ice,
#     frosted_ice), `entityType == OCELOT || entityType == PARROT` (the two
#     leaves) and `entityType.fireImmune()` (magma_block).
#
# So the emitted table is a rule id per block name, and the runtime resolves the
# per-type rules against the mob's own type name and flags (world/mob_spawn.cpp,
# `block_is_valid_spawn`). A block with no override is absent and reads the
# default — that is the whole lookup, one binary search.
#
# `STAINED_GLASS` is registered as a `ColorCollection`, so its one source line
# stands for every colour; the expansion is the project registry's own
# `*_stained_glass` names, which is checkable and fails loudly if the colour set
# ever changes.
#
# The same run also emits `spawn_placement_table.h` from `SpawnPlacements.java`:
# the *per-type* half of the question — the placement type (`ON_GROUND`,
# `IN_WATER`, `IN_LAVA`, `NO_RESTRICTIONS`), the heightmap the type measures
# against, and the spawn-rule predicate (`Monster::checkMonsterSpawnRules`,
# `Animal::checkAnimalSpawnRules`, ...). `mob_spawn.cpp` resolves the predicate
# id for a type it can spawn; the mob-specific predicates (a goat's slope, a
# turtle's beach) are carried for provenance and refused at runtime, because
# there is no mob for them yet.
#
# Regenerate with: python3 tools/gen_spawn_rule_table.py

import argparse
import pathlib
import re
import sys

# `isValidSpawn(Blocks::<ref>)` and the lambdas, in the order the enum names them.
BLOCK_RULES = {
    "never": "Never",
    "always": "Always",
    # `Blocks.ocelotOrParrot`: the two `UntintedParticleLeavesBlock`s (cherry and
    # pale oak) build the same predicate from a helper method.
    "ocelotOrParrot": "OcelotOrParrot",
}
LAMBDA_RULES = [
    ("entityType == EntityTypes.POLAR_BEAR", "PolarBear"),
    ("entityType == EntityTypes.OCELOT", "OcelotOrParrot"),
    ("entityType.fireImmune()", "FireImmune"),
]


def block_names(registry_header: pathlib.Path):
    text = registry_header.read_text(encoding="utf-8")
    return set(re.findall(r'\{"(minecraft:[a-z0-9_/]+)",', text))


# A one-line registration that stands for a *family* of blocks: the constant's
# own suffix is looked up in the project registry, so the expansion is checkable
# (16 colours, 8 weathering stages) instead of a list copied into this script.
COLLECTIONS = {
    "STAINED_GLASS": ("stained_glass", 16),
    # `WeatheringCopperCollection.registerBlocks` builds the four stages and
    # their waxed twins from the constant's own name (`copper_trapdoor`), so the
    # base suffix finds all eight — including the unaugmented one, which has no
    # `_` in front of the base.
    "COPPER_TRAPDOOR": ("copper_trapdoor", 8),
    "COPPER_GRATE": ("copper_grate", 8),
}


def build(blocks_java: pathlib.Path, registry: set):
    """(name -> rule) for every block whose isValidSpawn is overridden."""
    rules = {}
    lines = blocks_java.read_text(encoding="utf-8").splitlines()
    registrations = [line for line in lines if "isValidSpawn(" in line and ".register" in line]
    skipped = [line for line in lines if "isValidSpawn(" in line and ".register" not in line]
    # One non-registration line is expected: the leaf helper that builds its
    # properties from `Blocks.ocelotOrParrot`. Anything else means a source
    # change that this script would otherwise drop silently.
    for line in skipped:
        if "leavesProperties" not in line and "return BlockBehaviour.Properties" not in line:
            print(f"UNHANDLED isValidSpawn line: {line.strip()[:120]}", file=sys.stderr)
            sys.exit(1)
    for line in registrations:
        ref = re.search(r"BlockItemIds\.([A-Z_0-9]+)|BlockIds\.([A-Z_0-9]+)", line)
        if ref is None:
            continue
        constant = ref.group(1) or ref.group(2)
        # Take the call's argument by balancing parentheses: a lambda argument
        # carries its own `(state, blockGetter, ...)` inside it, so a regex that
        # stops at the first `)` splits it in half.
        start = line.index("isValidSpawn(") + len("isValidSpawn(")
        depth = 1
        end = start
        while end < len(line) and depth > 0:
            if line[end] == "(":
                depth += 1
            elif line[end] == ")":
                depth -= 1
            end += 1
        if depth != 0:
            continue
        argument = line[start:end - 1]
        rule = None
        for needle, name in BLOCK_RULES.items():
            if argument == f"Blocks::{needle}":
                rule = name
        if rule is None:
            for needle, name in LAMBDA_RULES:
                if needle in argument:
                    rule = name
        if rule is None:
            print(f"UNRECOGNISED isValidSpawn argument: {argument}", file=sys.stderr)
            sys.exit(1)

        names = []
        if constant in COLLECTIONS:
            suffix, expected = COLLECTIONS[constant]
            names = sorted(n for n in registry if n.endswith(suffix))
            if len(names) != expected:
                print(f"{constant} expansion found {len(names)} blocks, expected {expected}",
                      file=sys.stderr)
                sys.exit(1)
        else:
            names = ["minecraft:" + constant.lower()]
        for name in names:
            if name not in registry:
                print(f"NOT IN THE PROJECT REGISTRY: {name}", file=sys.stderr)
                sys.exit(1)
            previous = rules.get(name)
            if previous is not None and previous != rule:
                print(f"CONFLICTING RULES for {name}: {previous} vs {rule}", file=sys.stderr)
                sys.exit(1)
            rules[name] = rule
    if not rules:
        print("no isValidSpawn overrides found: is Blocks.java present?", file=sys.stderr)
        sys.exit(1)
    return rules


# `SpawnPlacementTypes.NAME` and the predicate reference `Class::method`, as
# `SpawnPlacements.java` spells them.
PLACEMENT_FIELDS = ["NO_RESTRICTIONS", "IN_WATER", "IN_LAVA"]
PLACEMENT_LAMBDA = re.compile(r"^\(level, blockPos, type\)")
REGISTRATION_PLACEMENT = re.compile(
    r"SpawnPlacements\.register\(EntityTypes\.([A-Z_0-9]+),\s*"
    r"(?:SpawnPlacementTypes\.([A-Z_0-9]+)|(new SpawnPlacementType\{\s*))\)?")
PLACEMENT_ENTRY = re.compile(
    r"SpawnPlacements\.register\(EntityTypes\.([A-Z_0-9]+),\s*"
    r"SpawnPlacementTypes\.([A-Z_0-9]+),\s*"
    r"Heightmap\.Types\.([A-Z_0-9]+),\s*"
    r"([A-Za-z0-9_]+)::([A-Za-z0-9_]+)\)")
ON_GROUND_ENTRY = re.compile(
    r"SpawnPlacements\.register\(EntityTypes\.([A-Z_0-9]+),\s*"
    r"SpawnPlacementTypes\.ON_GROUND,\s*"
    r"Heightmap\.Types\.([A-Z_0-9]+),\s*"
    r"([A-Za-z0-9_]+)::([A-Za-z0-9_]+)\)")


def build_placements(spawn_placements_java: pathlib.Path) -> dict:
    """(entity -> (placement, heightmap, predicate)) for every registration.

    `ON_GROUND` is not a constant in the source — it is an anonymous
    `SpawnPlacementType`, so its registrations are matched on their own shape
    rather than on a `SpawnPlacementTypes.X` field reference.
    """
    text = spawn_placements_java.read_text(encoding="utf-8")
    flat = re.sub(r"\s+", " ", text)
    out = {}
    for match in ON_GROUND_ENTRY.finditer(flat):
        entity, heightmap, cls, method = match.group(1, 2, 3, 4)
        out[entity] = ("ON_GROUND", heightmap, cls, method)
    for match in PLACEMENT_ENTRY.finditer(flat):
        entity, placement, heightmap, cls, method = match.group(1, 2, 3, 4, 5)
        if placement == "ON_GROUND":
            continue  # impossible; kept so the two patterns cannot disagree
        out[entity] = (placement, heightmap, cls, method)
    return out


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--blocks-java", type=pathlib.Path,
                        default=root / "build/_decomp/net/minecraft/world/level/block/Blocks.java")
    parser.add_argument("--registry", type=pathlib.Path,
                        default=root / "src/world/block_states.h")
    parser.add_argument("--tsv", type=pathlib.Path,
                        default=root / "data/vanilla/26.2/spawn_rules.tsv")
    parser.add_argument("--header", type=pathlib.Path,
                        default=root / "src/world/spawn_rule_table.h")
    parser.add_argument("--spawn-placements-java", type=pathlib.Path,
                        default=root / "build/_decomp/net/minecraft/world/entity/SpawnPlacements.java")
    parser.add_argument("--placements-tsv", type=pathlib.Path,
                        default=root / "data/vanilla/26.2/spawn_placements.tsv")
    parser.add_argument("--placements-header", type=pathlib.Path,
                        default=root / "src/world/spawn_placement_table.h")
    parser.add_argument("--biome-spawns", type=pathlib.Path,
                        default=root / "data/vanilla/26.2/biome_spawns.tsv")
    args = parser.parse_args()

    registry = block_names(args.registry)
    rules = build(args.blocks_java, registry)

    counts = {}
    for rule in rules.values():
        counts[rule] = counts.get(rule, 0) + 1

    lines = ["block\tspawn_rule"]
    lines += [f"{name}\t{rules[name]}" for name in sorted(rules)]
    args.tsv.parent.mkdir(parents=True, exist_ok=True)
    args.tsv.write_text("\n".join(lines) + "\n")

    out = []
    out.append("// SPDX-License-Identifier: GPL-3.0-or-later")
    out.append("// clang-format off")
    out.append("//")
    out.append(f"// `BlockStateBase.isValidSpawn`'s overrides ({len(rules)} blocks, generated —")
    out.append("// do not hand-edit), read out of the vanilla 26.2 `Blocks.java` by")
    out.append("// tools/gen_spawn_rule_table.py. This is the *ground* half of a mob's spawn")
    out.append("// position: `SpawnPlacementTypes.ON_GROUND` asks the block below the")
    out.append("// position whether the type may spawn on it, and a block with no override")
    out.append("// answers `isFaceSturdy(level, pos, Direction.UP)`.")
    out.append("//")
    out.append("// The unoverridden default is a *shape* question, and this engine's block")
    out.append("// data has no shapes (the same approximation world/light.cpp documents), so")
    out.append("// the runtime answers it with \"the block collides\" — a full cube, which is")
    out.append("// what a sturdy top face means for everything but a slab, a stair, a fence")
    out.append("// or a wall. Those four read spawnable here where vanilla reads not, and")
    out.append("// that is the whole of the error in this direction.")
    out.append("//")
    out.append("// The overrides themselves are exact, and split two ways:")
    out.append("//")
    for rule, count in sorted(counts.items()):
        detail = {
            "Never": "`Blocks::never`: no mob may spawn on it",
            "Always": "`Blocks::always`: any mob may",
            "PolarBear": "`entityType == POLAR_BEAR`",
            "OcelotOrParrot": "`entityType == OCELOT || PARROT`",
            "FireImmune": "`entityType.fireImmune()`",
        }.get(rule, rule)
        out.append(f"//   * {count} registrations are {detail}.")
    out.append("//")
    out.append("// The per-type rules are resolved against the mob's own name and flags")
    out.append("// (`block_is_valid_spawn` in world/mob_spawn.cpp), so a polar bear added to")
    out.append("// the mob table later spawns on ice without touching this table.")
    out.append("//")
    out.append("// Regenerate with tools/gen_spawn_rule_table.py.")
    out.append("#pragma once")
    out.append("")
    out.append("#include <cstdint>")
    out.append("")
    out.append("namespace mc::world {")
    out.append("")
    out.append("// What `isValidSpawn` answers for a block. `kSpawnRuleDefault` is the")
    out.append("// unoverridden `isFaceSturdy(UP)`, which the runtime approximates as \"the")
    out.append("// block collides\"; every other value is a registration in Blocks.java.")
    out.append("enum BlockSpawnRule : uint8_t {")
    out.append("  kSpawnRuleDefault = 0,")
    out.append("  kSpawnRuleNever,")
    out.append("  kSpawnRuleAlways,")
    out.append("  kSpawnRulePolarBear,      // entityType == POLAR_BEAR")
    out.append("  kSpawnRuleOcelotOrParrot, // entityType == OCELOT || PARROT")
    out.append("  kSpawnRuleFireImmune,     // entityType.fireImmune()")
    out.append("};")
    out.append("")
    out.append("struct GeneratedSpawnRule {")
    out.append("  const char* name;")
    out.append("  uint8_t rule; // BlockSpawnRule")
    out.append("};")
    out.append("")
    out.append("// Sorted by name, so block_is_valid_spawn can binary-search it. A block")
    out.append("// with no override is absent and reads kSpawnRuleDefault.")
    out.append("inline constexpr GeneratedSpawnRule kBlockSpawnRules[] = {")
    for name in sorted(rules):
        out.append(f'    {{"{name}", kSpawnRule{rules[name]}}},')
    out.append("};")
    out.append(f"inline constexpr int kBlockSpawnRuleCount = {len(rules)};")
    out.append("")
    out.append("} // namespace mc::world")
    out.append("")
    args.header.write_text("\n".join(out) + "\n")
    print(f"{len(rules)} blocks override isValidSpawn: "
          + ", ".join(f"{rule}={count}" for rule, count in sorted(counts.items())))
    print(f"wrote {args.tsv} and {args.header}")
    write_placements(args)


def write_placements(args):
    """The per-type half: `SpawnPlacements.java`'s registration table."""
    placements = build_placements(args.spawn_placements_java)
    if not placements:
        print("no SpawnPlacements registrations found", file=sys.stderr)
        sys.exit(1)
    # Cross-check the name this script derives from the Java constant against
    # the entity names the biome data pack actually lists. Types that are never
    # in a biome list (the ender dragon, a summoned golem) are informational;
    # a name that *looks* deriveable but does not appear anywhere is worth
    # knowing about, so both counts are printed.
    known = set()
    if args.biome_spawns.exists():
        for line in args.biome_spawns.read_text(encoding="utf-8").splitlines()[1:]:
            parts = line.split("\t")
            if len(parts) > 2:
                known.add(parts[2])
    rows = {}
    for entity, (placement, heightmap, cls, method) in placements.items():
        name = "minecraft:" + entity.lower()
        if heightmap not in ("MOTION_BLOCKING_NO_LEAVES", "MOTION_BLOCKING", "WORLD_SURFACE_WG"):
            print(f"UNEXPECTED heightmap {heightmap} for {entity}", file=sys.stderr)
            sys.exit(1)
        rows[name] = (placement, f"{cls}::{method}")
    unknown = sorted(n for n in rows if known and n not in known)

    lines = ["entity\tplacement\tspawn_rule"]
    lines += [f"{name}\t{rows[name][0]}\t{rows[name][1]}" for name in sorted(rows)]
    args.placements_tsv.write_text("\n".join(lines) + "\n")

    counts = {}
    for placement, _ in rows.values():
        counts[placement] = counts.get(placement, 0) + 1
    out = []
    out.append("// SPDX-License-Identifier: GPL-3.0-or-later")
    out.append("// clang-format off")
    out.append("//")
    out.append(f"// `SpawnPlacements`'s registration table ({len(rows)} entity types, generated —")
    out.append("// do not hand-edit), read out of the vanilla 26.2")
    out.append("// `world/entity/SpawnPlacements.java` by tools/gen_spawn_rule_table.py.")
    out.append("// This is the *per-type* half of \"may a mob of this type spawn here\":")
    out.append("//")
    out.append("//   * the placement type — `ON_GROUND` asks the block below to be a valid")
    out.append("//     spawn ground and the two cells of the mob's box to be empty,")
    out.append("//     `IN_WATER` / `IN_LAVA` ask the cell to be that fluid, and")
    out.append("//     `NO_RESTRICTIONS` asks nothing;")
    out.append("//   * the spawn-rule predicate, resolved in world/mob_spawn.cpp for the")
    out.append("//     handful of shared predicates this engine can answer")
    out.append("//     (`Mob::checkMobSpawnRules`, `Monster::checkMonsterSpawnRules`,")
    out.append("//     `Monster::checkAnyLightMonsterSpawnRules`,")
    out.append("//     `Monster::checkSurfaceMonstersSpawnRules`, `Animal::checkAnimalSpawnRules`).")
    out.append("//     The mob-specific ones (a goat's slope, an ocelot's jungle) are carried")
    out.append("//     for provenance and refused at runtime — `kSpawnRuleUnmodelled` — because")
    out.append("//     there is no mob of that type in world/mob_table.h to spawn yet.")
    out.append("//")
    out.append("// `Heightmap.Types` is not carried: the only heightmap the runtime asks for")
    out.append("// is `WORLD_SURFACE`, which `NaturalSpawner.getRandomPosWithin` names directly")
    out.append("// rather than reading from here. All %d registrations use a motion-blocking"
               % len(rows))
    out.append("// heightmap anyway, so nothing is lost.")
    out.append("//")
    out.append("// Regenerate with tools/gen_spawn_rule_table.py.")
    out.append("#pragma once")
    out.append("")
    out.append("#include <cstdint>")
    out.append("")
    out.append("namespace mc::world {")
    out.append("")
    out.append("// `SpawnPlacementTypes`' four answers.")
    out.append("enum MobSpawnPlacement : uint8_t {")
    out.append("  kPlacementOnGround = 0, // SpawnPlacementTypes.ON_GROUND")
    out.append("  kPlacementInWater,      // SpawnPlacementTypes.IN_WATER")
    out.append("  kPlacementInLava,       // SpawnPlacementTypes.IN_LAVA")
    out.append("  kPlacementNoRestrictions, // the default for an unregistered type")
    out.append("};")
    out.append("")
    out.append("// The spawn-rule predicates the runtime knows how to answer. A predicate")
    out.append("// outside this set is `kSpawnRuleUnmodelled`, which never spawns.")
    out.append("enum MobSpawnRule : uint8_t {")
    out.append("  kSpawnRuleMob = 0,        // Mob::checkMobSpawnRules (the ground alone)")
    out.append("  kSpawnRuleMonster,        // Monster::checkMonsterSpawnRules (darkness + ground)")
    out.append("  kSpawnRuleAnyLightMonster, // Monster::checkAnyLightMonsterSpawnRules (ground)")
    out.append("  kSpawnRuleSurfaceMonster, // ... && Level.canSeeSky")
    out.append("  kSpawnRuleAnimal,         // Animal::checkAnimalSpawnRules (bright + tag)")
    out.append("  kSpawnRuleUnmodelled,     // a mob-specific predicate, refused")
    out.append("};")
    out.append("")
    out.append("struct GeneratedSpawnPlacement {")
    out.append("  const char* entity;    // \"minecraft:zombie\"")
    out.append("  uint8_t placement;     // MobSpawnPlacement")
    out.append("  uint8_t rule;          // MobSpawnRule")
    out.append("  const char* source;    // the predicate as SpawnPlacements.java names it")
    out.append("};")
    out.append("")
    out.append("// Sorted by entity, so mob_spawn_placement can binary-search it.")
    out.append("inline constexpr GeneratedSpawnPlacement kSpawnPlacements[] = {")
    for name in sorted(rows):
        placement, rule = rows[name]
        out.append(f'    {{"{name}", kPlacement{placement.title().replace("_", "")}, '
                   f'{spawn_rule_id(rule)}, "{rule}"}},')
    out.append("};")
    out.append(f"inline constexpr int kSpawnPlacementCount = {len(rows)};")
    out.append("")
    out.append("} // namespace mc::world")
    out.append("")
    args.placements_header.write_text("\n".join(out) + "\n")
    print(f"{len(rows)} entity types registered in SpawnPlacements: "
          + ", ".join(f"{p}={c}" for p, c in sorted(counts.items()))
          + f"; {len(unknown)} are in no biome spawn list ("
          + ", ".join(unknown[:6]) + ")" if unknown else "")
    print(f"wrote {args.placements_tsv} and {args.placements_header}")


# The predicate the *runtime* implements for a source name. Anything else is
# unmodelled and never spawns.
SPAWN_RULE_IDS = {
    "Mob::checkMobSpawnRules": "kSpawnRuleMob",
    "Monster::checkMonsterSpawnRules": "kSpawnRuleMonster",
    "Monster::checkAnyLightMonsterSpawnRules": "kSpawnRuleAnyLightMonster",
    "Monster::checkSurfaceMonstersSpawnRules": "kSpawnRuleSurfaceMonster",
    "Animal::checkAnimalSpawnRules": "kSpawnRuleAnimal",
}


def spawn_rule_id(source):
    return SPAWN_RULE_IDS.get(source, "kSpawnRuleUnmodelled")


if __name__ == "__main__":
    main()
