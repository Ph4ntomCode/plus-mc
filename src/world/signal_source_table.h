// SPDX-License-Identifier: GPL-3.0-or-later
// clang-format off
//
// The blocks whose `BlockState.isSignalSource()` is true (55 blocks,
// generated — do not hand-edit), read out of the vanilla 26.2 decompiled
// sources by tools/gen_signal_source_table.py. `isSignalSource` is a method
// override rather than a property, so the generator resolves each block's
// class up its `extends` chain against the classes that declare
// `isSignalSource(BlockState)` — which is what carries the answer for
// `RepeaterBlock extends DiodeBlock` and
// `RedstoneWallTorchBlock extends RedstoneTorchBlock`.
//
// This is one clause of `NaturalSpawner.isValidEmptySpawnBlock`: a mob cannot
// spawn inside a redstone component, so a lever or a button in the spawn box
// refuses the spawn (world/mob_spawn.cpp).
//
// Regenerate with tools/gen_signal_source_table.py.
#pragma once

namespace mc::world {

// Sorted by name, so block_is_signal_source can binary-search it.
inline constexpr const char* kSignalSourceBlocks[] = {
    "minecraft:acacia_button",
    "minecraft:acacia_pressure_plate",
    "minecraft:bamboo_button",
    "minecraft:bamboo_pressure_plate",
    "minecraft:birch_button",
    "minecraft:birch_pressure_plate",
    "minecraft:calibrated_sculk_sensor",
    "minecraft:cherry_button",
    "minecraft:cherry_pressure_plate",
    "minecraft:comparator",
    "minecraft:crimson_button",
    "minecraft:crimson_pressure_plate",
    "minecraft:dark_oak_button",
    "minecraft:dark_oak_pressure_plate",
    "minecraft:daylight_detector",
    "minecraft:detector_rail",
    "minecraft:exposed_lightning_rod",
    "minecraft:heavy_weighted_pressure_plate",
    "minecraft:jukebox",
    "minecraft:jungle_button",
    "minecraft:jungle_pressure_plate",
    "minecraft:lectern",
    "minecraft:lever",
    "minecraft:light_weighted_pressure_plate",
    "minecraft:lightning_rod",
    "minecraft:mangrove_button",
    "minecraft:mangrove_pressure_plate",
    "minecraft:oak_button",
    "minecraft:oak_pressure_plate",
    "minecraft:observer",
    "minecraft:oxidized_lightning_rod",
    "minecraft:pale_oak_button",
    "minecraft:pale_oak_pressure_plate",
    "minecraft:polished_blackstone_button",
    "minecraft:polished_blackstone_pressure_plate",
    "minecraft:redstone_block",
    "minecraft:redstone_torch",
    "minecraft:redstone_wall_torch",
    "minecraft:redstone_wire",
    "minecraft:repeater",
    "minecraft:sculk_sensor",
    "minecraft:spruce_button",
    "minecraft:spruce_pressure_plate",
    "minecraft:stone_button",
    "minecraft:stone_pressure_plate",
    "minecraft:target",
    "minecraft:trapped_chest",
    "minecraft:tripwire_hook",
    "minecraft:warped_button",
    "minecraft:warped_pressure_plate",
    "minecraft:waxed_exposed_lightning_rod",
    "minecraft:waxed_lightning_rod",
    "minecraft:waxed_oxidized_lightning_rod",
    "minecraft:waxed_weathered_lightning_rod",
    "minecraft:weathered_lightning_rod",
};
inline constexpr int kSignalSourceBlockCount = 55;

} // namespace mc::world

