// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scheduled block-tick loop's golden half (Phase 3 milestone G follow-up):
// replay `tests/fixtures/block_tick/` through the real queue and the real
// dispatch, and compare every run's whole interaction trace row for row.
//
// The loop's *property* half lives next to the blocks it dispatches rather than
// here, and deliberately: `tests/fire_test.cpp`'s `test_block_tick_queue` pins
// `BlockTickQueue`'s own clock, its one-entry-per-position rule and its chunk
// bucketing, and its scripted-draw half pins each branch of `FireBlock.tick`; the
// leaf's schedule-a-tick and distance rules are pinned by
// `tests/random_tick_test.cpp`, and the fire -> leaf neighbour notify by
// `fire_test.cpp`'s own `set_block` assertions. What none of them does is *run
// the loop* — many ticks, a real stream, the queue's bookkeeping feeding back
// into what the next tick drains — which is what this test holds, exactly as
// `tests/fixtures/{worldgen,random_tick,weather,mob_spawn}` hold their
// subsystems' sequences. `tools/gen_block_tick_dump.cpp` owns the vocabulary and
// the invariants; this file is the replay.
//
// What it adds over the row comparison, on the fixture's own shape:
//   * each tick's `due` rows are in the canonical order the region path
//     dispatches (x, then y, then z) and carry no position twice — the queue's
//     own return order is the hash map's and is deliberately not pinned;
//   * every `action` row the run has is in `block_tick_actions.tsv` and vice
//     versa, so a run cannot be replayed with work missing from the fixture;
//   * every tick the meta names is closed by exactly one `state` row, so the
//     queue's clock and pending count are pinned per tick and not only at the end.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "world/block_tick.h"
#include "world/chunk.h"
#include "world/fire.h"
#include "world/leaves.h"
#include "world/random_tick.h"
#include "world/survival.h"
#include "world/worldgen_random.h"

using namespace mc::world;

static int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

namespace {

constexpr int kDifficulty = kDifficultyNormal;

const BlockStateKey& air_state() {
  static const BlockStateKey state{"minecraft:air", ""};
  return state;
}

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

BlockStateKey parse_state(const std::string& text) {
  const size_t open = text.find('[');
  if (open == std::string::npos)
    return BlockStateKey{text, ""};
  return BlockStateKey{text.substr(0, open), text.substr(open + 1, text.size() - open - 2)};
}

std::string coord(int x, int y, int z) {
  char text[64];
  std::snprintf(text, sizeof(text), "%d\t%d\t%d", x, y, z);
  return text;
}

std::vector<std::string> split_tabs(const std::string& line) {
  std::vector<std::string> fields;
  size_t pos = 0;
  while (pos <= line.size()) {
    const size_t tab = line.find('\t', pos);
    if (tab == std::string::npos) {
      fields.push_back(line.substr(pos));
      break;
    }
    fields.push_back(line.substr(pos, tab - pos));
    pos = tab + 1;
  }
  return fields;
}

// A fixture file's data rows, i.e. everything that is not a `#` comment. Returns
// false when the file cannot be opened, so a missing fixture fails loudly instead
// of comparing against nothing.
bool read_fixture_rows(const std::string& path, std::vector<std::string>* rows) {
  std::ifstream file(path);
  if (!file)
    return false;
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;
    rows->push_back(line);
  }
  return true;
}

struct Placement {
  int x;
  int y;
  int z;
  std::string name;
  std::string properties;
};

struct Action {
  int tick = 0;
  bool ignite = true;
  int x = 0;
  int y = 0;
  int z = 0;
};

struct RecordedRun {
  int64_t seed = 42;
  std::string name;
  std::string scene;
  int ticks = 0;
  size_t schedules = 0;
  size_t due = 0;
  size_t drops = 0;
  size_t writes = 0;
  size_t reads = 0;
  size_t units = 0;
  size_t draws = 0;
  int64_t end_now = 0;
  size_t end_pending = 0;
  size_t end_chunks = 0;
  size_t rows = 0;
  std::vector<Action> actions;
};

struct RecordedScene {
  std::string name;
  std::vector<Placement> blocks;
};

struct Replay {
  std::vector<std::string> rows;
  size_t schedules = 0;
  size_t due = 0;
  size_t drops = 0;
  size_t writes = 0;
  size_t reads = 0;
  size_t units = 0;
  size_t draws = 0;
  int64_t end_now = 0;
  size_t end_pending = 0;
  size_t end_chunks = 0;
};

// One run of the loop, exactly as `tools/gen_block_tick_dump.cpp` ran it: `ticks`
// advances of the real queue, each due position dispatched by what is standing
// there now, and the caller's own scripted work in the window after each tick.
Replay replay_run(const RecordedRun& run, const RecordedScene& scene) {
  Replay replay;
  int tick = 0;
  auto push = [&replay, &tick](const char* kind, const std::string& body) {
    replay.rows.push_back(std::string(kind) + "\t" + std::to_string(tick) + "\t" + body);
  };

  std::vector<Placement> world;
  world.reserve(scene.blocks.size() + 64);
  for (const Placement& p : scene.blocks)
    world.push_back(p);
  auto raw_get = [&world](int x, int y, int z) {
    for (auto it = world.rbegin(); it != world.rend(); ++it)
      if (it->x == x && it->y == y && it->z == z)
        return BlockStateKey{it->name, it->properties};
    return air_state();
  };
  auto raw_set = [&world](int x, int y, int z, const BlockStateKey& state) {
    world.push_back(Placement{x, y, z, state.name, state.properties});
  };

  BlockTickQueue queue;
  WgLegacyRandom randomness(run.seed);
  auto next_int = [&randomness, &replay, &push](int bound) {
    const int value = randomness.next_int(bound);
    ++replay.draws;
    char text[96];
    std::snprintf(text, sizeof(text), "%d\t%d", bound, value);
    push("draw i", text);
    return value;
  };
  auto next_float = [&randomness, &replay, &push]() {
    const float value = randomness.next_float();
    ++replay.units;
    char text[64];
    std::snprintf(text, sizeof(text), "%.6f", static_cast<double>(value));
    push("draw u", text);
    return value;
  };
  auto get_block = [&raw_get, &replay, &push](int x, int y, int z) {
    const BlockStateKey state = raw_get(x, y, z);
    ++replay.reads;
    push("read", coord(x, y, z) + "\t" + canonical_state(state));
    return state;
  };
  auto is_sturdy = [&raw_get, &push](int x, int y, int z) {
    const bool sturdy = solidish(raw_get(x, y, z).name);
    push("sturdy", coord(x, y, z) + "\t" + (sturdy ? "true" : "false"));
    return sturdy;
  };
  // `BlockTickQueue::schedule`'s own arithmetic: due `max(delay, 1)` ticks from
  // now, where `now` is the tick the schedule is taken in — so a delay D drains
  // exactly D ticks later.
  auto schedule_tick = [&queue, &replay, &push](int x, int y, int z, int delay) {
    const int64_t due = queue.now() + (delay > 0 ? delay : 1);
    queue.schedule(x, y, z, delay);
    ++replay.schedules;
    char text[160];
    std::snprintf(text, sizeof(text), "%s\t%d\t%lld", coord(x, y, z).c_str(), delay,
                  static_cast<long long>(due));
    push("schedule", text);
  };

  std::function<void(int, int, int, const BlockStateKey&, bool)> set_block;
  RandomTickLevel tick_level;
  tick_level.get_block = get_block;
  tick_level.schedule_tick = schedule_tick;
  tick_level.raw_brightness = [&push](int x, int y, int z) {
    push("light", coord(x, y, z) + "\t15");
    return 15;
  };
  tick_level.drop_resources = [&push](int x, int y, int z, const BlockStateKey& state) {
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
  fire_level.prime_tnt = [&push](int x, int y, int z) { push("prime", coord(x, y, z)); };
  // No weather, exactly as the generator left it: null reads as "not raining".

  set_block = [&raw_get, &raw_set, &replay, &push,
               &tick_level](int x, int y, int z, const BlockStateKey& state, bool notify) {
    const BlockStateKey old = raw_get(x, y, z);
    raw_set(x, y, z, state);
    ++replay.writes;
    push("write",
         coord(x, y, z) + "\t" + canonical_state(state) + "\t" + (notify ? "true" : "false"));
    if (notify && old.name != state.name)
      leaves_notify_neighbours_of_change(tick_level, x, y, z, state);
  };

  const FireRandom fire_random{next_int, next_float};
  auto run_action = [&](const Action& action) {
    push("action", std::string(action.ignite ? "ignite" : "break") + "\t" +
                       coord(action.x, action.y, action.z));
    if (action.ignite) {
      const BlockStateKey state = fire_placement_state(fire_level, action.x, action.y, action.z);
      set_block(action.x, action.y, action.z, state, /*notify=*/true);
      if (state.name == kFireBlock)
        schedule_tick(action.x, action.y, action.z,
                      kFireTickMinDelay + next_int(kFireTickDelaySpread));
      return;
    }
    get_block(action.x, action.y, action.z);
    set_block(action.x, action.y, action.z, air_state(), /*notify=*/true);
  };

  for (const Action& action : run.actions)
    if (action.tick == 0)
      run_action(action);

  for (tick = 1; tick <= run.ticks; ++tick) {
    std::vector<BlockTickPos> ordered = queue.advance();
    std::sort(ordered.begin(), ordered.end(), [](const BlockTickPos& a, const BlockTickPos& b) {
      if (a.x != b.x)
        return a.x < b.x;
      if (a.y != b.y)
        return a.y < b.y;
      return a.z < b.z;
    });
    // The fixture's own shape: the canonical order, no position twice. The
    // queue's return order is the bucket layout's and is not pinned, so this is
    // what makes the rows comparable across standard libraries.
    for (size_t i = 0; i < ordered.size(); ++i) {
      CHECK(i == 0 || !(ordered[i - 1] == ordered[i]));
      CHECK(i == 0 || !(ordered[i].x < ordered[i - 1].x));
      push("due", coord(ordered[i].x, ordered[i].y, ordered[i].z));
      ++replay.due;
    }
    for (const BlockTickPos& pos : ordered) {
      const BlockStateKey state = get_block(pos.x, pos.y, pos.z);
      if (fire_is_fire(state)) {
        fire_tick(fire_level, fire_random, kDifficulty, pos.x, pos.y, pos.z);
      } else if (leaves_is_leaves(state.name)) {
        leaves_scheduled_tick(tick_level, state, pos.x, pos.y, pos.z);
      } else {
        ++replay.drops;
        push("drop", coord(pos.x, pos.y, pos.z) + "\t" + canonical_state(state));
      }
    }
    char text[96];
    std::snprintf(text, sizeof(text), "%lld\t%zu\t%zu", static_cast<long long>(queue.now()),
                  queue.size(), queue.chunk_count());
    push("state", text);
    for (const Action& action : run.actions)
      if (action.tick == tick)
        run_action(action);
  }

  replay.end_now = queue.now();
  replay.end_pending = queue.size();
  replay.end_chunks = queue.chunk_count();
  return replay;
}

// Prints the first rows that differ, so a failure names the drift instead of only
// counting it.
void report_mismatch(const RecordedRun& run, const std::vector<std::string>& on_disk,
                     const std::vector<std::string>& replayed) {
  std::fprintf(stderr, "FAIL %s:%d: run '%s': %zu rows on disk, %zu replayed\n", __FILE__, __LINE__,
               run.name.c_str(), on_disk.size(), replayed.size());
  size_t shown = 0;
  const size_t common = std::min(on_disk.size(), replayed.size());
  for (size_t i = 0; i < common && shown < 8; ++i) {
    if (on_disk[i] == replayed[i])
      continue;
    std::fprintf(stderr, "  row %zu:\n    on disk:   %s\n    replayed:  %s\n", i,
                 on_disk[i].c_str(), replayed[i].c_str());
    ++shown;
  }
  for (size_t i = common; i < on_disk.size() && shown < 8; ++i, ++shown)
    std::fprintf(stderr, "  row %zu:\n    on disk:   %s\n    replayed:  <absent>\n", i,
                 on_disk[i].c_str());
  for (size_t i = common; i < replayed.size() && shown < 8; ++i, ++shown)
    std::fprintf(stderr, "  row %zu:\n    on disk:   <absent>\n    replayed:  %s\n", i,
                 replayed[i].c_str());
}

// The golden half: replay every run the meta records and compare its whole
// interaction trace against the checked-in one, plus the caller's own work and
// the queue's end state.
void test_golden_loop(const std::string& fixtures_dir) {
  std::vector<std::string> meta_rows;
  std::vector<std::string> scene_rows;
  std::vector<std::string> action_rows;
  const bool have_meta = read_fixture_rows(fixtures_dir + "/block_tick_meta.tsv", &meta_rows);
  const bool have_scene = read_fixture_rows(fixtures_dir + "/block_tick_scene.tsv", &scene_rows);
  const bool have_actions =
      read_fixture_rows(fixtures_dir + "/block_tick_actions.tsv", &action_rows);
  CHECK(have_meta);
  CHECK(have_scene);
  CHECK(have_actions);
  if (!have_meta || !have_scene || !have_actions)
    return;
  CHECK(!meta_rows.empty());

  std::vector<RecordedRun> runs;
  for (const std::string& row : meta_rows) {
    const std::vector<std::string> fields = split_tabs(row);
    CHECK_EQ(fields.size(), size_t(15));
    if (fields.size() != 15)
      continue;
    RecordedRun run;
    run.seed = std::strtoll(fields[0].c_str(), nullptr, 10);
    run.name = fields[1];
    run.scene = fields[2];
    run.ticks = std::stoi(fields[3]);
    run.schedules = static_cast<size_t>(std::strtoull(fields[4].c_str(), nullptr, 10));
    run.due = static_cast<size_t>(std::strtoull(fields[5].c_str(), nullptr, 10));
    run.drops = static_cast<size_t>(std::strtoull(fields[6].c_str(), nullptr, 10));
    run.writes = static_cast<size_t>(std::strtoull(fields[7].c_str(), nullptr, 10));
    run.reads = static_cast<size_t>(std::strtoull(fields[8].c_str(), nullptr, 10));
    run.units = static_cast<size_t>(std::strtoull(fields[9].c_str(), nullptr, 10));
    run.draws = static_cast<size_t>(std::strtoull(fields[10].c_str(), nullptr, 10));
    run.end_now = std::strtoll(fields[11].c_str(), nullptr, 10);
    run.end_pending = static_cast<size_t>(std::strtoull(fields[12].c_str(), nullptr, 10));
    run.end_chunks = static_cast<size_t>(std::strtoull(fields[13].c_str(), nullptr, 10));
    run.rows = static_cast<size_t>(std::strtoull(fields[14].c_str(), nullptr, 10));
    runs.push_back(run);
  }

  std::vector<RecordedScene> scenes;
  auto scene_for = [&scenes](const std::string& name) -> RecordedScene* {
    for (RecordedScene& scene : scenes)
      if (scene.name == name)
        return &scene;
    return nullptr;
  };
  for (const std::string& row : scene_rows) {
    const std::vector<std::string> fields = split_tabs(row);
    CHECK_EQ(fields.size(), size_t(6));
    if (fields.size() != 6 || fields[0] != "block")
      continue;
    if (scene_for(fields[1]) == nullptr)
      scenes.push_back(RecordedScene{fields[1], {}});
    RecordedScene* scene = scene_for(fields[1]);
    scene->blocks.push_back(Placement{std::stoi(fields[2]), std::stoi(fields[3]),
                                      std::stoi(fields[4]), parse_state(fields[5]).name,
                                      parse_state(fields[5]).properties});
  }

  auto run_for = [&runs](const std::string& name) -> RecordedRun* {
    for (RecordedRun& run : runs)
      if (run.name == name)
        return &run;
    return nullptr;
  };
  for (const std::string& row : action_rows) {
    const std::vector<std::string> fields = split_tabs(row);
    CHECK_EQ(fields.size(), size_t(7));
    if (fields.size() != 7)
      continue;
    RecordedRun* run = run_for(fields[1]);
    CHECK(run != nullptr);
    if (run == nullptr)
      continue;
    Action action;
    action.tick = std::stoi(fields[2]);
    action.ignite = fields[3] == "ignite";
    action.x = std::stoi(fields[4]);
    action.y = std::stoi(fields[5]);
    action.z = std::stoi(fields[6]);
    run->actions.push_back(action);
  }

  size_t checked_rows = 0;
  for (const RecordedRun& run : runs) {
    const RecordedScene* scene = scene_for(run.scene);
    CHECK(scene != nullptr);
    if (scene == nullptr)
      continue;
    CHECK(!scene->blocks.empty());

    const Replay replay = replay_run(run, *scene);
    CHECK_EQ(replay.schedules, run.schedules);
    CHECK_EQ(replay.due, run.due);
    CHECK_EQ(replay.drops, run.drops);
    CHECK_EQ(replay.writes, run.writes);
    CHECK_EQ(replay.reads, run.reads);
    CHECK_EQ(replay.units, run.units);
    CHECK_EQ(replay.draws, run.draws);
    CHECK_EQ(replay.end_now, run.end_now);
    CHECK_EQ(replay.end_pending, run.end_pending);
    CHECK_EQ(replay.end_chunks, run.end_chunks);
    CHECK_EQ(replay.rows.size(), run.rows);
    // Every tick the meta names is closed by exactly one state row, so the
    // queue's clock and pending count are pinned per tick and not only at the end.
    size_t state_rows = 0;
    for (const std::string& row : replay.rows)
      if (row.compare(0, 6, "state\t") == 0)
        ++state_rows;
    CHECK_EQ(state_rows, static_cast<size_t>(run.ticks));

    std::vector<std::string> trace;
    const std::string path =
        fixtures_dir + "/block_tick_seed" + std::to_string(run.seed) + "_" + run.name + ".tsv";
    const bool have_trace = read_fixture_rows(path, &trace);
    CHECK(have_trace);
    if (!have_trace)
      continue;
    // Every action the run replays is one the fixture recorded, and vice versa:
    // a run cannot be replayed with work missing from the file.
    size_t action_rows_in_trace = 0;
    for (const std::string& row : trace)
      if (row.compare(0, 7, "action\t") == 0)
        ++action_rows_in_trace;
    CHECK_EQ(action_rows_in_trace, run.actions.size());
    if (trace == replay.rows) {
      checked_rows += trace.size();
      continue;
    }
    report_mismatch(run, trace, replay.rows);
    ++failures;
  }
  std::printf("block_tick: golden loop replayed for %zu runs, %zu rows compared\n", runs.size(),
              checked_rows);
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <fixtures_dir>\n", argv[0]);
    return 2;
  }
  test_golden_loop(argv[1]);
  if (failures == 0)
    std::printf("block_tick: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
