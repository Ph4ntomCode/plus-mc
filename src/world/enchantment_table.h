// SPDX-License-Identifier: GPL-3.0-or-later
//
// The 43 26.3 minecraft:enchantment entries, in wire order
// (generated — do not hand-edit).
//
// minecraft:enchantment is a *datapack* registry, so the id is the index
// the captured wire order (clientconfig/registries/enchantment.txt)
// assigns; that index is what the `minecraft:enchantments` item component
// carries on the wire.
//
// `effects` is a bitmask over minecraft:enchantment_effect_component_type,
// keyed by that registry's own protocol id (reports/registries.json), so
// the bits line up with the component names rather than with a local
// ordering. Five of them are modelled, as value effects with a linear
// level term:
//   kEffectComponentDamageProtection (bit 0): the
//     minecraft:damage_protection entries this header carries;
//   minecraft:damage and minecraft:knockback: the `damage_add` /
//     `knockback_add` fields, the melee hooks Sharpness, Smite, Bane of
//     Arthropods and Knockback are (world/combat.cpp applies them);
//   minecraft:smash_damage_per_fallen_block: the `smash_damage_add`
//     field, Density's damage per block fallen, which world/combat.cpp
//     multiplies by the fall distance of a mace smash;
//   minecraft:attributes: only `sweeping_damage_ratio`, as the
//     `sweeping_num`/`sweeping_den` fraction Sweeping Edge scales the
//     sweep with;
//   minecraft:post_attack: the entries that land on the *victim* — Fire
//     Aspect's ignite and Bane of Arthropods' Slowness IV — as the
//     `post_attack` list (world/enchantment.cpp resolves them for the
//     level the attacker's weapon carries).
// The rest (minecraft:projectile_piercing, a post-attack entry that
// affects the attacker or needs an entity or an explosion, ...) are
// recorded so the bits can be *tested* — see docs/PHASE3.md for what that
// means for each enchantment.
//
// A `minecraft:damage_protection` entry is a `minecraft:linear` value
// (base + per_level_above_first * (level - 1)) gated by a
// `damage_source_properties` damage-type tag predicate. required_tags must
// all be set on the damage type and excluded_tags must all be clear, which
// is how Protection covers everything while Blast Protection covers only
// minecraft:is_explosion.
//
// Regenerate with tools/gen_enchantment_table.py.
#pragma once

#include <cstdint>

#include "world/damage_table.h"

namespace mc::world {

// EquipmentSlotGroup, as a bit per equipment slot. The mask exists only to
// answer Enchantment.matchingSlot / EquipmentSlotGroup.test: which slots an
// enchantment's effects apply in.
constexpr uint8_t kEnchantSlotMainhand = 1 << 0;
constexpr uint8_t kEnchantSlotOffhand = 1 << 1;
constexpr uint8_t kEnchantSlotFeet = 1 << 2;
constexpr uint8_t kEnchantSlotLegs = 1 << 3;
constexpr uint8_t kEnchantSlotChest = 1 << 4;
constexpr uint8_t kEnchantSlotHead = 1 << 5;
constexpr uint8_t kEnchantSlotBody = 1 << 6;
constexpr uint8_t kEnchantSlotSaddle = 1 << 7;
constexpr uint8_t kEnchantSlotHand = kEnchantSlotMainhand | kEnchantSlotOffhand;
constexpr uint8_t kEnchantSlotArmor =
    kEnchantSlotFeet | kEnchantSlotLegs | kEnchantSlotChest | kEnchantSlotHead;
constexpr uint8_t kEnchantSlotAny = 0xFF;

// The enchantment effect components this server models: the four value
// effects with a linear level term (see above).
constexpr int kEffectComponentDamageProtection = 0;
constexpr int kEffectComponentDamage = 2;
constexpr int kEffectComponentKnockback = 4;
constexpr int kEffectComponentSmashDamagePerFallenBlock = 3;
constexpr int kEffectComponentAttributes = 24;
constexpr int kEffectComponentPostAttack = 6;

// The two minecraft:post_attack effect types this server lands on the
// victim. Entries that summon an entity or an explosion (Channeling's
// summon, Wind Burst) are carried only as the `effects` bit above; the
// generator names every such entry as it writes this header, so none of
// them is silently inert. Thorns is neither: it is the one entry aimed
// at the *attacker* from the victim's own equipment, and it is the
// `thorns` list below rather than a bit with no home.
constexpr uint8_t kPostAttackIgnite = 1;         // minecraft:ignite
constexpr uint8_t kPostAttackApplyMobEffect = 2; // minecraft:apply_mob_effect

struct GeneratedProtection {
  float base;                  // LevelBasedValue.linear base
  float per_level_above_first; // ...and its per_level_above_first
  uint64_t required_tags;      // every kDamageTag* bit must be set
  uint64_t excluded_tags;      // ...and every one here must be clear
};

// Which entity a minecraft:damage requirement tests its entity-type tag
// against: Enchantment.damageContext's loot context carries both, and the
// vanilla data uses both (Smite names `this`, Power `direct_attacker`).
constexpr uint8_t kDamageTagEntityNone = 0;           // no requirement
constexpr uint8_t kDamageTagEntityVictim = 1;         // entity: this
constexpr uint8_t kDamageTagEntityDirectAttacker = 2; // entity: direct_attacker

// A `minecraft:linear` level-based value: base + per_level_above_first x
// (level - 1).
struct GeneratedLinear {
  float base;
  float per_level_above_first;
};

// One minecraft:post_attack entry, as Enchantment.applyEffects would
// compute it for a level: `kind` says which effect, the duration pair is
// in *seconds* (the two are equal for ignite, which has one duration and
// no roll) and the amplifier pair in levels, and `tag`/`tag_entity` are
// the entity-properties requirement the entry is gated on
// (kDamageTagEntityNone is unconditional).
struct GeneratedPostAttack {
  uint8_t kind;                  // kPostAttack*
  const char* effect;            // apply_mob_effect's to_apply; nullptr
                                 //   for ignite
  GeneratedLinear duration_min;  // seconds, linear in the level
  GeneratedLinear duration_max;  // ...the pair vanilla rolls between
  GeneratedLinear amplifier_min; // levels, likewise
  GeneratedLinear amplifier_max;
  const char* tag;    // the entity-type tag it is gated on
  uint8_t tag_entity; // ...and which entity tests it
};

// One minecraft:post_attack entry the *victim's* equipment lands on its
// attacker (Thorns). `Enchantment.doPostAttack` resolves `affected:
// attacker` to `damageSource.getEntity()` and gives the effect that
// entity; DamageEntity then rolls `Mth.randomBetween(min, max)` and hurts
// it with a source whose *causing* entity is the enchanted item's owner —
// the victim — and ChangeItemDamage takes `item_damage` off the enchanted
// piece. `chance` is the random_chance requirement the entry is gated on
// before any of that runs. Every value is linear in the level.
struct GeneratedThorns {
  const char* damage_type;    // minecraft:thorns
  GeneratedLinear min_damage; // Mth.randomBetween's bounds, by level
  GeneratedLinear max_damage;
  GeneratedLinear item_damage; // change_item_damage, in durability
  GeneratedLinear chance;      // random_chance, 0.15 x level
};

struct GeneratedEnchantment {
  const char* name;  // minecraft:protection
  uint8_t max_level; // Enchantment.getMaxLevel
  uint8_t slots;     // kEnchantSlot* mask
  uint32_t effects;  // bit per effect component protocol id
  const GeneratedProtection* protections;
  int protection_count;
  const uint32_t* items; // supported_items, as item registry ids
  int item_count;
  const uint32_t* exclusive_set; // exclusive_set, as enchantment ids
  int exclusive_count;
  // The modelled melee value effects; all-zero when the enchantment does
  // not carry the component (see `effects`).
  GeneratedLinear damage_add;    // minecraft:damage
  const char* damage_tag;        // ...gated on an entity's type being in this
                                 // tag; nullptr is unconditional
  uint8_t damage_tag_entity;     // ...and which entity the tag is tested
                                 // against (kDamageTagEntity*)
  GeneratedLinear knockback_add; // minecraft:knockback
  const char* knockback_tag;     // ...gated the same way as damage_add
  uint8_t knockback_tag_entity;
  // minecraft:smash_damage_per_fallen_block (Density): the damage per
  // block fallen, which the mace's smash multiplies by the fall distance.
  GeneratedLinear smash_damage_add;
  const char* smash_damage_tag; // ...gated like damage_add
  uint8_t smash_damage_tag_entity;
  GeneratedLinear sweeping_num; // minecraft:attributes on
  GeneratedLinear sweeping_den; //   sweeping_damage_ratio, as a fraction
  // minecraft:post_attack's victim-facing entries, in data order; all-zero
  // (with a null list) for every other enchantment.
  const GeneratedPostAttack* post_attack;
  int post_attack_count;
  // ...and the attacker-facing ones, which only the *victim's* equipment
  // carries (Thorns); null for every other enchantment.
  const GeneratedThorns* thorns;
  int thorns_count;
};

// clang-format off
inline constexpr uint32_t kAquaAffinityItems[] = {
    1001, 1068, 1072, 1076, 1080, 1084, 1088, 1092,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kBaneOfArthropodsItems[] = {
    1025, 1028, 1030, 1033, 1035, 1038, 1040, 1043, 1045, 1048, 1050, 1053,
    1055, 1058, 1374, 1447, 1448, 1449, 1450, 1451, 1452, 1453,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kBaneOfArthropodsExclusive[] = {1, 4, 6, 15, 33, 35,};
// clang-format on
// clang-format off
inline constexpr GeneratedPostAttack kBaneOfArthropodsPostAttack[] = {
    {kPostAttackApplyMobEffect, "minecraft:slowness", {1.5f, 0.0f}, {1.5f, 0.5f}, {3.0f, 0.0f}, {3.0f, 0.0f}, "minecraft:sensitive_to_bane_of_arthropods", kDamageTagEntityVictim},
};
// clang-format on
// clang-format off
inline constexpr uint32_t kBindingCurseItems[] = {
    431, 974, 1001, 1068, 1069, 1070, 1071, 1072, 1073, 1074, 1075, 1076,
    1077, 1078, 1079, 1080, 1081, 1082, 1083, 1084, 1085, 1086, 1087, 1088,
    1089, 1090, 1091, 1092, 1093, 1094, 1095, 1384, 1385, 1386, 1387, 1388,
    1389, 1390,
};
// clang-format on
// clang-format off
inline constexpr GeneratedProtection kBlastProtectionProtections[] = {
    {2.0f, 2.0f, 0x0000000000080000ull, 0x0000000000000800ull},
};
// clang-format on
// clang-format off
inline constexpr uint32_t kBlastProtectionItems[] = {
    1001, 1068, 1069, 1070, 1071, 1072, 1073, 1074, 1075, 1076, 1077, 1078,
    1079, 1080, 1081, 1082, 1083, 1084, 1085, 1086, 1087, 1088, 1089, 1090,
    1091, 1092, 1093, 1094, 1095,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kBlastProtectionExclusive[] = {3, 11, 27, 28,};
// clang-format on
// clang-format off
inline constexpr uint32_t kBreachItems[] = {
    1374,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kBreachExclusive[] = {1, 4, 6, 15, 33, 35,};
// clang-format on
// clang-format off
inline constexpr uint32_t kChannelingItems[] = {
    1483,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kDensityItems[] = {
    1374,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kDensityExclusive[] = {1, 4, 6, 15, 33, 35,};
// clang-format on
// clang-format off
inline constexpr uint32_t kDepthStriderItems[] = {
    1071, 1075, 1079, 1083, 1087, 1091, 1095,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kDepthStriderExclusive[] = {7, 14,};
// clang-format on
// clang-format off
inline constexpr uint32_t kEfficiencyItems[] = {
    1026, 1027, 1028, 1029, 1031, 1032, 1033, 1034, 1036, 1037, 1038, 1039,
    1041, 1042, 1043, 1044, 1046, 1047, 1048, 1049, 1051, 1052, 1053, 1054,
    1056, 1057, 1058, 1059, 1255,
};
// clang-format on
// clang-format off
inline constexpr GeneratedProtection kFeatherFallingProtections[] = {
    {3.0f, 3.0f, 0x0000000000100000ull, 0x0000000000000800ull},
};
// clang-format on
// clang-format off
inline constexpr uint32_t kFeatherFallingItems[] = {
    1071, 1075, 1079, 1083, 1087, 1091, 1095,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kFireAspectItems[] = {
    1025, 1030, 1035, 1040, 1045, 1050, 1055, 1374, 1447, 1448, 1449, 1450,
    1451, 1452, 1453,
};
// clang-format on
// clang-format off
inline constexpr GeneratedPostAttack kFireAspectPostAttack[] = {
    {kPostAttackIgnite, nullptr, {4.0f, 4.0f}, {4.0f, 4.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone},
};
// clang-format on
// clang-format off
inline constexpr GeneratedProtection kFireProtectionProtections[] = {
    {2.0f, 2.0f, 0x0000000000200000ull, 0x0000000000000800ull},
};
// clang-format on
// clang-format off
inline constexpr uint32_t kFireProtectionItems[] = {
    1001, 1068, 1069, 1070, 1071, 1072, 1073, 1074, 1075, 1076, 1077, 1078,
    1079, 1080, 1081, 1082, 1083, 1084, 1085, 1086, 1087, 1088, 1089, 1090,
    1091, 1092, 1093, 1094, 1095,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kFireProtectionExclusive[] = {3, 11, 27, 28,};
// clang-format on
// clang-format off
inline constexpr uint32_t kFlameItems[] = {
    1008,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kFortuneItems[] = {
    1026, 1027, 1028, 1029, 1031, 1032, 1033, 1034, 1036, 1037, 1038, 1039,
    1041, 1042, 1043, 1044, 1046, 1047, 1048, 1049, 1051, 1052, 1053, 1054,
    1056, 1057, 1058, 1059,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kFortuneExclusive[] = {13, 34,};
// clang-format on
// clang-format off
inline constexpr uint32_t kFrostWalkerItems[] = {
    1071, 1075, 1079, 1083, 1087, 1091, 1095,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kFrostWalkerExclusive[] = {7, 14,};
// clang-format on
// clang-format off
inline constexpr uint32_t kImpalingItems[] = {
    1483,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kImpalingExclusive[] = {1, 4, 6, 15, 33, 35,};
// clang-format on
// clang-format off
inline constexpr uint32_t kInfinityItems[] = {
    1008,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kInfinityExclusive[] = {16, 23,};
// clang-format on
// clang-format off
inline constexpr uint32_t kKnockbackItems[] = {
    1025, 1030, 1035, 1040, 1045, 1050, 1055, 1447, 1448, 1449, 1450, 1451,
    1452, 1453,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kLootingItems[] = {
    1025, 1030, 1035, 1040, 1045, 1050, 1055, 1447, 1448, 1449, 1450, 1451,
    1452, 1453,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kLoyaltyItems[] = {
    1483,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kLuckOfTheSeaItems[] = {
    1186,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kLungeItems[] = {
    1447, 1448, 1449, 1450, 1451, 1452, 1453,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kLureItems[] = {
    1186,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kMendingItems[] = {
    971, 972, 974, 1001, 1005, 1008, 1025, 1026, 1027, 1028, 1029, 1030,
    1031, 1032, 1033, 1034, 1035, 1036, 1037, 1038, 1039, 1040, 1041, 1042,
    1043, 1044, 1045, 1046, 1047, 1048, 1049, 1050, 1051, 1052, 1053, 1054,
    1055, 1056, 1057, 1058, 1059, 1068, 1069, 1070, 1071, 1072, 1073, 1074,
    1075, 1076, 1077, 1078, 1079, 1080, 1081, 1082, 1083, 1084, 1085, 1086,
    1087, 1088, 1089, 1090, 1091, 1092, 1093, 1094, 1095, 1186, 1255, 1374,
    1446, 1447, 1448, 1449, 1450, 1451, 1452, 1453, 1483, 1491, 1578,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kMultishotItems[] = {
    1491,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kMultishotExclusive[] = {24, 25,};
// clang-format on
// clang-format off
inline constexpr uint32_t kPiercingItems[] = {
    1491,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kPiercingExclusive[] = {24, 25,};
// clang-format on
// clang-format off
inline constexpr uint32_t kPowerItems[] = {
    1008,
};
// clang-format on
// clang-format off
inline constexpr GeneratedProtection kProjectileProtectionProtections[] = {
    {2.0f, 2.0f, 0x0000000002000000ull, 0x0000000000000800ull},
};
// clang-format on
// clang-format off
inline constexpr uint32_t kProjectileProtectionItems[] = {
    1001, 1068, 1069, 1070, 1071, 1072, 1073, 1074, 1075, 1076, 1077, 1078,
    1079, 1080, 1081, 1082, 1083, 1084, 1085, 1086, 1087, 1088, 1089, 1090,
    1091, 1092, 1093, 1094, 1095,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kProjectileProtectionExclusive[] = {3, 11, 27, 28,};
// clang-format on
// clang-format off
inline constexpr GeneratedProtection kProtectionProtections[] = {
    {1.0f, 1.0f, 0x0000000000000000ull, 0x0000000000000800ull},
};
// clang-format on
// clang-format off
inline constexpr uint32_t kProtectionItems[] = {
    1001, 1068, 1069, 1070, 1071, 1072, 1073, 1074, 1075, 1076, 1077, 1078,
    1079, 1080, 1081, 1082, 1083, 1084, 1085, 1086, 1087, 1088, 1089, 1090,
    1091, 1092, 1093, 1094, 1095,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kProtectionExclusive[] = {3, 11, 27, 28,};
// clang-format on
// clang-format off
inline constexpr uint32_t kPunchItems[] = {
    1008,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kQuickChargeItems[] = {
    1491,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kRespirationItems[] = {
    1001, 1068, 1072, 1076, 1080, 1084, 1088, 1092,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kRiptideItems[] = {
    1483,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kRiptideExclusive[] = {5, 19,};
// clang-format on
// clang-format off
inline constexpr uint32_t kSharpnessItems[] = {
    1025, 1028, 1030, 1033, 1035, 1038, 1040, 1043, 1045, 1048, 1050, 1053,
    1055, 1058, 1447, 1448, 1449, 1450, 1451, 1452, 1453,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kSharpnessExclusive[] = {1, 4, 6, 15, 33, 35,};
// clang-format on
// clang-format off
inline constexpr uint32_t kSilkTouchItems[] = {
    1026, 1027, 1028, 1029, 1031, 1032, 1033, 1034, 1036, 1037, 1038, 1039,
    1041, 1042, 1043, 1044, 1046, 1047, 1048, 1049, 1051, 1052, 1053, 1054,
    1056, 1057, 1058, 1059,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kSilkTouchExclusive[] = {13, 34,};
// clang-format on
// clang-format off
inline constexpr uint32_t kSmiteItems[] = {
    1025, 1028, 1030, 1033, 1035, 1038, 1040, 1043, 1045, 1048, 1050, 1053,
    1055, 1058, 1374, 1447, 1448, 1449, 1450, 1451, 1452, 1453,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kSmiteExclusive[] = {1, 4, 6, 15, 33, 35,};
// clang-format on
// clang-format off
inline constexpr uint32_t kSoulSpeedItems[] = {
    1071, 1075, 1079, 1083, 1087, 1091, 1095,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kSweepingEdgeItems[] = {
    1025, 1030, 1035, 1040, 1045, 1050, 1055,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kSwiftSneakItems[] = {
    1070, 1074, 1078, 1082, 1086, 1090, 1094,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kThornsItems[] = {
    1001, 1068, 1069, 1070, 1071, 1072, 1073, 1074, 1075, 1076, 1077, 1078,
    1079, 1080, 1081, 1082, 1083, 1084, 1085, 1086, 1087, 1088, 1089, 1090,
    1091, 1092, 1093, 1094, 1095,
};
// clang-format on
// clang-format off
inline constexpr GeneratedThorns kThornsThorns[] = {
    {"minecraft:thorns", {1.0f, 0.0f}, {5.0f, 0.0f}, {2.0f, 0.0f}, {0.15f, 0.15f}},
};
// clang-format on
// clang-format off
inline constexpr uint32_t kUnbreakingItems[] = {
    971, 972, 974, 1001, 1005, 1008, 1025, 1026, 1027, 1028, 1029, 1030,
    1031, 1032, 1033, 1034, 1035, 1036, 1037, 1038, 1039, 1040, 1041, 1042,
    1043, 1044, 1045, 1046, 1047, 1048, 1049, 1050, 1051, 1052, 1053, 1054,
    1055, 1056, 1057, 1058, 1059, 1068, 1069, 1070, 1071, 1072, 1073, 1074,
    1075, 1076, 1077, 1078, 1079, 1080, 1081, 1082, 1083, 1084, 1085, 1086,
    1087, 1088, 1089, 1090, 1091, 1092, 1093, 1094, 1095, 1186, 1255, 1374,
    1446, 1447, 1448, 1449, 1450, 1451, 1452, 1453, 1483, 1491, 1578,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kVanishingCurseItems[] = {
    431, 971, 972, 974, 1001, 1005, 1008, 1025, 1026, 1027, 1028, 1029,
    1030, 1031, 1032, 1033, 1034, 1035, 1036, 1037, 1038, 1039, 1040, 1041,
    1042, 1043, 1044, 1045, 1046, 1047, 1048, 1049, 1050, 1051, 1052, 1053,
    1054, 1055, 1056, 1057, 1058, 1059, 1068, 1069, 1070, 1071, 1072, 1073,
    1074, 1075, 1076, 1077, 1078, 1079, 1080, 1081, 1082, 1083, 1084, 1085,
    1086, 1087, 1088, 1089, 1090, 1091, 1092, 1093, 1094, 1095, 1151, 1186,
    1255, 1374, 1384, 1385, 1386, 1387, 1388, 1389, 1390, 1446, 1447, 1448,
    1449, 1450, 1451, 1452, 1453, 1483, 1491, 1578,
};
// clang-format on
// clang-format off
inline constexpr uint32_t kWindBurstItems[] = {
    1374,
};
// clang-format on

// clang-format off
// The table index is the wire id.
inline constexpr GeneratedEnchantment kEnchantments[] = {
    {"minecraft:aqua_affinity", 1, 32, 0x01000000u, nullptr, 0, kAquaAffinityItems, 8, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:bane_of_arthropods", 5, 1, 0x00000044u, nullptr, 0, kBaneOfArthropodsItems, 22, kBaneOfArthropodsExclusive, 6, {2.5f, 2.5f}, "minecraft:sensitive_to_bane_of_arthropods", kDamageTagEntityVictim, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, kBaneOfArthropodsPostAttack, 1, nullptr, 0},
    {"minecraft:binding_curse", 1, 60, 0x20000000u, nullptr, 0, kBindingCurseItems, 38, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:blast_protection", 4, 60, 0x01000001u, kBlastProtectionProtections, 1, kBlastProtectionItems, 29, kBlastProtectionExclusive, 4, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:breach", 4, 1, 0x00000020u, nullptr, 0, kBreachItems, 1, kBreachExclusive, 6, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:channeling", 1, 1, 0x00000140u, nullptr, 0, kChannelingItems, 1, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:density", 5, 1, 0x00000008u, nullptr, 0, kDensityItems, 1, kDensityExclusive, 6, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.5f, 0.5f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:depth_strider", 3, 4, 0x01000000u, nullptr, 0, kDepthStriderItems, 7, kDepthStriderExclusive, 2, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:efficiency", 5, 1, 0x01000000u, nullptr, 0, kEfficiencyItems, 29, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:feather_falling", 4, 60, 0x00000001u, kFeatherFallingProtections, 1, kFeatherFallingItems, 7, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:fire_aspect", 2, 1, 0x00000040u, nullptr, 0, kFireAspectItems, 15, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, kFireAspectPostAttack, 1, nullptr, 0},
    {"minecraft:fire_protection", 4, 60, 0x01000001u, kFireProtectionProtections, 1, kFireProtectionItems, 29, kFireProtectionExclusive, 4, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:flame", 1, 1, 0x00008000u, nullptr, 0, kFlameItems, 1, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:fortune", 3, 1, 0x00000000u, nullptr, 0, kFortuneItems, 28, kFortuneExclusive, 2, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:frost_walker", 2, 4, 0x00000802u, nullptr, 0, kFrostWalkerItems, 7, kFrostWalkerExclusive, 2, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:impaling", 5, 1, 0x00000004u, nullptr, 0, kImpalingItems, 1, kImpalingExclusive, 6, {2.5f, 2.5f}, "minecraft:sensitive_to_impaling", kDamageTagEntityVictim, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:infinity", 1, 1, 0x00002000u, nullptr, 0, kInfinityItems, 1, kInfinityExclusive, 2, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:knockback", 2, 1, 0x00000010u, nullptr, 0, kKnockbackItems, 14, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {1.0f, 1.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:looting", 3, 1, 0x00000400u, nullptr, 0, kLootingItems, 14, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:loyalty", 3, 1, 0x00040000u, nullptr, 0, kLoyaltyItems, 1, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:luck_of_the_sea", 3, 1, 0x00100000u, nullptr, 0, kLuckOfTheSeaItems, 1, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:lunge", 3, 3, 0x00000080u, nullptr, 0, kLungeItems, 7, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:lure", 3, 1, 0x00080000u, nullptr, 0, kLureItems, 1, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:mending", 1, 255, 0x00800000u, nullptr, 0, kMendingItems, 83, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:multishot", 1, 1, 0x00030000u, nullptr, 0, kMultishotItems, 1, kMultishotExclusive, 2, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:piercing", 4, 1, 0x00004000u, nullptr, 0, kPiercingItems, 1, kPiercingExclusive, 2, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:power", 5, 1, 0x00000004u, nullptr, 0, kPowerItems, 1, nullptr, 0, {1.0f, 0.5f}, "minecraft:arrows", kDamageTagEntityDirectAttacker, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:projectile_protection", 4, 60, 0x00000001u, kProjectileProtectionProtections, 1, kProjectileProtectionItems, 29, kProjectileProtectionExclusive, 4, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:protection", 4, 60, 0x00000001u, kProtectionProtections, 1, kProtectionItems, 29, kProtectionExclusive, 4, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:punch", 2, 1, 0x00000010u, nullptr, 0, kPunchItems, 1, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {1.0f, 1.0f}, "minecraft:arrows", kDamageTagEntityDirectAttacker, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:quick_charge", 3, 3, 0x06000000u, nullptr, 0, kQuickChargeItems, 1, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:respiration", 3, 32, 0x01000000u, nullptr, 0, kRespirationItems, 8, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:riptide", 3, 3, 0x48000000u, nullptr, 0, kRiptideItems, 1, kRiptideExclusive, 2, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:sharpness", 5, 1, 0x00000004u, nullptr, 0, kSharpnessItems, 21, kSharpnessExclusive, 6, {1.0f, 0.5f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:silk_touch", 1, 1, 0x00200000u, nullptr, 0, kSilkTouchItems, 28, kSilkTouchExclusive, 2, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:smite", 5, 1, 0x00000004u, nullptr, 0, kSmiteItems, 22, kSmiteExclusive, 6, {2.5f, 2.5f}, "minecraft:sensitive_to_smite", kDamageTagEntityVictim, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:soul_speed", 3, 4, 0x00001800u, nullptr, 0, kSoulSpeedItems, 7, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:sweeping_edge", 3, 1, 0x01000000u, nullptr, 0, kSweepingEdgeItems, 7, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {1.0f, 1.0f}, {2.0f, 1.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:swift_sneak", 3, 8, 0x01000000u, nullptr, 0, kSwiftSneakItems, 7, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:thorns", 3, 255, 0x00000040u, nullptr, 0, kThornsItems, 29, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, kThornsThorns, 1},
    {"minecraft:unbreaking", 3, 255, 0x00000200u, nullptr, 0, kUnbreakingItems, 83, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:vanishing_curse", 1, 255, 0x10000000u, nullptr, 0, kVanishingCurseItems, 92, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
    {"minecraft:wind_burst", 3, 1, 0x00000040u, nullptr, 0, kWindBurstItems, 1, nullptr, 0, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, nullptr, kDamageTagEntityNone, {0.0f, 0.0f}, {0.0f, 0.0f}, nullptr, 0, nullptr, 0},
};
inline constexpr int kEnchantmentCount = 43;
// clang-format on

} // namespace mc::world
