// SPDX-License-Identifier: GPL-3.0-or-later
// clang-format off
//
// `BlockStateBase.isValidSpawn`'s overrides (62 blocks, generated —
// do not hand-edit), read out of the vanilla 26.2 `Blocks.java` by
// tools/gen_spawn_rule_table.py. This is the *ground* half of a mob's spawn
// position: `SpawnPlacementTypes.ON_GROUND` asks the block below the
// position whether the type may spawn on it, and a block with no override
// answers `isFaceSturdy(level, pos, Direction.UP)`.
//
// The unoverridden default is a *shape* question, and this engine's block
// data has no shapes (the same approximation world/light.cpp documents), so
// the runtime answers it with "the block collides" — a full cube, which is
// what a sturdy top face means for everything but a slab, a stair, a fence
// or a wall. Those four read spawnable here where vanilla reads not, and
// that is the whole of the error in this direction.
//
// The overrides themselves are exact, and split two ways:
//
//   * 5 registrations are `Blocks::always`: any mob may.
//   * 1 registrations are `entityType.fireImmune()`.
//   * 52 registrations are `Blocks::never`: no mob may spawn on it.
//   * 2 registrations are `entityType == OCELOT || PARROT`.
//   * 2 registrations are `entityType == POLAR_BEAR`.
//
// The per-type rules are resolved against the mob's own name and flags
// (`block_is_valid_spawn` in world/mob_spawn.cpp), so a polar bear added to
// the mob table later spawns on ice without touching this table.
//
// Regenerate with tools/gen_spawn_rule_table.py.
#pragma once

#include <cstdint>

namespace mc::world {

// What `isValidSpawn` answers for a block. `kSpawnRuleDefault` is the
// unoverridden `isFaceSturdy(UP)`, which the runtime approximates as "the
// block collides"; every other value is a registration in Blocks.java.
enum BlockSpawnRule : uint8_t {
  kSpawnRuleDefault = 0,
  kSpawnRuleNever,
  kSpawnRuleAlways,
  kSpawnRulePolarBear,      // entityType == POLAR_BEAR
  kSpawnRuleOcelotOrParrot, // entityType == OCELOT || PARROT
  kSpawnRuleFireImmune,     // entityType.fireImmune()
};

struct GeneratedSpawnRule {
  const char* name;
  uint8_t rule; // BlockSpawnRule
};

// Sorted by name, so block_is_valid_spawn can binary-search it. A block
// with no override is absent and reads kSpawnRuleDefault.
inline constexpr GeneratedSpawnRule kBlockSpawnRules[] = {
    {"minecraft:acacia_trapdoor", kSpawnRuleNever},
    {"minecraft:bamboo_trapdoor", kSpawnRuleNever},
    {"minecraft:barrier", kSpawnRuleNever},
    {"minecraft:bedrock", kSpawnRuleNever},
    {"minecraft:birch_trapdoor", kSpawnRuleNever},
    {"minecraft:black_stained_glass", kSpawnRuleNever},
    {"minecraft:blue_stained_glass", kSpawnRuleNever},
    {"minecraft:brown_stained_glass", kSpawnRuleNever},
    {"minecraft:carved_pumpkin", kSpawnRuleAlways},
    {"minecraft:cherry_leaves", kSpawnRuleOcelotOrParrot},
    {"minecraft:cherry_trapdoor", kSpawnRuleNever},
    {"minecraft:chorus_flower", kSpawnRuleNever},
    {"minecraft:copper_grate", kSpawnRuleNever},
    {"minecraft:copper_trapdoor", kSpawnRuleNever},
    {"minecraft:crimson_trapdoor", kSpawnRuleNever},
    {"minecraft:cyan_stained_glass", kSpawnRuleNever},
    {"minecraft:dark_oak_trapdoor", kSpawnRuleNever},
    {"minecraft:exposed_copper_grate", kSpawnRuleNever},
    {"minecraft:exposed_copper_trapdoor", kSpawnRuleNever},
    {"minecraft:frosted_ice", kSpawnRulePolarBear},
    {"minecraft:glass", kSpawnRuleNever},
    {"minecraft:gray_stained_glass", kSpawnRuleNever},
    {"minecraft:green_stained_glass", kSpawnRuleNever},
    {"minecraft:ice", kSpawnRulePolarBear},
    {"minecraft:iron_trapdoor", kSpawnRuleNever},
    {"minecraft:jack_o_lantern", kSpawnRuleAlways},
    {"minecraft:jungle_trapdoor", kSpawnRuleNever},
    {"minecraft:light_blue_stained_glass", kSpawnRuleNever},
    {"minecraft:light_gray_stained_glass", kSpawnRuleNever},
    {"minecraft:lime_stained_glass", kSpawnRuleNever},
    {"minecraft:magenta_stained_glass", kSpawnRuleNever},
    {"minecraft:magma_block", kSpawnRuleFireImmune},
    {"minecraft:mangrove_trapdoor", kSpawnRuleNever},
    {"minecraft:moving_piston", kSpawnRuleNever},
    {"minecraft:mud", kSpawnRuleAlways},
    {"minecraft:oak_trapdoor", kSpawnRuleNever},
    {"minecraft:orange_stained_glass", kSpawnRuleNever},
    {"minecraft:oxidized_copper_grate", kSpawnRuleNever},
    {"minecraft:oxidized_copper_trapdoor", kSpawnRuleNever},
    {"minecraft:pale_oak_leaves", kSpawnRuleOcelotOrParrot},
    {"minecraft:pale_oak_trapdoor", kSpawnRuleNever},
    {"minecraft:pink_stained_glass", kSpawnRuleNever},
    {"minecraft:purple_stained_glass", kSpawnRuleNever},
    {"minecraft:red_stained_glass", kSpawnRuleNever},
    {"minecraft:redstone_lamp", kSpawnRuleAlways},
    {"minecraft:scaffolding", kSpawnRuleNever},
    {"minecraft:soul_sand", kSpawnRuleAlways},
    {"minecraft:spruce_trapdoor", kSpawnRuleNever},
    {"minecraft:tinted_glass", kSpawnRuleNever},
    {"minecraft:warped_trapdoor", kSpawnRuleNever},
    {"minecraft:waxed_copper_grate", kSpawnRuleNever},
    {"minecraft:waxed_copper_trapdoor", kSpawnRuleNever},
    {"minecraft:waxed_exposed_copper_grate", kSpawnRuleNever},
    {"minecraft:waxed_exposed_copper_trapdoor", kSpawnRuleNever},
    {"minecraft:waxed_oxidized_copper_grate", kSpawnRuleNever},
    {"minecraft:waxed_oxidized_copper_trapdoor", kSpawnRuleNever},
    {"minecraft:waxed_weathered_copper_grate", kSpawnRuleNever},
    {"minecraft:waxed_weathered_copper_trapdoor", kSpawnRuleNever},
    {"minecraft:weathered_copper_grate", kSpawnRuleNever},
    {"minecraft:weathered_copper_trapdoor", kSpawnRuleNever},
    {"minecraft:white_stained_glass", kSpawnRuleNever},
    {"minecraft:yellow_stained_glass", kSpawnRuleNever},
};
inline constexpr int kBlockSpawnRuleCount = 62;

} // namespace mc::world

