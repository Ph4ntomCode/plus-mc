// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scheduled fluid-tick loop's golden half (Phase 4 milestone D follow-up):
// replay `tests/fixtures/fluid_tick/` through the real `FluidTickQueue` and the
// real dispatch, and compare every run's whole interaction trace row for row.
//
// The loop's *property* half lives next to the rules it runs rather than here,
// and deliberately: `tests/fluid_test.cpp` pins the state encoding, `fluid_shape`,
// the water/lava drop-off ladder and each branch of `fluid_tick` one call at a
// time, and its `FluidTickQueue` section pins the queue's one-entry-per-(cell,
// kind) rule and its earliest-due rule. What none of them does is *run the loop* —
// many ticks, the queue's bookkeeping feeding back into what the next tick
// drains, writes reshaping the world the later ticks read — which is what this
// test holds, exactly as `tests/fixtures/{worldgen,random_tick,weather,mob_spawn,
// block_tick}` hold their subsystems' sequences. `tools/gen_fluid_tick_dump.cpp`
// owns the vocabulary and the drain arithmetic; this file is the replay.
//
// What it adds over the row comparison, on the fixture's own shape:
//   * each tick's `due` rows are in the canonical order the region path
//     dispatches (x, then y, then z, then the fluid kind — the kind last because
//     the queue's own key allows two entries at one cell and the session's sort
//     key stops at the position) and carry no entry twice;
//   * every `action` row the run has is in `fluid_tick_actions.tsv` and vice
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
#include <utility>
#include <vector>

#include "world/chunk.h"
#include "world/container.h"
#include "world/fluid.h"

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

const BlockStateKey& air_state() {
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
  if (name == "minecraft:bubble_column")
    return FluidKind::Water;
  if (name == "minecraft:water" || name == "minecraft:lava")
    return fluid_state_of(BlockStateKey{std::string(name), ""}).kind;
  return FluidKind::Empty;
}

bool is_fluid_name(std::string_view name) {
  return name == "minecraft:water" || name == "minecraft:lava";
}

bool can_hold_fluid(const BlockStateKey& state) {
  if (state.name == "minecraft:air" || state.name == "minecraft:water" ||
      state.name == "minecraft:lava" || state.name == "minecraft:bubble_column")
    return true;
  return block_property(state.properties, "waterlogged") == "false" &&
         !block_blocks_motion(state.name);
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
  bool place = true;
  int x = 0;
  int y = 0;
  int z = 0;
  std::string name;
  std::string properties;
};

struct RecordedRun {
  std::string name;
  std::string scene;
  int ticks = 0;
  size_t schedules = 0;
  size_t due = 0;
  size_t stales = 0;
  size_t writes = 0;
  size_t reads = 0;
  int64_t end_now = 0;
  size_t end_pending = 0;
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
  size_t stales = 0;
  size_t writes = 0;
  size_t reads = 0;
  int64_t end_now = 0;
  size_t end_pending = 0;
};

using Due = std::pair<FluidKind, FluidTickPos>;

Due make_due(FluidKind kind, const FluidTickPos& pos) {
  return Due{kind, pos};
}

// One run of the loop, exactly as `tools/gen_fluid_tick_dump.cpp` ran it: `ticks`
// advances of the real queue, each entry dispatched by what is standing there now
// (`run_fluid_tick_position`'s shape — the schedule callback rebound to the
// entry's own kind, then `fluid_tick`), and the caller's own scripted work in the
// window after each tick.
Replay replay_run(const RecordedRun& run, const RecordedScene& scene) {
  Replay replay;
  int tick = 0;
  auto push = [&replay, &tick](const char* kind, const std::string& body) {
    replay.rows.push_back(std::string(kind) + "\t" + std::to_string(tick) + "\t" + body);
  };

  std::vector<Placement> world;
  world.reserve(scene.blocks.size() + 128);
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

  FluidTickQueue queue;
  FluidKind tick_kind = FluidKind::Empty;
  auto schedule_at = [&queue, &replay, &push](int x, int y, int z, FluidKind kind, int delay) {
    if (kind == FluidKind::Empty)
      return;
    const int64_t due = queue.now() + (delay > 0 ? delay : 1);
    queue.schedule(x, y, z, kind, delay);
    ++replay.schedules;
    char text[192];
    std::snprintf(text, sizeof(text), "%s\t%s\t%d\t%lld", coord(x, y, z).c_str(),
                  kind_name(kind).c_str(), delay, static_cast<long long>(due));
    push("schedule", text);
  };

  FluidLevel level;
  level.get_block = [&raw_get, &replay, &push](int x, int y, int z) {
    const BlockStateKey state = raw_get(x, y, z);
    ++replay.reads;
    push("read", coord(x, y, z) + "\t" + canonical_state(state));
    return state;
  };
  level.can_hold_fluid = [](const BlockStateKey& state) { return can_hold_fluid(state); };
  level.schedule_tick = [&schedule_at, &tick_kind](int x, int y, int z, int delay) {
    schedule_at(x, y, z, tick_kind, delay);
  };
  level.set_block = [&raw_get, &raw_set, &replay, &push](int x, int y, int z,
                                                         const BlockStateKey& state) {
    if (raw_get(x, y, z) == state)
      return; // the session's adapter writes nothing when the state is there
    raw_set(x, y, z, state);
    ++replay.writes;
    push("write", coord(x, y, z) + "\t" + canonical_state(state));
  };

  auto neighbour_fluid_ticks = [&raw_get, &schedule_at](int x, int y, int z) {
    constexpr int dx[6] = {1, -1, 0, 0, 0, 0};
    constexpr int dy[6] = {0, 0, 1, -1, 0, 0};
    constexpr int dz[6] = {0, 0, 0, 0, 1, -1};
    for (int i = 0; i < 6; ++i) {
      const FluidState fluid = fluid_state_of(raw_get(x + dx[i], y + dy[i], z + dz[i]));
      if (!fluid.empty())
        schedule_at(x + dx[i], y + dy[i], z + dz[i], fluid.kind, fluid_tick_delay(fluid.kind));
    }
  };

  auto run_action = [&](const Action& action) {
    push("action", std::string(action.place ? "place" : "break") + "\t" +
                       coord(action.x, action.y, action.z) + "\t" +
                       canonical_state(BlockStateKey{action.name, action.properties}));
    if (action.place) {
      const BlockStateKey state{action.name, action.properties};
      raw_set(action.x, action.y, action.z, state);
      ++replay.writes;
      push("write", coord(action.x, action.y, action.z) + "\t" + canonical_state(state));
      if (is_fluid_name(state.name))
        schedule_at(action.x, action.y, action.z, kind_from_name(state.name),
                    fluid_tick_delay(kind_from_name(state.name)));
    } else {
      level.get_block(action.x, action.y, action.z);
      raw_set(action.x, action.y, action.z, air_state());
      ++replay.writes;
      push("write", coord(action.x, action.y, action.z) + "\t" + canonical_state(air_state()));
    }
    neighbour_fluid_ticks(action.x, action.y, action.z);
  };

  for (const Action& action : run.actions)
    if (action.tick == 0)
      run_action(action);

  for (tick = 1; tick <= run.ticks; ++tick) {
    std::vector<Due> ordered = queue.advance();
    std::sort(ordered.begin(), ordered.end(), [](const Due& a, const Due& b) {
      if (a.second.x != b.second.x)
        return a.second.x < b.second.x;
      if (a.second.y != b.second.y)
        return a.second.y < b.second.y;
      if (a.second.z != b.second.z)
        return a.second.z < b.second.z;
      return a.first < b.first;
    });
    // The fixture's own shape: the canonical order, no entry twice. The queue's
    // return order is its chunk map's and is not pinned, so this is what makes the
    // rows comparable (and the kind last is what keeps two entries at one cell
    // ordered).
    for (size_t i = 0; i < ordered.size(); ++i) {
      CHECK(i == 0 || !(ordered[i - 1] == ordered[i]));
      if (i > 0 && ordered[i].second.x == ordered[i - 1].second.x &&
          ordered[i].second.y == ordered[i - 1].second.y &&
          ordered[i].second.z == ordered[i - 1].second.z)
        CHECK(ordered[i - 1].first <= ordered[i].first);
      push("due", kind_name(ordered[i].first) + "\t" +
                      coord(ordered[i].second.x, ordered[i].second.y, ordered[i].second.z));
      ++replay.due;
    }
    for (const Due& entry : ordered) {
      const FluidKind kind = entry.first;
      const FluidTickPos& pos = entry.second;
      const FluidState fluid = fluid_state_of(raw_get(pos.x, pos.y, pos.z));
      if (fluid.empty() || fluid.kind != kind) {
        ++replay.stales;
        push("stale", kind_name(kind) + "\t" + coord(pos.x, pos.y, pos.z) + "\t" +
                          canonical_state(raw_get(pos.x, pos.y, pos.z)));
        continue;
      }
      tick_kind = kind;
      fluid_tick(level, kind, pos.x, pos.y, pos.z);
    }
    char text[96];
    std::snprintf(text, sizeof(text), "%lld\t%zu", static_cast<long long>(queue.now()),
                  queue.size());
    push("state", text);
    for (const Action& action : run.actions)
      if (action.tick == tick)
        run_action(action);
  }

  replay.end_now = queue.now();
  replay.end_pending = queue.size();
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
  const bool have_meta = read_fixture_rows(fixtures_dir + "/fluid_tick_meta.tsv", &meta_rows);
  const bool have_scene = read_fixture_rows(fixtures_dir + "/fluid_tick_scene.tsv", &scene_rows);
  const bool have_actions =
      read_fixture_rows(fixtures_dir + "/fluid_tick_actions.tsv", &action_rows);
  CHECK(have_meta);
  CHECK(have_scene);
  CHECK(have_actions);
  if (!have_meta || !have_scene || !have_actions)
    return;
  CHECK(!meta_rows.empty());

  std::vector<RecordedRun> runs;
  for (const std::string& row : meta_rows) {
    const std::vector<std::string> fields = split_tabs(row);
    CHECK_EQ(fields.size(), size_t(11));
    if (fields.size() != 11)
      continue;
    RecordedRun run;
    run.name = fields[0];
    run.scene = fields[1];
    run.ticks = std::stoi(fields[2]);
    run.schedules = static_cast<size_t>(std::strtoull(fields[3].c_str(), nullptr, 10));
    run.due = static_cast<size_t>(std::strtoull(fields[4].c_str(), nullptr, 10));
    run.stales = static_cast<size_t>(std::strtoull(fields[5].c_str(), nullptr, 10));
    run.writes = static_cast<size_t>(std::strtoull(fields[6].c_str(), nullptr, 10));
    run.reads = static_cast<size_t>(std::strtoull(fields[7].c_str(), nullptr, 10));
    run.end_now = std::strtoll(fields[8].c_str(), nullptr, 10);
    run.end_pending = static_cast<size_t>(std::strtoull(fields[9].c_str(), nullptr, 10));
    run.rows = static_cast<size_t>(std::strtoull(fields[10].c_str(), nullptr, 10));
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
    const BlockStateKey state = parse_state(fields[5]);
    scene->blocks.push_back(Placement{std::stoi(fields[2]), std::stoi(fields[3]),
                                      std::stoi(fields[4]), state.name, state.properties});
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
    RecordedRun* run = run_for(fields[0]);
    CHECK(run != nullptr);
    if (run == nullptr)
      continue;
    Action action;
    action.tick = std::stoi(fields[1]);
    action.place = fields[2] == "place";
    action.x = std::stoi(fields[3]);
    action.y = std::stoi(fields[4]);
    action.z = std::stoi(fields[5]);
    const BlockStateKey state = parse_state(fields[6]);
    action.name = state.name;
    action.properties = state.properties;
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
    CHECK_EQ(replay.stales, run.stales);
    CHECK_EQ(replay.writes, run.writes);
    CHECK_EQ(replay.reads, run.reads);
    CHECK_EQ(replay.end_now, run.end_now);
    CHECK_EQ(replay.end_pending, run.end_pending);
    CHECK_EQ(replay.rows.size(), run.rows);
    // Every tick the meta names is closed by exactly one state row, so the
    // queue's clock and pending count are pinned per tick and not only at the end.
    size_t state_rows = 0;
    for (const std::string& row : replay.rows)
      if (row.compare(0, 6, "state\t") == 0)
        ++state_rows;
    CHECK_EQ(state_rows, static_cast<size_t>(run.ticks));

    std::vector<std::string> trace;
    const std::string path = fixtures_dir + "/fluid_tick_" + run.name + ".tsv";
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
  std::printf("fluid_tick: golden loop replayed for %zu runs, %zu rows compared\n", runs.size(),
              checked_rows);
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <fixtures_dir>\n", argv[0]);
    return 2;
  }
  test_golden_loop(argv[1]);
  if (failures == 0) {
    std::printf("fluid_tick: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "fluid_tick: %d check(s) failed\n", failures);
  return 1;
}
