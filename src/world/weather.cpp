// SPDX-License-Identifier: GPL-3.0-or-later
//
// Weather: the cycle, the precipitation query and the per-chunk sample. See
// world/weather.h for what is modelled and where the boundary sits.
#include "world/weather.h"

#include <algorithm>
#include <string>

#include "world/biome_table.h"
#include "world/container.h" // block_property / with_property

namespace mc::world {

namespace {

// `Biome.getPrecipitationAt`'s biome row, or nullptr for a name the table does
// not carry (the table is the whole vanilla registry, so that means a biome this
// server has never heard of — which answers NONE, like a biome with no
// precipitation).
const BiomeClimate* biome_climate_by_name(std::string_view name) {
  size_t lo = 0;
  size_t hi = sizeof(kBiomeClimate) / sizeof(kBiomeClimate[0]);
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const int cmp = name.compare(kBiomeClimate[mid].name);
    if (cmp == 0)
      return &kBiomeClimate[mid];
    if (cmp < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return nullptr;
}

// `UniformInt.of(min, max).sample(random)`.
int sample_uniform(const WeatherRandom& rng, int min, int max) {
  if (!rng.next_int || max < min)
    return min;
  return min + rng.next_int(max - min + 1);
}

constexpr std::string_view kCauldronEmpty = "minecraft:cauldron";
constexpr std::string_view kCauldronWater = "minecraft:water_cauldron";
constexpr std::string_view kCauldronPowderSnow = "minecraft:powder_snow_cauldron";
// `LayeredCauldronBlock.Layer`: `level` runs 1..3, and 3 is FULL.
constexpr int kCauldronFullLevel = 3;

// `LayeredCauldronBlock.Layer.getSerializedName()` read back out of the state's
// `level` property, or 0 when it is missing/unreadable.
int cauldron_level(const std::string& properties) {
  const std::string value = block_property(properties, "level");
  if (value.size() != 1 || value[0] < '1' || value[0] > '9')
    return 0;
  return value[0] - '0';
}

// `CauldronBlock.shouldHandlePrecipitation(level, precipitation)`: one float
// draw, which is why it is only ever reached through a block that has a
// precipitation handler at all (`Block.handlePrecipitation` is a no-op and takes
// no draw).
bool should_handle_precipitation(const WeatherRandom& rng, Precipitation precipitation) {
  if (!rng.next_float)
    return false;
  const float roll = rng.next_float();
  if (precipitation == Precipitation::kRain)
    return roll < kRainFillChance;
  if (precipitation == Precipitation::kSnow)
    return roll < kSnowFillChance;
  return false;
}

// The state `CauldronBlock.handlePrecipitation` writes: a full water or
// powder-snow cauldron (`Blocks.WATER_CAULDRON.defaultBlockState()` is level 1).
BlockStateKey full_cauldron_state(Precipitation precipitation) {
  return BlockStateKey{
      std::string(precipitation == Precipitation::kRain ? kCauldronWater : kCauldronPowderSnow),
      "level=1"};
}

} // namespace

bool biome_has_precipitation(std::string_view biome_name) {
  const BiomeClimate* biome = biome_climate_by_name(biome_name);
  return biome != nullptr && biome->has_precipitation;
}

Precipitation biome_precipitation_at(std::string_view biome_name, int /*y*/, int /*sea_level*/) {
  const BiomeClimate* biome = biome_climate_by_name(biome_name);
  if (biome == nullptr || !biome->has_precipitation)
    return Precipitation::kNone;
  // `coldEnoughToSnow(pos, seaLevel)` is `!warmEnoughToRain(pos, seaLevel)`,
  // i.e. the height-adjusted temperature below 0.15. Both noise terms of that
  // adjustment are omitted (world/weather.h), so this is the biome's base
  // temperature: a warm biome rains, a cold one snows.
  return biome->temperature < kSnowTemperature ? Precipitation::kSnow : Precipitation::kRain;
}

float Weather::rain_level_at(float partial_tick) const {
  return previous_rain_level + (rain_level - previous_rain_level) * partial_tick;
}

float Weather::thunder_level_at(float partial_tick) const {
  return previous_thunder_level + (thunder_level - previous_thunder_level) * partial_tick;
}

Weather::Advance Weather::advance(const WeatherRandom& rng) {
  Advance out;
  const bool was_raining = is_raining();

  // `if (this.canHaveWeather())`: sky light, no ceiling, not the end. This
  // server is one overworld, which answers true.
  int clear_time = clear_weather_time;
  int rain_left = rain_time;
  int thunder_left = thunder_time;
  bool raining_next = raining;
  bool thundering_next = thundering;
  if (clear_time > 0) {
    --clear_time;
    // Vanilla rewrites both countdowns to 0/1 (keeping the *current* flag's
    // sign) so that clearing the sky also ends the next rain decision.
    thunder_left = thundering_next ? 0 : 1;
    rain_left = raining_next ? 0 : 1;
    thundering_next = false;
    raining_next = false;
  } else {
    if (thunder_left > 0) {
      if (--thunder_left == 0)
        thundering_next = !thundering_next;
    } else {
      thunder_left = thundering_next ? sample_uniform(rng, kThunderDurationMin, kThunderDurationMax)
                                     : sample_uniform(rng, kThunderDelayMin, kThunderDelayMax);
    }
    if (rain_left > 0) {
      if (--rain_left == 0)
        raining_next = !raining_next;
    } else {
      rain_left = raining_next ? sample_uniform(rng, kRainDurationMin, kRainDurationMax)
                               : sample_uniform(rng, kRainDelayMin, kRainDelayMax);
    }
  }
  clear_weather_time = clear_time;
  rain_time = rain_left;
  thunder_time = thunder_left;
  raining = raining_next;
  thundering = thundering_next;

  // The level ramp: 0.01 a tick towards the flag, clamped. `isRaining` reads
  // the level, not the flag, so a flip becomes visible twenty ticks after it is
  // set — which is exactly the second a client's rain fade takes.
  previous_thunder_level = thunder_level;
  thunder_level =
      std::clamp(thunder_level + (thundering ? kWeatherLevelStep : -kWeatherLevelStep), 0.0f, 1.0f);
  previous_rain_level = rain_level;
  rain_level =
      std::clamp(rain_level + (raining ? kWeatherLevelStep : -kWeatherLevelStep), 0.0f, 1.0f);

  out.rain_level_changed = previous_rain_level != rain_level;
  out.thunder_level_changed = previous_thunder_level != thunder_level;
  out.rain_level = rain_level;
  out.thunder_level = thunder_level;
  const bool now_raining = is_raining();
  out.rain_started = !was_raining && now_raining;
  out.rain_stopped = was_raining && !now_raining;
  return out;
}

void Weather::set_parameters(int clear_time, int weather_time, bool raining_next,
                             bool thundering_next) {
  // `MinecraftServer.setWeatherParameters`. The thunder countdown is fed the
  // *rain* duration (`setThunderTime(rainTime)`), which is vanilla's own
  // argument reuse rather than a transcription slip.
  clear_weather_time = clear_time;
  rain_time = weather_time;
  thunder_time = weather_time;
  raining = raining_next;
  thundering = thundering_next;
}

void Weather::reset_cycle() {
  // `ServerLevel.resetWeatherCycle`: the countdowns and both flags, but not the
  // ticked levels (they ramp down on their own).
  rain_time = 0;
  raining = false;
  thunder_time = 0;
  thundering = false;
}

Precipitation precipitation_at(const Weather& weather, const PrecipitationQuery& query,
                               int sea_level, int x, int y, int z) {
  if (!weather.is_raining())
    return Precipitation::kNone;
  if (!query.motion_blocking_height || !query.biome_at)
    return Precipitation::kNone;
  // `canSeeSky(pos)`: the light engine's answer is the column's motion-blocking
  // height (world/weather.h). Vanilla's next test is the same comparison again
  // (`getHeightmapPos(MOTION_BLOCKING, pos).getY() > pos.getY()`), so it is not
  // repeated here.
  if (query.motion_blocking_height(x, z) > y)
    return Precipitation::kNone;
  return biome_precipitation_at(query.biome_at(x, y, z), y, sea_level);
}

bool is_raining_at(const Weather& weather, const PrecipitationQuery& query, int sea_level, int x,
                   int y, int z) {
  return precipitation_at(weather, query, sea_level, x, y, z) == Precipitation::kRain;
}

void BlockRandomPos::next(int xo, int yo, int zo, int y_mask, int* x, int* y, int* z) {
  // `this.randValue = this.randValue * 3 + 1013904223`: a wrapping int32
  // multiply-add, done in unsigned arithmetic so the wrap is defined.
  value = static_cast<int32_t>(static_cast<uint32_t>(value) * 3u + 1013904223u);
  const int32_t val = value >> 2; // Java's `>>` on an int is arithmetic
  *x = xo + (val & 0xF);
  *y = yo + ((val >> 16) & y_mask);
  *z = zo + ((val >> 8) & 0xF);
}

Precipitation cauldron_precipitation_type(std::string_view block_name) {
  if (block_name == kCauldronWater)
    return Precipitation::kRain;
  if (block_name == kCauldronPowderSnow)
    return Precipitation::kSnow;
  return Precipitation::kNone;
}

bool cauldron_handle_precipitation(const PrecipitationLevel& level, const WeatherRandom& rng, int x,
                                   int y, int z, const BlockStateKey& state,
                                   Precipitation precipitation) {
  if (precipitation == Precipitation::kNone || !level.set_block)
    return false;
  // `Block.handlePrecipitation` is a no-op, so only a cauldron draws at all.
  if (state.name == kCauldronEmpty) {
    // `CauldronBlock.handlePrecipitation`: the chance first, then the kind
    // (`Blocks.WATER_CAULDRON` for RAIN, `POWDER_SNOW_CAULDRON` for SNOW).
    if (!should_handle_precipitation(rng, precipitation))
      return false;
    level.set_block(x, y, z, full_cauldron_state(precipitation));
    return true;
  }
  // `LayeredCauldronBlock.handlePrecipitation`: vanilla's guard is
  // `!shouldHandlePrecipitation(...) || level == 3 || precipitation !=
  // precipitationType`, so the chance roll comes first — the draw order a
  // scripted test sees — and only a cauldron of this precipitation kind is
  // touched.
  const Precipitation accepts = cauldron_precipitation_type(state.name);
  if (accepts == Precipitation::kNone)
    return false;
  if (!should_handle_precipitation(rng, precipitation))
    return false;
  if (accepts != precipitation)
    return false;
  const int current = cauldron_level(state.properties);
  if (current < 1 || current >= kCauldronFullLevel)
    return false;
  // `state.cycle(LEVEL)`: one layer more, still below FULL.
  level.set_block(x, y, z,
                  BlockStateKey{state.name, with_property(state.properties, "level",
                                                          std::to_string(current + 1))});
  return true;
}

void tick_precipitation(const PrecipitationLevel& level, const WeatherRandom& rng,
                        const Weather& weather, int sea_level, int x, int z) {
  if (!level.get_block || !level.set_block || !level.motion_blocking_height || !level.biome_at)
    return;
  if (!weather.is_raining())
    return;
  // `topPos = getHeightmapPos(MOTION_BLOCKING, pos)` and `belowPos =
  // topPos.below()`: the block the column's rain actually lands on. Note that
  // the biome question below bypasses the sky check `precipitationAt` would do
  // — which is why a cauldron standing at the top of its own column (its height
  // is then one above the cauldron) still fills.
  const int top = level.motion_blocking_height(x, z);
  const int below = top - 1;
  // Vanilla splits the two positions: the biome is `getBiome(topPos)` while the
  // height it is asked about is `belowPos` (`biome.getPrecipitationAt(belowPos,
  // seaLevel)`). They can differ at a 3D biome boundary, and it is the first
  // free cell that names the biome.
  const Precipitation precipitation =
      biome_precipitation_at(level.biome_at(x, top, z), below, sea_level);
  if (precipitation == Precipitation::kNone)
    return;
  const BlockStateKey state = level.get_block(x, below, z);
  cauldron_handle_precipitation(level, rng, x, below, z, state, precipitation);
}

void tick_chunk_precipitation(const PrecipitationLevel& level, const WeatherRandom& rng,
                              const Weather& weather, BlockRandomPos& rand, int sea_level,
                              int tick_speed, int chunk_x, int chunk_z) {
  if (tick_speed <= 0 || !rng.next_int)
    return;
  const int min_x = chunk_x * 16;
  const int min_z = chunk_z * 16;
  for (int i = 0; i < tick_speed; ++i) {
    // `if (this.random.nextInt(48) != 0) continue;` — the gate is drawn whether
    // or not anything can be filled, so the sample sequence does not depend on
    // the weather.
    if (rng.next_int(kPrecipitationSampleGate) != 0)
      continue;
    // `this.tickPrecipitation(this.getBlockRandomPos(minX, 0, minZ, 15))`: the
    // sampled y is unused (the heightmap decides where the column's top is).
    int x = 0, y = 0, z = 0;
    rand.next(min_x, 0, min_z, 15, &x, &y, &z);
    tick_precipitation(level, rng, weather, sea_level, x, z);
  }
}

} // namespace mc::world
