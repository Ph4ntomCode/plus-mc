// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the weather (Phase 3 milestone H):
//   * the generated biome climate table: `Biome.getPrecipitationAt`'s three
//     fields, the 67 rows in sorted order (so the binary search is sound) and the
//     RAIN/SNOW/NONE answer for a warm, a cold and a precipitation-less biome;
//   * `Level.precipitationAt` / `isRainingAt`: the clear-sky gate (the column's
//     motion-blocking height), the biome that decides rain against snow, and the
//     "not raining, so nowhere" answer;
//   * `ServerLevel.advanceWeatherCycle`: the two delay/duration samples, the
//     countdown flips, the clear-weather countdown, the 0.01-a-tick rain and
//     thunder level ramp and the `0.2` / `0.9` thresholds that turn a level into
//     `isRaining` / `isThundering`, plus the three broadcasts the tick reports;
//   * `MinecraftServer.setWeatherParameters` (what `/weather` writes) and
//     `resetWeatherCycle`;
//   * `ServerLevel.tickPrecipitation` and the cauldron's `handlePrecipitation`:
//     the 0.05 rain / 0.1 snow chances, an empty cauldron becoming a full one, a
//     layered one gaining a level, the FULL stop, the kind mismatch, and the fact
//     that a block with no handler takes no draw at all;
//   * `ServerLevel.tickChunk`'s sample: the one-in-48 gate, `Level
//     .getBlockRandomPos`'s LCG (whose values are pinned exactly) and the column
//     it lands on. Every draw is scripted, so each branch is pinned;
//   * the golden trace: the recorded scenes in `tests/fixtures/weather` are
//     replayed through the real `tick_chunk_precipitation` with the level's own
//     random streams, and every interaction the pass has with the level is
//     compared against the checked-in trace, row for row. Scripted draws pin each
//     branch; only a real stream pins the sequence they arrive in.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "world/biome_table.h"
#include "world/block_states.h"
#include "world/chunk.h"
#include "world/collision_table.h"
#include "world/weather.h"
#include "world/worldgen_random.h" // WgLegacyRandom: the level's own streams

using namespace mc::world;

static int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECK_EQ(a, b)                                                                             \
  do {                                                                                             \
    const long long va = static_cast<long long>(a), vb = static_cast<long long>(b);                \
    if (va != vb) {                                                                                \
      std::fprintf(stderr, "FAIL %s:%d: %s == %lld, want %lld\n", __FILE__, __LINE__, #a, va, vb); \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECK_STR(a, b)                                                                            \
  do {                                                                                             \
    const std::string va = (a), vb = (b);                                                          \
    if (va != vb) {                                                                                \
      std::fprintf(stderr, "FAIL %s:%d: %s == \"%s\", want \"%s\"\n", __FILE__, __LINE__, #a,      \
                   va.c_str(), vb.c_str());                                                        \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

namespace {

// A scripted `WeatherRandom`: the int draws come from `ints` (then `int_tail`)
// and the float draws from `floats` (then `float_tail`), each clamped where the
// real draw would be. Both call counts are recorded, so a test can prove a draw
// was *not* taken.
struct ScriptedRandom {
  std::vector<int> ints;
  int int_tail = 0;
  std::vector<float> floats;
  float float_tail = 0.0f;
  std::shared_ptr<size_t> int_calls = std::make_shared<size_t>(0);
  std::shared_ptr<size_t> float_calls = std::make_shared<size_t>(0);

  WeatherRandom make() {
    WeatherRandom rng;
    auto ints_copy = std::make_shared<const std::vector<int>>(ints);
    auto floats_copy = std::make_shared<const std::vector<float>>(floats);
    rng.next_int = [ints_copy, this, calls = int_calls](int bound) {
      const size_t call = (*calls)++;
      const int raw = call < ints_copy->size() ? (*ints_copy)[call] : int_tail;
      return bound > 0 ? std::min(raw, bound - 1) : 0;
    };
    rng.next_float = [floats_copy, this, calls = float_calls]() {
      const size_t call = (*calls)++;
      return call < floats_copy->size() ? (*floats_copy)[call] : float_tail;
    };
    return rng;
  }
};

// `Biome.getPrecipitationAt` through the generated table.
void test_biome_table() {
  CHECK_EQ(std::size(kBiomeClimate), 67u);
  // Sorted by name, which is what makes the binary search valid.
  for (size_t i = 1; i < std::size(kBiomeClimate); ++i)
    CHECK(std::string(kBiomeClimate[i - 1].name) < std::string(kBiomeClimate[i].name));

  // The three answers. plains is the world this server's own tests run on
  // (0.8, warm enough to rain); frozen_ocean carries the `frozen` modifier and a
  // base temperature of 0.0, which is below the 0.15 snow line; the_void and the
  // desert have no precipitation at all — the_void is what makes the default
  // in-memory world weather-proof, and it is vanilla's own answer for it.
  CHECK(biome_has_precipitation("minecraft:plains"));
  CHECK_EQ(static_cast<int>(biome_precipitation_at("minecraft:plains", 64, 63)),
           static_cast<int>(Precipitation::kRain));
  CHECK_EQ(static_cast<int>(biome_precipitation_at("minecraft:frozen_ocean", 64, 63)),
           static_cast<int>(Precipitation::kSnow));
  CHECK_EQ(static_cast<int>(biome_precipitation_at("minecraft:frozen_ocean", 200, 63)),
           static_cast<int>(Precipitation::kSnow));
  CHECK(!biome_has_precipitation("minecraft:the_void"));
  CHECK_EQ(static_cast<int>(biome_precipitation_at("minecraft:the_void", 64, 63)),
           static_cast<int>(Precipitation::kNone));
  CHECK(!biome_has_precipitation("minecraft:desert"));
  CHECK_EQ(static_cast<int>(biome_precipitation_at("minecraft:desert", 64, 63)),
           static_cast<int>(Precipitation::kNone));
  CHECK_EQ(static_cast<int>(biome_precipitation_at("minecraft:nether_wastes", 64, 63)),
           static_cast<int>(Precipitation::kNone));
  // An unknown name (a biome this server has never heard of) answers NONE.
  CHECK_EQ(static_cast<int>(biome_precipitation_at("example:unknown", 64, 63)),
           static_cast<int>(Precipitation::kNone));
  CHECK_EQ(static_cast<int>(cauldron_precipitation_type("minecraft:water_cauldron")),
           static_cast<int>(Precipitation::kRain));
  CHECK_EQ(static_cast<int>(cauldron_precipitation_type("minecraft:powder_snow_cauldron")),
           static_cast<int>(Precipitation::kSnow));
  CHECK_EQ(static_cast<int>(cauldron_precipitation_type("minecraft:cauldron")),
           static_cast<int>(Precipitation::kNone));
}

// `Level.getBlockRandomPos`'s LCG: `randValue = randValue * 3 + 1013904223`,
// then three masked slices of `randValue >> 2`. The values below are that
// sequence from a zero seed, so a change in the arithmetic is caught exactly.
void test_block_random_pos() {
  BlockRandomPos rand;
  int x = 0, y = 0, z = 0;
  rand.next(0, 0, 0, 15, &x, &y, &z);
  CHECK_EQ(rand.value, 1013904223);
  CHECK_EQ(x, 7);  // 253476055 & 0xF
  CHECK_EQ(y, 11); // (253476055 >> 16) & 15
  CHECK_EQ(z, 12); // (253476055 >> 8) & 0xF
  rand.next(16, -64, 32, 15, &x, &y, &z);
  CHECK_EQ(rand.value, -239350404);
  CHECK_EQ(x, 16 + 15);
  CHECK_EQ(y, -64 + 14);
  CHECK_EQ(z, 32 + 3);
  rand.next(0, 0, 0, 15, &x, &y, &z);
  CHECK_EQ(rand.value, 295853011);
  CHECK_EQ(x, 4);
  CHECK_EQ(y, 8);
  CHECK_EQ(z, 6);
}

// `ServerLevel.advanceWeatherCycle`. The two countdowns sample their
// delay/duration distribution when they run out, and a level only becomes
// `isRaining` once the ramp has carried it past 0.2 — twenty ticks after the
// flip, which is the second a client's rain fade takes.
void test_weather_cycle() {
  // A clear world: the rain countdown is 0, so the tick samples RAIN_DELAY
  // (12000..180000) and nothing else happens.
  {
    Weather weather;
    ScriptedRandom rng;
    rng.int_tail = 0;
    const Weather::Advance change = weather.advance(rng.make());
    CHECK_EQ(weather.rain_time, kRainDelayMin + 0);
    CHECK_EQ(weather.thunder_time, kThunderDelayMin + 0);
    CHECK(!weather.raining);
    CHECK(!weather.is_raining());
    CHECK(!change.rain_started);
    CHECK(!change.rain_stopped);
    CHECK(!change.rain_level_changed); // 0 stays 0
    CHECK(!change.thunder_level_changed);
  }

  // `/weather rain 100`: the ramp climbs 0.01 a tick, so the level passes 0.2
  // after about twenty ticks — and the tick it crosses on is the one the flip is
  // reported on. (Twenty steps of 0.01f land on 0.20000002f, so it is the
  // twentieth here; the threshold is a strict `>`, which is why 0.2 exactly
  // would not do it.)
  {
    Weather weather;
    weather.set_parameters(/*clear_time=*/0, /*weather_time=*/100, /*raining=*/true,
                           /*thundering=*/false);
    ScriptedRandom rng;
    const WeatherRandom source = rng.make();
    int started_on = -1;
    for (int tick = 1; tick <= 100 && started_on < 0; ++tick) {
      const bool was_raining = weather.is_raining();
      const Weather::Advance change = weather.advance(source);
      // The flip is exactly "it was not raining and now it is" — the report and
      // the level can never disagree.
      CHECK_EQ(change.rain_started, !was_raining && weather.is_raining());
      if (change.rain_started)
        started_on = tick;
      if (tick <= 19) {
        CHECK(change.rain_level_changed); // the ramp moves every tick
        CHECK(!change.rain_started);
        CHECK(!weather.is_raining());
        CHECK(std::abs(weather.rain_level - static_cast<float>(tick) * 0.01f) < 1e-6f);
      }
    }
    CHECK(started_on == 20 || started_on == 21);
    CHECK(weather.is_raining());

    // ...and the countdown runs out a hundred ticks in, flipping `raining` off.
    // The level takes its own fall back under the threshold, and *that* is when
    // the stop is reported — a second later, which is the fade a client renders.
    int stopped_on = -1;
    for (int tick = 1; tick <= 200 && stopped_on < 0; ++tick)
      if (weather.advance(source).rain_stopped)
        stopped_on = tick;
    CHECK(stopped_on > 0);
    CHECK(!weather.is_raining());
  }

  // Thunder: `setThunderTime(rainTime)` is the rain duration, and the thunder
  // level needs a bigger ramp to cross 0.9 than rain needs for 0.2.
  {
    Weather weather;
    weather.set_parameters(0, 600, true, true);
    // `setThunderTime(rainTime)`: the thunder countdown is fed the *rain*
    // duration, which is vanilla's own argument reuse.
    CHECK_EQ(weather.thunder_time, 600);
    ScriptedRandom rng;
    const WeatherRandom source = rng.make();
    for (int tick = 0; tick < 90; ++tick) {
      weather.advance(source);
      CHECK(!weather.is_thundering()); // 0.90 is not past the threshold
    }
    int thundered_on = -1;
    for (int tick = 91; tick <= 120 && thundered_on < 0; ++tick) {
      weather.advance(source);
      if (weather.thunder_level > kThunderLevelThreshold)
        thundered_on = tick;
    }
    CHECK(thundered_on >= 91);
    CHECK(weather.is_thundering());
  }

  // The clear branch: `clear_weather_time` counts down, both flags go false, and
  // the two countdowns are rewritten to 0/1 according to the flag they were
  // carrying (vanilla's own arithmetic, and the reason `/weather clear` takes
  // effect without a rain spell in between).
  {
    Weather weather;
    weather.set_parameters(/*clear_time=*/2, /*weather_time=*/0, true, true);
    ScriptedRandom rng;
    const WeatherRandom source = rng.make();
    weather.advance(source);
    CHECK_EQ(weather.clear_weather_time, 1);
    CHECK_EQ(weather.thunder_time, 0);
    CHECK_EQ(weather.rain_time, 0);
    const Weather::Advance change = weather.advance(source);
    CHECK_EQ(weather.clear_weather_time, 0);
    CHECK(!weather.raining);
    CHECK(!weather.thundering);
    CHECK_EQ(weather.thunder_time, 1);
    CHECK_EQ(weather.rain_time, 1);
    // Both levels were still at zero and the flags are now false, so the ramp
    // keeps them clamped there and there is nothing to send: a clear sky that was
    // never cloudy costs no packets.
    CHECK(!change.rain_level_changed);
    CHECK(!change.thunder_level_changed);
    CHECK(!change.rain_stopped);
    CHECK(!change.rain_started);
  }

  // `resetWeatherCycle` clears both flags and countdowns; the levels fall back on
  // their own.
  {
    Weather weather;
    weather.set_parameters(0, 600, true, true);
    weather.rain_level = 1.0f;
    weather.thunder_level = 1.0f;
    weather.reset_cycle();
    CHECK(!weather.raining);
    CHECK(!weather.thundering);
    CHECK_EQ(weather.rain_time, 0);
    CHECK_EQ(weather.thunder_time, 0);
    CHECK_EQ(weather.rain_level, 1.0f);
  }
}

// A `PrecipitationQuery` over a fixed column height and biome.
PrecipitationQuery query_for(int height, std::string biome) {
  PrecipitationQuery query;
  query.motion_blocking_height = [height](int, int) { return height; };
  query.biome_at = [biome](int, int, int) { return biome; };
  return query;
}

// `BlockState.blocksMotion()` (world/collision_table.h) is the predicate the
// motion-blocking heightmap is built from, and the heightmap is what decides
// whether rain reaches a cell at all. A fire at the top of its own column is the
// case the whole milestone turns on: the fire is not a roof over its own cell, so
// the column's height is one past the block *below* it and rain reaches the fire
// — which is the branch `FireBlock.tick`'s `isNearRain` and a burning entity's
// `isInRain` hang on. With the "any non-air blocks motion" approximation this
// table replaces, a fire would have been its own roof and the branch was dead.
void test_motion_blocking() {
  // Sorted, so the binary search in `block_blocks_motion` is sound, and every
  // entry is a block the project's own registry carries (both are generated from
  // the same jar, so a name in one and not the other is a stale table).
  std::unordered_set<std::string> registry;
  for (int i = 0; i < kBlockStateCount; ++i)
    registry.insert(kBlockStates[i].name);
  for (int i = 0; i < kNonMotionBlockingCount; ++i) {
    if (i > 0)
      CHECK(std::string(kNonMotionBlocking[i - 1]) < std::string(kNonMotionBlocking[i]));
    CHECK(registry.count(kNonMotionBlocking[i]) == 1);
  }

  // The shapes that must NOT block: fire is the one this milestone needs, and
  // the rest are the forms a player can stand in or light a fire on — grass,
  // torches, saplings, snow, rails and wires, ladders, cobwebs (which
  // `blocksMotion` excludes by name), and the two fluids (which the *heightmap's*
  // fluid half, not this table, makes motion-blocking).
  for (const char* name :
       {"minecraft:air", "minecraft:cave_air", "minecraft:void_air", "minecraft:fire",
        "minecraft:soul_fire", "minecraft:torch", "minecraft:wall_torch", "minecraft:cobweb",
        "minecraft:bamboo_sapling", "minecraft:short_grass", "minecraft:tall_grass",
        "minecraft:wildflowers", "minecraft:oak_sapling", "minecraft:dandelion", "minecraft:snow",
        "minecraft:ladder", "minecraft:redstone_wire", "minecraft:water", "minecraft:lava"}) {
    CHECK(!block_blocks_motion(name));
  }
  // And the ones that must: the ground a fire sits on, the block it is built
  // against, and the cauldron whose own cell the precipitation pass samples.
  for (const char* name : {"minecraft:stone", "minecraft:glass", "minecraft:oak_planks",
                           "minecraft:hay_block", "minecraft:oak_stairs", "minecraft:white_wool",
                           "minecraft:cauldron", "minecraft:water_cauldron", "minecraft:oak_log"}) {
    CHECK(block_blocks_motion(name));
  }

  // The chunk half: stone at 64 with a fire at 65 makes the column's
  // motion-blocking height 65 (one past the stone), so the fire's own cell is at
  // the height and the stone's cell is under it.
  Chunk chunk;
  chunk.min_section_y = 4; // y 64..79
  chunk.sections.assign(1, Section{});
  chunk.sections[0].y = 4;
  CHECK(chunk.set_block_state(0, 64, 0, BlockStateKey{"minecraft:stone", ""}));
  CHECK(chunk.set_block_state(0, 65, 0,
                              BlockStateKey{"minecraft:fire",
                                            "age=0,east=false,north=false,south=false,up=false,"
                                            "west=false"}));
  CHECK_EQ(chunk_motion_blocking_height(chunk, 0, 0), 65);
  CHECK_EQ(chunk_motion_blocking_height(chunk, 1, 0), 64); // nothing there at all
  // Stone with a torch on it: the torch is no more a roof than the fire is, so
  // the column tops out one past the *stone*.
  CHECK(chunk.set_block_state(1, 64, 0, BlockStateKey{"minecraft:stone", ""}));
  CHECK(chunk.set_block_state(1, 65, 0, BlockStateKey{"minecraft:torch", ""}));
  CHECK_EQ(chunk_motion_blocking_height(chunk, 1, 0), 65);
  // A stair on the stone does block, so that column tops out one higher.
  CHECK(chunk.set_block_state(2, 65, 0,
                              BlockStateKey{"minecraft:oak_stairs",
                                            "facing=north,half=bottom,shape=straight,"
                                            "waterlogged=false"}));
  CHECK_EQ(chunk_motion_blocking_height(chunk, 2, 0), 66);

  // The *other* heightmap question, which natural spawning draws its attempt's y
  // from: WORLD_SURFACE counts any non-air block, so the fire's and the torch's
  // own cells are the tops of their columns where the motion-blocking map looked
  // past them. Same chunk, same two cells, two different answers above them.
  CHECK_EQ(chunk_world_surface_height(chunk, 0, 0), 66); // stone 64 + fire 65
  CHECK_EQ(chunk_world_surface_height(chunk, 1, 0), 66); // stone 64 + torch 65
  CHECK_EQ(chunk_world_surface_height(chunk, 2, 0), 66); // the stair itself at 65
  CHECK_EQ(chunk_world_surface_height(chunk, 3, 0), 64); // nothing there at all

  // And the weather's side of the same question, with the chunk's real heights:
  // in a warm biome, in the rain, the fire's cell precipitates and the stone
  // under it does not.
  Weather weather;
  weather.set_parameters(0, 6000, true, false);
  for (int i = 0; i < 21; ++i) {
    ScriptedRandom rng;
    weather.advance(rng.make());
  }
  CHECK(weather.is_raining());
  PrecipitationQuery query;
  query.motion_blocking_height = [&chunk](int x, int z) {
    return chunk_motion_blocking_height(chunk, x, z);
  };
  query.biome_at = [](int, int, int) { return std::string("minecraft:plains"); };
  CHECK(is_raining_at(weather, query, 63, 0, 65, 0));
  CHECK(!is_raining_at(weather, query, 63, 0, 64, 0));
  CHECK(is_raining_at(weather, query, 63, 1, 65, 0));
}

// `Level.precipitationAt`: the three gates, in vanilla's order.
void test_precipitation_at() {
  Weather weather;
  const PrecipitationQuery plains = query_for(66, "minecraft:plains");

  // Not raining: nowhere precipitates, whatever the biome.
  CHECK_EQ(static_cast<int>(precipitation_at(weather, plains, 63, 0, 66, 0)),
           static_cast<int>(Precipitation::kNone));

  weather.set_parameters(0, 6000, true, false);
  for (int i = 0; i < 21; ++i) {
    ScriptedRandom rng;
    weather.advance(rng.make());
  }
  CHECK(weather.is_raining());

  // The top of the column is 66, so 66 is clear to the sky and 65 is under the
  // block the heightmap counts.
  CHECK_EQ(static_cast<int>(precipitation_at(weather, plains, 63, 0, 66, 0)),
           static_cast<int>(Precipitation::kRain));
  CHECK_EQ(static_cast<int>(precipitation_at(weather, plains, 63, 0, 100, 0)),
           static_cast<int>(Precipitation::kRain));
  CHECK_EQ(static_cast<int>(precipitation_at(weather, plains, 63, 0, 65, 0)),
           static_cast<int>(Precipitation::kNone));
  CHECK(is_raining_at(weather, plains, 63, 0, 66, 0));
  CHECK(!is_raining_at(weather, plains, 63, 0, 65, 0));

  // A snowy biome precipitates, but not as rain: `isRainingAt` is RAIN-only.
  const PrecipitationQuery snowy = query_for(66, "minecraft:snowy_plains");
  CHECK_EQ(static_cast<int>(precipitation_at(weather, snowy, 63, 0, 66, 0)),
           static_cast<int>(Precipitation::kSnow));
  CHECK(!is_raining_at(weather, snowy, 63, 0, 66, 0));

  // A biome with no precipitation at all — the default in-memory world.
  const PrecipitationQuery void_world = query_for(66, "minecraft:the_void");
  CHECK_EQ(static_cast<int>(precipitation_at(weather, void_world, 63, 0, 66, 0)),
           static_cast<int>(Precipitation::kNone));
  CHECK(!is_raining_at(weather, void_world, 63, 0, 66, 0));
}

// A `PrecipitationLevel` over a sparse cell map, recording every write.
struct TestLevel {
  std::vector<std::pair<std::array<int, 3>, BlockStateKey>> cells;
  std::vector<BlockStateKey> writes;
  int height = 66;
  std::string biome = "minecraft:plains";

  void put(const char* name, const char* props, int x, int y, int z) {
    cells.push_back({{x, y, z}, BlockStateKey{name, props}});
  }
  BlockStateKey at(int x, int y, int z) const {
    for (const auto& cell : cells)
      if (cell.first[0] == x && cell.first[1] == y && cell.first[2] == z)
        return cell.second;
    return BlockStateKey{"minecraft:air", ""};
  }

  PrecipitationLevel make() {
    PrecipitationLevel level;
    level.get_block = [this](int x, int y, int z) { return at(x, y, z); };
    level.set_block = [this](int, int, int, const BlockStateKey& state) {
      writes.push_back(state);
    };
    level.motion_blocking_height = [this](int, int) { return height; };
    level.biome_at = [this](int, int, int) { return biome; };
    return level;
  }
};

// `ServerLevel.tickPrecipitation`: the block the column's rain lands on (one
// *below* the heightmap) is asked for the biome's precipitation there and then
// for `handlePrecipitation`.
void test_cauldron_precipitation() {
  // An empty cauldron: a 0.049 roll fills it with water (`WATER_CAULDRON`
  // default state). At exactly 0.05 the chance misses, which is the boundary
  // `nextFloat() < 0.05f` draws.
  for (int pass = 0; pass < 2; ++pass) {
    TestLevel world;
    world.put("minecraft:cauldron", "", 0, 65, 0);
    world.height = 66;
    PrecipitationLevel level = world.make();
    ScriptedRandom rng;
    rng.float_tail = pass == 0 ? 0.049f : 0.05f;
    Weather weather;
    weather.set_parameters(0, 6000, true, false);
    for (int i = 0; i < 21; ++i) {
      ScriptedRandom tick;
      weather.advance(tick.make());
    }
    tick_precipitation(level, rng.make(), weather, 63, 0, 0);
    CHECK_EQ(*rng.float_calls, 1u);
    if (pass == 0) {
      CHECK_EQ(world.writes.size(), 1u);
      CHECK_STR(world.writes[0].name, "minecraft:water_cauldron");
      CHECK_STR(world.writes[0].properties, "level=1");
    } else {
      CHECK(world.writes.empty());
    }
  }

  // A layered water cauldron gains a level; a FULL one stops (and still draws:
  // vanilla's guard rolls the chance first).
  for (int level_value = 1; level_value <= 3; ++level_value) {
    TestLevel world;
    world.put("minecraft:water_cauldron",
              level_value == 1   ? "level=1"
              : level_value == 2 ? "level=2"
                                 : "level=3",
              0, 65, 0);
    PrecipitationLevel level = world.make();
    ScriptedRandom rng;
    rng.float_tail = 0.0f;
    Weather weather;
    weather.set_parameters(0, 6000, true, false);
    for (int i = 0; i < 21; ++i) {
      ScriptedRandom tick;
      weather.advance(tick.make());
    }
    tick_precipitation(level, rng.make(), weather, 63, 0, 0);
    CHECK_EQ(*rng.float_calls, 1u);
    if (level_value == 3) {
      CHECK(world.writes.empty());
    } else {
      CHECK_EQ(world.writes.size(), 1u);
      CHECK_STR(world.writes[0].name, "minecraft:water_cauldron");
      CHECK_STR(world.writes[0].properties, "level=" + std::to_string(level_value + 1));
    }
  }

  // The kind has to match: a snow cauldron ignores rain, and a water cauldron
  // ignores snow. The cold biome is what makes the second half possible.
  {
    TestLevel world;
    world.put("minecraft:powder_snow_cauldron", "level=1", 0, 65, 0);
    PrecipitationLevel level = world.make();
    ScriptedRandom rng;
    Weather weather;
    weather.set_parameters(0, 6000, true, false);
    for (int i = 0; i < 21; ++i) {
      ScriptedRandom tick;
      weather.advance(tick.make());
    }
    tick_precipitation(level, rng.make(), weather, 63, 0, 0);
    CHECK(world.writes.empty()); // RAIN into a powder-snow cauldron
  }
  {
    TestLevel world;
    world.put("minecraft:water_cauldron", "level=2", 0, 65, 0);
    world.biome = "minecraft:snowy_plains";
    PrecipitationLevel level = world.make();
    ScriptedRandom rng;
    Weather weather;
    weather.set_parameters(0, 6000, true, false);
    for (int i = 0; i < 21; ++i) {
      ScriptedRandom tick;
      weather.advance(tick.make());
    }
    tick_precipitation(level, rng.make(), weather, 63, 0, 0);
    CHECK(world.writes.empty()); // SNOW into a water cauldron
  }

  // A block with no precipitation handler at all takes no draw — this is the
  // mass of the world, and the reason the chance is only ever reached through a
  // cauldron.
  {
    TestLevel world;
    world.put("minecraft:stone", "", 0, 65, 0);
    PrecipitationLevel level = world.make();
    ScriptedRandom rng;
    Weather weather;
    weather.set_parameters(0, 6000, true, false);
    for (int i = 0; i < 21; ++i) {
      ScriptedRandom tick;
      weather.advance(tick.make());
    }
    tick_precipitation(level, rng.make(), weather, 63, 0, 0);
    CHECK(world.writes.empty());
    CHECK_EQ(*rng.float_calls, 0u);
  }
  // ...and a biome that does not precipitate is never asked for one either.
  {
    TestLevel world;
    world.put("minecraft:cauldron", "", 0, 65, 0);
    world.biome = "minecraft:the_void";
    PrecipitationLevel level = world.make();
    ScriptedRandom rng;
    Weather weather;
    weather.set_parameters(0, 6000, true, false);
    for (int i = 0; i < 21; ++i) {
      ScriptedRandom tick;
      weather.advance(tick.make());
    }
    tick_precipitation(level, rng.make(), weather, 63, 0, 0);
    CHECK(world.writes.empty());
    CHECK_EQ(*rng.float_calls, 0u);
  }
  // With the sky clear, `tickPrecipitation`'s whole RAIN half is skipped.
  {
    TestLevel world;
    world.put("minecraft:cauldron", "", 0, 65, 0);
    PrecipitationLevel level = world.make();
    ScriptedRandom rng;
    Weather weather;
    tick_precipitation(level, rng.make(), weather, 63, 0, 0);
    CHECK(world.writes.empty());
    CHECK_EQ(*rng.float_calls, 0u);
  }
}

// The chunk's own height, sampled by the precipitation pass: the block the rain
// lands on is the one below the height, and a cauldron standing in the fire's own
// cell is what `handlePrecipitation` is then asked about — which is how a
// cauldron at the top of its column fills at all, and the other half of this
// milestone that the "any non-air blocks motion" predicate used to make
// unreachable.
void test_fire_column_precipitation() {
  Chunk chunk;
  chunk.min_section_y = 4;
  chunk.sections.assign(1, Section{});
  chunk.sections[0].y = 4;
  CHECK(chunk.set_block_state(0, 64, 0, BlockStateKey{"minecraft:stone", ""}));
  CHECK(chunk.set_block_state(0, 65, 0, BlockStateKey{"minecraft:cauldron", ""}));
  const int height = chunk_motion_blocking_height(chunk, 0, 0);
  CHECK_EQ(height, 66); // one past the cauldron, which does block motion

  Weather weather;
  weather.set_parameters(0, 6000, true, false);
  for (int i = 0; i < 21; ++i) {
    ScriptedRandom rng;
    weather.advance(rng.make());
  }
  TestLevel world;
  world.put("minecraft:cauldron", "", 0, 65, 0);
  world.height = height;
  PrecipitationLevel level = world.make();
  ScriptedRandom rng;
  rng.float_tail = 0.0f;
  tick_precipitation(level, rng.make(), weather, 63, 0, 0);
  CHECK_EQ(world.writes.size(), 1u);
  CHECK_STR(world.writes[0].name, "minecraft:water_cauldron");
  CHECK_STR(world.writes[0].properties, "level=1");
  // The fire's column, one block over and one block taller: the sample lands on
  // the stone under the fire, which has no precipitation handler and draws
  // nothing.
  world.writes.clear();
  world.cells.clear();
  world.put("minecraft:stone", "", 1, 64, 0);
  world.height = 65;
  *rng.float_calls = 0;
  rng.float_tail = 0.0f;
  tick_precipitation(level, rng.make(), weather, 63, 1, 0);
  CHECK(world.writes.empty());
  CHECK_EQ(*rng.float_calls, 0u);
}

// `ServerLevel.tickChunk`'s precipitation half: `randomTickSpeed` draws, one in
// 48 of which samples a random column of the chunk. From a zero-seeded LCG the
// first sampled column is (7, _, 12) — see test_block_random_pos — so that is
// where the cauldron has to stand for a sample to reach it.
void test_chunk_sampling() {
  const auto run = [](int tick_speed, int cauldron_x, int cauldron_z, std::vector<int> ints,
                      int int_tail) {
    TestLevel world;
    world.put("minecraft:cauldron", "", cauldron_x, 65, cauldron_z);
    // Every column tops out at 66, so the heightmap decides where a sampled
    // column's rain lands (one below it), for every column of the chunk.
    world.height = 66;
    PrecipitationLevel level = world.make();
    Weather weather;
    weather.set_parameters(0, 6000, true, false);
    for (int i = 0; i < 21; ++i) {
      ScriptedRandom tick;
      weather.advance(tick.make());
    }
    ScriptedRandom rng;
    rng.ints = std::move(ints);
    rng.int_tail = int_tail;
    rng.float_tail = 0.0f; // the cauldron's 0.05 chance always passes
    BlockRandomPos rand;
    tick_chunk_precipitation(level, rng.make(), weather, rand, 63, tick_speed, 0, 0);
    return world.writes.size();
  };

  // `randomTickSpeed` 0: not a draw.
  CHECK_EQ(run(0, 7, 12, {}, 0), 0u);
  // `tick_speed` draws, all gated off (`nextInt(48) != 0`): the chunk is never
  // sampled, so the cauldron standing where a sample would have landed is
  // untouched — and the LCG never advanced either.
  CHECK_EQ(run(3, 7, 12, {1, 2, 3}, 0), 0u);
  CHECK_EQ(run(1, 7, 12, {47}, 47), 0u);
  // The gate passes on the first draw and the column is (7, 12): filled.
  CHECK_EQ(run(1, 7, 12, {0}, 0), 1u);
  // The same draw with the cauldron in another column of the chunk: nothing.
  CHECK_EQ(run(1, 3, 4, {0}, 0), 0u);
  // A gated-off draw consumes the *gate* draw but not an LCG step, so two draws
  // where only the second passes still sample the first column of the sequence.
  CHECK_EQ(run(2, 7, 12, {47, 0}, 0), 1u);
  // With the cauldron where the *second* LCG step lands ((15, _, 3), from
  // test_block_random_pos): the first sample misses, so it takes two gated draws.
  CHECK_EQ(run(1, 15, 3, {0}, 0), 0u);
  CHECK_EQ(run(2, 15, 3, {0, 0}, 0), 1u);
}

// ---- the golden trace -------------------------------------------------------

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

std::string canonical_state(const BlockStateKey& key) {
  std::string text = key.name;
  if (!key.properties.empty()) {
    text += '[';
    text += key.properties;
    text += ']';
  }
  return text;
}

// The `name[properties]` form the scene file writes, back to a state.
BlockStateKey parse_state(const std::string& text) {
  const size_t open = text.find('[');
  if (open == std::string::npos)
    return BlockStateKey{text, ""};
  return BlockStateKey{text.substr(0, open), text.substr(open + 1, text.size() - open - 2)};
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

struct RecordedRun {
  int64_t seed = 42;
  std::string name;
  std::string scene;
  int tick_speed = 0;
  int ticks = 0;
  int sea_level = 63;
  float rain_level = 0.0f;
  float thunder_level = 0.0f;
  int32_t rand_start = 0;
  int32_t rand_end = 0;
  size_t gates = 0;
  size_t sampled_gates = 0;
  size_t lcg_steps = 0;
  size_t samples = 0;
  size_t rolls = 0;
  size_t fills = 0;
  size_t rows = 0;
  bool log_gates = false;
};

struct RecordedScene {
  std::string name;
  std::string default_biome;
  int default_height = 0;
  std::vector<std::tuple<int, int, int, BlockStateKey>> blocks; // x, y, z, state
  std::vector<std::tuple<int, int, int>> heights;               // x, z, top
  std::vector<std::tuple<int, int, int, std::string>> biomes;   // x, y, z, name
};

struct PrecipitationReplay {
  std::vector<std::string> rows;
  int32_t rand_start = 0;
  int32_t rand_end = 0;
  size_t gates = 0;
  size_t sampled_gates = 0;
  size_t lcg_steps = 0;
  size_t samples = 0;
  size_t rolls = 0;
  size_t fills = 0;
};

// Replays one recorded run against the real pass, with the level's two streams
// seeded the way the generator seeded them, and formats the log the way the
// fixture does. The call is one draw per `tick_chunk_precipitation` call so that
// the column LCG's steps can be counted from outside: one call is one gate draw,
// and `BlockRandomPos` is the test's own, so a step is visible between calls.
PrecipitationReplay replay_precipitation(const RecordedRun& run, const RecordedScene& scene) {
  PrecipitationReplay replay;
  Weather weather;
  weather.rain_level = run.rain_level;
  weather.previous_rain_level = run.rain_level;
  weather.thunder_level = run.thunder_level;
  weather.previous_thunder_level = run.thunder_level;
  weather.raining = run.rain_level > kRainLevelThreshold;
  weather.thundering = run.thunder_level > kThunderLevelThreshold;

  mc::world::WgLegacyRandom level_random(run.seed);
  BlockRandomPos rand;
  rand.value = level_random.next_int();
  replay.rand_start = rand.value;

  mc::world::WgLegacyRandom randomness(run.seed);
  WeatherRandom pass_rng;

  int tick = 0;
  auto push = [&replay](std::string row) { replay.rows.push_back(std::move(row)); };

  pass_rng.next_int = [&randomness, &replay, &tick, &push, &run](int bound) {
    const int value = randomness.next_int(bound);
    if (bound == kPrecipitationSampleGate) {
      ++replay.gates;
      if (value == 0)
        ++replay.sampled_gates;
      if (run.log_gates) {
        char text[128];
        std::snprintf(text, sizeof(text), "%d\t%d\t%s", tick, value,
                      value == 0 ? "sampled" : "skipped");
        push(std::string("gate\t") + text);
      }
    }
    return value;
  };
  pass_rng.next_float = [&randomness, &replay, &tick, &push]() {
    const float value = randomness.next_float();
    ++replay.rolls;
    char text[64];
    std::snprintf(text, sizeof(text), "%d\t%.6f", tick, static_cast<double>(value));
    push(std::string("roll\t") + text);
    return value;
  };

  PrecipitationLevel level;
  level.get_block = [&scene, &replay, &tick, &push](int x, int y, int z) {
    BlockStateKey block{"minecraft:air", ""};
    for (const auto& cell : scene.blocks)
      if (std::get<0>(cell) == x && std::get<1>(cell) == y && std::get<2>(cell) == z)
        block = std::get<3>(cell);
    char text[320];
    std::snprintf(text, sizeof(text), "%d\t%d\t%d\t%d\t%s", tick, x, y, z,
                  canonical_state(block).c_str());
    push(std::string("read\t") + text);
    return block;
  };
  level.set_block = [&replay, &tick, &push](int x, int y, int z, const BlockStateKey& block) {
    ++replay.fills;
    char text[320];
    std::snprintf(text, sizeof(text), "%d\t%d\t%d\t%d\t%s", tick, x, y, z,
                  canonical_state(block).c_str());
    push(std::string("fill\t") + text);
  };
  level.motion_blocking_height = [&scene, &replay, &rand, &tick, &push](int x, int z) {
    int top = scene.default_height;
    for (const auto& height : scene.heights)
      if (std::get<0>(height) == x && std::get<1>(height) == z)
        top = std::get<2>(height);
    ++replay.samples;
    char text[256];
    std::snprintf(text, sizeof(text), "%d\t%d\t%d\t%d", tick, x, z, rand.value);
    push(std::string("column\t") + text);
    return top;
  };
  level.biome_at = [&scene, &replay, &tick, &push](int x, int y, int z) {
    std::string name = scene.default_biome;
    for (const auto& biome : scene.biomes)
      if (std::get<0>(biome) == x && std::get<1>(biome) == y && std::get<2>(biome) == z)
        name = std::get<3>(biome);
    char text[320];
    std::snprintf(text, sizeof(text), "%d\t%d\t%d\t%d\t%s\t%s", tick, x, y, z, name.c_str(),
                  precipitation_name(biome_precipitation_at(name, y, kOverworldSeaLevel)));
    push(std::string("biome\t") + text);
    return name;
  };

  const int draws = run.tick_speed > 0 ? run.tick_speed : 1;
  const int speed = run.tick_speed > 0 ? 1 : 0;
  for (tick = 0; tick < run.ticks; ++tick) {
    for (int draw = 0; draw < draws; ++draw) {
      const int32_t before = rand.value;
      tick_chunk_precipitation(level, pass_rng, weather, rand, run.sea_level, speed, 0, 0);
      if (rand.value != before) {
        ++replay.lcg_steps;
        char text[128];
        std::snprintf(text, sizeof(text), "%d\t%d\t%d", tick, draw, rand.value);
        push(std::string("lcg\t") + text);
      }
    }
  }

  replay.rand_end = rand.value;
  return replay;
}

// Prints the first few rows that differ, so a failure names the drift instead of
// only counting it.
void report_precipitation_mismatch(const RecordedRun& run, const std::vector<std::string>& on_disk,
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
// interaction trace against the checked-in one, plus the counters and the two
// invariants that make the trace a stream pin (the column LCG steps once per gate
// that passed, and a height query happens exactly when it was raining).
void test_golden_precipitation(const std::string& fixtures_dir) {
  std::vector<std::string> meta_rows;
  std::vector<std::string> scene_rows;
  const bool have_meta = read_fixture_rows(fixtures_dir + "/weather_meta.tsv", &meta_rows);
  const bool have_scene = read_fixture_rows(fixtures_dir + "/weather_scene.tsv", &scene_rows);
  CHECK(have_meta);
  CHECK(have_scene);
  if (!have_meta || !have_scene)
    return;
  CHECK(!meta_rows.empty());

  std::vector<RecordedRun> runs;
  for (const std::string& row : meta_rows) {
    const std::vector<std::string> fields = split_tabs(row);
    CHECK_EQ(fields.size(), size_t(18));
    if (fields.size() != 18)
      continue;
    RecordedRun run;
    run.seed = std::strtoll(fields[0].c_str(), nullptr, 10);
    run.name = fields[1];
    run.scene = fields[2];
    run.tick_speed = std::stoi(fields[3]);
    run.ticks = std::stoi(fields[4]);
    run.sea_level = std::stoi(fields[5]);
    run.rain_level = std::stof(fields[6]);
    run.thunder_level = std::stof(fields[7]);
    run.rand_start = static_cast<int32_t>(std::strtol(fields[8].c_str(), nullptr, 10));
    run.rand_end = static_cast<int32_t>(std::strtol(fields[9].c_str(), nullptr, 10));
    run.gates = static_cast<size_t>(std::strtoull(fields[10].c_str(), nullptr, 10));
    run.sampled_gates = static_cast<size_t>(std::strtoull(fields[11].c_str(), nullptr, 10));
    run.lcg_steps = static_cast<size_t>(std::strtoull(fields[12].c_str(), nullptr, 10));
    run.samples = static_cast<size_t>(std::strtoull(fields[13].c_str(), nullptr, 10));
    run.rolls = static_cast<size_t>(std::strtoull(fields[14].c_str(), nullptr, 10));
    run.fills = static_cast<size_t>(std::strtoull(fields[15].c_str(), nullptr, 10));
    run.rows = static_cast<size_t>(std::strtoull(fields[16].c_str(), nullptr, 10));
    run.log_gates = fields[17] == "true";
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
    CHECK(fields.size() >= 3);
    if (fields.size() < 3)
      continue;
    if (fields[0] == "default_biome" && fields.size() == 3) {
      scenes.push_back(RecordedScene{fields[1], fields[2], 0, {}, {}, {}});
    } else if (fields[0] == "default_height" && fields.size() == 3) {
      RecordedScene* scene = scene_for(fields[1]);
      CHECK(scene != nullptr);
      if (scene != nullptr)
        scene->default_height = std::stoi(fields[2]);
    } else if (fields[0] == "block" && fields.size() == 6) {
      RecordedScene* scene = scene_for(fields[1]);
      CHECK(scene != nullptr);
      if (scene != nullptr)
        scene->blocks.emplace_back(std::stoi(fields[2]), std::stoi(fields[3]), std::stoi(fields[4]),
                                   parse_state(fields[5]));
    } else if (fields[0] == "height" && fields.size() == 5) {
      RecordedScene* scene = scene_for(fields[1]);
      CHECK(scene != nullptr);
      if (scene != nullptr)
        scene->heights.emplace_back(std::stoi(fields[2]), std::stoi(fields[3]),
                                    std::stoi(fields[4]));
    } else if (fields[0] == "biome" && fields.size() == 6) {
      RecordedScene* scene = scene_for(fields[1]);
      CHECK(scene != nullptr);
      if (scene != nullptr)
        scene->biomes.emplace_back(std::stoi(fields[2]), std::stoi(fields[3]), std::stoi(fields[4]),
                                   fields[5]);
    } else {
      CHECK(false); // a kind this replayer does not know
    }
  }

  size_t checked_rows = 0;
  for (const RecordedRun& run : runs) {
    const RecordedScene* scene = scene_for(run.scene);
    CHECK(scene != nullptr);
    if (scene == nullptr)
      continue;
    CHECK(!scene->blocks.empty());

    const PrecipitationReplay replay = replay_precipitation(run, *scene);

    // The meta's own numbers, then the two invariants that make the trace a
    // stream pin: the column LCG advances exactly once per draw that passed the
    // 1-in-48 gate (whatever the weather is — `rand.next` is the argument to
    // `tick_precipitation`), and the height query is behind the rain check, so it
    // happens only when it was raining.
    CHECK_EQ(replay.gates, run.gates);
    CHECK_EQ(replay.sampled_gates, run.sampled_gates);
    CHECK_EQ(replay.lcg_steps, run.lcg_steps);
    CHECK_EQ(replay.samples, run.samples);
    CHECK_EQ(replay.rolls, run.rolls);
    CHECK_EQ(replay.fills, run.fills);
    CHECK_EQ(replay.rows.size(), run.rows);
    CHECK_EQ(replay.rand_start, run.rand_start);
    CHECK_EQ(replay.rand_end, run.rand_end);
    CHECK_EQ(replay.sampled_gates, replay.lcg_steps);
    CHECK_EQ(replay.samples, run.rain_level > kRainLevelThreshold ? replay.lcg_steps : size_t(0));
    if (run.tick_speed <= 0) {
      CHECK_EQ(replay.gates, size_t(0));
      CHECK_EQ(replay.rand_end, replay.rand_start);
    }

    std::vector<std::string> trace;
    const std::string path =
        fixtures_dir + "/weather_seed" + std::to_string(run.seed) + "_" + run.name + ".tsv";
    const bool have_trace = read_fixture_rows(path, &trace);
    CHECK(have_trace);
    if (!have_trace)
      continue;
    if (trace == replay.rows) {
      checked_rows += trace.size();
      continue;
    }
    report_precipitation_mismatch(run, trace, replay.rows);
    ++failures;
  }
  std::printf("weather: golden precipitation replayed for %zu runs, %zu rows compared\n",
              runs.size(), checked_rows);
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <fixtures_dir>\n", argv[0]);
    return 2;
  }
  test_biome_table();
  test_block_random_pos();
  test_weather_cycle();
  test_motion_blocking();
  test_precipitation_at();
  test_cauldron_precipitation();
  test_fire_column_precipitation();
  test_chunk_sampling();
  test_golden_precipitation(argv[1]);
  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("weather: all checks passed\n");
  return 0;
}
