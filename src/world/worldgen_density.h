// SPDX-License-Identifier: GPL-3.0-or-later
//
// The density-function evaluator: the generated table's 330 nodes, wrapped the
// way `NoiseChunk.wrapNew` wraps them, and evaluated with the same caches and
// the same interpolation cursor.
//
// The wrapping is where the faithfulness lives. `NoiseChunk` does not evaluate
// the router as written; it rewrites it first:
//
//   interpolated  -> an interpolator that samples its child on a
//                    (cellCountXZ+1) x (cellCountY+1) lattice and lerps between
//   flat_cache    -> an eagerly filled (noiseSizeXZ+1)^2 grid sampled at y = 0
//   cache_2d      -> a one-entry (x, z) memo
//   cache_once    -> a memo keyed on the interpolation counter, plus a whole
//                    filled array keyed on the *array* counter
//   cache_all_in_cell -> the 4 x 4 x 8 cell of values the fill loop reads back
//   blend_density -> the child itself (the blender is empty for a plain world)
//   blend_alpha / blend_offset -> the constants 1.0 and 0.0
//   beardifier    -> 0.0 (no structures in the noise stage)
//
// The other half is the *context*: a wrapped node answers from its cache only
// when it is asked with the chunk itself as the context, and falls straight
// through to its child for a bare `SinglePointContext` — which is how the
// aquifer's own noise reads stay uncached. `WgDfContext::in_chunk` is that
// distinction.
#pragma once

#include <cstdint>
#include <vector>

#include "world/worldgen_dimension.h"
#include "world/worldgen_math.h"
#include "world/worldgen_state.h"
#include "world/worldgen_table.h"

namespace mc::world {

// One evaluation point: the chunk (with its cursor) or a bare point.
struct WgDfContext {
  int32_t x = 0;
  int32_t y = 0;
  int32_t z = 0;
  bool in_chunk = false;

  static WgDfContext chunk(int32_t x, int32_t y, int32_t z) {
    return {x, y, z, true};
  }
  static WgDfContext point(int32_t x, int32_t y, int32_t z) {
    return {x, y, z, false};
  }
};

// The interpolation cursor: `NoiseChunk`'s mutable fields, in one struct.
struct WgChunkCursor {
  int32_t cell_width = kWgCellWidth;
  int32_t cell_height = kWgCellHeight;
  int32_t cell_count_xz = 16 / kWgCellWidth;
  int32_t cell_count_y = kWgHeight / kWgCellHeight;
  int32_t cell_noise_min_y = kWgMinY / kWgCellHeight;
  int32_t noise_size_xz = 4;
  int32_t first_noise_x = 0;
  int32_t first_noise_z = 0;
  int32_t first_cell_x = 0;
  int32_t first_cell_z = 0;
  int32_t cell_start_block_x = 0;
  int32_t cell_start_block_y = 0;
  int32_t cell_start_block_z = 0;
  int32_t in_cell_x = 0;
  int32_t in_cell_y = 0;
  int32_t in_cell_z = 0;
  int32_t array_index = 0;
  bool interpolating = false;
  bool filling_cell = false;
  int64_t interpolation_counter = 0;
  int64_t array_interpolation_counter = 0;
};

// A wrapped node. The `i0..i3`/`d0..d1` fields keep the table's meaning; the
// node-typed ones hold *wrapped* indices instead of table indices, and `slot`
// indexes the matching state vector for the five stateful markers.
struct WgWrappedDf {
  WgDfKind kind = WgDfKind::kConstant;
  int32_t i0 = 0;
  int32_t i1 = 0;
  int32_t i2 = 0;
  int32_t i3 = 0;
  double d0 = 0.0;
  double d1 = 0.0;
  int32_t slot = -1;
};

class WgDensityGraph {
public:
  WgDensityGraph(const WgRandomState& state, WgChunkCursor& cursor);
  // `wrap_markers=false` is the router as `RandomState` hands it out, before
  // `NoiseChunk` rewrites it: every marker is transparent. It is what a plain
  // `router().<field>.compute(point)` sees, and what the dump's router rows pin.
  WgDensityGraph(const WgRandomState& state, WgChunkCursor& cursor, bool wrap_markers);
  WgDensityGraph(const WgRandomState& state, WgChunkCursor& cursor, bool wrap_markers,
                 const WgDensityTable& table);

  // `NoiseChunk.wrap`: the wrapped node for a table node, memoised, so two
  // references to the same named function share one wrapper (and one cache).
  int32_t wrapped(uint32_t table_node);

  const WgWrappedDf& node(int32_t index) const {
    return nodes_[static_cast<size_t>(index)];
  }

  double compute(int32_t index, const WgDfContext& context);

  // The fill loop's own drivers. `fill_slice` is `NoiseChunk.fillSlice`,
  // `select_cell_yz` / `update_for_*` / `swap_slices` are their namesakes.
  void initialize_for_first_cell_x();
  void advance_cell_x(int32_t cell_x_index);
  void select_cell_yz(int32_t cell_y_index, int32_t cell_z_index);
  void update_for_y(int32_t pos_y, double factor_y);
  void update_for_x(int32_t pos_x, double factor_x);
  void update_for_z(int32_t pos_z, double factor_z);
  void swap_slices();
  void stop_interpolation();

  // `NoiseBasedChunkGenerator.getInterpolatedNoiseValue`'s read: the density of
  // the cell the cursor currently sits in.
  double interpolated_density() {
    return compute(noise_density_root_, chunk_context());
  }

  // `NoiseChunk` itself as a `DensityFunction.FunctionContext` — the context
  // every filler in `getInterpolatedState`'s rule list is handed. Public so a
  // filler that lives outside this class (the ore veinifier) can pass the same
  // one the aquifer's own reads are given.
  WgDfContext chunk_context() const;

  void set_noise_density_root(int32_t root) {
    noise_density_root_ = root;
  }

  size_t interpolator_count() const {
    return interpolators_.size();
  }
  size_t flat_cache_count() const {
    return flat_caches_.size();
  }
  size_t cache_2d_count() const {
    return cache_2d_.size();
  }
  size_t cache_once_count() const {
    return cache_once_.size();
  }
  size_t cell_cache_count() const {
    return cell_caches_.size();
  }
  // What `wrap_new` folded away: nodes that became a constant, and constant
  // operands absorbed into an add/mul/min/max node (so `compute` reads the value
  // instead of recursing into the constant). The worldgen test pins that a real
  // overworld graph folds something; the digest fixtures pin that the values do
  // not change.
  size_t folded_constants() const {
    return folded_constants_;
  }
  size_t folded_operands() const {
    return folded_operands_;
  }
  const WgChunkCursor& cursor() const {
    return cursor_;
  }
  const WgDensityTable& table() const {
    return *table_;
  }
  WgChunkCursor& cursor() {
    return cursor_;
  }

private:
  struct Interpolator {
    int32_t filler = 0;
    int32_t row_stride = 1; // cellCountY + 1: the y length of one z slice
    std::vector<double> slice0;
    std::vector<double> slice1;
    double noise000 = 0, noise001 = 0, noise100 = 0, noise101 = 0;
    double noise010 = 0, noise011 = 0, noise110 = 0, noise111 = 0;
    double value_xz00 = 0, value_xz10 = 0, value_xz01 = 0, value_xz11 = 0;
    double value_z0 = 0, value_z1 = 0, value = 0;

    double at(const std::vector<double>& slice, int32_t cell_z_index, int32_t cell_y_index) const {
      return slice[static_cast<size_t>(cell_z_index) * static_cast<size_t>(row_stride) +
                   static_cast<size_t>(cell_y_index)];
    }
    void set(std::vector<double>& slice, int32_t cell_z_index, int32_t cell_y_index, double value) {
      slice[static_cast<size_t>(cell_z_index) * static_cast<size_t>(row_stride) +
            static_cast<size_t>(cell_y_index)] = value;
    }
  };

  struct FlatCache {
    int32_t filler = 0;
    int32_t size_xz = 0;
    std::vector<double> values;
  };

  struct Cache2D {
    int32_t filler = 0;
    int64_t last_pos_2d = 0;
    double last_value = 0.0;
  };

  struct CacheOnce {
    int32_t filler = 0;
    int64_t last_counter = 0;
    int64_t last_array_counter = 0;
    double last_value = 0.0;
    std::vector<double> last_array;
    bool has_array = false;
  };

  struct CellCache {
    int32_t filler = 0;
    std::vector<double> values;
  };

  int32_t wrap_new(uint32_t table_node);
  // Constant folding for `wrap_new`, run after the children are wrapped and
  // before the node is pushed: see worldgen_density.cpp for what it may do and
  // why each rewrite is bit-identical to `compute`'s expression.
  void fold_constants(WgWrappedDf* node);
  // `CubicSpline.mapCoordinates(c -> c.mapChildren(visitor))`: a spline's
  // coordinate functions are part of the wrapped graph, so they are wrapped
  // when the spline node is, not lazily during an evaluation (the graph must
  // not grow while something is walking it).
  void wrap_spline(uint32_t spline_index);
  void fill_slice(bool slice0, int32_t cell_x);
  double eval_spline(uint32_t spline_index, const WgDfContext& context);
  float sample_spline_point(const WgSpline& spline, uint32_t point_index,
                            const WgDfContext& context);

  // `NoiseChunk.fillAllDirectly` is *two* methods with the same name: the
  // slice provider walks one column of cells (cellCountY + 1 entries), the
  // chunk provider walks the whole 4 x 4 x 8 cell. Both are mirrored here.
  void fill_slice_directly(int32_t index, double* output, int length);
  void fill_cell_directly(int32_t index, double* output, int length);
  void fill_array(int32_t index, double* output, int length, bool slice_provider);

  const WgRandomState& state_;
  WgChunkCursor& cursor_;
  bool wrap_markers_ = true;
  const WgDensityTable* table_ = nullptr;
  std::vector<WgWrappedDf> nodes_;
  std::vector<int32_t> wrap_map_;
  std::vector<Interpolator> interpolators_;
  std::vector<FlatCache> flat_caches_;
  std::vector<Cache2D> cache_2d_;
  std::vector<CacheOnce> cache_once_;
  std::vector<CellCache> cell_caches_;
  std::vector<int32_t> spline_coordinates_;
  std::vector<uint8_t> splines_wrapped_;
  size_t folded_constants_ = 0;
  size_t folded_operands_ = 0;
  int32_t noise_density_root_ = -1;
};

} // namespace mc::world
