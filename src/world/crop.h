// SPDX-License-Identifier: GPL-3.0-or-later
//
// `CropBlock`: how a planted crop grows (Phase 3 milestone J).
//
// A crop is a randomly-ticking block whose `randomTick` is one roll against the
// light level and the ground it stands on:
//
//     if (level.getRawBrightness(pos, 0) >= 9 && (age = getAge(state)) < getMaxAge()
//         && random.nextInt((int)(25.0f / getGrowthSpeed(this, level, pos)) + 1) == 0)
//         level.setBlock(pos, getStateForAge(age + 1), 2);
//
// `getGrowthSpeed` is the whole of the farming mechanic: the nine cells *below*
// the crop contribute 1.0 each (3.0 when the cell is hydrated farmland, i.e.
// `#minecraft:grows_crops` with `moisture > 0`), the eight side cells a quarter
// of that, and a crop packed in with its own kind halves the total — once for a
// neighbour on both axes, once for a diagonal one. A lone crop on dry farmland
// therefore grows at speed 1 (a 1-in-26 roll per tick, ~23 minutes a stage on
// average) and one on hydrated farmland in a field at ~1-in-9.
//
// Modelled, and from where:
//   * `CropBlock.isRandomlyTicking` / `randomTick` / `getGrowthSpeed` /
//     `getStateForAge` — all of it, per block, with each block's own
//     `getMaxAge` from world/random_tick_table.h (wheat, carrots and potatoes
//     end at 7, beetroot at 3, the torchflower at 1);
//   * `CropBlock.hasSufficientLight` / `canSurvive` need a light level, which
//     this engine models from the column's height (see the light note on
//     `RandomTickLevel::raw_brightness`). `randomTick` itself only compares
//     against 9, and `getRawBrightness(pos, 0)` is daytime skylight or a
//     glowstone, so a crop under open sky grows and one in the dark does not.
//
// What is NOT modelled, and why:
//   * **bone meal.** `BonemealableBlock.performBonemeal` (2..5 stages at once)
//     needs the item-use path to dispatch on the target block; nothing here
//     consumes bone meal on a crop yet;
//   * **the crop's own destruction.** A crop on a broken farmland, or one whose
//     light drops below 8, disappears in vanilla through `canSurvive` and the
//     neighbour update that follows. This engine has no neighbour-update pass,
//     so a crop survives its farmland being removed;
//   * **ravagers trampling crops.** `entityInside` with `MOB_GRIEFING`; the ravager
//     is not a mob this server has.
#pragma once

#include <string_view>

#include "world/chunk.h"
#include "world/random_tick.h"

namespace mc::world {

// `CropBlock.hasSufficientLight`: the light a crop needs to *survive* (its
// growth asks for 9).
constexpr int kCropSurvivalLight = 8;
// `CropBlock.randomTick`'s growth gate: `getRawBrightness(pos, 0) >= 9`.
constexpr int kCropGrowthLight = 9;
// `CropBlock.getGrowthSpeed`'s own scale: the roll is
// `nextInt((int)(25.0f / speed) + 1) == 0`.
constexpr float kCropGrowthDivisor = 25.0f;
// `FarmlandBlock.MOISTURE` for the hydrated-ground bonus (3.0 instead of 1.0).
constexpr int kFarmlandMaxMoisture = 7;

// What the generated table says about a block: is it a crop of this family,
// and what is its last age.
struct CropInfo {
  bool is_crop = false;
  int max_age = 0;
};
CropInfo crop_info(std::string_view block_name);

// `CropBlock.getAge` / `getMaxAge` / `isMaxAge` for a state of a known crop.
int crop_age(const BlockStateKey& state);
bool crop_is_max_age(const BlockStateKey& state);

// `CropBlock.isRandomlyTicking`: `!isMaxAge(state)`.
bool crop_is_randomly_ticking(const BlockStateKey& state);

// `CropBlock.getGrowthSpeed`: the nine below-cells, the four side neighbours and
// the diagonal, as the formula above.
float crop_growth_speed(const RandomTickLevel& level, const BlockStateKey& state, int x, int y,
                        int z);

// `CropBlock.getStateForAge`: the crop's own state at `age` (clamped to
// `getMaxAge`), which is what a growth writes and what a `/setblock`-style
// caller is really asking for.
BlockStateKey crop_state_for_age(const BlockStateKey& state, int age);

// `CropBlock.randomTick`: the two gates and the roll, then the age+1 write with
// `setBlock(pos, state, 2)` (clients, no neighbour update). Returns true when a
// stage was gained.
bool crop_random_tick(const RandomTickLevel& level, const RandomTickRandom& rng,
                      const BlockStateKey& state, int x, int y, int z);

} // namespace mc::world
