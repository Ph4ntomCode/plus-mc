// SPDX-License-Identifier: GPL-3.0-or-later
//
// A density table as one value. The overworld, Nether and End each have their
// own `NoiseGeneratorSettings`: their own router, node list, height and cell
// size, and their own defaults. The evaluator is parameterised over this
// descriptor instead of naming `worldgen_table.h`'s globals directly, so the
// same wrapping/interpolation code runs all three dimensions.
#pragma once

#include <cstdint>

#include "world/worldgen_table.h"

namespace mc::world {

enum class WgDimensionKind { kOverworld, kNether, kEnd };

struct WgDensityTable {
  const WgDf* dfs = nullptr;
  uint32_t df_count = 0;
  const WgSpline* splines = nullptr;
  uint32_t spline_count = 0;
  const WgSplinePoint* spline_points = nullptr;
  uint32_t spline_point_count = 0;
  const uint32_t* children = nullptr;
  const double* thresholds = nullptr;
  const WgBlendedNoiseParams* blended = nullptr;
  uint32_t blended_count = 0;
  // Index of this table's first blended-noise parameter set in
  // `WgRandomState::blended` (the overworld's come first).
  uint32_t blended_base = 0;
  const uint32_t* router = nullptr;
  uint32_t noise_density_root = 0;
  const WgNamedDf* named = nullptr;
  uint32_t named_count = 0;

  int32_t min_y = 0;
  WgDimensionKind dimension = WgDimensionKind::kOverworld;
  int32_t height = 0;
  int32_t cell_width = 4;
  int32_t cell_height = 8;
  int32_t sea_level = 0;
  bool aquifers_enabled = false;
  bool ore_veins_enabled = false;
  bool legacy_random_source = false;

  const char* default_block = nullptr;
  const char* default_fluid = nullptr;
  int32_t default_fluid_level = 0;
  // `WgNoiseBlock` code of the default fluid, kept as a small integer here so
  // this descriptor does not depend on `worldgen_chunk.h`.
  int32_t default_fluid_code = 0; // 0 air, 2 water, 3 lava
};

const WgDensityTable& wg_overworld_table();
const WgDensityTable& wg_nether_table();
const WgDensityTable& wg_end_table();

} // namespace mc::world
