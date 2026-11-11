// SPDX-License-Identifier: GPL-3.0-or-later
//
// Weather: the level's rain and thunder, and the precipitation that makes them
// matter (Phase 3 milestone H).
//
// Before this, `Level.isRaining()` was a constant false. That made three things
// unreachable rather than merely untested: `FireBlock.tick`'s rain removal (the
// `isNearRain` + `0.2 + age * 0.03` branch at the top of the tick) and its twin
// on the spread, `Entity.applyEffectsFromBlocks`'s `isInRain` -> `clearFire`
// (the half of the burning countdown a fire cannot end by itself), and the
// cauldron's `handlePrecipitation`. All three are live here.
//
// Modelled, and from where:
//   * `ServerLevel.advanceWeatherCycle` — the whole cycle: `WeatherData`'s
//     clear/rain/thunder countdowns against `RAIN_DELAY` / `RAIN_DURATION` /
//     `THUNDER_DELAY` / `THUNDER_DURATION`, and the rain/thunder *level* ramp
//     (0.01 a tick, clamped to 0..1) that `Level.isRaining` / `isThundering`
//     read — which is why a flip is only visible a second after it is set;
//   * `MinecraftServer.setWeatherParameters` — what `/weather` sets;
//   * `Level.precipitationAt` / `isRainingAt` — the sky and biome query, from
//     the column's motion-blocking height and the generated biome table
//     (world/biome_table.h);
//   * `ServerLevel.tickChunk`'s precipitation half and `tickPrecipitation` —
//     the `nextInt(48)` sample per chunk per random-tick draw, the
//     `Level.getBlockRandomPos` LCG that picks the column, and the block's own
//     `handlePrecipitation` (a cauldron's RAIN and SNOW branches, at vanilla's
//     0.05 / 0.1 chances).
//
// What is NOT modelled, and why:
//   * **snow layers and ice.** `Biome.shouldSnow` and `shouldFreeze` gate on
//     `level.getBrightness(BLOCK, pos) < 10`, and this server has no light
//     engine (chunk light is only what the *client* was sent). The precipitation
//     *kind* is modelled, so a cold biome's cauldron takes its powder-snow
//     branch while its ground never accumulates snow;
//   * **the two noise terms in `Biome.getHeightAdjustedTemperature`** — the
//     above-`sea_level + 17` temperature fall-off and `TemperatureModifier
//     .FROZEN`'s two noise fields. Both are octave Perlin noise this server does
//     not have, and both matter only within a hair of the 0.15 rain/snow line.
//     `biome_precipitation_at` compares the biome's base temperature, so a
//     high-altitude cell is as warm as sea level and a frozen ocean (base 0.0)
//     snows;
//   * **lightning.** `ServerLevel.tickThunder` spawns a `LightningBolt` entity
//     (plus a skeleton-horse trap) on a 1-in-100000 roll. Thunder is state only
//     here: `/weather thunder` sets the levels and the client sees a storm, but
//     no bolt ever lands and nothing can be struck or set alight by one;
//   * **`Level.canSeeSky` is answered by the heightmap.** Vanilla asks its light
//     engine (`getBrightness(SKY, pos) >= 15`) and *then* the same
//     motion-blocking height this server has, so for a block whose light
//     attenuation and whose `blocksMotion()` agree the two answers coincide
//     (glass: transparent to light, yet motion-blocking, so both say "blocked").
//     They can part company only for a block that darkens a cell without
//     blocking motion — `blocksMotion()` excludes cobweb outright, which is one
//     such block;
//   * **persistence is +MC's own extension for the ramp.** `level.dat` carries
//     `rainTime` / `thunderTime` / `raining` / `thundering`, and +MC adds
//     `rainLevel` / `thunderLevel` (float tags vanilla ignores) so a restart
//     resumes a 0.01-a-tick fade exactly instead of snapping to full strength.
//     The fields are written on the autosave tick and at shutdown; a file
//     written before the pair existed falls back to the flags. `WEATHER_CYCLE`
//     / `ADVANCE_WEATHER` are not gamerules: the cycle always advances, which
//     is both of their defaults.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "world/chunk.h"

namespace mc::world {

// ---- vanilla constants ------------------------------------------------------

// `Level.isRaining` / `isThundering`: the *ramped* level against these, not the
// `WeatherData` flags.
constexpr float kRainLevelThreshold = 0.2f;
constexpr float kThunderLevelThreshold = 0.9f;
// `advanceWeatherCycle`'s per-tick ramp and clamp.
constexpr float kWeatherLevelStep = 0.01f;
// `ServerLevel.RAIN_DELAY` / `RAIN_DURATION` / `THUNDER_DELAY` /
// `THUNDER_DURATION` — `UniformInt.of(min, max).sample(random)`, i.e.
// `min + nextInt(max - min + 1)`.
constexpr int kRainDelayMin = 12000;
constexpr int kRainDelayMax = 180000;
constexpr int kRainDurationMin = 12000;
constexpr int kRainDurationMax = 24000;
constexpr int kThunderDelayMin = 12000;
constexpr int kThunderDelayMax = 180000;
constexpr int kThunderDurationMin = 3600;
constexpr int kThunderDurationMax = 15600;
// `CauldronBlock.shouldHandlePrecipitation`'s two chances, shared with
// `LayeredCauldronBlock`.
constexpr float kRainFillChance = 0.05f;
constexpr float kSnowFillChance = 0.1f;
// `ServerLevel.tickChunk`: `random.nextInt(48) != 0` skips the draw.
constexpr int kPrecipitationSampleGate = 48;
// `Biome.warmEnoughToRain`: the height-adjusted temperature line between rain
// and snow.
constexpr float kSnowTemperature = 0.15f;
// The overworld's sea level — `Level.getSeaLevel()`, the base the temperature
// adjustment and the join packet's own `sea_level = 63` both use.
constexpr int kOverworldSeaLevel = 63;

// ---- biome climate ----------------------------------------------------------

// `Biome.Precipitation`.
enum class Precipitation : uint8_t {
  kNone = 0,
  kRain,
  kSnow,
};

// `Biome.hasPrecipitation()`.
bool biome_has_precipitation(std::string_view biome_name);
// `Biome.getPrecipitationAt(pos, seaLevel)`: NONE when the biome does not
// precipitate at all (`minecraft:the_void`, the nether, the end), else SNOW or
// RAIN on the 0.15 temperature line. `y`/`sea_level` are `getPrecipitationAt`'s
// own arguments and are unused here because they only feed the two omitted
// height/noise terms (see the header comment).
Precipitation biome_precipitation_at(std::string_view biome_name, int /*y*/, int /*sea_level*/);

// ---- the level's weather ----------------------------------------------------

// The draws the cycle takes: `UniformInt.sample(random)` is
// `min + nextInt(max - min + 1)`, so the weather cycle needs only `next_int`;
// `next_float` is the cauldron's own chance (`RandomSource.nextFloat`).
struct WeatherRandom {
  std::function<int(int bound)> next_int;
  std::function<float()> next_float;
};

// `WeatherData` plus `Level`'s own four level fields, which is the state the
// clients are told about. One of these is the whole level: weather is not
// per-player.
struct Weather {
  // WeatherData (the record `ServerLevel.resetWeatherCycle` clears).
  int clear_weather_time = 0;
  int rain_time = 0;
  int thunder_time = 0;
  bool raining = false;    // weatherData.isRaining()
  bool thundering = false; // weatherData.isThundering()
  // `Level.rainLevel` / `oRainLevel` / `thunderLevel` / `oThunderLevel`.
  float rain_level = 0.0f;
  float previous_rain_level = 0.0f;
  float thunder_level = 0.0f;
  float previous_thunder_level = 0.0f;

  // `Level.getRainLevel(partialTick)` / `getThunderLevel` (`Mth.lerp`).
  float rain_level_at(float partial_tick) const;
  float thunder_level_at(float partial_tick) const;
  // `Level.isRaining` / `isThundering`. Vanilla also requires `canHaveWeather()`
  // (sky light, no ceiling, not the end); this server has one overworld
  // dimension, which answers true.
  bool is_raining() const {
    return rain_level_at(1.0f) > kRainLevelThreshold;
  }
  bool is_thundering() const {
    return thunder_level_at(1.0f) > kThunderLevelThreshold;
  }

  // What one tick of `advanceWeatherCycle` owes the clients. The three packets
  // are sent in vanilla's order and only when their own field says so.
  struct Advance {
    bool rain_level_changed = false;    // oRainLevel != rainLevel
    bool thunder_level_changed = false; // oThunderLevel != thunderLevel
    bool rain_started = false;          // wasRaining false -> isRaining true
    bool rain_stopped = false;          // wasRaining true  -> isRaining false
    float rain_level = 0.0f;            // the new levels, for the packets
    float thunder_level = 0.0f;
  };
  // One tick of `ServerLevel.advanceWeatherCycle`: the countdowns, the level
  // ramp, and (via the returned struct) the broadcasts that follow it. The
  // `ADVANCE_WEATHER` / `WEATHER_CYCLE` gamerules are their true defaults.
  Advance advance(const WeatherRandom& rng);

  // `MinecraftServer.setWeatherParameters(clearTime, weatherTime, raining,
  // thundering)`: what `/weather` calls. (`this.weatherData.setThunderTime` is
  // fed `rainTime`, not the thunder duration — vanilla's own argument reuse.)
  void set_parameters(int clear_weather_time, int weather_time, bool raining, bool thundering);

  // `ServerLevel.resetWeatherCycle` (the "enough players slept" path).
  void reset_cycle();
};

// ---- precipitation ----------------------------------------------------------

// The two level questions `Level.precipitationAt` asks, as callbacks so the
// rules stay free of any one world.
struct PrecipitationQuery {
  // `LevelReader.getHeight(Heightmap.Types.MOTION_BLOCKING, x, z)`.
  std::function<int(int x, int z)> motion_blocking_height;
  // `Level.getBiome(pos).value()`.
  std::function<std::string(int x, int y, int z)> biome_at;
};

// The world `tickPrecipitation` reads and mutates. `set_block` is
// `Level.setBlockAndUpdate`: unlike a fire tick's writes, every precipitation
// write is sent.
struct PrecipitationLevel : PrecipitationQuery {
  std::function<BlockStateKey(int x, int y, int z)> get_block;
  std::function<void(int x, int y, int z, const BlockStateKey& state)> set_block;
};

// `Level.precipitationAt(pos)`: NONE unless it is raining, the cell is clear to
// the sky (the column's motion-blocking height is at or below it), and the
// biome there precipitates.
Precipitation precipitation_at(const Weather& weather, const PrecipitationQuery& query,
                               int sea_level, int x, int y, int z);

// `Level.isRainingAt(pos)`: the RAIN half of the above — what fire's
// `isNearRain` and a burning entity's `isInRain` ask.
bool is_raining_at(const Weather& weather, const PrecipitationQuery& query, int sea_level, int x,
                   int y, int z);

// `Level.getBlockRandomPos`'s own LCG (`Level.randValue`, seeded from the
// level's random at construction, advanced one step per call): the column
// `tickChunk` samples is this sequence's, not the caller's.
struct BlockRandomPos {
  // `Level.randValue = RandomSource.createThreadLocalInstance().nextInt()`.
  int32_t value = 0;

  // The sampled position: `xo + (val & 0xF)`, `yo + (val >> 16 & yMask)`,
  // `zo + (val >> 8 & 0xF)`.
  void next(int xo, int yo, int zo, int y_mask, int* x, int* y, int* z);
};

// `ServerLevel.tickChunk`'s precipitation half: `tick_speed` draws
// (`randomTickSpeed`, vanilla's default of 3), each with a 1-in-48 chance of
// sampling one random column of the chunk and running `tickPrecipitation` on
// it. (The random *block* ticks that follow it in `tickChunk` are the block-tick
// loop's job — see world/block_tick.h.)
void tick_chunk_precipitation(const PrecipitationLevel& level, const WeatherRandom& rng,
                              const Weather& weather, BlockRandomPos& rand, int sea_level,
                              int tick_speed, int chunk_x, int chunk_z);

// `ServerLevel.tickPrecipitation(pos)`: `topPos` is the column's first free
// cell, `belowPos` the block the rain lands on, and vanilla asks the biome at
// the former for the precipitation at the latter. The ice/snow half is the
// documented omission above; the cauldron half is
// `cauldron_handle_precipitation`.
void tick_precipitation(const PrecipitationLevel& level, const WeatherRandom& rng,
                        const Weather& weather, int sea_level, int x, int z);

// `CauldronBlock.handlePrecipitation` (empty -> a full water or powder-snow
// cauldron) and `LayeredCauldronBlock.handlePrecipitation` (level + 1, at most
// 3, and only for the precipitation kind the cauldron holds). Returns true when
// the caller's `set_block` was called; false for a block with no precipitation
// handler at all.
bool cauldron_handle_precipitation(const PrecipitationLevel& level, const WeatherRandom& rng, int x,
                                   int y, int z, const BlockStateKey& state,
                                   Precipitation precipitation);

// The precipitation a cauldron block accepts (`LayeredCauldronBlock`'s
// `precipitationType`): RAIN for `minecraft:water_cauldron`, SNOW for
// `minecraft:powder_snow_cauldron`, NONE for anything else.
Precipitation cauldron_precipitation_type(std::string_view block_name);

} // namespace mc::world
