// SPDX-License-Identifier: GPL-3.0-or-later
//
// `FossilFeature.place`, over the generated `worldgen_fossil_table.h`. The
// header names the stage's two approximations (neighbouring columns in the
// height/empty-corner scans, and dropped out-of-column writes).
#include "world/worldgen_fossil.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

#include "world/worldgen_table.h"

namespace mc::world {

namespace {

constexpr int kMinY = kWgMinY;
constexpr int kHeight = kWgHeight;

bool in_chunk(const Chunk& chunk, int wx, int wz) {
  const int lx = wx - chunk.x * 16;
  const int lz = wz - chunk.z * 16;
  return lx >= 0 && lx < 16 && lz >= 0 && lz < 16;
}

// The state at a world position; synthetic air outside the column (the same
// contract every feature kind has).
BlockStateKey block_at(const Chunk& chunk, int wx, int wy, int wz) {
  const int lx = wx - chunk.x * 16;
  const int lz = wz - chunk.z * 16;
  if (lx < 0 || lx >= 16 || lz < 0 || lz >= 16 || wy < kMinY || wy >= kMinY + kHeight)
    return BlockStateKey{"minecraft:air", ""};
  return chunk.block_state_at(lx, wy, lz);
}

bool is_air_name(std::string_view name) {
  return name == "minecraft:air" || name == "minecraft:cave_air" || name == "minecraft:void_air";
}

// `FossilFeature.countEmptyCorners`: `state.isAir() || LAVA || WATER`.
bool is_empty_corner(const BlockStateKey& state) {
  return is_air_name(state.name) || state.name == "minecraft:lava" ||
         state.name == "minecraft:water";
}

// A `|`-joined name list membership test (the generator's canonical form).
bool name_in_list(std::string_view list, std::string_view name) {
  size_t start = 0;
  while (start <= list.size()) {
    const size_t bar = list.find('|', start);
    const std::string_view piece =
        list.substr(start, bar == std::string_view::npos ? std::string_view::npos : bar - start);
    if (piece == name)
      return true;
    if (bar == std::string_view::npos)
      break;
    start = bar + 1;
  }
  return false;
}

// `Heightmap.Types.OCEAN_FLOOR_WG`'s answer: one past the highest
// motion-blocking cell in the column. The `_WG` heightmap is answered from the
// cells the stage has just written, which is what a generated column holds.
int ocean_floor_height(const Chunk& chunk, int lx, int lz) {
  for (int y = kMinY + kHeight - 1; y >= kMinY; --y)
    if (block_blocks_motion(chunk.block_state_at(lx, y, lz).name))
      return y + 1;
  return kMinY;
}

struct XZ {
  int x;
  int z;
};

// `StructureTemplate.transform` for Mirror.NONE and pivot (0, 0):
//
//   NONE               (x, z)
//   CLOCKWISE_90       (-z, x)
//   CLOCKWISE_180      (-x, -z)
//   COUNTERCLOCKWISE_90 (z, -x)
//
// (read off the 26.2 server's own `StructureTemplate.transform`, not guessed
// from the enum's names — the table is pinned again by the fossil test).
XZ rotate_xz(int x, int z, uint32_t rotation) {
  switch (rotation & 3u) {
  case 1:
    return XZ{-z, x};
  case 2:
    return XZ{-x, -z};
  case 3:
    return XZ{z, -x};
  default:
    return XZ{x, z};
  }
}

// `BlockState.rotate(Rotation)` for the only property the fossil templates
// carry: `bone_block`'s axis. A quarter turn swaps the horizontal axes and
// leaves `axis=y` and a half turn alone; the ore overlays' states have no
// properties.
std::string rotate_properties(std::string_view properties, uint32_t rotation) {
  if ((rotation & 3u) != 1 && (rotation & 3u) != 3)
    return std::string(properties);
  if (properties == "axis=x")
    return "axis=z";
  if (properties == "axis=z")
    return "axis=x";
  return std::string(properties);
}

struct Box {
  int min_x = 0;
  int min_y = 0;
  int min_z = 0;
  int max_x = 0;
  int max_y = 0;
  int max_z = 0;
};

// `StructureTemplate.getBoundingBox(settings, target)`: transform the two
// opposite corners of the *unrotated* size (0 and size - 1) and add the target.
Box template_box(const WgFossilTemplate& tpl, uint32_t rotation, int tx, int ty, int tz) {
  const XZ a = rotate_xz(0, 0, rotation);
  const XZ b = rotate_xz(tpl.size_x - 1, tpl.size_z - 1, rotation);
  Box box;
  box.min_x = tx + std::min(a.x, b.x);
  box.max_x = tx + std::max(a.x, b.x);
  box.min_z = tz + std::min(a.z, b.z);
  box.max_z = tz + std::max(a.z, b.z);
  box.min_y = ty;
  box.max_y = ty + tpl.size_y - 1;
  return box;
}

int count_empty_corners(const Chunk& chunk, const Box& box) {
  int empty = 0;
  const int xs[2] = {box.min_x, box.max_x};
  const int ys[2] = {box.min_y, box.max_y};
  const int zs[2] = {box.min_z, box.max_z};
  for (int x : xs)
    for (int y : ys)
      for (int z : zs) {
        // The approximation the header names: a corner this stage cannot read
        // is not counted as empty.
        if (!in_chunk(chunk, x, z))
          continue;
        if (is_empty_corner(block_at(chunk, x, y, z)))
          ++empty;
      }
  return empty;
}

// One `StructureTemplate.placeInWorld` pass: the palette draw, then one
// integrity roll per template block in the template's own order, then the
// config's rules and the protected-block check. Returns the cells changed.
//
// The processors run in the pack's order (`block_rot`, `rule`,
// `protected_blocks`). The base pass' protected reads see the pre-existing
// column; the overlay's see the skeleton, which is what vanilla's
// process-then-write separation produces for these templates (none of them
// writes a protected block, so the separation changes no cell).
uint32_t place_template(Chunk& chunk, const WgFossilTemplate& tpl, const WgFossilConfig& config,
                        WgRandom& rng, uint32_t rotation, int tx, int ty, int tz, float integrity) {
  // `StructurePlaceSettings.getRandomPalette` draws `nextInt(paletteCount)`
  // even when there is only one palette, so the next block's roll starts where
  // vanilla's does.
  (void)rng.next_int(1);
  uint32_t placed = 0;
  for (uint32_t i = 0; i < tpl.block_count; ++i) {
    const WgFossilBlock& block = kWgFossilBlocks[tpl.block_offset + i];
    const WgState& template_state = kWgFossilStates[block.state];
    if (rng.next_float() > integrity)
      continue;
    const WgState* state = &template_state;
    for (uint32_t r = 0; r < config.rule_count; ++r) {
      const WgFossilRule& rule = kWgFossilRules[config.rule_offset + r];
      if (std::strcmp(template_state.name, rule.input_block) == 0) {
        state = &rule.output;
        break;
      }
    }
    const XZ p = rotate_xz(block.x, block.z, rotation);
    const int wx = tx + p.x;
    const int wy = ty + block.y;
    const int wz = tz + p.z;
    if (!in_chunk(chunk, wx, wz) || wy < kMinY || wy >= kMinY + kHeight)
      continue;
    const int lx = wx - chunk.x * 16;
    const int lz = wz - chunk.z * 16;
    if (name_in_list(config.protected_blocks, chunk.block_state_at(lx, wy, lz).name))
      continue;
    if (chunk.set_block_state(
            lx, wy, lz, BlockStateKey{state->name, rotate_properties(state->properties, rotation)}))
      ++placed;
  }
  return placed;
}

} // namespace

WgFossilTransform wg_fossil_transform(int size_x, int size_y, int size_z, uint32_t rotation) {
  WgFossilTransform out;
  switch (rotation & 3u) {
  case 1:
    // `getSize`: a quarter turn swaps the horizontal extents.
    out.size_x = size_z;
    out.size_y = size_y;
    out.size_z = size_x;
    // `getZeroPositionWithTransform(pos, NONE, CLOCKWISE_90, sx, sz)`, read
    // off the 26.2 server's own answer (`size_z - 1`, not `size_z`).
    out.zero_x = size_z - 1;
    out.zero_z = 0;
    break;
  case 2:
    out.size_x = size_x;
    out.size_y = size_y;
    out.size_z = size_z;
    out.zero_x = size_x - 1;
    out.zero_z = size_z - 1;
    break;
  case 3:
    out.size_x = size_z;
    out.size_y = size_y;
    out.size_z = size_x;
    out.zero_x = 0;
    out.zero_z = size_x - 1;
    break;
  default:
    out.size_x = size_x;
    out.size_y = size_y;
    out.size_z = size_z;
    out.zero_x = 0;
    out.zero_z = 0;
    break;
  }
  return out;
}

uint32_t place_fossil(Chunk& chunk, const WgFossilConfig& config, WgRandom& rng, int ox, int oy,
                      int oz) {
  if (config.structure_count == 0 || config.overlay_count == 0)
    return 0;

  const uint32_t rotation = static_cast<uint32_t>(rng.next_int(4));
  const uint32_t pick =
      static_cast<uint32_t>(rng.next_int(static_cast<int32_t>(config.structure_count)));
  const WgFossilTemplate& base =
      kWgFossilTemplates[kWgFossilTemplateRefs[config.structure_offset + pick]];
  const WgFossilTemplate& overlay =
      kWgFossilTemplates[kWgFossilTemplateRefs[config.overlay_offset + pick]];

  const WgFossilTransform size =
      wg_fossil_transform(base.size_x, base.size_y, base.size_z, rotation);
  const int low_x = ox - size.size_x / 2;
  const int low_z = oz - size.size_z / 2;

  // `FossilFeature.place`'s height scan: the lowest `OCEAN_FLOOR_WG` column of
  // the rotated footprint. A column outside this stage's own chunk answers
  // with the nearest column it can read (the header's first approximation).
  int lowest = oy;
  for (int xs = 0; xs < size.size_x; ++xs)
    for (int zs = 0; zs < size.size_z; ++zs) {
      const int lx = std::clamp(low_x + xs - chunk.x * 16, 0, 15);
      const int lz = std::clamp(low_z + zs - chunk.z * 16, 0, 15);
      lowest = std::min(lowest, ocean_floor_height(chunk, lx, lz));
    }
  const int target_y = std::max(lowest - 15 - rng.next_int(10), kMinY + 10);
  const int target_x = low_x + size.zero_x;
  const int target_z = low_z + size.zero_z;

  if (count_empty_corners(chunk, template_box(base, rotation, target_x, target_y, target_z)) >
      static_cast<int>(config.max_empty_corners))
    return 0;

  uint32_t placed = 0;
  placed += place_template(chunk, base, config, rng, rotation, target_x, target_y, target_z,
                           config.structure_integrity);
  placed += place_template(chunk, overlay, config, rng, rotation, target_x, target_y, target_z,
                           config.overlay_integrity);
  return placed;
}

} // namespace mc::world
