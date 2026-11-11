// SPDX-License-Identifier: GPL-3.0-or-later
//
// See `worldgen_density.h` for the shape of the wrapping. The two things this
// file is most careful about are:
//
//   1. Evaluation order. `add`/`mul`/`min`/`max` evaluate their left argument
//      first (and `mul` returns early on a zero left argument, exactly as
//      `Ap2` does); `shifted_noise` computes x, then y, then z; a runtime of
//      pure functions means order only shows up in the fixture values, which
//      is exactly why the port keeps it.
//   2. Which context a node is asked in. A wrapped node answers from its cache
//      only for the chunk's own context; anything holding a bare point walks
//      through. `flat_cache` and `cache_2d` are the exceptions — vanilla gives
//      them no such check, so neither does this.
//
// `Mth.binarySearch` and `Mth.lerp`'s float overload are reproduced literally:
// the spline sampler runs entirely in binary32, including the coordinate, and
// the fixtures pin it.

#include "world/worldgen_density.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "world/worldgen_noise.h"

namespace mc::world {
namespace {

// `DensityFunctions.EndIslandDensityFunction.getHeightValue` (26.2 bytecode):
// the central-island falloff plus the twelve-by-twelve neighbourhood of small
// islands, all in binary32 except the simplex lookup itself.
float end_island_height(const WgSimplexNoise& noise, int32_t x, int32_t z) {
  const int32_t half_x = x / 2;
  const int32_t half_z = z / 2;
  const int32_t parity_x = x % 2;
  const int32_t parity_z = z % 2;
  const int32_t distance_sq =
      static_cast<int32_t>(static_cast<uint32_t>(x) * static_cast<uint32_t>(x) +
                           static_cast<uint32_t>(z) * static_cast<uint32_t>(z));
  const float sqrt_distance = static_cast<float>(std::sqrt(static_cast<double>(distance_sq)));
  float height = 100.0f - sqrt_distance * 8.0f;
  height = std::min(80.0f, std::max(-100.0f, height));

  for (int32_t dx = -12; dx <= 12; ++dx) {
    for (int32_t dz = -12; dz <= 12; ++dz) {
      const int64_t island_x = static_cast<int64_t>(half_x) + dx;
      const int64_t island_z = static_cast<int64_t>(half_z) + dz;
      if (island_x * island_x + island_z * island_z <= 4096LL)
        continue;
      if (!(noise.get_value(static_cast<double>(island_x), static_cast<double>(island_z)) <
            -0.8999999761581421))
        continue;
      const float shape = std::fmod(std::fabs(static_cast<float>(island_x)) * 3439.0f +
                                        std::fabs(static_cast<float>(island_z)) * 147.0f,
                                    13.0f) +
                          9.0f;
      const float offset_x = static_cast<float>(parity_x - dx * 2);
      const float offset_z = static_cast<float>(parity_z - dz * 2);
      float island_height =
          100.0f - static_cast<float>(
                       std::sqrt(static_cast<double>(offset_x * offset_x + offset_z * offset_z))) *
                       shape;
      island_height = std::min(80.0f, std::max(-100.0f, island_height));
      height = std::max(height, island_height);
    }
  }
  return height;
}

// `Mth.lerp(float, float, float)` — the spline sampler's own arithmetic.
inline float float_lerp(float delta, float start, float end) {
  return start + delta * (end - start);
}

// `ChunkPos.INVALID_CHUNK_POS = ChunkPos.pack(1875066, 1875066)`.
constexpr int64_t kInvalidChunkPos =
    (1875066LL & 0xFFFFFFFFLL) | ((1875066LL & 0xFFFFFFFFLL) << 32);

inline int64_t pack_pos_2d(int32_t x, int32_t z) {
  return (static_cast<int64_t>(x) & 0xFFFFFFFFLL) |
         ((static_cast<int64_t>(z) & 0xFFFFFFFFLL) << 32);
}

} // namespace

WgDensityGraph::WgDensityGraph(const WgRandomState& state, WgChunkCursor& cursor)
    : WgDensityGraph(state, cursor, /*wrap_markers=*/true) {}

WgDensityGraph::WgDensityGraph(const WgRandomState& state, WgChunkCursor& cursor, bool wrap_markers)
    : WgDensityGraph(state, cursor, wrap_markers, wg_overworld_table()) {}

WgDensityGraph::WgDensityGraph(const WgRandomState& state, WgChunkCursor& cursor, bool wrap_markers,
                               const WgDensityTable& table)
    : state_(state), cursor_(cursor), wrap_markers_(wrap_markers), table_(&table) {
  wrap_map_.assign(table.df_count, -1);
  // `wrapped()` memoises one wrapper per table node, so this is the most entries
  // `nodes_` can ever hold. Reserving it up front is a correctness property of
  // `compute`, not just an allocation saving: `compute` holds a reference into
  // `nodes_` across recursive calls, and a child evaluation can wrap a lazily
  // reached node (`kIntervalSelect`), which pushes. With the capacity already
  // the table's own size that push never relocates, so the reference stays
  // valid. Do not shrink this below `kWgDfs`'s count.
  nodes_.reserve(table.df_count);
  spline_coordinates_.assign(table.spline_count, -1);
  splines_wrapped_.assign(table.spline_count, 0);
}

void WgDensityGraph::wrap_spline(uint32_t spline_index) {
  if (spline_index >= splines_wrapped_.size())
    return;
  if (splines_wrapped_[spline_index] != 0)
    return;
  splines_wrapped_[spline_index] = 1;
  const WgSpline& spline = table_->splines[spline_index];
  if (spline.is_constant)
    return;
  spline_coordinates_[spline_index] = wrapped(spline.coordinate);
  for (uint32_t i = 0; i < spline.point_count; ++i) {
    const WgSplinePoint& point = table_->spline_points[spline.point_offset + i];
    if (point.spline >= 0)
      wrap_spline(static_cast<uint32_t>(point.spline));
  }
}

int32_t WgDensityGraph::wrapped(uint32_t table_node) {
  if (table_node >= wrap_map_.size())
    return 0;
  if (wrap_map_[table_node] >= 0)
    return wrap_map_[table_node];
  const int32_t index = wrap_new(table_node);
  wrap_map_[table_node] = index;
  return index;
}

int32_t WgDensityGraph::wrap_new(uint32_t table_node) {
  const WgDf& df = table_->dfs[table_node];
  WgWrappedDf node;
  // In raw mode every marker is transparent — which is exactly what
  // `blend_density` already means — so the child rewiring below covers both.
  node.kind = df.kind;
  if (!wrap_markers_) {
    switch (df.kind) {
    case WgDfKind::kInterpolated:
    case WgDfKind::kFlatCache:
    case WgDfKind::kCache2D:
    case WgDfKind::kCacheOnce:
    case WgDfKind::kCacheAllInCell:
      node.kind = WgDfKind::kBlendDensity;
      break;
    default:
      break;
    }
  }
  node.i0 = df.i0;
  node.i1 = df.i1;
  node.i2 = df.i2;
  node.i3 = df.i3;
  node.d0 = df.d0;
  node.d1 = df.d1;

  // `BlendDensity` is a pure forward: the blender's alpha and offset are
  // constants (the two markers above), so the wrapper's value is exactly its
  // child's. Giving it a wrapper node of its own costs one recursive `compute`
  // call for every cell of every column and computes nothing; answering with
  // the child's own index removes it from the graph instead. (Only the real
  // `blend_density` kind is skipped here — the raw-mode rewrite of the cache
  // kinds to `kBlendDensity` still wants its own node, because `fill_array`
  // fills those arrays directly rather than recursing.)
  if (df.kind == WgDfKind::kBlendDensity)
    return wrapped(static_cast<uint32_t>(df.i0));

  switch (node.kind) {
  // The blender is empty for a plain world, so the two blend markers become
  // the constants `DensityFunctions.BlendAlpha.INSTANCE` / `BlendOffset`
  // already are, and `blend_density` is simply its child.
  case WgDfKind::kBlendAlpha:
    node.kind = WgDfKind::kConstant;
    node.d0 = 1.0;
    break;
  case WgDfKind::kBlendOffset:
  case WgDfKind::kBeardifier:
    node.kind = WgDfKind::kConstant;
    node.d0 = 0.0;
    break;

  case WgDfKind::kInterpolated: {
    Interpolator interpolator;
    interpolator.filler = wrapped(static_cast<uint32_t>(df.i0));
    interpolator.row_stride = cursor_.cell_count_y + 1;
    const size_t cells = static_cast<size_t>(cursor_.cell_count_xz + 1) *
                         static_cast<size_t>(interpolator.row_stride);
    interpolator.slice0.assign(cells, 0.0);
    interpolator.slice1.assign(cells, 0.0);
    node.i0 = interpolator.filler;
    node.slot = static_cast<int32_t>(interpolators_.size());
    interpolators_.push_back(std::move(interpolator));
    break;
  }

  case WgDfKind::kFlatCache: {
    FlatCache cache;
    cache.filler = wrapped(static_cast<uint32_t>(df.i0));
    cache.size_xz = cursor_.noise_size_xz + 1;
    cache.values.assign(static_cast<size_t>(cache.size_xz) * cache.size_xz, 0.0);
    // `new FlatCache(this, wrapped, fill=true)`: filled right there, at the
    // quart lattice, from a y = 0 single point.
    for (int32_t x = 0; x <= cursor_.noise_size_xz; ++x) {
      const int32_t block_x = (cursor_.first_noise_x + x) * 4;
      for (int32_t z = 0; z <= cursor_.noise_size_xz; ++z) {
        const int32_t block_z = (cursor_.first_noise_z + z) * 4;
        cache.values[static_cast<size_t>(x) + static_cast<size_t>(z) * cache.size_xz] =
            compute(cache.filler, WgDfContext::point(block_x, 0, block_z));
      }
    }
    node.i0 = cache.filler;
    node.slot = static_cast<int32_t>(flat_caches_.size());
    flat_caches_.push_back(std::move(cache));
    break;
  }

  case WgDfKind::kCache2D: {
    Cache2D cache;
    cache.filler = wrapped(static_cast<uint32_t>(df.i0));
    cache.last_pos_2d = kInvalidChunkPos;
    cache.last_value = 0.0;
    node.i0 = cache.filler;
    node.slot = static_cast<int32_t>(cache_2d_.size());
    cache_2d_.push_back(cache);
    break;
  }

  case WgDfKind::kCacheOnce: {
    CacheOnce cache;
    cache.filler = wrapped(static_cast<uint32_t>(df.i0));
    node.i0 = cache.filler;
    node.slot = static_cast<int32_t>(cache_once_.size());
    cache_once_.push_back(std::move(cache));
    break;
  }

  case WgDfKind::kCacheAllInCell: {
    CellCache cache;
    cache.filler = wrapped(static_cast<uint32_t>(df.i0));
    cache.values.assign(
        static_cast<size_t>(cursor_.cell_width) * cursor_.cell_width * cursor_.cell_height, 0.0);
    node.i0 = cache.filler;
    node.slot = static_cast<int32_t>(cell_caches_.size());
    cell_caches_.push_back(std::move(cache));
    break;
  }

  case WgDfKind::kShiftedNoise:
    node.i0 = wrapped(static_cast<uint32_t>(df.i0));
    node.i1 = wrapped(static_cast<uint32_t>(df.i1));
    node.i2 = wrapped(static_cast<uint32_t>(df.i2));
    break;

  case WgDfKind::kRangeChoice:
    node.i0 = wrapped(static_cast<uint32_t>(df.i0));
    node.i1 = wrapped(static_cast<uint32_t>(df.i1));
    node.i2 = wrapped(static_cast<uint32_t>(df.i2));
    break;

  case WgDfKind::kIntervalSelect:
    node.i0 = wrapped(static_cast<uint32_t>(df.i0));
    break;

  case WgDfKind::kClamp:
  case WgDfKind::kAbs:
  case WgDfKind::kSquare:
  case WgDfKind::kCube:
  case WgDfKind::kHalfNegative:
  case WgDfKind::kQuarterNegative:
  case WgDfKind::kInvert:
  case WgDfKind::kSqueeze:
  case WgDfKind::kAdd:
  case WgDfKind::kMul:
  case WgDfKind::kMin:
  case WgDfKind::kMax:
  case WgDfKind::kBlendDensity:
    node.i0 = wrapped(static_cast<uint32_t>(df.i0));
    if (df.kind == WgDfKind::kAdd || df.kind == WgDfKind::kMul || df.kind == WgDfKind::kMin ||
        df.kind == WgDfKind::kMax) {
      node.i1 = wrapped(static_cast<uint32_t>(df.i1));
    }
    break;

  case WgDfKind::kFindTopSurface:
    node.i0 = wrapped(static_cast<uint32_t>(df.i0));
    node.i1 = wrapped(static_cast<uint32_t>(df.i1));
    break;

  case WgDfKind::kSpline:
    wrap_spline(static_cast<uint32_t>(df.i0));
    break;

  // Leaves: nothing to wrap.
  default:
    break;
  }

  fold_constants(&node);
  nodes_.push_back(node);
  return static_cast<int32_t>(nodes_.size()) - 1;
}

// Constant folding, the half that is provably bit-identical.
//
// The generated overworld graph has 79 `kConstant` nodes and (measured from the
// table) every `add`/`mul` that mentions one puts it in `i0`; a few dozen of the
// graph's arithmetic nodes are therefore `constant op something`. `compute`
// spends a switch and a recursive call fetching that constant for every block
// cell and every interpolation corner. Folding it into the node once removes
// that call without touching the arithmetic: the expression and the operand
// order are the ones `compute` would have evaluated.
//
// Two shapes:
//   * both operands constant -> the node becomes `kConstant` with the value
//     `compute` would have produced. `Mul`'s zero short-circuit is reproduced
//     exactly (zero times a NaN is zero here, as it is in `compute`, because the
//     right operand is never evaluated).
//   * one operand constant -> its value goes in `d1` and its index becomes -1
//     (no wrapped index is negative). `compute` reads `d1` for that side and
//     still evaluates the other in the same order.
// A node with no constant operand is left alone.
void WgDensityGraph::fold_constants(WgWrappedDf* node) {
  if (node->kind != WgDfKind::kAdd && node->kind != WgDfKind::kMul &&
      node->kind != WgDfKind::kMin && node->kind != WgDfKind::kMax)
    return;
  const bool left_constant = nodes_[static_cast<size_t>(node->i0)].kind == WgDfKind::kConstant;
  const bool right_constant = nodes_[static_cast<size_t>(node->i1)].kind == WgDfKind::kConstant;
  if (!left_constant && !right_constant)
    return;

  const auto literal = [&](int32_t index) { return nodes_[static_cast<size_t>(index)].d0; };

  if (node->kind == WgDfKind::kMul && left_constant && literal(node->i0) == 0.0) {
    // `Mul`: `if (v1 == 0.0) return 0.0;` before the right side is touched.
    node->kind = WgDfKind::kConstant;
    node->d0 = 0.0;
    node->d1 = 0.0;
    node->i0 = node->i1 = 0;
    ++folded_constants_;
    return;
  }

  if (left_constant && right_constant) {
    const double a = literal(node->i0);
    const double b = literal(node->i1);
    double value = 0.0;
    switch (node->kind) {
    case WgDfKind::kAdd:
      value = a + b;
      break;
    case WgDfKind::kMul:
      value = a * b; // `a != 0` is settled above
      break;
    case WgDfKind::kMin:
      value = wg_math_min(a, b);
      break;
    case WgDfKind::kMax:
      value = wg_math_max(a, b);
      break;
    default:
      return;
    }
    node->kind = WgDfKind::kConstant;
    node->d0 = value;
    node->d1 = 0.0;
    node->i0 = node->i1 = 0;
    ++folded_constants_;
    return;
  }

  if (left_constant) {
    node->d1 = literal(node->i0);
    node->i0 = -1;
  } else {
    node->d1 = literal(node->i1);
    node->i1 = -1;
  }
  ++folded_operands_;
}

WgDfContext WgDensityGraph::chunk_context() const {
  return WgDfContext::chunk(cursor_.cell_start_block_x + cursor_.in_cell_x,
                            cursor_.cell_start_block_y + cursor_.in_cell_y,
                            cursor_.cell_start_block_z + cursor_.in_cell_z);
}

float WgDensityGraph::sample_spline_point(const WgSpline& spline, uint32_t point_index,
                                          const WgDfContext& context) {
  const WgSplinePoint& point = table_->spline_points[spline.point_offset + point_index];
  if (point.spline >= 0) {
    return static_cast<float>(eval_spline(static_cast<uint32_t>(point.spline), context));
  }
  return point.constant;
}

double WgDensityGraph::eval_spline(uint32_t spline_index, const WgDfContext& context) {
  const WgSpline& spline = table_->splines[spline_index];
  if (spline.is_constant)
    return static_cast<double>(spline.constant);

  const WgSplinePoint* points = table_->spline_points + spline.point_offset;
  // `Coordinate.apply` is `(float)function.compute(context)` — the coordinate
  // is narrowed to binary32 before anything else happens.
  const float input = static_cast<float>(compute(spline_coordinates_[spline_index], context));

  // `Mth.binarySearch(0, locations.length, i -> input < locations[i]) - 1`.
  int32_t from = 0;
  int32_t len = static_cast<int32_t>(spline.point_count);
  while (len > 0) {
    const int32_t half = len / 2;
    const int32_t middle = from + half;
    if (input < points[middle].location) {
      len = half;
      continue;
    }
    from = middle + 1;
    len -= half + 1;
  }
  const int32_t start = from - 1;
  const int32_t last = static_cast<int32_t>(spline.point_count) - 1;

  const auto linear_extend = [&](int32_t index, float value) {
    const float derivative = points[index].derivative;
    if (derivative == 0.0f)
      return value;
    return value + derivative * (input - points[index].location);
  };

  if (start < 0) {
    return static_cast<double>(linear_extend(0, sample_spline_point(spline, 0, context)));
  }
  if (start == last) {
    return static_cast<double>(
        linear_extend(last, sample_spline_point(spline, static_cast<uint32_t>(last), context)));
  }
  const float x1 = points[start].location;
  const float x2 = points[start + 1].location;
  const float t = (input - x1) / (x2 - x1);
  const float d1 = points[start].derivative;
  const float d2 = points[start + 1].derivative;
  const float y1 = sample_spline_point(spline, static_cast<uint32_t>(start), context);
  const float y2 = sample_spline_point(spline, static_cast<uint32_t>(start + 1), context);
  const float a = d1 * (x2 - x1) - (y2 - y1);
  const float b = -d2 * (x2 - x1) + (y2 - y1);
  return static_cast<double>(float_lerp(t, y1, y2) + t * (1.0f - t) * float_lerp(t, a, b));
}

double WgDensityGraph::compute(int32_t index, const WgDfContext& context) {
  // A reference, not a copy: the constructor reserves `kWgDfs`'s count, which
  // is the most wrappers that can ever exist (one per table node, memoised in
  // `wrap_map_`), so a recursive child that wraps a new node cannot relocate
  // `nodes_`. This is the hottest function of a column: the density graph is
  // evaluated once per block cell and once per interpolation corner, and the
  // 48-byte copy here was pure overhead.
  const WgWrappedDf& node = nodes_[static_cast<size_t>(index)];
  switch (node.kind) {
  case WgDfKind::kConstant:
    return node.d0;

  case WgDfKind::kYClampedGradient:
    return wg_clamped_map(static_cast<double>(context.y), node.i0, node.i1, node.d0, node.d1);

  case WgDfKind::kNoise:
    return state_.wired_noise(node.i0).get_value(static_cast<double>(context.x) * node.d0,
                                                 static_cast<double>(context.y) * node.d1,
                                                 static_cast<double>(context.z) * node.d0);

  case WgDfKind::kShiftedNoise: {
    const double x = static_cast<double>(context.x) * node.d0 + compute(node.i0, context);
    const double y = static_cast<double>(context.y) * node.d1 + compute(node.i1, context);
    const double z = static_cast<double>(context.z) * node.d0 + compute(node.i2, context);
    return state_.wired_noise(node.i3).get_value(x, y, z);
  }

  case WgDfKind::kShiftA:
    return state_.wired_noise(node.i0).get_value(static_cast<double>(context.x) * 0.25, 0.0,
                                                 static_cast<double>(context.z) * 0.25) *
           4.0;

  case WgDfKind::kShiftB:
    return state_.wired_noise(node.i0).get_value(static_cast<double>(context.z) * 0.25,
                                                 static_cast<double>(context.x) * 0.25, 0.0) *
           4.0;

  case WgDfKind::kShift:
    return state_.wired_noise(node.i0).get_value(static_cast<double>(context.x) * 0.25,
                                                 static_cast<double>(context.y) * 0.25,
                                                 static_cast<double>(context.z) * 0.25) *
           4.0;

  case WgDfKind::kRangeChoice: {
    const double input = compute(node.i0, context);
    if (input >= node.d0 && input < node.d1)
      return compute(node.i1, context);
    return compute(node.i2, context);
  }

  case WgDfKind::kIntervalSelect: {
    // `IntervalSelect.compute(context, input)`: the first threshold the input
    // is under, else the last function. `i1`/`i2`/`i3` are the *table's* child
    // and threshold ranges (the wrapped node copies them as they are), so the
    // branch index goes through the wrapper memo like any other child.
    const double input = compute(node.i0, context);
    for (int32_t i = 0; i < node.i2 - 1; ++i) {
      if (input < table_->thresholds[node.i3 + i]) {
        return compute(wrapped(table_->children[node.i1 + i]), context);
      }
    }
    return compute(wrapped(table_->children[node.i1 + node.i2 - 1]), context);
  }

  case WgDfKind::kClamp:
    return wg_clamp(compute(node.i0, context), node.d0, node.d1);

  case WgDfKind::kAbs:
    return std::abs(compute(node.i0, context));

  case WgDfKind::kSquare: {
    const double v = compute(node.i0, context);
    return v * v;
  }

  case WgDfKind::kCube: {
    const double v = compute(node.i0, context);
    return v * v * v;
  }

  case WgDfKind::kHalfNegative: {
    const double v = compute(node.i0, context);
    return v > 0.0 ? v : v * 0.5;
  }

  case WgDfKind::kQuarterNegative: {
    const double v = compute(node.i0, context);
    return v > 0.0 ? v : v * 0.25;
  }

  case WgDfKind::kInvert:
    return 1.0 / compute(node.i0, context);

  case WgDfKind::kSqueeze: {
    const double v = compute(node.i0, context);
    const double c = wg_clamp(v, -1.0, 1.0);
    return c / 2.0 - c * c * c / 24.0;
  }

  // An index of -1 means `fold_constants` put that operand's value in `d1`
  // instead of wrapping it (no wrapped index is negative). The other operand
  // is still evaluated here, in the same order as before.
  case WgDfKind::kAdd: {
    const double v1 = node.i0 >= 0 ? compute(node.i0, context) : node.d1;
    const double v2 = node.i1 >= 0 ? compute(node.i1, context) : node.d1;
    return v1 + v2;
  }

  case WgDfKind::kMul: {
    const double v1 = node.i0 >= 0 ? compute(node.i0, context) : node.d1;
    if (v1 == 0.0)
      return 0.0; // `Ap2` skips the right argument entirely
    return v1 * (node.i1 >= 0 ? compute(node.i1, context) : node.d1);
  }

  // `Ap2` also short-circuits `min`/`max` against the right argument's
  // *static* min/max value, which a flat table does not carry. The two only
  // differ if the right argument evaluates to NaN, and `wg_math_min`/
  // `wg_math_max` propagate NaN the way Java's `Math.min`/`Math.max` do.
  case WgDfKind::kMin: {
    const double v1 = node.i0 >= 0 ? compute(node.i0, context) : node.d1;
    const double v2 = node.i1 >= 0 ? compute(node.i1, context) : node.d1;
    return wg_math_min(v1, v2);
  }

  case WgDfKind::kMax: {
    const double v1 = node.i0 >= 0 ? compute(node.i0, context) : node.d1;
    const double v2 = node.i1 >= 0 ? compute(node.i1, context) : node.d1;
    return wg_math_max(v1, v2);
  }

  case WgDfKind::kSpline:
    return eval_spline(static_cast<uint32_t>(node.i0), context);

  case WgDfKind::kFindTopSurface: {
    const double upper = compute(node.i1, context);
    const int32_t top_y = wg_floor(upper / static_cast<double>(node.i3)) * node.i3;
    if (top_y <= node.i2)
      return node.i2;
    for (int32_t block_y = top_y; block_y >= node.i2; block_y -= node.i3) {
      if (compute(node.i0, WgDfContext::point(context.x, block_y, context.z)) > 0.0) {
        return block_y;
      }
    }
    return node.i2;
  }

  case WgDfKind::kBlendedNoise:
    return state_.blended(table_->blended_base + node.i0).compute(context.x, context.y, context.z);

  case WgDfKind::kEndIslands:
    return (static_cast<double>(
                end_island_height(state_.end_island_noise(), context.x / 8, context.z / 8)) -
            8.0) /
           128.0;

  case WgDfKind::kInterpolated: {
    const Interpolator& interpolator = interpolators_[static_cast<size_t>(node.slot)];
    if (!context.in_chunk)
      return compute(interpolator.filler, context);
    if (cursor_.filling_cell) {
      return wg_lerp3(static_cast<double>(cursor_.in_cell_x) / cursor_.cell_width,
                      static_cast<double>(cursor_.in_cell_y) / cursor_.cell_height,
                      static_cast<double>(cursor_.in_cell_z) / cursor_.cell_width,
                      interpolator.noise000, interpolator.noise100, interpolator.noise010,
                      interpolator.noise110, interpolator.noise001, interpolator.noise101,
                      interpolator.noise011, interpolator.noise111);
    }
    return interpolator.value;
  }

  case WgDfKind::kFlatCache: {
    const FlatCache& cache = flat_caches_[static_cast<size_t>(node.slot)];
    const int32_t quart_x = context.x >> 2;
    const int32_t quart_z = context.z >> 2;
    const int32_t x = quart_x - cursor_.first_noise_x;
    const int32_t z = quart_z - cursor_.first_noise_z;
    if (x >= 0 && z >= 0 && x < cache.size_xz && z < cache.size_xz) {
      return cache.values[static_cast<size_t>(x) + static_cast<size_t>(z) * cache.size_xz];
    }
    return compute(cache.filler, context);
  }

  case WgDfKind::kCache2D: {
    Cache2D& cache = cache_2d_[static_cast<size_t>(node.slot)];
    const int64_t pos_2d = pack_pos_2d(context.x, context.z);
    if (cache.last_pos_2d == pos_2d)
      return cache.last_value;
    cache.last_pos_2d = pos_2d;
    cache.last_value = compute(cache.filler, context);
    return cache.last_value;
  }

  case WgDfKind::kCacheOnce: {
    CacheOnce& cache = cache_once_[static_cast<size_t>(node.slot)];
    if (!context.in_chunk)
      return compute(cache.filler, context);
    if (cache.has_array && cache.last_array_counter == cursor_.array_interpolation_counter) {
      return cache.last_array[static_cast<size_t>(cursor_.array_index)];
    }
    if (cache.last_counter == cursor_.interpolation_counter)
      return cache.last_value;
    cache.last_counter = cursor_.interpolation_counter;
    cache.last_value = compute(cache.filler, context);
    return cache.last_value;
  }

  case WgDfKind::kCacheAllInCell: {
    CellCache& cache = cell_caches_[static_cast<size_t>(node.slot)];
    if (!context.in_chunk)
      return compute(cache.filler, context);
    const int32_t x = cursor_.in_cell_x;
    const int32_t y = cursor_.in_cell_y;
    const int32_t z = cursor_.in_cell_z;
    if (x >= 0 && y >= 0 && z >= 0 && x < cursor_.cell_width && y < cursor_.cell_height &&
        z < cursor_.cell_width) {
      return cache
          .values[(static_cast<size_t>(cursor_.cell_height - 1 - y) * cursor_.cell_width + x) *
                      cursor_.cell_width +
                  z];
    }
    return compute(cache.filler, context);
  }

  case WgDfKind::kBlendDensity:
    return compute(node.i0, context);

  // `wrap_new` rewrites these three into constants, so reaching them here
  // would mean the wrapped graph was built some other way.
  case WgDfKind::kBlendAlpha:
    return 1.0;
  case WgDfKind::kBlendOffset:
  case WgDfKind::kBeardifier:
    return 0.0;
  }
  return 0.0;
}

void WgDensityGraph::fill_slice_directly(int32_t index, double* output, int length) {
  for (int32_t cell_y_index = 0; cell_y_index < cursor_.cell_count_y + 1; ++cell_y_index) {
    if (cell_y_index >= length)
      break;
    cursor_.cell_start_block_y = (cell_y_index + cursor_.cell_noise_min_y) * cursor_.cell_height;
    ++cursor_.interpolation_counter;
    cursor_.in_cell_y = 0;
    cursor_.array_index = cell_y_index;
    output[cell_y_index] = compute(index, chunk_context());
  }
}

void WgDensityGraph::fill_cell_directly(int32_t index, double* output, int length) {
  cursor_.array_index = 0;
  for (int32_t y_in_cell = cursor_.cell_height - 1; y_in_cell >= 0; --y_in_cell) {
    cursor_.in_cell_y = y_in_cell;
    for (int32_t x_in_cell = 0; x_in_cell < cursor_.cell_width; ++x_in_cell) {
      cursor_.in_cell_x = x_in_cell;
      for (int32_t z_in_cell = 0; z_in_cell < cursor_.cell_width; ++z_in_cell) {
        cursor_.in_cell_z = z_in_cell;
        // `output[this.arrayIndex++] = function.compute(this)`: Java evaluates
        // the index expression before the right-hand side, so the function
        // sees the counter already advanced.
        const int32_t write_index = cursor_.array_index++;
        if (write_index >= length)
          return;
        output[write_index] = compute(index, chunk_context());
      }
    }
  }
}

void WgDensityGraph::fill_array(int32_t index, double* output, int length, bool slice_provider) {
  const WgWrappedDf node = nodes_[static_cast<size_t>(index)];
  switch (node.kind) {
  case WgDfKind::kInterpolated:
    if (cursor_.filling_cell) {
      if (slice_provider) {
        fill_slice_directly(index, output, length);
      } else {
        fill_cell_directly(index, output, length);
      }
      return;
    }
    fill_array(node.i0, output, length, slice_provider);
    return;

  case WgDfKind::kFlatCache:
  case WgDfKind::kCacheAllInCell:
    if (slice_provider) {
      fill_slice_directly(index, output, length);
    } else {
      fill_cell_directly(index, output, length);
    }
    return;

  case WgDfKind::kCache2D:
    fill_array(node.i0, output, length, slice_provider);
    return;

  case WgDfKind::kCacheOnce: {
    CacheOnce& cache = cache_once_[static_cast<size_t>(node.slot)];
    if (cache.has_array && cache.last_array_counter == cursor_.array_interpolation_counter) {
      std::copy(cache.last_array.begin(), cache.last_array.begin() + length, output);
      return;
    }
    fill_array(node.i0, output, length, slice_provider);
    if (static_cast<int>(cache.last_array.size()) == length) {
      std::copy(output, output + length, cache.last_array.begin());
    } else {
      cache.last_array.assign(output, output + length);
    }
    cache.has_array = true;
    cache.last_array_counter = cursor_.array_interpolation_counter;
    return;
  }

  default:
    if (slice_provider) {
      fill_slice_directly(index, output, length);
    } else {
      fill_cell_directly(index, output, length);
    }
    return;
  }
}

void WgDensityGraph::fill_slice(bool slice0, int32_t cell_x) {
  cursor_.cell_start_block_x = cell_x * cursor_.cell_width;
  cursor_.in_cell_x = 0;
  for (int32_t cell_z_index = 0; cell_z_index < cursor_.cell_count_xz + 1; ++cell_z_index) {
    const int32_t cell_z = cursor_.first_cell_z + cell_z_index;
    cursor_.cell_start_block_z = cell_z * cursor_.cell_width;
    cursor_.in_cell_z = 0;
    ++cursor_.array_interpolation_counter;
    for (Interpolator& interpolator : interpolators_) {
      std::vector<double>& slice = slice0 ? interpolator.slice0 : interpolator.slice1;
      double* row = slice.data() + static_cast<size_t>(cell_z_index) * interpolator.row_stride;
      fill_array(interpolator.filler, row, interpolator.row_stride, /*slice_provider=*/true);
    }
  }
  ++cursor_.array_interpolation_counter;
}

void WgDensityGraph::initialize_for_first_cell_x() {
  cursor_.interpolating = true;
  cursor_.interpolation_counter = 0;
  fill_slice(true, cursor_.first_cell_x);
}

void WgDensityGraph::advance_cell_x(int32_t cell_x_index) {
  // An `INTERPOLATED` node reads `slice1` for the cell being entered while its
  // `slice0` still holds the previous boundary, so the next slice is filled
  // into `slice1` before the *current* cell's start X is restored.
  fill_slice(false, cursor_.first_cell_x + cell_x_index + 1);
  cursor_.cell_start_block_x = (cursor_.first_cell_x + cell_x_index) * cursor_.cell_width;
}

void WgDensityGraph::select_cell_yz(int32_t cell_y_index, int32_t cell_z_index) {
  for (Interpolator& interpolator : interpolators_) {
    interpolator.noise000 = interpolator.at(interpolator.slice0, cell_z_index, cell_y_index);
    interpolator.noise001 = interpolator.at(interpolator.slice0, cell_z_index + 1, cell_y_index);
    interpolator.noise100 = interpolator.at(interpolator.slice1, cell_z_index, cell_y_index);
    interpolator.noise101 = interpolator.at(interpolator.slice1, cell_z_index + 1, cell_y_index);
    interpolator.noise010 = interpolator.at(interpolator.slice0, cell_z_index, cell_y_index + 1);
    interpolator.noise011 =
        interpolator.at(interpolator.slice0, cell_z_index + 1, cell_y_index + 1);
    interpolator.noise110 = interpolator.at(interpolator.slice1, cell_z_index, cell_y_index + 1);
    interpolator.noise111 =
        interpolator.at(interpolator.slice1, cell_z_index + 1, cell_y_index + 1);
  }
  cursor_.filling_cell = true;
  cursor_.cell_start_block_y = (cell_y_index + cursor_.cell_noise_min_y) * cursor_.cell_height;
  cursor_.cell_start_block_z = (cursor_.first_cell_z + cell_z_index) * cursor_.cell_width;
  ++cursor_.array_interpolation_counter;
  for (CellCache& cache : cell_caches_) {
    fill_array(cache.filler, cache.values.data(), static_cast<int>(cache.values.size()),
               /*slice_provider=*/false);
  }
  ++cursor_.array_interpolation_counter;
  cursor_.filling_cell = false;
}

void WgDensityGraph::update_for_y(int32_t pos_y, double factor_y) {
  cursor_.in_cell_y = pos_y - cursor_.cell_start_block_y;
  for (Interpolator& interpolator : interpolators_) {
    interpolator.value_xz00 = wg_lerp(factor_y, interpolator.noise000, interpolator.noise010);
    interpolator.value_xz10 = wg_lerp(factor_y, interpolator.noise100, interpolator.noise110);
    interpolator.value_xz01 = wg_lerp(factor_y, interpolator.noise001, interpolator.noise011);
    interpolator.value_xz11 = wg_lerp(factor_y, interpolator.noise101, interpolator.noise111);
  }
}

void WgDensityGraph::update_for_x(int32_t pos_x, double factor_x) {
  cursor_.in_cell_x = pos_x - cursor_.cell_start_block_x;
  for (Interpolator& interpolator : interpolators_) {
    interpolator.value_z0 = wg_lerp(factor_x, interpolator.value_xz00, interpolator.value_xz10);
    interpolator.value_z1 = wg_lerp(factor_x, interpolator.value_xz01, interpolator.value_xz11);
  }
}

void WgDensityGraph::update_for_z(int32_t pos_z, double factor_z) {
  cursor_.in_cell_z = pos_z - cursor_.cell_start_block_z;
  ++cursor_.interpolation_counter;
  for (Interpolator& interpolator : interpolators_) {
    interpolator.value = wg_lerp(factor_z, interpolator.value_z0, interpolator.value_z1);
  }
}

void WgDensityGraph::swap_slices() {
  for (Interpolator& interpolator : interpolators_) {
    interpolator.slice0.swap(interpolator.slice1);
  }
}

void WgDensityGraph::stop_interpolation() {
  cursor_.interpolating = false;
}

} // namespace mc::world
