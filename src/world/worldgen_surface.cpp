// SPDX-License-Identifier: GPL-3.0-or-later
//
// The surface stage: `SurfaceSystem` over `SurfaceRuleData.overworldLike(
// biomes, doPreliminarySurfaceCheck=true, bedrockRoof=false, bedrockFloor=true)`,
// the rule set `NoiseGeneratorSettings.OVERWORLD` carries.
//
// What is ported, one vanilla class per section below:
//
//   * `SurfaceRules`' engine: conditions (`biome`, `noise_threshold`,
//     `vertical_gradient`, `y_above`, `water`, `stone_depth`, `not`, `steep`,
//     `hole`, `temperature`, `above_preliminary_surface`) and rules
//     (`block`, `sequence`, `condition`, `bandlands`).
//   * `SurfaceSystem`: the per-column driver, `getSurfaceDepth`,
//     `getSurfaceSecondary`, the 192-entry clay band table (`getBand`), and
//     the two biome extensions (`erodedBadlandsExtension` and
//     `frozenOceanExtension`) that run around the rule pass.
//   * `BiomeManager`'s fuzzy biome lookup, its `obfuscateSeed` (SHA-256 of the
//     seed, little-endian both ways the way Guava's `hashLong`/`asLong` are),
//     and `LinearCongruentialGenerator`.
//   * `Biome`'s legacy simplex noises (`TEMPERATURE_NOISE`,
//     `FROZEN_TEMPERATURE_NOISE`, `BIOME_INFO_NOISE`) and the
//     `getHeightAdjustedTemperature`/`coldEnoughToSnow` chain the
//     `temperature` condition reads.
//
// The one deliberate simplification: `SurfaceRules.Context`'s laziness. Every
// condition in vanilla caches its last answer keyed by the context's XZ/Y
// update counters; the caches are pure performance here, so this port keeps
// only the two the context itself owns (secondary surface depth and the
// preliminary-surface interpolation) and recomputes the rest. Values are
// identical either way — only the cost moves.
//
// The rule tree is written as code (constructing it is `SurfaceRuleData`'s
// job), but the two `noiseCondition2d(Noises.SURFACE, ...)` thresholds that
// `SurfaceRuleData.surfaceNoiseAbove` derives are spelled where they are used,
// e.g. `1.0 / 8.25`, so the arithmetic is visible.
#include "world/worldgen_surface.h"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "world/biome_table.h"
#include "world/worldgen_biome.h"
#include "world/worldgen_math.h"
#include "world/worldgen_noise.h"
#include "world/worldgen_random.h"

namespace mc::world {
namespace {

struct Condition;
struct Rule;
using ConditionPtr = std::unique_ptr<Condition>;
using RulePtr = std::unique_ptr<Rule>;

// ---- the block states, in `WgSurfaceBlock` order ---------------------------

const WgSurfaceState kSurfaceStates[kWgSurfaceBlockCount] = {
    {"minecraft:air", ""},
    {"minecraft:stone", ""},
    {"minecraft:water", "level=0"},
    {"minecraft:lava", "level=0"},
    {"minecraft:granite", ""},
    {"minecraft:copper_ore", ""},
    {"minecraft:raw_copper_block", ""},
    {"minecraft:tuff", ""},
    {"minecraft:deepslate_iron_ore", ""},
    {"minecraft:raw_iron_block", ""},
    {"minecraft:bedrock", ""},
    {"minecraft:deepslate", "axis=y"},
    {"minecraft:dirt", ""},
    {"minecraft:grass_block", "snowy=false"},
    {"minecraft:podzol", "snowy=false"},
    {"minecraft:coarse_dirt", ""},
    {"minecraft:mycelium", "snowy=false"},
    {"minecraft:calcite", ""},
    {"minecraft:gravel", ""},
    {"minecraft:sand", ""},
    {"minecraft:sandstone", ""},
    {"minecraft:red_sand", ""},
    {"minecraft:red_sandstone", ""},
    {"minecraft:terracotta", ""},
    {"minecraft:white_terracotta", ""},
    {"minecraft:orange_terracotta", ""},
    {"minecraft:yellow_terracotta", ""},
    {"minecraft:brown_terracotta", ""},
    {"minecraft:light_gray_terracotta", ""},
    {"minecraft:packed_ice", ""},
    {"minecraft:snow_block", ""},
    {"minecraft:mud", ""},
    {"minecraft:powder_snow", ""},
    {"minecraft:ice", ""},
    {"minecraft:cinnabar", ""},
    {"minecraft:sulfur", ""},
    {"minecraft:red_terracotta", ""},
    {"minecraft:netherrack", ""},
    {"minecraft:soul_sand", ""},
    {"minecraft:soul_soil", ""},
    {"minecraft:basalt", "axis=y"},
    {"minecraft:blackstone", ""},
    {"minecraft:warped_nylium", ""},
    {"minecraft:crimson_nylium", ""},
    {"minecraft:nether_wart_block", ""},
    {"minecraft:warped_wart_block", ""},
    {"minecraft:cave_air", ""},
};

constexpr uint8_t code_of(WgSurfaceBlock block) {
  return static_cast<uint8_t>(block);
}

// ---- small math and table helpers ------------------------------------------

// `Biome.getTemperature`'s biome row, or nullptr for a name the table does not
// carry (the table is the whole vanilla registry).
const BiomeClimate* climate_of(const char* name) {
  size_t lo = 0;
  size_t hi = sizeof(kBiomeClimate) / sizeof(kBiomeClimate[0]);
  const std::string_view wanted = name == nullptr ? std::string_view{} : name;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const int cmp = wanted.compare(kBiomeClimate[mid].name);
    if (cmp == 0)
      return &kBiomeClimate[mid];
    if (cmp < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return nullptr;
}

// The index of a data-pack noise in the generated table, or -1.
int32_t noise_index_by_name(const char* name) {
  for (size_t i = 0; i < sizeof(kWgNoiseParams) / sizeof(kWgNoiseParams[0]); ++i) {
    if (std::strcmp(name, kWgNoiseParams[i].name) == 0)
      return static_cast<int32_t>(i);
  }
  return -1;
}

// `Biome`'s three static legacy noises, built once from their fixed seeds.
struct LegacyNoises {
  WgPerlinSimplexNoise
      temperature; // `new PerlinSimplexNoise(WorldgenRandom(LegacyRandomSource(1234)), [0])`
  WgPerlinSimplexNoise frozen; // seed 3456, octaves [-2, -1, 0]
  WgPerlinSimplexNoise info;   // seed 2345, octaves [0]
};

const LegacyNoises& legacy_noises() {
  static const LegacyNoises noises = [] {
    LegacyNoises built;
    const int32_t temperature_octaves[] = {0};
    const int32_t frozen_octaves[] = {-2, -1, 0};
    const int32_t info_octaves[] = {0};
    WgLegacyRandom temperature_random(1234);
    built.temperature.init(temperature_random, temperature_octaves, 1);
    WgLegacyRandom frozen_random(3456);
    built.frozen.init(frozen_random, frozen_octaves, 3);
    WgLegacyRandom info_random(2345);
    built.info.init(info_random, info_octaves, 1);
    return built;
  }();
  return noises;
}

// `LinearCongruentialGenerator.next(rval, c)`.
int64_t lcg_next(int64_t rval, int64_t c) {
  const uint64_t product =
      static_cast<uint64_t>(rval) *
          (static_cast<uint64_t>(rval) * 6364136223846793005ull + 1442695040888963407ull) +
      static_cast<uint64_t>(c);
  return static_cast<int64_t>(product);
}

// `BiomeManager.getFiddle`.
double get_fiddle(int64_t rval) {
  // `Math.floorMod(rval >> 24, 1024)`: arithmetic shift, then a non-negative
  // remainder.
  int64_t value = rval >> 24;
  value %= 1024;
  if (value < 0)
    value += 1024;
  return (static_cast<double>(value) / 1024.0 - 0.5) * 0.9;
}

// `BiomeManager.getFiddledDistance`'s corner offsets are computed by
// `ChunkBiomeManager::update_fiddles` (below, per parent quart cell) rather than
// per block cell: the offsets depend on the corner coordinates and the seed
// only, and one parent cell answers up to 64 block cells.

// `VerticalAnchor`, resolved against this dimension's height.
struct Anchor {
  enum class Kind { kAbsolute, kAboveBottom, kBelowTop };
  Kind kind = Kind::kAbsolute;
  int32_t value = 0;

  int32_t resolve(int32_t min_y, int32_t height) const {
    switch (kind) {
    case Kind::kAbsolute:
      return value;
    case Kind::kAboveBottom:
      return min_y + value;
    case Kind::kBelowTop:
      return min_y + height - 1 - value;
    }
    return value;
  }
};

constexpr Anchor absolute(int32_t y) {
  return Anchor{Anchor::Kind::kAbsolute, y};
}
constexpr Anchor above_bottom(int32_t offset) {
  return Anchor{Anchor::Kind::kAboveBottom, offset};
}
constexpr Anchor below_top(int32_t offset) {
  return Anchor{Anchor::Kind::kBelowTop, offset};
}

// The noise-instantiated `SurfaceSystem` fields, resolved once per call.
struct SurfaceNoises {
  int32_t surface = -1;
  int32_t surface_secondary = -1;
  int32_t clay_bands_offset = -1;
  int32_t badlands_pillar = -1;
  int32_t badlands_pillar_roof = -1;
  int32_t badlands_surface = -1;
  int32_t iceberg_pillar = -1;
  int32_t iceberg_pillar_roof = -1;
  int32_t iceberg_surface = -1;

  void resolve() {
    surface = noise_index_by_name("minecraft:surface");
    surface_secondary = noise_index_by_name("minecraft:surface_secondary");
    clay_bands_offset = noise_index_by_name("minecraft:clay_bands_offset");
    badlands_pillar = noise_index_by_name("minecraft:badlands_pillar");
    badlands_pillar_roof = noise_index_by_name("minecraft:badlands_pillar_roof");
    badlands_surface = noise_index_by_name("minecraft:badlands_surface");
    iceberg_pillar = noise_index_by_name("minecraft:iceberg_pillar");
    iceberg_pillar_roof = noise_index_by_name("minecraft:iceberg_pillar_roof");
    iceberg_surface = noise_index_by_name("minecraft:iceberg_surface");
  }
};

// `BiomeManager`: the fuzzy (fiddled-distance) lookup over the bare biome
// source. `at` is vanilla's `getBiome(BlockPos)`; the source itself is the
// caller's `biome_at_quart`.
struct ChunkBiomeManager {
  ChunkBiomeManager() = default;
  ChunkBiomeManager(const WgSurfaceChunk* chunk_, int64_t zoom_seed_)
      : chunk(chunk_), zoom_seed(zoom_seed_) {}

  const WgSurfaceChunk* chunk = nullptr;
  int64_t zoom_seed = 0;

  const char* at(int32_t x, int32_t y, int32_t z) const {
    const int32_t abs_x = x - 2;
    const int32_t abs_y = y - 2;
    const int32_t abs_z = z - 2;
    const int32_t parent_x = abs_x >> 2;
    const int32_t parent_y = abs_y >> 2;
    const int32_t parent_z = abs_z >> 2;
    // The answer is a pure function of the parent quart cell, and the surface
    // walk hands this one parent cell three of every four block steps it takes
    // down a column (y decreases by one, the parent is a 4-block cell). The
    // last answer is therefore kept like the fiddle offsets are: one compare on
    // the hit path, no eight-corner distance loop, no biome lookup.
    if (result_valid_ && result_x_ == parent_x && result_y_ == parent_y && result_z_ == parent_z)
      return result_;
    const double fract_x = static_cast<double>(abs_x & 3) / 4.0;
    const double fract_y = static_cast<double>(abs_y & 3) / 4.0;
    const double fract_z = static_cast<double>(abs_z & 3) / 4.0;
    // The eight corners' fiddle offsets depend on the corner coordinates and
    // the seed alone, and one parent quart cell answers up to 64 block cells
    // (the surface walk asks every cell of the 4x4x4 block cube it covers).
    // They are three `get_fiddle` values per corner — an integer modulo and a
    // double division each — which `sample` put at most of
    // `ChunkBiomeManager::at`'s cost. Caching them per parent cell leaves the
    // per-cell arithmetic (eight squared distances) and nothing else; the
    // expressions and their order are unchanged, so this is memoisation of a
    // pure function, not a re-association.
    update_fiddles(parent_x, parent_y, parent_z);
    int32_t min_i = 0;
    double min_distance = std::numeric_limits<double>::infinity();
    for (int32_t i = 0; i < 8; ++i) {
      const bool x_even = (i & 4) == 0;
      const bool y_even = (i & 2) == 0;
      const bool z_even = (i & 1) == 0;
      const double distance_x = x_even ? fract_x : fract_x - 1.0;
      const double distance_y = y_even ? fract_y : fract_y - 1.0;
      const double distance_z = z_even ? fract_z : fract_z - 1.0;
      const double& fiddle_x = fiddles_[i][0];
      const double& fiddle_y = fiddles_[i][1];
      const double& fiddle_z = fiddles_[i][2];
      const double dz = distance_z + fiddle_z;
      const double dy = distance_y + fiddle_y;
      const double dx = distance_x + fiddle_x;
      const double next = dz * dz + dy * dy + dx * dx;
      if (min_distance > next) {
        min_i = i;
        min_distance = next;
      }
    }
    const int32_t biome_x = (min_i & 4) == 0 ? parent_x : parent_x + 1;
    const int32_t biome_y = (min_i & 2) == 0 ? parent_y : parent_y + 1;
    const int32_t biome_z = (min_i & 1) == 0 ? parent_z : parent_z + 1;
    // Canonical pointer (worldgen_biome.h), so the rule tree's biome conditions
    // can compare with `==`: this also covers a biome source that answers with
    // its own name strings rather than the preset table's.
    const char* biome = chunk->biome_at_quart(chunk->user, biome_x, biome_y, biome_z);
    result_ = wg_canonical_biome(biome);
    result_valid_ = true;
    result_x_ = parent_x;
    result_y_ = parent_y;
    result_z_ = parent_z;
    return result_;
  }

private:
  // `BiomeManager.getFiddledDistance`'s three `getFiddle` values for the eight
  // corners of the parent cell, in the bit order `at` walks (x = bit 4, y = bit
  // 2, z = bit 1).
  void update_fiddles(int32_t parent_x, int32_t parent_y, int32_t parent_z) const {
    if (fiddles_valid_ && cache_seed_ == zoom_seed && cache_x_ == parent_x &&
        cache_y_ == parent_y && cache_z_ == parent_z)
      return;
    for (int32_t i = 0; i < 8; ++i) {
      const int32_t corner_x = (i & 4) == 0 ? parent_x : parent_x + 1;
      const int32_t corner_y = (i & 2) == 0 ? parent_y : parent_y + 1;
      const int32_t corner_z = (i & 1) == 0 ? parent_z : parent_z + 1;
      int64_t rval = zoom_seed;
      rval = lcg_next(rval, corner_x);
      rval = lcg_next(rval, corner_y);
      rval = lcg_next(rval, corner_z);
      rval = lcg_next(rval, corner_x);
      rval = lcg_next(rval, corner_y);
      rval = lcg_next(rval, corner_z);
      fiddles_[i][0] = get_fiddle(rval);
      rval = lcg_next(rval, zoom_seed);
      fiddles_[i][1] = get_fiddle(rval);
      rval = lcg_next(rval, zoom_seed);
      fiddles_[i][2] = get_fiddle(rval);
    }
    fiddles_valid_ = true;
    cache_seed_ = zoom_seed;
    cache_x_ = parent_x;
    cache_y_ = parent_y;
    cache_z_ = parent_z;
  }

  mutable bool fiddles_valid_ = false;
  mutable int64_t cache_seed_ = 0;
  mutable int32_t cache_x_ = 0;
  mutable int32_t cache_y_ = 0;
  mutable int32_t cache_z_ = 0;
  mutable double fiddles_[8][3] = {};
  // The memo above: the last parent cell asked and the biome it resolved to.
  mutable bool result_valid_ = false;
  mutable int32_t result_x_ = 0;
  mutable int32_t result_y_ = 0;
  mutable int32_t result_z_ = 0;
  mutable const char* result_ = nullptr;
};

// `Biome.getHeightAdjustedTemperature`, with the frozen modifier. The
// `TEMPERATURE_NOISE` term only applies above sea level + 17; the modifier's
// two simplex reads apply everywhere.
float height_adjusted_temperature(int32_t block_x, int32_t block_y, int32_t block_z,
                                  int32_t sea_level, const BiomeClimate& climate) {
  float temperature = climate.temperature;
  if (climate.frozen_modifier) {
    const LegacyNoises& noises = legacy_noises();
    const double large_variation = noises.frozen.get_value(static_cast<double>(block_x) * 0.05,
                                                           static_cast<double>(block_z) * 0.05) *
                                   7.0;
    const double edge_variation = noises.info.get_value(static_cast<double>(block_x) * 0.2,
                                                        static_cast<double>(block_z) * 0.2);
    if (large_variation + edge_variation < 0.3 &&
        noises.info.get_value(static_cast<double>(block_x) * 0.09,
                              static_cast<double>(block_z) * 0.09) < 0.8) {
      temperature = 0.2f;
    }
  }
  const int32_t snow_level = sea_level + 17;
  if (block_y > snow_level) {
    // The noise call's arguments are *float* divisions widened to double; the
    // scale and the subtraction below are float arithmetic (`0.05f / 40.0f`).
    const float noise_value = static_cast<float>(
        legacy_noises().temperature.get_value(static_cast<float>(block_x) / 8.0f,
                                              static_cast<float>(block_z) / 8.0f) *
        8.0);
    return temperature -
           (noise_value + static_cast<float>(block_y) - static_cast<float>(snow_level)) * 0.05f /
               40.0f;
  }
  return temperature;
}

// ---- the mutable per-column context (SurfaceRules.Context) -----------------
//
// `update_xz` runs once per column and `update_y` once per stone cell, exactly
// like vanilla. Only the two reads the context itself caches in vanilla are
// cached here (`surface_secondary` and the preliminary-surface interpolation);
// the conditions' own lazy caches are pure performance and are not kept (see
// the header comment).
struct SurfaceContext {
  struct CachedValue {
    int64_t last = std::numeric_limits<int64_t>::max();
    double value = 0.0;
  };

  SurfaceContext(const WgSurfaceChunk& chunk, const WgRandomState& state)
      : chunk(chunk), state(state) {}

  const WgSurfaceChunk& chunk;
  const WgRandomState& state;
  SurfaceNoises noises;
  std::array<uint8_t, 192> clay_bands{};
  int32_t min_y = 0;
  int32_t height = 0;
  int32_t sea_level = 0;
  WgPositionalFactory noise_random;
  ChunkBiomeManager biome_manager;

  uint8_t* codes = nullptr;
  int32_t heights[16][16] = {};

  int64_t last_update_xz = std::numeric_limits<int64_t>::min();
  int64_t last_update_y = std::numeric_limits<int64_t>::min();
  int32_t block_x = 0;
  int32_t block_z = 0;
  int32_t block_y = 0;
  int32_t surface_depth = 0;
  int32_t water_height = 0;
  int32_t stone_depth_below = 0;
  int32_t stone_depth_above = 0;

  int64_t secondary_last = std::numeric_limits<int64_t>::max();
  double secondary_value = 0.0;
  int64_t min_surface_last = std::numeric_limits<int64_t>::max();
  int32_t min_surface_value = 0;
  int64_t prelim_cell_x = std::numeric_limits<int64_t>::max();
  int64_t prelim_cell_z = std::numeric_limits<int64_t>::max();
  int32_t prelim[4] = {};
  bool biome_valid = false;
  const char* biome_cache = nullptr;
  std::unordered_map<int32_t, CachedValue> noise_2d;
  std::unordered_map<int32_t, CachedValue> noise_3d;

  void update_xz(int32_t x, int32_t z) {
    ++last_update_xz;
    ++last_update_y;
    block_x = x;
    block_z = z;
    surface_depth = compute_surface_depth(x, z);
  }

  void update_y(int32_t stone_above, int32_t stone_below, int32_t water, int32_t y) {
    ++last_update_y;
    biome_valid = false;
    block_y = y;
    water_height = water;
    stone_depth_below = stone_below;
    stone_depth_above = stone_above;
  }

  int32_t compute_surface_depth(int32_t x, int32_t z) const {
    const double noise_value = state.noise(noises.surface).get_value(x, 0.0, z);
    return static_cast<int32_t>(noise_value * 2.75 + 3.0 +
                                noise_random.at(x, 0, z).next_double() * 0.25);
  }

  double secondary() {
    if (secondary_last != last_update_xz) {
      secondary_last = last_update_xz;
      secondary_value = state.noise(noises.surface_secondary).get_value(block_x, 0.0, block_z);
    }
    return secondary_value;
  }

  const char* biome() {
    if (!biome_valid) {
      biome_cache = biome_manager.at(block_x, block_y, block_z);
      biome_valid = true;
    }
    return biome_cache;
  }

  double noise2d(int32_t index) {
    CachedValue& entry = noise_2d[index];
    if (entry.last != last_update_xz) {
      entry.last = last_update_xz;
      entry.value = state.noise(index, chunk.use_legacy_random).get_value(block_x, 0.0, block_z);
    }
    return entry.value;
  }

  double noise3d(int32_t index) {
    CachedValue& entry = noise_3d[index];
    if (entry.last != last_update_y) {
      entry.last = last_update_y;
      entry.value =
          state.noise(index, chunk.use_legacy_random).get_value(block_x, block_y, block_z);
    }
    return entry.value;
  }

  int32_t min_surface_level() {
    if (min_surface_last != last_update_xz) {
      min_surface_last = last_update_xz;
      const int32_t cell_x = block_x >> 4;
      const int32_t cell_z = block_z >> 4;
      if (prelim_cell_x != cell_x || prelim_cell_z != cell_z) {
        prelim_cell_x = cell_x;
        prelim_cell_z = cell_z;
        prelim[0] = chunk.preliminary_surface_level(chunk.user, cell_x * 16, cell_z * 16);
        prelim[1] = chunk.preliminary_surface_level(chunk.user, (cell_x + 1) * 16, cell_z * 16);
        prelim[2] = chunk.preliminary_surface_level(chunk.user, cell_x * 16, (cell_z + 1) * 16);
        prelim[3] =
            chunk.preliminary_surface_level(chunk.user, (cell_x + 1) * 16, (cell_z + 1) * 16);
      }
      const double lx = static_cast<double>(static_cast<float>(block_x & 0xF) / 16.0f);
      const double lz = static_cast<double>(static_cast<float>(block_z & 0xF) / 16.0f);
      const int32_t level = wg_floor(wg_lerp2(lx, lz, prelim[0], prelim[1], prelim[2], prelim[3]));
      min_surface_value = level + surface_depth - 8;
    }
    return min_surface_value;
  }

  size_t index_of(int32_t x, int32_t y, int32_t z) const {
    return (static_cast<size_t>(x) * 16 + static_cast<size_t>(z)) * static_cast<size_t>(height) +
           static_cast<size_t>(y - min_y);
  }

  uint8_t get(int32_t x, int32_t y, int32_t z) const {
    return codes[index_of(x, y, z)];
  }

  // `BlockColumn.setBlock`: the write and the WORLD_SURFACE_WG heightmap
  // update `ProtoChunk.setBlockState` performs (`Heightmap.update`'s rules:
  // raising needs a write at or above the stored top, lowering only happens
  // when the top itself becomes air).
  void set(int32_t x, int32_t y, int32_t z, uint8_t code) {
    codes[index_of(x, y, z)] = code;
    const int32_t first_available = heights[x][z] + 1;
    if (y <= first_available - 2)
      return;
    if (!wg_surface_code_is_air(code)) {
      if (y >= first_available)
        heights[x][z] = y;
      return;
    }
    if (first_available - 1 == y) {
      for (int32_t scan = y - 1; scan >= min_y; --scan) {
        if (!wg_surface_code_is_air(get(x, scan, z))) {
          heights[x][z] = scan;
          return;
        }
      }
      heights[x][z] = min_y - 1;
    }
  }

  int32_t height_at(int32_t x, int32_t z) const {
    return heights[x][z];
  }

  // `SurfaceSystem.getBand`: the 192-entry clay table indexed by y plus the
  // clay-bands-offset noise's rounded contribution.
  uint8_t band(int32_t x, int32_t y, int32_t z) const {
    const double noise_value = state.noise(noises.clay_bands_offset).get_value(x, 0.0, z);
    const int32_t offset = static_cast<int32_t>(std::floor(noise_value * 4.0 + 0.5));
    return clay_bands[static_cast<size_t>((y + offset + 192) % 192)];
  }
};

// ---- conditions (`SurfaceRules.ConditionSource`) ---------------------------

struct Condition {
  virtual ~Condition() = default;
  virtual bool test(SurfaceContext& context) const = 0;
};

struct NotCondition final : Condition {
  ConditionPtr target;
  explicit NotCondition(ConditionPtr target) : target(std::move(target)) {}
  bool test(SurfaceContext& context) const override {
    return !target->test(context);
  }
};

struct BiomeCondition final : Condition {
  // Interned pointers (`wg_canonical_biome`): `cond_is_biome` canonicals the
  // rule's names when the tree is built, and the biome manager canonicals what
  // the source answered, so this loop is `==` instead of a `strcmp` per cell.
  // That loop is the hottest comparison in the surface stage (a stone cell asks
  // it once per biome rule).
  std::vector<const char*> names;
  explicit BiomeCondition(std::vector<const char*> names) : names(std::move(names)) {}
  bool test(SurfaceContext& context) const override {
    const char* biome = context.biome();
    if (biome == nullptr)
      return false;
    for (const char* name : names) {
      if (biome == name)
        return true;
    }
    return false;
  }
};

struct NoiseCondition final : Condition {
  int32_t index = -1;
  double min_threshold = 0.0;
  double max_threshold = 0.0;
  bool is_3d = false;
  bool test(SurfaceContext& context) const override {
    if (index < 0)
      return false;
    const double value = is_3d ? context.noise3d(index) : context.noise2d(index);
    return value >= min_threshold && value <= max_threshold;
  }
};

struct YCondition final : Condition {
  Anchor anchor;
  int32_t multiplier = 0;
  bool add_stone_depth = false;
  bool test(SurfaceContext& context) const override {
    const int32_t y = context.block_y + (add_stone_depth ? context.stone_depth_above : 0);
    return y >= anchor.resolve(context.min_y, context.height) + context.surface_depth * multiplier;
  }
};

struct WaterCondition final : Condition {
  int32_t offset = 0;
  int32_t multiplier = 0;
  bool add_stone_depth = false;
  bool test(SurfaceContext& context) const override {
    return context.water_height == std::numeric_limits<int32_t>::min() ||
           context.block_y + (add_stone_depth ? context.stone_depth_above : 0) >=
               context.water_height + offset + context.surface_depth * multiplier;
  }
};

struct StoneDepthCondition final : Condition {
  int32_t offset = 0;
  bool add_surface_depth = false;
  int32_t secondary_depth_range = 0;
  bool ceiling = false;
  bool test(SurfaceContext& context) const override {
    const int32_t depth = ceiling ? context.stone_depth_below : context.stone_depth_above;
    const int32_t surface = add_surface_depth ? context.surface_depth : 0;
    const int32_t secondary =
        secondary_depth_range == 0
            ? 0
            : static_cast<int32_t>(wg_map(context.secondary(), -1.0, 1.0, 0.0,
                                          static_cast<double>(secondary_depth_range)));
    return depth <= 1 + offset + surface + secondary;
  }
};

struct VerticalGradientCondition final : Condition {
  Anchor true_at;
  Anchor false_at;
  WgPositionalFactory factory;
  bool legacy = false;
  int64_t legacy_seed = 0;
  bool test(SurfaceContext& context) const override {
    const int32_t true_y = true_at.resolve(context.min_y, context.height);
    const int32_t false_y = false_at.resolve(context.min_y, context.height);
    const int32_t y = context.block_y;
    if (y <= true_y)
      return true;
    if (y >= false_y)
      return false;
    const double probability = wg_map(static_cast<double>(y), static_cast<double>(true_y),
                                      static_cast<double>(false_y), 1.0, 0.0);
    if (legacy) {
      WgLegacyRandom random(
          static_cast<int64_t>(wg_mth_get_seed(context.block_x, y, context.block_z)) ^ legacy_seed);
      return static_cast<double>(random.next_float()) < probability;
    }
    WgRandom random = factory.at(context.block_x, y, context.block_z);
    return static_cast<double>(random.next_float()) < probability;
  }
};

struct SteepCondition final : Condition {
  // `SteepMaterialCondition`: the chunk's live WORLD_SURFACE_WG heights, not
  // the rule context's own column.
  bool test(SurfaceContext& context) const override {
    const int32_t chunk_x = context.block_x & 0xF;
    const int32_t chunk_z = context.block_z & 0xF;
    const int32_t z_north = std::max(chunk_z - 1, 0);
    const int32_t z_south = std::min(chunk_z + 1, 15);
    const int32_t height_north = context.height_at(chunk_x, z_north);
    const int32_t height_south = context.height_at(chunk_x, z_south);
    if (height_south >= height_north + 4)
      return true;
    const int32_t x_west = std::max(chunk_x - 1, 0);
    const int32_t x_east = std::min(chunk_x + 1, 15);
    const int32_t height_west = context.height_at(x_west, chunk_z);
    const int32_t height_east = context.height_at(x_east, chunk_z);
    return height_west >= height_east + 4;
  }
};

struct HoleCondition final : Condition {
  bool test(SurfaceContext& context) const override {
    return context.surface_depth <= 0;
  }
};

struct AbovePreliminarySurfaceCondition final : Condition {
  bool test(SurfaceContext& context) const override {
    return context.block_y >= context.min_surface_level();
  }
};

struct TemperatureCondition final : Condition {
  // `Context.TemperatureHelperCondition` -> `Biome.coldEnoughToSnow`.
  bool test(SurfaceContext& context) const override {
    const BiomeClimate* climate = climate_of(context.biome());
    if (climate == nullptr)
      return false;
    const float temperature = height_adjusted_temperature(
        context.block_x, context.block_y, context.block_z, context.sea_level, *climate);
    return !(temperature >= 0.15f);
  }
};

// ---- condition factories, named after `SurfaceRules`' own helpers ----------

ConditionPtr cond_not(ConditionPtr target) {
  return std::make_unique<NotCondition>(std::move(target));
}

ConditionPtr cond_is_biome(std::vector<const char*> names) {
  for (const char*& name : names)
    name = wg_canonical_biome(name);
  return std::make_unique<BiomeCondition>(std::move(names));
}

ConditionPtr cond_noise2d(const char* noise_name, double min_threshold, double max_threshold) {
  auto condition = std::make_unique<NoiseCondition>();
  condition->index = noise_index_by_name(noise_name);
  condition->min_threshold = min_threshold;
  condition->max_threshold = max_threshold;
  condition->is_3d = false;
  return condition;
}

ConditionPtr cond_noise3d(const char* noise_name, double min_threshold, double max_threshold) {
  auto condition = std::make_unique<NoiseCondition>();
  condition->index = noise_index_by_name(noise_name);
  condition->min_threshold = min_threshold;
  condition->max_threshold = max_threshold;
  condition->is_3d = true;
  return condition;
}

ConditionPtr cond_y_block(Anchor anchor, int32_t multiplier) {
  auto condition = std::make_unique<YCondition>();
  condition->anchor = anchor;
  condition->multiplier = multiplier;
  condition->add_stone_depth = false;
  return condition;
}

ConditionPtr cond_y_start(Anchor anchor, int32_t multiplier) {
  auto condition = std::make_unique<YCondition>();
  condition->anchor = anchor;
  condition->multiplier = multiplier;
  condition->add_stone_depth = true;
  return condition;
}

ConditionPtr cond_water_block(int32_t offset, int32_t multiplier) {
  auto condition = std::make_unique<WaterCondition>();
  condition->offset = offset;
  condition->multiplier = multiplier;
  condition->add_stone_depth = false;
  return condition;
}

ConditionPtr cond_water_start(int32_t offset, int32_t multiplier) {
  auto condition = std::make_unique<WaterCondition>();
  condition->offset = offset;
  condition->multiplier = multiplier;
  condition->add_stone_depth = true;
  return condition;
}

ConditionPtr cond_stone_depth(int32_t offset, bool add_surface_depth, int32_t secondary_range,
                              bool ceiling) {
  auto condition = std::make_unique<StoneDepthCondition>();
  condition->offset = offset;
  condition->add_surface_depth = add_surface_depth;
  condition->secondary_depth_range = secondary_range;
  condition->ceiling = ceiling;
  return condition;
}

ConditionPtr cond_vertical_gradient(const WgRandomState& state, const char* name, Anchor true_at,
                                    Anchor false_at) {
  auto condition = std::make_unique<VerticalGradientCondition>();
  condition->true_at = true_at;
  condition->false_at = false_at;
  condition->factory = state.named_random(name);
  return condition;
}

// The Nether's `legacy_random_source` builds the same named factory from
// `LegacyRandomSource(seed)`, whose `fromHashOf` is Java's `String.hashCode`
// rather than the Xoroshiro factory's MD5.
ConditionPtr cond_vertical_gradient_legacy(int64_t seed, const char* name, Anchor true_at,
                                           Anchor false_at) {
  auto condition = std::make_unique<VerticalGradientCondition>();
  condition->true_at = true_at;
  condition->false_at = false_at;
  condition->legacy = true;
  // Two forks, both of which consume one `nextLong()`: the root becomes the
  // legacy positional factory, then `getOrCreateRandomFactory` hashes the name
  // into a new source and forks that source into the condition's factory.
  WgLegacyRandom root(seed);
  WgLegacyPositionalFactory root_factory(root.next_long());
  WgLegacyRandom named = root_factory.from_hash_of(name);
  condition->legacy_seed = named.next_long();
  return condition;
}

ConditionPtr cond_steep() {
  return std::make_unique<SteepCondition>();
}
ConditionPtr cond_hole() {
  return std::make_unique<HoleCondition>();
}
ConditionPtr cond_above_preliminary_surface() {
  return std::make_unique<AbovePreliminarySurfaceCondition>();
}
ConditionPtr cond_temperature() {
  return std::make_unique<TemperatureCondition>();
}

ConditionPtr on_floor() {
  return cond_stone_depth(0, false, 0, false);
}
ConditionPtr under_floor() {
  return cond_stone_depth(0, true, 0, false);
}
ConditionPtr deep_under_floor() {
  return cond_stone_depth(0, true, 6, false);
}
ConditionPtr very_deep_under_floor() {
  return cond_stone_depth(0, true, 30, false);
}
ConditionPtr on_ceiling() {
  return cond_stone_depth(0, false, 0, true);
}
ConditionPtr under_ceiling() {
  return cond_stone_depth(0, true, 0, true);
}

// ---- rules (`SurfaceRules.RuleSource`) -------------------------------------

struct Rule {
  virtual ~Rule() = default;
  virtual std::optional<uint8_t> try_apply(SurfaceContext& context, int32_t x, int32_t y,
                                           int32_t z) const = 0;
};

struct StateRule final : Rule {
  uint8_t code = code_of(WgSurfaceBlock::kStone);
  explicit StateRule(uint8_t code) : code(code) {}
  std::optional<uint8_t> try_apply(SurfaceContext&, int32_t, int32_t, int32_t) const override {
    return code;
  }
};

struct TestRule final : Rule {
  ConditionPtr condition;
  RulePtr followup;
  TestRule(ConditionPtr condition, RulePtr followup)
      : condition(std::move(condition)), followup(std::move(followup)) {}
  std::optional<uint8_t> try_apply(SurfaceContext& context, int32_t x, int32_t y,
                                   int32_t z) const override {
    if (!condition->test(context))
      return std::nullopt;
    return followup->try_apply(context, x, y, z);
  }
};

struct SequenceRule final : Rule {
  std::vector<RulePtr> rules;
  explicit SequenceRule(std::vector<RulePtr> rules) : rules(std::move(rules)) {}
  std::optional<uint8_t> try_apply(SurfaceContext& context, int32_t x, int32_t y,
                                   int32_t z) const override {
    for (const RulePtr& rule : rules) {
      std::optional<uint8_t> state = rule->try_apply(context, x, y, z);
      if (state.has_value())
        return state;
    }
    return std::nullopt;
  }
};

struct BandlandsRule final : Rule {
  std::optional<uint8_t> try_apply(SurfaceContext& context, int32_t x, int32_t y,
                                   int32_t z) const override {
    return context.band(x, y, z);
  }
};

RulePtr rule_state(WgSurfaceBlock block) {
  return std::make_unique<StateRule>(code_of(block));
}

RulePtr rule_if(ConditionPtr condition, RulePtr followup) {
  return std::make_unique<TestRule>(std::move(condition), std::move(followup));
}

RulePtr rule_sequence(std::vector<RulePtr> rules) {
  return std::make_unique<SequenceRule>(std::move(rules));
}

RulePtr rule_bandlands() {
  return std::make_unique<BandlandsRule>();
}

// `SurfaceRules.sequence(...)` with the arguments spelled out; a
// `std::vector<std::unique_ptr<...>>` cannot be brace-initialised, so the
// variadic form is what keeps the tree readable.
template <typename... Rules> RulePtr rule_seq(Rules&&... rules) {
  std::vector<RulePtr> list;
  list.reserve(sizeof...(rules));
  (list.push_back(std::forward<Rules>(rules)), ...);
  return rule_sequence(std::move(list));
}

// ---- `SurfaceRuleData.overworldLike(biomes, true, false, true)` -------------
//
// The rule set's own order and nesting, with fresh condition objects at every
// use (vanilla's `ConditionSource.apply` does the same). The `surfaceNoiseAbove
// (t)` helper is `noiseCondition2d(Noises.SURFACE, t / 8.25, Double.MAX_VALUE)`.
RulePtr build_overworld_rules(const WgRandomState& state) {
  const auto biome = [](std::initializer_list<const char*> names) {
    return cond_is_biome(std::vector<const char*>(names));
  };
  const auto surface_noise_above = [](double threshold) {
    return cond_noise2d("minecraft:surface", threshold / 8.25, std::numeric_limits<double>::max());
  };
  const auto sand_or_sandstone_if_ceiling = [] {
    return rule_seq(rule_if(on_ceiling(), rule_state(WgSurfaceBlock::kSandstone)),
                    rule_state(WgSurfaceBlock::kSand));
  };
  const auto gravel_or_stone_if_ceiling = [] {
    return rule_seq(rule_if(on_ceiling(), rule_state(WgSurfaceBlock::kStone)),
                    rule_state(WgSurfaceBlock::kGravel));
  };
  const auto grass_or_dirt_if_underwater = [] {
    return rule_seq(rule_if(cond_water_block(0, 0), rule_state(WgSurfaceBlock::kGrassBlock)),
                    rule_state(WgSurfaceBlock::kDirt));
  };

  const auto sulfur_cave_bands = [] {
    return rule_seq(
        rule_if(cond_noise3d("minecraft:sulfur_cave_gradient", static_cast<double>(-0.4f),
                             static_cast<double>(-0.1f)),
                rule_state(WgSurfaceBlock::kCinnabar)),
        rule_if(cond_noise3d("minecraft:sulfur_cave_gradient", 0.0, static_cast<double>(0.4f)),
                rule_state(WgSurfaceBlock::kSulfur)),
        rule_if(cond_noise3d("minecraft:sulfur_cave_gradient", static_cast<double>(0.4f),
                             std::numeric_limits<double>::max()),
                rule_state(WgSurfaceBlock::kCinnabar)));
  };

  const auto common_surface_and_under_rules = [&] {
    return rule_seq(
        rule_if(biome({"minecraft:stony_peaks"}),
                rule_seq(rule_if(cond_noise2d("minecraft:calcite", -0.0125, 0.0125),
                                 rule_state(WgSurfaceBlock::kCalcite)),
                         rule_state(WgSurfaceBlock::kStone))),
        rule_if(biome({"minecraft:stony_shore"}),
                rule_seq(rule_if(cond_noise2d("minecraft:gravel", -0.05, 0.05),
                                 gravel_or_stone_if_ceiling()),
                         rule_state(WgSurfaceBlock::kStone))),
        rule_if(biome({"minecraft:windswept_hills"}),
                rule_if(surface_noise_above(1.0), rule_state(WgSurfaceBlock::kStone))),
        rule_if(biome({"minecraft:warm_ocean", "minecraft:beach", "minecraft:snowy_beach"}),
                sand_or_sandstone_if_ceiling()),
        rule_if(biome({"minecraft:desert"}), sand_or_sandstone_if_ceiling()),
        rule_if(biome({"minecraft:dripstone_caves"}), rule_state(WgSurfaceBlock::kStone)),
        rule_if(biome({"minecraft:sulfur_caves"}),
                rule_seq(sulfur_cave_bands(), rule_state(WgSurfaceBlock::kStone))));
  };

  const auto powder_snow_under_rule = [] {
    return rule_if(cond_noise2d("minecraft:powder_snow", 0.45, 0.58),
                   rule_if(cond_water_block(0, 0), rule_state(WgSurfaceBlock::kPowderSnow)));
  };
  const auto powder_snow_surface_rule = [] {
    return rule_if(cond_noise2d("minecraft:powder_snow", 0.35, 0.6),
                   rule_if(cond_water_block(0, 0), rule_state(WgSurfaceBlock::kPowderSnow)));
  };

  const auto biome_under_surface_rule = [&] {
    return rule_seq(
        rule_if(biome({"minecraft:frozen_peaks"}),
                rule_seq(rule_if(cond_steep(), rule_state(WgSurfaceBlock::kPackedIce)),
                         rule_if(cond_noise2d("minecraft:packed_ice", -0.5, 0.2),
                                 rule_state(WgSurfaceBlock::kPackedIce)),
                         rule_if(cond_noise2d("minecraft:ice", -0.0625, 0.025),
                                 rule_state(WgSurfaceBlock::kIce)),
                         rule_if(cond_water_block(0, 0), rule_state(WgSurfaceBlock::kSnowBlock)))),
        rule_if(biome({"minecraft:snowy_slopes"}),
                rule_seq(rule_if(cond_steep(), rule_state(WgSurfaceBlock::kStone)),
                         powder_snow_under_rule(),
                         rule_if(cond_water_block(0, 0), rule_state(WgSurfaceBlock::kSnowBlock)))),
        rule_if(biome({"minecraft:jagged_peaks"}), rule_state(WgSurfaceBlock::kStone)),
        rule_if(biome({"minecraft:grove"}),
                rule_seq(powder_snow_under_rule(), rule_state(WgSurfaceBlock::kDirt))),
        common_surface_and_under_rules(),
        rule_if(biome({"minecraft:windswept_savanna"}),
                rule_if(surface_noise_above(1.75), rule_state(WgSurfaceBlock::kStone))),
        rule_if(biome({"minecraft:windswept_gravelly_hills"}),
                rule_seq(rule_if(surface_noise_above(2.0), gravel_or_stone_if_ceiling()),
                         rule_if(surface_noise_above(1.0), rule_state(WgSurfaceBlock::kStone)),
                         rule_if(surface_noise_above(-1.0), rule_state(WgSurfaceBlock::kDirt)),
                         gravel_or_stone_if_ceiling())),
        rule_if(biome({"minecraft:mangrove_swamp"}), rule_state(WgSurfaceBlock::kMud)),
        rule_state(WgSurfaceBlock::kDirt));
  };

  const auto biome_surface_rule = [&] {
    return rule_seq(
        rule_if(biome({"minecraft:frozen_peaks"}),
                rule_seq(rule_if(cond_steep(), rule_state(WgSurfaceBlock::kPackedIce)),
                         rule_if(cond_noise2d("minecraft:packed_ice", 0.0, 0.2),
                                 rule_state(WgSurfaceBlock::kPackedIce)),
                         rule_if(cond_noise2d("minecraft:ice", 0.0, 0.025),
                                 rule_state(WgSurfaceBlock::kIce)),
                         rule_if(cond_water_block(0, 0), rule_state(WgSurfaceBlock::kSnowBlock)))),
        rule_if(biome({"minecraft:snowy_slopes"}),
                rule_seq(rule_if(cond_steep(), rule_state(WgSurfaceBlock::kStone)),
                         powder_snow_surface_rule(),
                         rule_if(cond_water_block(0, 0), rule_state(WgSurfaceBlock::kSnowBlock)))),
        rule_if(biome({"minecraft:jagged_peaks"}),
                rule_seq(rule_if(cond_steep(), rule_state(WgSurfaceBlock::kStone)),
                         rule_if(cond_water_block(0, 0), rule_state(WgSurfaceBlock::kSnowBlock)))),
        rule_if(biome({"minecraft:grove"}),
                rule_seq(powder_snow_surface_rule(),
                         rule_if(cond_water_block(0, 0), rule_state(WgSurfaceBlock::kSnowBlock)))),
        common_surface_and_under_rules(),
        rule_if(
            biome({"minecraft:windswept_savanna"}),
            rule_seq(rule_if(surface_noise_above(1.75), rule_state(WgSurfaceBlock::kStone)),
                     rule_if(surface_noise_above(-0.5), rule_state(WgSurfaceBlock::kCoarseDirt)))),
        rule_if(biome({"minecraft:windswept_gravelly_hills"}),
                rule_seq(rule_if(surface_noise_above(2.0), gravel_or_stone_if_ceiling()),
                         rule_if(surface_noise_above(1.0), rule_state(WgSurfaceBlock::kStone)),
                         rule_if(surface_noise_above(-1.0), grass_or_dirt_if_underwater()),
                         gravel_or_stone_if_ceiling())),
        rule_if(
            biome({"minecraft:old_growth_pine_taiga", "minecraft:old_growth_spruce_taiga"}),
            rule_seq(rule_if(surface_noise_above(1.75), rule_state(WgSurfaceBlock::kCoarseDirt)),
                     rule_if(surface_noise_above(-0.95), rule_state(WgSurfaceBlock::kPodzol)))),
        rule_if(biome({"minecraft:ice_spikes"}),
                rule_if(cond_water_block(0, 0), rule_state(WgSurfaceBlock::kSnowBlock))),
        rule_if(biome({"minecraft:mangrove_swamp"}), rule_state(WgSurfaceBlock::kMud)),
        rule_if(biome({"minecraft:mushroom_fields"}), rule_state(WgSurfaceBlock::kMycelium)),
        grass_or_dirt_if_underwater());
  };

  const auto clay_band_1 = [] { return cond_noise2d("minecraft:surface", -0.909, -0.5454); };
  const auto clay_band_2 = [] { return cond_noise2d("minecraft:surface", -0.1818, 0.1818); };
  const auto clay_band_3 = [] { return cond_noise2d("minecraft:surface", 0.5454, 0.909); };
  const auto badlands_mid = [] { return cond_y_start(absolute(74), 1); };

  // `mainRuleCloseToSurface`: badge the swamp/badlands puddles and bands, then
  // the biome surface rules. Built as a vector because it is twelve to
  // fourteen rules long depending on how the sequences are counted.
  std::vector<RulePtr> main_rules;
  main_rules.push_back(rule_if(
      on_floor(),
      rule_seq(
          rule_if(biome({"minecraft:wooded_badlands"}),
                  rule_if(cond_y_block(absolute(97), 2),
                          rule_seq(rule_if(clay_band_1(), rule_state(WgSurfaceBlock::kCoarseDirt)),
                                   rule_if(clay_band_2(), rule_state(WgSurfaceBlock::kCoarseDirt)),
                                   rule_if(clay_band_3(), rule_state(WgSurfaceBlock::kCoarseDirt)),
                                   grass_or_dirt_if_underwater()))),
          rule_if(biome({"minecraft:swamp"}),
                  rule_if(cond_y_block(absolute(62), 0),
                          rule_if(cond_not(cond_y_block(absolute(63), 0)),
                                  rule_if(cond_noise2d("minecraft:surface_swamp", 0.0,
                                                       std::numeric_limits<double>::max()),
                                          rule_state(WgSurfaceBlock::kWater))))),
          rule_if(biome({"minecraft:mangrove_swamp"}),
                  rule_if(cond_y_block(absolute(60), 0),
                          rule_if(cond_not(cond_y_block(absolute(63), 0)),
                                  rule_if(cond_noise2d("minecraft:surface_swamp", 0.0,
                                                       std::numeric_limits<double>::max()),
                                          rule_state(WgSurfaceBlock::kWater))))))));

  // The badlands branch, in `SurfaceRuleData`'s order: the top terracotta
  // levels, the mid-band rules (`bandlands()` over the clay table), the sand
  // cap, and the deep under-floor white terracotta.
  main_rules.push_back(rule_if(
      biome({"minecraft:badlands", "minecraft:eroded_badlands", "minecraft:wooded_badlands"}),
      rule_seq(
          rule_if(
              on_floor(),
              rule_seq(
                  rule_if(cond_y_block(absolute(256), 0),
                          rule_state(WgSurfaceBlock::kOrangeTerracotta)),
                  rule_if(badlands_mid(),
                          rule_seq(rule_if(clay_band_1(), rule_state(WgSurfaceBlock::kTerracotta)),
                                   rule_if(clay_band_2(), rule_state(WgSurfaceBlock::kTerracotta)),
                                   rule_if(clay_band_3(), rule_state(WgSurfaceBlock::kTerracotta)),
                                   rule_bandlands())),
                  rule_if(cond_water_block(-1, 0),
                          rule_seq(rule_if(on_ceiling(), rule_state(WgSurfaceBlock::kRedSandstone)),
                                   rule_state(WgSurfaceBlock::kRedSand))),
                  rule_if(cond_not(cond_hole()), rule_state(WgSurfaceBlock::kOrangeTerracotta)),
                  rule_if(cond_water_start(-6, -1), rule_state(WgSurfaceBlock::kWhiteTerracotta)),
                  gravel_or_stone_if_ceiling())),
          rule_if(cond_y_start(absolute(63), -1),
                  rule_seq(rule_if(cond_y_block(absolute(63), 0),
                                   rule_if(cond_not(badlands_mid()),
                                           rule_state(WgSurfaceBlock::kOrangeTerracotta))),
                           rule_bandlands())),
          rule_if(under_floor(), rule_if(cond_water_start(-6, -1),
                                         rule_state(WgSurfaceBlock::kWhiteTerracotta))))));

  // The floor rule: underwater surface material (the frozen-ocean hole ICE
  // special case first), then the biome surface rule.
  main_rules.push_back(rule_if(
      on_floor(),
      rule_if(
          cond_water_block(-1, 0),
          rule_seq(rule_if(biome({"minecraft:frozen_ocean", "minecraft:deep_frozen_ocean"}),
                           rule_if(cond_hole(), rule_seq(rule_if(cond_water_block(0, 0),
                                                                 rule_state(WgSurfaceBlock::kAir)),
                                                         rule_if(cond_temperature(),
                                                                 rule_state(WgSurfaceBlock::kIce)),
                                                         rule_state(WgSurfaceBlock::kWater)))),
                   biome_surface_rule()))));

  // The deep branch: lake floors, under-floor materials and the deep sandstone
  // of the sand biomes.
  main_rules.push_back(rule_if(
      cond_water_start(-6, -1),
      rule_seq(rule_if(on_floor(),
                       rule_if(biome({"minecraft:frozen_ocean", "minecraft:deep_frozen_ocean"}),
                               rule_if(cond_hole(), rule_state(WgSurfaceBlock::kWater)))),
               rule_if(under_floor(), biome_under_surface_rule()),
               rule_if(biome({"minecraft:warm_ocean", "minecraft:beach", "minecraft:snowy_beach"}),
                       rule_if(deep_under_floor(), rule_state(WgSurfaceBlock::kSandstone))),
               rule_if(biome({"minecraft:desert"}),
                       rule_if(very_deep_under_floor(), rule_state(WgSurfaceBlock::kSandstone))))));

  // The last dispenser: bare rock for the peaks, sand for the warm oceans, and
  // gravel/stone everywhere else.
  main_rules.push_back(rule_if(
      on_floor(), rule_seq(rule_if(biome({"minecraft:frozen_peaks", "minecraft:jagged_peaks"}),
                                   rule_state(WgSurfaceBlock::kStone)),
                           rule_if(biome({"minecraft:warm_ocean", "minecraft:lukewarm_ocean",
                                          "minecraft:deep_lukewarm_ocean"}),
                                   sand_or_sandstone_if_ceiling()),
                           gravel_or_stone_if_ceiling())));

  RulePtr main_rule = rule_sequence(std::move(main_rules));

  std::vector<RulePtr> top;
  top.push_back(rule_if(
      cond_vertical_gradient(state, "minecraft:bedrock_floor", above_bottom(0), above_bottom(5)),
      rule_state(WgSurfaceBlock::kBedrock)));
  top.push_back(rule_if(cond_above_preliminary_surface(), std::move(main_rule)));
  top.push_back(rule_if(biome({"minecraft:sulfur_caves"}), sulfur_cave_bands()));
  top.push_back(
      rule_if(cond_vertical_gradient(state, "minecraft:deepslate", absolute(0), absolute(8)),
              rule_state(WgSurfaceBlock::kDeepslate)));
  return rule_sequence(std::move(top));
}

// ---- `SurfaceRuleData.nether()` -------------------------------------------
//
// The Nether tree is a separate shape, not a parameter of the overworld one:
// bedrock at both the floor and the roof, netherrack above the roof margin, and
// four biome branches whose ceiling/floor rules write soul soil, basalt,
// blackstone, gravel, the two nyliums and their wart blocks. The `y_above`
// thresholds are absolute block heights (30..35), exactly as in
// `NoiseGeneratorSettings.NETHER`'s JSON.
RulePtr build_nether_rules(const WgRandomState& state, int64_t seed) {
  (void)state;
  const auto biome = [](std::initializer_list<const char*> names) {
    return cond_is_biome(std::vector<const char*>(names));
  };
  const auto noise = [](const char* name, double min_threshold) {
    return cond_noise2d(name, min_threshold, std::numeric_limits<double>::max());
  };

  const auto basalt_floor = [&] {
    return rule_seq(
        rule_if(
            noise("minecraft:patch", -0.012),
            rule_if(cond_y_start(absolute(30), 0), rule_if(cond_not(cond_y_start(absolute(35), 0)),
                                                           rule_state(WgSurfaceBlock::kGravel)))),
        rule_if(noise("minecraft:nether_state_selector", 0.0), rule_state(WgSurfaceBlock::kBasalt)),
        rule_state(WgSurfaceBlock::kBlackstone));
  };

  const auto soul_floor = [&] {
    return rule_seq(rule_if(noise("minecraft:patch", -0.012),
                            rule_if(cond_y_start(absolute(30), 0),
                                    rule_if(cond_not(cond_y_start(absolute(35), 0)),
                                            rule_state(WgSurfaceBlock::kGravel)))),
                    rule_if(noise("minecraft:nether_state_selector", 0.0),
                            rule_state(WgSurfaceBlock::kSoulSand)),
                    rule_state(WgSurfaceBlock::kSoulSoil));
  };

  const auto forest = [&](const char* biome_name, WgSurfaceBlock wart, WgSurfaceBlock nylium) {
    return rule_if(
        biome({biome_name}),
        rule_if(cond_not(noise("minecraft:netherrack", 0.54)),
                rule_if(cond_y_block(absolute(31), 0),
                        rule_seq(rule_if(noise("minecraft:nether_wart", 1.17), rule_state(wart)),
                                 rule_state(nylium)))));
  };

  // The nether-wastes branch nests its own floor-depth pair under the shared
  // floor gate: the soul-sand layer is the surface-depth pass, the gravel layer
  // the bare-floor pass.
  const auto nether_wastes = [&] {
    return rule_seq(
        rule_if(under_floor(),
                rule_if(noise("minecraft:soul_sand_layer", -0.012),
                        rule_seq(rule_if(cond_not(cond_hole()),
                                         rule_if(cond_y_start(absolute(30), 0),
                                                 rule_if(cond_not(cond_y_start(absolute(35), 0)),
                                                         rule_state(WgSurfaceBlock::kSoulSand)))),
                                 rule_state(WgSurfaceBlock::kNetherrack)))),
        rule_if(on_floor(),
                rule_if(cond_y_block(absolute(31), 0),
                        rule_if(cond_not(cond_y_start(absolute(35), 0)),
                                rule_if(noise("minecraft:gravel_layer", -0.012),
                                        rule_seq(rule_if(cond_y_block(absolute(32), 0),
                                                         rule_state(WgSurfaceBlock::kGravel)),
                                                 rule_if(cond_not(cond_hole()),
                                                         rule_state(WgSurfaceBlock::kGravel))))))));
  };

  auto floor = rule_seq(rule_if(cond_not(cond_y_block(absolute(32), 0)),
                                rule_if(cond_hole(), rule_state(WgSurfaceBlock::kLava))),
                        forest("minecraft:warped_forest", WgSurfaceBlock::kWarpedWartBlock,
                               WgSurfaceBlock::kWarpedNylium),
                        forest("minecraft:crimson_forest", WgSurfaceBlock::kNetherWartBlock,
                               WgSurfaceBlock::kCrimsonNylium),
                        rule_if(biome({"minecraft:nether_wastes"}), nether_wastes()));

  return rule_seq(
      rule_if(cond_vertical_gradient_legacy(seed, "minecraft:bedrock_floor", above_bottom(0),
                                            above_bottom(5)),
              rule_state(WgSurfaceBlock::kBedrock)),
      rule_if(cond_not(cond_vertical_gradient_legacy(seed, "minecraft:bedrock_roof", below_top(5),
                                                     below_top(0))),
              rule_state(WgSurfaceBlock::kBedrock)),
      rule_if(cond_y_block(below_top(5), 0), rule_state(WgSurfaceBlock::kNetherrack)),
      rule_if(biome({"minecraft:basalt_deltas"}),
              rule_seq(rule_if(under_ceiling(), rule_state(WgSurfaceBlock::kBasalt)),
                       rule_if(under_floor(), basalt_floor()))),
      rule_if(biome({"minecraft:soul_sand_valley"}),
              rule_seq(rule_if(under_ceiling(),
                               rule_seq(rule_if(noise("minecraft:nether_state_selector", 0.0),
                                                rule_state(WgSurfaceBlock::kSoulSand)),
                                        rule_state(WgSurfaceBlock::kSoulSoil))),
                       rule_if(under_floor(), soul_floor()))),
      rule_if(on_floor(), std::move(floor)), rule_state(WgSurfaceBlock::kNetherrack));
}

// ---- `SurfaceSystem`'s own tables and column helpers ----------------------

void make_bands(WgRandom& random, std::array<uint8_t, 192>& bands, int32_t base_width,
                WgSurfaceBlock state) {
  const int32_t band_count = random.next_int(15 - 6 + 1) + 6;
  for (int32_t i = 0; i < band_count; ++i) {
    const int32_t width = base_width + random.next_int(3);
    const int32_t start = random.next_int(192);
    for (int32_t p = 0; start + p < 192 && p < width; ++p) {
      bands[static_cast<size_t>(start + p)] = code_of(state);
    }
  }
}

// `SurfaceSystem.generateBands`, drawn from `noiseRandom.fromHashOf(
// "minecraft:clay_bands")` as a plain `RandomSource` (not forked). The
// identifier's string form carries its namespace, and that string is what the
// positional factory hashes.
std::array<uint8_t, 192> generate_clay_bands(WgRandom& random) {
  std::array<uint8_t, 192> bands{};
  bands.fill(code_of(WgSurfaceBlock::kTerracotta));
  for (int32_t i = 0; i < 192; ++i) {
    i += random.next_int(5) + 1;
    if (i >= 192)
      continue;
    bands[static_cast<size_t>(i)] = code_of(WgSurfaceBlock::kOrangeTerracotta);
  }
  make_bands(random, bands, 1, WgSurfaceBlock::kYellowTerracotta);
  make_bands(random, bands, 2, WgSurfaceBlock::kBrownTerracotta);
  make_bands(random, bands, 1, WgSurfaceBlock::kRedTerracotta);
  const int32_t white_band_count = random.next_int(15 - 9 + 1) + 9;
  int32_t placed = 0;
  for (int32_t start = 0; placed < white_band_count && start < 192;
       ++placed, start += random.next_int(16) + 4) {
    bands[static_cast<size_t>(start)] = code_of(WgSurfaceBlock::kWhiteTerracotta);
    if (start - 1 > 0 && random.next_boolean()) {
      bands[static_cast<size_t>(start - 1)] = code_of(WgSurfaceBlock::kLightGrayTerracotta);
    }
    if (start + 1 >= 192 || !random.next_boolean())
      continue;
    bands[static_cast<size_t>(start + 1)] = code_of(WgSurfaceBlock::kLightGrayTerracotta);
  }
  return bands;
}

// The chunk's WORLD_SURFACE_WG heightmap as `fill_from_noise` left it: the top
// non-air cell of each column (min_y - 1 when a column is empty, which matches
// `Heightmap.getHeight`'s `firstAvailable - 1`).
void init_heights(SurfaceContext& context) {
  for (int32_t x = 0; x < 16; ++x) {
    for (int32_t z = 0; z < 16; ++z) {
      int32_t top = context.min_y - 1;
      for (int32_t y = context.min_y + context.height - 1; y >= context.min_y; --y) {
        if (context.get(x, y, z) != code_of(WgSurfaceBlock::kAir)) {
          top = y;
          break;
        }
      }
      context.heights[x][z] = top;
    }
  }
}

bool is_stone_code(uint8_t code) {
  return !wg_surface_code_is_air(code) && code != code_of(WgSurfaceBlock::kWater) &&
         code != code_of(WgSurfaceBlock::kLava);
}

// `SurfaceSystem.erodedBadlandsExtension`: the stone pillars the eroded
// badlands biome grows above its surface noise. The first loop is only the
// water veto; the raising loop restarts at `startY` exactly as the source does.
// The noise samples are world coordinates, the reads and writes local ones.
void eroded_badlands_extension(SurfaceContext& context, int32_t local_x, int32_t local_z,
                               int32_t block_x, int32_t block_z, int32_t height) {
  const double pillar_buffer = std::min(
      std::abs(
          context.state.noise(context.noises.badlands_surface).get_value(block_x, 0.0, block_z) *
          8.25),
      context.state.noise(context.noises.badlands_pillar)
              .get_value(block_x * 0.2, 0.0, block_z * 0.2) *
          15.0);
  if (pillar_buffer <= 0.0)
    return;
  const double pillar_floor = std::abs(context.state.noise(context.noises.badlands_pillar_roof)
                                           .get_value(block_x * 0.75, 0.0, block_z * 0.75) *
                                       1.5);
  const double extension_top =
      64.0 + std::min(pillar_buffer * pillar_buffer * 2.5, std::ceil(pillar_floor * 50.0) + 24.0);
  const int32_t start_y = wg_floor(extension_top);
  if (height > start_y)
    return;
  for (int32_t scan = start_y; scan >= context.min_y; --scan) {
    const uint8_t old = context.get(local_x, scan, local_z);
    if (old == code_of(WgSurfaceBlock::kStone))
      break;
    if (old == code_of(WgSurfaceBlock::kWater))
      return;
  }
  for (int32_t scan = start_y; scan >= context.min_y &&
                               context.get(local_x, scan, local_z) == code_of(WgSurfaceBlock::kAir);
       --scan) {
    context.set(local_x, scan, local_z, code_of(WgSurfaceBlock::kStone));
  }
}

// `SurfaceSystem.frozenOceanExtension`: icebergs, including the snow cap whose
// depth comes from the position's own random stream. The branch order of the
// two `random.nextDouble()` gates is part of the draw sequence and is kept;
// the second gate places ice when the draw is *above* 0.15, the 15% water
// gaps an iceberg's interior is full of.
void frozen_ocean_extension(SurfaceContext& context, const char* biome_name, int32_t local_x,
                            int32_t local_z, int32_t block_x, int32_t block_z, int32_t height) {
  const double iceberg = std::min(
      std::abs(
          context.state.noise(context.noises.iceberg_surface).get_value(block_x, 0.0, block_z) *
          8.25),
      context.state.noise(context.noises.iceberg_pillar)
              .get_value(block_x * 1.28, 0.0, block_z * 1.28) *
          15.0);
  if (iceberg <= 1.8)
    return;
  const double iceberg_roof = std::abs(context.state.noise(context.noises.iceberg_pillar_roof)
                                           .get_value(block_x * 1.17, 0.0, block_z * 1.17) *
                                       1.5);
  double top = std::min(iceberg * iceberg * 1.2, std::ceil(iceberg_roof * 40.0) + 14.0);
  const BiomeClimate* climate = climate_of(biome_name);
  if (climate != nullptr) {
    const float temperature = height_adjusted_temperature(block_x, context.sea_level, block_z,
                                                          context.sea_level, *climate);
    if (temperature > 0.1f)
      top -= 2.0;
  }
  double extension_bottom;
  if (top > 2.0) {
    extension_bottom = static_cast<double>(context.sea_level) - top - 7.0;
    top += static_cast<double>(context.sea_level);
  } else {
    top = 0.0;
    extension_bottom = 0.0;
  }
  const double extension_top = top;
  WgRandom random = context.noise_random.at(block_x, 0, block_z);
  const int32_t max_snow_depth = 2 + random.next_int(4);
  const int32_t min_snow_height = context.sea_level + 18 + random.next_int(10);
  int32_t snow_depth = 0;
  for (int32_t y = std::max(height, static_cast<int32_t>(extension_top) + 1);
       y >= context.min_surface_level(); --y) {
    const uint8_t block = context.get(local_x, y, local_z);
    bool place = false;
    if (block == code_of(WgSurfaceBlock::kAir) && y < static_cast<int32_t>(extension_top)) {
      place = random.next_double() > 0.01;
    }
    if (!place && block == code_of(WgSurfaceBlock::kWater)) {
      // Vanilla's guard is `if (<air place> && (!water || y <= bottom ||
      // y >= sea || bottom == 0 || !(rnd > 0.15))) continue;`, so a water cell is
      // placed only when every disjunct is false: strictly inside the iceberg's
      // vertical span (`bottom < y < sea`), a real iceberg (`bottom != 0`), and
      // the 85% draw. The draws are consumed exactly in that span, which is why
      // the short-circuit order below is the source's own.
      if (extension_bottom != 0.0 && y > extension_bottom && y < context.sea_level)
        place = random.next_double() > 0.15;
    }
    if (!place)
      continue;
    if (snow_depth <= max_snow_depth && y > min_snow_height) {
      context.set(local_x, y, local_z, code_of(WgSurfaceBlock::kSnowBlock));
      ++snow_depth;
      continue;
    }
    context.set(local_x, y, local_z, code_of(WgSurfaceBlock::kPackedIce));
  }
}

// `DimensionType.WAY_BELOW_MIN_Y`, the "no stone below found" sentinel.
constexpr int32_t kWayBelowMinY = -2032 << 4;

// The shared tree's root, dereferenced where the tree is incomplete (its
// definition is below, after the rule types it owns). Null passes through, so
// the callers read "the caller's tree, or build one".
const Rule* rule_root_of(const WgSurfaceRuleTree* tree);

// `SurfaceSystem.buildSurface`'s column loop, over one chunk.
void apply_surface(const WgRandomState& state, int64_t seed, const WgSurfaceChunk& chunk,
                   const WgSurfaceRuleTree* tree, bool nether) {
  SurfaceContext context{chunk, state};
  context.noises.resolve();
  context.min_y = chunk.min_y;
  context.height = chunk.height;
  context.sea_level = chunk.sea_level;
  context.noise_random = state.root_random();
  context.biome_manager = ChunkBiomeManager{&chunk, obfuscate_seed(seed)};
  const size_t cells = static_cast<size_t>(16) * 16 * static_cast<size_t>(chunk.height);
  if (chunk.out != chunk.codes)
    std::memcpy(chunk.out, chunk.codes, cells);
  context.codes = chunk.out;
  WgRandom bands_random = context.noise_random.from_hash_of("minecraft:clay_bands");
  context.clay_bands = generate_clay_bands(bands_random);
  init_heights(context);
  // The caller's shared tree when it has one; a fresh tree otherwise.
  RulePtr own_rules;
  const Rule* rules = nullptr;
  rules = rule_root_of(tree);
  if (rules == nullptr) {
    own_rules = nether ? build_nether_rules(state, seed) : build_overworld_rules(state);
    rules = own_rules.get();
  }

  const std::optional<uint8_t> air = code_of(WgSurfaceBlock::kAir);
  const std::optional<uint8_t> water = code_of(WgSurfaceBlock::kWater);
  const std::optional<uint8_t> lava = code_of(WgSurfaceBlock::kLava);
  const std::optional<uint8_t> stone = code_of(WgSurfaceBlock::kStone);
  const int32_t base_x = chunk.chunk_x * 16;
  const int32_t base_z = chunk.chunk_z * 16;
  for (int32_t x = 0; x < 16; ++x) {
    for (int32_t z = 0; z < 16; ++z) {
      const int32_t block_x = base_x + x;
      const int32_t block_z = base_z + z;
      const int32_t starting_height = context.heights[x][z] + 1;
      const int32_t biome_y = chunk.use_legacy_random ? 0 : starting_height;
      const char* surface_biome = context.biome_manager.at(block_x, biome_y, block_z);
      if (surface_biome != nullptr &&
          std::strcmp(surface_biome, "minecraft:eroded_badlands") == 0) {
        eroded_badlands_extension(context, x, z, block_x, block_z, starting_height);
      }
      const int32_t height = context.heights[x][z] + 1;
      context.update_xz(block_x, block_z);
      int32_t stone_above_depth = 0;
      int32_t water_height = std::numeric_limits<int32_t>::min();
      int32_t next_ceiling_stone = std::numeric_limits<int32_t>::max();
      for (int32_t y = height; y >= context.min_y; --y) {
        const uint8_t old = context.get(x, y, z);
        if (old == *air) {
          stone_above_depth = 0;
          water_height = std::numeric_limits<int32_t>::min();
          continue;
        }
        if (old == *water || old == *lava) {
          if (water_height != std::numeric_limits<int32_t>::min())
            continue;
          water_height = y + 1;
          continue;
        }
        if (next_ceiling_stone >= y) {
          next_ceiling_stone = kWayBelowMinY;
          for (int32_t look = y - 1; look >= context.min_y - 1; --look) {
            const bool is_stone = look >= context.min_y && is_stone_code(context.get(x, look, z));
            if (is_stone)
              continue;
            next_ceiling_stone = look + 1;
            break;
          }
        }
        const int32_t stone_below_depth = y - next_ceiling_stone + 1;
        context.update_y(++stone_above_depth, stone_below_depth, water_height, y);
        if (old != *stone)
          continue;
        const std::optional<uint8_t> replaced = rules->try_apply(context, block_x, y, block_z);
        if (replaced.has_value())
          context.set(x, y, z, *replaced);
      }
      if (surface_biome != nullptr &&
          (std::strcmp(surface_biome, "minecraft:frozen_ocean") == 0 ||
           std::strcmp(surface_biome, "minecraft:deep_frozen_ocean") == 0)) {
        frozen_ocean_extension(context, surface_biome, x, z, block_x, block_z, starting_height);
      }
    }
  }
}

// `SurfaceSystem.topMaterial`'s body: the chunk's rule tree plus the mutable
// context the carver asks one cell at a time. `apply_surface` builds the same
// two things for its column pass; this is the same construction without the
// column loop, so a re-top sees exactly the context a column would have.
struct TopMaterialState {
  SurfaceContext context;
  RulePtr own_rules;
  const Rule* rules = nullptr;

  TopMaterialState(const WgRandomState& state, int64_t seed, const WgSurfaceChunk& chunk,
                   const WgSurfaceRuleTree* tree)
      : context(chunk, state) {
    context.noises.resolve();
    context.min_y = chunk.min_y;
    context.height = chunk.height;
    context.sea_level = chunk.sea_level;
    context.noise_random = state.root_random();
    context.biome_manager = ChunkBiomeManager{&chunk, obfuscate_seed(seed)};
    context.codes = chunk.out;
    WgRandom bands_random = context.noise_random.from_hash_of("minecraft:clay_bands");
    context.clay_bands = generate_clay_bands(bands_random);
    rules = rule_root_of(tree);
    if (rules == nullptr) {
      own_rules = build_overworld_rules(state);
      rules = own_rules.get();
    }
  }

  bool at(int32_t block_x, int32_t block_y, int32_t block_z, bool under_fluid, uint8_t* out) {
    context.update_xz(block_x, block_z);
    context.update_y(1, 1, under_fluid ? block_y + 1 : std::numeric_limits<int32_t>::min(),
                     block_y);
    const std::optional<uint8_t> state = rules->try_apply(context, block_x, block_y, block_z);
    if (!state.has_value())
      return false;
    *out = *state;
    return true;
  }
};

} // namespace

// The rule tree's one owning handle, so the same built tree can be shared by
// every chunk a generator produces.
struct WgSurfaceRuleTree {
  RulePtr root;
};

std::shared_ptr<const WgSurfaceRuleTree> wg_build_overworld_rules(const WgRandomState& state) {
  auto tree = std::make_shared<WgSurfaceRuleTree>();
  tree->root = build_overworld_rules(state);
  return tree;
}

std::shared_ptr<const WgSurfaceRuleTree> wg_build_nether_rules(const WgRandomState& state,
                                                               int64_t seed) {
  auto tree = std::make_shared<WgSurfaceRuleTree>();
  tree->root = build_nether_rules(state, seed);
  return tree;
}

namespace {

const Rule* rule_root_of(const WgSurfaceRuleTree* tree) {
  return tree != nullptr ? tree->root.get() : nullptr;
}

} // namespace

struct WgSurfaceTopMaterial::Impl {
  TopMaterialState state;

  Impl(const WgRandomState& state_in, int64_t seed, const WgSurfaceChunk& chunk,
       const WgSurfaceRuleTree* rules)
      : state(state_in, seed, chunk, rules) {}
};

WgSurfaceTopMaterial::WgSurfaceTopMaterial(const WgRandomState& state, int64_t seed,
                                           const WgSurfaceChunk& chunk,
                                           const WgSurfaceRuleTree* rules)
    : impl_(std::make_unique<Impl>(state, seed, chunk, rules)) {}

WgSurfaceTopMaterial::~WgSurfaceTopMaterial() = default;

bool WgSurfaceTopMaterial::at(int32_t block_x, int32_t block_y, int32_t block_z, bool under_fluid,
                              uint8_t* out) {
  return impl_->state.at(block_x, block_y, block_z, under_fluid, out);
}

// `BiomeManager.obfuscateSeed`: `Hashing.sha256().hashLong(seed).asLong()`.
// Guava writes the long little-endian and `HashCode.asLong` reads the first
// eight bytes back little-endian; both were checked against the game's own
// answer for seed 42 (see the surface fixture's meta row).
//
// Public because it is also what a client is told: Join Game carries this hash,
// not the seed itself (`ServerPlayer.createCommonSpawnInfo`), and the client's
// own biome manager samples its unloaded-chunk fallback through it.
int64_t obfuscate_seed(int64_t seed) {
  unsigned char input[8];
  for (int i = 0; i < 8; ++i) {
    input[i] = static_cast<unsigned char>((static_cast<uint64_t>(seed) >> (8 * i)) & 0xFF);
  }
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (ctx == nullptr || EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(ctx, input, sizeof(input)) != 1 ||
      EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1 || digest_len < 8) {
    EVP_MD_CTX_free(ctx);
    return 0;
  }
  EVP_MD_CTX_free(ctx);
  uint64_t out = 0;
  for (int i = 0; i < 8; ++i) {
    out |= static_cast<uint64_t>(digest[i]) << (8 * i);
  }
  return static_cast<int64_t>(out);
}

const WgPerlinSimplexNoise& wg_biome_info_noise() {
  return legacy_noises().info;
}

const WgSurfaceState& wg_surface_state(uint8_t code) {
  return kSurfaceStates[code < kWgSurfaceBlockCount ? code : 0];
}

void wg_apply_overworld_surface(const WgRandomState& state, int64_t seed,
                                const WgSurfaceChunk& chunk, const WgSurfaceRuleTree* rules) {
  apply_surface(state, seed, chunk, rules, /*nether=*/false);
}

void wg_apply_nether_surface(const WgRandomState& state, int64_t seed, const WgSurfaceChunk& chunk,
                             const WgSurfaceRuleTree* rules) {
  apply_surface(state, seed, chunk, rules, /*nether=*/true);
}

} // namespace mc::world
