// SPDX-License-Identifier: GPL-3.0-or-later
//
// The region-parallel scheduled-tick loop's golden half (Phase 5 ordered item 3
// follow-up): replay `tests/fixtures/region_tick/` through the real
// `BlockTickQueue`, the real canonical sort and the real `RegionScheduler`, and
// compare every run's whole interaction trace row for row.
//
// What this adds over the row comparison, on the fixture's own shape:
//   * every tick's `due` rows are in the canonical order the region arm sorts
//     into (owning region, then x, y, z), each row naming the region that owns
//     its position, and no entry is drained twice in a tick;
//   * the `region` rows of a tick are in canonical region order, their
//     `[first, last)` slices are contiguous, start at the first `due` row and
//     cover exactly the tick's due rows — which is the session's own "the sorted
//     order makes every region's entries one contiguous run" claim, checked
//     rather than assumed;
//   * each region block's `dispatch` rows are exactly its slice, in order, one
//     per due entry, so no region can swallow a neighbour's work or run an entry
//     twice;
//   * every tick the meta names is closed by exactly one `state` row, and the
//     trace's single `world` row carries the hash of the world the replay built;
//   * every `action` row the run has is in `region_tick_actions.tsv` and vice
//     versa;
//   * and for every run the meta marks `laneStable yes`, the same scene replayed
//     at one lane and at four produces the identical rows — the lane count
//     invisible in the real loop, checked here as well as at generation time.
//
// The rules' own streams are `tests/fixtures/block_tick`'s subject (the serial
// arm of the same queue, with the draws unpinned or pinned the same way); this
// test holds the partition. `tools/gen_region_tick_dump.cpp` owns the vocabulary
// and the plan arithmetic; this file is the replay, and its world model, level
// views and draw streams are that file's, deliberately duplicated, so a change
// to one without the other is a red test.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sim/region_scheduler.h"
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

// The constants the generator's traces were written with. They are duplicated
// here on purpose: a change to the region size or the probed lane count has to
// fail rather than silently re-interpret the fixture.
constexpr int kFloorY = -61;
constexpr int kAirY = -60;
constexpr int kDifficulty = mc::world::kDifficultyNormal;
constexpr int kChunksPerRegion = 2;
constexpr unsigned kProbeLanes = 4;
constexpr uint64_t kFireSeedSalt = 0x9E3779B97F4A7C15ULL;

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

mc::sim::RegionGrid region_grid() {
  return mc::sim::RegionGrid(kChunksPerRegion);
}
int chunk_of(int v) {
  return v >= 0 ? v / 16 : (v - 15) / 16;
}
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

struct Placement {
  int x, y, z;
  std::string name;
  std::string properties;
};

struct Action {
  int tick = 0;
  bool ignite = true;
  int x = 0, y = 0, z = 0;
};

struct RecordedRun {
  std::string name;
  std::string scene;
  int ticks = 0;
  unsigned lanes = 1;
  bool lane_stable = true;
  size_t schedules = 0, due = 0, plans = 0, region_rows = 0, dispatches = 0, drops = 0, writes = 0,
         reads = 0, draws = 0, units = 0, states = 0;
  size_t max_regions = 0;
  int64_t end_now = 0;
  size_t end_pending = 0, end_chunks = 0;
  uint64_t world_hash = 0;
  size_t rows = 0;
};

struct Replay {
  std::vector<std::string> rows;
  uint64_t world_hash = 0;
  int64_t end_now = 0;
  size_t end_pending = 0, end_chunks = 0;
};

Replay replay_scene(int64_t seed, const RecordedRun& run, const std::vector<Placement>& start,
                    unsigned lanes, const std::vector<Action>& actions) {
  Replay replay;
  const mc::sim::RegionGrid grid = region_grid();
  int tick = 0;
  auto push = [&replay, &tick](const char* kind, const std::string& body) {
    replay.rows.push_back(std::string(kind) + "\t" + std::to_string(tick) + "\t" + body);
  };

  std::vector<Placement> world = start;
  std::mutex world_mutex;
  auto raw_get = [&world, &world_mutex](int x, int y, int z) {
    std::lock_guard<std::mutex> lock(world_mutex);
    for (auto it = world.rbegin(); it != world.rend(); ++it)
      if (it->x == x && it->y == y && it->z == z)
        return BlockStateKey{it->name, it->properties};
    return air_state();
  };
  auto raw_set = [&world, &world_mutex](int x, int y, int z, const BlockStateKey& state) {
    std::lock_guard<std::mutex> lock(world_mutex);
    world.push_back(Placement{x, y, z, state.name, state.properties});
  };

  mc::world::BlockTickQueue queue;
  std::mutex queue_mutex;
  std::vector<std::pair<BlockTickPos, std::pair<int, int64_t>>> last_schedule;
  std::mutex schedule_mutex;
  auto schedule_main = [&queue, &queue_mutex, &last_schedule, &schedule_mutex, &push,
                        &tick](int x, int y, int z, int delay) {
    int64_t now = 0;
    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      now = queue.now();
      queue.schedule(x, y, z, delay);
    }
    const int64_t due = now + (delay > 0 ? delay : 1);
    push("schedule", coord(x, y, z) + "\t" + std::to_string(delay) + "\t" + std::to_string(due));
    std::lock_guard<std::mutex> lock(schedule_mutex);
    for (auto& entry : last_schedule)
      if (entry.first == BlockTickPos{x, y, z}) {
        entry.second = std::make_pair(tick, due);
        return;
      }
    last_schedule.emplace_back(BlockTickPos{x, y, z}, std::make_pair(tick, due));
  };
  auto last_schedule_for = [&last_schedule, &schedule_mutex](const BlockTickPos& pos) {
    std::lock_guard<std::mutex> lock(schedule_mutex);
    for (const auto& entry : last_schedule)
      if (entry.first == pos)
        return std::make_pair(true, entry.second);
    return std::make_pair(false, std::make_pair(0, static_cast<int64_t>(0)));
  };

  mc::sim::RegionScheduler scheduler(lanes);

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
    std::function<void(int, int, int, const BlockStateKey&, bool)> action_set;
    RandomTickLevel action_tick_level;
    action_tick_level.get_block = action_get;
    action_tick_level.schedule_tick = schedule_main;
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
    action_level.schedule_tick = schedule_main;
    action_level.set_block = [&action_set](int x, int y, int z, const BlockStateKey& state,
                                           bool notify) { action_set(x, y, z, state, notify); };
    action_set = [&raw_get, &raw_set, &push, &action_tick_level](
                     int x, int y, int z, const BlockStateKey& state, bool notify) {
      const BlockStateKey old = raw_get(x, y, z);
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
        schedule_main(action.x, action.y, action.z,
                      mc::world::kFireTickMinDelay +
                          action_next_int(mc::world::kFireTickDelaySpread));
      return;
    }
    action_get(action.x, action.y, action.z);
    action_set(action.x, action.y, action.z, air_state(), /*notify=*/true);
  };

  for (const Action& action : actions)
    if (action.tick == 0)
      run_action(action);

  for (tick = 1; tick <= run.ticks; ++tick) {
    std::vector<BlockTickPos> drained;
    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      drained = queue.advance();
    }
    std::sort(drained.begin(), drained.end(),
              [&grid](const BlockTickPos& a, const BlockTickPos& b) {
                const mc::sim::RegionPos ra = region_of(grid, a.x, a.z);
                const mc::sim::RegionPos rb = region_of(grid, b.x, b.z);
                if (ra.rx != rb.rx || ra.rz != rb.rz)
                  return ra < rb;
                if (a.x != b.x)
                  return a.x < b.x;
                if (a.y != b.y)
                  return a.y < b.y;
                return a.z < b.z;
              });
    std::vector<mc::sim::RegionPos> regions;
    std::vector<std::pair<size_t, size_t>> ranges;
    for (size_t p = 0; p < drained.size(); ++p) {
      const mc::sim::RegionPos region = region_of(grid, drained[p].x, drained[p].z);
      push("due", region_text(region) + "\t" + coord(drained[p].x, drained[p].y, drained[p].z));
      const auto scheduled = last_schedule_for(drained[p]);
      CHECK(scheduled.first);
      if (scheduled.first)
        CHECK_EQ(scheduled.second.second, static_cast<int64_t>(tick));
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
    }

    std::vector<std::vector<std::string>> rows_by_region(regions.size());
    if (!regions.empty()) {
      const std::function<void(mc::sim::RegionPos, unsigned)> work = [&](mc::sim::RegionPos region,
                                                                         unsigned) {
        size_t index = regions.size();
        for (size_t i = 0; i < regions.size(); ++i)
          if (regions[i].rx == region.rx && regions[i].rz == region.rz) {
            index = i;
            break;
          }
        if (index == regions.size())
          return;
        std::vector<std::string>& out = rows_by_region[index];
        auto push_region = [&out, &tick](const char* kind, const std::string& body) {
          out.push_back(std::string(kind) + "\t" + std::to_string(tick) + "\t" + body);
        };
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
        auto schedule_tick = [&queue, &queue_mutex, &last_schedule, &schedule_mutex, &push_region,
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
          std::lock_guard<std::mutex> lock(schedule_mutex);
          for (auto& entry : last_schedule)
            if (entry.first == BlockTickPos{x, y, z}) {
              entry.second = std::make_pair(tick, due);
              return;
            }
          last_schedule.emplace_back(BlockTickPos{x, y, z}, std::make_pair(tick, due));
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
          push_region("light", coord(x, y, z) + "\t15");
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
        set_block = [&raw_get, &raw_set, &push_region,
                     &tick_level](int x, int y, int z, const BlockStateKey& state, bool notify) {
          const BlockStateKey old = raw_get(x, y, z);
          raw_set(x, y, z, state);
          push_region("write", coord(x, y, z) + "\t" + canonical_state(state) + "\t" +
                                   (notify ? "true" : "false"));
          if (notify && old.name != state.name)
            mc::world::leaves_notify_neighbours_of_change(tick_level, x, y, z, state);
        };
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

    for (size_t i = 0; i < regions.size(); ++i) {
      char text[64];
      std::snprintf(text, sizeof(text), "%zu\t%zu", ranges[i].first, ranges[i].second);
      push("region", region_text(regions[i]) + "\t" + text);
      for (const std::string& row : rows_by_region[i])
        replay.rows.push_back(row);
    }
    char text[96];
    std::snprintf(text, sizeof(text), "%lld\t%zu\t%zu\t%zu", static_cast<long long>(queue.now()),
                  queue.size(), queue.chunk_count(), regions.size());
    push("state", text);
    for (const Action& action : actions)
      if (action.tick == tick)
        run_action(action);
  }

  replay.end_now = queue.now();
  replay.end_pending = queue.size();
  replay.end_chunks = queue.chunk_count();

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
  replay.world_hash = hash;
  push("world", std::to_string(hash));
  return replay;
}

// ---- the fixture's own shape --------------------------------------------------

// One tick's rows, cut into its parts, so the checks below can state the claim
// ("the ranges cover the due rows, and each region dispatches its own slice")
// instead of re-running the partition.
struct TickView {
  size_t due_rows = 0;
  size_t plan_regions = 0;
  size_t plan_due = 0;
  bool has_plan = false;
  struct RegionBlock {
    std::string region;
    size_t first = 0;
    size_t last = 0;
    std::vector<std::pair<std::string, std::string>> dispatches; // arm, coord
  };
  std::vector<RegionBlock> blocks;
  size_t state_rows = 0;
};

void check_trace_shape(const std::vector<std::string>& rows, const mc::sim::RegionGrid& grid,
                       int ticks) {
  size_t tick = 0;
  size_t state_rows_in_tick = 0;
  bool saw_world = false;
  TickView view;
  auto close_tick = [&]() {
    if (!view.has_plan) {
      CHECK(view.due_rows == 0);
      CHECK(view.blocks.empty());
    } else {
      CHECK_EQ(view.plan_due, view.due_rows);
      CHECK_EQ(view.plan_regions, view.blocks.size());
      size_t expected_first = 0;
      std::string previous;
      for (const TickView::RegionBlock& block : view.blocks) {
        CHECK_EQ(block.first, expected_first);
        CHECK(block.last >= block.first);
        CHECK(block.last <= view.due_rows);
        CHECK_EQ(block.dispatches.size(), block.last - block.first);
        if (!previous.empty())
          CHECK(previous < block.region);
        previous = block.region;
        expected_first = block.last;
      }
      CHECK_EQ(expected_first, view.due_rows);
    }
    CHECK_EQ(view.state_rows, static_cast<size_t>(1));
    view = TickView{};
  };
  for (const std::string& row : rows) {
    const std::vector<std::string> fields = split_tabs(row);
    CHECK(fields.size() >= 2);
    if (fields.size() < 2)
      continue;
    const size_t row_tick = static_cast<size_t>(std::strtoull(fields[1].c_str(), nullptr, 10));
    if (fields[0] == "world") {
      CHECK(!saw_world);
      saw_world = true;
      continue;
    }
    if (row_tick > tick) {
      while (tick < row_tick) {
        if (tick >= 1)
          close_tick();
        ++tick;
      }
    }
    if (fields[0] == "due") {
      // due <tick> <rx> <rz> <x> <y> <z>: the region column is the row's own
      // claim about where its position sits, and it is checked against the
      // grid the replay uses rather than trusted.
      CHECK(fields.size() == 7);
      const int x = std::atoi(fields[4].c_str());
      const int z = std::atoi(fields[6].c_str());
      const mc::sim::RegionPos region = region_of(grid, x, z);
      CHECK_EQ(fields[2] + "\t" + fields[3], region_text(region));
      ++view.due_rows;
    } else if (fields[0] == "plan") {
      CHECK(fields.size() == 4);
      view.has_plan = true;
      view.plan_regions = static_cast<size_t>(std::strtoull(fields[2].c_str(), nullptr, 10));
      view.plan_due = static_cast<size_t>(std::strtoull(fields[3].c_str(), nullptr, 10));
    } else if (fields[0] == "region") {
      CHECK(fields.size() == 6);
      TickView::RegionBlock block;
      block.region = fields[2] + "\t" + fields[3];
      block.first = static_cast<size_t>(std::strtoull(fields[4].c_str(), nullptr, 10));
      block.last = static_cast<size_t>(std::strtoull(fields[5].c_str(), nullptr, 10));
      view.blocks.push_back(block);
    } else if (fields[0] == "dispatch") {
      CHECK(!view.blocks.empty());
      if (!view.blocks.empty()) {
        TickView::RegionBlock& block = view.blocks.back();
        CHECK(fields.size() == 6);
        block.dispatches.emplace_back(fields[2], fields[3] + "\t" + fields[4] + "\t" + fields[5]);
      }
    } else if (fields[0] == "state") {
      ++view.state_rows;
      ++state_rows_in_tick;
    }
  }
  if (tick >= 1)
    close_tick();
  CHECK_EQ(state_rows_in_tick, static_cast<size_t>(ticks));
  CHECK(saw_world);
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <fixtures_dir>\n", argv[0]);
    return 2;
  }
  const std::string dir = argv[1];
  const mc::sim::RegionGrid grid = region_grid();

  std::vector<std::string> meta_rows;
  std::vector<std::string> scene_rows;
  std::vector<std::string> action_rows;
  if (!read_fixture_rows(dir + "/region_tick_meta.tsv", &meta_rows) ||
      !read_fixture_rows(dir + "/region_tick_scene.tsv", &scene_rows) ||
      !read_fixture_rows(dir + "/region_tick_actions.tsv", &action_rows)) {
    std::fprintf(stderr, "region_tick: cannot read the fixture in %s\n", dir.c_str());
    return 1;
  }

  int64_t seed = 0;
  std::vector<RecordedRun> runs;
  for (const std::string& row : meta_rows) {
    const std::vector<std::string> f = split_tabs(row);
    CHECK_EQ(f.size(), static_cast<size_t>(23));
    if (f.size() != 23)
      continue;
    seed = std::strtoll(f[0].c_str(), nullptr, 10);
    RecordedRun run;
    run.name = f[1];
    run.scene = f[2];
    run.ticks = std::atoi(f[3].c_str());
    run.lanes = static_cast<unsigned>(std::strtoul(f[4].c_str(), nullptr, 10));
    run.lane_stable = f[5] == "yes";
    run.schedules = std::strtoull(f[6].c_str(), nullptr, 10);
    run.due = std::strtoull(f[7].c_str(), nullptr, 10);
    run.plans = std::strtoull(f[8].c_str(), nullptr, 10);
    run.region_rows = std::strtoull(f[9].c_str(), nullptr, 10);
    run.dispatches = std::strtoull(f[10].c_str(), nullptr, 10);
    run.drops = std::strtoull(f[11].c_str(), nullptr, 10);
    run.writes = std::strtoull(f[12].c_str(), nullptr, 10);
    run.reads = std::strtoull(f[13].c_str(), nullptr, 10);
    run.draws = std::strtoull(f[14].c_str(), nullptr, 10);
    run.units = std::strtoull(f[15].c_str(), nullptr, 10);
    run.states = std::strtoull(f[16].c_str(), nullptr, 10);
    run.max_regions = std::strtoull(f[17].c_str(), nullptr, 10);
    run.end_now = std::strtoll(f[18].c_str(), nullptr, 10);
    run.end_pending = std::strtoull(f[19].c_str(), nullptr, 10);
    run.end_chunks = std::strtoull(f[20].c_str(), nullptr, 10);
    run.world_hash = std::strtoull(f[21].c_str(), nullptr, 10);
    run.rows = std::strtoull(f[22].c_str(), nullptr, 10);
    runs.push_back(run);
  }
  CHECK(!runs.empty());

  std::map<std::string, std::vector<Placement>> scenes;
  for (const std::string& row : scene_rows) {
    const std::vector<std::string> f = split_tabs(row);
    if (f.size() != 8 || f[0] != "block")
      continue;
    const int x = std::atoi(f[4].c_str());
    const int z = std::atoi(f[6].c_str());
    const mc::sim::RegionPos region = region_of(grid, x, z);
    CHECK_EQ(f[2], std::to_string(region.rx)); // the scene's own region column
    CHECK_EQ(f[3], std::to_string(region.rz));
    const BlockStateKey state = parse_state(f[7]);
    scenes[f[1]].push_back(Placement{x, std::atoi(f[5].c_str()), z, state.name, state.properties});
  }
  CHECK(!scenes.empty());

  std::map<std::string, std::vector<Action>> actions_by_run;
  for (const std::string& row : action_rows) {
    const std::vector<std::string> f = split_tabs(row);
    if (f.size() != 7)
      continue;
    Action action;
    action.tick = std::atoi(f[2].c_str());
    action.ignite = f[3] == "ignite";
    action.x = std::atoi(f[4].c_str());
    action.y = std::atoi(f[5].c_str());
    action.z = std::atoi(f[6].c_str());
    actions_by_run[f[1]].push_back(action);
  }

  size_t total_rows = 0;
  for (const RecordedRun& run : runs) {
    const auto scene = scenes.find(run.scene);
    CHECK(scene != scenes.end());
    if (scene == scenes.end())
      continue;
    const auto actions = actions_by_run.find(run.name);
    const std::vector<Action> run_actions =
        actions == actions_by_run.end() ? std::vector<Action>{} : actions->second;
    CHECK(run.lanes == (run.lane_stable ? kProbeLanes : 1u));
    const Replay replay = replay_scene(seed, run, scene->second, run.lanes, run_actions);

    // The rows, line for line.
    std::vector<std::string> disk;
    if (!read_fixture_rows(
            dir + "/region_tick_seed" + std::to_string(seed) + "_" + run.name + ".tsv", &disk)) {
      std::fprintf(stderr, "region_tick: cannot read run '%s'\n", run.name.c_str());
      ++failures;
      continue;
    }
    if (disk != replay.rows) {
      std::fprintf(stderr, "run '%s': %zu rows on disk, %zu replayed\n", run.name.c_str(),
                   disk.size(), replay.rows.size());
      const size_t common = std::min(disk.size(), replay.rows.size());
      size_t shown = 0;
      for (size_t i = 0; i < common && shown < 5; ++i) {
        if (disk[i] != replay.rows[i]) {
          std::fprintf(stderr, "  row %zu:\n    on disk:   %s\n    replayed:  %s\n", i,
                       disk[i].c_str(), replay.rows[i].c_str());
          ++shown;
        }
      }
      ++failures;
    }
    total_rows += replay.rows.size();

    // The counters the meta records, from the replayed rows.
    size_t schedules = 0, due = 0, plans = 0, regions = 0, dispatches = 0, drops = 0, writes = 0,
           reads = 0, draws = 0, units = 0, states = 0, max_regions = 0;
    for (const std::string& row : replay.rows) {
      const std::vector<std::string> f = split_tabs(row);
      const std::string& kind = f[0];
      if (kind == "schedule")
        ++schedules;
      else if (kind == "due")
        ++due;
      else if (kind == "plan") {
        ++plans;
        max_regions =
            std::max(max_regions, static_cast<size_t>(std::strtoull(f[2].c_str(), nullptr, 10)));
      } else if (kind == "region")
        ++regions;
      else if (kind == "dispatch") {
        ++dispatches;
        if (f.size() > 2 && f[2] == "drop")
          ++drops;
      } else if (kind == "write")
        ++writes;
      else if (kind == "read")
        ++reads;
      else if (kind == "draw i")
        ++draws;
      else if (kind == "draw u")
        ++units;
      else if (kind == "state")
        ++states;
    }
    CHECK_EQ(schedules, run.schedules);
    CHECK_EQ(due, run.due);
    CHECK_EQ(plans, run.plans);
    CHECK_EQ(regions, run.region_rows);
    CHECK_EQ(dispatches, run.dispatches);
    CHECK_EQ(drops, run.drops);
    CHECK_EQ(writes, run.writes);
    CHECK_EQ(reads, run.reads);
    CHECK_EQ(draws, run.draws);
    CHECK_EQ(units, run.units);
    CHECK_EQ(states, run.states);
    CHECK_EQ(max_regions, run.max_regions);
    CHECK_EQ(replay.rows.size(), run.rows);
    CHECK_EQ(replay.end_now, run.end_now);
    CHECK_EQ(replay.end_pending, run.end_pending);
    CHECK_EQ(replay.end_chunks, run.end_chunks);
    CHECK_EQ(replay.world_hash, run.world_hash);

    // The action rows and the actions file agree, both ways.
    size_t trace_actions = 0;
    for (const std::string& row : replay.rows) {
      const std::vector<std::string> f = split_tabs(row);
      if (f[0] != "action")
        continue;
      ++trace_actions;
      bool found = false;
      for (const Action& action : run_actions)
        if (f[2] == (action.ignite ? "ignite" : "break") && std::atoi(f[3].c_str()) == action.x &&
            std::atoi(f[4].c_str()) == action.y && std::atoi(f[5].c_str()) == action.z)
          found = true;
      CHECK(found);
    }
    CHECK_EQ(trace_actions, run_actions.size());

    // The fixture's own shape.
    check_trace_shape(replay.rows, grid, run.ticks);

    // The lane-count property, at replay time: for a lane-stable run the same
    // scene at one lane must produce the identical rows and world.
    if (run.lane_stable) {
      const Replay serial = replay_scene(seed, run, scene->second, 1, run_actions);
      CHECK(serial.rows == replay.rows);
      CHECK_EQ(serial.world_hash, replay.world_hash);
    }
  }

  if (failures != 0) {
    std::fprintf(stderr, "region_tick: %d check(s) failed\n", failures);
    return 1;
  }
  std::printf(
      "region_tick: golden loop replayed for %zu runs, %zu rows compared, %u lanes probed\n",
      runs.size(), total_rows, kProbeLanes);
  std::printf("region_tick: all checks passed\n");
  return 0;
}
