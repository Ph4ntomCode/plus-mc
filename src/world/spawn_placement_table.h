// SPDX-License-Identifier: GPL-3.0-or-later
// clang-format off
//
// `SpawnPlacements`'s registration table (83 entity types, generated —
// do not hand-edit), read out of the vanilla 26.2
// `world/entity/SpawnPlacements.java` by tools/gen_spawn_rule_table.py.
// This is the *per-type* half of "may a mob of this type spawn here":
//
//   * the placement type — `ON_GROUND` asks the block below to be a valid
//     spawn ground and the two cells of the mob's box to be empty,
//     `IN_WATER` / `IN_LAVA` ask the cell to be that fluid, and
//     `NO_RESTRICTIONS` asks nothing;
//   * the spawn-rule predicate, resolved in world/mob_spawn.cpp for the
//     handful of shared predicates this engine can answer
//     (`Mob::checkMobSpawnRules`, `Monster::checkMonsterSpawnRules`,
//     `Monster::checkAnyLightMonsterSpawnRules`,
//     `Monster::checkSurfaceMonstersSpawnRules`, `Animal::checkAnimalSpawnRules`).
//     The mob-specific ones (a goat's slope, an ocelot's jungle) are carried
//     for provenance and refused at runtime — `kSpawnRuleUnmodelled` — because
//     there is no mob of that type in world/mob_table.h to spawn yet.
//
// `Heightmap.Types` is not carried: the only heightmap the runtime asks for
// is `WORLD_SURFACE`, which `NaturalSpawner.getRandomPosWithin` names directly
// rather than reading from here. All 83 registrations use a motion-blocking
// heightmap anyway, so nothing is lost.
//
// Regenerate with tools/gen_spawn_rule_table.py.
#pragma once

#include <cstdint>

namespace mc::world {

// `SpawnPlacementTypes`' four answers.
enum MobSpawnPlacement : uint8_t {
  kPlacementOnGround = 0, // SpawnPlacementTypes.ON_GROUND
  kPlacementInWater,      // SpawnPlacementTypes.IN_WATER
  kPlacementInLava,       // SpawnPlacementTypes.IN_LAVA
  kPlacementNoRestrictions, // the default for an unregistered type
};

// The spawn-rule predicates the runtime knows how to answer. A predicate
// outside this set is `kSpawnRuleUnmodelled`, which never spawns.
enum MobSpawnRule : uint8_t {
  kSpawnRuleMob = 0,        // Mob::checkMobSpawnRules (the ground alone)
  kSpawnRuleMonster,        // Monster::checkMonsterSpawnRules (darkness + ground)
  kSpawnRuleAnyLightMonster, // Monster::checkAnyLightMonsterSpawnRules (ground)
  kSpawnRuleSurfaceMonster, // ... && Level.canSeeSky
  kSpawnRuleAnimal,         // Animal::checkAnimalSpawnRules (bright + tag)
  kSpawnRuleUnmodelled,     // a mob-specific predicate, refused
};

struct GeneratedSpawnPlacement {
  const char* entity;    // "minecraft:zombie"
  uint8_t placement;     // MobSpawnPlacement
  uint8_t rule;          // MobSpawnRule
  const char* source;    // the predicate as SpawnPlacements.java names it
};

// Sorted by entity, so mob_spawn_placement can binary-search it.
inline constexpr GeneratedSpawnPlacement kSpawnPlacements[] = {
    {"minecraft:armadillo", kPlacementOnGround, kSpawnRuleUnmodelled, "Armadillo::checkArmadilloSpawnRules"},
    {"minecraft:axolotl", kPlacementInWater, kSpawnRuleUnmodelled, "Axolotl::checkAxolotlSpawnRules"},
    {"minecraft:bat", kPlacementOnGround, kSpawnRuleUnmodelled, "Bat::checkBatSpawnRules"},
    {"minecraft:blaze", kPlacementOnGround, kSpawnRuleAnyLightMonster, "Monster::checkAnyLightMonsterSpawnRules"},
    {"minecraft:bogged", kPlacementOnGround, kSpawnRuleMonster, "Monster::checkMonsterSpawnRules"},
    {"minecraft:breeze", kPlacementOnGround, kSpawnRuleAnyLightMonster, "Monster::checkAnyLightMonsterSpawnRules"},
    {"minecraft:camel", kPlacementOnGround, kSpawnRuleUnmodelled, "Camel::checkCamelSpawnRules"},
    {"minecraft:camel_husk", kPlacementOnGround, kSpawnRuleSurfaceMonster, "Monster::checkSurfaceMonstersSpawnRules"},
    {"minecraft:cat", kPlacementOnGround, kSpawnRuleAnimal, "Animal::checkAnimalSpawnRules"},
    {"minecraft:cave_spider", kPlacementOnGround, kSpawnRuleMonster, "Monster::checkMonsterSpawnRules"},
    {"minecraft:chicken", kPlacementOnGround, kSpawnRuleAnimal, "Animal::checkAnimalSpawnRules"},
    {"minecraft:cod", kPlacementInWater, kSpawnRuleUnmodelled, "WaterAnimal::checkSurfaceWaterAnimalSpawnRules"},
    {"minecraft:cow", kPlacementOnGround, kSpawnRuleAnimal, "Animal::checkAnimalSpawnRules"},
    {"minecraft:creaking", kPlacementOnGround, kSpawnRuleMonster, "Monster::checkMonsterSpawnRules"},
    {"minecraft:creeper", kPlacementOnGround, kSpawnRuleMonster, "Monster::checkMonsterSpawnRules"},
    {"minecraft:dolphin", kPlacementInWater, kSpawnRuleUnmodelled, "AgeableWaterCreature::checkSurfaceAgeableWaterCreatureSpawnRules"},
    {"minecraft:donkey", kPlacementOnGround, kSpawnRuleAnimal, "Animal::checkAnimalSpawnRules"},
    {"minecraft:drowned", kPlacementInWater, kSpawnRuleUnmodelled, "Drowned::checkDrownedSpawnRules"},
    {"minecraft:elder_guardian", kPlacementInWater, kSpawnRuleUnmodelled, "Guardian::checkGuardianSpawnRules"},
    {"minecraft:ender_dragon", kPlacementOnGround, kSpawnRuleMob, "Mob::checkMobSpawnRules"},
    {"minecraft:enderman", kPlacementOnGround, kSpawnRuleMonster, "Monster::checkMonsterSpawnRules"},
    {"minecraft:endermite", kPlacementOnGround, kSpawnRuleUnmodelled, "Endermite::checkEndermiteSpawnRules"},
    {"minecraft:evoker", kPlacementNoRestrictions, kSpawnRuleMonster, "Monster::checkMonsterSpawnRules"},
    {"minecraft:fox", kPlacementNoRestrictions, kSpawnRuleUnmodelled, "Fox::checkFoxSpawnRules"},
    {"minecraft:frog", kPlacementOnGround, kSpawnRuleUnmodelled, "Frog::checkFrogSpawnRules"},
    {"minecraft:ghast", kPlacementOnGround, kSpawnRuleUnmodelled, "Ghast::checkGhastSpawnRules"},
    {"minecraft:giant", kPlacementOnGround, kSpawnRuleMonster, "Monster::checkMonsterSpawnRules"},
    {"minecraft:glow_squid", kPlacementInWater, kSpawnRuleUnmodelled, "GlowSquid::checkGlowSquidSpawnRules"},
    {"minecraft:goat", kPlacementOnGround, kSpawnRuleUnmodelled, "Goat::checkGoatSpawnRules"},
    {"minecraft:guardian", kPlacementInWater, kSpawnRuleUnmodelled, "Guardian::checkGuardianSpawnRules"},
    {"minecraft:happy_ghast", kPlacementOnGround, kSpawnRuleAnimal, "Animal::checkAnimalSpawnRules"},
    {"minecraft:hoglin", kPlacementOnGround, kSpawnRuleUnmodelled, "Hoglin::checkHoglinSpawnRules"},
    {"minecraft:horse", kPlacementOnGround, kSpawnRuleAnimal, "Animal::checkAnimalSpawnRules"},
    {"minecraft:husk", kPlacementOnGround, kSpawnRuleSurfaceMonster, "Monster::checkSurfaceMonstersSpawnRules"},
    {"minecraft:illusioner", kPlacementNoRestrictions, kSpawnRuleMonster, "Monster::checkMonsterSpawnRules"},
    {"minecraft:iron_golem", kPlacementOnGround, kSpawnRuleMob, "Mob::checkMobSpawnRules"},
    {"minecraft:llama", kPlacementOnGround, kSpawnRuleAnimal, "Animal::checkAnimalSpawnRules"},
    {"minecraft:magma_cube", kPlacementOnGround, kSpawnRuleUnmodelled, "MagmaCube::checkMagmaCubeSpawnRules"},
    {"minecraft:mooshroom", kPlacementOnGround, kSpawnRuleUnmodelled, "MushroomCow::checkMushroomSpawnRules"},
    {"minecraft:mule", kPlacementOnGround, kSpawnRuleAnimal, "Animal::checkAnimalSpawnRules"},
    {"minecraft:nautilus", kPlacementInWater, kSpawnRuleUnmodelled, "AbstractNautilus::checkNautilusSpawnRules"},
    {"minecraft:ocelot", kPlacementOnGround, kSpawnRuleUnmodelled, "Ocelot::checkOcelotSpawnRules"},
    {"minecraft:panda", kPlacementNoRestrictions, kSpawnRuleAnimal, "Animal::checkAnimalSpawnRules"},
    {"minecraft:parched", kPlacementOnGround, kSpawnRuleSurfaceMonster, "Monster::checkSurfaceMonstersSpawnRules"},
    {"minecraft:parrot", kPlacementOnGround, kSpawnRuleUnmodelled, "Parrot::checkParrotSpawnRules"},
    {"minecraft:phantom", kPlacementNoRestrictions, kSpawnRuleMob, "Mob::checkMobSpawnRules"},
    {"minecraft:pig", kPlacementOnGround, kSpawnRuleAnimal, "Animal::checkAnimalSpawnRules"},
    {"minecraft:piglin", kPlacementOnGround, kSpawnRuleUnmodelled, "Piglin::checkPiglinSpawnRules"},
    {"minecraft:pillager", kPlacementOnGround, kSpawnRuleUnmodelled, "PatrollingMonster::checkPatrollingMonsterSpawnRules"},
    {"minecraft:polar_bear", kPlacementOnGround, kSpawnRuleUnmodelled, "PolarBear::checkPolarBearSpawnRules"},
    {"minecraft:pufferfish", kPlacementInWater, kSpawnRuleUnmodelled, "WaterAnimal::checkSurfaceWaterAnimalSpawnRules"},
    {"minecraft:rabbit", kPlacementOnGround, kSpawnRuleUnmodelled, "Rabbit::checkRabbitSpawnRules"},
    {"minecraft:ravager", kPlacementOnGround, kSpawnRuleMonster, "Monster::checkMonsterSpawnRules"},
    {"minecraft:salmon", kPlacementInWater, kSpawnRuleUnmodelled, "WaterAnimal::checkSurfaceWaterAnimalSpawnRules"},
    {"minecraft:sheep", kPlacementOnGround, kSpawnRuleAnimal, "Animal::checkAnimalSpawnRules"},
    {"minecraft:shulker", kPlacementNoRestrictions, kSpawnRuleMob, "Mob::checkMobSpawnRules"},
    {"minecraft:silverfish", kPlacementOnGround, kSpawnRuleUnmodelled, "Silverfish::checkSilverfishSpawnRules"},
    {"minecraft:skeleton", kPlacementOnGround, kSpawnRuleMonster, "Monster::checkMonsterSpawnRules"},
    {"minecraft:skeleton_horse", kPlacementOnGround, kSpawnRuleUnmodelled, "SkeletonHorse::checkSkeletonHorseSpawnRules"},
    {"minecraft:slime", kPlacementOnGround, kSpawnRuleUnmodelled, "Slime::checkSlimeSpawnRules"},
    {"minecraft:snow_golem", kPlacementOnGround, kSpawnRuleMob, "Mob::checkMobSpawnRules"},
    {"minecraft:spider", kPlacementOnGround, kSpawnRuleMonster, "Monster::checkMonsterSpawnRules"},
    {"minecraft:squid", kPlacementInWater, kSpawnRuleUnmodelled, "AgeableWaterCreature::checkSurfaceAgeableWaterCreatureSpawnRules"},
    {"minecraft:stray", kPlacementOnGround, kSpawnRuleUnmodelled, "Stray::checkStraySpawnRules"},
    {"minecraft:strider", kPlacementInLava, kSpawnRuleUnmodelled, "Strider::checkStriderSpawnRules"},
    {"minecraft:sulfur_cube", kPlacementOnGround, kSpawnRuleUnmodelled, "SulfurCube::checkSulfurCubeSpawnRules"},
    {"minecraft:trader_llama", kPlacementNoRestrictions, kSpawnRuleAnimal, "Animal::checkAnimalSpawnRules"},
    {"minecraft:tropical_fish", kPlacementInWater, kSpawnRuleUnmodelled, "TropicalFish::checkTropicalFishSpawnRules"},
    {"minecraft:turtle", kPlacementOnGround, kSpawnRuleUnmodelled, "Turtle::checkTurtleSpawnRules"},
    {"minecraft:vex", kPlacementNoRestrictions, kSpawnRuleMonster, "Monster::checkMonsterSpawnRules"},
    {"minecraft:villager", kPlacementOnGround, kSpawnRuleMob, "Mob::checkMobSpawnRules"},
    {"minecraft:vindicator", kPlacementNoRestrictions, kSpawnRuleMonster, "Monster::checkMonsterSpawnRules"},
    {"minecraft:wandering_trader", kPlacementOnGround, kSpawnRuleMob, "Mob::checkMobSpawnRules"},
    {"minecraft:warden", kPlacementNoRestrictions, kSpawnRuleMonster, "Monster::checkMonsterSpawnRules"},
    {"minecraft:witch", kPlacementOnGround, kSpawnRuleMonster, "Monster::checkMonsterSpawnRules"},
    {"minecraft:wither", kPlacementOnGround, kSpawnRuleMonster, "Monster::checkMonsterSpawnRules"},
    {"minecraft:wither_skeleton", kPlacementOnGround, kSpawnRuleMonster, "Monster::checkMonsterSpawnRules"},
    {"minecraft:wolf", kPlacementOnGround, kSpawnRuleUnmodelled, "Wolf::checkWolfSpawnRules"},
    {"minecraft:zoglin", kPlacementOnGround, kSpawnRuleAnyLightMonster, "Monster::checkAnyLightMonsterSpawnRules"},
    {"minecraft:zombie", kPlacementOnGround, kSpawnRuleMonster, "Monster::checkMonsterSpawnRules"},
    {"minecraft:zombie_horse", kPlacementOnGround, kSpawnRuleMonster, "Monster::checkMonsterSpawnRules"},
    {"minecraft:zombie_villager", kPlacementOnGround, kSpawnRuleMonster, "Monster::checkMonsterSpawnRules"},
    {"minecraft:zombified_piglin", kPlacementOnGround, kSpawnRuleUnmodelled, "ZombifiedPiglin::checkZombifiedPiglinSpawnRules"},
};
inline constexpr int kSpawnPlacementCount = 83;

} // namespace mc::world

