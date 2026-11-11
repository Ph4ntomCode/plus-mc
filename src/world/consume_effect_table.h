// SPDX-License-Identifier: GPL-3.0-or-later
//
// `minecraft:consumable`'s `on_consume_effects` list: the 11 26.3 items
// that carry one (generated — do not hand-edit).
//
// Source: the vanilla data generator's reports — the component itself from
// reports/minecraft/components/item/*.json (the same files gen_item_table.py
// reads the food and consumable columns from), the effect ids from
// reports/registries.json — flattened into
// data/vanilla/26.3/consume_effects.tsv.
// Regenerate with tools/gen_consume_effect_table.py --reports <reports dir>.
//
// `kind` is the `minecraft:consume_effect_type` protocol id, i.e.
// ConsumeEffect.Type's registration order, and the constants below are that
// order spelled out. `value` is the one scalar a kind carries: a
// probability for apply_effects (the roll `nextFloat()` is compared
// against), a diameter for teleport_randomly (TeleportRandomlyConsumeEffect
// .DEFAULT_DIAMETER), and 1 otherwise.
//
// `first`/`count` index the arrays below: an apply_effects row into
// kConsumeStatusEffects (the MobEffectInstance list it applies, with
// MobEffectInstance.Details' own field names and defaults), a
// remove_effects row into kConsumeRemoveEffects (the HolderSet it names).
// Every other kind has count 0.
//
// `sound` is carried for the play_sound rows and nothing reads it: this
// server models no sounds. It is kept so the table shows *what* an ominous
// bottle would play rather than implying the row is empty.
#pragma once

#include <cstdint>

namespace mc::world {

// ConsumeEffect.Type, in registration order.
constexpr uint8_t kConsumeKindApplyEffects = 0;
constexpr uint8_t kConsumeKindRemoveEffects = 1;
constexpr uint8_t kConsumeKindClearAllEffects = 2;
constexpr uint8_t kConsumeKindTeleportRandomly = 3;
constexpr uint8_t kConsumeKindPlaySound = 4;
constexpr int kConsumeKindCount = 5;

struct GeneratedConsumeStatusEffect {
  int32_t effect;   // the mob_effect id, i.e. its wire id
  int32_t duration; // ticks
  int32_t amplifier;
  bool ambient;
  bool visible;   // show_particles
  bool show_icon; // defaults to show_particles (Details.create)
};

struct GeneratedConsumeEffect {
  int32_t item;      // the item id the row belongs to
  uint8_t kind;      // kConsumeKind*
  float value;       // probability / diameter
  const char* sound; // play_sound only; never read
  int32_t first;     // index into kConsumeStatusEffects / kConsumeRemoveEffects
  int32_t count;
};

inline constexpr int32_t kConsumeRemoveEffects[] = {
    18, // minecraft:poison
};
inline constexpr int kConsumeRemoveEffectCount = 1;

inline constexpr GeneratedConsumeStatusEffect kConsumeStatusEffects[] = {
    {/*minecraft:regeneration*/ 9, 100, 1, false, true, true},
    {/*minecraft:absorption*/ 21, 2400, 0, false, true, true},
    {/*minecraft:regeneration*/ 9, 400, 1, false, true, true},
    {/*minecraft:resistance*/ 10, 6000, 0, false, true, true},
    {/*minecraft:fire_resistance*/ 11, 6000, 0, false, true, true},
    {/*minecraft:absorption*/ 21, 2400, 3, false, true, true},
    {/*minecraft:poison*/ 18, 1200, 1, false, true, true},
    {/*minecraft:hunger*/ 16, 300, 2, false, true, true},
    {/*minecraft:nausea*/ 8, 300, 0, false, true, true},
    {/*minecraft:hunger*/ 16, 600, 0, false, true, true},
    {/*minecraft:hunger*/ 16, 600, 0, false, true, true},
    {/*minecraft:poison*/ 18, 100, 0, false, true, true},
    {/*minecraft:poison*/ 18, 100, 0, false, true, true},
};
inline constexpr int kConsumeStatusEffectCount = 13;

// clang-format off
inline constexpr GeneratedConsumeEffect kConsumeEffects[] = {
    {/*minecraft:golden_apple*/ 1100, 0, 1.0f, nullptr, 0, 2},
    {/*minecraft:enchanted_golden_apple*/ 1101, 0, 1.0f, nullptr, 2, 4},
    {/*minecraft:milk_bucket*/ 1134, 2, 1.0f, nullptr, 0, 0},
    {/*minecraft:pufferfish*/ 1193, 0, 1.0f, nullptr, 6, 3},
    {/*minecraft:chicken*/ 1262, 0, 0.3f, nullptr, 9, 1},
    {/*minecraft:rotten_flesh*/ 1264, 0, 0.8f, nullptr, 10, 1},
    {/*minecraft:spider_eye*/ 1272, 0, 1.0f, nullptr, 11, 1},
    {/*minecraft:poisonous_potato*/ 1381, 0, 0.6f, nullptr, 12, 1},
    {/*minecraft:chorus_fruit*/ 1434, 3, 16.0f, nullptr, 0, 0},
    {/*minecraft:honey_bottle*/ 1533, 1, 1.0f, nullptr, 0, 1},
    {/*minecraft:ominous_bottle*/ 1657, 4, 1.0f, "minecraft:item.ominous_bottle.dispose", 0, 0},
};
inline constexpr int kConsumeEffectCount = 11;
// clang-format on

} // namespace mc::world
