// SPDX-License-Identifier: GPL-3.0-or-later
// clang-format off
//
// Biome climate (67 biomes, 49 with precipitation —
// generated, do not hand-edit): the three fields `Biome.getPrecipitationAt`
// reads, transcribed from data/minecraft/worldgen/biome/*.json.
//
// `has_precipitation` is the gate: a biome without it answers
// `Biome.Precipitation.NONE` everywhere, so fire is never rained out in it and
// its cauldrons never fill (world/weather.h's `biome_precipitation_at`).
// `temperature` is compared against 0.15 (warm enough to rain) to pick RAIN or
// SNOW, and `temperature_modifier: frozen` is carried for completeness even
// though both of the modifier's noise fields are omitted (see world/weather.h).
//
// Regenerate with tools/gen_biome_table.py.
#pragma once

#include <cstdint>

namespace mc::world {

struct BiomeClimate {
  const char* name; // "minecraft:plains"
  bool has_precipitation;
  float temperature;
  bool frozen_modifier; // `temperature_modifier: minecraft:frozen`
};

// Sorted by name, so biome_climate_by_name can binary-search it.
inline constexpr BiomeClimate kBiomeClimate[] = {
    {"minecraft:badlands", false, 2.0f, false},
    {"minecraft:bamboo_jungle", true, 0.95f, false},
    {"minecraft:basalt_deltas", false, 2.0f, false},
    {"minecraft:beach", true, 0.8f, false},
    {"minecraft:birch_forest", true, 0.6f, false},
    {"minecraft:cherry_grove", true, 0.5f, false},
    {"minecraft:cold_ocean", true, 0.5f, false},
    {"minecraft:crimson_forest", false, 2.0f, false},
    {"minecraft:dappled_forest", true, 0.6f, false},
    {"minecraft:dark_forest", true, 0.7f, false},
    {"minecraft:deep_cold_ocean", true, 0.5f, false},
    {"minecraft:deep_dark", true, 0.8f, false},
    {"minecraft:deep_frozen_ocean", true, 0.5f, true},
    {"minecraft:deep_lukewarm_ocean", true, 0.5f, false},
    {"minecraft:deep_ocean", true, 0.5f, false},
    {"minecraft:desert", false, 2.0f, false},
    {"minecraft:dripstone_caves", true, 0.8f, false},
    {"minecraft:end_barrens", false, 0.5f, false},
    {"minecraft:end_highlands", false, 0.5f, false},
    {"minecraft:end_midlands", false, 0.5f, false},
    {"minecraft:eroded_badlands", false, 2.0f, false},
    {"minecraft:flower_forest", true, 0.7f, false},
    {"minecraft:forest", true, 0.7f, false},
    {"minecraft:frozen_ocean", true, 0.0f, true},
    {"minecraft:frozen_peaks", true, -0.7f, false},
    {"minecraft:frozen_river", true, 0.0f, false},
    {"minecraft:grove", true, -0.2f, false},
    {"minecraft:ice_spikes", true, 0.0f, false},
    {"minecraft:jagged_peaks", true, -0.7f, false},
    {"minecraft:jungle", true, 0.95f, false},
    {"minecraft:lukewarm_ocean", true, 0.5f, false},
    {"minecraft:lush_caves", true, 0.5f, false},
    {"minecraft:mangrove_swamp", true, 0.8f, false},
    {"minecraft:meadow", true, 0.5f, false},
    {"minecraft:mushroom_fields", true, 0.9f, false},
    {"minecraft:nether_wastes", false, 2.0f, false},
    {"minecraft:ocean", true, 0.5f, false},
    {"minecraft:old_growth_birch_forest", true, 0.6f, false},
    {"minecraft:old_growth_pine_taiga", true, 0.3f, false},
    {"minecraft:old_growth_spruce_taiga", true, 0.25f, false},
    {"minecraft:pale_garden", true, 0.7f, false},
    {"minecraft:plains", true, 0.8f, false},
    {"minecraft:river", true, 0.5f, false},
    {"minecraft:savanna", false, 2.0f, false},
    {"minecraft:savanna_plateau", false, 2.0f, false},
    {"minecraft:small_end_islands", false, 0.5f, false},
    {"minecraft:snowy_beach", true, 0.05f, false},
    {"minecraft:snowy_plains", true, 0.0f, false},
    {"minecraft:snowy_slopes", true, -0.3f, false},
    {"minecraft:snowy_taiga", true, -0.5f, false},
    {"minecraft:soul_sand_valley", false, 2.0f, false},
    {"minecraft:sparse_jungle", true, 0.95f, false},
    {"minecraft:stony_peaks", true, 1.0f, false},
    {"minecraft:stony_shore", true, 0.2f, false},
    {"minecraft:sulfur_caves", true, 0.8f, false},
    {"minecraft:sunflower_plains", true, 0.8f, false},
    {"minecraft:swamp", true, 0.8f, false},
    {"minecraft:taiga", true, 0.25f, false},
    {"minecraft:the_end", false, 0.5f, false},
    {"minecraft:the_void", false, 0.5f, false},
    {"minecraft:warm_ocean", true, 0.5f, false},
    {"minecraft:warped_forest", false, 2.0f, false},
    {"minecraft:windswept_forest", true, 0.2f, false},
    {"minecraft:windswept_gravelly_hills", true, 0.2f, false},
    {"minecraft:windswept_hills", true, 0.2f, false},
    {"minecraft:windswept_savanna", false, 2.0f, false},
    {"minecraft:wooded_badlands", false, 2.0f, false},
};

} // namespace mc::world
