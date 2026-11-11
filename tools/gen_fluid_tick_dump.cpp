// SPDX-License-Identifier: GPL-3.0-or-later
//
// Writes the scheduled fluid-tick loop's golden output for a small set of
// recorded scenes: the run parameters (`fluid_tick_meta.tsv`), the world each run
// starts from (`fluid_tick_scene.tsv`), the caller's own scripted work
// (`fluid_tick_actions.tsv`) and one interaction trace per run
// (`fluid_tick_<run>.tsv`).
//
// This is **not** a vanilla dump, and cannot be: `tests/fluid_test.cpp` pins the
// state encoding, `fluid_shape`, the drop-off ladder and each branch of
// `fluid_tick` one call at a time, and its `FluidTickQueue` section pins the
// queue's dedupe rule — but nothing runs the loop: many ticks, the queue's
// bookkeeping feeding back into what the next tick drains, a stream of writes
// reshaping the world the later ticks read. That is what this fixture holds, the
// same way `tests/fixtures/{worldgen,random_tick,weather,mob_spawn,block_tick}`
// hold their subsystems' sequences.
//
// **There is no seed, and that is a property of the subsystem rather than an
// omission.** `FireBlock.tick` draws `30 + nextInt(10)` from the level's own
// `RandomSource`, so the block-tick fixture can only pin the loop's shape and its
// draw *order* — the values belong to the server. `fluid_tick` takes no draw at
// all: its whole stream is a function of the world and two constants,
// `fluid_drop_off` (1 for water, 2 for lava) and `fluid_tick_delay` (5 and 30).
// Every schedule in this fixture therefore comes from a constant, and the trace
// below is the *entire* stream, not a sampled part of it.
//
// What a run is: `ticks` world ticks. In each one the real `FluidTickQueue` is
// advanced and every entry it hands back is dispatched the way
// `run_fluid_tick_position` (src/server/session.cpp) dispatches it — the
// schedule callback rebound to the entry's own kind, then `fluid_tick` — and then
// the caller's scripted work for that tick happens: a placement or a break, the
// two session paths that touch this queue. Everything the rule and the caller do
// to the queue, to the world and to the recorded stream is in the trace, in
// order:
//
//   action   <tick> <place|break> <x> <y> <z> <state>   (the caller's scripted work)
//   due      <tick> <water|lava> <x> <y> <z>            (one entry the drain ran)
//   state    <tick> <now> <pending>                     (the queue after the drain)
//   schedule <tick> <x> <y> <z> <water|lava> <delay> <due>   (`Level.scheduleTick`)
//   read     <tick> <x> <y> <z> <state>                 (`get_block`)
//   write    <tick> <x> <y> <z> <state>                 (`set_block`)
//   stale    <tick> <water|lava> <x> <y> <z> <state>    (the tick found no such fluid there)
//
// **The queue's return order is deliberately not pinned.** `advance()` walks a
// `std::map` of per-chunk buckets, so its order is portable — chunk key (x, then
// z), then insertion — but it is not the *canonical* order, and the two session
// paths disagree about what to do with it: the serial arm dispatches the raw
// order and the region arm sorts into (region, then x, y, z). A fixture that
// pinned either raw order would encode one arm's choice; so each tick's `due`
// rows are written in the canonical order the region path dispatches, and what
// the fixture holds is *which* entries (position **and** fluid kind) are due in
// which tick, plus everything each of them did. The kind breaks a position tie
// here because the queue's own key allows two entries at one cell — a lava entry
// and a water entry coexist (the run `mixed` pins exactly that) — and the
// session's sort key stops at the position, which `std::sort` does not resolve
// stably.
//
// A schedule's due tick is `now + max(delay, 1)` (`FluidTickQueue::schedule`'s
// own arithmetic), where `now` is the tick the schedule was taken in — and a
// second schedule for the same (position, kind) keeps the *earlier* of the two,
// which is vanilla's "the earliest pending tick wins" and is why the run
// `reschedule` exists: the queue must not let a later schedule push a pending
// entry back. Every `due` row is checked against that arithmetic rather than
// merely satisfying it, so a drain that lands a tick late cannot be pinned as if
// it were intended.
//
// Two things are deliberately not modelled, and the session's own adapters do
// both: the block-queue half of a caller's write (`notify_block_neighbours` also
// tells the leaves around a change to re-derive their distance, which schedules
// *block* ticks — `tests/fixtures/block_tick` owns that stream, and no scene here
// has a leaf), and the client broadcast / redstone fan-out a write raises
// (neither touches a cell a fluid reads, and neither schedules a fluid tick). The
// fluid half of that fan-out *is* modelled, because it schedules into the queue
// this fixture pins: a block placed or broken beside settled fluid is what makes
// it run into the opening (`tests/fluid_resume_test.py` / `fluids-retract`
// exercise the same path end to end over the wire).
//
// Usage:
//   plusmc-fluid-tick-dump <fixtures_dir> [--diff]
//
// `--diff` writes nothing: it re-runs every scene in memory, reads what is
// checked in, and prints a patch of each file that would change (invariant
// violations included), exiting 1 while the fixture differs. The review is
//
//   ./build/plusmc-fluid-tick-dump <fixtures_dir> --diff   # read the patch
//   ./build/plusmc-fluid-tick-dump <fixtures_dir>          # then rewrite it
//   ctest -R fluid_tick                                    # and re-pin it
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
#include "world/chunk.h"     // block_blocks_motion
#include "world/container.h" // block_property, with_property
#include "world/fluid.h"

namespace {

using mc::world::BlockStateKey;
using mc::world::FluidKind;
using mc::world::FluidLevel;
using mc::world::FluidState;
using mc::world::FluidTickPos;

// The floor and the air level every scene is built on.
constexpr int kFloorY = -61;
constexpr int kAirY = -60;

const BlockStateKey& air() {
  static const BlockStateKey state{"minecraft:air", ""};
  return state;
}

std::string kind_name(FluidKind kind) {
  switch (kind) {
  case FluidKind::Water:
    return "water";
  case FluidKind::Lava:
    return "lava";
  default:
    return "empty";
  }
}

FluidKind kind_from_name(std::string_view name) {
  // `minecraft:bubble_column` carries a water fluid state, the same reading
  // `fluid_state_of` makes.
  if (name == "minecraft:bubble_column")
    return FluidKind::Water;
  if (name == "minecraft:water" || name == "minecraft:lava") {
    const FluidState state = mc::world::fluid_state_of(BlockStateKey{std::string(name), ""});
    return state.kind;
  }
  return FluidKind::Empty;
}

bool is_fluid_name(std::string_view name) {
  return name == "minecraft:water" || name == "minecraft:lava";
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

// The session's own `FluidLevel::can_hold_fluid` (src/server/session.cpp's
// `fluid_level_for`): the fluid blocks themselves, then a block that does not
// block motion and does not already hold a fluid. Spelled here rather than
// approximated because it is half of `can_replace`, i.e. half of the rule that
// decides every write in the trace.
bool can_hold_fluid(const BlockStateKey& state) {
  if (state.name == "minecraft:air" || state.name == "minecraft:water" ||
      state.name == "minecraft:lava" || state.name == "minecraft:bubble_column")
    return true;
  return mc::world::block_property(state.properties, "waterlogged") == "false" &&
         !mc::world::block_blocks_motion(state.name);
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
// this queue: a placement (`/setblock`, a bucket) and a break.
struct Action {
  int tick; // 0 means "in the window before the first tick"
  bool place;
  int x;
  int y;
  int z;
  std::string name;
  std::string properties;
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

// Every scene carries a floor wide enough to catch a pool: water runs seven
// cells from its source before the amount ladder reaches zero, so a floor that
// stops short of that lets the outermost cells fall off the edge and the run
// never drains. `kReach` is the margin every basin floor keeps around any
// placement.
constexpr int kReach = 10;

std::vector<Scene> build_scenes() {
  std::vector<Scene> scenes;

  // The ordinary case: a source on flat ground. It spreads its seven rings,
  // settles, and the queue drains.
  scenes.push_back(
      Scene{"flat", floor(kFloorY, -kReach, kReach, -kReach, kReach, "minecraft:stone")});

  // The same floor with a one-cell-wide shaft above it: the source is placed at
  // the top and falls three cells before it lands. The walls around the shaft
  // exist so the falling cell's lateral spread (a falling fluid spreads sideways
  // with the full 7) is refused at every level — otherwise the fall fans out
  // into open air at each height and never settles. This is the run that pins the
  // falling ladder: a falling cell writes the cell below as falling and takes
  // five ticks per level.
  Scene shaft{"shaft", floor(kFloorY, -kReach, kReach, -kReach, kReach, "minecraft:stone")};
  for (int y = -59; y <= -57; ++y) {
    add(&shaft.blocks, 1, y, 0, "minecraft:stone");
    add(&shaft.blocks, -1, y, 0, "minecraft:stone");
    add(&shaft.blocks, 0, y, 1, "minecraft:stone");
    add(&shaft.blocks, 0, y, -1, "minecraft:stone");
  }
  scenes.push_back(shaft);

  // A wide floor so two sources 20 blocks apart in x land in different chunks
  // (0 and 1): the queue is bucketed per chunk, and this run shows both buckets
  // delivered in one tick. The floor reaches past the eastern pool's own seven
  // cells (x = 27) — `kReach` measured from the far source, not from the origin.
  scenes.push_back(
      Scene{"wide", floor(kFloorY, -kReach, 20 + kReach, -kReach, kReach, "minecraft:stone")});

  // A one-wide channel along +z with its back wall closed, so a stream can only
  // run one way. Cutting the source is the run's work: the cells downstream have
  // to retract one at a time, which is `retract_to_air` and its fan-out.
  Scene canal{"canal", floor(kFloorY, -3, 3, -3, 16, "minecraft:stone")};
  for (int z = -1; z <= 16; ++z) {
    add(&canal.blocks, -1, kAirY, z, "minecraft:stone");
    add(&canal.blocks, 1, kAirY, z, "minecraft:stone");
  }
  add(&canal.blocks, 0, kAirY, -1, "minecraft:stone");
  scenes.push_back(canal);

  // A rail beside a source: rail is the waterlogging case that can actually be
  // reached by a *spread* — it does not block motion and carries
  // `waterlogged=false`, so `placed_state` keeps its name and flips the property
  // instead of writing a water block (`FluidState.createLegacyBlock`'s
  // waterlogged arm). A slab is the other shape of the same rule but does block
  // motion, so the spread refuses it outright.
  Scene rail{"rail", floor(kFloorY, -kReach, kReach, -kReach, kReach, "minecraft:stone")};
  add(&rail.blocks, 1, kAirY, 0, "minecraft:rail", "shape=north_south,waterlogged=false");
  scenes.push_back(rail);

  // A source against a wall. The stream settles against it, and the run's work is
  // to break the wall: the break's neighbour fan-out is what schedules the fluid
  // that can now flow into the opening.
  Scene wall{"wall", floor(kFloorY, -kReach, kReach, -kReach, kReach, "minecraft:stone")};
  for (int z = -kReach; z <= kReach; ++z)
    add(&wall.blocks, 1, kAirY, z, "minecraft:stone");
  scenes.push_back(wall);

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

// A placement, the session's own put: the write, then `Level.scheduleTick` for a
// fluid source, then the fluid half of the neighbour fan-out.
Action place(int tick, int x, int y, int z, const char* name, const char* properties = "") {
  return Action{tick, true, x, y, z, name, properties};
}

Action break_block(int tick, int x, int y, int z) {
  return Action{tick, false, x, y, z, "minecraft:air", ""};
}

// `spread` and `fall` are the ordinary cases (a pool that settles, and a fall
// onto the same floor); `fall_cut` is that fall with its source cut mid-descent,
// which is the one shape where a falling cell pours into a still-dry cell below
// it and then retracts in the same tick — the retract's fan-out then reads the
// cell this very tick wrote, the case a per-tick read cache can get wrong;
// `two_chunks` is two pools in two chunks due in one tick;
// `lava` is the other ladder (drop-off 2, delay 30); `cut` is the retraction
// cascade after a source is removed; `waterlog` is a spread into a rail, which
// flips `waterlogged` instead of writing water; `resume` is a block removed
// beside settled fluid; `stale` is an entry whose fluid is gone when it comes
// due; `reschedule` is the same cell scheduled twice, which the queue keeps the
// earliest of; and `mixed` is water and lava at one cell, the only way this
// queue holds two entries for one position.
// A run's `ticks` is a few past the tick it settles on rather than a round
// number: the tail is what proves nothing *keeps* ticking (a settled pool stops
// re-queueing itself), and a hundred ticks of it would be a hundred rows saying
// the same thing. The tick each run last did anything is the number below.
std::vector<RecordedRun> build_runs() {
  std::vector<RecordedRun> runs;
  runs.push_back(
      RecordedRun{"spread", "flat", 45, 1, true, {place(0, 0, kAirY, 0, "minecraft:water")}});
  runs.push_back(
      RecordedRun{"fall", "shaft", 70, 1, true, {place(0, 0, -57, 0, "minecraft:water")}});
  // The same shaft with the source cut *mid-fall*: the column is still
  // descending when its feed goes. The falling cell whose cell below is still dry
  // then pours down and retracts in the same tick, so the retract's neighbour
  // fan-out reads a cell this very tick wrote. That is the one shape a per-tick
  // read cache can get wrong, and nothing else here reaches it: the tick the
  // cut lands on is the run's whole point. The source's entry drains at tick 5
  // and the cell below it at tick 10, so breaking the source after tick 9
  // leaves that cell heading into a dry shaft.
  runs.push_back(RecordedRun{"fall_cut",
                             "shaft",
                             30,
                             1,
                             true,
                             {place(0, 0, -57, 0, "minecraft:water"), break_block(9, 0, -57, 0)}});
  runs.push_back(RecordedRun{
      "two_chunks",
      "wide",
      45,
      2,
      true,
      {place(0, 0, kAirY, 2, "minecraft:water"), place(0, 20, kAirY, 2, "minecraft:water")}});
  runs.push_back(
      RecordedRun{"lava", "flat", 155, 1, true, {place(0, 0, kAirY, 0, "minecraft:lava")}});
  runs.push_back(
      RecordedRun{"cut",
                  "canal",
                  160,
                  1,
                  true,
                  {place(0, 0, kAirY, 0, "minecraft:water"), break_block(120, 0, kAirY, 0)}});
  runs.push_back(
      RecordedRun{"waterlog", "rail", 55, 1, true, {place(0, 0, kAirY, 0, "minecraft:water")}});
  runs.push_back(
      RecordedRun{"resume",
                  "wall",
                  125,
                  1,
                  true,
                  {place(0, 0, kAirY, 0, "minecraft:water"), break_block(80, 1, kAirY, 0)}});
  // The doused source: placed, then removed two ticks later, so the entry it
  // scheduled is still pending when it comes due and the tick finds air.
  runs.push_back(
      RecordedRun{"stale",
                  "flat",
                  8,
                  1,
                  true,
                  {place(0, 0, kAirY, 0, "minecraft:water"), break_block(2, 0, kAirY, 0)}});
  // The same cell placed twice: the second schedule must not push the pending
  // entry back — the earliest tick wins.
  runs.push_back(RecordedRun{
      "reschedule",
      "flat",
      45,
      1,
      true,
      {place(0, 0, kAirY, 0, "minecraft:water"), place(2, 0, kAirY, 0, "minecraft:water")}});
  // Water then lava at the same cell: two entries for one position, which is what
  // the queue's (cell, kind) key allows. The water entry is stale by the time it
  // comes due (the cell is lava now) and the lava one ticks.
  runs.push_back(RecordedRun{
      "mixed",
      "flat",
      157,
      2,
      true,
      {place(0, 0, kAirY, 0, "minecraft:water"), place(2, 0, kAirY, 0, "minecraft:lava")}});
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
  size_t writes = 0;
  size_t stales = 0;
  int64_t end_now = 0;
  size_t end_pending = 0;
  std::vector<std::string> violations;
};

RunResult run_scene(const RecordedRun& run, const std::vector<Placement>& start) {
  RunResult result;
  // The tick every row below is stamped with; the loop at the end moves it.
  int tick = 0;
  auto push = [&result, &tick](const char* kind, const std::string& body) {
    result.rows.push_back(std::string(kind) + "\t" + std::to_string(tick) + "\t" + body);
  };
  auto violation = [&result](const std::string& text) { result.violations.push_back(text); };

  // The world: the scene's cells, plus everything the caller and the rule write.
  // A write appends, so the *last* matching entry is what a read finds — which is
  // also what makes the recorded scene's own state the first word on a cell.
  std::vector<Placement> world;
  world.reserve(start.size() + 128);
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

  mc::world::FluidTickQueue queue;
  // The schedule the level's callbacks use: the session rebinds the kind per due
  // entry (`run_fluid_tick_position`), so the level has to read the tick's own
  // kind from somewhere — this is that somewhere, and `speed_tick` below is where
  // it is set before each dispatch.
  FluidKind tick_kind = FluidKind::Empty;

  // What the last schedule for (position, kind) said: the tick it was taken in
  // and the earliest tick it can come due. The queue's rule is "the earliest
  // pending tick wins", so a repeat schedule is a `min`, and the drain is checked
  // against that floor — a later due can never be what the fixture pins.
  struct Scheduled {
    FluidKind kind;
    FluidTickPos pos;
    int at;
    int64_t due;
  };
  std::vector<Scheduled> last_schedule;
  auto remember_schedule = [&last_schedule](FluidKind kind, const FluidTickPos& pos, int at,
                                            int64_t due) {
    for (Scheduled& entry : last_schedule)
      if (entry.kind == kind && entry.pos == pos) {
        entry.at = at;
        entry.due = std::min(entry.due, due);
        return;
      }
    last_schedule.push_back(Scheduled{kind, pos, at, due});
  };
  auto last_schedule_for = [&last_schedule](FluidKind kind, const FluidTickPos& pos) {
    for (const Scheduled& entry : last_schedule)
      if (entry.kind == kind && entry.pos == pos)
        return &entry;
    return static_cast<const Scheduled*>(nullptr);
  };

  // `Level.scheduleTick`, with the kind spelled: the caller's own paths pass it
  // (`/setblock`, a bucket), and the level's callbacks read the tick's kind.
  auto schedule_at = [&queue, &result, &push, &remember_schedule,
                      &tick](int x, int y, int z, FluidKind kind, int delay) {
    if (kind == FluidKind::Empty)
      return;
    // `FluidTickQueue::schedule`'s own arithmetic: due `max(delay, 1)` ticks from
    // now, where `now` is the tick the schedule is taken in — so a delay D drains
    // exactly D ticks later. The drain re-derives that below rather than trusting
    // it.
    const int64_t due = queue.now() + (delay > 0 ? delay : 1);
    queue.schedule(x, y, z, kind, delay);
    ++result.schedules;
    char text[192];
    std::snprintf(text, sizeof(text), "%s\t%s\t%d\t%lld", coord(x, y, z).c_str(),
                  kind_name(kind).c_str(), delay, static_cast<long long>(due));
    push("schedule", text);
    remember_schedule(kind, FluidTickPos{x, y, z}, tick, due);
  };

  FluidLevel level;
  level.get_block = [&raw_get, &result, &push](int x, int y, int z) {
    const BlockStateKey state = raw_get(x, y, z);
    ++result.reads;
    push("read", coord(x, y, z) + "\t" + canonical_state(state));
    return state;
  };
  level.can_hold_fluid = [](const BlockStateKey& state) { return can_hold_fluid(state); };
  level.schedule_tick = [&schedule_at, &tick_kind](int x, int y, int z, int delay) {
    schedule_at(x, y, z, tick_kind, delay);
  };
  // The session's own write: nothing happens when the state is already there
  // (`fluid_level_for`'s equality guard), which is why a settled pool stops
  // producing rows. The guard's own read and the client broadcast are the
  // session's, not a rule's, so neither is in the trace.
  level.set_block = [&raw_get, &raw_set, &result, &push](int x, int y, int z,
                                                         const BlockStateKey& state) {
    if (raw_get(x, y, z) == state)
      return;
    raw_set(x, y, z, state);
    ++result.writes;
    push("write", coord(x, y, z) + "\t" + canonical_state(state));
  };

  // The fluid half of the session's `notify_block_neighbours`: the six cells
  // around a write that now holds fluid get a tick with their own kind and delay.
  // Its other half — the leaf distance update — schedules *block* ticks and is
  // the block-tick fixture's; its own reads are the session's, so they are not
  // rows.
  auto neighbour_fluid_ticks = [&raw_get, &schedule_at](int x, int y, int z) {
    constexpr int dx[6] = {1, -1, 0, 0, 0, 0};
    constexpr int dy[6] = {0, 0, 1, -1, 0, 0};
    constexpr int dz[6] = {0, 0, 0, 0, 1, -1};
    for (int i = 0; i < 6; ++i) {
      const FluidState fluid = mc::world::fluid_state_of(raw_get(x + dx[i], y + dy[i], z + dz[i]));
      if (!fluid.empty())
        schedule_at(x + dx[i], y + dy[i], z + dz[i], fluid.kind,
                    mc::world::fluid_tick_delay(fluid.kind));
    }
  };

  // The two caller paths, exactly as the session spells them: a placement writes
  // the block through the world (no equality guard — that write *is* the client's
  // block change), schedules the new source when it is one, and fans out; a break
  // reads the block it is removing, writes air and fans out.
  auto run_action = [&](const Action& action) {
    push("action", std::string(action.place ? "place" : "break") + "\t" +
                       coord(action.x, action.y, action.z) + "\t" +
                       canonical_state(BlockStateKey{action.name, action.properties}));
    ++result.actions;
    if (action.place) {
      const BlockStateKey state{action.name, action.properties};
      raw_set(action.x, action.y, action.z, state);
      ++result.writes;
      push("write", coord(action.x, action.y, action.z) + "\t" + canonical_state(state));
      if (is_fluid_name(state.name))
        schedule_at(action.x, action.y, action.z, kind_from_name(state.name),
                    mc::world::fluid_tick_delay(kind_from_name(state.name)));
    } else {
      level.get_block(action.x, action.y, action.z); // the break's own read
      raw_set(action.x, action.y, action.z, air());
      ++result.writes;
      push("write", coord(action.x, action.y, action.z) + "\t" + canonical_state(air()));
    }
    neighbour_fluid_ticks(action.x, action.y, action.z);
  };

  // The window before the first tick belongs to whatever was placed before the
  // world started ticking.
  for (const Action& action : run.actions)
    if (action.tick == 0)
      run_action(action);

  for (tick = 1; tick <= run.ticks; ++tick) {
    std::vector<std::pair<FluidKind, FluidTickPos>> drained = queue.advance();
    std::vector<std::pair<FluidKind, FluidTickPos>> ordered = drained;
    std::sort(ordered.begin(), ordered.end(),
              [](const std::pair<FluidKind, FluidTickPos>& a,
                 const std::pair<FluidKind, FluidTickPos>& b) {
                if (a.second.x != b.second.x)
                  return a.second.x < b.second.x;
                if (a.second.y != b.second.y)
                  return a.second.y < b.second.y;
                if (a.second.z != b.second.z)
                  return a.second.z < b.second.z;
                return a.first < b.first; // two kinds at one cell: the tie-break
              });
    for (size_t i = 0; i < ordered.size(); ++i) {
      push("due", kind_name(ordered[i].first) + "\t" +
                      coord(ordered[i].second.x, ordered[i].second.y, ordered[i].second.z));
      ++result.due;
      char message[256];
      if (i > 0 && ordered[i - 1] == ordered[i]) {
        std::snprintf(message, sizeof(message), "run '%s': tick %d drained %s %s twice", run.name,
                      tick, kind_name(ordered[i].first).c_str(),
                      coord(ordered[i].second.x, ordered[i].second.y, ordered[i].second.z).c_str());
        violation(message);
      }
      const Scheduled* scheduled = last_schedule_for(ordered[i].first, ordered[i].second);
      if (scheduled == nullptr) {
        std::snprintf(message, sizeof(message),
                      "run '%s': tick %d drained %s %s with no schedule behind it", run.name, tick,
                      kind_name(ordered[i].first).c_str(),
                      coord(ordered[i].second.x, ordered[i].second.y, ordered[i].second.z).c_str());
        violation(message);
      } else if (scheduled->due != tick) {
        std::snprintf(message, sizeof(message),
                      "run '%s': tick %d drained %s %s, whose schedules (last at tick %d) made it"
                      " due at %lld",
                      run.name, tick, kind_name(ordered[i].first).c_str(),
                      coord(ordered[i].second.x, ordered[i].second.y, ordered[i].second.z).c_str(),
                      scheduled->at, static_cast<long long>(scheduled->due));
        violation(message);
      }
    }
    // The drain frees every entry it returned, so the earliest-pending floor for
    // those (position, kind) pairs is gone: the next schedule for one of them
    // starts a fresh entry from the current clock, and comparing it against the
    // tick that already drained would fail on a loop that is behaving.
    for (const auto& entry : ordered) {
      for (auto it = last_schedule.begin(); it != last_schedule.end(); ++it) {
        if (it->kind == entry.first && it->pos == entry.second) {
          last_schedule.erase(it);
          break;
        }
      }
    }
    // The dispatch, in `run_fluid_tick_position`'s shape: the schedule callback
    // rebound to the entry's kind, then the rule. The check in front of it is
    // `fluid_tick`'s own guard (`here.empty() || here.kind != kind`) spelled by
    // the caller, so the trace names the entry that found nothing — the read the
    // rule makes is the row below when the guard passes, which is the same read
    // the real dispatch makes.
    for (const auto& entry : ordered) {
      const FluidKind kind = entry.first;
      const FluidTickPos& pos = entry.second;
      const BlockStateKey here = raw_get(pos.x, pos.y, pos.z); // the dispatch's own check
      const FluidState fluid = mc::world::fluid_state_of(here);
      if (fluid.empty() || fluid.kind != kind) {
        ++result.stales;
        push("stale",
             kind_name(kind) + "\t" + coord(pos.x, pos.y, pos.z) + "\t" + canonical_state(here));
        continue;
      }
      tick_kind = kind;
      mc::world::fluid_tick(level, kind, pos.x, pos.y, pos.z);
    }
    char text[96];
    std::snprintf(text, sizeof(text), "%lld\t%zu", static_cast<long long>(queue.now()),
                  queue.size());
    push("state", text);
    ++result.states;
    // ...and the caller's work in the window after this tick and before the next.
    for (const Action& action : run.actions)
      if (action.tick == tick)
        run_action(action);
  }

  result.end_now = queue.now();
  result.end_pending = queue.size();

  // The invariants that make the trace a pin of the loop rather than a transcript
  // of one: the counters have to account for every row, the run has to reach the
  // coverage it exists for, and the queue has to end where the run says it does.
  const size_t counted = result.actions + result.due + result.states + result.schedules +
                         result.reads + result.writes + result.stales;
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
  if (run.drains_to_empty && result.end_pending != 0) {
    char message[256];
    std::snprintf(message, sizeof(message), "run '%s': the queue still holds %zu entries", run.name,
                  result.end_pending);
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

std::string meta_text(const std::vector<RecordedRun>& runs, const std::vector<RunResult>& results) {
  std::string text;
  text += "# Phase 4 milestone D follow-up: the scheduled fluid-tick loop's golden run\n"
          "# parameters.\n"
          "#\n"
          "# NOT a vanilla dump: `tests/fluid_test.cpp` pins the state encoding and each\n"
          "# branch of `fluid_tick` one call at a time, but nothing there runs the loop.\n"
          "# `ctest -R fluid_tick` replays every row below through the real\n"
          "# `FluidTickQueue` and the real dispatch (`run_fluid_tick_position`'s shape: the\n"
          "# schedule callback rebound to the entry's own kind, then `fluid_tick`), and\n"
          "# compares the whole trace line for line, so a write that moved, a schedule one\n"
          "# tick early or an entry that silently ran cannot land.\n"
          "#\n"
          "# There is no seed column. `fluid_tick` takes no draw at all — its stream is a\n"
          "# function of the world and the two constants `fluid_drop_off` (1 water, 2\n"
          "# lava) and `fluid_tick_delay` (5 and 30) — so unlike the fire dump there is no\n"
          "# level stream whose state belongs to the server, and this trace is the whole\n"
          "# stream rather than a sampled part of it.\n"
          "#\n"
          "# `ticks` is how many world ticks the run advanced the queue through (one\n"
          "# `advance()` each), `schedules`/`due`/`stales`/`writes`/`reads` are the counts\n"
          "# of those trace rows — `reads` is one row per world read, and a dispatch\n"
          "# makes exactly one per cell it asks about — and `endNow`/`endPending` the\n"
          "# queue's own state after the last tick — a run that settles is zero (the rule\n"
          "# only re-queues a cell that actually moved, so a settled pool stops ticking\n"
          "# itself), and every run here does. `rows` is the trace's length.\n"
          "#\n"
          "# run\tscene\tticks\tschedules\tdue\tstales\twrites\treads\tendNow\tendPending\trows\n";
  for (size_t i = 0; i < runs.size(); ++i) {
    const RunResult& result = results[i];
    char line[512];
    std::snprintf(line, sizeof(line), "%s\t%s\t%d\t%zu\t%zu\t%zu\t%zu\t%zu\t%lld\t%zu\t%zu\n",
                  runs[i].name, runs[i].scene, runs[i].ticks, result.schedules, result.due,
                  result.stales, result.writes, result.reads,
                  static_cast<long long>(result.end_now), result.end_pending, result.rows.size());
    text += line;
  }
  return text;
}

std::string scene_text(const std::vector<Scene>& scenes) {
  std::string text =
      "# The world each recorded run starts from. Everything not listed is air; the\n"
      "# caller's own work (a placement, a break) is in fluid_tick_actions.tsv, since a\n"
      "# scene can serve more than one run. Every basin floor reaches at least ten cells\n"
      "# past any placement: water runs seven cells before its amount ladder reaches\n"
      "# zero, and a floor that stops short lets the outermost cells fall off the edge.\n"
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

std::string actions_text(const std::vector<RecordedRun>& runs) {
  std::string text =
      "# The caller's own work: what a session does between ticks, and the only other\n"
      "# thing that touches this queue. `place` is the placement path (`/setblock`, a\n"
      "# bucket): the write, then `Level.scheduleTick` when the block is a fluid source,\n"
      "# then the fluid half of the neighbour fan-out. `break` is a removal: the read of\n"
      "# the block going away, the write to air, then the same fan-out. A tick of 0 is\n"
      "# the window before the first tick.\n"
      "#\n"
      "# run\ttick\tkind\tx\ty\tz\tstate\n";
  for (const RecordedRun& run : runs)
    for (const Action& action : run.actions) {
      char row[224];
      std::snprintf(row, sizeof(row), "%s\t%d\t%s\t%d\t%d\t%d\t%s\n", run.name, action.tick,
                    action.place ? "place" : "break", action.x, action.y, action.z,
                    canonical_state(BlockStateKey{action.name, action.properties}).c_str());
      text += row;
    }
  return text;
}

std::string trace_text(const RecordedRun& run, const RunResult& result) {
  std::string text;
  text += "# Phase 4 milestone D follow-up: the scheduled fluid-tick loop's interaction trace\n";
  text += "# for run '";
  text += run.name;
  text += "' (scene '";
  text += run.scene;
  text += "').\n";
  text += "#\n"
          "# NOT a vanilla dump (see fluid_tick_meta.tsv): every row is one thing the loop,\n"
          "# the rule or the caller did, in order, with the tick it happened in. `action` is\n"
          "# the caller's scripted work (see fluid_tick_actions.tsv); `due` is the drain —\n"
          "# one row per entry, in the canonical order the region path dispatches them, with\n"
          "# the fluid kind the queue stored (the queue's own order is its chunk map's and is\n"
          "# deliberately not pinned; see the tool's header); `state` closes each tick with\n"
          "# the queue's own clock and pending count; `schedule` is `Level.scheduleTick` with\n"
          "# the kind, the delay the rule or the caller asked for and the due tick that\n"
          "# follows from it; `read` is a world read the rule makes — the tick asks about\n"
          "# its own cell, what is below, what is above and its four sides, and asks each\n"
          "# of them once per dispatch, because every later question about that cell is\n"
          "# answered from the same read; two `read` rows for one cell in a tick are two\n"
          "# entries from the drain asking, not one tick asking twice, and these rows are\n"
          "# most of what the trace holds — and `write` a block write (the session's adapter\n"
          "# writes nothing when the state is already there, so a settled cell produces no\n"
          "# write row);\n"
          "# `stale` is an entry that came due and found neither that fluid nor any fluid in\n"
          "# the cell.\n"
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

std::string trace_path(const std::string& dir, const char* run) {
  return dir + "/fluid_tick_" + run + ".tsv";
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
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--diff") == 0) {
      diff = true;
    } else if (dir.empty()) {
      dir = argv[i];
    } else {
      std::fprintf(stderr, "usage: %s <fixtures_dir> [--diff]\n", argv[0]);
      return 2;
    }
  }
  if (dir.empty()) {
    std::fprintf(stderr, "usage: %s <fixtures_dir> [--diff]\n", argv[0]);
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
    results.push_back(run_scene(run, scene->blocks));
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

  const std::string meta_path = dir + "/fluid_tick_meta.tsv";
  const std::string scene_path = dir + "/fluid_tick_scene.tsv";
  const std::string actions_path = dir + "/fluid_tick_actions.tsv";
  const std::string meta = meta_text(runs, results);
  const std::string scene = scene_text(scenes);
  const std::string actions = actions_text(runs);

  if (diff) {
    std::printf("fluid-tick fixture: %zu runs\n", runs.size());
    bool changed = false;
    changed |= diff_text("fluid_tick_meta.tsv", meta_path, meta);
    changed |= diff_text("fluid_tick_scene.tsv", scene_path, scene);
    changed |= diff_text("fluid_tick_actions.tsv", actions_path, actions);
    for (size_t i = 0; i < runs.size(); ++i)
      changed |= diff_text(std::string("fluid_tick_") + runs[i].name + ".tsv",
                           trace_path(dir, runs[i].name), trace_text(runs[i], results[i]));
    if (!changed) {
      std::printf("summary: in sync with this run's output\n");
      return 0;
    }
    std::printf("summary: the checked-in fixture is OUT OF SYNC\n");
    std::printf("re-run without --diff to rewrite it, then `ctest -R fluid_tick`\n");
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
    const std::string path = trace_path(dir, runs[i].name);
    std::FILE* trace = std::fopen(path.c_str(), "w");
    if (trace == nullptr) {
      std::fprintf(stderr, "error: cannot write %s\n", path.c_str());
      return 1;
    }
    std::fputs(trace_text(runs[i], results[i]).c_str(), trace);
    std::fclose(trace);
    std::printf("wrote fluid_tick_%s.tsv (%zu rows: %zu due, %zu schedules, %zu stales, %zu writes,"
                " pending %zu)\n",
                runs[i].name, results[i].rows.size(), results[i].due, results[i].schedules,
                results[i].stales, results[i].writes, results[i].end_pending);
  }
  return 0;
}
