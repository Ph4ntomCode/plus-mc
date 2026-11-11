// SPDX-License-Identifier: GPL-3.0-or-later
//
// Writes the scheduled block-tick loop's golden output for a small set of
// recorded scenes: the run parameters (`block_tick_meta.tsv`), the world each run
// starts from (`block_tick_scene.tsv`), the caller's own scripted work
// (`block_tick_actions.tsv`) and one interaction trace per run
// (`block_tick_seed42_<run>.tsv`).
//
// This is **not** a vanilla dump, and cannot be. What a fire schedules is
// `30 + random.nextInt(10)` — a draw from the level's own `RandomSource`, whose
// *state* on any tick belongs to the server — so there is no jar output to
// compare against; the fixture is a regression pin of this runtime's own
// behaviour. And what it pins is the half the scripted-draw half cannot:
// `tests/fire_test.cpp` scripts every draw, so it holds each *branch* of
// `FireBlock.tick` one call at a time, and its `test_block_tick_queue` holds the
// queue's own clock on its own — neither ever runs the loop.
//
// What a run is: `ticks` world ticks. In each one the queue is advanced and every
// position it hands back is dispatched by what is standing there *now*
// (`LevelTicks.run`'s check, `run_block_tick_position` in src/server/session.cpp:
// a fire ticks as fire, a leaf as a leaf, anything else is dropped) — and then
// the caller's own scripted work for that tick happens, which is a placement or a
// break, the two things a session does between ticks. Everything the rules and
// the caller do to the queue, to the world and to the recorded stream is in the
// trace, in order:
//
//   action   <tick> <ignite|break> <x> <y> <z>      (the caller's scripted work)
//   due      <tick> <x> <y> <z>                     (one position the drain ran)
//   state    <tick> <now> <pending> <chunks>        (the queue after the drain)
//   schedule <tick> <x> <y> <z> <delay> <due>       (`Level.scheduleTick`)
//   read     <tick> <x> <y> <z> <state>             (`get_block`)
//   sturdy   <tick> <x> <y> <z> <true|false>        (`is_sturdy`)
//   light    <tick> <x> <y> <z> <level>             (`getRawBrightness`)
//   write    <tick> <x> <y> <z> <state> <notify>    (`set_block`)
//   drop     <tick> <x> <y> <z> <state>             (the dispatch refused it)
//   item     <tick> <x> <y> <z> <state>             (`drop_resources`)
//   prime    <tick> <x> <y> <z>                     (`TntBlock.prime`)
//   draw i   <tick> <bound> <result>                (`nextInt(bound)`)
//   draw u   <tick> <result>                        (`nextFloat`)
//
// **The queue's return order is deliberately not pinned.** `advance()` walks an
// `unordered_map`, so its order is the bucket layout's — which differs between
// libstdc++ and libc++ and is why the session's region path sorts the drained
// positions (region, then x, y, z) before dispatching them while the serial path
// does not. A fixture that pinned the raw order would fail on a Linux build for a
// reason that is not a regression. So each tick's `due` rows are written in that
// canonical order — the order the region path dispatches them — and what the
// fixture holds is *which* positions are due in which tick, plus everything each
// of them did.
//
// A schedule's due tick is `now + max(delay, 1)` (`BlockTickQueue::schedule`'s
// own arithmetic), where `now` is the tick the schedule was taken in, so the
// drain tick is exactly `delay` ticks later: `write_and_notify`'s one-tick leaf
// delay is why a chop walks the canopy one layer per tick. Every `due` row is
// checked against that arithmetic rather than merely satisfying it — the tool
// refuses to write a fixture whose drains do not land on the tick their last
// schedule named.
//
// The stream is a `WgLegacyRandom` seeded with the run's seed: the level's own
// `RandomSource` flavour (`LegacyRandomSource`), ported exactly. The live server
// hands fire a thread-local `std::mt19937` it seeds from `std::random_device`, so
// this fixture pins the loop, the dispatch and the rules' *draw order*, not the
// values a running server would draw — the same bargain the other dumps make.
//
// One run exists for a behaviour rather than a happy path: `blob` — two adjacent
// leaves whose log is gone. They do not both jump to the ceiling; each recomputes
// from the other, so they chase each other's distance up one step per tick, and
// the last recompute yields the distance the leaf already had. `leaves.cpp`'s
// `leaves_scheduled_tick` skips the write *and* the neighbour update for that
// case, which is vanilla's own no-op (`LevelChunk.setBlockState` returns null for
// an unchanged state, so `Level.setBlock` skips both — `build/_decomp`'s
// `LevelChunk.java:268`, `StateHolder.java:125`), so the chase converges at the
// ceiling and the queue drains: the meta records `endPending` 0 and the trace's
// last ticks hold no `due` row at all. (Before that branch, `write_and_notify`
// re-notified an unchanged write and the two leaves rescheduled each other
// forever — the standing load `docs/PHASE3.md` recorded, pinned here as it then
// behaved and changed deliberately, after this `--diff`, once the rule landed.)
//
// Two things are deliberately not modelled, and the session's own adapters do
// both: the client broadcast and redstone fan-out a write raises (neither touches
// a cell a fire or a leaf reads, and neither schedules a block tick), and weather
// (`FireLevel::is_raining` is left null, i.e. "not raining", which is the
// documented answer for a caller with no weather — `tests/fixtures/weather` and
// `tests/fire_test.cpp`'s scripted half own the rain branches).
//
// Usage:
//   plusmc-block-tick-dump <fixtures_dir> [seed] [--diff]
//
// `--diff` writes nothing: it re-runs every scene in memory, reads what is
// checked in, and prints a patch of each file that would change (invariant
// violations included), exiting 1 while the fixture differs. The review is
//
//   ./build/plusmc-block-tick-dump <fixtures_dir> --diff   # read the patch
//   ./build/plusmc-block-tick-dump <fixtures_dir>          # then rewrite it
//   ctest -R block_tick                                    # and re-pin it
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fixture_diff.h"
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
// recorded world starts settled: a leaf's distance is
// `min(getDistanceAt(neighbour) + 1)` over its six neighbours, where a log
// answers 0 and anything else 7. Recorded rather than left to the first tick, so
// a run's writes are the *change* and not the scene's own initialisation.
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
  // Bounded by the distance ceiling: every leaf relaxes at most seven times.
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

// The world a run starts from. Small on purpose: the reads a fire's spread makes
// are the bulk of a trace, so each scene is the smallest one that reaches the
// branch it is there for.
std::vector<Scene> build_scenes() {
  std::vector<Scene> scenes;

  // A stone platform: fire burns on it, ages up, and never spreads (nothing
  // around it is flammable). The ordinary case.
  scenes.push_back(Scene{"platform", floor(kFloorY, -4, 6, -4, 6, "minecraft:stone")});

  // A wide platform, so two placements 20 blocks apart in x land in different
  // chunks (0 and 1) and their due entries interleave in one tick.
  scenes.push_back(Scene{"wide", floor(kFloorY, -4, 22, -2, 6, "minecraft:stone")});

  // A wooden platform with a leaf hanging two cells above the fire and two cells
  // across from it: the fire spreads into the planks, and the spread's own write
  // lands in a cell beside the leaf, which has to re-derive its distance. That is
  // the one place a *fire* write reaches the leaf module (the session's own
  // `set_block` adapter does exactly this), and the leaf is out of the fire's
  // burn-out reach until a flame lands next to it — which is also why the run is
  // long enough for the spread to get there.
  Scene wood{"wood", floor(kFloorY, -3, 5, -3, 5, "minecraft:oak_planks")};
  add(&wood.blocks, 0, -57, 2, "minecraft:oak_leaves");
  scenes.push_back(wood);

  // A log with four leaves around it, none of the four adjacent to another (the
  // four orthogonal neighbours of one cell are pairwise diagonal). Breaking the
  // log is the chop: all four schedule in one tick, all four recompute, and the
  // queue drains — a cascade that settles.
  Scene trunk{"trunk", floor(kFloorY, -2, 2, -2, 2, "minecraft:stone")};
  add(&trunk.blocks, 0, kAirY, 0, "minecraft:oak_log");
  add(&trunk.blocks, 1, kAirY, 0, "minecraft:oak_leaves");
  add(&trunk.blocks, -1, kAirY, 0, "minecraft:oak_leaves");
  add(&trunk.blocks, 0, kAirY, 1, "minecraft:oak_leaves");
  add(&trunk.blocks, 0, kAirY, -1, "minecraft:oak_leaves");
  scenes.push_back(trunk);

  // Two *adjacent* leaves anchored by one log, and the log broken. Neither jumps
  // to 7: each recomputes from the other, so they chase each other's distance up
  // one step per tick until both hit the ceiling — and then they stop, because the
  // recompute that yields the distance the leaf already had writes nothing and
  // notifies nothing (`leaves_scheduled_tick`; vanilla's `LevelChunk.setBlockState`
  // no-op, which the tool's header cites). This scene pins both halves of that:
  // the chase, and the tick where it ends, so the run is long enough to watch the
  // queue drain and stay drained.
  Scene blob{"blob", floor(kFloorY, -2, 3, -2, 2, "minecraft:stone")};
  add(&blob.blocks, 0, kAirY, 0, "minecraft:oak_log");
  add(&blob.blocks, 1, kAirY, 0, "minecraft:oak_leaves");
  add(&blob.blocks, 2, kAirY, 0, "minecraft:oak_leaves");
  scenes.push_back(blob);

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
  std::vector<Action> actions;
};

// `one_fire` and `two_chunks` are the ordinary case (one fire's clock, and two
// fires in two chunks); `wood` is the flammable world where a fire spreads,
// burns out and reaches a leaf through its own write; `trunk` is a chop that
// settles in one pass; `blob` is the same chop where the leaves are adjacent, so
// they chase each other up to the ceiling and settle on the tick whose recompute
// changes nothing; `stale` is the entry whose block is gone by the time it comes
// due; and `relight` schedules the same position twice, which the queue rewrites
// rather than stacking.
std::vector<RecordedRun> build_runs() {
  std::vector<RecordedRun> runs;
  runs.push_back(RecordedRun{"one_fire", "platform", 128, 1, false, {{0, true, 2, kAirY, 2}}});
  runs.push_back(RecordedRun{
      "two_chunks", "wide", 64, 2, false, {{0, true, 1, kAirY, 2}, {0, true, 21, kAirY, 2}}});
  runs.push_back(RecordedRun{"wood", "wood", 200, 2, false, {{0, true, 2, kAirY, 2}}});
  runs.push_back(RecordedRun{"trunk", "trunk", 12, 4, true, {{0, false, 0, kAirY, 0}}});
  // The chase's seven recomputes (tick 1's dispatch and six neighbours' worth of
  // layers after it), then the ticks that find nothing due: `min_due` is the last
  // tick the chase reaches, and the queue must be empty at the end of the run.
  runs.push_back(RecordedRun{"blob", "blob", 10, 7, true, {{0, false, 0, kAirY, 0}}});
  // The doused fire: placed, then removed four ticks later, so the entry it
  // scheduled is still pending when the tick comes due and the dispatch has to
  // drop it.
  runs.push_back(RecordedRun{
      "stale", "platform", 48, 1, true, {{0, true, 2, kAirY, 2}, {4, false, 2, kAirY, 2}}});
  // The same cell placed twice: `LevelChunkTicks.schedule` rewrites the pending
  // entry's due time, it does not stack a second one.
  runs.push_back(RecordedRun{
      "relight", "platform", 40, 1, false, {{0, true, 2, kAirY, 2}, {4, true, 2, kAirY, 2}}});
  return runs;
}

// ---- one run ------------------------------------------------------------------

struct RunResult {
  std::vector<std::string> rows;
  size_t actions = 0;
  size_t due = 0;
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
  int64_t end_now = 0;
  size_t end_pending = 0;
  size_t end_chunks = 0;
  std::vector<std::string> violations;
};

RunResult run_scene(int64_t seed, const RecordedRun& run, const std::vector<Placement>& start) {
  RunResult result;
  // The tick every row below is stamped with; the loop at the end moves it.
  int tick = 0;
  auto push = [&result, &tick](const char* kind, const std::string& body) {
    result.rows.push_back(std::string(kind) + "\t" + std::to_string(tick) + "\t" + body);
  };
  auto violation = [&result](const std::string& text) { result.violations.push_back(text); };

  // The world: the scene's cells, plus everything the caller and the rules write.
  // A write appends, so the *last* matching entry is what a read finds — which is
  // also what makes the recorded scene's own state the first word on a cell.
  std::vector<Placement> world;
  world.reserve(start.size() + 64);
  for (const Placement& p : start)
    world.push_back(p);
  auto raw_get = [&world](int x, int y, int z) {
    for (auto it = world.rbegin(); it != world.rend(); ++it)
      if (it->x == x && it->y == y && it->z == z)
        return BlockStateKey{it->name, it->properties};
    return air();
  };
  auto raw_set = [&world](int x, int y, int z, const BlockStateKey& state) {
    world.push_back(Placement{x, y, z, state.name, state.properties});
  };

  mc::world::BlockTickQueue queue;
  WgLegacyRandom randomness(seed);
  auto next_int = [&randomness, &result, &push](int bound) {
    const int value = randomness.next_int(bound);
    ++result.draws;
    char text[96];
    std::snprintf(text, sizeof(text), "%d\t%d", bound, value);
    push("draw i", text);
    return value;
  };
  auto next_float = [&randomness, &result, &push]() {
    const float value = randomness.next_float();
    ++result.units;
    char text[64];
    std::snprintf(text, sizeof(text), "%.6f", static_cast<double>(value));
    push("draw u", text);
    return value;
  };

  // The reads are logged as they happen, so the order the gates consult the world
  // is the order the trace shows.
  auto get_block = [&raw_get, &result, &push](int x, int y, int z) {
    const BlockStateKey state = raw_get(x, y, z);
    ++result.reads;
    push("read", coord(x, y, z) + "\t" + canonical_state(state));
    return state;
  };
  auto is_sturdy = [&raw_get, &result, &push](int x, int y, int z) {
    const bool sturdy = solidish(raw_get(x, y, z).name);
    ++result.sturdies;
    push("sturdy", coord(x, y, z) + "\t" + (sturdy ? "true" : "false"));
    return sturdy;
  };

  // What the last schedule for a position said: the tick it was taken in and the
  // tick it comes due. The drain is checked against it, so a due tick the queue's
  // own arithmetic does not produce cannot be written into a fixture.
  std::vector<std::pair<BlockTickPos, std::pair<int, int64_t>>> last_schedule;
  auto remember_schedule = [&last_schedule](const BlockTickPos& pos, int at, int64_t due) {
    for (auto& entry : last_schedule)
      if (entry.first == pos) {
        entry.second = std::make_pair(at, due);
        return;
      }
    last_schedule.emplace_back(pos, std::make_pair(at, due));
  };
  auto last_schedule_for =
      [&last_schedule](const BlockTickPos& pos) -> const std::pair<int, int64_t>* {
    for (const auto& entry : last_schedule)
      if (entry.first == pos)
        return &entry.second;
    return nullptr;
  };

  auto schedule_tick = [&queue, &result, &push, &remember_schedule, &tick](int x, int y, int z,
                                                                           int delay) {
    // `BlockTickQueue::schedule`'s own arithmetic: due `max(delay, 1)` ticks from
    // now. `now` is the tick this schedule is taken in (the queue has been
    // advanced for it), so a delay D comes due — and drains — exactly D ticks
    // later. The drain re-derives that below rather than trusting it.
    const int64_t due = queue.now() + (delay > 0 ? delay : 1);
    queue.schedule(x, y, z, delay);
    ++result.schedules;
    char text[160];
    std::snprintf(text, sizeof(text), "%s\t%d\t%lld", coord(x, y, z).c_str(), delay,
                  static_cast<long long>(due));
    push("schedule", text);
    remember_schedule(BlockTickPos{x, y, z}, tick, due);
  };

  // The write half, shared by both level views and the caller's own actions — the
  // session's `set_block` adapter, minus the client broadcast and the redstone
  // fan-out, neither of which schedules a block tick or changes a cell a fire or a
  // leaf reads. The one neighbour reaction it keeps is the leaf one: a flag-3
  // write that really changed the block tells the adjacent leaves to re-derive
  // their distance, which is `Level.setBlock(..., 3)`'s leaf half.
  std::function<void(int, int, int, const BlockStateKey&, bool)> set_block;
  RandomTickLevel tick_level;
  tick_level.get_block = get_block;
  tick_level.schedule_tick = schedule_tick;
  tick_level.raw_brightness = [&result, &push](int x, int y, int z) {
    ++result.lights;
    push("light", coord(x, y, z) + "\t15"); // no light engine here: full sky
    return 15;
  };
  tick_level.drop_resources = [&result, &push](int x, int y, int z, const BlockStateKey& state) {
    ++result.items;
    push("item", coord(x, y, z) + "\t" + canonical_state(state));
  };
  tick_level.set_block = [&set_block](int x, int y, int z, const BlockStateKey& state,
                                      bool notify) { set_block(x, y, z, state, notify); };

  FireLevel fire_level;
  fire_level.get_block = get_block;
  fire_level.is_sturdy = is_sturdy;
  fire_level.schedule_tick = schedule_tick;
  fire_level.set_block = [&set_block](int x, int y, int z, const BlockStateKey& state,
                                      bool notify) { set_block(x, y, z, state, notify); };
  fire_level.prime_tnt = [&result, &push](int x, int y, int z) {
    ++result.primes;
    push("prime", coord(x, y, z));
  };
  // `is_raining` / `is_raining_at` stay null: the documented answer for a caller
  // with no weather is "not raining", and the rain branches are the weather
  // fixture's and `fire_test.cpp`'s to pin.

  set_block = [&raw_get, &raw_set, &result, &push,
               &tick_level](int x, int y, int z, const BlockStateKey& state, bool notify) {
    const BlockStateKey old = raw_get(x, y, z); // the guard's own read, not a rule's
    raw_set(x, y, z, state);
    ++result.writes;
    push("write",
         coord(x, y, z) + "\t" + canonical_state(state) + "\t" + (notify ? "true" : "false"));
    if (notify && old.name != state.name)
      mc::world::leaves_notify_neighbours_of_change(tick_level, x, y, z, state);
  };

  const FireRandom fire_random{next_int, next_float};

  // The caller's own work: the session's placement path (the state `BaseFireBlock
  // .getState` gives, the write with its notify, then the block's own clock —
  // `FireBlock.onPlace`'s 30 + nextInt(10); soul fire is inert) and its break path
  // (the write to air, whose neighbour notify is what schedules a chopped
  // canopy).
  auto run_action = [&](const Action& action) {
    push("action", std::string(action.ignite ? "ignite" : "break") + "\t" +
                       coord(action.x, action.y, action.z));
    ++result.actions;
    if (action.ignite) {
      const BlockStateKey state =
          mc::world::fire_placement_state(fire_level, action.x, action.y, action.z);
      set_block(action.x, action.y, action.z, state, /*notify=*/true);
      if (state.name == mc::world::kFireBlock)
        schedule_tick(action.x, action.y, action.z,
                      kFireTickMinDelay + next_int(kFireTickDelaySpread));
      return;
    }
    get_block(action.x, action.y, action.z);
    set_block(action.x, action.y, action.z, air(), /*notify=*/true);
  };

  // The window before the first tick belongs to whatever was placed before the
  // world started ticking.
  for (const Action& action : run.actions)
    if (action.tick == 0)
      run_action(action);

  for (tick = 1; tick <= run.ticks; ++tick) {
    const std::vector<BlockTickPos> drained = queue.advance();
    std::vector<BlockTickPos> ordered = drained;
    std::sort(ordered.begin(), ordered.end(), [](const BlockTickPos& a, const BlockTickPos& b) {
      if (a.x != b.x)
        return a.x < b.x;
      if (a.y != b.y)
        return a.y < b.y;
      return a.z < b.z;
    });
    for (size_t i = 0; i < ordered.size(); ++i) {
      push("due", coord(ordered[i].x, ordered[i].y, ordered[i].z));
      ++result.due;
      char message[256];
      if (i > 0 && ordered[i - 1] == ordered[i]) {
        std::snprintf(message, sizeof(message), "run '%s': tick %d drained %s twice", run.name,
                      tick, coord(ordered[i].x, ordered[i].y, ordered[i].z).c_str());
        violation(message);
      }
      const std::pair<int, int64_t>* scheduled = last_schedule_for(ordered[i]);
      if (scheduled == nullptr) {
        std::snprintf(message, sizeof(message),
                      "run '%s': tick %d drained %s with no schedule behind it", run.name, tick,
                      coord(ordered[i].x, ordered[i].y, ordered[i].z).c_str());
        violation(message);
      } else if (scheduled->second != tick) {
        std::snprintf(message, sizeof(message),
                      "run '%s': tick %d drained %s, whose last schedule (tick %d) made it due "
                      "at %lld",
                      run.name, tick, coord(ordered[i].x, ordered[i].y, ordered[i].z).c_str(),
                      scheduled->first, static_cast<long long>(scheduled->second));
        violation(message);
      }
    }
    // The dispatch: what is standing there *now* decides, which is
    // `LevelTicks.run`'s own check. Everything the block then does logs itself.
    for (const BlockTickPos& pos : ordered) {
      const BlockStateKey state = get_block(pos.x, pos.y, pos.z);
      if (mc::world::fire_is_fire(state)) {
        mc::world::fire_tick(fire_level, fire_random, kDifficulty, pos.x, pos.y, pos.z);
      } else if (mc::world::leaves_is_leaves(state.name)) {
        mc::world::leaves_scheduled_tick(tick_level, state, pos.x, pos.y, pos.z);
      } else {
        ++result.drops;
        push("drop", coord(pos.x, pos.y, pos.z) + "\t" + canonical_state(state));
      }
    }
    char text[96];
    std::snprintf(text, sizeof(text), "%lld\t%zu\t%zu", static_cast<long long>(queue.now()),
                  queue.size(), queue.chunk_count());
    push("state", text);
    ++result.states;
    // ...and the caller's work in the window after this tick and before the next.
    for (const Action& action : run.actions)
      if (action.tick == tick)
        run_action(action);
  }

  result.end_now = queue.now();
  result.end_pending = queue.size();
  result.end_chunks = queue.chunk_count();

  // The invariants that make the trace a pin of the loop rather than a transcript
  // of one: the counters have to account for every row, the run has to reach the
  // coverage it exists for, and the queue has to end where the run says it does.
  const size_t counted = result.actions + result.due + result.states + result.schedules +
                         result.reads + result.sturdies + result.lights + result.writes +
                         result.drops + result.items + result.primes + result.draws + result.units;
  if (counted != result.rows.size()) {
    char message[256];
    std::snprintf(message, sizeof(message), "run '%s': %zu rows, %zu counted by kind", run.name,
                  result.rows.size(), counted);
    result.violations.push_back(message);
  }
  if (result.due < static_cast<size_t>(run.min_due)) {
    char message[256];
    std::snprintf(message, sizeof(message),
                  "run '%s': %zu drains, and this run exists for at least %d", run.name, result.due,
                  run.min_due);
    result.violations.push_back(message);
  }
  if (run.drains_to_empty && (result.end_pending != 0 || result.end_chunks != 0)) {
    char message[256];
    std::snprintf(message, sizeof(message),
                  "run '%s': the queue still holds %zu entries in %zu chunks", run.name,
                  result.end_pending, result.end_chunks);
    result.violations.push_back(message);
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
  text += "# Phase 3 milestone G follow-up: the scheduled block-tick loop's golden run\n"
          "# parameters.\n"
          "#\n"
          "# NOT a vanilla dump: what a fire schedules is `30 + random.nextInt(10)` from the\n"
          "# level's own `RandomSource`, whose state on any tick belongs to the server, so\n"
          "# this is a regression pin of this runtime's own behaviour rather than a\n"
          "# comparison against the game jar. `ctest -R block_tick` replays every row\n"
          "# below through the real `BlockTickQueue` and the real dispatch\n"
          "# (`run_block_tick_position`'s shape: a fire ticks as fire, a leaf as a leaf,\n"
          "# anything else is dropped), and compares the whole trace line for line, so a\n"
          "# reschedule one tick early or a dropped entry that silently ran cannot land.\n"
          "# Regenerate deliberately with `plusmc-block-tick-dump <fixtures_dir>`, reading\n"
          "# `--diff`'s patch first.\n"
          "#\n"
          "# `ticks` is how many world ticks the run advanced the queue through (one\n"
          "# `advance()` each), `schedules`/`due`/`drops`/`writes`/`reads` are the counts\n"
          "# of those trace rows, `draws`/`units` the two halves of the recorded stream\n"
          "# (integer and float draws) and `endPending`/`endChunks` the queue's own state\n"
          "# after the last tick — a fire that survives is still pending, and a settled\n"
          "# chop is zero, `blob` (two adjacent leaves whose log is gone) included: its\n"
          "# last recompute changes nothing, and a write that changes nothing is skipped\n"
          "# the way `LevelChunk.setBlockState`'s unchanged-state early return skips it\n"
          "# (the tool's header has the branch and its history). `rows` is the trace's\n"
          "# length.\n"
          "#\n"
          "# seed\trun\tscene\tticks\tschedules\tdue\tdrops\twrites\treads\tunits\tdraws"
          "\tendNow\tendPending\tendChunks\trows\n";
  for (size_t i = 0; i < runs.size(); ++i) {
    const RunResult& result = results[i];
    char line[512];
    std::snprintf(line, sizeof(line),
                  "%lld\t%s\t%s\t%d\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%lld\t%zu\t%zu\t%zu\n",
                  static_cast<long long>(seed), runs[i].name, runs[i].scene, runs[i].ticks,
                  result.schedules, result.due, result.drops, result.writes, result.reads,
                  result.units, result.draws, static_cast<long long>(result.end_now),
                  result.end_pending, result.end_chunks, result.rows.size());
    text += line;
  }
  return text;
}

std::string scene_text(const std::vector<Scene>& scenes) {
  std::string text =
      "# The world each recorded run starts from. Everything not listed is air; the\n"
      "# caller's own work (a placement, a break) is in block_tick_actions.tsv, since a\n"
      "# scene can serve more than one run. The leaf distances are the settled fixed\n"
      "# point of `LeavesBlock.updateDistance` over the scene, so a run's writes are the\n"
      "# change and not the scene's own initialisation.\n"
      "#\n"
      "# kind\tscene\tx\ty\tz\tstate\n";
  for (const Scene& scene : scenes)
    for (const Placement& block : scene.blocks) {
      char row[320];
      std::snprintf(row, sizeof(row), "block\t%s\t%d\t%d\t%d\t%s\n", scene.name, block.x, block.y,
                    block.z, canonical_state(BlockStateKey{block.name, block.properties}).c_str());
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
      "# canopy's leaves. A tick of 0 is the window before the first tick.\n"
      "#\n"
      "# seed\trun\ttick\tkind\tx\ty\tz\n";
  for (const RecordedRun& run : runs)
    for (const Action& action : run.actions) {
      char row[160];
      std::snprintf(row, sizeof(row), "%lld\t%s\t%d\t%s\t%d\t%d\t%d\n",
                    static_cast<long long>(seed), run.name, action.tick,
                    action.ignite ? "ignite" : "break", action.x, action.y, action.z);
      text += row;
    }
  return text;
}

std::string trace_text(int64_t seed, const RecordedRun& run, const RunResult& result) {
  std::string text;
  text += "# Phase 3 milestone G follow-up: the scheduled block-tick loop's interaction trace\n";
  text += "# for run '";
  text += run.name;
  text += "' (scene '";
  text += run.scene;
  text += "'), seed ";
  text += std::to_string(seed);
  text += ".\n";
  text += "#\n"
          "# NOT a vanilla dump (see block_tick_meta.tsv): every row is one thing the loop,\n"
          "# a rule or the caller did, in order, with the tick it happened in. `action` is\n"
          "# the caller's scripted work (see block_tick_actions.tsv); `due` is the drain —\n"
          "# one row per position, in the canonical order the region path dispatches them\n"
          "# (the queue's own order is the hash map's and is deliberately not pinned; see the\n"
          "# tool's header); `state` closes each tick with the queue's own clock, pending\n"
          "# count and non-empty chunk count; `schedule` is `Level.scheduleTick` with the\n"
          "# delay the block asked for and the due tick that follows from it; `read`,\n"
          "# `sturdy` and `light` are the world questions the rules ask (a fire's spread\n"
          "# walks a 3 x 3 x 5 box, which is most of what these rows are); `write` is a\n"
          "# block write with vanilla's client flag — the age-only write is the one with\n"
          "# `false`, which is also why it notifies no neighbour; `drop` is a due position\n"
          "# the dispatch refused (the block that scheduled it is gone); `item`/`prime` are\n"
          "# the two hooks the scheduled path can reach; `draw i`/`draw u` are the recorded\n"
          "# stream's two halves, in order.\n"
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
  return dir + "/block_tick_seed" + std::to_string(seed) + "_" + run + ".tsv";
}

bool diff_text(const std::string& label, const std::string& path, const std::string& regenerated) {
  const fixture_diff::Lines disk = fixture_diff::read_lines(path);
  if (!disk.exists)
    std::printf("  (%s does not exist yet)\n", label.c_str());
  fixture_diff::print_row_diff(label, disk.lines, to_lines(regenerated), /*max_rows=*/60);
  return !disk.exists || disk.lines != to_lines(regenerated);
}

} // namespace

int main(int argc, char** argv) {
  bool diff = false;
  std::string dir;
  int64_t seed = 42;
  bool have_seed = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--diff") == 0) {
      diff = true;
    } else if (dir.empty()) {
      dir = argv[i];
    } else if (!have_seed) {
      seed = std::strtoll(argv[i], nullptr, 10);
      have_seed = true;
    } else {
      std::fprintf(stderr, "usage: %s <fixtures_dir> [seed] [--diff]\n", argv[0]);
      return 2;
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
    results.push_back(run_scene(seed, run, scene->blocks));
    for (const std::string& violation : results.back().violations)
      violations.push_back(violation);
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

  const std::string meta_path = dir + "/block_tick_meta.tsv";
  const std::string scene_path = dir + "/block_tick_scene.tsv";
  const std::string actions_path = dir + "/block_tick_actions.tsv";
  const std::string meta = meta_text(seed, runs, results);
  const std::string scene = scene_text(scenes);
  const std::string actions = actions_text(seed, runs);

  if (diff) {
    std::printf("block-tick fixture: %zu runs, seed %lld\n", runs.size(),
                static_cast<long long>(seed));
    bool changed = false;
    changed |= diff_text("block_tick_meta.tsv", meta_path, meta);
    changed |= diff_text("block_tick_scene.tsv", scene_path, scene);
    changed |= diff_text("block_tick_actions.tsv", actions_path, actions);
    for (size_t i = 0; i < runs.size(); ++i) {
      const std::string name =
          std::string("block_tick_seed") + std::to_string(seed) + "_" + runs[i].name + ".tsv";
      changed |= diff_text(name, trace_path(dir, seed, runs[i].name),
                           trace_text(seed, runs[i], results[i]));
    }
    if (!changed) {
      std::printf("summary: in sync with this run's output\n");
      return 0;
    }
    std::printf("summary: the checked-in fixture is OUT OF SYNC\n");
    std::printf("re-run without --diff to rewrite it, then `ctest -R block_tick`\n");
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
    std::printf("wrote block_tick_seed%lld_%s.tsv (%zu rows: %zu due, %zu schedules, %zu drops, "
                "%zu writes, pending %zu)\n",
                static_cast<long long>(seed), runs[i].name, results[i].rows.size(), results[i].due,
                results[i].schedules, results[i].drops, results[i].writes, results[i].end_pending);
  }
  return 0;
}
