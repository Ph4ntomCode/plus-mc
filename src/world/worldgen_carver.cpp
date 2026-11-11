// SPDX-License-Identifier: GPL-3.0-or-later
//
// `CaveWorldCarver` and `CanyonWorldCarver`, and the loop
// `NoiseBasedChunkGenerator.applyCarvers` drives them with. The arithmetic is
// transcribed from the 26.2 server's own classes; every float/double boundary
// is kept where Java has one, because a carve is a boundary test and a widened
// multiply moves cells.
#include "world/worldgen_carver.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <tuple>
#include <vector>

#include "world/worldgen_carver_table.h"
#include "world/worldgen_chunk.h"
#include "world/worldgen_math.h"
#include "world/worldgen_random.h"
#include "world/worldgen_surface.h"

namespace mc::world {
namespace {

// `(float)Math.PI`, the value both carvers multiply by before `Mth.sin`.
constexpr float kPiF = 3.14159265f;

// `Mth.SIN`: a 65,536-entry table of `(float)Math.sin(i / 10430.378350470453)`.
// Built once; the index is the truncated product's low 16 bits, exactly as
// `Mth.sin(double)` reads it. The entry is Java's `sin`, not the platform's:
// the two round a few entries differently, which the carve fixtures see.
const std::array<float, 65536>& sin_table() {
  static const std::array<float, 65536> table = [] {
    std::array<float, 65536> t{};
    for (int i = 0; i < 65536; ++i)
      t[static_cast<size_t>(i)] =
          static_cast<float>(wg_java_sin(static_cast<double>(i) / 10430.378350470453));
    return t;
  }();
  return table;
}

float mth_sin(double value) {
  const int64_t index =
      static_cast<int64_t>(value * 10430.378350470453) & static_cast<int64_t>(0xFFFF);
  return sin_table()[static_cast<size_t>(index)];
}

float mth_cos(double value) {
  const int64_t index =
      (static_cast<int64_t>(value * 10430.378350470453 + 16384.0)) & static_cast<int64_t>(0xFFFF);
  return sin_table()[static_cast<size_t>(index)];
}

// `FloatProvider.sample`: constant, uniform [min, max), or trapezoid. Java
// keeps these in binary32, so the arithmetic below is float until a caller
// widens it.
float sample_float(const WgFloatProvider& provider, WgLegacyRandom& random) {
  switch (provider.kind) {
  case WgFloatProviderKind::kConstant:
    return provider.min;
  case WgFloatProviderKind::kUniform:
    return random.next_float() * (provider.max - provider.min) + provider.min;
  case WgFloatProviderKind::kTrapezoid: {
    // `TrapezoidFloat.sample`: two draws, one over the plateau's end and one
    // over its ramp.
    const float range = provider.max - provider.min;
    const float plateau_start = (range - provider.plateau) / 2.0f;
    const float plateau_end = range - plateau_start;
    // Java evaluates the two draws left to right; hoisting them keeps the
    // stream in the game's order whatever the compiler picks.
    const float primary = random.next_float();
    const float secondary = random.next_float();
    return provider.min + primary * plateau_end + secondary * plateau_start;
  }
  }
  return provider.min;
}

int32_t resolve_anchor(WgAnchorKind kind, int32_t value, const WgCarverChunk& chunk) {
  switch (kind) {
  case WgAnchorKind::kAbsolute:
    return value;
  case WgAnchorKind::kAboveBottom:
    return chunk.min_y + value;
  case WgAnchorKind::kBelowTop:
    return chunk.height - 1 + chunk.min_y - value;
  }
  return value;
}

// `HeightProvider.sample` for the uniform shape every carver config uses:
// `Mth.randomBetweenInclusive` (`nextInt(max - min + 1) + min`), with vanilla's
// "empty range returns the minimum" guard.
int32_t sample_height(const WgHeightProvider& provider, WgLegacyRandom& random,
                      const WgCarverChunk& chunk) {
  const int32_t low = resolve_anchor(provider.min_kind, provider.min_value, chunk);
  const int32_t high = resolve_anchor(provider.max_kind, provider.max_value, chunk);
  if (low > high)
    return low;
  return random.next_int(high - low + 1) + low;
}

// `WorldgenRandom.setLargeFeatureSeed`: reseed, pull the two scale longs, and
// mix the source chunk into the seed.
void set_large_feature_seed(WgLegacyRandom& random, int64_t seed, int32_t chunk_x,
                            int32_t chunk_z) {
  random.set_seed(seed);
  const uint64_t x_scale = static_cast<uint64_t>(random.next_long());
  const uint64_t z_scale = static_cast<uint64_t>(random.next_long());
  const uint64_t result = static_cast<uint64_t>(chunk_x) * x_scale ^
                          static_cast<uint64_t>(chunk_z) * z_scale ^ static_cast<uint64_t>(seed);
  random.set_seed(static_cast<int64_t>(result));
}

// The replaceable tag as a bitmask over `WgSurfaceBlock` ordinals. Built once
// per carver config; a name the surface stage cannot produce (andesite,
// copper_ore, ...) simply has no bit.
uint64_t replaceable_mask(uint32_t carver_index) {
  static std::array<uint64_t, kWgCarverCount> masks{};
  static const bool built = [] {
    for (uint32_t index = 0; index < kWgCarverCount; ++index) {
      uint64_t mask = 0;
      const WgCarver& config = kWgCarvers[index];
      for (uint32_t i = 0; i < config.replaceable_count; ++i) {
        const char* name = kWgCarverReplaceables[config.replaceable_offset + i];
        for (uint32_t code = 0; code < kWgSurfaceBlockCount; ++code) {
          if (std::strcmp(wg_surface_state(static_cast<uint8_t>(code)).name, name) == 0) {
            mask |= uint64_t{1} << code;
            break;
          }
        }
      }
      masks[index] = mask;
    }
    return true;
  }();
  (void)built;
  return masks[carver_index];
}

struct CarveContext {
  const WgCarverChunk& chunk;
  const WgCarver& carver;
  WgAquifer& aquifer;
  // One mask per *target* chunk, shared by every source chunk and carver
  // (`ProtoChunk.getOrCreateCarvingMask`): a cell carved by one carver is not
  // offered to the next.
  std::vector<uint8_t>& mask;
  uint64_t replaceable = 0;
  int32_t lava_level = 0;
  uint32_t changed = 0;

  CarveContext(const WgCarverChunk& chunk_in, const WgCarver& carver_in, WgAquifer& aquifer_in,
               std::vector<uint8_t>& mask_in)
      : chunk(chunk_in), carver(carver_in), aquifer(aquifer_in), mask(mask_in),
        replaceable(replaceable_mask(static_cast<uint32_t>(&carver_in - kWgCarvers))),
        lava_level(resolve_anchor(carver_in.lava_kind, carver_in.lava_value, chunk_in)) {}

  size_t index(int32_t x, int32_t y, int32_t z) const {
    return (static_cast<size_t>(x) * 16 + static_cast<size_t>(z)) *
               static_cast<size_t>(chunk.height) +
           static_cast<size_t>(y - chunk.min_y);
  }
};

// `WorldCarver.carveBlock`, without the `topMaterial` half (see the header).
// `has_grass` tracks the column's grass for that half and is carried so the
// follow-up slice can add it without re-deriving the state machine.
bool carve_block(CarveContext& context, int32_t world_x, int32_t world_y, int32_t world_z,
                 bool& has_grass) {
  const size_t index = context.index(world_x & 0xF, world_y, world_z & 0xF);
  const uint8_t state = context.chunk.codes[index];
  if (state == static_cast<uint8_t>(WgSurfaceBlock::kGrassBlock) ||
      state == static_cast<uint8_t>(WgSurfaceBlock::kMycelium))
    has_grass = true;
  if (((context.replaceable >> state) & 1u) == 0)
    return false;

  // `NetherWorldCarver.carveBlock` is not the generic `WorldCarver` arm: it
  // writes lava through `minY + 31` and cave air above that, with no aquifer
  // query and no grass re-top. Using the generic path left the lava sea as
  // netherrack and wrote surface air instead of cave air.
  if (context.carver.kind == WgCarverKind::kNetherCave) {
    context.chunk.codes[index] = static_cast<uint8_t>(
        world_y <= context.chunk.min_y + 31 ? WgSurfaceBlock::kLava : WgSurfaceBlock::kCaveAir);
    ++context.changed;
    return true;
  }

  uint8_t carved = static_cast<uint8_t>(WgSurfaceBlock::kAir);
  if (world_y <= context.lava_level) {
    carved = static_cast<uint8_t>(WgSurfaceBlock::kLava);
  } else {
    const std::optional<WgNoiseBlock> substance =
        context.aquifer.compute_substance(world_x, world_y, world_z, 0.0);
    if (!substance.has_value())
      return false;
    carved = static_cast<uint8_t>(*substance);
  }
  context.chunk.codes[index] = carved;
  // `hasGrass` is the column's, not the cell's: once a tunnel has opened a
  // grass (or mycelium) ceiling, every later cell of that column re-tops the
  // dirt under it. `markPosForPostProcessing` has no meaning here — the carving
  // stage's output is the block array itself, not a chunk with a tick queue.
  if (has_grass) {
    const size_t below = context.index(world_x & 0xF, world_y - 1, world_z & 0xF);
    if (context.chunk.codes[below] == static_cast<uint8_t>(WgSurfaceBlock::kDirt) &&
        context.chunk.top_material != nullptr) {
      const bool under_fluid = carved == static_cast<uint8_t>(WgSurfaceBlock::kWater) ||
                               carved == static_cast<uint8_t>(WgSurfaceBlock::kLava);
      uint8_t top = 0;
      if (context.chunk.top_material(context.chunk.top_material_user, world_x, world_y - 1, world_z,
                                     under_fluid, &top))
        context.chunk.codes[below] = top;
    }
  }
  ++context.changed;
  return true;
}

using SkipChecker = std::function<bool(double xd, double yd, double zd, int32_t world_y)>;

// `WorldCarver.carveEllipsoid`.
bool carve_ellipsoid(CarveContext& context, double x, double y, double z, double horizontal_radius,
                     double vertical_radius, const SkipChecker& skip) {
  const int32_t chunk_min_x = context.chunk.chunk_x * 16;
  const int32_t chunk_min_z = context.chunk.chunk_z * 16;
  const double center_x = static_cast<double>(chunk_min_x + 8);
  const double center_z = static_cast<double>(chunk_min_z + 8);
  const double max_delta = 16.0 + horizontal_radius * 2.0;
  if (std::fabs(x - center_x) > max_delta || std::fabs(z - center_z) > max_delta)
    return false;

  const int32_t min_x_index =
      std::max(static_cast<int32_t>(std::floor(x - horizontal_radius)) - chunk_min_x - 1, 0);
  const int32_t max_x_index =
      std::min(static_cast<int32_t>(std::floor(x + horizontal_radius)) - chunk_min_x, 15);
  const int32_t min_y =
      std::max(static_cast<int32_t>(std::floor(y - vertical_radius)) - 1, context.chunk.min_y + 1);
  constexpr int32_t kProtectedBlocksOnTop = 7; // `chunk.isUpgrading() ? 0 : 7`
  const int32_t max_y =
      std::min(static_cast<int32_t>(std::floor(y + vertical_radius)) + 1,
               context.chunk.min_y + context.chunk.height - 1 - kProtectedBlocksOnTop);
  const int32_t min_z_index =
      std::max(static_cast<int32_t>(std::floor(z - horizontal_radius)) - chunk_min_z - 1, 0);
  const int32_t max_z_index =
      std::min(static_cast<int32_t>(std::floor(z + horizontal_radius)) - chunk_min_z, 15);

  bool carved = false;
  for (int32_t x_index = min_x_index; x_index <= max_x_index; ++x_index) {
    const int32_t world_x = chunk_min_x + x_index;
    const double xd = (static_cast<double>(world_x) + 0.5 - x) / horizontal_radius;
    for (int32_t z_index = min_z_index; z_index <= max_z_index; ++z_index) {
      const int32_t world_z = chunk_min_z + z_index;
      const double zd = (static_cast<double>(world_z) + 0.5 - z) / horizontal_radius;
      if (xd * xd + zd * zd >= 1.0)
        continue;
      bool has_grass = false;
      for (int32_t world_y = max_y; world_y > min_y; --world_y) {
        const double yd = (static_cast<double>(world_y) - 0.5 - y) / vertical_radius;
        const size_t index = context.index(x_index, world_y, z_index);
        if (skip(xd, yd, zd, world_y) || context.mask[index] != 0)
          continue;
        context.mask[index] = 1;
        carved |= carve_block(context, world_x, world_y, world_z, has_grass);
      }
    }
  }
  return carved;
}

// `WorldCarver.canReach` against the chunk being carved (not the source one).
bool can_reach(const WgCarverChunk& chunk, double x, double z, int32_t current_step,
               int32_t total_steps, float thickness) {
  const double x_mid = static_cast<double>(chunk.chunk_x * 16 + 8);
  const double z_mid = static_cast<double>(chunk.chunk_z * 16 + 8);
  const double xd = x - x_mid;
  const double zd = z - z_mid;
  const double remaining = static_cast<double>(total_steps - current_step);
  const double reach = static_cast<double>(thickness) + 2.0 + 16.0;
  return xd * xd + zd * zd - remaining * remaining <= reach * reach;
}

void carve_cave(CarveContext& context, WgLegacyRandom& random, int32_t source_x, int32_t source_z) {
  const WgCarver& config = context.carver;
  constexpr int32_t kMaxDistance = (4 * 2 - 1) * 16; // getRange() == 4
  // `NetherWorldCarver.getCaveBound()` is 10; the generic cave carver's is 15.
  const int32_t cave_bound = config.kind == WgCarverKind::kNetherCave ? 10 : 15;
  const int32_t cave_count = random.next_int(random.next_int(random.next_int(cave_bound) + 1) + 1);
  for (int32_t cave = 0; cave < cave_count; ++cave) {
    double x = static_cast<double>(source_x + random.next_int(16));
    double y = static_cast<double>(sample_height(config.y, random, context.chunk));
    double z = static_cast<double>(source_z + random.next_int(16));
    const double horizontal_multiplier =
        static_cast<double>(sample_float(config.horizontal_radius_multiplier, random));
    const double vertical_multiplier =
        static_cast<double>(sample_float(config.vertical_radius_multiplier, random));
    const float floor_level = sample_float(config.floor_level, random);
    const SkipChecker skip = [floor_level](double xd, double yd, double zd, int32_t) {
      if (yd <= static_cast<double>(floor_level))
        return true;
      return xd * xd + yd * yd + zd * zd >= 1.0;
    };
    int32_t tunnels = 1;
    if (random.next_int(4) == 0) {
      const float y_scale = sample_float(config.y_scale, random);
      const float thickness = 1.0f + random.next_float() * 6.0f;
      const double horizontal_radius = 1.5 + static_cast<double>(thickness);
      const double vertical_radius = horizontal_radius * static_cast<double>(y_scale);
      carve_ellipsoid(context, x + 1.0, y, z, horizontal_radius, vertical_radius, skip);
      tunnels += random.next_int(4);
    }
    for (int32_t tunnel = 0; tunnel < tunnels; ++tunnel) {
      const float horizontal_rotation = random.next_float() * (kPiF * 2.0f);
      const float vertical_rotation = (random.next_float() - 0.5f) / 4.0f;
      float thickness = 0.0f;
      if (config.kind == WgCarverKind::kNetherCave) {
        // `NetherWorldCarver.getThickness`: `(nextFloat() * 2 + nextFloat()) * 2`.
        // Keep the two draws in source order: argument evaluation order is
        // unspecified, and a shared RandomSource makes that a wire difference.
        const float first = random.next_float();
        const float second = random.next_float();
        thickness = (first * 2.0f + second) * 2.0f;
      } else {
        const float thickness_base = random.next_float() * 2.0f;
        thickness = thickness_base + random.next_float();
        if (random.next_int(10) == 0) {
          const float first = random.next_float();
          const float second = random.next_float();
          thickness *= first * second * 3.0f + 1.0f;
        }
      }
      const int32_t distance = kMaxDistance - random.next_int(kMaxDistance / 4);
      // `getYScale()`: 1.0 for the cave carver, 5.0 for the nether one.
      const double y_scale = config.kind == WgCarverKind::kNetherCave ? 5.0 : 1.0;
      const int64_t tunnel_seed = random.next_long();
      WgLegacyRandom tunnel_random(tunnel_seed);
      const int32_t split_point = tunnel_random.next_int(distance / 2) + distance / 4;
      const bool steep = tunnel_random.next_int(6) == 0;
      float y_rota = 0.0f;
      float x_rota = 0.0f;
      float local_vertical = vertical_rotation;
      float local_horizontal = horizontal_rotation;
      double tx = x;
      double ty = y;
      double tz = z;
      for (int32_t step = 0; step < distance; ++step) {
        const float angle = kPiF * static_cast<float>(step) / static_cast<float>(distance);
        const double horizontal_radius =
            1.5 + static_cast<double>(mth_sin(static_cast<double>(angle)) * thickness);
        const double vertical_radius = horizontal_radius * y_scale;
        const float cos_x = mth_cos(static_cast<double>(local_vertical));
        tx += static_cast<double>(mth_cos(static_cast<double>(local_horizontal)) * cos_x);
        ty += static_cast<double>(mth_sin(static_cast<double>(local_vertical)));
        tz += static_cast<double>(mth_sin(static_cast<double>(local_horizontal)) * cos_x);
        local_vertical *= steep ? 0.92f : 0.7f;
        local_vertical += x_rota * 0.1f;
        local_horizontal += y_rota * 0.1f;
        x_rota *= 0.9f;
        y_rota *= 0.75f;
        const float x_first = tunnel_random.next_float();
        const float x_second = tunnel_random.next_float();
        const float x_third = tunnel_random.next_float();
        x_rota += (x_first - x_second) * x_third * 2.0f;
        const float y_first = tunnel_random.next_float();
        const float y_second = tunnel_random.next_float();
        const float y_third = tunnel_random.next_float();
        y_rota += (y_first - y_second) * y_third * 4.0f;
        if (step == split_point && thickness > 1.0f) {
          // Java evaluates these arguments left to right; hoisting them keeps
          // the four draws in the game's order regardless of the C++ compiler.
          const int64_t seed_a = tunnel_random.next_long();
          const float thickness_a = tunnel_random.next_float() * 0.5f + 0.5f;
          const int64_t seed_b = tunnel_random.next_long();
          const float thickness_b = tunnel_random.next_float() * 0.5f + 0.5f;
          // The two branches are `createTunnel` itself, continued from this
          // step with the same skip checker and radii multipliers.
          for (const auto& branch : {std::make_tuple(seed_a, thickness_a, -1.5707964f),
                                     std::make_tuple(seed_b, thickness_b, 1.5707964f)}) {
            WgLegacyRandom branch_random(std::get<0>(branch));
            // The recursive call passes `dist` unchanged, so the branch splits
            // at the same fraction of the *total* length, not of the remainder.
            // Drawn for the stream, not for a value: the branch's own split
            // point can only be reached when its thickness is above 1, and the
            // two arguments this call passes are `random.nextFloat() * 0.5f +
            // 0.5f`, i.e. at most 1.0 — so `createTunnel`'s nested split never
            // fires and the draw is the whole of what it contributes.
            (void)branch_random.next_int(distance / 2);
            const bool branch_steep = branch_random.next_int(6) == 0;
            float branch_y_rota = 0.0f;
            float branch_x_rota = 0.0f;
            float branch_vertical = local_vertical / 3.0f;
            float branch_horizontal = local_horizontal + std::get<2>(branch);
            double bx = tx;
            double by = ty;
            double bz = tz;
            for (int32_t branch_step = step; branch_step < distance; ++branch_step) {
              const float branch_angle =
                  kPiF * static_cast<float>(branch_step) / static_cast<float>(distance);
              const double branch_horizontal_radius =
                  1.5 + static_cast<double>(mth_sin(static_cast<double>(branch_angle)) *
                                            std::get<1>(branch));
              const double branch_vertical_radius = branch_horizontal_radius * 1.0;
              const float branch_cos_x = mth_cos(static_cast<double>(branch_vertical));
              bx += static_cast<double>(mth_cos(static_cast<double>(branch_horizontal)) *
                                        branch_cos_x);
              by += static_cast<double>(mth_sin(static_cast<double>(branch_vertical)));
              bz += static_cast<double>(mth_sin(static_cast<double>(branch_horizontal)) *
                                        branch_cos_x);
              branch_vertical *= branch_steep ? 0.92f : 0.7f;
              branch_vertical += branch_x_rota * 0.1f;
              branch_horizontal += branch_y_rota * 0.1f;
              branch_x_rota *= 0.9f;
              branch_y_rota *= 0.75f;
              const float bx_first = branch_random.next_float();
              const float bx_second = branch_random.next_float();
              const float bx_third = branch_random.next_float();
              branch_x_rota += (bx_first - bx_second) * bx_third * 2.0f;
              const float by_first = branch_random.next_float();
              const float by_second = branch_random.next_float();
              const float by_third = branch_random.next_float();
              branch_y_rota += (by_first - by_second) * by_third * 4.0f;
              if (branch_random.next_int(4) == 0)
                continue;
              if (!can_reach(context.chunk, bx, bz, branch_step, distance, std::get<1>(branch)))
                break;
              carve_ellipsoid(context, bx, by, bz, branch_horizontal_radius * horizontal_multiplier,
                              branch_vertical_radius * vertical_multiplier, skip);
            }
          }
          // `createTunnel`'s own `return`, which ends this tunnel only; the
          // cave's remaining tunnels still start.
          break;
        }
        if (tunnel_random.next_int(4) == 0)
          continue;
        if (!can_reach(context.chunk, tx, tz, step, distance, thickness))
          break; // `createTunnel`'s early return: this tunnel only, not the cave
        carve_ellipsoid(context, tx, ty, tz, horizontal_radius * horizontal_multiplier,
                        vertical_radius * vertical_multiplier, skip);
      }
    }
  }
}

void carve_canyon(CarveContext& context, WgLegacyRandom& random, int32_t source_x,
                  int32_t source_z) {
  const WgCarver& config = context.carver;
  constexpr int32_t kMaxDistance = (4 * 2 - 1) * 16;
  const double x = static_cast<double>(source_x + random.next_int(16));
  const double y = static_cast<double>(sample_height(config.y, random, context.chunk));
  const double z = static_cast<double>(source_z + random.next_int(16));
  const float horizontal_rotation = random.next_float() * (kPiF * 2.0f);
  const float vertical_rotation = sample_float(config.vertical_rotation, random);
  const double y_scale = static_cast<double>(sample_float(config.y_scale, random));
  const float thickness = sample_float(config.canyon_thickness, random);
  const int32_t distance = static_cast<int32_t>(
      static_cast<float>(kMaxDistance) * sample_float(config.canyon_distance_factor, random));
  const int64_t tunnel_seed = random.next_long();
  WgLegacyRandom tunnel_random(tunnel_seed);
  // `initWidthFactors`: one factor per y index of the generated depth.
  std::vector<float> width_factors(static_cast<size_t>(context.chunk.height), 1.0f);
  {
    float width_factor = 1.0f;
    for (int32_t y_index = 0; y_index < context.chunk.height; ++y_index) {
      if (y_index == 0 || tunnel_random.next_int(config.canyon_width_smoothness) == 0) {
        const float first = tunnel_random.next_float();
        const float second = tunnel_random.next_float();
        width_factor = 1.0f + first * second;
      }
      width_factors[static_cast<size_t>(y_index)] = width_factor * width_factor;
    }
  }
  const SkipChecker skip = [&width_factors, &context](double xd, double yd, double zd,
                                                      int32_t world_y) {
    const int32_t y_index = world_y - context.chunk.min_y;
    return (xd * xd + zd * zd) *
                   static_cast<double>(width_factors[static_cast<size_t>(y_index - 1)]) +
               yd * yd / 6.0 >=
           1.0;
  };

  float y_rota = 0.0f;
  float x_rota = 0.0f;
  float local_vertical = vertical_rotation;
  float local_horizontal = horizontal_rotation;
  double cx = x;
  double cy = y;
  double cz = z;
  for (int32_t step = 0; step < distance; ++step) {
    const float angle = static_cast<float>(step) * kPiF / static_cast<float>(distance);
    double horizontal_radius =
        1.5 + static_cast<double>(mth_sin(static_cast<double>(angle)) * thickness);
    double vertical_radius = horizontal_radius * y_scale;
    horizontal_radius *=
        static_cast<double>(sample_float(config.canyon_horizontal_radius_factor, tunnel_random));
    // `updateVerticalRadius`.
    const float vertical_multiplier =
        1.0f - std::fabs(0.5f - static_cast<float>(step) / static_cast<float>(distance)) * 2.0f;
    const float factor = config.canyon_vertical_radius_default_factor +
                         config.canyon_vertical_radius_center_factor * vertical_multiplier;
    vertical_radius = static_cast<double>(factor) * vertical_radius *
                      static_cast<double>(tunnel_random.next_float() * 0.25f + 0.75f);
    const float cos_x = mth_cos(static_cast<double>(local_vertical));
    const float sin_x = mth_sin(static_cast<double>(local_vertical));
    cx += static_cast<double>(mth_cos(static_cast<double>(local_horizontal)) * cos_x);
    cy += static_cast<double>(sin_x);
    cz += static_cast<double>(mth_sin(static_cast<double>(local_horizontal)) * cos_x);
    local_vertical *= 0.7f;
    local_vertical += x_rota * 0.05f;
    local_horizontal += y_rota * 0.05f;
    x_rota *= 0.8f;
    y_rota *= 0.5f;
    const float x_first = tunnel_random.next_float();
    const float x_second = tunnel_random.next_float();
    const float x_third = tunnel_random.next_float();
    x_rota += (x_first - x_second) * x_third * 2.0f;
    const float y_first = tunnel_random.next_float();
    const float y_second = tunnel_random.next_float();
    const float y_third = tunnel_random.next_float();
    y_rota += (y_first - y_second) * y_third * 4.0f;
    if (tunnel_random.next_int(4) == 0)
      continue;
    if (!can_reach(context.chunk, cx, cz, step, distance, thickness))
      return;
    carve_ellipsoid(context, cx, cy, cz, horizontal_radius, vertical_radius, skip);
  }
}

const WgCarver* find_carver(const char* name) {
  uint32_t low = 0;
  uint32_t high = kWgCarverCount;
  while (low < high) {
    const uint32_t mid = (low + high) / 2;
    const int comparison = std::strcmp(kWgCarvers[mid].name, name);
    if (comparison == 0)
      return &kWgCarvers[mid];
    if (comparison < 0)
      low = mid + 1;
    else
      high = mid;
  }
  return nullptr;
}

const WgBiomeCarvers* find_biome_carvers(const char* biome) {
  if (biome == nullptr)
    return nullptr;
  uint32_t low = 0;
  uint32_t high = kWgBiomeCarversCount;
  while (low < high) {
    const uint32_t mid = (low + high) / 2;
    const int comparison = std::strcmp(kWgBiomeCarvers[mid].biome, biome);
    if (comparison == 0)
      return &kWgBiomeCarvers[mid];
    if (comparison < 0)
      low = mid + 1;
    else
      high = mid;
  }
  return nullptr;
}

} // namespace

uint32_t wg_biome_carvers(const char* biome, const char** out, uint32_t capacity) {
  const WgBiomeCarvers* range = find_biome_carvers(biome);
  if (range == nullptr)
    return 0;
  const uint32_t count = std::min<uint32_t>(range->count, capacity);
  for (uint32_t i = 0; i < count; ++i)
    out[i] = kWgBiomeCarverRefs[range->offset + i];
  return count;
}

uint32_t wg_apply_carvers(int64_t seed, const WgCarverChunk& chunk) {
  if (chunk.codes == nullptr || chunk.aquifer == nullptr || chunk.biome_at_quart == nullptr)
    return 0;
  uint32_t started = 0;
  WgLegacyRandom random(0);
  std::vector<uint8_t> mask(static_cast<size_t>(16) * 16 * chunk.height, 0);
  for (int32_t dx = -8; dx <= 8; ++dx) {
    for (int32_t dz = -8; dz <= 8; ++dz) {
      const int32_t source_chunk_x = chunk.chunk_x + dx;
      const int32_t source_chunk_z = chunk.chunk_z + dz;
      // `QuartPos.fromBlock(sourcePos.getMinBlockX())` and y = 0.
      const char* biome = chunk.biome_at_quart(chunk.user, (source_chunk_x * 16) >> 2, 0,
                                               (source_chunk_z * 16) >> 2);
      const WgBiomeCarvers* range = find_biome_carvers(biome);
      if (range == nullptr)
        continue;
      for (uint32_t index = 0; index < range->count; ++index) {
        const WgCarver* carver = find_carver(kWgBiomeCarverRefs[range->offset + index]);
        if (carver == nullptr)
          continue;
        const uint64_t mixed = static_cast<uint64_t>(seed) + index;
        set_large_feature_seed(random, static_cast<int64_t>(mixed), source_chunk_x, source_chunk_z);
        if (random.next_float() > carver->probability)
          continue;
        ++started;
        CarveContext context(chunk, *carver, *chunk.aquifer, mask);
        const int32_t source_x = source_chunk_x * 16;
        const int32_t source_z = source_chunk_z * 16;
        if (carver->kind == WgCarverKind::kCanyon) {
          carve_canyon(context, random, source_x, source_z);
        } else if (carver->kind == WgCarverKind::kCave ||
                   carver->kind == WgCarverKind::kNetherCave) {
          carve_cave(context, random, source_x, source_z);
        }
      }
    }
  }
  return started;
}

} // namespace mc::world
