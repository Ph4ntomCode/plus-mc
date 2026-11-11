// SPDX-License-Identifier: GPL-3.0-or-later
//
// The 40 26.2 minecraft:mob_effect entries, in protocol-id order
// (generated — do not hand-edit).
//
// minecraft:mob_effect is a *built-in* registry, so the wire id in an
// update_mob_effect / remove_mob_effect packet is the protocol_id the
// data generator's registries.json assigns — which for this registry is
// also the registration order (the generator checks that the ids are
// dense 0..N-1). The table index *is* that id.
//
// Everything that is not an id — the colour the client tints the
// particles with, the category, the attribute modifiers and the blend
// durations — is compiled into MobEffects.java rather than carried in
// any data file, so those columns come from parsing the decompiled
// net/minecraft/world/effect/MobEffects.java.
//
// `amount` on a modifier is *per level*: MobEffect.AttributeTemplate
// .create multiplies it by (amplifier + 1) at runtime, which is why
// Speed II is +40% and not +20%.
//
// Regenerate with tools/gen_effect_table.py.
#pragma once

#include <cstdint>

namespace mc::world {

// MobEffectCategory. The ordinal is vanilla's enum order.
constexpr uint8_t kEffectCategoryBeneficial = 0;
constexpr uint8_t kEffectCategoryHarmful = 1;
constexpr uint8_t kEffectCategoryNeutral = 2;

// The applyEffectTick subclass an effect is built from, i.e. what it does
// on the ticks it fires. `plain` is MobEffect itself (a no-op that returns
// true), which is what an effect with no server-visible behaviour ticks on.
constexpr uint8_t kEffectPlain = 0;
constexpr uint8_t kEffectRegeneration = 1;
constexpr uint8_t kEffectPoison = 2;
constexpr uint8_t kEffectWither = 3;
constexpr uint8_t kEffectHunger = 4;
constexpr uint8_t kEffectSaturation = 5;
constexpr uint8_t kEffectInstantHealthOrHarm = 6;
constexpr uint8_t kEffectAbsorption = 7;
constexpr int kEffectKindCount = 8;

// AttributeModifier.Operation, in vanilla's enum order.
constexpr uint8_t kModifierAddValue = 0;
constexpr uint8_t kModifierAddMultipliedBase = 1;
constexpr uint8_t kModifierAddMultipliedTotal = 2;

struct GeneratedEffectModifier {
  const char* attribute; // minecraft:movement_speed
  float amount;          // per level: scaled by (amplifier + 1) at runtime
  uint8_t operation;     // kModifier*
};

struct GeneratedMobEffect {
  const char* name;          // minecraft:speed
  uint8_t category;          // kEffectCategory*
  uint8_t kind;              // kEffect*
  bool instantaneous;        // MobEffect.isInstantaneous()
  bool harm;                 // HealOrHarmMobEffect's isHarm flag
                             // (instant_damage rather than _health)
  uint8_t blend_in;          // MobEffect.getBlendInDurationTicks
  uint8_t blend_out;         // ...setBlendDuration's out ticks
  uint8_t blend_out_advance; // ...and the advance, for Darkness
  uint32_t color;            // MobEffect.getColor (the client's tint)
  const GeneratedEffectModifier* modifiers;
  int modifier_count;
};

// clang-format off
inline constexpr GeneratedEffectModifier kSpeedModifiers[] = {
    {"minecraft:movement_speed", 0.2f, kModifierAddMultipliedTotal},
};
// clang-format on
// clang-format off
inline constexpr GeneratedEffectModifier kSlownessModifiers[] = {
    {"minecraft:movement_speed", -0.15f, kModifierAddMultipliedTotal},
};
// clang-format on
// clang-format off
inline constexpr GeneratedEffectModifier kHasteModifiers[] = {
    {"minecraft:attack_speed", 0.1f, kModifierAddMultipliedTotal},
};
// clang-format on
// clang-format off
inline constexpr GeneratedEffectModifier kMiningFatigueModifiers[] = {
    {"minecraft:attack_speed", -0.1f, kModifierAddMultipliedTotal},
};
// clang-format on
// clang-format off
inline constexpr GeneratedEffectModifier kStrengthModifiers[] = {
    {"minecraft:attack_damage", 3.0f, kModifierAddValue},
};
// clang-format on
// clang-format off
inline constexpr GeneratedEffectModifier kJumpBoostModifiers[] = {
    {"minecraft:safe_fall_distance", 1.0f, kModifierAddValue},
};
// clang-format on
// clang-format off
inline constexpr GeneratedEffectModifier kInvisibilityModifiers[] = {
    {"minecraft:waypoint_transmit_range", -1.0f, kModifierAddMultipliedTotal},
};
// clang-format on
// clang-format off
inline constexpr GeneratedEffectModifier kWeaknessModifiers[] = {
    {"minecraft:attack_damage", -4.0f, kModifierAddValue},
};
// clang-format on
// clang-format off
inline constexpr GeneratedEffectModifier kHealthBoostModifiers[] = {
    {"minecraft:max_health", 4.0f, kModifierAddValue},
};
// clang-format on
// clang-format off
inline constexpr GeneratedEffectModifier kAbsorptionModifiers[] = {
    {"minecraft:max_absorption", 4.0f, kModifierAddValue},
};
// clang-format on
// clang-format off
inline constexpr GeneratedEffectModifier kLuckModifiers[] = {
    {"minecraft:luck", 1.0f, kModifierAddValue},
};
// clang-format on
// clang-format off
inline constexpr GeneratedEffectModifier kUnluckModifiers[] = {
    {"minecraft:luck", -1.0f, kModifierAddValue},
};
// clang-format on

// clang-format off
// The table index is the protocol id.
inline constexpr GeneratedMobEffect kMobEffects[] = {
    {"minecraft:speed", kEffectCategoryBeneficial, kEffectPlain, false, false, 0, 0, 0, 0x0033ebffu, kSpeedModifiers, 1},
    {"minecraft:slowness", kEffectCategoryHarmful, kEffectPlain, false, false, 0, 0, 0, 0x008bafe0u, kSlownessModifiers, 1},
    {"minecraft:haste", kEffectCategoryBeneficial, kEffectPlain, false, false, 0, 0, 0, 0x00d9c043u, kHasteModifiers, 1},
    {"minecraft:mining_fatigue", kEffectCategoryHarmful, kEffectPlain, false, false, 0, 0, 0, 0x004a4217u, kMiningFatigueModifiers, 1},
    {"minecraft:strength", kEffectCategoryBeneficial, kEffectPlain, false, false, 0, 0, 0, 0x00ffc700u, kStrengthModifiers, 1},
    {"minecraft:instant_health", kEffectCategoryBeneficial, kEffectInstantHealthOrHarm, true, false, 0, 0, 0, 0x00f82423u, nullptr, 0},
    {"minecraft:instant_damage", kEffectCategoryHarmful, kEffectInstantHealthOrHarm, true, true, 0, 0, 0, 0x00a9656au, nullptr, 0},
    {"minecraft:jump_boost", kEffectCategoryBeneficial, kEffectPlain, false, false, 0, 0, 0, 0x00fdff84u, kJumpBoostModifiers, 1},
    {"minecraft:nausea", kEffectCategoryHarmful, kEffectPlain, false, false, 150, 20, 60, 0x00551d4au, nullptr, 0},
    {"minecraft:regeneration", kEffectCategoryBeneficial, kEffectRegeneration, false, false, 0, 0, 0, 0x00cd5cabu, nullptr, 0},
    {"minecraft:resistance", kEffectCategoryBeneficial, kEffectPlain, false, false, 0, 0, 0, 0x009146f0u, nullptr, 0},
    {"minecraft:fire_resistance", kEffectCategoryBeneficial, kEffectPlain, false, false, 0, 0, 0, 0x00ff9900u, nullptr, 0},
    {"minecraft:water_breathing", kEffectCategoryBeneficial, kEffectPlain, false, false, 0, 0, 0, 0x0098dac0u, nullptr, 0},
    {"minecraft:invisibility", kEffectCategoryBeneficial, kEffectPlain, false, false, 0, 0, 0, 0x00f6f6f6u, kInvisibilityModifiers, 1},
    {"minecraft:blindness", kEffectCategoryHarmful, kEffectPlain, false, false, 0, 0, 0, 0x001f1f23u, nullptr, 0},
    {"minecraft:night_vision", kEffectCategoryBeneficial, kEffectPlain, false, false, 0, 0, 0, 0x00c2ff66u, nullptr, 0},
    {"minecraft:hunger", kEffectCategoryHarmful, kEffectHunger, false, false, 0, 0, 0, 0x00587653u, nullptr, 0},
    {"minecraft:weakness", kEffectCategoryHarmful, kEffectPlain, false, false, 0, 0, 0, 0x00484d48u, kWeaknessModifiers, 1},
    {"minecraft:poison", kEffectCategoryHarmful, kEffectPoison, false, false, 0, 0, 0, 0x0087a363u, nullptr, 0},
    {"minecraft:wither", kEffectCategoryHarmful, kEffectWither, false, false, 0, 0, 0, 0x00736156u, nullptr, 0},
    {"minecraft:health_boost", kEffectCategoryBeneficial, kEffectPlain, false, false, 0, 0, 0, 0x00f87d23u, kHealthBoostModifiers, 1},
    {"minecraft:absorption", kEffectCategoryBeneficial, kEffectAbsorption, false, false, 0, 0, 0, 0x002552a5u, kAbsorptionModifiers, 1},
    {"minecraft:saturation", kEffectCategoryBeneficial, kEffectSaturation, true, false, 0, 0, 0, 0x00f82423u, nullptr, 0},
    {"minecraft:glowing", kEffectCategoryNeutral, kEffectPlain, false, false, 0, 0, 0, 0x0094a061u, nullptr, 0},
    {"minecraft:levitation", kEffectCategoryHarmful, kEffectPlain, false, false, 0, 0, 0, 0x00ceffffu, nullptr, 0},
    {"minecraft:luck", kEffectCategoryBeneficial, kEffectPlain, false, false, 0, 0, 0, 0x0059c106u, kLuckModifiers, 1},
    {"minecraft:unluck", kEffectCategoryHarmful, kEffectPlain, false, false, 0, 0, 0, 0x00c0a44du, kUnluckModifiers, 1},
    {"minecraft:slow_falling", kEffectCategoryBeneficial, kEffectPlain, false, false, 0, 0, 0, 0x00f3cfb9u, nullptr, 0},
    {"minecraft:conduit_power", kEffectCategoryBeneficial, kEffectPlain, false, false, 0, 0, 0, 0x001dc2d1u, nullptr, 0},
    {"minecraft:dolphins_grace", kEffectCategoryBeneficial, kEffectPlain, false, false, 0, 0, 0, 0x0088a3beu, nullptr, 0},
    {"minecraft:bad_omen", kEffectCategoryNeutral, kEffectPlain, false, false, 0, 0, 0, 0x000b6138u, nullptr, 0},
    {"minecraft:hero_of_the_village", kEffectCategoryBeneficial, kEffectPlain, false, false, 0, 0, 0, 0x0044ff44u, nullptr, 0},
    {"minecraft:darkness", kEffectCategoryHarmful, kEffectPlain, false, false, 22, 22, 22, 0x00292721u, nullptr, 0},
    {"minecraft:trial_omen", kEffectCategoryNeutral, kEffectPlain, false, false, 0, 0, 0, 0x0016a6a6u, nullptr, 0},
    {"minecraft:raid_omen", kEffectCategoryNeutral, kEffectPlain, false, false, 0, 0, 0, 0x00de4058u, nullptr, 0},
    {"minecraft:wind_charged", kEffectCategoryHarmful, kEffectPlain, false, false, 0, 0, 0, 0x00bdc9ffu, nullptr, 0},
    {"minecraft:weaving", kEffectCategoryHarmful, kEffectPlain, false, false, 0, 0, 0, 0x0078695au, nullptr, 0},
    {"minecraft:oozing", kEffectCategoryHarmful, kEffectPlain, false, false, 0, 0, 0, 0x0099ffa3u, nullptr, 0},
    {"minecraft:infested", kEffectCategoryHarmful, kEffectPlain, false, false, 0, 0, 0, 0x008c9b8cu, nullptr, 0},
    {"minecraft:breath_of_the_nautilus", kEffectCategoryBeneficial, kEffectPlain, false, false, 0, 0, 0, 0x0000ffeeu, nullptr, 0},
};
inline constexpr int kMobEffectCount = 40;
// clang-format on

} // namespace mc::world
