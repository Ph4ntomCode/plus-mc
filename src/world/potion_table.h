// SPDX-License-Identifier: GPL-3.0-or-later
//
// The 46 26.2 minecraft:potion entries and their 45 effects, in
// registry-id order (generated — do not hand-edit).
//
// minecraft:potion is a registry whose ids are synchronized to the client
// (`Potion.STREAM_CODEC` is ByteBufCodecs.holderRegistry, which writes the
// raw registry id), so a potion_contents component's `potion` field is the
// protocol_id the data generator's registries.json assigns. The table
// index *is* that id (the generator checks the ids are dense 0..N-1).
//
// A potion's effects are constructor arguments rather than data —
// `new Potion("healing", new MobEffectInstance(MobEffects.INSTANT_HEALTH, 1))`
// — so the entries come from parsing the decompiled
// net/minecraft/world/item/alchemy/Potions.java, with the registry key read
// from its PotionIds.java (which is where long_/strong_ variants get the
// key that `Potion.name()` does not repeat). The two numeric arguments are
// the duration in ticks and the amplifier; ambient, visible and show_icon
// are `MobEffectInstance`'s own constructor defaults (false/true/true) for
// every potion in the vanilla registry, so they are not columns here.
//
// Regenerate with tools/gen_potion_table.py.
#pragma once

#include <cstdint>

namespace mc::world {

struct GeneratedPotionEffect {
  int32_t effect;   // the minecraft:mob_effect registry id
  int32_t duration; // ticks
  int32_t amplifier;
};

struct GeneratedPotion {
  const char* name; // "minecraft:healing"
  int32_t id;
  // The potion's effects in the order `Potion.getEffects()` returns them,
  // which is the order a drink applies them.
  int32_t first;
  int32_t count;
};

// clang-format off
inline constexpr GeneratedPotion kPotions[] = {
    {"minecraft:water", 0, 0, 0},
    {"minecraft:mundane", 1, 0, 0},
    {"minecraft:thick", 2, 0, 0},
    {"minecraft:awkward", 3, 0, 0},
    {"minecraft:night_vision", 4, 0, 1},
    {"minecraft:long_night_vision", 5, 1, 1},
    {"minecraft:invisibility", 6, 2, 1},
    {"minecraft:long_invisibility", 7, 3, 1},
    {"minecraft:leaping", 8, 4, 1},
    {"minecraft:long_leaping", 9, 5, 1},
    {"minecraft:strong_leaping", 10, 6, 1},
    {"minecraft:fire_resistance", 11, 7, 1},
    {"minecraft:long_fire_resistance", 12, 8, 1},
    {"minecraft:swiftness", 13, 9, 1},
    {"minecraft:long_swiftness", 14, 10, 1},
    {"minecraft:strong_swiftness", 15, 11, 1},
    {"minecraft:slowness", 16, 12, 1},
    {"minecraft:long_slowness", 17, 13, 1},
    {"minecraft:strong_slowness", 18, 14, 1},
    {"minecraft:turtle_master", 19, 15, 2},
    {"minecraft:long_turtle_master", 20, 17, 2},
    {"minecraft:strong_turtle_master", 21, 19, 2},
    {"minecraft:water_breathing", 22, 21, 1},
    {"minecraft:long_water_breathing", 23, 22, 1},
    {"minecraft:healing", 24, 23, 1},
    {"minecraft:strong_healing", 25, 24, 1},
    {"minecraft:harming", 26, 25, 1},
    {"minecraft:strong_harming", 27, 26, 1},
    {"minecraft:poison", 28, 27, 1},
    {"minecraft:long_poison", 29, 28, 1},
    {"minecraft:strong_poison", 30, 29, 1},
    {"minecraft:regeneration", 31, 30, 1},
    {"minecraft:long_regeneration", 32, 31, 1},
    {"minecraft:strong_regeneration", 33, 32, 1},
    {"minecraft:strength", 34, 33, 1},
    {"minecraft:long_strength", 35, 34, 1},
    {"minecraft:strong_strength", 36, 35, 1},
    {"minecraft:weakness", 37, 36, 1},
    {"minecraft:long_weakness", 38, 37, 1},
    {"minecraft:luck", 39, 38, 1},
    {"minecraft:slow_falling", 40, 39, 1},
    {"minecraft:long_slow_falling", 41, 40, 1},
    {"minecraft:wind_charged", 42, 41, 1},
    {"minecraft:weaving", 43, 42, 1},
    {"minecraft:oozing", 44, 43, 1},
    {"minecraft:infested", 45, 44, 1},
};
inline constexpr int kPotionCount = 46;

inline constexpr GeneratedPotionEffect kPotionEffects[] = {
    {15, 3600, 0},  // minecraft:night_vision
    {15, 9600, 0},  // minecraft:long_night_vision
    {13, 3600, 0},  // minecraft:invisibility
    {13, 9600, 0},  // minecraft:long_invisibility
    {7, 3600, 0},  // minecraft:leaping
    {7, 9600, 0},  // minecraft:long_leaping
    {7, 1800, 1},  // minecraft:strong_leaping
    {11, 3600, 0},  // minecraft:fire_resistance
    {11, 9600, 0},  // minecraft:long_fire_resistance
    {0, 3600, 0},  // minecraft:swiftness
    {0, 9600, 0},  // minecraft:long_swiftness
    {0, 1800, 1},  // minecraft:strong_swiftness
    {1, 1800, 0},  // minecraft:slowness
    {1, 4800, 0},  // minecraft:long_slowness
    {1, 400, 3},  // minecraft:strong_slowness
    {1, 400, 3},  // minecraft:turtle_master
    {10, 400, 2},  // minecraft:turtle_master
    {1, 800, 3},  // minecraft:long_turtle_master
    {10, 800, 2},  // minecraft:long_turtle_master
    {1, 400, 5},  // minecraft:strong_turtle_master
    {10, 400, 3},  // minecraft:strong_turtle_master
    {12, 3600, 0},  // minecraft:water_breathing
    {12, 9600, 0},  // minecraft:long_water_breathing
    {5, 1, 0},  // minecraft:healing
    {5, 1, 1},  // minecraft:strong_healing
    {6, 1, 0},  // minecraft:harming
    {6, 1, 1},  // minecraft:strong_harming
    {18, 900, 0},  // minecraft:poison
    {18, 1800, 0},  // minecraft:long_poison
    {18, 432, 1},  // minecraft:strong_poison
    {9, 900, 0},  // minecraft:regeneration
    {9, 1800, 0},  // minecraft:long_regeneration
    {9, 450, 1},  // minecraft:strong_regeneration
    {4, 3600, 0},  // minecraft:strength
    {4, 9600, 0},  // minecraft:long_strength
    {4, 1800, 1},  // minecraft:strong_strength
    {17, 1800, 0},  // minecraft:weakness
    {17, 4800, 0},  // minecraft:long_weakness
    {25, 6000, 0},  // minecraft:luck
    {27, 1800, 0},  // minecraft:slow_falling
    {27, 4800, 0},  // minecraft:long_slow_falling
    {35, 3600, 0},  // minecraft:wind_charged
    {36, 3600, 0},  // minecraft:weaving
    {37, 3600, 0},  // minecraft:oozing
    {38, 3600, 0},  // minecraft:infested
};
inline constexpr int kPotionEffectCount = 45;
// clang-format on

} // namespace mc::world
