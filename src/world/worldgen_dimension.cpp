// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/worldgen_dimension.h"

#include <iterator>

#include "world/worldgen_end_table.h"
#include "world/worldgen_nether_table.h"

namespace mc::world {

const WgDensityTable& wg_overworld_table() {
  static const WgDensityTable table = [] {
    WgDensityTable t;
    t.dfs = kWgDfs;
    t.df_count = static_cast<uint32_t>(std::size(kWgDfs));
    t.splines = kWgSplines;
    t.spline_count = static_cast<uint32_t>(std::size(kWgSplines));
    t.spline_points = kWgSplinePoints;
    t.spline_point_count = static_cast<uint32_t>(std::size(kWgSplinePoints));
    t.children = kWgDfChildren;
    t.thresholds = kWgDfThresholds;
    t.blended = kWgBlendedNoise;
    t.blended_count = static_cast<uint32_t>(std::size(kWgBlendedNoise));
    t.router = kWgRouter;
    t.noise_density_root = kWgNoiseDensityRoot;
    t.named = kWgNamed;
    t.named_count = static_cast<uint32_t>(std::size(kWgNamed));
    t.min_y = kWgMinY;
    t.height = kWgHeight;
    t.cell_width = kWgCellWidth;
    t.cell_height = kWgCellHeight;
    t.sea_level = kWgSeaLevel;
    t.aquifers_enabled = kWgAquifersEnabled;
    t.ore_veins_enabled = kWgOreVeinsEnabled;
    t.legacy_random_source = kWgLegacyRandomSource;
    t.default_block = kWgDefaultBlock;
    t.default_fluid = kWgDefaultFluid;
    t.default_fluid_level = kWgDefaultFluidLevel;
    t.default_fluid_code = 2; // water
    return t;
  }();
  return table;
}

const WgDensityTable& wg_nether_table() {
  static const WgDensityTable table = [] {
    WgDensityTable t;
    t.dfs = kWgNetherDfs;
    t.df_count = static_cast<uint32_t>(std::size(kWgNetherDfs));
    t.blended = kWgNetherBlendedNoise;
    t.blended_count = static_cast<uint32_t>(std::size(kWgNetherBlendedNoise));
    t.blended_base = static_cast<uint32_t>(std::size(kWgBlendedNoise));
    t.router = kWgNetherRouter;
    t.noise_density_root = kWgNetherNoiseDensityRoot;
    t.named = kWgNetherNamed;
    t.named_count = static_cast<uint32_t>(std::size(kWgNetherNamed));
    t.min_y = kWgNetherMinY;
    t.dimension = WgDimensionKind::kNether;
    t.height = kWgNetherHeight;
    t.cell_width = kWgNetherCellWidth;
    t.cell_height = kWgNetherCellHeight;
    t.sea_level = kWgNetherSeaLevel;
    t.aquifers_enabled = kWgNetherAquifersEnabled;
    t.ore_veins_enabled = kWgNetherOreVeinsEnabled;
    t.legacy_random_source = kWgNetherLegacyRandomSource;
    t.default_block = kWgNetherDefaultBlock;
    t.default_fluid = kWgNetherDefaultFluid;
    t.default_fluid_level = kWgNetherDefaultFluidLevel;
    t.default_fluid_code = 3; // lava
    return t;
  }();
  return table;
}

const WgDensityTable& wg_end_table() {
  static const WgDensityTable table = [] {
    WgDensityTable t;
    t.dfs = kWgEndDfs;
    t.df_count = static_cast<uint32_t>(std::size(kWgEndDfs));
    t.blended = kWgEndBlendedNoise;
    t.blended_count = static_cast<uint32_t>(std::size(kWgEndBlendedNoise));
    t.blended_base =
        static_cast<uint32_t>(std::size(kWgBlendedNoise) + std::size(kWgNetherBlendedNoise));
    t.router = kWgEndRouter;
    t.noise_density_root = kWgEndNoiseDensityRoot;
    t.named = kWgEndNamed;
    t.named_count = static_cast<uint32_t>(std::size(kWgEndNamed));
    t.min_y = kWgEndMinY;
    t.dimension = WgDimensionKind::kEnd;
    t.height = kWgEndHeight;
    t.cell_width = kWgEndCellWidth;
    t.cell_height = kWgEndCellHeight;
    t.sea_level = kWgEndSeaLevel;
    t.aquifers_enabled = kWgEndAquifersEnabled;
    t.ore_veins_enabled = kWgEndOreVeinsEnabled;
    t.legacy_random_source = kWgEndLegacyRandomSource;
    t.default_block = kWgEndDefaultBlock;
    t.default_fluid = kWgEndDefaultFluid;
    t.default_fluid_level = kWgEndDefaultFluidLevel;
    t.default_fluid_code = 0; // air
    return t;
  }();
  return table;
}

} // namespace mc::world
