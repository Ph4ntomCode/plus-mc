// SPDX-License-Identifier: GPL-3.0-or-later
// clang-format off
//
// The mob table (90 rows, generated — do not hand-edit).
//
// Every living entity type the 26.3 server registers except the player, the
// armour stand and the mannequin (tools/gen_mob_table.py drops those by name;
// all three are living with attributes but have no AI). Rows are sorted by name.
//
// Where each column comes from:
//
//   * `entity_type_id`, `category`, `width`, `height`, `max_health`,
//     `movement_speed`, `attack_damage`, `follow_range` and `armor` are the
//     game's own numbers, dumped by tools/gen_entity_types.py (a Java probe
//     against the server jar: BuiltInRegistries.ENTITY_TYPE, EntityType
//     .getDimensions(), DefaultAttributes.getSupplier(type));
//     `movement_speed` is the vanilla attribute — world/mob.cpp converts it with
//     `m/s = attribute^2 * 43.1718` (Minecraft Wiki, Talk:Attribute).
//   * `category` is the type's `MobCategory`, which natural spawning filters
//     and caps on (world/mob_spawn.h) and `Mob.checkDespawn` measures against
//     (world/mob_category.h).
//   * `fire_immune` is `EntityType.fireImmune()` from the same probe: the flag
//     `Entity.isInvulnerableTo` refuses `IS_FIRE` damage on (so a blaze or a
//     magma cube never burns), the flag `EntityType.isBlockDangerous` reads
//     for a burning block (so only a fire-immune type may stand in one), and
//     the flag a magma block's `isValidSpawn` reads for spawning on it.
//   * the flags are derived from the class hierarchy and the decompiled
//     sources, one rule each:
//       - kMobHostile          the chain contains `Monster`, or the class
//                              implements the `Enemy` marker (a ghast, a
//                              phantom, a slime, a shulker, a hoglin, the
//                              dragon);
//       - kMobMustSee          its player-target goal is a must-see scan
//                              (the first boolean in the NearestAttackable-
//                              TargetGoal call — true for every monster here);
//       - kMobFollowsUnseen    its melee goal tracks a target it cannot see
//                              (MeleeAttackGoal's followingTargetEvenIfNotSeen,
//                              read from the call site or the goal class's
//                              own `super(...)` — spider is the one live case);
//       - kMobInvertedHealAndHarm  membership of the
//                              `minecraft:inverted_healing_and_harm` entity-type
//                              tag (resolved from the data pack, nested tags and
//                              all), so a healing potion damages it and an
//                              instant-damage one heals it;
//       - kMobNoFarDespawn     `Animal.removeWhenFarAway` returning false, or a
//                              literal-false override in its own chain (the
//                              golems, an allay);
//       - kMobAnimal           the class chain contains `Animal` (an AgeableMob
//                              with a food tag — breeding, babies and feeding);
//       - kMobRetaliates       it registers HurryByTargetGoal (whoever hits it
//                              becomes its target, non-hostiles included).
//   * `baby_width`/`baby_height` are the nearest `BABY_DIMENSIONS` initialiser
//     (`EntityDimensions.scalable(w, h)`, or the type's own adult box
//     `.scale(f)`), and an `Animal` that declares none falls back to half its
//     adult box — stated here because it is the one approximation in a column.
//   * `xp_min`/`xp_max` are the nearest `this.xpReward = N` in the hierarchy,
//     else `Animal`'s `1 + random.nextInt(3)` or `Monster`'s 5.
//   * `follow_parent_speed` is the nearest `new FollowParentGoal(this, speed)`
//     registration (0 for a mob with no such goal).
//
// What a row cannot express, and why — all of it is "not modelled yet", none
// of it is silently wrong:
//   * equipment, DropChances and picked-up items; riders; per-mob baby
//     multipliers (a zombie's 2.5x XP) and group data (reinforcements, a
//     slime's size); `notInPeaceful`; per-type `getMaxSpawnClusterSize`;
//     the per-type `immuneTo` block tags (`#fox_immune_to`,
//     `#polar_bear_immune_to`, `#stray_immune_to`, `#wither_immune_to`, …);
//     `isSensitiveToWater` (the snow golem is the one type in this table that
//     overrides it);
//   * the mob's eye height is the `height * 0.85` default `EntityType.Builder
//     .sized` fills in, not each class's own `withEyeHeight` value;
//   * every mob-specific *ability*: the table's rows are spawned with their
//     real stats and their family's generic goal (a monster chases and hits,
//     anything else strolls), so the types whose signature behaviour is
//     something else are named here and nowhere pretend to it. The one
//     exception is the skeleton family's bow (`RangedBowAttackGoal`, in
//     mob.cpp): those five rows stand at range and loose arrows through the
//     same projectile entity a player's bow spawns.
//       - blaze                  fires small fireballs and hovers;
//       - bogged                 fires arrows (modelled: the skeleton family's bow);
//       - breeze                 leaps and fires wind charges;
//       - creeper                fuses and explodes;
//       - elder_guardian         a laser beam and mining fatigue;
//       - ender_dragon           a boss fight with phases and crystals;
//       - enderman               teleports, picks blocks up and is provoked by being looked at;
//       - endermite              burrows into blocks;
//       - evoker                 summons fangs and vexes;
//       - ghast                  flies and fires fireballs;
//       - giant                  has no AI in vanilla at all (unused mob);
//       - guardian               a laser beam;
//       - happy_ghast            flies and carries players;
//       - hoglin                 charges;
//       - illusioner             blinds and fires its bow;
//       - magma_cube             jumps and splits on death;
//       - parched                fires arrows (modelled: the skeleton family's bow);
//       - phantom                flies in swoops at night;
//       - piglin                 ranged crossbow attacks and bartering;
//       - piglin_brute           axes and no ranged attack;
//       - pillager               a crossbow;
//       - ravager                charges and roars;
//       - shulker                a bullet and levitation;
//       - silverfish             infests blocks;
//       - skeleton               fires arrows (modelled: the skeleton family's bow);
//       - slime                  jumps and splits on death;
//       - stray                  fires arrows (modelled: the skeleton family's bow);
//       - sulfur_cube            jumps and splits on death;
//       - vex                    flies through walls;
//       - warden                 a sonic boom and darkness;
//       - witch                  throws splash potions;
//       - wither                 a boss fight with phases;
//       - wither_skeleton        fires arrows (modelled: the skeleton family's bow);
//       - zoglin                 charges;
//     they exist, take damage, die and drop loot exactly as their type says,
//     and the generic goal is what is missing.
//   * entity-type tags this table's readers use (an enchantment's sensitive_to,
//     the undead membership above) resolve by name, so a mob with no row is
//     still correct in those tests — see world/entity_type_tags.h.
//
// Regenerate with tools/gen_mob_table.py (it cross-checks the six rows Phase 3
// hand-verified, byte for byte, and fails rather than move one).
#pragma once

#include <cstdint>
#include <string_view>

#include "world/biome_spawn_table.h" // MobSpawnCategory (the `category` column)

namespace mc::world {

// Mob flags bit values.
constexpr uint8_t kMobHostile = 1;      // Monster: hunts players and attacks them.
// NearestAttackableTargetGoal's `mustSee`: the goal can only *acquire* a target it
// can see (TargetingConditions.forCombat checks line of sight), and once it has
// one it forgets it after 60 unseen ticks (TargetGoal.unseenMemoryTicks).
constexpr uint8_t kMobMustSee = 2;
// MeleeAttackGoal's `followingTargetEvenIfNotSeen`: the *tick* re-plan gate. With
// it the mob keeps re-planning its path while blind; without it the mob walks the
// path it had and only re-plans when the goal restarts (which happens through the
// 20-tick `lastCanUseCheck` gate).
constexpr uint8_t kMobFollowsUnseen = 4;
// LivingEntity.isInvertedHealAndHarm, i.e. membership of the
// `minecraft:inverted_healing_and_harm` entity-type tag — so a thrown instant
// health *damages* this mob and an instant damage heals it, with the flipped
// amounts (`HealOrHarmMobEffect`).
constexpr uint8_t kMobInvertedHealAndHarm = 8;
// `Mob.removeWhenFarAway(distSqr)` is false: the mob is not discarded for being
// far from every player (every `Animal`, the golems and an allay). It gates only
// `Mob.checkDespawn`'s 128-block branch, not the slow one.
constexpr uint8_t kMobNoFarDespawn = 16;
// `Animal`: an `AgeableMob` with a food tag, a love timer and a breed goal.
constexpr uint8_t kMobAnimal = 32;
// `HurtByTargetGoal`: whoever damages this mob becomes its target (held with the
// 300-tick UnseenMemoryTicks that goal sets), *including* mobs that never hunt
// otherwise — a wolf, a polar bear, an iron golem. A non-hostile with this flag
// acquires a target only through being hurt; its nearest-target scan stays off.
constexpr uint8_t kMobRetaliates = 64;

struct GeneratedMob {
  const char* name;         // "minecraft:zombie"
  const char* display_name; // "Zombie" (the death message's attacker name)
  int32_t entity_type_id;   // minecraft:entity_type protocol id (wire spawn)
  uint8_t category;         // MobSpawnCategory (EntityType.Builder.of(.., category))
  uint8_t flags;
  bool fire_immune;         // EntityType.fireImmune() (no IS_FIRE damage)
  float width;              // EntityType.Builder.sized
  float height;
  // The `getDefaultDimensions`' baby box for an `Animal` (0 for a mob that is not
  // one): the class's `BABY_DIMENSIONS`, or half the adult box when it declares
  // none (the generator's one documented fallback).
  float baby_width;
  float baby_height;
  float max_health;         // Attributes.MAX_HEALTH
  double movement_speed;    // Attributes.MOVEMENT_SPEED (an attribute, not blocks/tick)
  float attack_damage;      // Attributes.ATTACK_DAMAGE (0 for a passive mob)
  double follow_range;      // Attributes.FOLLOW_RANGE (target acquisition)
  // `new FollowParentGoal(this, speed)`: the navigation speed modifier a baby
  // walks at while following a parent, 0 for a mob that has no such goal.
  float follow_parent_speed;
  float armor;              // Attributes.ARMOR
  int xp_min;               // XP the kill awards, rolled in [xp_min, xp_max]
  int xp_max;
};

// clang-format off
inline constexpr GeneratedMob kMobs[] = {
    {"minecraft:allay", "Allay", 2, kSpawnCreature, kMobNoFarDespawn, false, 0.35f, 0.6f, 0.0f, 0.0f, 20.0f, 0.1, 2.0f, 16.0, 0.0f, 0.0f, 0, 0},
    {"minecraft:armadillo", "Armadillo", 4, kSpawnCreature, kMobNoFarDespawn | kMobAnimal, false, 0.7f, 0.65f, 0.42f, 0.39f, 12.0f, 0.14, 0.0f, 16.0, 0.0f, 0.0f, 1, 3},
    {"minecraft:axolotl", "Axolotl", 7, kSpawnAxolotls, kMobNoFarDespawn | kMobAnimal, false, 0.75f, 0.42f, 0.375f, 0.21f, 14.0f, 1.0, 2.0f, 16.0, 0.0f, 0.0f, 1, 3},
    {"minecraft:bat", "Bat", 10, kSpawnAmbient, 0, false, 0.5f, 0.9f, 0.0f, 0.0f, 6.0f, 0.7, 0.0f, 16.0, 0.0f, 0.0f, 0, 0},
    {"minecraft:bee", "Bee", 11, kSpawnCreature, kMobFollowsUnseen | kMobNoFarDespawn | kMobAnimal, false, 0.55f, 0.5f, 0.275f, 0.25f, 10.0f, 0.3, 2.0f, 16.0, 1.25f, 0.0f, 1, 3},
    {"minecraft:blaze", "Blaze", 14, kSpawnMonster, kMobHostile | kMobMustSee | kMobRetaliates, true, 0.6f, 1.8f, 0.0f, 0.0f, 20.0f, 0.23, 6.0f, 48.0, 0.0f, 0.0f, 10, 10},
    {"minecraft:bogged", "Bogged", 16, kSpawnMonster, kMobHostile | kMobMustSee | kMobInvertedHealAndHarm | kMobRetaliates, false, 0.6f, 1.99f, 0.0f, 0.0f, 16.0f, 0.25, 2.0f, 16.0, 0.0f, 0.0f, 5, 5},
    {"minecraft:breeze", "Breeze", 17, kSpawnMonster, kMobHostile | kMobMustSee, false, 0.6f, 1.77f, 0.0f, 0.0f, 30.0f, 0.63, 3.0f, 24.0, 0.0f, 0.0f, 10, 10},
    {"minecraft:camel", "Camel", 19, kSpawnCreature, kMobNoFarDespawn | kMobAnimal, false, 1.7f, 2.375f, 0.85f, 1.1875f, 32.0f, 0.09, 0.0f, 16.0, 1.0f, 0.0f, 1, 3},
    {"minecraft:camel_husk", "Camel Husk", 20, kSpawnMonster, kMobInvertedHealAndHarm | kMobNoFarDespawn | kMobAnimal, false, 1.7f, 2.375f, 0.85f, 1.1875f, 32.0f, 0.09, 0.0f, 16.0, 1.0f, 0.0f, 1, 3},
    {"minecraft:cat", "Cat", 21, kSpawnCreature, kMobNoFarDespawn | kMobAnimal, false, 0.6f, 0.7f, 0.3f, 0.35f, 10.0f, 0.3, 3.0f, 16.0, 0.0f, 0.0f, 1, 3},
    {"minecraft:cave_spider", "Cave Spider", 22, kSpawnMonster, kMobHostile | kMobMustSee | kMobFollowsUnseen | kMobRetaliates, false, 0.7f, 0.5f, 0.0f, 0.0f, 12.0f, 0.3, 2.0f, 16.0, 0.0f, 0.0f, 5, 5},
    {"minecraft:chicken", "Chicken", 26, kSpawnCreature, kMobNoFarDespawn | kMobAnimal, false, 0.4f, 0.7f, 0.3f, 0.4f, 4.0f, 0.25, 0.0f, 16.0, 1.1f, 0.0f, 1, 3},
    {"minecraft:cod", "Cod", 27, kSpawnWaterAmbient, 0, false, 0.5f, 0.3f, 0.0f, 0.0f, 3.0f, 0.7, 0.0f, 16.0, 0.0f, 0.0f, 1, 3},
    {"minecraft:copper_golem", "Copper Golem", 28, kSpawnMisc, kMobNoFarDespawn, false, 0.49f, 0.98f, 0.0f, 0.0f, 12.0f, 0.2, 0.0f, 16.0, 0.0f, 0.0f, 0, 0},
    {"minecraft:cow", "Cow", 30, kSpawnCreature, kMobNoFarDespawn | kMobAnimal, false, 0.9f, 1.4f, 0.45f, 0.7f, 10.0f, 0.2, 0.0f, 16.0, 1.25f, 0.0f, 1, 3},
    {"minecraft:creaking", "Creaking", 31, kSpawnMonster, kMobHostile | kMobMustSee, false, 0.9f, 2.7f, 0.0f, 0.0f, 1.0f, 0.4, 3.0f, 32.0, 0.0f, 0.0f, 0, 0},
    {"minecraft:creeper", "Creeper", 32, kSpawnMonster, kMobHostile | kMobMustSee | kMobRetaliates, false, 0.6f, 1.7f, 0.0f, 0.0f, 20.0f, 0.25, 2.0f, 16.0, 0.0f, 0.0f, 5, 5},
    {"minecraft:dolphin", "Dolphin", 36, kSpawnWaterCreature, kMobFollowsUnseen | kMobRetaliates, false, 0.9f, 0.6f, 0.0f, 0.0f, 10.0f, 1.2, 3.0f, 16.0, 0.0f, 0.0f, 1, 3},
    {"minecraft:donkey", "Donkey", 37, kSpawnCreature, kMobNoFarDespawn | kMobAnimal, false, 1.39648f, 1.5f, 0.698242f, 0.75f, 53.0f, 0.175, 0.0f, 16.0, 1.0f, 0.0f, 1, 3},
    {"minecraft:drowned", "Drowned", 39, kSpawnMonster, kMobHostile | kMobMustSee | kMobInvertedHealAndHarm | kMobRetaliates, false, 0.6f, 1.95f, 0.0f, 0.0f, 20.0f, 0.23, 3.0f, 35.0, 0.0f, 2.0f, 5, 5},
    {"minecraft:elder_guardian", "Elder Guardian", 41, kSpawnMonster, kMobHostile | kMobMustSee, false, 1.9975f, 1.9975f, 0.0f, 0.0f, 80.0f, 0.3, 8.0f, 16.0, 0.0f, 0.0f, 10, 10},
    {"minecraft:ender_dragon", "Ender Dragon", 44, kSpawnMonster, kMobHostile | kMobMustSee, true, 16.0f, 8.0f, 0.0f, 0.0f, 200.0f, 0.7, 0.0f, 16.0, 0.0f, 0.0f, 0, 0},
    {"minecraft:enderman", "Enderman", 42, kSpawnMonster, kMobHostile | kMobMustSee | kMobRetaliates, false, 0.6f, 2.9f, 0.0f, 0.0f, 40.0f, 0.3, 7.0f, 64.0, 0.0f, 0.0f, 5, 5},
    {"minecraft:endermite", "Endermite", 43, kSpawnMonster, kMobHostile | kMobMustSee | kMobRetaliates, false, 0.4f, 0.3f, 0.0f, 0.0f, 8.0f, 0.25, 2.0f, 16.0, 0.0f, 0.0f, 3, 3},
    {"minecraft:evoker", "Evoker", 47, kSpawnMonster, kMobHostile | kMobMustSee | kMobRetaliates, false, 0.6f, 1.95f, 0.0f, 0.0f, 24.0f, 0.5, 2.0f, 12.0, 0.0f, 0.0f, 10, 10},
    {"minecraft:fox", "Fox", 55, kSpawnCreature, kMobFollowsUnseen | kMobNoFarDespawn | kMobAnimal, false, 0.6f, 0.7f, 0.36f, 0.42f, 10.0f, 0.3, 2.0f, 32.0, 0.0f, 0.0f, 1, 3},
    {"minecraft:frog", "Frog", 56, kSpawnCreature, kMobNoFarDespawn | kMobAnimal, false, 0.5f, 0.5f, 0.25f, 0.25f, 10.0f, 1.0, 10.0f, 16.0, 0.0f, 0.0f, 1, 3},
    {"minecraft:ghast", "Ghast", 58, kSpawnMonster, kMobHostile | kMobMustSee, true, 4.0f, 4.0f, 0.0f, 0.0f, 10.0f, 0.7, 0.0f, 100.0, 0.0f, 0.0f, 5, 5},
    {"minecraft:giant", "Giant", 60, kSpawnMonster, kMobHostile | kMobMustSee, false, 3.6f, 12.0f, 0.0f, 0.0f, 100.0f, 0.5, 50.0f, 16.0, 0.0f, 0.0f, 5, 5},
    {"minecraft:glow_squid", "Glow Squid", 62, kSpawnUndergroundWaterCreature, 0, false, 0.8f, 0.8f, 0.0f, 0.0f, 10.0f, 0.7, 0.0f, 16.0, 0.0f, 0.0f, 1, 3},
    {"minecraft:goat", "Goat", 63, kSpawnCreature, kMobNoFarDespawn | kMobAnimal, false, 0.9f, 1.3f, 0.45f, 0.65f, 10.0f, 0.2, 2.0f, 16.0, 0.0f, 0.0f, 1, 3},
    {"minecraft:guardian", "Guardian", 64, kSpawnMonster, kMobHostile | kMobMustSee, false, 0.85f, 0.85f, 0.0f, 0.0f, 30.0f, 0.5, 6.0f, 16.0, 0.0f, 0.0f, 10, 10},
    {"minecraft:happy_ghast", "Happy Ghast", 59, kSpawnCreature, kMobNoFarDespawn | kMobAnimal, false, 4.0f, 4.0f, 0.95f, 0.95f, 20.0f, 0.05, 0.0f, 16.0, 0.0f, 0.0f, 1, 3},
    {"minecraft:hoglin", "Hoglin", 65, kSpawnMonster, kMobHostile | kMobMustSee | kMobNoFarDespawn | kMobAnimal, false, 1.39648f, 1.4f, 0.75f, 0.85f, 40.0f, 0.3, 6.0f, 16.0, 0.0f, 0.0f, 5, 5},
    {"minecraft:horse", "Horse", 67, kSpawnCreature, kMobNoFarDespawn | kMobAnimal, false, 1.39648f, 1.6f, 0.977539f, 1.12f, 53.0f, 0.225, 0.0f, 16.0, 1.0f, 0.0f, 1, 3},
    {"minecraft:husk", "Husk", 68, kSpawnMonster, kMobHostile | kMobMustSee | kMobInvertedHealAndHarm | kMobRetaliates, false, 0.6f, 1.95f, 0.0f, 0.0f, 20.0f, 0.23, 3.0f, 35.0, 0.0f, 2.0f, 5, 5},
    {"minecraft:illusioner", "Illusioner", 69, kSpawnMonster, kMobHostile | kMobMustSee | kMobRetaliates, false, 0.6f, 1.95f, 0.0f, 0.0f, 32.0f, 0.5, 2.0f, 18.0, 0.0f, 0.0f, 5, 5},
    {"minecraft:iron_golem", "Iron Golem", 71, kSpawnMisc, kMobFollowsUnseen | kMobNoFarDespawn | kMobRetaliates, false, 1.4f, 2.7f, 0.0f, 0.0f, 100.0f, 0.25, 15.0f, 16.0, 0.0f, 0.0f, 0, 0},
    {"minecraft:llama", "Llama", 79, kSpawnCreature, kMobNoFarDespawn | kMobAnimal, false, 0.9f, 1.87f, 0.45f, 0.935f, 53.0f, 0.175, 0.0f, 16.0, 1.0f, 0.0f, 1, 3},
    {"minecraft:magma_cube", "Magma Cube", 81, kSpawnMonster, kMobHostile | kMobMustSee, true, 0.52f, 0.52f, 0.0f, 0.0f, 20.0f, 0.2, 2.0f, 16.0, 0.0f, 0.0f, 1, 4},
    {"minecraft:mooshroom", "Mooshroom", 87, kSpawnCreature, kMobNoFarDespawn | kMobAnimal, false, 0.9f, 1.4f, 0.45f, 0.7f, 10.0f, 0.2, 0.0f, 16.0, 1.25f, 0.0f, 1, 3},
    {"minecraft:mule", "Mule", 88, kSpawnCreature, kMobNoFarDespawn | kMobAnimal, false, 1.39648f, 1.6f, 0.698242f, 0.8f, 53.0f, 0.175, 0.0f, 16.0, 1.0f, 0.0f, 1, 3},
    {"minecraft:nautilus", "Nautilus", 89, kSpawnWaterCreature, kMobNoFarDespawn | kMobAnimal, false, 0.875f, 0.95f, 0.4375f, 0.475f, 15.0f, 1.0, 3.0f, 16.0, 0.0f, 0.0f, 1, 3},
    {"minecraft:ocelot", "Ocelot", 92, kSpawnCreature, kMobNoFarDespawn | kMobAnimal, false, 0.6f, 0.7f, 0.3f, 0.35f, 10.0f, 0.3, 3.0f, 16.0, 0.0f, 0.0f, 1, 3},
    {"minecraft:panda", "Panda", 97, kSpawnCreature, kMobFollowsUnseen | kMobNoFarDespawn | kMobAnimal, false, 1.3f, 1.25f, 0.65f, 0.625f, 20.0f, 0.15, 6.0f, 16.0, 1.25f, 0.0f, 1, 3},
    {"minecraft:parched", "Parched", 98, kSpawnMonster, kMobHostile | kMobMustSee | kMobInvertedHealAndHarm | kMobRetaliates, false, 0.6f, 1.99f, 0.0f, 0.0f, 16.0f, 0.25, 2.0f, 16.0, 0.0f, 0.0f, 5, 5},
    {"minecraft:parrot", "Parrot", 99, kSpawnCreature, kMobNoFarDespawn | kMobAnimal, false, 0.5f, 0.9f, 0.25f, 0.45f, 6.0f, 0.2, 3.0f, 16.0, 0.0f, 0.0f, 1, 3},
    {"minecraft:phantom", "Phantom", 100, kSpawnMonster, kMobHostile | kMobMustSee | kMobInvertedHealAndHarm, false, 0.9f, 0.5f, 0.0f, 0.0f, 20.0f, 0.7, 2.0f, 16.0, 0.0f, 0.0f, 5, 5},
    {"minecraft:pig", "Pig", 101, kSpawnCreature, kMobNoFarDespawn | kMobAnimal, false, 0.9f, 0.9f, 0.45f, 0.45f, 10.0f, 0.25, 0.0f, 16.0, 1.1f, 0.0f, 1, 3},
    {"minecraft:piglin", "Piglin", 102, kSpawnMonster, kMobHostile | kMobMustSee, false, 0.6f, 1.95f, 0.0f, 0.0f, 16.0f, 0.35, 5.0f, 16.0, 0.0f, 0.0f, 5, 5},
    {"minecraft:piglin_brute", "Piglin Brute", 103, kSpawnMonster, kMobHostile | kMobMustSee, false, 0.6f, 1.95f, 0.0f, 0.0f, 50.0f, 0.35, 7.0f, 12.0, 0.0f, 0.0f, 20, 20},
    {"minecraft:pillager", "Pillager", 104, kSpawnMonster, kMobHostile | kMobMustSee | kMobRetaliates, false, 0.6f, 1.95f, 0.0f, 0.0f, 24.0f, 0.35, 5.0f, 32.0, 0.0f, 0.0f, 5, 5},
    {"minecraft:polar_bear", "Polar Bear", 105, kSpawnCreature, kMobFollowsUnseen | kMobNoFarDespawn | kMobAnimal, false, 1.4f, 1.4f, 0.7f, 0.7f, 30.0f, 0.25, 6.0f, 20.0, 1.25f, 0.0f, 1, 3},
    {"minecraft:pufferfish", "Pufferfish", 110, kSpawnWaterAmbient, 0, false, 0.7f, 0.7f, 0.0f, 0.0f, 3.0f, 0.7, 0.0f, 16.0, 0.0f, 0.0f, 1, 3},
    {"minecraft:rabbit", "Rabbit", 111, kSpawnCreature, kMobFollowsUnseen | kMobNoFarDespawn | kMobAnimal | kMobRetaliates, false, 0.49f, 0.6f, 0.24f, 0.4f, 3.0f, 0.3, 3.0f, 16.0, 0.0f, 0.0f, 1, 3},
    {"minecraft:ravager", "Ravager", 112, kSpawnMonster, kMobHostile | kMobMustSee | kMobFollowsUnseen | kMobRetaliates, false, 1.95f, 2.2f, 0.0f, 0.0f, 100.0f, 0.3, 12.0f, 32.0, 0.0f, 0.0f, 20, 20},
    {"minecraft:salmon", "Salmon", 113, kSpawnWaterAmbient, 0, false, 0.7f, 0.4f, 0.0f, 0.0f, 3.0f, 0.7, 0.0f, 16.0, 0.0f, 0.0f, 1, 3},
    {"minecraft:sheep", "Sheep", 114, kSpawnCreature, kMobNoFarDespawn | kMobAnimal, false, 0.9f, 1.3f, 0.45f, 0.65f, 8.0f, 0.23, 0.0f, 16.0, 1.1f, 0.0f, 1, 3},
    {"minecraft:shulker", "Shulker", 115, kSpawnMonster, kMobHostile | kMobMustSee | kMobFollowsUnseen | kMobNoFarDespawn | kMobRetaliates, true, 1.0f, 1.0f, 0.0f, 0.0f, 30.0f, 0.7, 0.0f, 16.0, 0.0f, 0.0f, 5, 5},
    {"minecraft:silverfish", "Silverfish", 117, kSpawnMonster, kMobHostile | kMobMustSee | kMobRetaliates, false, 0.4f, 0.3f, 0.0f, 0.0f, 8.0f, 0.25, 1.0f, 16.0, 0.0f, 0.0f, 5, 5},
    {"minecraft:skeleton", "Skeleton", 118, kSpawnMonster, kMobHostile | kMobMustSee | kMobInvertedHealAndHarm | kMobRetaliates, false, 0.6f, 1.99f, 0.0f, 0.0f, 20.0f, 0.25, 2.0f, 16.0, 0.0f, 0.0f, 5, 5},
    {"minecraft:skeleton_horse", "Skeleton Horse", 119, kSpawnCreature, kMobInvertedHealAndHarm | kMobNoFarDespawn | kMobAnimal, false, 1.39648f, 1.6f, 0.977539f, 1.12f, 15.0f, 0.2, 0.0f, 16.0, 1.0f, 0.0f, 1, 3},
    {"minecraft:slime", "Slime", 120, kSpawnMonster, kMobHostile | kMobMustSee, false, 0.52f, 0.52f, 0.0f, 0.0f, 20.0f, 0.7, 2.0f, 16.0, 0.0f, 0.0f, 1, 4},
    {"minecraft:sniffer", "Sniffer", 122, kSpawnCreature, kMobNoFarDespawn | kMobAnimal, false, 1.9f, 1.75f, 0.95f, 0.875f, 14.0f, 0.1, 0.0f, 16.0, 0.0f, 0.0f, 1, 3},
    {"minecraft:snow_golem", "Snow Golem", 124, kSpawnMisc, kMobNoFarDespawn, false, 0.7f, 1.9f, 0.0f, 0.0f, 4.0f, 0.2, 0.0f, 16.0, 0.0f, 0.0f, 0, 0},
    {"minecraft:spider", "Spider", 127, kSpawnMonster, kMobHostile | kMobMustSee | kMobFollowsUnseen | kMobRetaliates, false, 1.4f, 0.9f, 0.0f, 0.0f, 16.0f, 0.3, 2.0f, 16.0, 0.0f, 0.0f, 5, 5},
    {"minecraft:squid", "Squid", 130, kSpawnWaterCreature, 0, false, 0.8f, 0.8f, 0.0f, 0.0f, 10.0f, 0.7, 0.0f, 16.0, 0.0f, 0.0f, 1, 3},
    {"minecraft:stray", "Stray", 131, kSpawnMonster, kMobHostile | kMobMustSee | kMobInvertedHealAndHarm | kMobRetaliates, false, 0.6f, 1.99f, 0.0f, 0.0f, 20.0f, 0.25, 2.0f, 16.0, 0.0f, 0.0f, 5, 5},
    {"minecraft:strider", "Strider", 132, kSpawnCreature, kMobNoFarDespawn | kMobAnimal, true, 0.9f, 1.7f, 0.45f, 0.85f, 20.0f, 0.175, 0.0f, 16.0, 1.0f, 0.0f, 1, 3},
    {"minecraft:sulfur_cube", "Sulfur Cube", 133, kSpawnMonster, 0, false, 0.49f, 0.49f, 0.0f, 0.0f, 20.0f, 0.7, 0.0f, 16.0, 0.0f, 0.0f, 1, 4},
    {"minecraft:tadpole", "Tadpole", 134, kSpawnCreature, 0, false, 0.4f, 0.3f, 0.0f, 0.0f, 6.0f, 1.0, 0.0f, 16.0, 0.0f, 0.0f, 1, 3},
    {"minecraft:trader_llama", "Trader Llama", 138, kSpawnCreature, kMobNoFarDespawn | kMobAnimal, false, 0.9f, 1.87f, 0.45f, 0.935f, 53.0f, 0.175, 0.0f, 16.0, 1.0f, 0.0f, 1, 3},
    {"minecraft:tropical_fish", "Tropical Fish", 140, kSpawnWaterAmbient, 0, false, 0.5f, 0.4f, 0.0f, 0.0f, 3.0f, 0.7, 0.0f, 16.0, 0.0f, 0.0f, 1, 3},
    {"minecraft:turtle", "Turtle", 141, kSpawnCreature, kMobNoFarDespawn | kMobAnimal, false, 1.2f, 0.4f, 0.36f, 0.12f, 30.0f, 0.25, 0.0f, 16.0, 0.0f, 0.0f, 1, 3},
    {"minecraft:vex", "Vex", 142, kSpawnMonster, kMobHostile | kMobMustSee | kMobRetaliates, true, 0.4f, 0.8f, 0.0f, 0.0f, 14.0f, 0.7, 4.0f, 16.0, 0.0f, 0.0f, 3, 3},
    {"minecraft:villager", "Villager", 143, kSpawnMisc, kMobNoFarDespawn, false, 0.6f, 1.95f, 0.0f, 0.0f, 20.0f, 0.5, 0.0f, 16.0, 0.0f, 0.0f, 0, 0},
    {"minecraft:vindicator", "Vindicator", 144, kSpawnMonster, kMobHostile | kMobMustSee | kMobRetaliates, false, 0.6f, 1.95f, 0.0f, 0.0f, 24.0f, 0.35, 5.0f, 12.0, 0.0f, 0.0f, 5, 5},
    {"minecraft:wandering_trader", "Wandering Trader", 145, kSpawnCreature, kMobNoFarDespawn, false, 0.6f, 1.95f, 0.0f, 0.0f, 20.0f, 0.7, 0.0f, 16.0, 0.0f, 0.0f, 0, 0},
    {"minecraft:warden", "Warden", 146, kSpawnMonster, kMobHostile | kMobMustSee | kMobNoFarDespawn, true, 0.9f, 2.9f, 0.0f, 0.0f, 500.0f, 0.3, 30.0f, 24.0, 0.0f, 0.0f, 5, 5},
    {"minecraft:witch", "Witch", 148, kSpawnMonster, kMobHostile | kMobMustSee | kMobRetaliates, false, 0.6f, 1.95f, 0.0f, 0.0f, 26.0f, 0.25, 2.0f, 16.0, 0.0f, 0.0f, 5, 5},
    {"minecraft:wither", "Wither", 149, kSpawnMonster, kMobHostile | kMobInvertedHealAndHarm | kMobRetaliates, true, 0.9f, 3.5f, 0.0f, 0.0f, 300.0f, 0.6, 2.0f, 40.0, 0.0f, 4.0f, 50, 50},
    {"minecraft:wither_skeleton", "Wither Skeleton", 150, kSpawnMonster, kMobHostile | kMobMustSee | kMobInvertedHealAndHarm | kMobRetaliates, true, 0.7f, 2.4f, 0.0f, 0.0f, 20.0f, 0.25, 2.0f, 16.0, 0.0f, 0.0f, 5, 5},
    {"minecraft:wolf", "Wolf", 152, kSpawnCreature, kMobFollowsUnseen | kMobNoFarDespawn | kMobAnimal | kMobRetaliates, false, 0.6f, 0.85f, 0.3f, 0.425f, 8.0f, 0.3, 4.0f, 16.0, 0.0f, 0.0f, 1, 3},
    {"minecraft:zoglin", "Zoglin", 153, kSpawnMonster, kMobHostile | kMobMustSee | kMobInvertedHealAndHarm, true, 1.39648f, 1.4f, 0.0f, 0.0f, 40.0f, 0.3, 6.0f, 16.0, 0.0f, 0.0f, 5, 5},
    {"minecraft:zombie", "Zombie", 154, kSpawnMonster, kMobHostile | kMobMustSee | kMobInvertedHealAndHarm | kMobRetaliates, false, 0.6f, 1.95f, 0.0f, 0.0f, 20.0f, 0.23, 3.0f, 35.0, 0.0f, 2.0f, 5, 5},
    {"minecraft:zombie_horse", "Zombie Horse", 155, kSpawnMonster, kMobInvertedHealAndHarm | kMobNoFarDespawn | kMobAnimal, false, 1.39648f, 1.6f, 0.977539f, 1.12f, 25.0f, 0.225, 0.0f, 16.0, 1.0f, 0.0f, 1, 3},
    {"minecraft:zombie_nautilus", "Zombie Nautilus", 156, kSpawnMonster, kMobInvertedHealAndHarm | kMobNoFarDespawn | kMobAnimal, false, 0.875f, 0.95f, 0.4375f, 0.475f, 15.0f, 1.1, 3.0f, 16.0, 0.0f, 0.0f, 1, 3},
    {"minecraft:zombie_villager", "Zombie Villager", 157, kSpawnMonster, kMobHostile | kMobMustSee | kMobInvertedHealAndHarm | kMobRetaliates, false, 0.6f, 1.95f, 0.0f, 0.0f, 20.0f, 0.23, 3.0f, 35.0, 0.0f, 2.0f, 5, 5},
    {"minecraft:zombified_piglin", "Zombified Piglin", 158, kSpawnMonster, kMobHostile | kMobMustSee | kMobInvertedHealAndHarm | kMobRetaliates, true, 0.6f, 1.95f, 0.0f, 0.0f, 20.0f, 0.23, 5.0f, 35.0, 0.0f, 2.0f, 5, 5},
};
// clang-format on
inline constexpr int kMobTypeCount = 90;

// The mob a name refers to ("zombie" / "minecraft:zombie" / "ZOMBIE"), or
// nullptr. The name is normalized first.
const GeneratedMob* mob_by_name(std::string_view name);

// The mob an entity id in an Add Entity packet refers to, or nullptr.
const GeneratedMob* mob_by_entity_type_id(int32_t entity_type_id);

} // namespace mc::world
