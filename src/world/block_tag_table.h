// SPDX-License-Identifier: GPL-3.0-or-later
// clang-format off
//
// The block tags the block-tick loop asks about (50 blocks, generated — do not
// hand-edit), flattened out of the vanilla 26.2 tag data pack by
// tools/gen_block_tags.py. A tag is the union of its entries and of every tag
// it names (`#minecraft:prevents_nearby_leaf_decay` is `#minecraft:logs`, which
// is `#logs_that_burn` plus the two stem tags), so what is stored here is the
// *resolved* membership: one bitmask per block, because that is what a
// tick-time lookup wants.
//
#pragma once

#include <cstdint>

namespace mc::world {

// `BlockTags.GROWS_CROPS`: what a crop grows fastest on (farmland).
constexpr uint8_t kBlockTagGrowsCrops = 1u << 0;

// `BlockTags.PREVENTS_NEARBY_LEAF_DECAY`: the blocks whose presence keeps a leaf at distance 0.
constexpr uint8_t kBlockTagPreventsNearbyLeafDecay = 1u << 1;

// `BlockTags.ANIMALS_SPAWNABLE_ON`: the ground an animal may spawn on (grass, dirt, ...).
constexpr uint8_t kBlockTagAnimalsSpawnableOn = 1u << 2;

// `BlockTags.PREVENT_MOB_SPAWNING_INSIDE`: a block a mob may never spawn inside (rails).
constexpr uint8_t kBlockTagPreventMobSpawningInside = 1u << 3;

struct GeneratedBlockTag {
  const char* name;
  uint8_t tags;
};

// Sorted by name, so block_has_tag (world/block_tags.h) can binary-search it.
// A block with no tags at all is absent.
inline constexpr GeneratedBlockTag kBlockTags[] = {
    {"minecraft:acacia_log", 2},
    {"minecraft:acacia_wood", 2},
    {"minecraft:activator_rail", 8},
    {"minecraft:birch_log", 2},
    {"minecraft:birch_wood", 2},
    {"minecraft:cherry_log", 2},
    {"minecraft:cherry_wood", 2},
    {"minecraft:crimson_hyphae", 2},
    {"minecraft:crimson_stem", 2},
    {"minecraft:dark_oak_log", 2},
    {"minecraft:dark_oak_wood", 2},
    {"minecraft:detector_rail", 8},
    {"minecraft:farmland", 1},
    {"minecraft:grass_block", 4},
    {"minecraft:jungle_log", 2},
    {"minecraft:jungle_wood", 2},
    {"minecraft:mangrove_log", 2},
    {"minecraft:mangrove_wood", 2},
    {"minecraft:oak_log", 2},
    {"minecraft:oak_wood", 2},
    {"minecraft:pale_oak_log", 2},
    {"minecraft:pale_oak_wood", 2},
    {"minecraft:powered_rail", 8},
    {"minecraft:rail", 8},
    {"minecraft:spruce_log", 2},
    {"minecraft:spruce_wood", 2},
    {"minecraft:stripped_acacia_log", 2},
    {"minecraft:stripped_acacia_wood", 2},
    {"minecraft:stripped_birch_log", 2},
    {"minecraft:stripped_birch_wood", 2},
    {"minecraft:stripped_cherry_log", 2},
    {"minecraft:stripped_cherry_wood", 2},
    {"minecraft:stripped_crimson_hyphae", 2},
    {"minecraft:stripped_crimson_stem", 2},
    {"minecraft:stripped_dark_oak_log", 2},
    {"minecraft:stripped_dark_oak_wood", 2},
    {"minecraft:stripped_jungle_log", 2},
    {"minecraft:stripped_jungle_wood", 2},
    {"minecraft:stripped_mangrove_log", 2},
    {"minecraft:stripped_mangrove_wood", 2},
    {"minecraft:stripped_oak_log", 2},
    {"minecraft:stripped_oak_wood", 2},
    {"minecraft:stripped_pale_oak_log", 2},
    {"minecraft:stripped_pale_oak_wood", 2},
    {"minecraft:stripped_spruce_log", 2},
    {"minecraft:stripped_spruce_wood", 2},
    {"minecraft:stripped_warped_hyphae", 2},
    {"minecraft:stripped_warped_stem", 2},
    {"minecraft:warped_hyphae", 2},
    {"minecraft:warped_stem", 2},
};
inline constexpr int kBlockTagCount = 50;

} // namespace mc::world
