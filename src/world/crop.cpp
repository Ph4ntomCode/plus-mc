// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/crop.h"

#include <algorithm>
#include <cstring>

#include "world/block_tags.h"

namespace mc::world {
namespace {

const GeneratedRandomTickBlock* find_block(std::string_view name) {
  const GeneratedRandomTickBlock* begin = kRandomTickBlocks;
  const GeneratedRandomTickBlock* end = kRandomTickBlocks + kRandomTickBlockCount;
  const GeneratedRandomTickBlock* it = std::lower_bound(
      begin, end, name, [](const GeneratedRandomTickBlock& entry, std::string_view key) {
        return std::string_view(entry.name) < key;
      });
  if (it == end || std::string_view(it->name) != name)
    return nullptr;
  return it;
}

// `BlockState.is(BlockTags.GROWS_CROPS)` — the ground a crop draws from.
bool grows_crops(const BlockStateKey& state) {
  return block_has_tag(kBlockTagGrowsCrops, state.name);
}

bool same_block(const BlockStateKey& a, const BlockStateKey& b) {
  return a.name == b.name;
}

} // namespace

CropInfo crop_info(std::string_view block_name) {
  const GeneratedRandomTickBlock* entry = find_block(block_name);
  CropInfo info;
  if (entry == nullptr || entry->kind != kRandomTickCrop)
    return info;
  info.is_crop = true;
  info.max_age = entry->max_age;
  return info;
}

int crop_age(const BlockStateKey& state) {
  return state_prop_int(state.properties, "age").value_or(0);
}

bool crop_is_max_age(const BlockStateKey& state) {
  const CropInfo info = crop_info(state.name);
  return !info.is_crop || crop_age(state) >= info.max_age;
}

bool crop_is_randomly_ticking(const BlockStateKey& state) {
  const CropInfo info = crop_info(state.name);
  return info.is_crop && crop_age(state) < info.max_age;
}

float crop_growth_speed(const RandomTickLevel& level, const BlockStateKey& state, int x, int y,
                        int z) {
  float speed = 1.0f;
  // The nine cells under the crop: the centre is the farmland the crop stands
  // on, the other eight are its neighbours, each worth a quarter.
  for (int xx = -1; xx <= 1; ++xx) {
    for (int zz = -1; zz <= 1; ++zz) {
      float block_speed = 0.0f;
      const BlockStateKey below = level.get_block(x + xx, y - 1, z + zz);
      if (grows_crops(below)) {
        block_speed = 1.0f;
        // `blockState.getValueOrElse(FarmlandBlock.MOISTURE, 0) > 0`.
        if (state_prop_int(below.properties, "moisture").value_or(0) > 0)
          block_speed = 3.0f;
      }
      if (xx != 0 || zz != 0)
        block_speed /= 4.0f;
      speed += block_speed;
    }
  }
  // `pos.north()` is -Z and `pos.west()` -X in Minecraft's axes.
  const bool horizontal = same_block(level.get_block(x - 1, y, z), state) ||
                          same_block(level.get_block(x + 1, y, z), state);
  const bool vertical = same_block(level.get_block(x, y, z - 1), state) ||
                        same_block(level.get_block(x, y, z + 1), state);
  if (horizontal && vertical) {
    speed /= 2.0f;
  } else {
    const bool diagonal = same_block(level.get_block(x - 1, y, z - 1), state) ||
                          same_block(level.get_block(x + 1, y, z - 1), state) ||
                          same_block(level.get_block(x + 1, y, z + 1), state) ||
                          same_block(level.get_block(x - 1, y, z + 1), state);
    if (diagonal)
      speed /= 2.0f;
  }
  return speed;
}

BlockStateKey crop_state_for_age(const BlockStateKey& state, int age) {
  const CropInfo info = crop_info(state.name);
  if (info.is_crop)
    age = std::clamp(age, 0, info.max_age);
  return state_with_prop_int(state, "age", age);
}

bool crop_random_tick(const RandomTickLevel& level, const RandomTickRandom& rng,
                      const BlockStateKey& state, int x, int y, int z) {
  const CropInfo info = crop_info(state.name);
  if (!info.is_crop)
    return false;
  const int age = crop_age(state);
  if (age >= info.max_age)
    return false;
  // `level.getRawBrightness(pos, 0) >= 9`.
  if (!level.raw_brightness || level.raw_brightness(x, y, z) < kCropGrowthLight)
    return false;
  const float speed = crop_growth_speed(level, state, x, y, z);
  if (speed <= 0.0f)
    return false;
  const int bound = static_cast<int>(kCropGrowthDivisor / speed) + 1;
  if (!rng.next_int || rng.next_int(bound) != 0)
    return false;
  // `level.setBlock(pos, getStateForAge(age + 1), 2)`: clients see the new
  // stage, no neighbour update follows it.
  level.set_block(x, y, z, crop_state_for_age(state, age + 1), /*notify=*/true);
  return true;
}

} // namespace mc::world
