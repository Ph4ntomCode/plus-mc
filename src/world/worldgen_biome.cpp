// SPDX-License-Identifier: GPL-3.0-or-later
//
// The implementation behind `world/worldgen_biome.h`: `Climate.target`'s
// quantization and a port of the search vanilla does over the preset.
//
// The search is `Climate.RTree`, not a linear scan, because the preset has 7594
// parameter points and a chunk asks 1536 biome questions: a full scan would be
// ~45k interval distances *per cell*. The tree is built exactly as the game
// builds it — `CHILDREN_PER_NODE = 6`, the per-dimension center sorts, the
// `6^floor(log6(n - 0.01))` bucket size (written here as the integer "largest
// power of six below n", which is the same value without the libm rounding),
// the min-total-cost dimension pick — so its bounding boxes prune exactly the
// subtrees the game's do.
//
// One thing is deliberately not ported: `RTree.lastResult`, the thread-local
// warm-start candidate. It is an accelerator, and it can only change *which* of
// several equally distant parameter points wins; vanilla's own answer then
// depends on which chunks a worker thread filled before (it is genuinely
// history-dependent). Searching cold is deterministic and, wherever the worldgen
// test's dumps can see (biome_meta.tsv records zero disagreements of the game's
// own RTree against a linear scan over 3072 cells), agrees with the game.
#include "world/worldgen_biome.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "world/worldgen_biome_table.h"

namespace mc::world {

int64_t wg_quantize_climate_coord(float coord) {
  return static_cast<int64_t>(coord * 10000.0f);
}

WgClimateTarget wg_climate_target(float temperature, float humidity, float continentalness,
                                  float erosion, float depth, float weirdness) {
  return WgClimateTarget{
      wg_quantize_climate_coord(temperature),     wg_quantize_climate_coord(humidity),
      wg_quantize_climate_coord(continentalness), wg_quantize_climate_coord(erosion),
      wg_quantize_climate_coord(depth),           wg_quantize_climate_coord(weirdness)};
}

namespace {

// --- the memo's own accounting (docs/PHASE5.md, the biome memo) --------------
//
// A memo's win *is* its hit rate, and a hit rate is the one thing an outside
// profiler cannot see: the search either runs or it does not, and a sample of a
// storm shows the searches that happened without saying how many were answered
// without one. So the memo counts itself, two relaxed increments on the hot
// path, and each thread registers the counters it owns — *weakly*, so a thread
// that has since exited cannot leave a dangling row behind for the report, and
// so the report can say how the entries are spread across the threads that are
// still doing the work.
// The counters are atomic because the report is read from the main thread while
// generation workers are still asking questions (shutdown prints it before the
// pool is joined). Relaxed is enough: they are independent counters, and the
// report is a snapshot whose exact boundary is not part of any contract — the
// alternative, a plain `uint64_t` written by one thread and read by another, is
// a data race on the one line this whole instrument exists to make trustworthy.
struct BiomeMemoStats {
  std::atomic<uint64_t> hits{0};    // answered from the memo
  std::atomic<uint64_t> misses{0};  // cold `Climate.RTree` searches
  std::atomic<uint64_t> refills{0}; // a live slot was replaced by a colliding target
  std::atomic<size_t> entries{0};   // targets it holds right now
};

std::mutex& biome_memo_registry_mutex() {
  static std::mutex mu;
  return mu;
}

std::vector<std::weak_ptr<BiomeMemoStats>>& biome_memo_registry() {
  static std::vector<std::weak_ptr<BiomeMemoStats>> rows;
  return rows;
}

// This thread's counters, made and registered on first use. The registry is
// pruned of expired rows here rather than only in the report: a long soak opens
// one row per connection thread, and a report nobody ever asks for must not be
// what keeps the list bounded.
BiomeMemoStats& biome_memo_stats() {
  static thread_local std::shared_ptr<BiomeMemoStats> mine = [] {
    auto stats = std::make_shared<BiomeMemoStats>();
    std::lock_guard<std::mutex> lock(biome_memo_registry_mutex());
    auto& rows = biome_memo_registry();
    if (rows.size() > 1024)
      rows.erase(
          std::remove_if(rows.begin(), rows.end(),
                         [](const std::weak_ptr<BiomeMemoStats>& row) { return row.expired(); }),
          rows.end());
    rows.push_back(stats);
    return stats;
  }();
  return *mine;
}

constexpr size_t kParameterCount = sizeof(kWgBiomeParameters) / sizeof(kWgBiomeParameters[0]);

// --- biome identity ---------------------------------------------------------
//
// The surface stage asks "is this cell's biome X?" for every stone cell and
// every rule that names a biome, which used to be a `strcmp` apiece. A biome
// name is really an identity — the preset has 28 of them, each carried by many
// parameter rows — so this interns the table's names once and hands every
// equal name the same pointer. Both sides of the comparison then run through
// `wg_canonical_biome`: selection answers with the canonical pointer for the
// row it picked (by index, no hashing), and the rule tree interns the names it
// was built from. The map is read-only after construction and function-local
// statics are thread-safe, so generator workers share it without a lock.
struct CanonicalBiomes {
  std::unordered_map<std::string_view, const char*> by_name;
  std::vector<const char*> by_index;

  CanonicalBiomes() {
    by_name.reserve(64);
    by_index.reserve(kParameterCount);
    for (size_t i = 0; i < kParameterCount; ++i) {
      const char* name = kWgBiomeParameters[i].biome;
      // `try_emplace` keeps the *first* row's pointer, so the canonical form is
      // well defined ("the first name in the table") and stable across runs.
      auto [it, inserted] = by_name.try_emplace(std::string_view(name), name);
      (void)inserted;
      by_index.push_back(it->second);
    }
    // End and Nether biomes do not participate in the overworld preset, but
    // their sources still hand out names that surface rules compare by
    // identity, so intern them here as well.
    for (const char* name :
         {"minecraft:the_end", "minecraft:end_highlands", "minecraft:end_midlands",
          "minecraft:small_end_islands", "minecraft:end_barrens", "minecraft:nether_wastes",
          "minecraft:soul_sand_valley", "minecraft:crimson_forest", "minecraft:warped_forest",
          "minecraft:basalt_deltas"}) {
      by_name.try_emplace(std::string_view(name), name);
    }
  }
};

const CanonicalBiomes& canonical_biomes() {
  static const CanonicalBiomes table;
  return table;
}

// `Climate.PARAMETER_COUNT`: the six coordinates plus the offset interval.
constexpr int kDimensions = 7;
// `Climate.RTree.CHILDREN_PER_NODE`.
constexpr int kChildrenPerNode = 6;

// Every coordinate in the preset fits an `int32_t` (the quantized climate
// values are in [-10000, 10000] and the offset is zero); `search` is a hot loop
// over thousands of boxes, and halving each node's width keeps more of the tree
// in cache. Distances and box centers are still computed in `int64_t`, which is
// what Java's `long` arithmetic does; this only shrinks the storage.
constexpr bool parameter_bounds_fit_int32() {
  for (size_t i = 0; i < kParameterCount; ++i) {
    const WgBiomeParameter& p = kWgBiomeParameters[i];
    if (p.offset < std::numeric_limits<int32_t>::min() ||
        p.offset > std::numeric_limits<int32_t>::max())
      return false;
    for (int d = 0; d < 6; ++d) {
      if (p.min[d] < std::numeric_limits<int32_t>::min() ||
          p.min[d] > std::numeric_limits<int32_t>::max() ||
          p.max[d] < std::numeric_limits<int32_t>::min() ||
          p.max[d] > std::numeric_limits<int32_t>::max())
        return false;
    }
  }
  return true;
}
static_assert(parameter_bounds_fit_int32(), "SearchNode stores its bounds as int32");

// `Climate.RTree.Node`: a leaf carries one parameter point; a subtree carries
// the bounding `parameterSpace` of its children. Children live in a side pool
// (contiguous per node) so the whole tree is one flat vector.
struct SearchNode {
  int32_t min[kDimensions];
  int32_t max[kDimensions];
  int32_t parameter = -1; // leaf: index into `kWgBiomeParameters`
  int32_t first_child = 0;
  int32_t child_count = 0;
};

// `Climate.Parameter.distance(long)`: zero inside the interval, else the gap.
int64_t parameter_distance(int64_t target, int64_t min, int64_t max) {
  const int64_t above = target - max;
  const int64_t below = min - target;
  return above > 0 ? above : std::max(below, int64_t{0});
}

// Every point in this preset carries offset zero (the table's generator only
// emits `minecraft:overworld` points), and `search_and_count` asks with offset
// zero, so the seventh dimension of `Climate.Node.distance` is always zero and
// the loop below skips it. The assertion makes a future table with a non-zero
// offset a compile error instead of a silently wrong search.
constexpr bool offsets_are_zero() {
  for (size_t i = 0; i < kParameterCount; ++i)
    if (kWgBiomeParameters[i].offset != 0)
      return false;
  return true;
}
static_assert(offsets_are_zero(), "node_distance assumes a zero offset dimension");

// `Climate.Node.distance(long[])`: the squared distance to the box, over the
// six climate coordinates. `cap` bounds the answer: the sum of squares only
// grows, so as soon as it reaches `cap` the caller only needs to know "not
// closer than this" and the rest of the loop cannot change that. Every child
// that loses the search — most of them, at every level — stops early instead
// of walking all six coordinates.
int64_t node_distance(const SearchNode& node, const int64_t target[kDimensions],
                      int64_t cap = std::numeric_limits<int64_t>::max()) {
  int64_t distance = 0;
  for (int d = 0; d < 6; ++d) {
    const int64_t delta = parameter_distance(target[d], node.min[d], node.max[d]);
    distance += delta * delta; // `Mth.square`
    if (distance >= cap)
      return cap;
  }
  return distance;
}

// `Climate.RTree.cost(Parameter[])`: the box's total extent.
int64_t box_cost(const int64_t min[kDimensions], const int64_t max[kDimensions]) {
  int64_t cost = 0;
  for (int d = 0; d < kDimensions; ++d)
    cost += std::abs(max[d] - min[d]);
  return cost;
}

// A bucket of `bucketize`: its children and their bounding box, the two things
// Java's `SubTree` carries at that point of the build.
struct Box {
  std::vector<int32_t> children;
  int64_t min[kDimensions] = {};
  int64_t max[kDimensions] = {};
};

class BiomeIndex {
public:
  BiomeIndex() {
    nodes_.reserve(kParameterCount + kParameterCount / 2);
    child_pool_.reserve(kParameterCount + kParameterCount / 2);
    std::vector<int32_t> children;
    children.reserve(kParameterCount);
    for (size_t i = 0; i < kParameterCount; ++i)
      children.push_back(add_leaf(i));
    root_ = build(std::move(children));
  }

  // `Climate.RTree.search`, cold: no `lastResult` candidate (see the file
  // comment). The recursion mirrors `SubTree.search` including the detail that
  // a descendant leaf's distance is recomputed (not read from the child's box).
  // The search is a pure function of the target and the (immutable) tree, so a
  // memo of its answer cannot change any result — it is the same "cold search"
  // the file comment pins as the deterministic one, just not recomputed. A
  // generated column asks 1,536 biome questions and neighbouring cells quantise
  // to the same 1e-4 target often enough that a per-thread memo is a large win
  // (`sample` put `search` at ~4x the noise sampler in a join storm).
  size_t find_index(const WgClimateTarget& target) const {
    // The A/B arm (`PLUSMC_BIOME_MEMO=off`), the same shape as the engine's
    // other measurement knobs: "before" on the same binary, not a rebuild that
    // differs in a dozen other files.
    static const bool kEnabled = [] {
      const char* text = std::getenv("PLUSMC_BIOME_MEMO");
      return text == nullptr || std::strcmp(text, "off") != 0;
    }();
    BiomeMemoStats& stats = biome_memo_stats();
    if (!kEnabled)
      return search_and_count(target, stats);
    // A direct-mapped table rather than an `unordered_map`: every miss used to
    // allocate a node, every full memo freed all 16,384 of them in one
    // `clear()`, and the table's ~1 MiB of scattered nodes pushed the
    // generator's own working set out of the L2. A generated column asks 1,536
    // distinct targets, so the hits this leans on are the repeats between
    // neighbouring columns — exactly the entries a fixed table keeps — while a
    // miss costs one slot write and no allocator at all. A collision replaces
    // the slot (a recompute, never a wrong answer: the search is pure).
    static thread_local std::array<MemoSlot, kMemoSlots> memo{};
    const uint64_t hash = hash_target(target);
    MemoSlot& slot = memo[static_cast<size_t>(hash) & (kMemoSlots - 1)];
    if (slot.valid && slot.hash == hash && equal_target(slot.key, target)) {
      stats.hits.fetch_add(1, std::memory_order_relaxed);
      return slot.index;
    }
    const size_t index = search_and_count(target, stats);
    if (slot.valid) {
      stats.refills.fetch_add(1, std::memory_order_relaxed); // collision eviction
    } else {
      ++memo_filled();
      stats.entries.store(memo_filled(), std::memory_order_relaxed);
    }
    slot.key = target;
    slot.index = index;
    slot.hash = hash;
    slot.valid = true;
    return index;
  }

private:
  // 2,048 slots is ~160 KiB per generating thread (a slot is the six int64
  // coordinates plus the answer and its hash), which is what lets it sit in L2
  // where the node map could not.
  static constexpr size_t kMemoSlots = 2048; // power of two
  struct MemoSlot {
    WgClimateTarget key{};
    size_t index = 0;
    uint64_t hash = 0;
    bool valid = false;
  };
  // The memo's key: the six quantized coordinates, hashed as authored.
  static uint64_t hash_target(const WgClimateTarget& t) noexcept {
    uint64_t h = 1469598103934665603ULL;
    for (const int64_t value :
         {t.temperature, t.humidity, t.continentalness, t.erosion, t.depth, t.weirdness}) {
      h ^= static_cast<uint64_t>(value);
      h *= 1099511628211ULL;
    }
    return h;
  }
  static bool equal_target(const WgClimateTarget& a, const WgClimateTarget& b) noexcept {
    return a.temperature == b.temperature && a.humidity == b.humidity &&
           a.continentalness == b.continentalness && a.erosion == b.erosion && a.depth == b.depth &&
           a.weirdness == b.weirdness;
  }
  // How many slots this thread has filled at least once. Rides with the table
  // above, so it needs no atomic on the miss path.
  static size_t& memo_filled() {
    static thread_local size_t filled = 0;
    return filled;
  }
  size_t search_and_count(const WgClimateTarget& target, BiomeMemoStats& stats) const {
    const int64_t target_array[kDimensions] = {target.temperature,
                                               target.humidity,
                                               target.continentalness,
                                               target.erosion,
                                               target.depth,
                                               target.weirdness,
                                               0 /* `ParameterPoint.parameterSpace`'s offset */};
    const int32_t leaf = search(root_, target_array, -1, std::numeric_limits<int64_t>::max());
    stats.misses.fetch_add(1, std::memory_order_relaxed);
    return static_cast<size_t>(nodes_[static_cast<size_t>(leaf)].parameter);
  }

  int32_t add_leaf(size_t parameter) {
    SearchNode node{};
    for (int d = 0; d < 6; ++d) {
      node.min[d] = static_cast<int32_t>(kWgBiomeParameters[parameter].min[d]);
      node.max[d] = static_cast<int32_t>(kWgBiomeParameters[parameter].max[d]);
    }
    node.min[6] = node.max[6] = static_cast<int32_t>(kWgBiomeParameters[parameter].offset);
    node.parameter = static_cast<int32_t>(parameter);
    nodes_.push_back(node);
    return static_cast<int32_t>(nodes_.size() - 1);
  }

  // `Climate.RTree.buildParameterSpace`: the span of the children's boxes.
  void fill_bounds(Box* box) const {
    for (int d = 0; d < kDimensions; ++d) {
      box->min[d] = std::numeric_limits<int64_t>::max();
      box->max[d] = std::numeric_limits<int64_t>::min();
    }
    for (const int32_t child : box->children) {
      for (int d = 0; d < kDimensions; ++d) {
        box->min[d] = std::min<int64_t>(box->min[d], nodes_[static_cast<size_t>(child)].min[d]);
        box->max[d] = std::max<int64_t>(box->max[d], nodes_[static_cast<size_t>(child)].max[d]);
      }
    }
  }

  int32_t add_subtree(const std::vector<int32_t>& children) {
    SearchNode node{};
    Box bounds;
    bounds.children = children;
    fill_bounds(&bounds);
    for (int d = 0; d < kDimensions; ++d) {
      node.min[d] = static_cast<int32_t>(bounds.min[d]);
      node.max[d] = static_cast<int32_t>(bounds.max[d]);
    }
    node.first_child = static_cast<int32_t>(child_pool_.size());
    node.child_count = static_cast<int32_t>(children.size());
    child_pool_.insert(child_pool_.end(), children.begin(), children.end());
    nodes_.push_back(node);
    return static_cast<int32_t>(nodes_.size() - 1);
  }

  // `RTree.comparator`: the box center along one dimension, optionally absolute.
  int64_t center(int32_t node, int dimension, bool absolute) const {
    const SearchNode& n = nodes_[static_cast<size_t>(node)];
    // The sum is int64 even though each bound is int32: Java's `center()` is
    // `(min + max) / 2` in `long`, and a pair of same-signed int32 bounds can
    // overflow an int32 sum for values near the extremes.
    const int64_t value =
        (static_cast<int64_t>(n.min[dimension]) + static_cast<int64_t>(n.max[dimension])) / 2;
    return absolute ? std::abs(value) : value;
  }

  // `RTree.sort`: the starting dimension, then the rest cyclically. Java's
  // `List.sort` is stable, so equal keys keep their order; so does this.
  void sort_children(std::vector<int32_t>& children, int dimension, bool absolute) const {
    std::stable_sort(children.begin(), children.end(), [&](int32_t a, int32_t b) {
      for (int d = 0; d < kDimensions; ++d) {
        const int dim = (dimension + d) % kDimensions;
        const int64_t ka = center(a, dim, absolute);
        const int64_t kb = center(b, dim, absolute);
        if (ka != kb)
          return ka < kb;
      }
      return false;
    });
  }

  // `RTree.bucketize`: groups of the largest power of six below the count, with
  // a short last bucket. Java's expression is
  // `(int) Math.pow(6, Math.floor(Math.log(n - 0.01) / Math.log(6)))`; for an
  // integer `n` that is the largest power of six strictly below `n`, which this
  // integer walk states without depending on libm.
  std::vector<Box> bucketize(const std::vector<int32_t>& children) const {
    int expected = 1;
    while (expected * 6 < static_cast<int>(children.size()))
      expected *= 6;
    std::vector<Box> buckets;
    Box current;
    for (const int32_t child : children) {
      current.children.push_back(child);
      if (static_cast<int>(current.children.size()) < expected)
        continue;
      fill_bounds(&current);
      buckets.push_back(std::move(current));
      current = Box{};
    }
    if (!current.children.empty()) {
      fill_bounds(&current);
      buckets.push_back(std::move(current));
    }
    return buckets;
  }

  // `RTree.build`: the small case sorts by total center magnitude; the large
  // case picks the dimension whose bucketization is cheapest, sorts those
  // buckets by their own centers along it, and recurses per bucket.
  int32_t build(std::vector<int32_t> children) {
    if (children.size() == 1)
      return children[0];
    if (children.size() <= static_cast<size_t>(kChildrenPerNode)) {
      std::stable_sort(children.begin(), children.end(), [&](int32_t a, int32_t b) {
        return total_magnitude(a) < total_magnitude(b);
      });
      return add_subtree(children);
    }
    int64_t min_cost = std::numeric_limits<int64_t>::max();
    int min_dimension = -1;
    std::vector<Box> min_buckets;
    for (int d = 0; d < kDimensions; ++d) {
      sort_children(children, d, false);
      std::vector<Box> buckets = bucketize(children);
      int64_t total_cost = 0;
      for (const Box& bucket : buckets)
        total_cost += box_cost(bucket.min, bucket.max);
      if (min_cost <= total_cost)
        continue;
      min_cost = total_cost;
      min_dimension = d;
      min_buckets = std::move(buckets);
    }
    std::stable_sort(min_buckets.begin(), min_buckets.end(), [&](const Box& a, const Box& b) {
      for (int d = 0; d < kDimensions; ++d) {
        const int dim = (min_dimension + d) % kDimensions;
        const int64_t ka = box_center(a, dim, true);
        const int64_t kb = box_center(b, dim, true);
        if (ka != kb)
          return ka < kb;
      }
      return false;
    });
    std::vector<int32_t> built;
    built.reserve(min_buckets.size());
    for (Box& bucket : min_buckets)
      built.push_back(build(std::move(bucket.children)));
    return add_subtree(built);
  }

  int64_t total_magnitude(int32_t node) const {
    int64_t total = 0;
    for (int d = 0; d < kDimensions; ++d)
      total += std::abs(center(node, d, false));
    return total;
  }

  int64_t box_center(const Box& box, int dimension, bool absolute) const {
    const int64_t value = (box.min[dimension] + box.max[dimension]) / 2;
    return absolute ? std::abs(value) : value;
  }

  // `candidate_distance` is the distance the caller already computed for
  // `candidate` (max when there is none). Threading it through is exactly the
  // value this level used to recompute from the candidate's box — one full
  // distance evaluation per node visited, and the search visits a node per
  // level of every branch it takes.
  int32_t search(int32_t node_index, const int64_t target[kDimensions], int32_t candidate,
                 int64_t candidate_distance) const {
    const SearchNode& node = nodes_[static_cast<size_t>(node_index)];
    if (node.parameter >= 0)
      return node_index; // `Leaf.search` returns itself
    int64_t min_distance = candidate_distance;
    int32_t closest = candidate;
    // A distance of zero is the minimum, and a child has to be *strictly*
    // closer to replace the candidate, so nothing below can improve on it.
    if (min_distance == 0)
      return closest;
    for (int32_t i = 0; i < node.child_count; ++i) {
      const int32_t child =
          child_pool_[static_cast<size_t>(node.first_child) + static_cast<size_t>(i)];
      const int64_t child_distance =
          node_distance(nodes_[static_cast<size_t>(child)], target, min_distance);
      if (min_distance <= child_distance)
        continue;
      const int32_t leaf = search(child, target, closest, min_distance);
      const int64_t leaf_distance =
          child == leaf ? child_distance
                        : node_distance(nodes_[static_cast<size_t>(leaf)], target, min_distance);
      if (min_distance <= leaf_distance)
        continue;
      min_distance = leaf_distance;
      closest = leaf;
      if (min_distance == 0)
        return closest;
    }
    return closest;
  }

  std::vector<SearchNode> nodes_;
  std::vector<int32_t> child_pool_;
  int32_t root_ = 0;
};

} // namespace

size_t wg_select_biome_index(const WgClimateTarget& target) {
  // Built once, on first use; the search is const, so concurrent readers share
  // the tree.
  static const BiomeIndex index;
  return index.find_index(target);
}

const char* wg_select_biome(const WgClimateTarget& target) {
  // The canonical pointer for the row, not the row's own literal: see
  // `CanonicalBiomes` above. This is an array read, so it costs the hot path
  // nothing while letting every consumer compare biome identity by pointer.
  const size_t index = wg_select_biome_index(target);
  return canonical_biomes().by_index[index];
}

const char* wg_select_end_biome(int32_t section_x, int32_t section_z, double erosion) {
  const int64_t sx = section_x;
  const int64_t sz = section_z;
  if (sx * sx + sz * sz <= 4096)
    return wg_canonical_biome("minecraft:the_end");
  if (erosion > 0.25)
    return wg_canonical_biome("minecraft:end_highlands");
  if (erosion >= -0.0625)
    return wg_canonical_biome("minecraft:end_midlands");
  if (erosion < -0.21875)
    return wg_canonical_biome("minecraft:small_end_islands");
  return wg_canonical_biome("minecraft:end_barrens");
}

const char* wg_select_nether_biome(const WgClimateTarget& target) {
  // `minecraft:nether` is a five-point `Climate.ParameterList`. Every point is
  // a point interval, and the five non-climate coordinates are zero, so the
  // nearest search is this scan. `fitness` includes the point's offset.
  struct Point {
    int64_t temperature;
    int64_t humidity;
    int64_t offset;
    const char* biome;
  };
  static constexpr Point kPoints[] = {
      {0, 0, 0, "minecraft:nether_wastes"},        {0, -5000, 0, "minecraft:soul_sand_valley"},
      {4000, 0, 0, "minecraft:crimson_forest"},    {0, 5000, 3750, "minecraft:warped_forest"},
      {-5000, 0, 1750, "minecraft:basalt_deltas"},
  };
  const int64_t climate[6] = {target.temperature, target.humidity, target.continentalness,
                              target.erosion,     target.depth,    target.weirdness};
  const char* closest = kPoints[0].biome;
  int64_t closest_fitness = std::numeric_limits<int64_t>::max();
  for (const Point& point : kPoints) {
    const int64_t values[6] = {point.temperature, point.humidity, 0, 0, 0, 0};
    int64_t fitness = point.offset * point.offset;
    for (int d = 0; d < 6; ++d) {
      const int64_t delta = climate[d] - values[d];
      fitness += delta * delta;
    }
    if (fitness < closest_fitness) {
      closest_fitness = fitness;
      closest = point.biome;
    }
  }
  return wg_canonical_biome(closest);
}

const char* wg_canonical_biome(const char* name) {
  if (name == nullptr)
    return nullptr;
  const CanonicalBiomes& table = canonical_biomes();
  const auto it = table.by_name.find(std::string_view(name));
  return it == table.by_name.end() ? name : it->second;
}

std::string wg_biome_memo_report() {
  std::lock_guard<std::mutex> lock(biome_memo_registry_mutex());
  struct Row {
    uint64_t hits = 0;
    uint64_t misses = 0;
    size_t entries = 0;
  };
  std::vector<Row> per_thread;
  uint64_t hits = 0, misses = 0, refills = 0;
  size_t threads = 0, entries = 0, most = 0;
  auto& rows = biome_memo_registry();
  for (auto it = rows.begin(); it != rows.end();) {
    const std::shared_ptr<BiomeMemoStats> row = it->lock();
    if (!row) {
      it = rows.erase(it); // that thread has exited
      continue;
    }
    const uint64_t row_hits = row->hits.load(std::memory_order_relaxed);
    const uint64_t row_misses = row->misses.load(std::memory_order_relaxed);
    hits += row_hits;
    misses += row_misses;
    refills += row->refills.load(std::memory_order_relaxed);
    const size_t row_entries = row->entries.load(std::memory_order_relaxed);
    entries += row_entries;
    most = std::max(most, row_entries);
    per_thread.push_back(Row{row_hits, row_misses, row_entries});
    ++threads;
    ++it;
  }
  if (hits + misses == 0)
    return {}; // no biome question was asked in this process
  const double rate = 100.0 * static_cast<double>(hits) / static_cast<double>(hits + misses);
  // The counters are per thread because the memos are: a pool worker's memo is
  // warm from the columns it built, a session thread's is cold from the one
  // column it happened to build inline. `most` alone hides that spread, so the
  // tail lists the largest memos with their own hit rate — that is the evidence
  // that a win is hits and not a cheaper cold search, and it stays bounded on a
  // 60-connection server (six rows and a count of the rest).
  constexpr std::size_t kListed = 6;
  std::sort(per_thread.begin(), per_thread.end(),
            [](const Row& a, const Row& b) { return a.entries > b.entries; });
  char tail[320];
  int used = 0;
  for (std::size_t i = 0; i < per_thread.size() && i < kListed; ++i) {
    const Row& row = per_thread[i];
    const uint64_t asked = row.hits + row.misses;
    const double row_rate =
        asked ? 100.0 * static_cast<double>(row.hits) / static_cast<double>(asked) : 0.0;
    used += std::snprintf(tail + used, sizeof(tail) - static_cast<std::size_t>(used),
                          "%s%zue@%.1f%%", i ? " " : "", row.entries, row_rate);
    if (used < 0 || static_cast<std::size_t>(used) >= sizeof(tail))
      break;
  }
  if (per_thread.size() > kListed && used > 0 && static_cast<std::size_t>(used) < sizeof(tail))
    std::snprintf(tail + used, sizeof(tail) - static_cast<std::size_t>(used), " +%zu more",
                  per_thread.size() - kListed);
  char line[640];
  std::snprintf(line, sizeof(line),
                "biome memo: %llu hits, %llu misses (%.1f%% hit rate), %zu entr(ies) across "
                "%zu thread(s) (most %zu), %llu refill(s); threads %s",
                static_cast<unsigned long long>(hits), static_cast<unsigned long long>(misses),
                rate, entries, threads, most, static_cast<unsigned long long>(refills),
                tail[0] ? tail : "none");
  return std::string(line);
}

int64_t wg_biome_fitness(const WgClimateTarget& target, size_t parameter_index) {
  const WgBiomeParameter& parameter = kWgBiomeParameters[parameter_index];
  const int64_t values[6] = {target.temperature, target.humidity, target.continentalness,
                             target.erosion,     target.depth,    target.weirdness};
  int64_t fitness = 0;
  for (int d = 0; d < 6; ++d) {
    const int64_t delta = parameter_distance(values[d], parameter.min[d], parameter.max[d]);
    fitness += delta * delta; // `Mth.square`
  }
  return fitness + parameter.offset * parameter.offset;
}

} // namespace mc::world
