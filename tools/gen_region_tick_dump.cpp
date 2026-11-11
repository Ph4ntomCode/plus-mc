// SPDX-License-Identifier: GPL-3.0-or-later
//
// Writes the region-parallel scheduled-tick loop's golden output for a small set
// of recorded scenes: the run parameters (`region_tick_meta.tsv`), the world each
// run starts from (`region_tick_scene.tsv`), the caller's own scripted work
// (`region_tick_actions.tsv`) and one interaction trace per run
// (`region_tick_seed42_<run>.tsv`).
//
// This is **not** a vanilla dump: what a fire schedules is `30 + nextInt(10)`
// from a `RandomSource` whose state on a tick belongs to the server, and the
// partition itself is this engine's, not the game's. It is the seventh pin, and
// the one the other six cannot state: `block_tick` runs the scheduled-tick loop
// on the serial arm, `region-scheduler` pins the scheduler's own contract on a
// synthetic workload (every region once on lane `i % lanes`, the lane count
// invisible in a region's result, a failure surfacing after every lane is
// joined), and neither runs the loop that actually dispatches the queue — the
// one `tick_block_ticks` (src/server/session.cpp) hands a region set to.
//
// What a run is: `ticks` world ticks. In each one the real `BlockTickQueue` is
// advanced, its entries are sorted into the canonical order the session's region
// arm sorts them into (owning region, then x, y, z), that order is cut into one
// contiguous range per region, and `RegionScheduler::run` dispatches each range
// through `run_block_tick_position`'s shape (the block standing there *now*
// decides: a fire ticks as fire, a leaf as a leaf, anything else is dropped).
// Then the caller's own scripted work for that tick happens — a placement or a
// break, the two session paths that touch this queue. Everything the partition,
// the rules and the caller do to the queue, to the world and to the recorded
// stream is in the trace, in order:
//
//   action   <tick> <ignite|break> <x> <y> <z>            (the caller's scripted work)
//   due      <tick> <rx> <rz> <x> <y> <z>                 (one entry, canonical order, with its
//   region) plan     <tick> <regions> <due>                       (the region set one `run` was
//   handed) region   <tick> <rx> <rz> <first> <last>              (the slice of this tick's `due`
//   list that region owned) dispatch <tick> <fire|leaves|drop> <x> <y> <z>        (inside the
//   region's block) schedule <tick> <x> <y> <z> <delay> <due>             (`Level.scheduleTick`)
//   read     <tick> <x> <y> <z> <state>                   (`get_block`)
//   sturdy   <tick> <x> <y> <z> <true|false>              (`is_sturdy`)
//   light    <tick> <x> <y> <z> <level>                   (`getRawBrightness`)
//   write    <tick> <x> <y> <z> <state> <notify>          (`set_block`)
//   item     <tick> <x> <y> <z> <state>                   (`drop_resources`)
//   prime    <tick> <x> <y> <z>                           (`TntBlock.prime`)
//   draw i   <tick> <bound> <result>                      (`nextInt(bound)`)
//   draw u   <tick> <result>                              (`nextFloat`)
//   state    <tick> <now> <pending> <chunks> <regions>    (the queue after the drain)
//   world    <tick> <hash>                                (one row: the whole world at the end)
//
// **What is pinned, and what deliberately is not.**
//
//   * *The snapshot is pinned.* Which entries are due in a tick, the canonical
//     order they are sorted into, the region each one belongs to, the contiguous
//     range each region is handed, and the order each region's own entries are
//     dispatched in — row for row. That is the region path's own contribution
//     and this fixture's subject; `block_tick` pins the serial drain of the same
//     queue and the rules' own streams.
//
//   * *The queue's raw return order is not pinned.* `advance()` walks an
//     `unordered_map`, so its order is the bucket layout's, which differs
//     between libstdc++ and libc++ — and is exactly why the session's region arm
//     sorts before dispatching. The trace holds the sorted order, which is a
//     property of the world rather than of the hash map.
//
//   * *The lane each region runs on is not pinned.* That is a function of the
//     lane count by construction (`i % lanes`), and `region-scheduler` pins it
//     directly. What this tool does instead is run every run twice — at one lane
//     and at `kProbeLanes` — and require the two traces (and the two end-of-run
//     world hashes) to be equal for the runs whose work cannot cross a region
//     boundary inside one tick. `laneStable` in the meta says which those were,
//     and the trace of a stable run is the *parallel* arm's: replaying it at the
//     meta's lane count is the lane-count check.
//
//   * *The residue is a number, not a row.* A write that crosses a region
//     boundary inside one tick lands under the world's lock, so whether the
//     neighbouring region sampled that cell before or after it is
//     timing-dependent (docs/PHASE5.md §5). The `edge` run exists to be that
//     case: it is recorded `laneStable no`, at one lane (the canonical order by
//     definition), and the tool *prints* how many rows the parallel arm differed
//     by instead of checking a number in — that count is a property of the
//     machine's timing, not of the engine.
//
//   * *The fire's draw values are a region's own stream, not the lane's.* The
//     session's region arm reseeds the lane's stream from `region_tick_seed`
//     before each region's block-tick slice and hands `fire_tick` a `FireRandom`
//     over it (session.cpp), matching the random-tick, precipitation, mob and
//     projectile passes, so a region's fire draws do not depend on the lane
//     count. This tool records the draws from a stream seeded
//     `region_tick_seed(seed ^ kFireSeedSalt, region, tick)`, the same shape, so
//     the trace is reproducible and the lane A/B compares one simulation rather
//     than two — the same bargain `block_tick` makes for the serial arm's
//     values. The session's serial arm and its placement path
//     (`schedule_new_fire`) keep the thread-local source; neither is part of
//     this fixture's lane A/B.
//
// Two things are deliberately not modelled, and the session's adapters do both:
// the client broadcast and the redstone fan-out a write raises (neither touches
// a cell a fire or a leaf reads and neither schedules a block tick), and weather
// (`FireLevel::is_raining` stays null, i.e. "not raining" — `tests/fixtures/
// weather` and `fire_test.cpp` own the rain branches).
//
// Usage:
//   plusmc-region-tick-dump <fixtures_dir> [seed] [--diff]
//
// `--diff` writes nothing: it re-runs every scene in memory, reads what is
// checked in, and prints a patch of each file that would change (invariant
// violations included), exiting 1 while the fixture differs. The review is
//
//   ./build/plusmc-region-tick-dump <fixtures_dir> --diff   # read the patch
//   ./build/plusmc-region-tick-dump <fixtures_dir>          # then rewrite it
//   ctest -R region_tick                                    # and re-pin it
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fixture_diff.h"
#include "sim/region_scheduler.h"
#include "world/block_tick.h"
#include "world/chunk.h"
#include "world/fire.h"
#include "world/leaves.h"
#include "world/random_tick.h"
#include "world/survival.h" // kDifficultyNormal
#include "world/worldgen_random.h"

namespace {

using mc::world::BlockStateKey;
using mc::world::BlockTickPos;
using mc::world::FireLevel;
using mc::world::FireRandom;
using mc::world::kFireTickDelaySpread;
using mc::world::kFireTickMinDelay;
using mc::world::RandomTickLevel;
using mc::world::WgLegacyRandom;

// The floor and the cell above it every scene is built on. A fire needs a sturdy
// or burnable block below it (`FireBlock.getStateForPlacement`), so the floor is
// what makes a placement possible at all.
constexpr int kFloorY = -61;
constexpr int kAirY = -60;
constexpr int kDifficulty = mc::world::kDifficultyNormal;

// The region grid the session's region arm is configured with by default, and
// the lane count the A/B's parallel arm uses. Two chunks per region side means a
// region spans 32 blocks in x and z, so work at x = 8, 40 and 72 sits in three
// different regions and the grouping is visible in the trace.
constexpr int kChunksPerRegion = 2;
constexpr unsigned kProbeLanes = 4;

// The modelled `RandomSource` salts: one stream per (region, tick) for the fire
// draws, and one for the placement path's own `onPlace` clock (which in a live
// session is drawn on the calling thread, with no region in hand — the tool
// draws it from the stream of the region the placement lands in, which is what
// keeps a whole run a function of (region, tick)).
constexpr uint64_t kFireSeedSalt = 0x9E3779B97F4A7C15ULL;

const int64_t kDefaultSeed = 42;

const BlockStateKey& air() {
  static const BlockStateKey state{"minecraft:air", ""};
  return state;
}

// `entity_block_is_solid`, the session's own `is_sturdy`: this engine has no
// collision shapes, so "not air and not a fluid" is the whole of it.
bool solidish(std::string_view name) {
  return name != "minecraft:air" && name != "minecraft:cave_air" && name != "minecraft:void_air" &&
         name != "minecraft:water" && name != "minecraft:lava" && name != "minecraft:bubble_column";
}

std::string canonical_state(const BlockStateKey& state) {
  std::string text = state.name;
  if (!state.properties.empty()) {
    text += '[';
    text += state.properties;
    text += ']';
  }
  return text;
}

std::string coord(int x, int y, int z) {
  char text[64];
  std::snprintf(text, sizeof(text), "%d\t%d\t%d", x, y, z);
  return text;
}

// ---- the region layer, spelled the way the session spells it ----------------

mc::sim::RegionGrid region_grid() {
  return mc::sim::RegionGrid(kChunksPerRegion);
}

int chunk_of(int v) {
  return v >= 0 ? v / 16 : (v - 15) / 16;
}

// `region_of_block` (session.cpp): the region that owns a world position, which
// is the test every partitioned dispatch makes.
mc::sim::RegionPos region_of(const mc::sim::RegionGrid& grid, int x, int z) {
  return grid.region_of(chunk_of(x), chunk_of(z));
}

std::string region_text(const mc::sim::RegionPos& region) {
  char text[48];
  std::snprintf(text, sizeof(text), "%d\t%d", region.rx, region.rz);
  return text;
}

uint64_t fnv1a(uint64_t h, const void* data, size_t n) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < n; ++i) {
    h ^= bytes[i];
    h *= 1099511628211ULL;
  }
  return h;
}

// ---- the scenes ---------------------------------------------------------------

struct Placement {
  int x;
  int y;
  int z;
  std::string name;
  std::string properties;
};

// One thing the caller does between ticks, i.e. the two session paths that touch
// this queue: a placement (`flint_and_steel`, `/setblock`) and a break.
struct Action {
  int tick; // 0 means "in the window before the first tick"
  bool ignite;
  int x;
  int y;
  int z;
};

struct Scene {
  const char* name;
  std::vector<Placement> blocks; // everything not listed is air
};

std::vector<Placement> floor(int y, int x0, int x1, int z0, int z1, const char* name) {
  std::vector<Placement> out;
  for (int x = x0; x <= x1; ++x)
    for (int z = z0; z <= z1; ++z)
      out.push_back({x, y, z, name, ""});
  return out;
}

void add(std::vector<Placement>* out, int x, int y, int z, const char* name,
         const char* properties = "") {
  out->push_back({x, y, z, name, properties});
}

// `LeavesBlock.updateDistance`'s fixed point, computed over the scene so the
// recorded world starts settled: a leaf's distance is `min(neighbour + 1)` over
// its six neighbours, where a log answers 0 and anything else 7. Recorded rather
// than left to the first tick, so a run's writes are the *change* and not the
// scene's own initialisation. (The same helper the block-tick generator uses,
// and for the same reason.)
void settle_leaf_distances(std::vector<Placement>* blocks) {
  constexpr int kNeighbours[6][3] = {{0, -1, 0}, {0, 1, 0},  {0, 0, -1},
                                     {0, 0, 1},  {-1, 0, 0}, {1, 0, 0}};
  auto distance_at = [blocks](int x, int y, int z) {
    for (const Placement& p : *blocks) {
      if (p.x != x || p.y != y || p.z != z)
        continue;
      if (p.name == "minecraft:oak_log")
        return 0;
      if (p.name != "minecraft:oak_leaves")
        return 7;
      const size_t at = p.properties.find("distance=");
      return at == std::string::npos ? 7 : std::atoi(p.properties.c_str() + at + 9);
    }
    return 7;
  };
  for (int pass = 0; pass < 8; ++pass) {
    bool changed = false;
    for (Placement& p : *blocks) {
      if (p.name != "minecraft:oak_leaves")
        continue;
      int best = 7;
      for (const auto& offset : kNeighbours)
        best = std::min(best, distance_at(p.x + offset[0], p.y + offset[1], p.z + offset[2]) + 1);
      best = std::min(best, 7);
      char text[64];
      std::snprintf(text, sizeof(text), "distance=%d,persistent=false,waterlogged=false", best);
      if (p.properties != text) {
        p.properties = text;
        changed = true;
      }
    }
    if (!changed)
      break;
  }
}

// The world a run starts from. The coordinates are the point of most of them: a
// region is 32 blocks across, so x = 8, 40 and 72 are three regions, and a
// platform spanning x = 24..44 straddles the boundary at 32.
std::vector<Scene> build_scenes() {
  std::vector<Scene> scenes;

  // A stone platform inside one region (x and z from -4 to 6, i.e. 30 blocks
  // from the next boundary): fire burns on it, ages up, and never spreads. The
  // ordinary case, and the one whose trace has no cross-region row at all.
  scenes.push_back(Scene{"platform", floor(kFloorY, -4, 6, -4, 6, "minecraft:stone")});

  // The same platform under each of four regions — work at (8, 8), (40, 8),
  // (8, 40) and (40, 40), i.e. regions (0,0), (1,0), (0,1) and (1,1) — so four
  // placements are due in one tick and the plan's region set, its ranges and the
  // per-region dispatch order are all in one trace. Each placement is eight
  // blocks inside its region's corner, so no write here can cross a boundary.
  scenes.push_back(Scene{"regions", floor(kFloorY, -4, 44, -4, 44, "minecraft:stone")});

  // A log at the last chunk of region 0 with the first leaf of region 1 beside
  // it, and three more neighbours around it (one of them across the chunk
  // boundary in z, which is a third region): breaking the log is a chop whose
  // four entries are draw-free, so the whole trace after it is the rules' own,
  // and their cascades land in three chunks of three regions.
  Scene canopy{"canopy", floor(kFloorY, 24, 36, -2, 4, "minecraft:stone")};
  add(&canopy.blocks, 31, kAirY, 0, "minecraft:oak_log");
  add(&canopy.blocks, 30, kAirY, 0, "minecraft:oak_leaves");
  add(&canopy.blocks, 32, kAirY, 0, "minecraft:oak_leaves");
  add(&canopy.blocks, 31, kAirY, 1, "minecraft:oak_leaves");
  add(&canopy.blocks, 31, kAirY, -1, "minecraft:oak_leaves");
  scenes.push_back(canopy);

  // A flammable platform that crosses the region boundary at x = 32, with a fire
  // on either side of it: region 0's fire spreads into region 1's chunk while
  // region 1 has its own fire to run, which is the cross-region write the residue
  // section of the header describes. The trace is recorded at one lane.
  scenes.push_back(Scene{"edge", floor(kFloorY, 24, 44, -4, 4, "minecraft:oak_planks")});

  for (Scene& scene : scenes)
    settle_leaf_distances(&scene.blocks);
  return scenes;
}

// ---- the runs -----------------------------------------------------------------

struct RecordedRun {
  const char* name;
  const char* scene;
  int ticks;
  int min_due;          // the coverage this run exists for
  bool drains_to_empty; // the queue must be exactly empty after the last tick
  // Whether the trace must be identical at one lane and at `kProbeLanes`. True
  // for every run whose work cannot cross a region boundary inside one tick;
  // false for the one that exists to cross it, where the difference is the
  // documented residue and is measured rather than pinned. A stable run's trace
  // is recorded from the parallel arm.
  bool lane_stable;
  std::vector<Action> actions;
};

// `one_fire` is the ordinary case (one region's clock, no cross-region row at
// all); `four_regions` is one placement in each of four regions — their own
// clocks are drawn from each region's stream, so a tick's plan holds several of
// the four at once rather than all of them at one tick; `chop` is a draw-free
// cascade whose four leaves land in three chunks of three regions; `stale` is the
// entry whose block is gone by the time it comes due, which the dispatch has to
// drop; and `edge` is the fire that spreads into the next region's chunk while
// that region has its own fire to run — the one run recorded at a single lane.
std::vector<RecordedRun> build_runs() {
  std::vector<RecordedRun> runs;
  runs.push_back(
      RecordedRun{"one_fire", "platform", 128, 1, false, true, {{0, true, 2, kAirY, 2}}});
  runs.push_back(RecordedRun{"four_regions",
                             "regions",
                             64,
                             4,
                             false,
                             true,
                             {{0, true, 8, kAirY, 8},
                              {0, true, 40, kAirY, 8},
                              {0, true, 8, kAirY, 40},
                              {0, true, 40, kAirY, 40}}});
  runs.push_back(RecordedRun{"chop", "canopy", 12, 4, true, true, {{0, false, 31, kAirY, 0}}});
  runs.push_back(RecordedRun{
      "stale", "platform", 48, 1, true, true, {{0, true, 2, kAirY, 2}, {4, false, 2, kAirY, 2}}});
  runs.push_back(RecordedRun{
      "edge", "edge", 120, 4, false, false, {{0, true, 30, kAirY, 0}, {0, true, 34, kAirY, 0}}});
  return runs;
}

// ---- one run ------------------------------------------------------------------

struct RunResult {
  std::vector<std::string> rows;
  size_t actions = 0;
  size_t due = 0;
  size_t plans = 0;
  size_t region_rows = 0;
  size_t dispatches = 0;
  size_t states = 0;
  size_t schedules = 0;
  size_t reads = 0;
  size_t sturdies = 0;
  size_t lights = 0;
  size_t writes = 0;
  size_t drops = 0;
  size_t items = 0;
  size_t primes = 0;
  size_t draws = 0;
  size_t units = 0;
  size_t worlds = 0;
  size_t drained_ticks = 0;
  size_t max_regions_in_a_tick = 0;
  int64_t end_now = 0;
  size_t end_pending = 0;
  size_t end_chunks = 0;
  unsigned lanes_used = 1;
  uint64_t world_hash = 0;
  std::vector<std::string> violations;

  size_t counted() const {
    return actions + due + plans + region_rows + dispatches + states + schedules + reads +
           sturdies + lights + writes + items + primes + draws + units + worlds;
  }
};

// One row's kind, and the counters it accounts for. Every row the tool writes
// goes through here, so a new row kind that nobody counts is a violation rather
// than a silent hole in the fixture.
void count_row(RunResult* result, const std::string& row) {
  const size_t first = row.find('\t');
  const std::string_view kind(row.data(), first == std::string::npos ? row.size() : first);
  if (kind == "action") {
    ++result->actions;
  } else if (kind == "due") {
    ++result->due;
  } else if (kind == "plan") {
    ++result->plans;
  } else if (kind == "region") {
    ++result->region_rows;
  } else if (kind == "dispatch") {
    ++result->dispatches;
    const size_t second = row.find('\t', first + 1);
    const size_t third =
        second == std::string::npos ? std::string::npos : row.find('\t', second + 1);
    const std::string_view arm(row.data() + second + 1, third - second - 1);
    if (arm == "drop")
      ++result->drops;
  } else if (kind == "state") {
    ++result->states;
  } else if (kind == "schedule") {
    ++result->schedules;
  } else if (kind == "read") {
    ++result->reads;
  } else if (kind == "sturdy") {
    ++result->sturdies;
  } else if (kind == "light") {
    ++result->lights;
  } else if (kind == "write") {
    ++result->writes;
  } else if (kind == "item") {
    ++result->items;
  } else if (kind == "prime") {
    ++result->primes;
  } else if (kind == "draw i") {
    ++result->draws;
  } else if (kind == "draw u") {
    ++result->units;
  } else if (kind == "world") {
    ++result->worlds;
  } else {
    result->violations.push_back("row kind nobody counts: " + std::string(kind));
  }
}

RunResult run_scene(int64_t seed, const RecordedRun& run, const std::vector<Placement>& start,
                    unsigned lanes) {
  RunResult result;
  result.lanes_used = lanes;
  const mc::sim::RegionGrid grid = region_grid();
  // The tick every row below is stamped with; the loop at the end moves it.
  int tick = 0;
  auto push = [&result, &tick](const char* kind, const std::string& body) {
    result.rows.push_back(std::string(kind) + "\t" + std::to_string(tick) + "\t" + body);
  };
  auto violation = [&result](const std::string& text) { result.violations.push_back(text); };

  // The world: the scene's cells, plus everything the caller and the rules write.
  // A write appends, so the *last* matching entry is what a read finds — which is
  // also what makes the recorded scene's own state the first word on a cell. The
  // lanes share it (`RegionScheduler::run` runs regions in parallel, and a fire's
  // spread reaches into the next region's chunk), so every access is taken under
  // the world's own lock, the way the session's store takes its own.
  std::vector<Placement> world;
  world.reserve(start.size() + 4096);
  for (const Placement& p : start)
    world.push_back(p);
  std::mutex world_mutex;
  auto raw_get = [&world, &world_mutex](int x, int y, int z) {
    std::lock_guard<std::mutex> lock(world_mutex);
    for (auto it = world.rbegin(); it != world.rend(); ++it)
      if (it->x == x && it->y == y && it->z == z)
        return BlockStateKey{it->name, it->properties};
    return air();
  };
  auto raw_set = [&world, &world_mutex](int x, int y, int z, const BlockStateKey& state) {
    std::lock_guard<std::mutex> lock(world_mutex);
    world.push_back(Placement{x, y, z, state.name, state.properties});
  };

  mc::world::BlockTickQueue queue;
  // `g_block_ticks_mutex`'s job: several lanes schedule into one queue and the
  // drain reads its clock.
  std::mutex queue_mutex;
  // What the last schedule for a position said: the tick it was taken in and the
  // tick it comes due. The drain is checked against it, so a due tick the queue's
  // own arithmetic does not produce cannot be written into a fixture.
  std::vector<std::pair<BlockTickPos, std::pair<int, int64_t>>> last_schedule;
  std::mutex schedule_mutex;
  auto remember_schedule = [&last_schedule, &schedule_mutex](const BlockTickPos& pos, int at,
                                                             int64_t due) {
    std::lock_guard<std::mutex> lock(schedule_mutex);
    for (auto& entry : last_schedule)
      if (entry.first == pos) {
        entry.second = std::make_pair(at, due);
        return;
      }
    last_schedule.emplace_back(pos, std::make_pair(at, due));
  };
  auto last_schedule_for = [&last_schedule, &schedule_mutex](const BlockTickPos& pos) {
    std::lock_guard<std::mutex> lock(schedule_mutex);
    for (const auto& entry : last_schedule)
      if (entry.first == pos)
        return std::make_pair(true, entry.second);
    return std::make_pair(false, std::make_pair(0, static_cast<int64_t>(0)));
  };

  mc::sim::RegionScheduler scheduler(lanes);

  // ---- the caller's own work, in the window between two ticks -----------------
  // A placement is the session's path: the state `BaseFireBlock.getState` gives,
  // the write with its notify, then `FireBlock.onPlace`'s own clock. A break is
  // the write to air, whose notify schedules a chopped canopy's leaves. These
  // happen outside any region (a session thread, in a live server), so their rows
  // go straight into the trace and their `onPlace` draw comes from the stream of
  // the region the placement lands in.
  auto run_action = [&](const Action& action) {
    push("action", std::string(action.ignite ? "ignite" : "break") + "\t" +
                       coord(action.x, action.y, action.z));
    const mc::sim::RegionPos region = region_of(grid, action.x, action.z);
    mc::sim::RegionRandom random;
    random.seed(
        mc::sim::region_tick_seed(static_cast<uint64_t>(seed) ^ kFireSeedSalt, region, tick));
    auto action_next_int = [&random, &push](int bound) {
      const int value = random.next_int(bound);
      push("draw i", std::to_string(bound) + "\t" + std::to_string(value));
      return value;
    };
    auto action_get = [&raw_get, &push](int x, int y, int z) {
      const BlockStateKey state = raw_get(x, y, z);
      push("read", coord(x, y, z) + "\t" + canonical_state(state));
      return state;
    };
    auto action_schedule = [&queue, &queue_mutex, &remember_schedule, &push,
                            &tick](int x, int y, int z, int delay) {
      int64_t now = 0;
      {
        std::lock_guard<std::mutex> lock(queue_mutex);
        now = queue.now();
        queue.schedule(x, y, z, delay);
      }
      const int64_t due = now + (delay > 0 ? delay : 1);
      push("schedule", coord(x, y, z) + "\t" + std::to_string(delay) + "\t" + std::to_string(due));
      remember_schedule(BlockTickPos{x, y, z}, tick, due);
    };
    std::function<void(int, int, int, const BlockStateKey&, bool)> action_set;
    RandomTickLevel action_tick_level;
    action_tick_level.get_block = action_get;
    action_tick_level.schedule_tick = action_schedule;
    action_tick_level.set_block = [&action_set](int x, int y, int z, const BlockStateKey& state,
                                                bool notify) {
      action_set(x, y, z, state, notify);
    };
    FireLevel action_level;
    action_level.get_block = action_get;
    action_level.is_sturdy = [&raw_get, &push](int x, int y, int z) {
      const bool sturdy = solidish(raw_get(x, y, z).name);
      push("sturdy", coord(x, y, z) + "\t" + (sturdy ? "true" : "false"));
      return sturdy;
    };
    action_level.schedule_tick = action_schedule;
    action_level.set_block = [&action_set](int x, int y, int z, const BlockStateKey& state,
                                           bool notify) { action_set(x, y, z, state, notify); };
    action_set = [&raw_get, &raw_set, &push, &action_tick_level](
                     int x, int y, int z, const BlockStateKey& state, bool notify) {
      const BlockStateKey old = raw_get(x, y, z); // the adapter's own read, not a rule's
      raw_set(x, y, z, state);
      push("write",
           coord(x, y, z) + "\t" + canonical_state(state) + "\t" + (notify ? "true" : "false"));
      if (notify && old.name != state.name)
        mc::world::leaves_notify_neighbours_of_change(action_tick_level, x, y, z, state);
    };
    if (action.ignite) {
      const BlockStateKey state =
          mc::world::fire_placement_state(action_level, action.x, action.y, action.z);
      action_set(action.x, action.y, action.z, state, /*notify=*/true);
      if (state.name == mc::world::kFireBlock)
        action_schedule(action.x, action.y, action.z,
                        kFireTickMinDelay + action_next_int(kFireTickDelaySpread));
      return;
    }
    action_get(action.x, action.y, action.z);
    action_set(action.x, action.y, action.z, air(), /*notify=*/true);
  };

  // The window before the first tick belongs to whatever was placed before the
  // world started ticking.
  for (const Action& action : run.actions)
    if (action.tick == 0)
      run_action(action);

  for (tick = 1; tick <= run.ticks; ++tick) {
    std::vector<BlockTickPos> drained;
    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      drained = queue.advance();
    }
    // The session's own canonical order: the owning region first, then the
    // position. It is what makes the dispatch order a property of the world
    // rather than of the queue's bucket layout.
    std::sort(drained.begin(), drained.end(),
              [&grid](const BlockTickPos& a, const BlockTickPos& b) {
                const mc::sim::RegionPos ra = region_of(grid, a.x, a.z);
                const mc::sim::RegionPos rb = region_of(grid, b.x, b.z);
                if (ra != rb)
                  return ra < rb;
                if (a.x != b.x)
                  return a.x < b.x;
                if (a.y != b.y)
                  return a.y < b.y;
                return a.z < b.z;
              });
    // The sorted order makes every region's entries one contiguous run, so the
    // dispatch reads them by range rather than by building a second structure.
    std::vector<mc::sim::RegionPos> regions;
    std::vector<std::pair<size_t, size_t>> ranges;
    for (size_t p = 0; p < drained.size(); ++p) {
      const mc::sim::RegionPos region = region_of(grid, drained[p].x, drained[p].z);
      char row[128];
      std::snprintf(row, sizeof(row), "%s\t%s", region_text(region).c_str(),
                    coord(drained[p].x, drained[p].y, drained[p].z).c_str());
      push("due", row);
      char message[256];
      if (p > 0 && drained[p - 1] == drained[p]) {
        std::snprintf(message, sizeof(message), "run '%s': tick %d drained %s twice", run.name,
                      tick, coord(drained[p].x, drained[p].y, drained[p].z).c_str());
        violation(message);
      }
      const auto scheduled = last_schedule_for(drained[p]);
      if (!scheduled.first) {
        std::snprintf(message, sizeof(message),
                      "run '%s': tick %d drained %s with no schedule behind it", run.name, tick,
                      coord(drained[p].x, drained[p].y, drained[p].z).c_str());
        violation(message);
      } else if (scheduled.second.second != tick) {
        std::snprintf(message, sizeof(message),
                      "run '%s': tick %d drained %s, whose last schedule (tick %d) made it due "
                      "at %lld",
                      run.name, tick, coord(drained[p].x, drained[p].y, drained[p].z).c_str(),
                      scheduled.second.first, static_cast<long long>(scheduled.second.second));
        violation(message);
      }
      if (regions.empty() || regions.back().rx != region.rx || regions.back().rz != region.rz) {
        regions.push_back(region);
        ranges.emplace_back(p, p + 1);
      } else {
        ranges.back().second = p + 1;
      }
    }
    if (!drained.empty()) {
      char text[64];
      std::snprintf(text, sizeof(text), "%zu\t%zu", regions.size(), drained.size());
      push("plan", text);
      ++result.drained_ticks;
      result.max_regions_in_a_tick = std::max(result.max_regions_in_a_tick, regions.size());
    }

    // The phase itself: one contiguous range per region, each on its own lane's
    // copy of the level views, all of it joined before a single row is appended.
    // Every row a region's work makes goes into that region's own buffer and is
    // appended below in canonical region order, which is what makes the recorded
    // order a function of the world rather than of which lane finished first.
    std::vector<std::vector<std::string>> rows_by_region(regions.size());
    if (!regions.empty()) {
      const std::function<void(mc::sim::RegionPos, unsigned)> work = [&](mc::sim::RegionPos region,
                                                                         unsigned /*lane*/) {
        size_t index = regions.size();
        for (size_t i = 0; i < regions.size(); ++i)
          if (regions[i].rx == region.rx && regions[i].rz == region.rz) {
            index = i;
            break;
          }
        if (index == regions.size())
          return; // the scheduler runs the list it was handed
        std::vector<std::string>& out = rows_by_region[index];
        auto push_region = [&out, &tick](const char* kind, const std::string& body) {
          out.push_back(std::string(kind) + "\t" + std::to_string(tick) + "\t" + body);
        };
        // The region's own stream for this tick: the modelled `RandomSource`
        // (see the header on why it is the region's and not the lane's).
        mc::sim::RegionRandom random;
        random.seed(
            mc::sim::region_tick_seed(static_cast<uint64_t>(seed) ^ kFireSeedSalt, region, tick));
        const FireRandom fire_random{
            [&random, &push_region](int bound) {
              const int value = random.next_int(bound);
              push_region("draw i", std::to_string(bound) + "\t" + std::to_string(value));
              return value;
            },
            [&random, &push_region]() {
              const float value = random.next_float();
              char text[64];
              std::snprintf(text, sizeof(text), "%.6f", static_cast<double>(value));
              push_region("draw u", text);
              return value;
            }};

        // `Level.scheduleTick`: into the one queue (under its own lock, the
        // way `schedule_block_tick` takes `g_block_ticks_mutex`) and into
        // this region's rows.
        auto schedule_tick = [&queue, &queue_mutex, &remember_schedule, &push_region,
                              &tick](int x, int y, int z, int delay) {
          int64_t now = 0;
          {
            std::lock_guard<std::mutex> lock(queue_mutex);
            now = queue.now();
            queue.schedule(x, y, z, delay);
          }
          const int64_t due = now + (delay > 0 ? delay : 1);
          push_region("schedule",
                      coord(x, y, z) + "\t" + std::to_string(delay) + "\t" + std::to_string(due));
          remember_schedule(BlockTickPos{x, y, z}, tick, due);
        };
        auto get_block = [&raw_get, &push_region](int x, int y, int z) {
          const BlockStateKey state = raw_get(x, y, z);
          push_region("read", coord(x, y, z) + "\t" + canonical_state(state));
          return state;
        };
        auto is_sturdy = [&raw_get, &push_region](int x, int y, int z) {
          const bool sturdy = solidish(raw_get(x, y, z).name);
          push_region("sturdy", coord(x, y, z) + "\t" + (sturdy ? "true" : "false"));
          return sturdy;
        };

        std::function<void(int, int, int, const BlockStateKey&, bool)> set_block;
        RandomTickLevel tick_level;
        tick_level.get_block = get_block;
        tick_level.schedule_tick = schedule_tick;
        tick_level.raw_brightness = [&push_region](int x, int y, int z) {
          push_region("light", coord(x, y, z) + "\t15"); // no light engine: full sky
          return 15;
        };
        tick_level.drop_resources = [&push_region](int x, int y, int z,
                                                   const BlockStateKey& state) {
          push_region("item", coord(x, y, z) + "\t" + canonical_state(state));
        };
        tick_level.set_block = [&set_block](int x, int y, int z, const BlockStateKey& state,
                                            bool notify) { set_block(x, y, z, state, notify); };
        FireLevel fire_level;
        fire_level.get_block = get_block;
        fire_level.is_sturdy = is_sturdy;
        fire_level.schedule_tick = schedule_tick;
        fire_level.set_block = [&set_block](int x, int y, int z, const BlockStateKey& state,
                                            bool notify) { set_block(x, y, z, state, notify); };
        fire_level.prime_tnt = [&push_region](int x, int y, int z) {
          push_region("prime", coord(x, y, z));
        };
        // The session's `set_block` adapter, minus the client broadcast and
        // the redstone fan-out, neither of which schedules a block tick or
        // changes a cell a fire or a leaf reads. The one neighbour reaction
        // it keeps is the leaf one: a notify write that really changed the
        // block tells the adjacent leaves to re-derive their distance, which
        // is `Level.setBlock(..., 3)`'s leaf half.
        set_block = [&raw_get, &raw_set, &push_region,
                     &tick_level](int x, int y, int z, const BlockStateKey& state, bool notify) {
          const BlockStateKey old = raw_get(x, y, z); // the adapter's own read
          raw_set(x, y, z, state);
          push_region("write", coord(x, y, z) + "\t" + canonical_state(state) + "\t" +
                                   (notify ? "true" : "false"));
          if (notify && old.name != state.name)
            mc::world::leaves_notify_neighbours_of_change(tick_level, x, y, z, state);
        };

        // `run_block_tick_position`'s shape: what is standing there *now*
        // decides. The read in front of it is the dispatch's own check, and
        // it is a row like any other rule's read.
        for (size_t p = ranges[index].first; p < ranges[index].second; ++p) {
          const BlockTickPos& pos = drained[p];
          const BlockStateKey state = get_block(pos.x, pos.y, pos.z);
          if (mc::world::fire_is_fire(state)) {
            push_region("dispatch", std::string("fire\t") + coord(pos.x, pos.y, pos.z));
            mc::world::fire_tick(fire_level, fire_random, kDifficulty, pos.x, pos.y, pos.z);
          } else if (mc::world::leaves_is_leaves(state.name)) {
            push_region("dispatch", std::string("leaves\t") + coord(pos.x, pos.y, pos.z));
            mc::world::leaves_scheduled_tick(tick_level, state, pos.x, pos.y, pos.z);
          } else {
            push_region("dispatch", std::string("drop\t") + coord(pos.x, pos.y, pos.z));
          }
        }
      };
      scheduler.run(regions, work);
    }

    // The barrier is past: the region blocks are appended in canonical region
    // order, so the trace says what the world decided and not which lane was
    // quicker.
    for (size_t i = 0; i < regions.size(); ++i) {
      char text[64];
      std::snprintf(text, sizeof(text), "%zu\t%zu", ranges[i].first, ranges[i].second);
      push("region", region_text(regions[i]) + "\t" + text);
      for (const std::string& row : rows_by_region[i])
        result.rows.push_back(row);
    }
    char text[96];
    std::snprintf(text, sizeof(text), "%lld\t%zu\t%zu\t%zu", static_cast<long long>(queue.now()),
                  queue.size(), queue.chunk_count(), regions.size());
    push("state", text);
    // ...and the caller's work in the window after this tick and before the next.
    for (const Action& action : run.actions)
      if (action.tick == tick)
        run_action(action);
  }

  result.end_now = queue.now();
  result.end_pending = queue.size();
  result.end_chunks = queue.chunk_count();

  // The whole world, hashed cell by cell over the box the run touched: the
  // strongest statement the fixture can make about the two lane counts — the
  // same ticks left the same world, not merely the same rows.
  {
    int min_x = 0, max_x = 0, min_z = 0, max_z = 0;
    bool any = false;
    for (const Placement& p : world) {
      if (!any) {
        min_x = max_x = p.x;
        min_z = max_z = p.z;
        any = true;
        continue;
      }
      min_x = std::min(min_x, p.x);
      max_x = std::max(max_x, p.x);
      min_z = std::min(min_z, p.z);
      max_z = std::max(max_z, p.z);
    }
    uint64_t hash = 1469598103934665603ULL;
    if (any) {
      for (int x = min_x; x <= max_x; ++x) {
        for (int z = min_z; z <= max_z; ++z) {
          for (int y = kFloorY - 1; y <= kAirY + 1; ++y) {
            const BlockStateKey state = raw_get(x, y, z);
            hash = fnv1a(hash, state.name.data(), state.name.size());
            hash = fnv1a(hash, state.properties.data(), state.properties.size());
          }
        }
      }
    }
    result.world_hash = hash;
    push("world", std::to_string(hash));
  }

  // The invariants that make the trace a pin of the loop rather than a transcript
  // of one: the counters have to account for every row, a region's rows have to
  // be the ones its range named, the run has to reach the coverage it exists for,
  // and the queue has to end where the run says it does.
  for (const std::string& row : result.rows)
    count_row(&result, row);
  if (result.counted() != result.rows.size()) {
    char message[256];
    std::snprintf(message, sizeof(message), "run '%s': %zu rows, %zu counted by kind", run.name,
                  result.rows.size(), result.counted());
    violation(message);
  }
  if (result.due < static_cast<size_t>(run.min_due)) {
    char message[256];
    std::snprintf(message, sizeof(message),
                  "run '%s': %zu drains, and this run exists for at least %d", run.name, result.due,
                  run.min_due);
    violation(message);
  }
  if (result.plans != result.drained_ticks) {
    char message[256];
    std::snprintf(message, sizeof(message),
                  "run '%s': %zu plans for %zu ticks that drained something", run.name,
                  result.plans, result.drained_ticks);
    violation(message);
  }
  if (run.drains_to_empty && (result.end_pending != 0 || result.end_chunks != 0)) {
    char message[256];
    std::snprintf(message, sizeof(message),
                  "run '%s': the queue still holds %zu entries in %zu chunks", run.name,
                  result.end_pending, result.end_chunks);
    violation(message);
  }
  return result;
}

// ---- the artifacts ------------------------------------------------------------

const Scene* find_scene(const std::vector<Scene>& scenes, const char* name) {
  for (const Scene& scene : scenes)
    if (std::strcmp(scene.name, name) == 0)
      return &scene;
  return nullptr;
}

std::string meta_text(int64_t seed, const std::vector<RecordedRun>& runs,
                      const std::vector<RunResult>& results) {
  std::string text;
  text += "# Phase 5 ordered item 3 follow-up: the region-parallel scheduled-tick loop's\n"
          "# golden run parameters.\n"
          "#\n"
          "# NOT a vanilla dump: what a fire schedules is `30 + nextInt(10)` from a\n"
          "# `RandomSource` whose state belongs to the server, and the partition is this\n"
          "# engine's. `ctest -R region_tick` replays every row below through the real\n"
          "# `BlockTickQueue`, the real canonical sort and the real `RegionScheduler`,\n"
          "# dispatching each region's range through `run_block_tick_position`'s shape,\n"
          "# and compares the whole trace line for line — so an entry sorted into the\n"
          "# wrong region, a range that swallowed a neighbour's work or a dispatch in the\n"
          "# queue's raw order cannot land. `tests/fixtures/block_tick` owns the same\n"
          "# queue on the serial arm and the rules' own streams; this pin owns the\n"
          "# partition.\n"
          "#\n"
          "# `ticks` is how many world ticks the run advanced the queue through (one\n"
          "# `advance()` each), `lanes` the lane count the checked-in trace came from\n"
          "# (the parallel arm for every `laneStable yes` run, one lane for the run that\n"
          "# crosses a region boundary) and `laneStable` whether the trace at one lane\n"
          "# and at four lanes must be identical — it is required of every run whose work\n"
          "# cannot cross a boundary inside one tick, and the `edge` run is the one that\n"
          "# can, where the difference is the residue docs/PHASE5.md §5 documents and the\n"
          "# tool prints rather than pins.\n"
          "#\n"
          "# `schedules`/`due`/`plans`/`regionRows`/`dispatches`/`drops`/`writes`/`reads`/\n"
          "# `draws`/`units`/`states` are the counts of those trace rows, `maxRegions` the\n"
          "# largest region set one tick planned, `endNow`/`endPending`/`endChunks` the\n"
          "# queue's own state after the last tick and `worldHash` the whole world's FNV-1a\n"
          "# over the box the run touched (the A/B's correctness half: the two lane counts\n"
          "# leave the same world). `rows` is the trace's length.\n"
          "#\n"
          "# seed\trun\tscene\tticks\tlanes\tlaneStable\tschedules\tdue\tplans\tregionRows"
          "\tdispatches\tdrops\twrites\treads\tdraws\tunits\tstates\tmaxRegions\tendNow"
          "\tendPending\tendChunks\tworldHash\trows\n";
  for (size_t i = 0; i < runs.size(); ++i) {
    const RunResult& result = results[i];
    char line[768];
    std::snprintf(line, sizeof(line),
                  "%lld\t%s\t%s\t%d\t%u\t%s\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu"
                  "\t%zu\t%zu\t%lld\t%zu\t%zu\t%llu\t%zu\n",
                  static_cast<long long>(seed), runs[i].name, runs[i].scene, runs[i].ticks,
                  result.lanes_used, runs[i].lane_stable ? "yes" : "no", result.schedules,
                  result.due, result.plans, result.region_rows, result.dispatches, result.drops,
                  result.writes, result.reads, result.draws, result.units, result.states,
                  result.max_regions_in_a_tick, static_cast<long long>(result.end_now),
                  result.end_pending, result.end_chunks,
                  static_cast<unsigned long long>(result.world_hash), result.rows.size());
    text += line;
  }
  return text;
}

std::string scene_text(const std::vector<Scene>& scenes) {
  const mc::sim::RegionGrid grid = region_grid();
  std::string text =
      "# The world each recorded run starts from. Everything not listed is air; the\n"
      "# caller's own work (a placement, a break) is in region_tick_actions.tsv, since\n"
      "# a scene can serve more than one run. `rx`/`rz` are the region that owns the\n"
      "# cell under this port's own grid (`chunks_per_region` from the meta header's\n"
      "# constants: two chunks a side, so a region spans 32 blocks), which is what\n"
      "# makes the partition in the traces readable without recomputing it. The leaf\n"
      "# distances are the settled fixed point of `LeavesBlock.updateDistance` over the\n"
      "# scene, so a run's writes are the change and not the scene's own\n"
      "# initialisation.\n"
      "#\n"
      "# kind\tscene\trx\trz\tx\ty\tz\tstate\n";
  for (const Scene& scene : scenes)
    for (const Placement& block : scene.blocks) {
      const mc::sim::RegionPos region = region_of(grid, block.x, block.z);
      char row[360];
      std::snprintf(row, sizeof(row), "block\t%s\t%d\t%d\t%d\t%d\t%d\t%s\n", scene.name, region.rx,
                    region.rz, block.x, block.y, block.z,
                    canonical_state(BlockStateKey{block.name, block.properties}).c_str());
      text += row;
    }
  return text;
}

std::string actions_text(int64_t seed, const std::vector<RecordedRun>& runs) {
  std::string text =
      "# The caller's own work: what a session does between ticks, and the only other\n"
      "# thing that touches this queue. `ignite` is the placement path\n"
      "# (`flint_and_steel` / `/setblock`): the state `BaseFireBlock.getState` gives, the\n"
      "# write with its neighbour notify, then `FireBlock.onPlace`'s own clock. `break`\n"
      "# is a removal: the write to air, whose notify is what schedules a chopped\n"
      "# canopy's leaves. A tick of 0 is the window before the first tick. These run\n"
      "# outside any region (a session thread, in a live server), which is why their\n"
      "# rows sit in the trace's own block and not inside a region's.\n"
      "#\n"
      "# seed\trun\ttick\tkind\tx\ty\tz\n";
  for (const RecordedRun& run : runs)
    for (const Action& action : run.actions) {
      char row[224];
      std::snprintf(row, sizeof(row), "%lld\t%s\t%d\t%s\t%d\t%d\t%d\n",
                    static_cast<long long>(seed), run.name, action.tick,
                    action.ignite ? "ignite" : "break", action.x, action.y, action.z);
      text += row;
    }
  return text;
}

std::string trace_text(int64_t seed, const RecordedRun& run, const RunResult& result) {
  std::string text;
  text += "# Phase 5 ordered item 3 follow-up: the region-parallel scheduled-tick loop's\n";
  text += "# interaction trace for run '";
  text += run.name;
  text += "' (scene '";
  text += run.scene;
  text += "'), seed ";
  text += std::to_string(seed);
  text += ", ";
  text += std::to_string(result.lanes_used);
  text += " lane(s).\n";
  text += "#\n"
          "# NOT a vanilla dump (see region_tick_meta.tsv): every row is one thing the\n"
          "# partition, the rule or the caller did, in order, with the tick it happened in.\n"
          "# `action` is the caller's own scripted work (see region_tick_actions.tsv). A\n"
          "# tick's rows are: the `due` rows — every entry that tick drained, in the\n"
          "# canonical order the region arm sorts them into (owning region, then x, y, z),\n"
          "# each row naming its region; the `plan` row, the number of regions and entries\n"
          "# one `RegionScheduler::run` was handed; then one block per region, in canonical\n"
          "# region order: a `region` row naming the region and the slice of this tick's\n"
          "# `due` list it owned, followed by that region's own rows in the order its lane\n"
          "# did them (`dispatch` — which arm the entry took, fire, leaves or drop — and\n"
          "# everything the rule then logged: `schedule`, `read`, `sturdy`, `light`,\n"
          "# `write`, `item`, `prime`, `draw i`, `draw u`); and finally the `state` row, the\n"
          "# queue's own clock, pending count, chunk count and this tick's region count. The\n"
          "# lanes themselves are not in the trace: the recorded order is the canonical one,\n"
          "# assembled after the barrier, because the lane assignment is the scheduler's own\n"
          "# contract and not the loop's. The `world` row closes the trace with the whole\n"
          "# world's hash.\n"
          "#\n"
          "# kind\ttick\t...\n";
  for (const std::string& row : result.rows)
    text += row + "\n";
  return text;
}

std::vector<std::string> to_lines(const std::string& text) {
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t newline = text.find('\n', pos);
    if (newline == std::string::npos) {
      lines.push_back(text.substr(pos));
      break;
    }
    lines.push_back(text.substr(pos, newline - pos));
    pos = newline + 1;
  }
  return lines;
}

std::string trace_path(const std::string& dir, int64_t seed, const char* run) {
  return dir + "/region_tick_seed" + std::to_string(seed) + "_" + run + ".tsv";
}

bool diff_text(const std::string& label, const std::string& path, const std::string& regenerated) {
  const fixture_diff::Lines disk = fixture_diff::read_lines(path);
  if (!disk.exists)
    std::printf("  (%s does not exist yet)\n", label.c_str());
  fixture_diff::print_row_diff(label, disk.lines, to_lines(regenerated), /*max_rows=*/60);
  return !disk.exists || disk.lines != to_lines(regenerated);
}

std::string hex64(uint64_t value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(value));
  return text;
}

} // namespace

int main(int argc, char** argv) {
  bool diff = false;
  std::string dir;
  int64_t seed = kDefaultSeed;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--diff") == 0) {
      diff = true;
    } else if (dir.empty()) {
      dir = argv[i];
    } else {
      seed = std::strtoll(argv[i], nullptr, 10);
    }
  }
  if (dir.empty()) {
    std::fprintf(stderr, "usage: %s <fixtures_dir> [seed] [--diff]\n", argv[0]);
    return 2;
  }

  const std::vector<Scene> scenes = build_scenes();
  const std::vector<RecordedRun> runs = build_runs();
  std::vector<RunResult> results;
  std::vector<std::string> violations;
  for (const RecordedRun& run : runs) {
    const Scene* scene = find_scene(scenes, run.scene);
    if (scene == nullptr) {
      std::fprintf(stderr, "error: run '%s' names an unknown scene '%s'\n", run.name, run.scene);
      return 2;
    }
    // The A/B: every run twice, one lane and `kProbeLanes`. A stable run's trace
    // must be identical (rows *and* world hash); the unstable one's difference is
    // the documented cross-region residue, so it is reported and not checked.
    RunResult serial = run_scene(seed, run, scene->blocks, 1);
    RunResult parallel = run_scene(seed, run, scene->blocks, kProbeLanes);
    size_t differing = 0;
    const size_t common = std::min(serial.rows.size(), parallel.rows.size());
    for (size_t i = 0; i < common; ++i)
      if (serial.rows[i] != parallel.rows[i])
        ++differing;
    differing += serial.rows.size() > parallel.rows.size()
                     ? serial.rows.size() - parallel.rows.size()
                     : parallel.rows.size() - serial.rows.size();
    if (differing != 0 || serial.world_hash != parallel.world_hash) {
      char message[320];
      std::snprintf(message, sizeof(message),
                    "run '%s': %zu of %zu rows differ between 1 lane and %u, world hash %s%s",
                    run.name, differing, serial.rows.size(), kProbeLanes,
                    serial.world_hash == parallel.world_hash ? "same" : "differ",
                    run.lane_stable ? " (the lane count must not be visible here)"
                                    : " (the documented cross-region residue)");
      if (run.lane_stable)
        violations.push_back(message);
      else
        std::printf("note: %s\n", message);
    } else if (!run.lane_stable) {
      // The unstable run is allowed to agree: whether the two lanes actually
      // interleave on a write that crossed the boundary is the machine's
      // timing, so "no difference observed" is a result too. It is reported
      // either way, and never checked in.
      std::printf("note: run '%s': the two lane counts produced the identical trace this run "
                  "(no cross-region interleaving observed)\n",
                  run.name);
    }
    results.push_back(run.lane_stable ? std::move(parallel) : std::move(serial));
    for (const std::string& violation : results.back().violations)
      violations.push_back(violation);
  }
  for (size_t i = 0; i < runs.size(); ++i) {
    std::printf("run %-13s %5zu rows, %4zu due, %4zu schedules, %3zu dispatches, max %zu regions, "
                "endPending %zu, world %s%s\n",
                runs[i].name, results[i].rows.size(), results[i].due, results[i].schedules,
                results[i].dispatches, results[i].max_regions_in_a_tick, results[i].end_pending,
                hex64(results[i].world_hash).c_str(),
                runs[i].lane_stable ? "" : " (laneStable no: recorded at one lane)");
  }
  if (!violations.empty()) {
    for (const std::string& violation : violations)
      std::fprintf(diff ? stdout : stderr, diff ? "!! invariant: %s\n" : "error: %s\n",
                   violation.c_str());
    if (!diff) {
      std::fprintf(stderr, "refusing to write a fixture that pins this\n");
      return 1;
    }
  }

  const std::string meta_path = dir + "/region_tick_meta.tsv";
  const std::string scene_path = dir + "/region_tick_scene.tsv";
  const std::string actions_path = dir + "/region_tick_actions.tsv";
  const std::string meta = meta_text(seed, runs, results);
  const std::string scene = scene_text(scenes);
  const std::string actions = actions_text(seed, runs);

  if (diff) {
    std::printf("region-tick fixture: %zu runs, %u lanes probed\n", runs.size(), kProbeLanes);
    bool changed = false;
    changed |= diff_text("region_tick_meta.tsv", meta_path, meta);
    changed |= diff_text("region_tick_scene.tsv", scene_path, scene);
    changed |= diff_text("region_tick_actions.tsv", actions_path, actions);
    for (size_t i = 0; i < runs.size(); ++i)
      changed |= diff_text(
          std::string("region_tick_seed") + std::to_string(seed) + "_" + runs[i].name + ".tsv",
          trace_path(dir, seed, runs[i].name), trace_text(seed, runs[i], results[i]));
    if (!changed) {
      std::printf("summary: in sync with this run's output\n");
      return 0;
    }
    std::printf("summary: the checked-in fixture is OUT OF SYNC\n");
    std::printf("re-run without --diff to rewrite it, then `ctest -R region_tick`\n");
    return 1;
  }

  struct File {
    const char* path;
    const std::string* text;
  };
  const File files[] = {
      {meta_path.c_str(), &meta}, {scene_path.c_str(), &scene}, {actions_path.c_str(), &actions}};
  for (const File& file : files) {
    std::FILE* handle = std::fopen(file.path, "w");
    if (handle == nullptr) {
      std::fprintf(stderr, "error: cannot write %s\n", file.path);
      return 1;
    }
    std::fputs(file.text->c_str(), handle);
    std::fclose(handle);
    std::printf("wrote %s\n", file.path);
  }
  for (size_t i = 0; i < runs.size(); ++i) {
    const std::string path = trace_path(dir, seed, runs[i].name);
    std::FILE* trace = std::fopen(path.c_str(), "w");
    if (trace == nullptr) {
      std::fprintf(stderr, "error: cannot write %s\n", path.c_str());
      return 1;
    }
    std::fputs(trace_text(seed, runs[i], results[i]).c_str(), trace);
    std::fclose(trace);
    std::printf("wrote region_tick_seed%lld_%s.tsv (%zu rows, %u lanes, %zu drops)\n",
                static_cast<long long>(seed), runs[i].name, results[i].rows.size(),
                results[i].lanes_used, results[i].drops);
  }
  return 0;
}
