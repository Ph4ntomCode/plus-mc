// SPDX-License-Identifier: GPL-3.0-or-later
//
// The surface stage of the overworld: `SurfaceSystem` over the default
// overworld rule set, the step `ChunkGenerator.fillFromNoise` is followed by
// in vanilla (`NoiseBasedChunkGenerator.buildSurface`).
//
// The noise stage leaves every column as stone/air/water/lava/ore; this stage
// walks each column from its world-surface height down and rewrites the stone
// near the top into the biome's own materials — grass and dirt, sand and
// sandstone, terracotta bands, podzol, mycelium, snow and ice — exactly as the
// game's own rule tree does. The rule tree itself is transcribed from
// `SurfaceRuleData.overworldLike(biomes, true, false, true)` in
// worldgen_surface.cpp; the engine that evaluates it (`SurfaceRules`'
// conditions and the `SurfaceSystem` helpers they read) is ported there too.
//
// The block-state codes this stage writes are the noise stage's own ten codes
// plus the surface materials, so a caller can start from `fill_noise_chunk`'s
// array and rewrite it in place.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "world/worldgen_state.h"

namespace mc::world {

class WgPerlinSimplexNoise;

// `Biome.BIOME_INFO_NOISE` — the one `PerlinSimplexNoise` the surface system and
// the decoration stage share (`seed 2345`, octaves `[0]`). `noise_threshold_count`
// samples it at `(x / 200, z / 200)`. The surface stage already builds it; this
// exposes the same instance rather than a second construction.
const WgPerlinSimplexNoise& wg_biome_info_noise();

// The block states the surface stage can produce, in a fixed order: 0..9 are
// `WgNoiseBlock`'s own codes (unchanged cells pass through), and 10.. are the
// materials the overworld rule tree writes.
enum class WgSurfaceBlock : uint8_t {
  kAir = 0,
  kStone = 1,
  kWater = 2,
  kLava = 3,
  kGranite = 4,
  kCopperOre = 5,
  kRawCopperBlock = 6,
  kTuff = 7,
  kDeepslateIronOre = 8,
  kRawIronBlock = 9,
  kBedrock = 10,
  kDeepslate = 11,
  kDirt = 12,
  kGrassBlock = 13,
  kPodzol = 14,
  kCoarseDirt = 15,
  kMycelium = 16,
  kCalcite = 17,
  kGravel = 18,
  kSand = 19,
  kSandstone = 20,
  kRedSand = 21,
  kRedSandstone = 22,
  kTerracotta = 23,
  kWhiteTerracotta = 24,
  kOrangeTerracotta = 25,
  kYellowTerracotta = 26,
  kBrownTerracotta = 27,
  kLightGrayTerracotta = 28,
  kPackedIce = 29,
  kSnowBlock = 30,
  kMud = 31,
  kPowderSnow = 32,
  kIce = 33,
  kCinnabar = 34,
  kSulfur = 35,
  kRedTerracotta = 36,
  kNetherrack = 37,
  kSoulSand = 38,
  kSoulSoil = 39,
  kBasalt = 40,
  kBlackstone = 41,
  kWarpedNylium = 42,
  kCrimsonNylium = 43,
  kNetherWartBlock = 44,
  kWarpedWartBlock = 45,
  // `NetherWorldCarver` writes cave air above its lava sea; the generic carver
  // writes plain air. Both are air for heightmaps and surface reads, but the
  // carved fixture distinguishes them by name.
  kCaveAir = 46,
};

inline constexpr size_t kWgSurfaceBlockCount = static_cast<size_t>(WgSurfaceBlock::kCaveAir) + 1;

inline bool wg_surface_code_is_air(uint8_t code) {
  return code == static_cast<uint8_t>(WgSurfaceBlock::kAir) ||
         code == static_cast<uint8_t>(WgSurfaceBlock::kCaveAir);
}

// The block state each code means, for the chunk bridge and for tests that
// compare against a fixture dump. `properties` is the canonical
// `a=b,c=d` string (sorted) the state keys use.
struct WgSurfaceState {
  const char* name;
  const char* properties;
};
const WgSurfaceState& wg_surface_state(uint8_t code);

// One chunk's inputs. `codes` points at 16 x 16 x `height` cells in the noise
// stage's layout (`(x*16 + z) * height + (y - min_y)`); `out` receives the same
// layout and may alias `codes`. The two callbacks are the reads the surface
// system makes on the live chunk and the biome source: the noise chunk's
// `preliminarySurfaceLevel`, and `BiomeManager.NoiseBiomeSource.getNoiseBiome`
// for the bare `MultiNoiseBiomeSource` (the manager's fuzzy lookup is applied
// on top, in this file).
struct WgSurfaceChunk {
  int32_t chunk_x = 0;
  int32_t chunk_z = 0;
  int32_t min_y = -64;
  int32_t height = 384;
  int32_t sea_level = 63;
  bool use_legacy_random = false;
  const uint8_t* codes = nullptr;
  uint8_t* out = nullptr;
  void* user = nullptr;
  int32_t (*preliminary_surface_level)(void* user, int32_t x, int32_t z) = nullptr;
  const char* (*biome_at_quart)(void* user, int32_t quart_x, int32_t quart_y,
                                int32_t quart_z) = nullptr;
};

// The overworld rule tree, built once and shared. Constructing it allocates a
// few hundred immutable condition and rule nodes from static data, and a
// served column builds it twice — once for `buildSurface` and once for the
// carver's own re-top context — so the generator keeps one tree for both. The
// tree is a pure function of the random state (the `vertical_gradient`
// conditions hold positional factories, not per-chunk state), so sharing it
// across chunks and threads is exactly the per-chunk tree's answer.
struct WgSurfaceRuleTree;
std::shared_ptr<const WgSurfaceRuleTree> wg_build_overworld_rules(const WgRandomState& state);

// `SurfaceRuleData.nether()`, the rule set `NoiseGeneratorSettings.NETHER`
// carries. It is a separate tree because every material, anchor and biome
// branch differs from the overworld's.
std::shared_ptr<const WgSurfaceRuleTree> wg_build_nether_rules(const WgRandomState& state,
                                                               int64_t seed);

// Applies the default overworld surface rule set to one chunk. Deterministic:
// the same generator, chunk and inputs always produce the same codes. `rules`
// may be a tree from `wg_build_overworld_rules`; null builds one for this call
// (what every caller did before the tree was shareable).
void wg_apply_overworld_surface(const WgRandomState& state, int64_t seed,
                                const WgSurfaceChunk& chunk,
                                const WgSurfaceRuleTree* rules = nullptr);

// The same column pass under the Nether rule tree.
void wg_apply_nether_surface(const WgRandomState& state, int64_t seed, const WgSurfaceChunk& chunk,
                             const WgSurfaceRuleTree* rules = nullptr);

// `SurfaceSystem.topMaterial`: the same rule tree, asked for one cell instead
// of a whole column. The carver stage calls this when a tunnel opens a grass
// ceiling and vanilla re-tops the dirt underneath it. Building the tree and
// resolving the system's noises is the expensive part, so one instance serves
// every cell of a chunk; `chunk` must outlive it and must point at the live
// block array (the rules read it, e.g. `stoneDepth` and the badlands bands).
//
// `at` follows vanilla's `CarvingContext.topMaterial`: `updateXZ` then
// `updateY(1, 1, under_fluid ? y + 1 : MIN_VALUE, y)` and one `tryApply`. It
// returns true and writes `out` when the rule produced a state; false is
// vanilla's empty `Optional` (the cell keeps what the carver wrote).
class WgSurfaceTopMaterial {
public:
  WgSurfaceTopMaterial(const WgRandomState& state, int64_t seed, const WgSurfaceChunk& chunk,
                       const WgSurfaceRuleTree* rules = nullptr);
  ~WgSurfaceTopMaterial();
  WgSurfaceTopMaterial(const WgSurfaceTopMaterial&) = delete;
  WgSurfaceTopMaterial& operator=(const WgSurfaceTopMaterial&) = delete;

  bool at(int32_t block_x, int32_t block_y, int32_t block_z, bool under_fluid, uint8_t* out);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// `BiomeManager.obfuscateSeed(seed)` — SHA-256 of the seed's little-endian
// bytes, first eight bytes read back little-endian. This is the value vanilla
// sends in Join Game's `hashedSeed` field (`ServerPlayer.createCommonSpawnInfo`),
// so a client is told the hash and never the seed; the client's biome manager
// uses it for the fallback sample outside loaded chunks.
int64_t obfuscate_seed(int64_t seed);

} // namespace mc::world
