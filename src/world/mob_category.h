// SPDX-License-Identifier: GPL-3.0-or-later
//
// `MobCategory`'s numbers (Phase 4 milestone C): the per-category spawn cap, the
// friendly/persistent flags and the two despawn distances. One hand-written row
// per category, transcribed from `world/entity/MobCategory.java`:
//
//     MONSTER("monster", "MO", 70, false, false, 128)
//     CREATURE("creature", "C", 10, true, true, 128)
//     AMBIENT("ambient", "AM", 15, true, false, 128)
//     AXOLOTLS("axolotls", "AX", 5, true, false, 128)
//     UNDERGROUND_WATER_CREATURE("underground_water_creature", "UWC", 5, true, false, 128)
//     WATER_CREATURE("water_creature", "WC", 5, true, false, 128)
//     WATER_AMBIENT("water_ambient", "WA", 20, true, false, 64)
//     MISC("misc", "MI", -1, true, true, 128)
//
// `getNoDespawnDistance()` is not a field: the class returns the constant 32.
//
// The *category vocabulary* itself is not here — it is generated, because the
// order is `MobCategory.values()` and the biome data pack indexes into it
// (world/biome_spawn_table.h). This table is the numbers that go with it, in
// that same order, which a unit test pins by name so a reordering of one
// without the other cannot pass.
#pragma once

#include <cstdint>

#include "world/biome_spawn_table.h" // MobSpawnCategory, kSpawnCategoryCount

namespace mc::world {

struct MobCategoryStats {
  uint8_t category;            // MobSpawnCategory (redundant, so a row can be checked)
  int max_instances_per_chunk; // MobCategory.getMaxInstancesPerChunk()
  bool friendly;               // MobCategory.isFriendly() — true = not a monster
  bool persistent;             // MobCategory.isPersistent() — true = always spawned
  int despawn_distance;        // MobCategory.getDespawnDistance()
  int no_despawn_distance;     // MobCategory.getNoDespawnDistance()
};

// Indexed by `MobSpawnCategory`, in `MobCategory.values()` order.
inline constexpr MobCategoryStats kMobCategories[kSpawnCategoryCount] = {
    {kSpawnMonster, 70, false, false, 128, 32},
    {kSpawnCreature, 10, true, true, 128, 32},
    {kSpawnAmbient, 15, true, false, 128, 32},
    {kSpawnAxolotls, 5, true, false, 128, 32},
    {kSpawnUndergroundWaterCreature, 5, true, false, 128, 32},
    {kSpawnWaterCreature, 5, true, false, 128, 32},
    {kSpawnWaterAmbient, 20, true, false, 64, 32},
    {kSpawnMisc, -1, true, true, 128, 32},
};

// The row for a category, or the MISC row for an out-of-range value (which is
// what a corrupt table entry would be).
inline const MobCategoryStats& mob_category_stats(uint8_t category) {
  return category < kSpawnCategoryCount ? kMobCategories[category] : kMobCategories[kSpawnMisc];
}

} // namespace mc::world
