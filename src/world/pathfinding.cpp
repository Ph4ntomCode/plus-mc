// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ground pathfinding: see world/pathfinding.h for what is and is not modelled.
#include "world/pathfinding.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

namespace mc::world {
namespace {

// The eight horizontal directions, orthogonals first so a tie between a straight
// step and a diagonal one prefers the straight one (which keeps a mob's path
// from zig-zagging when both cost the same).
constexpr int kDirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};

// Three 21-bit coordinates packed into one 64-bit key; the world is far smaller
// than +- 1 048 576 blocks in every axis. Used for both the node table and the
// block cache, which key the same space.
inline uint64_t cell_key(int x, int y, int z) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(x) & 0x1FFFFF) << 42) |
         (static_cast<uint64_t>(static_cast<uint32_t>(y) & 0x1FFFFF) << 21) |
         static_cast<uint64_t>(static_cast<uint32_t>(z) & 0x1FFFFF);
}

struct CellHash {
  size_t operator()(const NavCell& c) const noexcept {
    return static_cast<size_t>(cell_key(c.x, c.y, c.z));
  }
};

bool is_solid(const NavEnvironment& env, int x, int y, int z) {
  return env.is_solid && env.is_solid(x, y, z);
}

// A search's view of the block predicate, one probe deep.
//
// `env.is_solid` is a read of the world, and a synchronous search reads one
// snapshot of it, so an answer can be reused *inside* that search — the same
// assumption the node table already makes about a cell, applied a level down, to
// blocks, which is where the duplication is: a cell's floor block is the
// headroom block of the cell below it, and `step_to`'s corner check asks for
// exactly the headroom blocks of the two orthogonal neighbours it has already
// tested this expansion. The differential probe's wall world has the search ask
// the predicate ~4,300 times per path; the distinct blocks behind those calls are
// far fewer.
//
// Direct-mapped and one probe per query: the index is the packed key's top bits
// after a Fibonacci multiply, and a generation stamp makes a new search read
// empty slots without clearing them (the node table's own trick). A collision
// overwrites — no probing, no chain — which is what keeps a hit cheaper than the
// `std::function` call it replaces.
struct SolidCache {
  static constexpr size_t kSlots = 1024;
  struct Slot {
    uint64_t key = 0;
    uint32_t generation = 0;
    bool value = false;
  };
  std::array<Slot, kSlots> slots{};
  uint32_t generation = 0;

  void begin_search() {
    if (++generation == 0) { // wrapped: retire every slot's stamp
      for (Slot& slot : slots)
        slot.generation = 0;
      generation = 1;
    }
  }

  bool solid(const NavEnvironment& env, int x, int y, int z) {
    if (!env.is_solid)
      return false;
    const uint64_t key = cell_key(x, y, z);
    // The *top* bits of the Fibonacci product, not the middle: the packed key's
    // low field is z and a neighbour differs by one in exactly one coordinate,
    // so only the high bits have the multiplication's carry chain mixed all three
    // fields into them. Measured both ways on the probe: top bits gave 1.54M
    // predicate calls, `>> 32 & (kSlots - 1)` gave 6.34M -- an almost total
    // miss rate, on the same digest-identical paths.
    constexpr int kIndexBits = 10;
    static_assert(kSlots == (size_t{1} << kIndexBits), "the index shift follows kSlots");
    const uint64_t mixed = key * 0x9E3779B97F4A7C15ull;
    Slot& slot = slots[static_cast<size_t>(mixed >> (64 - kIndexBits))];
    if (slot.generation == generation && slot.key == key)
      return slot.value;
    const bool value = env.is_solid(x, y, z);
    slot.key = key;
    slot.value = value;
    slot.generation = generation;
    return value;
  }
};

// The predicate, through `cache` when a search is paying for one. `nullptr` is
// the one-shot path (`nav_cell_walkable`, `nav_nearest_walkable`): a single cell
// or a small shell, with no search to amortise a table over.
inline bool solid_at(const NavEnvironment& env, SolidCache* cache, int x, int y, int z) {
  if (cache != nullptr)
    return cache->solid(env, x, y, z);
  return is_solid(env, x, y, z);
}

// The headroom a walkable cell owes `env.height`: `ceil(height - 1e-6)`, floored
// at one. It depends on the environment alone, so a search computes it once
// instead of once per candidate cell.
int headroom_for(const NavEnvironment& env) {
  return std::max(1, static_cast<int>(std::ceil(env.height - 1.0e-6)));
}

// `nav_cell_walkable`'s body with the caller's precomputed headroom, so a search
// that asks the same question of thousands of cells pays the ceiling once.
bool walkable_with(const NavCell& cell, const NavEnvironment& env, int clear, SolidCache* cache) {
  if (!solid_at(env, cache, cell.x, cell.y - 1, cell.z))
    return false; // nothing to stand on
  for (int i = 0; i < clear; ++i)
    if (solid_at(env, cache, cell.x, cell.y + i, cell.z))
      return false;
  return true;
}

double heuristic(const NavCell& a, const NavCell& b) {
  const double dx = static_cast<double>(a.x - b.x);
  const double dz = static_cast<double>(a.z - b.z);
  // Euclidean in the horizontal plane plus a penalty per vertical step, which is
  // admissible because no move is cheaper than its own vertical cost.
  return std::sqrt(dx * dx + dz * dz) + 0.5 * static_cast<double>(std::abs(a.y - b.y));
}

// The cheapest walkable cell one horizontal step away, or `out` untouched.
// Tries the same level first (WalkNodeEvaluator's ordering: straight ahead
// before climbing), then one step up, then the fall distances.
bool step_to(const NavCell& from, int dx, int dz, const NavEnvironment& env, int clear,
             SolidCache* cache, NavCell* out, double* cost) {
  // Same level first.
  NavCell same{from.x + dx, from.y, from.z + dz};
  // Step up: the mob jumps. Vanilla allows a single block of step-up for a
  // ground mob (`maxUpStep` is 0.6, so a full block is a jump, not a step).
  NavCell up{from.x + dx, from.y + 1, from.z + dz};
  NavCell order[1 + kNavMaxFall + 1];
  int count = 0;
  order[count++] = same;
  order[count++] = up;
  for (int dy = 1; dy <= kNavMaxFall; ++dy)
    order[count++] = NavCell{from.x + dx, from.y - dy, from.z + dz};
  for (int i = 0; i < count; ++i) {
    const NavCell& candidate = order[i];
    if (!walkable_with(candidate, env, clear, cache))
      continue;
    // A diagonal may not squeeze through a corner: both cells it passes between
    // have to be clear at the mob's own level.
    if (dx != 0 && dz != 0) {
      if (solid_at(env, cache, from.x, from.y, from.z + dz) ||
          solid_at(env, cache, from.x, from.y + 1, from.z + dz))
        continue;
      if (solid_at(env, cache, from.x + dx, from.y, from.z) ||
          solid_at(env, cache, from.x + dx, from.y + 1, from.z))
        continue;
    }
    const int climb = candidate.y - from.y;
    double c = (dx != 0 && dz != 0) ? 1.41421356 : 1.0;
    if (climb > 0)
      c += 0.5 * climb; // a jump costs more than a step
    else if (climb < 0)
      c += 0.25 * static_cast<double>(-climb); // walking off a ledge
    *out = candidate;
    *cost = c;
    return true;
  }
  return false;
}

// One visited cell's bookkeeping: its cost so far, the heuristic cached at
// discovery, its parent edge and whether that edge exists. One table holds what
// `g_score` and `came_from` held separately, which halves the hash lookups --
// and the duplicate keys -- of every visited node.
struct Node {
  double g = 0.0;
  double h = 0.0;
  NavCell parent{};
  bool has_parent = false;
};

struct OpenEntry {
  double f = 0.0;
  double g = 0.0;
  double h = 0.0;
  NavCell cell{};
  // A total order, not just `f > other.f`. Equal-cost paths are common (the
  // eight directions and the 1.0 / 1.41421356 step costs produce many ties),
  // and the heap's tie order is unspecified: libstdc++ and libc++ pop the same
  // equal-f entries in different orders, which changes which shortest path is
  // returned. The remaining fields make the winner unique, so the path shape
  // no longer depends on the standard library's heap implementation.
  bool operator>(const OpenEntry& other) const {
    if (f != other.f)
      return f > other.f;
    if (h != other.h)
      return h > other.h;
    if (g != other.g)
      return g > other.g;
    if (cell.x != other.cell.x)
      return cell.x > other.cell.x;
    if (cell.y != other.cell.y)
      return cell.y > other.cell.y;
    return cell.z > other.cell.z;
  }
};

// One slot of the node table: the cell it holds, its bookkeeping, and the
// generation the two were written in.
struct NodeSlot {
  NavCell cell{};
  Node node{};
  uint32_t generation = 0;
};

// A search's containers, borrowed from the calling thread and *cleared* rather
// than rebuilt. Mobs re-plan every 4-10 ticks, and a call-ordered profile of
// the mob tick put allocation and deallocation at roughly a quarter of all
// samples — the `std::unordered_map` this table replaces allocates a node per
// visited cell (libc++'s map does: 500 searches over a wall world measured
// 192,780 allocations, ~385 per search). The table here is open-addressed over
// one vector, and a new search retires the old one by bumping a generation
// counter instead of walking slots, so the steady state allocates nothing. The
// open heap is the same vector driven by `std::push_heap`/`std::pop_heap` with
// the `std::greater<OpenEntry>` a priority_queue used — a priority_queue owns
// its vector and would take the capacity with it.
struct SearchScratch {
  std::vector<NodeSlot> slots; // power-of-two capacity while non-empty
  uint32_t generation = 0;     // bumped per search: an older slot reads empty
  size_t live = 0;             // slots this generation holds
  std::vector<OpenEntry> heap;
  std::vector<NavCell> reversed;
  SolidCache solid;
  bool in_use = false;

  void begin_search() {
    if (++generation == 0) { // wrapped: retire the previous generation wholesale
      for (NodeSlot& slot : slots)
        slot.generation = 0;
      generation = 1;
    }
    solid.begin_search();
    live = 0;
    heap.clear();
    reversed.clear();
    if (slots.empty())
      slots.resize(512); // ~24 KiB, and growth keeps its high-water mark
  }

  // The first slot `cell` can live in. The packed hash's low bits are one
  // coordinate's, and neighbours here are one block apart, so the index is a
  // Fibonacci mix of it rather than the low bits themselves.
  size_t slot_index(const NavCell& cell) const {
    const uint64_t mixed = static_cast<uint64_t>(CellHash{}(cell)) * 0x9E3779B97F4A7C15ull;
    return static_cast<size_t>(mixed >> 32) & (slots.size() - 1);
  }

  // The node for `cell`, or null when this search has not reached it.
  Node* find(const NavCell& cell) {
    const size_t mask = slots.size() - 1;
    size_t index = slot_index(cell);
    for (;;) {
      NodeSlot& slot = slots[index];
      if (slot.generation != generation)
        return nullptr;
      if (slot.cell == cell)
        return &slot.node;
      index = (index + 1) & mask;
    }
  }

  // The node for `cell`, `*inserted` saying whether this search had not reached
  // it yet. An expansion does `find` then `set` on the same cell — two walks of
  // the same probe chain — so they are resolved in one here: the caller fills
  // the returned node in when `*inserted`, and skips the write when a cheaper
  // path already owns the cell.
  Node* find_or_insert(const NavCell& cell, bool* inserted) {
    if ((live + 1) * 4 > slots.size() * 3)
      grow();
    const size_t mask = slots.size() - 1;
    size_t index = slot_index(cell);
    for (;;) {
      NodeSlot& slot = slots[index];
      if (slot.generation != generation) {
        slot.cell = cell;
        slot.node = Node{};
        slot.generation = generation;
        ++live;
        *inserted = true;
        return &slot.node;
      }
      if (slot.cell == cell) {
        *inserted = false;
        return &slot.node;
      }
      index = (index + 1) & mask;
    }
  }

  // Insert or overwrite, exactly as `map[cell] = node` did.
  void set(const NavCell& cell, const Node& node) {
    if ((live + 1) * 4 > slots.size() * 3)
      grow();
    const size_t mask = slots.size() - 1;
    size_t index = slot_index(cell);
    for (;;) {
      NodeSlot& slot = slots[index];
      if (slot.generation != generation) {
        slot.cell = cell;
        slot.node = node;
        slot.generation = generation;
        ++live;
        return;
      }
      if (slot.cell == cell) {
        slot.node = node;
        return;
      }
      index = (index + 1) & mask;
    }
  }

  void grow() {
    std::vector<NodeSlot> old;
    old.swap(slots);
    slots.assign(old.size() * 2, NodeSlot{});
    live = 0;
    // `set`'s load check cannot fire during the re-insert: the capacity halved
    // the load and the table is re-inserted at under half full.
    for (const NodeSlot& slot : old)
      if (slot.generation == generation)
        set(slot.cell, slot.node);
  }
};

// `nav_nearest_walkable`'s body with the caller's precomputed headroom.
NavCell nearest_walkable(const NavCell& cell, const NavEnvironment& env, int radius, bool* blocked,
                         int clear, SolidCache* cache) {
  if (walkable_with(cell, env, clear, cache)) {
    if (blocked)
      *blocked = false;
    return cell;
  }
  // Nearest first: widen the cube one shell at a time and take the best cell on
  // the first shell that has one. The score prefers the same column (the space
  // directly above a buried target), then the least horizontal distance, then
  // the smaller vertical step.
  for (int r = 1; r <= radius; ++r) {
    NavCell best{};
    int best_score = std::numeric_limits<int>::max();
    bool found = false;
    for (int dx = -r; dx <= r; ++dx) {
      for (int dz = -r; dz <= r; ++dz) {
        for (int dy = -r; dy <= r; ++dy) {
          if (std::max(std::max(std::abs(dx), std::abs(dz)), std::abs(dy)) != r)
            continue; // only the new shell
          const NavCell candidate{cell.x + dx, cell.y + dy, cell.z + dz};
          if (!walkable_with(candidate, env, clear, cache))
            continue;
          const int score = (std::abs(dx) + std::abs(dz)) * 4 + std::abs(dy);
          if (score >= best_score)
            continue;
          best_score = score;
          best = candidate;
          found = true;
        }
      }
    }
    if (found) {
      if (blocked)
        *blocked = false;
      return best;
    }
  }
  if (blocked)
    *blocked = true;
  return cell;
}

} // namespace

bool nav_cell_walkable(const NavCell& cell, const NavEnvironment& env) {
  return walkable_with(cell, env, headroom_for(env), nullptr);
}

NavCell nav_nearest_walkable(const NavCell& cell, const NavEnvironment& env, int radius,
                             bool* blocked) {
  return nearest_walkable(cell, env, radius, blocked, headroom_for(env), nullptr);
}

NavPath find_path(const NavCell& from, const NavCell& to, const NavEnvironment& env, bool* reached,
                  int max_nodes) {
  if (reached)
    *reached = false;
  NavPath path;
  bool goal_blocked = false;
  const int clear = headroom_for(env);
  const NavCell goal = nearest_walkable(to, env, 3, &goal_blocked, clear, nullptr);
  // The goal's shell is resolved before the scratch is taken, so it goes
  // uncached: three small shells against the thousands of expansion queries
  // below is not where the predicate's cost is.
  if (goal_blocked)
    return path;
  if (goal == from) {
    if (reached)
      *reached = true; // already standing there: an empty path, not a failure
    return path;
  }

  // The table and the heap come from the thread's scratch (see
  // `SearchScratch`) and are retired, not destroyed, so the steady state
  // allocates nothing. If the thread's scratch is already in use — an
  // `is_solid` callback that re-enters the search — this call takes a
  // stack-local one instead: one code path, and no two searches sharing a
  // table.
  std::optional<SearchScratch> local;
  thread_local SearchScratch borrowed;
  if (borrowed.in_use)
    local.emplace();
  SearchScratch& scratch = local ? *local : borrowed;
  scratch.in_use = true;
  const struct ScratchRelease {
    SearchScratch* scratch;
    ~ScratchRelease() {
      scratch->in_use = false;
    }
  } scratch_release{&scratch};
  scratch.begin_search();
  auto& heap = scratch.heap;
  const auto open_greater = std::greater<OpenEntry>{};

  const NavCell start = walkable_with(from, env, clear, &scratch.solid)
                            ? from
                            : nearest_walkable(from, env, 2, nullptr, clear, &scratch.solid);
  const double start_h = heuristic(start, goal);
  scratch.set(start, Node{0.0, start_h, NavCell{}, false});
  heap.push_back(OpenEntry{start_h, 0.0, start_h, start});
  std::push_heap(heap.begin(), heap.end(), open_greater);

  NavCell best = start;
  double best_h = start_h;
  bool found = false;
  int visited = 0;

  while (!heap.empty() && visited < max_nodes) {
    // The same pop a `std::priority_queue` would do: the least entry (by the
    // total order above) comes off the back.
    std::pop_heap(heap.begin(), heap.end(), open_greater);
    const OpenEntry top = heap.back();
    heap.pop_back();
    const Node* known = scratch.find(top.cell);
    if (known != nullptr && top.g > known->g)
      continue; // a stale queue entry
    ++visited;
    if (top.cell == goal) {
      best = goal;
      found = true;
      break;
    }
    if (top.h < best_h) {
      best_h = top.h;
      best = top.cell;
    }
    for (const auto& dir : kDirs) {
      NavCell next{};
      double cost = 0.0;
      if (!step_to(top.cell, dir[0], dir[1], env, clear, &scratch.solid, &next, &cost))
        continue;
      const double tentative = top.g + cost;
      bool inserted = false;
      Node* slot = scratch.find_or_insert(next, &inserted);
      if (!inserted && tentative >= slot->g)
        continue;
      const double h = heuristic(next, goal);
      *slot = Node{tentative, h, top.cell, true};
      heap.push_back(OpenEntry{tentative + h, tentative, h, next});
      std::push_heap(heap.begin(), heap.end(), open_greater);
    }
  }

  // Reconstruct: walk the parents back to the start, then reverse.
  auto& reversed = scratch.reversed;
  reversed.clear();
  NavCell cursor = best;
  reversed.push_back(cursor);
  while (!(cursor == start)) {
    const Node* node = scratch.find(cursor);
    if (node == nullptr || !node->has_parent)
      break; // should not happen; treat what we have as the path
    cursor = node->parent;
    reversed.push_back(cursor);
    if (reversed.size() > static_cast<size_t>(kNavMaxPathLength) + 1)
      break;
  }
  // Drop the start cell: the waypoints are where to walk *to*.
  if (!reversed.empty() && reversed.back() == start)
    reversed.pop_back();
  if (reversed.size() > static_cast<size_t>(kNavMaxPathLength))
    reversed.resize(static_cast<size_t>(kNavMaxPathLength));
  path.points.assign(reversed.rbegin(), reversed.rend());
  if (reached)
    *reached = found;
  return path;
}

} // namespace mc::world
