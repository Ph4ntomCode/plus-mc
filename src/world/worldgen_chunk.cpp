// SPDX-License-Identifier: GPL-3.0-or-later
//
// `Aquifer`, `OreVeinifier` and the fill loop, transcribed. The pieces most
// worth reading closely are the four-nearest aquifer search (its comparison
// order decides which centers are "nearest" and therefore which fluid a cell
// gets), the shared `MutableDouble` the three pressure checks reuse, and the
// `<`-with-float-literals thresholds in the ore veins — `0.4f` cast to double
// is 0.4000000059604645, not 0.4, and the difference is visible in a dump.

#include "world/worldgen_chunk.h"

#include "world/worldgen_carver.h"
#include "world/worldgen_structure.h"
#include "world/worldgen_surface.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>

#include "world/worldgen_math.h"

namespace mc::world {
namespace {

// `Aquifer.NoiseBasedAquifer`'s lattice.
constexpr int32_t kGridXSamples = 10; // random offset range, [0, 10)
constexpr int32_t kGridYSamples = 9;  // random offset range, [0, 9)
constexpr int32_t kGridZSamples = 10;
constexpr int32_t kYSeparation = 12;
constexpr int32_t kMinCellSampleY = -1;
constexpr int32_t kMaxCellSampleY = 1;
constexpr int32_t kSampleOffsetX = -5;
constexpr int32_t kSampleOffsetY = 1;
constexpr int32_t kSampleOffsetZ = -5;
// `DimensionType.WAY_BELOW_MIN_Y = DimensionType.MIN_Y << 4` = -64 << 4.
constexpr int32_t kWayBelowMinY = -1024;

// `SURFACE_SAMPLING_OFFSETS_IN_CHUNKS`, in chunks; each becomes `* 16` blocks.
constexpr int32_t kSurfaceSamplingOffsets[13][2] = {
    {0, 0},  {-2, -1}, {-1, -1}, {0, -1}, {1, -1}, {-3, 0}, {-2, 0},
    {-1, 0}, {1, 0},   {-2, 1},  {-1, 1}, {0, 1},  {1, 1},
};

int32_t grid_x(int32_t block_coord) {
  return block_coord >> 4;
}
int32_t grid_y(int32_t block_coord) {
  return wg_floor_div(block_coord, kYSeparation);
}
int32_t grid_z(int32_t block_coord) {
  return block_coord >> 4;
}
int32_t from_grid_x(int32_t grid_coord, int32_t block_offset) {
  return (grid_coord << 4) + block_offset;
}
int32_t from_grid_y(int32_t grid_coord, int32_t block_offset) {
  return grid_coord * kYSeparation + block_offset;
}
int32_t from_grid_z(int32_t grid_coord, int32_t block_offset) {
  return (grid_coord << 4) + block_offset;
}

// `NoiseBasedAquifer.similarity`.
inline double similarity(int32_t distance_sqr_1, int32_t distance_sqr_2) {
  return 1.0 - static_cast<double>(distance_sqr_2 - distance_sqr_1) / 25.0;
}

// `FLOWING_UPDATE_SIMULARITY = similarity(100, 144)`.
constexpr double kFlowingUpdateSimilarity = 1.0 - (144.0 - 100.0) / 25.0;

// The four nearest aquifer-center bookkeeping, in one place so the comparison
// chain is not copy-pasted.
struct NearestCenters {
  int32_t index[4] = {0, 0, 0, 0};
  int32_t distance[4] = {std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max(),
                         std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max()};

  void offer(int32_t candidate_index, int32_t candidate_distance) {
    if (distance[0] >= candidate_distance) {
      index[3] = index[2];
      index[2] = index[1];
      index[1] = index[0];
      index[0] = candidate_index;
      distance[3] = distance[2];
      distance[2] = distance[1];
      distance[1] = distance[0];
      distance[0] = candidate_distance;
      return;
    }
    if (distance[1] >= candidate_distance) {
      index[3] = index[2];
      index[2] = index[1];
      index[1] = candidate_index;
      distance[3] = distance[2];
      distance[2] = distance[1];
      distance[1] = candidate_distance;
      return;
    }
    if (distance[2] >= candidate_distance) {
      index[3] = index[2];
      index[2] = candidate_index;
      distance[3] = distance[2];
      distance[2] = candidate_distance;
      return;
    }
    if (distance[3] < candidate_distance)
      return;
    index[3] = candidate_index;
    distance[3] = candidate_distance;
  }
};

// `BlockPos.asLong` and its three reads: x and z get 26 bits, y gets 12.
inline int64_t pack_block_pos(int32_t x, int32_t y, int32_t z) {
  return (static_cast<int64_t>(x) & 0x3FFFFFFLL) << 38 |
         (static_cast<int64_t>(z) & 0x3FFFFFFLL) << 12 | (static_cast<int64_t>(y) & 0xFFFLL);
}
inline int32_t unpack_block_x(int64_t packed) {
  return static_cast<int32_t>(packed >> 38);
}
inline int32_t unpack_block_y(int64_t packed) {
  return static_cast<int32_t>((packed << 52) >> 52);
}
inline int32_t unpack_block_z(int64_t packed) {
  return static_cast<int32_t>((packed << 26) >> 38);
}

// Defined below the chunk members it serves; declared here so they can call it.
WgClimateTarget climate_target_at(WgDensityGraph& graph, int32_t block_x, int32_t block_y,
                                  int32_t block_z);
const char* dimension_biome_at(WgDensityGraph& graph, const WgDensityTable& table, int32_t quart_x,
                               int32_t quart_y, int32_t quart_z);

WgChunkCursor make_cursor(int32_t cell_count_xz, int32_t chunk_min_block_x,
                          int32_t chunk_min_block_z,
                          const WgDensityTable& table = wg_overworld_table()) {
  WgChunkCursor cursor;
  cursor.cell_width = table.cell_width;
  cursor.cell_height = table.cell_height;
  cursor.cell_count_xz = cell_count_xz;
  cursor.cell_count_y = wg_floor_div(table.height, table.cell_height);
  cursor.cell_noise_min_y = wg_floor_div(table.min_y, table.cell_height);
  cursor.first_cell_x = wg_floor_div(chunk_min_block_x, table.cell_width);
  cursor.first_cell_z = wg_floor_div(chunk_min_block_z, table.cell_width);
  cursor.first_noise_x = chunk_min_block_x >> 2; // QuartPos.fromBlock
  cursor.first_noise_z = chunk_min_block_z >> 2;
  cursor.noise_size_xz = (cell_count_xz * table.cell_width) >> 2;
  return cursor;
}

} // namespace

WgAquifer::WgAquifer(const WgRandomState& state, WgDensityGraph& graph, int32_t chunk_min_block_x,
                     int32_t chunk_min_block_z, int32_t min_block_y, int32_t y_block_size,
                     int32_t barrier_node, int32_t fluid_level_floodedness_node,
                     int32_t fluid_level_spread_node, int32_t lava_node, int32_t erosion_node,
                     int32_t depth_node, int32_t preliminary_surface_node, int32_t sea_level,
                     WgNoiseBlock default_fluid)
    : random_(state.aquifer_random()), graph_(graph), barrier_node_(barrier_node),
      floodedness_node_(fluid_level_floodedness_node), spread_node_(fluid_level_spread_node),
      lava_node_(lava_node), erosion_node_(erosion_node), depth_node_(depth_node),
      preliminary_surface_node_(preliminary_surface_node), sea_level_(sea_level),
      default_fluid_(default_fluid) {
  const int32_t chunk_min_x = wg_floor_div(chunk_min_block_x, 16) * 16;
  const int32_t chunk_min_z = wg_floor_div(chunk_min_block_z, 16) * 16;
  const int32_t chunk_max_x = chunk_min_x + 15;
  const int32_t chunk_max_z = chunk_min_z + 15;

  min_grid_x_ = grid_x(chunk_min_x + kSampleOffsetX);
  const int32_t max_grid_x = grid_x(chunk_max_x + kSampleOffsetX) + 1;
  grid_size_x_ = max_grid_x - min_grid_x_ + 1;

  min_grid_y_ = grid_y(min_block_y + kSampleOffsetY) + kMinCellSampleY;
  const int32_t max_grid_y =
      grid_y(min_block_y + y_block_size + kSampleOffsetY) + kMaxCellSampleY + 1;
  const int32_t grid_size_y = max_grid_y - min_grid_y_ + 1;

  min_grid_z_ = grid_z(chunk_min_z + kSampleOffsetZ);
  const int32_t max_grid_z = grid_z(chunk_max_z + kSampleOffsetZ) + 1;
  grid_size_z_ = max_grid_z - min_grid_z_ + 1;

  const size_t total = static_cast<size_t>(grid_size_x_) * grid_size_y * grid_size_z_;
  aquifer_cache_.assign(total, WgFluidStatus{});
  // Vanilla's cache is an array of references with `null` as "not computed";
  // a status of {0, air} is a legitimate value, so a flag is carried beside it.
  aquifer_cache_present_.assign(total, 0);
  aquifer_location_cache_.assign(total, std::numeric_limits<int64_t>::max());

  const int32_t max_adjusted_surface_level = adjust_surface_level(
      max_preliminary_surface_level(from_grid_x(min_grid_x_, 0), from_grid_z(min_grid_z_, 0),
                                    from_grid_x(max_grid_x, 9), from_grid_z(max_grid_z, 9)));
  // `gridY(y + 12) - -1`, then `fromGridY(g, 11) - 1`.
  const int32_t skip_sampling_above_grid_y = grid_y(max_adjusted_surface_level + 12) - -1;
  skip_sampling_above_y_ = from_grid_y(skip_sampling_above_grid_y, 11) - 1;
}

int32_t WgAquifer::index_of(int32_t grid_x_value, int32_t grid_y_value,
                            int32_t grid_z_value) const {
  const int32_t x = grid_x_value - min_grid_x_;
  const int32_t y = grid_y_value - min_grid_y_;
  const int32_t z = grid_z_value - min_grid_z_;
  return (y * grid_size_z_ + z) * grid_size_x_ + x;
}

WgFluidStatus WgAquifer::global_fluid(int32_t x, int32_t y, int32_t z) const {
  (void)x;
  (void)z;
  // `NoiseBasedChunkGenerator.createFluidPicker`: a lava status at y -54, a
  // sea-level status of the dimension's default fluid, and the lava one wins
  // below `min(-54, seaLevel)`. The Nether sets the default fluid to lava, so
  // its disabled-aquifer path must not inherit the overworld's water picker.
  if (y < std::min(-54, sea_level_))
    return WgFluidStatus{-54, WgNoiseBlock::kLava};
  return WgFluidStatus{sea_level_, default_fluid_};
}

int32_t WgAquifer::preliminary_surface_level(int32_t sample_x, int32_t sample_z) {
  // `QuartPos.toBlock(QuartPos.fromBlock(v))`: quantized to 4 blocks.
  const int32_t quantized_x = (sample_x >> 2) << 2;
  const int32_t quantized_z = (sample_z >> 2) << 2;
  return wg_floor(
      graph_.compute(preliminary_surface_node_, WgDfContext::point(quantized_x, 0, quantized_z)));
}

int32_t WgAquifer::max_preliminary_surface_level(int32_t min_block_x, int32_t min_block_z,
                                                 int32_t max_block_x, int32_t max_block_z) {
  int32_t max_y = std::numeric_limits<int32_t>::min();
  for (int32_t block_z = min_block_z; block_z <= max_block_z; block_z += 4) {
    for (int32_t block_x = min_block_x; block_x <= max_block_x; block_x += 4) {
      max_y = std::max(max_y, preliminary_surface_level(block_x, block_z));
    }
  }
  return max_y;
}

WgFluidStatus WgAquifer::get_aquifer_status(int32_t index) {
  if (index < 0 || static_cast<size_t>(index) >= aquifer_cache_.size())
    return {};
  const size_t slot = static_cast<size_t>(index);
  if (aquifer_cache_present_[slot] != 0)
    return aquifer_cache_[slot];
  const int64_t location = aquifer_location_cache_[slot];
  const WgFluidStatus status =
      compute_fluid(unpack_block_x(location), unpack_block_y(location), unpack_block_z(location));
  aquifer_cache_[slot] = status;
  aquifer_cache_present_[slot] = 1;
  return status;
}

std::optional<WgNoiseBlock> WgAquifer::compute_substance(int32_t x, int32_t y, int32_t z,
                                                         double density) {
  if (density > 0.0) {
    should_schedule_fluid_update_ = false;
    return std::nullopt;
  }
  const WgFluidStatus global = global_fluid(x, y, z);
  if (y > skip_sampling_above_y_) {
    should_schedule_fluid_update_ = false;
    return global.at(y);
  }
  if (global.at(y) == WgNoiseBlock::kLava) {
    should_schedule_fluid_update_ = false;
    return WgNoiseBlock::kLava;
  }

  const int32_t x_anchor = grid_x(x + kSampleOffsetX);
  const int32_t y_anchor = grid_y(y + kSampleOffsetY);
  const int32_t z_anchor = grid_z(z + kSampleOffsetZ);
  NearestCenters nearest;
  for (int32_t dx = 0; dx <= 1; ++dx) {
    for (int32_t dy = kMinCellSampleY; dy <= kMaxCellSampleY; ++dy) {
      for (int32_t dz = 0; dz <= 1; ++dz) {
        const int32_t spaced_grid_x = x_anchor + dx;
        const int32_t spaced_grid_y = y_anchor + dy;
        const int32_t spaced_grid_z = z_anchor + dz;
        const int32_t index = index_of(spaced_grid_x, spaced_grid_y, spaced_grid_z);
        int64_t location = aquifer_location_cache_[static_cast<size_t>(index)];
        if (location == std::numeric_limits<int64_t>::max()) {
          // `positionalRandomFactory.at(gridX, gridY, gridZ)`, three draws.
          WgRandom random = random_.at(spaced_grid_x, spaced_grid_y, spaced_grid_z);
          // The three draws are pulled in their own statements on purpose. They
          // advance one shared generator, so writing them inline as
          // `pack_block_pos(nextInt, nextInt, nextInt)` leaves their order
          // unspecified: clang evaluates the argument list left to right and gcc
          // right to left, so the x and z offsets swap and gcc picks a
          // different aquifer centre (and therefore a different fluid) for the
          // same seed. Java, which this reproduces, orders those calls left to
          // right, so the order below is the faithful one as well as the fixed
          // one.
          const int32_t center_offset_x = random.next_int(kGridXSamples);
          const int32_t center_offset_y = random.next_int(kGridYSamples);
          const int32_t center_offset_z = random.next_int(kGridZSamples);
          location = pack_block_pos(from_grid_x(spaced_grid_x, center_offset_x),
                                    from_grid_y(spaced_grid_y, center_offset_y),
                                    from_grid_z(spaced_grid_z, center_offset_z));
          aquifer_location_cache_[static_cast<size_t>(index)] = location;
        }
        const int32_t center_x = unpack_block_x(location);
        const int32_t center_y = unpack_block_y(location);
        const int32_t center_z = unpack_block_z(location);
        const int32_t ddx = center_x - x;
        const int32_t ddy = center_y - y;
        const int32_t ddz = center_z - z;
        nearest.offer(index, ddx * ddx + ddy * ddy + ddz * ddz);
      }
    }
  }

  const WgFluidStatus closest_1 = get_aquifer_status(nearest.index[0]);
  const double similarity_12 = similarity(nearest.distance[0], nearest.distance[1]);
  const WgNoiseBlock fluid_state = closest_1.at(y);
  if (similarity_12 <= 0.0) {
    const WgFluidStatus closest_2 = get_aquifer_status(nearest.index[1]);
    should_schedule_fluid_update_ =
        similarity_12 >= kFlowingUpdateSimilarity && !(closest_1 == closest_2);
    return fluid_state;
  }
  if (fluid_state == WgNoiseBlock::kWater &&
      global_fluid(x, y - 1, z).at(y - 1) == WgNoiseBlock::kLava) {
    should_schedule_fluid_update_ = true;
    return fluid_state;
  }

  // One `MutableDouble`, shared by the three `calculatePressure` calls.
  double barrier_noise_value = std::numeric_limits<double>::quiet_NaN();
  const WgFluidStatus closest_2 = get_aquifer_status(nearest.index[1]);
  const double barrier_12 =
      similarity_12 * calculate_pressure(x, y, z, barrier_noise_value, closest_1, closest_2);
  if (density + barrier_12 > 0.0) {
    should_schedule_fluid_update_ = false;
    return std::nullopt;
  }
  const WgFluidStatus closest_3 = get_aquifer_status(nearest.index[2]);
  const double similarity_13 = similarity(nearest.distance[0], nearest.distance[2]);
  if (similarity_13 > 0.0) {
    const double barrier_13 =
        similarity_12 * similarity_13 *
        calculate_pressure(x, y, z, barrier_noise_value, closest_1, closest_3);
    if (density + barrier_13 > 0.0) {
      should_schedule_fluid_update_ = false;
      return std::nullopt;
    }
  }
  const double similarity_23 = similarity(nearest.distance[1], nearest.distance[2]);
  if (similarity_23 > 0.0) {
    const double barrier_23 =
        similarity_12 * similarity_23 *
        calculate_pressure(x, y, z, barrier_noise_value, closest_2, closest_3);
    if (density + barrier_23 > 0.0) {
      should_schedule_fluid_update_ = false;
      return std::nullopt;
    }
  }
  const bool may_flow_12 = !(closest_1 == closest_2);
  const bool may_flow_23 = similarity_23 >= kFlowingUpdateSimilarity && !(closest_2 == closest_3);
  const bool may_flow_13 = similarity_13 >= kFlowingUpdateSimilarity && !(closest_1 == closest_3);
  should_schedule_fluid_update_ = false;
  if (may_flow_12 || may_flow_23 || may_flow_13) {
    should_schedule_fluid_update_ = true;
  } else if (similarity_13 >= kFlowingUpdateSimilarity &&
             similarity(nearest.distance[0], nearest.distance[3]) >= kFlowingUpdateSimilarity &&
             !(closest_1 == get_aquifer_status(nearest.index[3]))) {
    should_schedule_fluid_update_ = true;
  }
  return fluid_state;
}

double WgAquifer::calculate_pressure(int32_t x, int32_t y, int32_t z, double& barrier_noise_value,
                                     const WgFluidStatus& first, const WgFluidStatus& second) {
  const WgNoiseBlock type_1 = first.at(y);
  const WgNoiseBlock type_2 = second.at(y);
  if ((type_1 == WgNoiseBlock::kLava && type_2 == WgNoiseBlock::kWater) ||
      (type_1 == WgNoiseBlock::kWater && type_2 == WgNoiseBlock::kLava)) {
    return 2.0;
  }
  const int32_t fluid_y_diff = std::abs(first.fluid_level - second.fluid_level);
  if (fluid_y_diff == 0)
    return 0.0;
  const double average_fluid_y = 0.5 * static_cast<double>(first.fluid_level + second.fluid_level);
  const double above_average = static_cast<double>(y) + 0.5 - average_fluid_y;
  const double base_value = static_cast<double>(fluid_y_diff) / 2.0;
  const double distance_from_barrier_edge = base_value - std::abs(above_average);
  const double gradient =
      above_average > 0.0
          ? (distance_from_barrier_edge > 0.0 ? distance_from_barrier_edge / 1.5
                                              : distance_from_barrier_edge / 2.5)
          : ((3.0 + distance_from_barrier_edge) > 0.0 ? (3.0 + distance_from_barrier_edge) / 3.0
                                                      : (3.0 + distance_from_barrier_edge) / 10.0);
  double noise_value = 0.0;
  if (gradient < -2.0 || gradient > 2.0) {
    noise_value = 0.0;
  } else if (barrier_noise_value != barrier_noise_value) { // NaN: not computed yet
    barrier_noise_value = graph_.compute(barrier_node_, WgDfContext::point(x, y, z));
    noise_value = barrier_noise_value;
  } else {
    noise_value = barrier_noise_value;
  }
  return 2.0 * (noise_value + gradient);
}

WgFluidStatus WgAquifer::compute_fluid(int32_t x, int32_t y, int32_t z) {
  const WgFluidStatus global = global_fluid(x, y, z);
  int32_t lowest_preliminary_surface = std::numeric_limits<int32_t>::max();
  const int32_t top_of_aquifer_cell = y + kYSeparation;
  const int32_t bottom_of_aquifer_cell = y - kYSeparation;
  bool surface_at_center_under_global_fluid_level = false;
  for (const auto& offset : kSurfaceSamplingOffsets) {
    const int32_t sample_x = x + offset[0] * 16;
    const int32_t sample_z = z + offset[1] * 16;
    const int32_t preliminary_surface = preliminary_surface_level(sample_x, sample_z);
    const int32_t adjusted_surface_level = adjust_surface_level(preliminary_surface);
    const bool start = offset[0] == 0 && offset[1] == 0;
    if (start && bottom_of_aquifer_cell > adjusted_surface_level)
      return global;
    const bool top_pokes_above_surface = top_of_aquifer_cell > adjusted_surface_level;
    if (top_pokes_above_surface || start) {
      const WgFluidStatus global_at_surface =
          global_fluid(sample_x, adjusted_surface_level, sample_z);
      if (global_at_surface.at(adjusted_surface_level) != WgNoiseBlock::kAir) {
        if (start)
          surface_at_center_under_global_fluid_level = true;
        if (top_pokes_above_surface)
          return global_at_surface;
      }
    }
    lowest_preliminary_surface = std::min(lowest_preliminary_surface, preliminary_surface);
  }
  const int32_t fluid_surface_level = compute_surface_level(
      x, y, z, global, lowest_preliminary_surface, surface_at_center_under_global_fluid_level);
  return {fluid_surface_level, compute_fluid_type(x, y, z, global, fluid_surface_level)};
}

int32_t WgAquifer::compute_surface_level(int32_t x, int32_t y, int32_t z,
                                         const WgFluidStatus& global,
                                         int32_t lowest_preliminary_surface,
                                         bool surface_at_center_under_global_fluid_level) {
  double fully_floodedness = 0.0;
  double partially_floodedness = 0.0;
  const WgDfContext context = WgDfContext::point(x, y, z);
  // `OverworldBiomeBuilder.isDeepDarkRegion`: erosion < (double) 0.225f
  // negated and depth > (double) 0.9f — float literals widened.
  if (graph_.compute(erosion_node_, context) < static_cast<double>(-0.225f) &&
      graph_.compute(depth_node_, context) > static_cast<double>(0.9f)) {
    partially_floodedness = -1.0;
    fully_floodedness = -1.0;
  } else {
    const int32_t distance_below_surface = lowest_preliminary_surface + 8 - y;
    const double floodedness_factor =
        surface_at_center_under_global_fluid_level
            ? wg_clamped_map(static_cast<double>(distance_below_surface), 0.0, 64.0, 1.0, 0.0)
            : 0.0;
    const double floodedness_noise_value =
        wg_clamp(graph_.compute(floodedness_node_, context), -1.0, 1.0);
    const double fully_flooded_threshold = wg_map(floodedness_factor, 1.0, 0.0, -0.3, 0.8);
    const double partially_flooded_threshold = wg_map(floodedness_factor, 1.0, 0.0, -0.8, 0.4);
    partially_floodedness = floodedness_noise_value - partially_flooded_threshold;
    fully_floodedness = floodedness_noise_value - fully_flooded_threshold;
  }
  if (fully_floodedness > 0.0)
    return global.fluid_level;
  if (partially_floodedness > 0.0) {
    return compute_randomized_fluid_surface_level(x, y, z, lowest_preliminary_surface);
  }
  return kWayBelowMinY;
}

int32_t WgAquifer::compute_randomized_fluid_surface_level(int32_t x, int32_t y, int32_t z,
                                                          int32_t lowest_preliminary_surface) {
  const int32_t fluid_level_cell_x = wg_floor_div(x, 16);
  const int32_t fluid_level_cell_y = wg_floor_div(y, 40);
  const int32_t fluid_level_cell_z = wg_floor_div(z, 16);
  const int32_t fluid_cell_middle_y = fluid_level_cell_y * 40 + 20;
  const double fluid_level_spread =
      graph_.compute(spread_node_, WgDfContext::point(fluid_level_cell_x, fluid_level_cell_y,
                                                      fluid_level_cell_z)) *
      10.0;
  const int32_t quantized = wg_quantize(fluid_level_spread, 3);
  return std::min(lowest_preliminary_surface, fluid_cell_middle_y + quantized);
}

WgNoiseBlock WgAquifer::compute_fluid_type(int32_t x, int32_t y, int32_t z,
                                           const WgFluidStatus& global,
                                           int32_t fluid_surface_level) {
  WgNoiseBlock fluid_type = global.fluid_type;
  if (fluid_surface_level <= -10 && fluid_surface_level != kWayBelowMinY &&
      global.fluid_type != WgNoiseBlock::kLava) {
    const int32_t fluid_type_cell_x = wg_floor_div(x, 64);
    const int32_t fluid_type_cell_y = wg_floor_div(y, 40);
    const int32_t fluid_type_cell_z = wg_floor_div(z, 64);
    const double lava_noise = graph_.compute(
        lava_node_, WgDfContext::point(fluid_type_cell_x, fluid_type_cell_y, fluid_type_cell_z));
    if (std::abs(lava_noise) > 0.3)
      fluid_type = WgNoiseBlock::kLava;
  }
  return fluid_type;
}

WgOreVeinifier::WgOreVeinifier(WgDensityGraph& graph, int32_t vein_toggle_node,
                               int32_t vein_ridged_node, int32_t vein_gap_node,
                               WgPositionalFactory ore_random)
    : graph_(graph), vein_toggle_node_(vein_toggle_node), vein_ridged_node_(vein_ridged_node),
      vein_gap_node_(vein_gap_node), ore_random_(ore_random) {}

std::optional<WgNoiseBlock> WgOreVeinifier::compute(int32_t x, int32_t y, int32_t z) {
  // Vanilla hands the *NoiseChunk* context here (`BlockStateFiller.calculate(this)`
  // in `NoiseBasedChunkGenerator.doFill`), not a bare point: `vein_toggle` is an
  // `Interpolated` marker, so a point context falls through to the raw filler and
  // skips the interpolation the fill loop has already advanced to. The three
  // vein reads are the same arithmetic; only which of the two paths is taken
  // differs, and the interpolated one is what the game fills with.
  const WgDfContext context = graph_.chunk_context();
  const double veininess = graph_.compute(vein_toggle_node_, context);
  const bool copper = veininess > 0.0;
  const double veininess_ridged = std::abs(veininess);
  const int32_t min_y = copper ? 0 : -60;
  const int32_t max_y = copper ? 50 : -8;
  const int32_t distance_from_top = max_y - y;
  const int32_t distance_from_bottom = y - min_y;
  if (distance_from_bottom < 0 || distance_from_top < 0)
    return std::nullopt;
  const int32_t distance_from_edge = std::min(distance_from_top, distance_from_bottom);
  const double edge_roundoff =
      wg_clamped_map(static_cast<double>(distance_from_edge), 0.0, 20.0, -0.2, 0.0);
  if (veininess_ridged + edge_roundoff < static_cast<double>(0.4f))
    return std::nullopt;
  WgRandom positional = ore_random_.at(x, y, z);
  if (positional.next_float() > 0.7f)
    return std::nullopt;
  if (graph_.compute(vein_ridged_node_, context) >= 0.0)
    return std::nullopt;
  const double richness =
      wg_clamped_map(veininess_ridged, static_cast<double>(0.4f), static_cast<double>(0.6f),
                     static_cast<double>(0.1f), static_cast<double>(0.3f));
  if (static_cast<double>(positional.next_float()) < richness &&
      graph_.compute(vein_gap_node_, context) > static_cast<double>(-0.3f)) {
    const bool raw = positional.next_float() < 0.02f;
    if (copper)
      return raw ? WgNoiseBlock::kRawCopperBlock : WgNoiseBlock::kCopperOre;
    return raw ? WgNoiseBlock::kRawIronBlock : WgNoiseBlock::kDeepslateIronOre;
  }
  return copper ? WgNoiseBlock::kGranite : WgNoiseBlock::kTuff;
}

std::shared_ptr<const WgSurfaceRuleTree> WgOverworldGenerator::surface_rules() const {
  std::shared_ptr<const WgSurfaceRuleTree> tree = std::atomic_load(&surface_rules_);
  if (!tree) {
    tree = table_->dimension == WgDimensionKind::kNether ? wg_build_nether_rules(state_, seed_)
                                                         : wg_build_overworld_rules(state_);
    std::atomic_store(&surface_rules_, tree);
  }
  return tree;
}

const WgStructurePlacementState& WgOverworldGenerator::structure_placement_state() const {
  std::shared_ptr<const WgStructurePlacementState> state =
      std::atomic_load(&structure_placement_state_);
  if (!state) {
    state = std::make_shared<WgStructurePlacementState>(*this);
    std::atomic_store(&structure_placement_state_, state);
  }
  return *state;
}

WgNoiseChunk::WgNoiseChunk(const WgRandomState& state, int32_t cell_count_xz,
                           int32_t chunk_min_block_x, int32_t chunk_min_block_z,
                           const WgDensityTable& table)
    : cursor_(make_cursor(cell_count_xz, chunk_min_block_x, chunk_min_block_z, table)),
      table_(&table), graph_(state, cursor_, /*wrap_markers=*/true, table),
      aquifer_(state, graph_, chunk_min_block_x, chunk_min_block_z, table.min_y, table.height,
               graph_.wrapped(table.router[0]), graph_.wrapped(table.router[1]),
               graph_.wrapped(table.router[2]), graph_.wrapped(table.router[3]),
               graph_.wrapped(table.router[7]), graph_.wrapped(table.router[8]),
               graph_.wrapped(table.router[10]), table.sea_level,
               static_cast<WgNoiseBlock>(table.default_fluid_code)),
      ore_veins_(graph_, graph_.wrapped(table.router[12]), graph_.wrapped(table.router[13]),
                 graph_.wrapped(table.router[14]), state.ore_random()) {
  // `NoiseChunk`'s own construction order, which is what decides when each
  // marker's state is allocated: the whole router is wrapped first (the
  // `flat_cache` grids fill themselves here), then the aquifer, then the
  // `cache_all_in_cell(add(final_density, beardifier))` root.
  for (uint32_t field = 0; field < 15; ++field) {
    graph_.wrapped(table.router[field]);
  }
  graph_.set_noise_density_root(graph_.wrapped(table.noise_density_root));
}

const char* WgNoiseChunk::biome_at_quart(int32_t quart_x, int32_t quart_y, int32_t quart_z) {
  // The cell's own hash picks the slot; the stored coordinates are compared so
  // a collision is a miss, never a wrong answer. `graph_`'s climate caches are
  // chunk-local, which is why the memo can live on the chunk object.
  // The A/B arm (`PLUSMC_BIOME_QUART_MEMO=off`), the same shape as
  // `PLUSMC_BIOME_MEMO`: "before" on the same binary, with nothing else rebuilt
  // between the two arms.
  static const bool kEnabled = [] {
    const char* text = std::getenv("PLUSMC_BIOME_QUART_MEMO");
    return text == nullptr || std::strcmp(text, "off") != 0;
  }();
  uint64_t hash = static_cast<uint32_t>(quart_x) * 0x9E3779B185EBCA87ull;
  hash ^= static_cast<uint32_t>(quart_y) * 0xC2B2AE3D27D4EB4Full;
  hash ^= static_cast<uint32_t>(quart_z) * 0x165667B19E3779F9ull;
  BiomeMemoEntry& entry = biome_memo_[(hash >> 32) % kBiomeMemoSlots];
  if (kEnabled && entry.value != nullptr && entry.x == quart_x && entry.y == quart_y &&
      entry.z == quart_z)
    return entry.value;
  const char* value = dimension_biome_at(graph_, *table_, quart_x, quart_y, quart_z);
  if (kEnabled) {
    entry.x = quart_x;
    entry.y = quart_y;
    entry.z = quart_z;
    entry.value = value;
  }
  return value;
}

void WgNoiseChunk::fill_chunk(uint8_t* blocks) {
  const int32_t cell_width = cursor_.cell_width;
  const int32_t cell_height = cursor_.cell_height;
  const int32_t cell_count_x = 16 / cell_width;
  const int32_t cell_count_z = 16 / cell_width;
  const int32_t cell_min_y = cursor_.cell_noise_min_y;
  const int32_t cell_count_y = cursor_.cell_count_y;
  const int32_t chunk_start_block_x = cursor_.first_cell_x * cell_width;
  const int32_t chunk_start_block_z = cursor_.first_cell_z * cell_width;

  graph_.initialize_for_first_cell_x();
  for (int32_t cell_x_index = 0; cell_x_index < cell_count_x; ++cell_x_index) {
    graph_.advance_cell_x(cell_x_index);
    for (int32_t cell_z_index = 0; cell_z_index < cell_count_z; ++cell_z_index) {
      for (int32_t cell_y_index = cell_count_y - 1; cell_y_index >= 0; --cell_y_index) {
        graph_.select_cell_yz(cell_y_index, cell_z_index);
        for (int32_t y_in_cell = cell_height - 1; y_in_cell >= 0; --y_in_cell) {
          const int32_t pos_y = (cell_min_y + cell_y_index) * cell_height + y_in_cell;
          const double factor_y = static_cast<double>(y_in_cell) / cell_height;
          graph_.update_for_y(pos_y, factor_y);
          for (int32_t x_in_cell = 0; x_in_cell < cell_width; ++x_in_cell) {
            const int32_t pos_x = chunk_start_block_x + cell_x_index * cell_width + x_in_cell;
            const double factor_x = static_cast<double>(x_in_cell) / cell_width;
            graph_.update_for_x(pos_x, factor_x);
            for (int32_t z_in_cell = 0; z_in_cell < cell_width; ++z_in_cell) {
              const int32_t pos_z = chunk_start_block_z + cell_z_index * cell_width + z_in_cell;
              const double factor_z = static_cast<double>(z_in_cell) / cell_width;
              graph_.update_for_z(pos_z, factor_z);

              // The block-state rule: the aquifer first, then the ore veins,
              // and the default block if neither applies.
              const double density = graph_.interpolated_density();
              WgNoiseBlock code = WgNoiseBlock::kStone;
              std::optional<WgNoiseBlock> substance;
              if (table_->aquifers_enabled) {
                substance = aquifer_.compute_substance(pos_x, pos_y, pos_z, density);
              } else if (density <= 0.0) {
                // `Aquifer.createDisabled`: the default block above the sea
                // level, the default fluid below it, air otherwise (an air
                // default fluid means the End has no sea).
                if (pos_y < table_->sea_level && table_->default_fluid_code != 0)
                  substance = static_cast<WgNoiseBlock>(table_->default_fluid_code);
                else
                  substance = WgNoiseBlock::kAir;
              }
              if (!substance.has_value() && table_->ore_veins_enabled) {
                substance = ore_veins_.compute(pos_x, pos_y, pos_z);
              }
              if (substance.has_value())
                code = *substance;
              if (code == WgNoiseBlock::kAir)
                continue;

              const size_t index =
                  (static_cast<size_t>(pos_x & 0xF) * 16 + (pos_z & 0xF)) * table_->height +
                  static_cast<size_t>(pos_y - table_->min_y);
              blocks[index] = static_cast<uint8_t>(code);
            }
          }
        }
      }
    }
    graph_.swap_slices();
  }
  graph_.stop_interpolation();
}

namespace {

// `Climate.Sampler`'s six entries in its own order: temperature, vegetation,
// continents, erosion, depth, ridges (`NoiseRouter`'s, and the biome stage's).
constexpr WgRouterField kClimateFields[6] = {
    WgRouterField::kTemperature, WgRouterField::kVegetation, WgRouterField::kContinents,
    WgRouterField::kErosion,     WgRouterField::kDepth,      WgRouterField::kRidges,
};

// One `SinglePointContext` sample of those six, quantized the way
// `Climate.Sampler.sample` does (`(float)` then `Climate.target`). `graph` is a
// wrapped graph: `NoiseChunk.cachedClimateSampler` is the sampler the biome
// fill actually reads through, and its stateful markers behave exactly as the
// port's do for a `!in_chunk` context — `flat_cache`/`cache_2d` look their
// position up (exact for a quart-aligned point), while `cache_once`,
// `cache_all_in_cell` and `interpolated` fall through to the plain algebra.
WgClimateTarget climate_target_at(WgDensityGraph& graph, int32_t block_x, int32_t block_y,
                                  int32_t block_z) {
  const WgDfContext context = WgDfContext::point(block_x, block_y, block_z);
  float value[6];
  for (size_t i = 0; i < 6; ++i) {
    value[i] = static_cast<float>(graph.compute(
        graph.wrapped(graph.table().router[static_cast<size_t>(kClimateFields[i])]), context));
  }
  return wg_climate_target(value[0], value[1], value[2], value[3], value[4], value[5]);
}

const char* dimension_biome_at(WgDensityGraph& graph, const WgDensityTable& table, int32_t quart_x,
                               int32_t quart_y, int32_t quart_z) {
  const int32_t block_x = quart_x * 4;
  const int32_t block_y = quart_y * 4;
  const int32_t block_z = quart_z * 4;
  if (table.dimension == WgDimensionKind::kEnd) {
    // `TheEndBiomeSource`: section coords from the block origin, then erosion
    // at the section's doubled origin. A quart's y is already the block y the
    // source passes in.
    const int32_t section_x = wg_floor_div(block_x, 16);
    const int32_t section_z = wg_floor_div(block_z, 16);
    const double erosion = graph.compute(
        graph.wrapped(table.router[7]),
        WgDfContext::point((section_x * 2 + 1) * 8, block_y, (section_z * 2 + 1) * 8));
    return wg_select_end_biome(section_x, section_z, erosion);
  }
  const WgClimateTarget target = climate_target_at(graph, block_x, block_y, block_z);
  return table.dimension == WgDimensionKind::kNether ? wg_select_nether_biome(target)
                                                     : wg_select_biome(target);
}

} // namespace

WgClimateSampler::WgClimateSampler(const WgRandomState& state, const WgDensityTable& table)
    : table_(&table), cursor_(make_cursor(1, 0, 0, table)),
      graph_(state, cursor_, /*wrap_markers=*/false, table) {}

WgClimateTarget WgClimateSampler::target_at_quart(int32_t quart_x, int32_t quart_y,
                                                  int32_t quart_z) {
  return climate_target_at(graph_, quart_x * 4, quart_y * 4, quart_z * 4);
}

const char* WgClimateSampler::biome_at_quart(int32_t quart_x, int32_t quart_y, int32_t quart_z) {
  return dimension_biome_at(graph_, *table_, quart_x, quart_y, quart_z);
}

namespace {

// The surface stage's view of one chunk. `noise_chunk` is the chunk the surface
// stage reads `preliminarySurfaceLevel` from and the biome source samples
// through; `fill_carved_chunk` hands the carver stage the same one, which is why
// its aquifer and caches are already warm there.
WgSurfaceChunk surface_chunk_inputs(int32_t chunk_x, int32_t chunk_z, uint8_t* out,
                                    WgNoiseChunk& noise_chunk, const WgDensityTable& table) {
  WgSurfaceChunk inputs;
  inputs.chunk_x = chunk_x;
  inputs.chunk_z = chunk_z;
  inputs.min_y = table.min_y;
  inputs.height = table.height;
  inputs.sea_level = table.sea_level;
  inputs.use_legacy_random = table.legacy_random_source;
  inputs.codes = out;
  inputs.out = out;
  inputs.user = &noise_chunk;
  inputs.preliminary_surface_level = [](void* user, int32_t x, int32_t z) {
    return static_cast<WgNoiseChunk*>(user)->preliminary_surface_level(x, z);
  };
  inputs.biome_at_quart = [](void* user, int32_t quart_x, int32_t quart_y, int32_t quart_z) {
    return static_cast<WgNoiseChunk*>(user)->biome_at_quart(quart_x, quart_y, quart_z);
  };
  return inputs;
}

// A point query asks the six climate router entries at one quart-aligned block
// origin. The caches `cachedClimateSampler` adds are a *chunk* shortcut:
// `flat_cache` and `cache_2d` answer from the cell the point sits on (exact at
// a quart-aligned origin), and the other three markers already fall through for
// a bare point (`WgDensityGraph::compute`). So the marked graph is not what a
// point query needs, and building one per query paid the whole wrap walk, the
// cache arrays and every allocation again for each call — `MemoryWorld::
// biome_name_at`, under its world mutex, is one such call per natural-spawn
// position. A marker-transparent graph depends only on the seed, so one is kept
// per thread and reused. `PLUSMC_BIOME_POINT_GRAPH=pre` is the same binary's
// per-query arm.
struct PointClimateGraph {
  const WgOverworldGenerator* owner = nullptr;
  int64_t seed = 0;
  WgChunkCursor cursor;
  std::unique_ptr<WgDensityGraph> graph;
};

thread_local PointClimateGraph tls_point_climate_graph;
thread_local uint64_t tls_point_climate_graph_builds = 0;

} // namespace

uint64_t point_climate_graph_builds() {
  return tls_point_climate_graph_builds;
}

WgClimateTarget WgOverworldGenerator::climate_target_at_quart(int32_t quart_x, int32_t quart_y,
                                                              int32_t quart_z) const {
  const int32_t block_x = quart_x * 4;
  const int32_t block_z = quart_z * 4;
  static const bool kPerQueryGraph = [] {
    const char* text = std::getenv("PLUSMC_BIOME_POINT_GRAPH");
    return text != nullptr && std::strcmp(text, "pre") == 0;
  }();
  if (kPerQueryGraph) {
    // A one-cell chunk around the queried cell, so the wrapped graph's flat
    // caches cover exactly the point — the same shape `interpolated_density`
    // builds. This is the shape every query used before the per-thread graph.
    WgChunkCursor cursor = make_cursor(1, block_x, block_z, *table_);
    WgDensityGraph graph(state_, cursor, /*wrap_markers=*/true, *table_);
    ++tls_point_climate_graph_builds;
    return climate_target_at(graph, block_x, quart_y * 4, block_z);
  }
  PointClimateGraph& cached = tls_point_climate_graph;
  if (cached.owner != this || cached.seed != seed_) {
    cached.owner = this;
    cached.seed = seed_;
    cached.cursor = make_cursor(1, 0, 0, *table_);
    cached.graph =
        std::make_unique<WgDensityGraph>(state_, cached.cursor, /*wrap_markers=*/false, *table_);
    ++tls_point_climate_graph_builds;
  }
  return climate_target_at(*cached.graph, block_x, quart_y * 4, block_z);
}

const char* WgOverworldGenerator::biome_at_quart(int32_t quart_x, int32_t quart_y,
                                                 int32_t quart_z) const {
  // The point query uses the same dimension-aware source as the chunk fill;
  // asking `wg_select_biome` here would report an overworld biome in the Nether
  // and End (the spawn search and mob checks read this answer).
  WgClimateSampler sampler(state_, *table_);
  return sampler.biome_at_quart(quart_x, quart_y, quart_z);
}

namespace {

// `ChunkAccess.fillBiomesFromNoise`'s cell order and quart coordinates, shared
// by the two biome fills so the order cannot drift between them: sections
// bottom-up, then within a section x, then y, then z. `biome_at` answers one
// quart cell.
template <typename BiomeAt>
void fill_biome_cells(int32_t chunk_x, int32_t chunk_z, const WgDensityTable& table,
                      const char** out, BiomeAt&& biome_at) {
  // `PalettedContainer`'s biome resolution: 16 / 4 blocks per axis.
  constexpr int32_t kBiomeCellsPerSection = 4 * 4 * 4;
  const int32_t section_count = table.height / 16;
  for (int32_t section = 0; section < section_count; ++section) {
    const int32_t quart_min_y = (table.min_y / 16 + section) * 4;
    const size_t section_base = static_cast<size_t>(section) * kBiomeCellsPerSection;
    for (int32_t x = 0; x < 4; ++x) {
      for (int32_t y = 0; y < 4; ++y) {
        for (int32_t z = 0; z < 4; ++z) {
          out[section_base + static_cast<size_t>((y * 4 + z) * 4 + x)] =
              biome_at(chunk_x * 4 + x, quart_min_y + y, chunk_z * 4 + z);
        }
      }
    }
  }
}

} // namespace

void WgOverworldGenerator::fill_biome_chunk(int32_t chunk_x, int32_t chunk_z,
                                            const char** out) const {
  // The chunk's own caches, as the game's biome fill gets them: one wrapped
  // graph whose flat caches cover this chunk's quart lattice, and one sample
  // per cell through it.
  WgChunkCursor cursor = make_cursor(16 / table_->cell_width, chunk_x * 16, chunk_z * 16, *table_);
  WgDensityGraph graph(state_, cursor, /*wrap_markers=*/true, *table_);
  fill_biome_cells(chunk_x, chunk_z, *table_, out,
                   [&](int32_t quart_x, int32_t quart_y, int32_t quart_z) {
                     // The dimension's own source: multi-noise for the overworld/Nether, the
                     // End island classifier for the End. The overworld selector here would put
                     // `minecraft:river` and its feature list into a Nether or End column.
                     return dimension_biome_at(graph, *table_, quart_x, quart_y, quart_z);
                   });
}

void WgOverworldGenerator::fill_biomes_with(WgNoiseChunk& chunk, int32_t chunk_x, int32_t chunk_z,
                                            const char** out) const {
  fill_biome_cells(chunk_x, chunk_z, *table_, out,
                   [&](int32_t quart_x, int32_t quart_y, int32_t quart_z) {
                     return chunk.biome_at_quart(quart_x, quart_y, quart_z);
                   });
}

void WgOverworldGenerator::fill_carved_chunk_with_biomes(int32_t chunk_x, int32_t chunk_z,
                                                         uint8_t* out, const char** biomes) const {
  // The same carry as `fill_carved_chunk`, extended by one stage: the biome
  // fill's 1,536 climate samples read the chunk's own guarded graph instead of
  // wrapping a third one over the same cursor. A/B arm
  // `PLUSMC_BIOME_CHUNK_REUSE=off` is the separate graph, exactly as
  // `fill_biome_chunk` builds it.
  static const bool kReuseBiomeChunk = [] {
    const char* text = std::getenv("PLUSMC_BIOME_CHUNK_REUSE");
    return text == nullptr || std::strcmp(text, "off") != 0;
  }();
  static const bool kCarrySurfaceChunk = [] {
    const char* text = std::getenv("PLUSMC_CARVE_CHUNK_REUSE");
    return text == nullptr || std::strcmp(text, "off") != 0;
  }();
  WgNoiseChunk surface_chunk(state_, 16 / table_->cell_width, chunk_x * 16, chunk_z * 16, *table_);
  fill_surface_with(surface_chunk, chunk_x, chunk_z, out);
  if (kCarrySurfaceChunk) {
    carve_with(surface_chunk, chunk_x, chunk_z, out);
  } else {
    WgNoiseChunk carve_chunk(state_, 16 / table_->cell_width, chunk_x * 16, chunk_z * 16, *table_);
    carve_with(carve_chunk, chunk_x, chunk_z, out);
  }
  // The biome samples go through the surface stage's chunk either way, so the
  // biome arm is independent of the carve arm.
  if (kReuseBiomeChunk) {
    fill_biomes_with(surface_chunk, chunk_x, chunk_z, biomes);
    return;
  }
  fill_biome_chunk(chunk_x, chunk_z, biomes);
}

void WgOverworldGenerator::fill_surface_with(WgNoiseChunk& chunk, int32_t chunk_x, int32_t chunk_z,
                                             uint8_t* out) const {
  // The noise stage first — its ten codes are the first ten codes of the
  // surface stage's own space — then the surface pass over the same array.
  chunk.fill_chunk(out);
  // The End's surface rule is the noise stage's own default block; there is no
  // material rewrite to apply in this code space.
  if (table_->dimension == WgDimensionKind::kEnd)
    return;
  const WgSurfaceChunk inputs = surface_chunk_inputs(chunk_x, chunk_z, out, chunk, *table_);
  // One tree per generator: a column otherwise builds it here and again for
  // the carver's re-top. A/B arm `PLUSMC_SURFACE_RULES_CACHE=off` builds both
  // per column, which is what every caller did before it was shareable.
  static const bool kCacheRules = [] {
    const char* text = std::getenv("PLUSMC_SURFACE_RULES_CACHE");
    return text == nullptr || std::strcmp(text, "off") != 0;
  }();
  const std::shared_ptr<const WgSurfaceRuleTree> rules = kCacheRules ? surface_rules() : nullptr;
  if (table_->dimension == WgDimensionKind::kNether)
    wg_apply_nether_surface(state_, seed_, inputs, rules.get());
  else
    wg_apply_overworld_surface(state_, seed_, inputs, rules.get());
}

void WgOverworldGenerator::fill_surface_chunk(int32_t chunk_x, int32_t chunk_z,
                                              uint8_t* out) const {
  WgNoiseChunk chunk(state_, 16 / table_->cell_width, chunk_x * 16, chunk_z * 16, *table_);
  fill_surface_with(chunk, chunk_x, chunk_z, out);
}

void WgOverworldGenerator::carve_with(WgNoiseChunk& chunk, int32_t chunk_x, int32_t chunk_z,
                                      uint8_t* out) const {
  // The re-top the carver asks for when it opens a grass ceiling is the surface
  // rule tree over this same array, so the context points at the cells the
  // carver is rewriting; it is built once per chunk, not once per cell.
  const WgSurfaceChunk surface = surface_chunk_inputs(chunk_x, chunk_z, out, chunk, *table_);
  static const bool kCacheRules = [] {
    const char* text = std::getenv("PLUSMC_SURFACE_RULES_CACHE");
    return text == nullptr || std::strcmp(text, "off") != 0;
  }();
  const std::shared_ptr<const WgSurfaceRuleTree> rules = kCacheRules ? surface_rules() : nullptr;
  WgSurfaceTopMaterial top_material(state_, seed_, surface, rules.get());
  WgCarverChunk inputs;
  inputs.chunk_x = chunk_x;
  inputs.chunk_z = chunk_z;
  inputs.min_y = table_->min_y;
  inputs.height = table_->height;
  inputs.codes = out;
  inputs.aquifer = &chunk.aquifer();
  inputs.user = &chunk;
  inputs.biome_at_quart = surface.biome_at_quart;
  inputs.top_material_user = &top_material;
  inputs.top_material = [](void* user, int32_t block_x, int32_t block_y, int32_t block_z,
                           bool under_fluid, uint8_t* code) {
    return static_cast<WgSurfaceTopMaterial*>(user)->at(block_x, block_y, block_z, under_fluid,
                                                        code);
  };
  wg_apply_carvers(seed_, inputs);
}

void WgOverworldGenerator::fill_carved_chunk(int32_t chunk_x, int32_t chunk_z, uint8_t* out) const {
  // `buildSurface`, then `applyCarvers`, over one `NoiseChunk` — the game's own
  // shape (`applyCarvers` reads `noiseChunk.aquifer()` on the chunk
  // `buildSurface` just wrote). What the carry saves is mostly the second
  // chunk's construction: the router is wrapped (its flat caches fill eagerly)
  // and the aquifer's lattice is allocated for a column whose fill and surface
  // pass have already done both. Its caches are warm where the fill asked the
  // aquifer (density <= 0: the air and fluid column), so those shallow lattice
  // cells and surface-level samples are not re-derived; the deep cells a carver
  // rewrites are new work on either arm. Every read either stage makes is pure,
  // and the few stateful markers fall through for a point context
  // (worldgen_density.h), so the carried chunk answers exactly what a fresh one
  // would.
  //
  // The A/B arm (`PLUSMC_CARVE_CHUNK_REUSE=off`) builds the second chunk this
  // entry point used to build, on the same binary — the "before" of
  // docs/PHASE5.md's carve-carry section.
  static const bool kCarrySurfaceChunk = [] {
    const char* text = std::getenv("PLUSMC_CARVE_CHUNK_REUSE");
    return text == nullptr || std::strcmp(text, "off") != 0;
  }();
  WgNoiseChunk surface_chunk(state_, 16 / table_->cell_width, chunk_x * 16, chunk_z * 16, *table_);
  fill_surface_with(surface_chunk, chunk_x, chunk_z, out);
  if (kCarrySurfaceChunk) {
    carve_with(surface_chunk, chunk_x, chunk_z, out);
    return;
  }
  WgNoiseChunk carve_chunk(state_, 16 / table_->cell_width, chunk_x * 16, chunk_z * 16, *table_);
  carve_with(carve_chunk, chunk_x, chunk_z, out);
}

double WgOverworldGenerator::interpolated_density(int32_t block_x, int32_t block_y,
                                                  int32_t block_z) const {
  if (block_y < table_->min_y || block_y >= table_->min_y + table_->height) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  WgNoiseChunk chunk(state_, 1, block_x - wg_floor_mod(block_x, table_->cell_width),
                     block_z - wg_floor_mod(block_z, table_->cell_width), *table_);
  WgDensityGraph& graph = chunk.graph();
  graph.initialize_for_first_cell_x();
  graph.advance_cell_x(0);
  graph.select_cell_yz(wg_floor_div(block_y - table_->min_y, table_->cell_height), 0);
  graph.update_for_y(
      block_y, static_cast<double>(wg_floor_mod(block_y - table_->min_y, table_->cell_height)) /
                   table_->cell_height);
  graph.update_for_x(block_x, static_cast<double>(wg_floor_mod(block_x, table_->cell_width)) /
                                  table_->cell_width);
  graph.update_for_z(block_z, static_cast<double>(wg_floor_mod(block_z, table_->cell_width)) /
                                  table_->cell_width);
  return graph.interpolated_density();
}

double WgOverworldGenerator::node_value(uint32_t table_node, int32_t x, int32_t y,
                                        int32_t z) const {
  // The graph as `RandomState` hands it out: no interpolation wrappers, so the
  // markers are transparent and the answer is the plain algebra at one point.
  WgChunkCursor cursor = make_cursor(1, x, z, *table_);
  WgDensityGraph graph(state_, cursor, /*wrap_markers=*/false, *table_);
  return graph.compute(graph.wrapped(table_node), WgDfContext::point(x, y, z));
}

double WgOverworldGenerator::router_value(WgRouterField field, int32_t x, int32_t y,
                                          int32_t z) const {
  return node_value(kWgRouter[static_cast<size_t>(field)], x, y, z);
}

} // namespace mc::world
