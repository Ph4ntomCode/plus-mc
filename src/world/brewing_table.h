// SPDX-License-Identifier: GPL-3.0-or-later
//
// The 26.3 brewing table (3 containers, 2 item mixes,
// 63 potion mixes, 1 fuel — generated, do not hand-edit).
//
// Brewing is code rather than data; 26.3 item ids are resolved below. The
// mixes come from
// the decompiled net/minecraft/world/item/alchemy/PotionBrewing.java and the
// fuel from the `minecraft:brewing_fuel` item tag that
// BrewingStandBlockEntity tests its fuel slot against. Ids are registry ids
// (world/item_registry.h and world/potion_table.h), because they are what a
// stack and a potion_contents component carry at runtime.
//
// Order is registration order and therefore lookup order. `Builder.addStartMix`
// contributes two entries — water plus the ingredient is mundane, awkward plus
// it is the named potion — which is why water appears more than once as a
// `from`. `mix` and `hasMix` take the *first* entry whose `from` and
// `ingredient` both match, so the order is preserved verbatim.
//
// Regenerate with tools/gen_brewing_table.py.
#pragma once

#include <cstdint>

namespace mc::world {

// PotionBrewing.BREWING_TIME_SECONDS = 20, in ticks: the data slot the client
// counts down from when a brew starts.
inline constexpr int kBrewingTimeTicks = 400;
// BrewingStandBlockEntity.FUEL_USES: the `Fuel` one unit of the fuel tag is
// worth. One use is spent to start a brew, not per tick.
inline constexpr int kBrewingFuelUses = 20;

// PotionBrewing.hasMix: the item a mix may start from (the three potion
// items, `Builder.addContainer`).
inline constexpr int32_t kBrewingContainers[] = {
    1271, // minecraft:potion
    1442, // minecraft:splash_potion
    1445, // minecraft:lingering_potion
};
inline constexpr int kBrewingContainerCount = 3;

// The `minecraft:brewing_fuel` tag, expanded (FuelSlot.mayPlaceItem).
inline constexpr int32_t kBrewingFuels[] = {
    1274, // minecraft:blaze_powder
};
inline constexpr int kBrewingFuelCount = 1;

// PotionBrewing.hasContainerMix / Mix<Item>: changing the container rather
// than the potion — `source` is the item, the potion it holds is untouched.
struct GeneratedContainerMix {
  int32_t from;
  int32_t ingredient;
  int32_t to;
};

inline constexpr GeneratedContainerMix kBrewingContainerMixes[] = {
    {1271, 1064, 1442}, // potion + gunpowder -> splash_potion
    {1442, 1441, 1445}, // splash_potion + dragon_breath -> lingering_potion
};
inline constexpr int kBrewingContainerMixCount = 2;

// PotionBrewing.hasPotionMix / Mix<Potion>: changing the potion, in the
// container the stack already is.
struct GeneratedPotionMix {
  int32_t from;
  int32_t ingredient;
  int32_t to;
};

inline constexpr GeneratedPotionMix kBrewingPotionMixes[] = {
    {0, 1189, 2},   // water + glowstone_dust -> thick
    {0, 824, 1},    // water + redstone -> mundane
    {0, 1269, 3},   // water + nether_wart -> awkward
    {0, 1373, 1},   // water + breeze_rod -> mundane
    {3, 1373, 42},  // awkward + breeze_rod -> wind_charged
    {0, 831, 1},    // water + slime_block -> mundane
    {3, 831, 44},   // awkward + slime_block -> oozing
    {0, 1, 1},      // water + stone -> mundane
    {3, 1, 45},     // awkward + stone -> infested
    {0, 237, 1},    // water + cobweb -> mundane
    {3, 237, 43},   // awkward + cobweb -> weaving
    {3, 1383, 4},   // awkward + golden_carrot -> night_vision
    {4, 824, 5},    // night_vision + redstone -> long_night_vision
    {4, 1273, 6},   // night_vision + fermented_spider_eye -> invisibility
    {5, 1273, 7},   // long_night_vision + fermented_spider_eye -> long_invisibility
    {6, 824, 7},    // invisibility + redstone -> long_invisibility
    {0, 1275, 1},   // water + magma_cream -> mundane
    {3, 1275, 11},  // awkward + magma_cream -> fire_resistance
    {11, 824, 12},  // fire_resistance + redstone -> long_fire_resistance
    {0, 1403, 1},   // water + rabbit_foot -> mundane
    {3, 1403, 8},   // awkward + rabbit_foot -> leaping
    {8, 824, 9},    // leaping + redstone -> long_leaping
    {8, 1189, 10},  // leaping + glowstone_dust -> strong_leaping
    {8, 1273, 16},  // leaping + fermented_spider_eye -> slowness
    {9, 1273, 17},  // long_leaping + fermented_spider_eye -> long_slowness
    {16, 824, 17},  // slowness + redstone -> long_slowness
    {16, 1189, 18}, // slowness + glowstone_dust -> strong_slowness
    {3, 1001, 19},  // awkward + turtle_helmet -> turtle_master
    {19, 824, 20},  // turtle_master + redstone -> long_turtle_master
    {19, 1189, 21}, // turtle_master + glowstone_dust -> strong_turtle_master
    {13, 1273, 16}, // swiftness + fermented_spider_eye -> slowness
    {14, 1273, 17}, // long_swiftness + fermented_spider_eye -> long_slowness
    {0, 1217, 1},   // water + sugar -> mundane
    {3, 1217, 13},  // awkward + sugar -> swiftness
    {13, 824, 14},  // swiftness + redstone -> long_swiftness
    {13, 1189, 15}, // swiftness + glowstone_dust -> strong_swiftness
    {3, 1193, 22},  // awkward + pufferfish -> water_breathing
    {22, 824, 23},  // water_breathing + redstone -> long_water_breathing
    {0, 1279, 1},   // water + glistering_melon_slice -> mundane
    {3, 1279, 24},  // awkward + glistering_melon_slice -> healing
    {24, 1189, 25}, // healing + glowstone_dust -> strong_healing
    {24, 1273, 26}, // healing + fermented_spider_eye -> harming
    {25, 1273, 27}, // strong_healing + fermented_spider_eye -> strong_harming
    {26, 1189, 27}, // harming + glowstone_dust -> strong_harming
    {28, 1273, 26}, // poison + fermented_spider_eye -> harming
    {29, 1273, 26}, // long_poison + fermented_spider_eye -> harming
    {30, 1273, 27}, // strong_poison + fermented_spider_eye -> strong_harming
    {0, 1272, 1},   // water + spider_eye -> mundane
    {3, 1272, 28},  // awkward + spider_eye -> poison
    {28, 824, 29},  // poison + redstone -> long_poison
    {28, 1189, 30}, // poison + glowstone_dust -> strong_poison
    {0, 1267, 1},   // water + ghast_tear -> mundane
    {3, 1267, 31},  // awkward + ghast_tear -> regeneration
    {31, 824, 32},  // regeneration + redstone -> long_regeneration
    {31, 1189, 33}, // regeneration + glowstone_dust -> strong_regeneration
    {0, 1274, 1},   // water + blaze_powder -> mundane
    {3, 1274, 34},  // awkward + blaze_powder -> strength
    {34, 824, 35},  // strength + redstone -> long_strength
    {34, 1189, 36}, // strength + glowstone_dust -> strong_strength
    {0, 1273, 37},  // water + fermented_spider_eye -> weakness
    {37, 824, 38},  // weakness + redstone -> long_weakness
    {3, 973, 40},   // awkward + phantom_membrane -> slow_falling
    {40, 824, 41},  // slow_falling + redstone -> long_slow_falling
};
inline constexpr int kBrewingPotionMixCount = 63;

} // namespace mc::world
