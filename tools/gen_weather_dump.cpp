// SPDX-License-Identifier: GPL-3.0-or-later
//
// Writes the precipitation pass's golden output for a small set of recorded
// scenes: the run parameters (`weather_meta.tsv`), the world each run starts
// from (`weather_scene.tsv`) and one interaction trace per run
// (`weather_seed42_<run>.tsv`).
//
// This is **not** a vanilla dump, and cannot be. Two streams decide what the
// pass does, and both are the level's own rather than a scripted test's: the
// 1-in-48 gate is drawn from `Level.random` (a `LegacyRandomSource`, whose
// algorithm is ported exactly but whose *state* on any given tick is the
// server's), and the sampled column is `Level.randValue` (`BlockRandomPos`), an
// LCG the level seeds at construction. So there is no jar output to compare
// against — only this runtime's own — and what the trace pins is the half
// `tests/weather_test.cpp` cannot: that test scripts every draw, so it pins each
// *branch* but never the sequence a real stream produces.
//
// The trace is a full interaction log, not just the fills. Every level read and
// every draw the pass makes, in order, so a reordered query or a gate consumed
// one step too early moves a row:
//
//   gate   <tick> <draw result> <sampled|skipped>
//   column <tick> <x> <z> <randValue after the step>
//   biome  <tick> <x> <y> <z> <name> <none|rain|snow>
//   read   <tick> <x> <y> <z> <state>
//   roll   <tick> <value>
//   fill   <tick> <x> <y> <z> <state>
//
// `gate` is the pass's own `random.nextInt(48) != 0`, drawn whether or not
// anything can be filled. A draw that passes the gate always steps the column LCG,
// because `rand.next` is the *argument* to `tick_precipitation` and the weather
// check is inside it — so a clear-sky run logs its gates, steps the LCG exactly
// once per gate that passed, and logs no `column` row at all (the height query is
// behind the rain check). `column` is the sampled column: `randValue`
// is the state right after `BlockRandomPos.next`, and x/z come from that draw,
// so the LCG's own sequence is in the file. `biome` is `getBiome(topPos)` — the
// first free cell above the heightmap — with the precipitation it answers, and
// `read` is the block one below it (`belowPos`), which is what the cauldron check
// is asked about; the two positions differ, which is vanilla's own split and the
// reason a cauldron standing at the top of its own column still fills. `roll` is
// a cauldron's one chance draw (0.05 rain / 0.1 snow): only a block that has a
// precipitation handler draws at all, which is why `read` stone or air is
// followed by nothing. `fill` is the write.
//
// Usage:
//   plusmc-weather-dump <fixtures_dir> [seed] [--diff]
//
// `--diff` writes nothing: it re-runs every scene in memory, reads what is
// checked in, and prints a patch of each file that would change, exiting 1 while
// the fixture differs. The review is
//
//   ./build/plusmc-weather-dump <fixtures_dir> --diff   # read the patch
//   ./build/plusmc-weather-dump <fixtures_dir>          # then rewrite it
//   ctest -R weather                                    # and re-pin it
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "fixture_diff.h"
#include "world/chunk.h"
#include "world/weather.h"
#include "world/worldgen_random.h"

namespace {

using mc::world::BlockStateKey;
using mc::world::Precipitation;

constexpr int kSeaLevel = mc::world::kOverworldSeaLevel;

struct Placement {
  int x;
  int y;
  int z;
  const char* name;
  const char* properties;
};

struct HeightOverride {
  int x;
  int z;
  int top;
};

// A 3D biome override: that one cell's own biome name. The precipitation pass
// asks the biome at the *top* of the column and reads the block one below it, so
// an override on a single cell is how that split is pinned rather than described.
struct BiomeOverride {
  int x;
  int y;
  int z;
  const char* name;
};

struct Scene {
  const char* name;
  const char* biome; // the biome every cell answers unless overridden
  int default_height;
};

// Every cauldron stands at `height - 1` so that its own column's rain lands on it
// (the geometry `test_fire_column_precipitation` documents). The `(x + z) % 7`
// ladder covers every branch of the cauldron check: an empty one, three layered
// water levels (the last FULL, which draws and then stops), a powder-snow one
// (a kind mismatch under rain), and stone and air, which have no handler and so
// take no draw at all.
std::vector<Placement> cauldron_field() {
  std::vector<Placement> out;
  for (int x = 0; x < 16; ++x) {
    for (int z = 0; z < 16; ++z) {
      switch ((x + z) % 7) {
      case 0:
        out.push_back({x, 65, z, "minecraft:cauldron", ""});
        break;
      case 1:
        out.push_back({x, 65, z, "minecraft:water_cauldron", "level=1"});
        break;
      case 2:
        out.push_back({x, 65, z, "minecraft:water_cauldron", "level=2"});
        break;
      case 3:
        out.push_back({x, 65, z, "minecraft:water_cauldron", "level=3"}); // FULL
        break;
      case 4:
        out.push_back({x, 65, z, "minecraft:powder_snow_cauldron", "level=1"});
        break;
      case 5:
        out.push_back({x, 65, z, "minecraft:stone", ""});
        break;
      default:
        break; // air
      }
    }
  }
  return out;
}

// The roofed strip: columns x 0..3 at z 8 have their heightmap at 70 (a stone
// roof at 69) with an empty cauldron left standing at 65 underneath. Rain
// reaches the roof, so those four columns draw nothing even in a downpour.
std::vector<Placement> roofed_blocks() {
  std::vector<Placement> out;
  for (int x = 0; x < 4; ++x) {
    out.push_back({x, 69, 8, "minecraft:stone", ""});
    out.push_back({x, 65, 8, "minecraft:cauldron", ""});
  }
  return out;
}

std::vector<Placement> scene_blocks(const std::string& scene) {
  std::vector<Placement> out = cauldron_field();
  if (scene == "rain" || scene == "clear") {
    const std::vector<Placement> roof = roofed_blocks();
    out.insert(out.end(), roof.begin(), roof.end());
  }
  return out;
}

std::vector<HeightOverride> scene_heights(const std::string& scene) {
  std::vector<HeightOverride> out;
  if (scene == "rain" || scene == "clear") {
    for (int x = 0; x < 4; ++x)
      out.push_back({x, 8, 70});
  }
  return out;
}

// One cell of the rain scene carries a biome of its own: the pass asks that cell
// (the first free one above the heightmap, y 66) and then reads the block one
// below it (y 65), so a single override proves the two positions are not the same
// query — and it turns that column's cauldron into a snow one.
std::vector<BiomeOverride> scene_biomes(const std::string& scene) {
  std::vector<BiomeOverride> out;
  if (scene == "rain")
    out.push_back({15, 66, 15, "minecraft:snowy_plains"});
  return out;
}

const Scene kScenes[] = {
    {"rain", "minecraft:plains", 66},
    {"snow", "minecraft:snowy_plains", 66},
    {"clear", "minecraft:plains", 66},
};

struct RecordedRun {
  const char* name;
  const char* scene;
  int tick_speed;
  int ticks;
  float rain_level;
  float thunder_level;
  // Whether every 1-in-48 gate draw is written to the trace, or only the draws
  // that passed it. Logging all of them is the readable half of the format, but
  // there is one per `tickSpeed` per tick, so the long runs — the ones with
  // enough samples for the 5% chance to actually fill something — would be
  // almost nothing but gate rows. The `stream` run below is the short one that
  // shows the whole sequence; the long ones pin the samples, the rolls and the
  // writes, and their gate count is in the meta either way.
  bool log_gates;
  // The coverage this run exists for: how many fills it must produce, or the
  // fixture would be written with a write path nothing exercised. A raining run
  // that never fills is regenerated rather than checked in.
  int min_fills;
};

// Five runs over three scenes. `stream` is a short run that logs every gate draw
// (the format's readable half); `rain` and `snow` are long enough that the
// cauldron chance actually lands a handful of fills, in a warm biome and a cold
// one, so the write path and both fill kinds are pinned; `clear` is the field
// with the sky clear, where the pass must step its column LCG and fill nothing;
// and `off` is the gamerule's zero, where it must not draw at all.
constexpr RecordedRun kRuns[] = {
    {"stream", "rain", 3, 40, 1.0f, 0.0f, true, 0},
    {"rain", "rain", 3, 10000, 1.0f, 0.0f, false, 1},
    {"snow", "snow", 3, 10000, 1.0f, 0.0f, false, 1},
    {"clear", "clear", 3, 10000, 0.0f, 0.0f, false, 0},
    {"off", "rain", 0, 600, 1.0f, 0.0f, false, 0},
};

std::string canonical_state(const BlockStateKey& key) {
  std::string text = key.name;
  if (!key.properties.empty()) {
    text += '[';
    text += key.properties;
    text += ']';
  }
  return text;
}

const char* precipitation_name(Precipitation precipitation) {
  switch (precipitation) {
  case Precipitation::kRain:
    return "rain";
  case Precipitation::kSnow:
    return "snow";
  default:
    return "none";
  }
}

struct RunResult {
  std::vector<std::string> rows;
  int32_t rand_start = 0;
  int32_t rand_end = 0;
  size_t gates = 0;         // `nextInt(48)` calls
  size_t sampled_gates = 0; // those that answered 0
  size_t lcg_steps = 0;     // how often `BlockRandomPos` moved
  size_t samples = 0;       // height queries, i.e. samples the rain reached
  size_t rolls = 0;
  size_t fills = 0;
};

size_t count_of_kind(const std::vector<std::string>& rows, const char* kind) {
  size_t count = 0;
  const std::string prefix = std::string(kind) + "\t";
  for (const std::string& row : rows)
    if (row.compare(0, prefix.size(), prefix) == 0)
      ++count;
  return count;
}

// The subset of a kind's rows that also end in `suffix` (the gate rows that
// passed the 1-in-48 draw).
size_t count_of_kind_matching(const std::vector<std::string>& rows, const char* kind,
                              const char* suffix) {
  size_t count = 0;
  const std::string prefix = std::string(kind) + "\t";
  const std::string tail = suffix;
  for (const std::string& row : rows) {
    if (row.compare(0, prefix.size(), prefix) != 0)
      continue;
    if (row.size() >= tail.size() && row.compare(row.size() - tail.size(), tail.size(), tail) == 0)
      ++count;
  }
  return count;
}

// The invariants that make a run's trace a stream pin rather than a transcript
// of whatever happened. A violation means the fixture would pin a lie, so the
// writer refuses; `--diff` only reports it (and still prints the patch), because
// a violated invariant is itself the drift a reviewer is looking for.
std::vector<std::string> invariant_violations(const RecordedRun& run, const RunResult& result) {
  std::vector<std::string> out;
  char message[512];
  const size_t expected_gates =
      static_cast<size_t>(run.tick_speed > 0 ? run.tick_speed : 0) * static_cast<size_t>(run.ticks);
  if (result.gates != expected_gates) {
    std::snprintf(message, sizeof(message), "run '%s': %zu gate draws, expected %zu", run.name,
                  result.gates, expected_gates);
    out.push_back(message);
  }
  if (result.samples > result.gates) {
    std::snprintf(message, sizeof(message), "run '%s': %zu samples from %zu gates", run.name,
                  result.samples, result.gates);
    out.push_back(message);
  }
  // The column LCG advances exactly once per draw that passed the gate, and every
  // step is logged, whatever the weather is.
  if (result.sampled_gates != result.lcg_steps ||
      count_of_kind(result.rows, "lcg") != result.lcg_steps) {
    std::snprintf(message, sizeof(message), "run '%s': %zu sampled gates but %zu LCG steps",
                  run.name, result.sampled_gates, result.lcg_steps);
    out.push_back(message);
  }
  if (run.log_gates &&
      count_of_kind_matching(result.rows, "gate", "\tsampled") != result.sampled_gates) {
    std::snprintf(message, sizeof(message), "run '%s': the logged gates disagree with the stream",
                  run.name);
    out.push_back(message);
  }
  if (result.samples != (run.rain_level > mc::world::kRainLevelThreshold ? result.lcg_steps : 0)) {
    std::snprintf(message, sizeof(message), "run '%s': %zu height queries for %zu sampled draws",
                  run.name, result.samples, result.lcg_steps);
    out.push_back(message);
  }
  if (run.rain_level <= mc::world::kRainLevelThreshold &&
      (result.samples != 0 || result.fills != 0 || result.rolls != 0)) {
    std::snprintf(message, sizeof(message), "run '%s': a clear-sky run filled something", run.name);
    out.push_back(message);
  }
  if (run.tick_speed <= 0 &&
      (result.gates != 0 || result.lcg_steps != 0 || result.rand_end != result.rand_start)) {
    std::snprintf(message, sizeof(message), "run '%s': randomTickSpeed 0 drew something", run.name);
    out.push_back(message);
  }
  if (count_of_kind(result.rows, "fill") != result.fills ||
      count_of_kind(result.rows, "roll") != result.rolls ||
      count_of_kind(result.rows, "column") != result.samples) {
    std::snprintf(message, sizeof(message), "run '%s': the counters disagree with the trace",
                  run.name);
    out.push_back(message);
  }
  if (result.fills < static_cast<size_t>(run.min_fills)) {
    std::snprintf(message, sizeof(message),
                  "run '%s': %zu fills, and this run exists for at least %d", run.name,
                  result.fills, run.min_fills);
    out.push_back(message);
  }
  return out;
}

// ---- one run -----------------------------------------------------------------

RunResult run_scene(int64_t seed, const RecordedRun& run) {
  const std::vector<Placement> blocks = scene_blocks(run.scene);
  const std::vector<HeightOverride> heights = scene_heights(run.scene);
  const std::vector<BiomeOverride> biomes = scene_biomes(run.scene);
  const std::string scene_biome = [&run]() {
    for (const Scene& scene : kScenes)
      if (std::strcmp(scene.name, run.scene) == 0)
        return std::string(scene.biome);
    return std::string("minecraft:the_void");
  }();
  const int scene_height = [&run]() {
    for (const Scene& scene : kScenes)
      if (std::strcmp(scene.name, run.scene) == 0)
        return scene.default_height;
    return 0;
  }();

  RunResult result;
  mc::world::Weather weather;
  weather.rain_level = run.rain_level;
  weather.previous_rain_level = run.rain_level;
  weather.thunder_level = run.thunder_level;
  weather.previous_thunder_level = run.thunder_level;
  weather.raining = run.rain_level > mc::world::kRainLevelThreshold;
  weather.thundering = run.thunder_level > mc::world::kThunderLevelThreshold;

  // `Level.randValue`, seeded from the level's own random at construction.
  mc::world::WgLegacyRandom level_random(seed);
  mc::world::BlockRandomPos rand;
  rand.value = level_random.next_int();
  result.rand_start = rand.value;

  // The level's own `random`, one stream for both the gate and the cauldron
  // rolls, exactly as the server shares one.
  mc::world::WgLegacyRandom randomness(seed);
  mc::world::WeatherRandom pass_rng;

  int tick = 0;
  auto push = [&result](std::string row) { result.rows.push_back(std::move(row)); };

  const bool log_gates = run.log_gates;
  pass_rng.next_int = [&randomness, &result, &tick, &push, log_gates](int bound) {
    const int value = randomness.next_int(bound);
    if (bound == mc::world::kPrecipitationSampleGate) {
      ++result.gates;
      if (value == 0)
        ++result.sampled_gates;
      if (log_gates) {
        char text[128];
        std::snprintf(text, sizeof(text), "%d\t%d\t%s", tick, value,
                      value == 0 ? "sampled" : "skipped");
        push(std::string("gate\t") + text);
      }
    }
    return value;
  };
  pass_rng.next_float = [&randomness, &result, &tick, &push]() {
    const float value = randomness.next_float();
    ++result.rolls;
    char text[64];
    std::snprintf(text, sizeof(text), "%d\t%.6f", tick, static_cast<double>(value));
    push(std::string("roll\t") + text);
    return value;
  };

  mc::world::PrecipitationLevel level;
  level.get_block = [&](int x, int y, int z) {
    BlockStateKey block{"minecraft:air", ""};
    for (const Placement& placement : blocks)
      if (placement.x == x && placement.y == y && placement.z == z)
        block = BlockStateKey{placement.name, placement.properties};
    char text[320];
    std::snprintf(text, sizeof(text), "%d\t%d\t%d\t%d\t%s", tick, x, y, z,
                  canonical_state(block).c_str());
    push(std::string("read\t") + text);
    return block;
  };
  level.set_block = [&](int x, int y, int z, const BlockStateKey& state) {
    ++result.fills;
    char text[320];
    std::snprintf(text, sizeof(text), "%d\t%d\t%d\t%d\t%s", tick, x, y, z,
                  canonical_state(state).c_str());
    push(std::string("fill\t") + text);
  };
  level.motion_blocking_height = [&](int x, int z) {
    int top = scene_height;
    for (const HeightOverride& height : heights)
      if (height.x == x && height.z == z)
        top = height.top;
    // `tick_precipitation` asks this once per sampled column, so the row is the
    // sample itself: the LCG state after the draw plus the column it produced.
    ++result.samples;
    char text[256];
    std::snprintf(text, sizeof(text), "%d\t%d\t%d\t%d", tick, x, z, rand.value);
    push(std::string("column\t") + text);
    return top;
  };
  level.biome_at = [&](int x, int y, int z) {
    std::string name = scene_biome;
    for (const BiomeOverride& biome : biomes)
      if (biome.x == x && biome.y == y && biome.z == z)
        name = biome.name;
    char text[320];
    std::snprintf(text, sizeof(text), "%d\t%d\t%d\t%d\t%s\t%s", tick, x, y, z, name.c_str(),
                  precipitation_name(mc::world::biome_precipitation_at(name, y, kSeaLevel)));
    push(std::string("biome\t") + text);
    return name;
  };

  // One `tick_chunk_precipitation` call per draw, with `tick_speed` 1, so that
  // each call is one gate draw and at most one column sample: the LCG's steps can
  // then be counted exactly (per call) without a hook inside the pass, which the
  // real `randomTickSpeed` loop would hide. A `tickSpeed` of 0 is the gamerule's
  // own early return, called once per tick so that it is the pass being asked and
  // not the harness skipping it.
  const int draws = run.tick_speed > 0 ? run.tick_speed : 1;
  const int speed = run.tick_speed > 0 ? 1 : 0;
  for (tick = 0; tick < run.ticks; ++tick) {
    for (int draw = 0; draw < draws; ++draw) {
      const int32_t before = rand.value;
      mc::world::tick_chunk_precipitation(level, pass_rng, weather, rand, kSeaLevel, speed, 0, 0);
      if (rand.value != before) {
        ++result.lcg_steps;
        // The column draw is not observable from inside, but it *is* visible
        // from here: one call is one gate draw, and the LCG moved, so this row
        // is that step's value. In a raining run the `column` row below repeats
        // it; in a clear one this is the only trace of the sample at all.
        char text[128];
        std::snprintf(text, sizeof(text), "%d\t%d\t%d", tick, draw, rand.value);
        push(std::string("lcg\t") + text);
      }
    }
  }

  result.rand_end = rand.value;
  return result;
}

// ---- the artifacts ------------------------------------------------------------

std::string meta_text(int64_t seed, const std::vector<RecordedRun>& runs,
                      const std::vector<RunResult>& results) {
  std::string text;
  text += "# Phase 3 milestone H: the precipitation pass's golden run parameters and"
          " stream ends.\n"
          "#\n"
          "# NOT a vanilla dump: the 1-in-48 gate is drawn from `Level.random` and the"
          " sampled\n"
          "# column from `Level.randValue` (`BlockRandomPos`), two streams the level"
          " seeds itself,\n"
          "# so this is a regression pin of this runtime's own output rather than a"
          " comparison\n"
          "# against the game jar. `ctest -R weather` replays every row below through"
          " the real\n"
          "# `tick_chunk_precipitation` and compares the run's trace line for line, so a"
          " reordered\n"
          "# query or a gate consumed one step early cannot land silently. Regenerate"
          " deliberately\n"
          "# with `plusmc-weather-dump <fixtures_dir>`, reviewing the change first with"
          " its `--diff`\n"
          "# mode.\n"
          "#\n"
          "# `rainLevel`/`thunderLevel` are the ramped levels `isRaining` reads. They"
          " are set\n"
          "# directly rather than ramped: the cycle is pinned by the weather test's own"
          " assertions,\n"
          "# and this fixture is about the precipitation pass. `randStart`/`randEnd` are"
          " `BlockRandomPos.value`\n"
          "# before and after the run — a pass that sampled one column too many or too"
          " few ends\n"
          "# somewhere else even when it filled the same cells, and `randomTickSpeed` 0"
          " must end\n"
          "# exactly where it started. `gates` is the number of `nextInt(48)` draws"
          " (always\n"
          "# `tickSpeed * ticks`), `sampledGates` the number of those that answered 0,"
          " and `lcgSteps`\n"
          "# how often `BlockRandomPos` moved: the column is drawn before the weather"
          " check\n"
          "# (`rand.next` is the argument to `tick_precipitation`), so a clear-sky run"
          " still steps\n"
          "# it there — `sampledGates == lcgSteps` in every run, and `lcgSteps` is"
          " `samples` only\n"
          "# when it was actually raining. `samples` is the number of height queries,"
          " `rolls` the\n"
          "# cauldron chance draws and `fills` the writes; `rows` is the trace's length."
          " `logGates`\n"
          "# says whether every gate draw is in the trace or only the samples it let"
          " through\n"
          "# (see the run's own header in tools/gen_weather_dump.cpp).\n"
          "#\n"
          "# seed\trun\tscene\ttickSpeed\tticks\tseaLevel\trainLevel\tthunderLevel"
          "\trandStart\trandEnd\tgates\tsampledGates\tlcgSteps\tsamples\trolls\tfills"
          "\trows\tlogGates\n";
  for (size_t i = 0; i < runs.size(); ++i) {
    const RunResult& result = results[i];
    char line[512];
    std::snprintf(line, sizeof(line),
                  "%lld\t%s\t%s\t%d\t%d\t%d\t%.2f\t%.2f\t%d\t%d\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu"
                  "\t%zu\t%s\n",
                  static_cast<long long>(seed), runs[i].name, runs[i].scene, runs[i].tick_speed,
                  runs[i].ticks, kSeaLevel, static_cast<double>(runs[i].rain_level),
                  static_cast<double>(runs[i].thunder_level), result.rand_start, result.rand_end,
                  result.gates, result.sampled_gates, result.lcg_steps, result.samples,
                  result.rolls, result.fills, result.rows.size(),
                  runs[i].log_gates ? "true" : "false");
    text += line;
  }
  return text;
}

std::string scene_text() {
  std::string text = "# The world each recorded run starts from: everything not listed is air, a"
                     " column's\n"
                     "# motion-blocking height is its `height` row or the scene's `default_height`,"
                     " and a\n"
                     "# cell's biome is the scene's `default_biome` unless a `biome` row overrides"
                     " that one\n"
                     "# cell.\n"
                     "#\n"
                     "# kind\tscene\t...\n";
  for (const Scene& scene : kScenes) {
    char row[320];
    std::snprintf(row, sizeof(row), "default_biome\t%s\t%s\n", scene.name, scene.biome);
    text += row;
    std::snprintf(row, sizeof(row), "default_height\t%s\t%d\n", scene.name, scene.default_height);
    text += row;
    for (const Placement& placement : scene_blocks(scene.name)) {
      std::snprintf(row, sizeof(row), "block\t%s\t%d\t%d\t%d\t%s\n", scene.name, placement.x,
                    placement.y, placement.z,
                    canonical_state(BlockStateKey{placement.name, placement.properties}).c_str());
      text += row;
    }
    for (const HeightOverride& height : scene_heights(scene.name)) {
      std::snprintf(row, sizeof(row), "height\t%s\t%d\t%d\t%d\n", scene.name, height.x, height.z,
                    height.top);
      text += row;
    }
    for (const BiomeOverride& biome : scene_biomes(scene.name)) {
      std::snprintf(row, sizeof(row), "biome\t%s\t%d\t%d\t%d\t%s\n", scene.name, biome.x, biome.y,
                    biome.z, biome.name);
      text += row;
    }
  }
  return text;
}

std::string trace_text(int64_t seed, const RecordedRun& run, const RunResult& result) {
  std::string text;
  text += "# Phase 3 milestone H: the precipitation pass's interaction trace for run '";
  text += run.name;
  text += "' (scene '";
  text += run.scene;
  text += "'), seed ";
  text += std::to_string(seed);
  text += ".\n";
  text += "#\n"
          "# NOT a vanilla dump (see weather_meta.tsv): every row is one interaction"
          " the pass had\n"
          "# with the level or one draw it took, in order. `gate` is the pass's own"
          " 1-in-48 draw\n"
          "# (drawn whatever the weather is, and written only by the runs whose meta row"
          " says\n"
          "# `logGates`); `lcg` is the column draw's own step — `BlockRandomPos.value`"
          " right after\n"
          "# it, once per draw that passed the gate; `column` is the sampled column (x,"
          " z and the\n"
          "# same LCG value) and the height the pass read there, which it only asks when"
          " it is\n"
          "# raining; `biome` is the query at the column's first free cell, with the"
          " precipitation\n"
          "# it answers; `read` is the block the rain lands on, one cell below that;"
          " `roll` is a\n"
          "# cauldron's chance draw and `fill` the write. The precipitation label on a"
          " `biome`\n"
          "# row is `biome_precipitation_at`'s own answer, the same call the pass"
          " makes.\n"
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
  return dir + "/weather_seed" + std::to_string(seed) + "_" + run + ".tsv";
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

  std::vector<RecordedRun> runs(kRuns, kRuns + sizeof(kRuns) / sizeof(kRuns[0]));
  std::vector<RunResult> results;
  std::vector<std::string> violations;
  results.reserve(runs.size());
  for (const RecordedRun& run : runs) {
    results.push_back(run_scene(seed, run));
    const std::vector<std::string> here = invariant_violations(run, results.back());
    violations.insert(violations.end(), here.begin(), here.end());
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

  const std::string meta_path = dir + "/weather_meta.tsv";
  const std::string scene_path = dir + "/weather_scene.tsv";
  const std::string meta = meta_text(seed, runs, results);
  const std::string scene = scene_text();

  if (diff) {
    std::printf("weather fixture: %zu runs, seed %lld\n", runs.size(),
                static_cast<long long>(seed));
    bool changed = false;
    changed |= diff_text("weather_meta.tsv", meta_path, meta);
    changed |= diff_text("weather_scene.tsv", scene_path, scene);
    for (size_t i = 0; i < runs.size(); ++i) {
      const std::string name =
          std::string("weather_seed") + std::to_string(seed) + "_" + runs[i].name + ".tsv";
      changed |= diff_text(name, trace_path(dir, seed, runs[i].name),
                           trace_text(seed, runs[i], results[i]));
    }
    if (!changed) {
      std::printf("summary: in sync with this run's output\n");
      return 0;
    }
    std::printf("summary: the checked-in fixture is OUT OF SYNC\n");
    std::printf("re-run without --diff to rewrite it, then `ctest -R weather`\n");
    return 1;
  }

  std::FILE* meta_file = std::fopen(meta_path.c_str(), "w");
  if (meta_file == nullptr) {
    std::fprintf(stderr, "error: cannot write %s\n", meta_path.c_str());
    return 1;
  }
  std::fputs(meta.c_str(), meta_file);
  std::fclose(meta_file);
  std::FILE* scene_file = std::fopen(scene_path.c_str(), "w");
  if (scene_file == nullptr) {
    std::fprintf(stderr, "error: cannot write %s\n", scene_path.c_str());
    return 1;
  }
  std::fputs(scene.c_str(), scene_file);
  std::fclose(scene_file);
  std::printf("wrote %s (%zu runs)\n", meta_path.c_str(), runs.size());
  std::printf("wrote %s\n", scene_path.c_str());
  for (size_t i = 0; i < runs.size(); ++i) {
    const std::string path = trace_path(dir, seed, runs[i].name);
    std::FILE* trace = std::fopen(path.c_str(), "w");
    if (trace == nullptr) {
      std::fprintf(stderr, "error: cannot write %s\n", path.c_str());
      return 1;
    }
    std::fputs(trace_text(seed, runs[i], results[i]).c_str(), trace);
    std::fclose(trace);
    std::printf("wrote weather_seed%lld_%s.tsv (%zu rows: %zu gates, %zu lcg steps, %zu samples, "
                "%zu rolls, %zu fills)\n",
                static_cast<long long>(seed), runs[i].name, results[i].rows.size(),
                results[i].gates, results[i].lcg_steps, results[i].samples, results[i].rolls,
                results[i].fills);
  }
  return 0;
}
