// SPDX-License-Identifier: GPL-3.0-or-later
//
// The noise stage of the overworld: `NoiseChunk` plus the two `BlockStateFiller`
// rules behind it (`Aquifer` and `OreVeinifier`), and the fill loop
// `NoiseBasedChunkGenerator.doFill` drives them with.
//
// This is the point where "as vanilla as possible" becomes checkable. The
// reference dump contains, for seed 42, the full 16 x 16 x 384 cell grid of two
// chunks as one byte per cell plus a state-code map — and the bytes are the
// whole noise stage: the default block, the default fluid, the lava floor, the
// aquifer's own fluid picks and the two ore-vein states. Nothing in this file
// makes a terrain decision of its own; it only reproduces the ones above it.
//
// The one deliberate simplification is the aquifer's `preliminarySurfaceLevel`
// memo (a `Long2IntOpenHashMap` in vanilla). Every input to it is pure, so the
// memo is a performance detail and not a semantic one, and the port computes
// the value straight through.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "world/worldgen_biome.h"
#include "world/worldgen_density.h"
#include "world/worldgen_dimension.h"
#include "world/worldgen_random.h"
#include "world/worldgen_state.h"
#include "world/worldgen_table.h"

namespace mc::world {

struct WgSurfaceRuleTree;
class WgStructurePlacementState;
// The block states the overworld noise stage can produce, in the order the
// reference dump codes them (`noise_meta.tsv`).
enum class WgNoiseBlock : uint8_t {
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
};

// `Aquifer.FluidStatus`: a fluid level and the fluid above it.
struct WgFluidStatus {
  int32_t fluid_level = 0;
  WgNoiseBlock fluid_type = WgNoiseBlock::kAir;

  // `FluidStatus.at(blockY)`.
  WgNoiseBlock at(int32_t block_y) const {
    return block_y < fluid_level ? fluid_type : WgNoiseBlock::kAir;
  }
  bool operator==(const WgFluidStatus& other) const {
    return fluid_level == other.fluid_level && fluid_type == other.fluid_type;
  }
  bool operator!=(const WgFluidStatus& other) const {
    return !(*this == other);
  }
};

// `Aquifer.NoiseBasedAquifer`: the per-chunk fluid picker. It samples a lattice
// of aquifer centers (one random block inside each 16 x 12 x 16 cell), keeps
// the four nearest, and uses their pressure difference against the local
// density to decide whether the point is fluid, rock, or solid.
class WgAquifer {
public:
  WgAquifer(const WgRandomState& state, WgDensityGraph& graph, int32_t chunk_min_block_x,
            int32_t chunk_min_block_z, int32_t min_block_y, int32_t y_block_size,
            int32_t barrier_node, int32_t fluid_level_floodedness_node,
            int32_t fluid_level_spread_node, int32_t lava_node, int32_t erosion_node,
            int32_t depth_node, int32_t preliminary_surface_node, int32_t sea_level,
            WgNoiseBlock default_fluid);

  // `computeSubstance(context, density)`: the rule applies (an air/water/lava
  // cell) or it does not; see `worldgen_density.h` for what the caller does
  // with each answer.
  std::optional<WgNoiseBlock> compute_substance(int32_t x, int32_t y, int32_t z, double density);
  bool should_schedule_fluid_update() const {
    return should_schedule_fluid_update_;
  }

  // `NoiseChunk.preliminarySurfaceLevel`, the quantized read the aquifer does
  // its own surface sampling with.
  int32_t preliminary_surface_level(int32_t sample_x, int32_t sample_z);
  int32_t max_preliminary_surface_level(int32_t min_block_x, int32_t min_block_z,
                                        int32_t max_block_x, int32_t max_block_z);

private:
  int32_t index_of(int32_t grid_x, int32_t grid_y, int32_t grid_z) const;
  WgFluidStatus get_aquifer_status(int32_t index);
  WgFluidStatus compute_fluid(int32_t x, int32_t y, int32_t z);
  WgFluidStatus global_fluid(int32_t x, int32_t y, int32_t z) const;
  int32_t adjust_surface_level(int32_t preliminary_surface_level) const {
    return preliminary_surface_level + 8;
  }
  int32_t compute_surface_level(int32_t x, int32_t y, int32_t z, const WgFluidStatus& global,
                                int32_t lowest_preliminary_surface,
                                bool surface_at_center_under_global_fluid_level);
  int32_t compute_randomized_fluid_surface_level(int32_t x, int32_t y, int32_t z,
                                                 int32_t lowest_preliminary_surface);
  WgNoiseBlock compute_fluid_type(int32_t x, int32_t y, int32_t z, const WgFluidStatus& global,
                                  int32_t fluid_surface_level);
  double calculate_pressure(int32_t x, int32_t y, int32_t z, double& barrier_noise_value,
                            const WgFluidStatus& first, const WgFluidStatus& second);

  const WgPositionalFactory& random_;
  WgDensityGraph& graph_;
  int32_t barrier_node_ = 0;
  int32_t floodedness_node_ = 0;
  int32_t spread_node_ = 0;
  int32_t lava_node_ = 0;
  int32_t erosion_node_ = 0;
  int32_t depth_node_ = 0;
  int32_t preliminary_surface_node_ = 0;
  // `NoiseGeneratorSettings`' fluid picker inputs: the Nether's sea level is
  // lava, while the overworld's is water. Keeping these on the aquifer avoids
  // the overworld's constants leaking into a dimension with aquifers disabled.
  int32_t sea_level_ = 0;
  WgNoiseBlock default_fluid_ = WgNoiseBlock::kAir;

  int32_t min_grid_x_ = 0;
  int32_t min_grid_y_ = 0;
  int32_t min_grid_z_ = 0;
  int32_t grid_size_x_ = 0;
  int32_t grid_size_z_ = 0;
  int32_t skip_sampling_above_y_ = 0;
  bool should_schedule_fluid_update_ = false;
  std::vector<WgFluidStatus> aquifer_cache_;
  std::vector<uint8_t> aquifer_cache_present_;
  std::vector<int64_t> aquifer_location_cache_;
};

// `OreVeinifier`: copper veins above y 0 and iron veins below y -8, each a
// `vein_toggle` sign test, an edge roundoff, a ridged-noise check and three
// draws from the ore positional random.
class WgOreVeinifier {
public:
  WgOreVeinifier(WgDensityGraph& graph, int32_t vein_toggle_node, int32_t vein_ridged_node,
                 int32_t vein_gap_node, WgPositionalFactory ore_random);

  std::optional<WgNoiseBlock> compute(int32_t x, int32_t y, int32_t z);

private:
  WgDensityGraph& graph_;
  int32_t vein_toggle_node_ = 0;
  int32_t vein_ridged_node_ = 0;
  int32_t vein_gap_node_ = 0;
  WgPositionalFactory ore_random_;
};

// One `NoiseChunk`. `cell_count_xz` is 16 / cellWidth for a whole chunk, or 1
// for the single-cell shape `getInterpolatedNoiseValue` uses.
class WgNoiseChunk {
public:
  WgNoiseChunk(const WgRandomState& state, int32_t cell_count_xz, int32_t chunk_min_block_x,
               int32_t chunk_min_block_z, const WgDensityTable& table = wg_overworld_table());

  // `NoiseBasedChunkGenerator.doFill`'s block loop, writing `WgNoiseBlock` codes
  // into the fixture's layout: `blocks[(x*16 + z) * kWgHeight + (y - kWgMinY)]`.
  void fill_chunk(uint8_t* blocks);

  // The density the cursor sits on (`getInterpolatedDensity`).
  double interpolated_density() {
    return graph_.interpolated_density();
  }

  // `NoiseChunk.preliminarySurfaceLevel` — the surface stage's read of the
  // same aquifer memo the noise fill samples.
  int32_t preliminary_surface_level(int32_t sample_x, int32_t sample_z) {
    return aquifer_.preliminary_surface_level(sample_x, sample_z);
  }

  WgDensityGraph& graph() {
    return graph_;
  }
  const WgChunkCursor& cursor() const {
    return cursor_;
  }

  // `NoiseChunk.aquifer()` for the carver stage: the carve state at a cell is
  // this aquifer's answer at density 0.0 (the game's own query).
  WgAquifer& aquifer() {
    return aquifer_;
  }

  // `MultiNoiseBiomeSource.getNoiseBiome` at a quart cell, memoised for this
  // chunk. A biome is a pure function of the quart position and this chunk's
  // climate caches, and the surface stage asks for the same quart cell up to
  // 64 times (once per block it covers), so the KD-tree search behind
  // `wg_select_biome` is worth not repeating. The table is a small direct-mapped
  // cache: a collision only recomputes.
  const char* biome_at_quart(int32_t quart_x, int32_t quart_y, int32_t quart_z);

private:
  struct BiomeMemoEntry {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    const char* value = nullptr;
  };
  static constexpr size_t kBiomeMemoSlots = 512;

  WgChunkCursor cursor_;
  const WgDensityTable* table_ = nullptr;
  WgDensityGraph graph_;
  WgAquifer aquifer_;
  WgOreVeinifier ore_veins_;
  std::array<BiomeMemoEntry, kBiomeMemoSlots> biome_memo_{};
};

// `RandomState.sampler()`: the six climate router entries as one reusable
// point sampler. Vanilla's `NoiseWiringHelper` flattens every `NoiseChunk`
// marker out of the sampler the biome source is handed, so this graph is built
// with `wrap_markers=false` and holds no per-chunk state: one instance can
// answer every quart in a ring search (416k samples at the pack's 128 rings)
// without rebuilding a `NoiseChunk` around each one.
class WgClimateSampler {
public:
  explicit WgClimateSampler(const WgRandomState& state,
                            const WgDensityTable& table = wg_overworld_table());

  WgClimateTarget target_at_quart(int32_t quart_x, int32_t quart_y, int32_t quart_z);
  const char* biome_at_quart(int32_t quart_x, int32_t quart_y, int32_t quart_z);

private:
  const WgDensityTable* table_ = nullptr;
  WgChunkCursor cursor_;
  WgDensityGraph graph_;
};

// One noise generator: a seed, a dimension's density table, and then either a
// filled chunk or a single density read. The name predates the Nether and End
// tables; the surface and carver members below are still the overworld rule
// set, so only the noise/biome stages are dimension-neutral.
class WgOverworldGenerator {
public:
  explicit WgOverworldGenerator(int64_t seed, const WgDensityTable& table = wg_overworld_table())
      : seed_(seed), state_(seed, table.legacy_random_source), table_(&table) {}

  const WgDensityTable& table() const {
    return *table_;
  }

  // The shared surface rule tree, built on first use. `std::atomic_load`/`store`
  // rather than a `once_flag`: the generator is copied (it has to stay copyable)
  // and generation runs on several pool threads, so a rare duplicate build is
  // preferable to a copy that will not compile. Both trees are the same tree.
  std::shared_ptr<const WgSurfaceRuleTree> surface_rules() const;

  const WgRandomState& state() const {
    return state_;
  }
  int64_t seed() const {
    return seed_;
  }

  // `ChunkGeneratorStructureState`'s concentric-ring list and its
  // `isStructureChunk` answers, built on first use. One instance per generator:
  // the ring search samples the climate graph 416k times, so a chunk-by-chunk
  // rebuild would be the wrong shape. `std::atomic_load`/`store`, like
  // `surface_rules`, keeps the generator copyable.
  const WgStructurePlacementState& structure_placement_state() const;

  void fill_noise_chunk(int32_t chunk_x, int32_t chunk_z, uint8_t* blocks) const {
    WgNoiseChunk chunk(state_, 16 / table_->cell_width, chunk_x * 16, chunk_z * 16, *table_);
    chunk.fill_chunk(blocks);
  }

  // `NoiseBasedChunkGenerator.getInterpolatedNoiseValue` — the value the chunk
  // fill would use at this block, NaN outside the dimension's height.
  double interpolated_density(int32_t block_x, int32_t block_y, int32_t block_z) const;

  // One router entry at one point, evaluated the way `RandomState.router()`
  // hands it out: no chunk, no interpolation wrappers, just the algebra.
  double router_value(WgRouterField field, int32_t x, int32_t y, int32_t z) const;

  // The same read for any table node, which is how the named density functions
  // (`kWgNamed`) are pinned one subtree at a time.
  double node_value(uint32_t table_node, int32_t x, int32_t y, int32_t z) const;

  // `Climate.Sampler.sample(quartX, quartY, quartZ)`: the six climate router
  // entries at the cell's block origin (`QuartPos.toBlock`), each narrowed to
  // float and quantized, read through the chunk-local caches
  // `NoiseChunk.cachedClimateSampler` wraps them with.
  WgClimateTarget climate_target_at_quart(int32_t quart_x, int32_t quart_y, int32_t quart_z) const;

  // `MultiNoiseBiomeSource.getNoiseBiome` at one quart cell.
  const char* biome_at_quart(int32_t quart_x, int32_t quart_y, int32_t quart_z) const;

  // `ChunkAccess.fillBiomesFromNoise` for one chunk: `table().height / 16`
  // sections x 4 x 4 x 4 biome cells, in vanilla's own query order (sections
  // bottom-up; within a section x, then y, then z), written in the chunk model's
  // cell order (`(y * 4 + z) * 4 + x` per section — world/chunk.h's
  // `kBiomeCellsPerSection`) so the bridge can copy each section wholesale.
  // `out` holds `(table().height / 16) * 64` names; one bare graph serves them
  // all.
  void fill_biome_chunk(int32_t chunk_x, int32_t chunk_z, const char** out) const;

  // The carved column and its biomes over one `NoiseChunk`. `fill_carved_chunk`
  // builds a chunk for the terrain stages and leaves it there; the biome fill
  // then builds a third graph over the same chunk-local caches for its 1,536
  // climate samples. This entry keeps the terrain chunk alive for that fill, so
  // the samples read the router and flat caches the surface and carve stages
  // already warmed (the biome memo in front of them is the surface stage's own).
  // A/B arm `PLUSMC_BIOME_CHUNK_REUSE=off` builds the separate graph
  // `fill_biome_chunk` builds, on the same binary.
  void fill_carved_chunk_with_biomes(int32_t chunk_x, int32_t chunk_z, uint8_t* out,
                                     const char** biomes) const;

  // `NoiseBasedChunkGenerator.buildSurface` for one chunk: the noise stage is
  // filled into `out` first, then the surface stage rewrites it in place. The
  // codes are `world/worldgen_surface.h`'s `WgSurfaceBlock` (the first ten are
  // the noise stage's own). Deterministic, and slow enough to be worth a
  // single pass per chunk rather than per column.
  void fill_surface_chunk(int32_t chunk_x, int32_t chunk_z, uint8_t* out) const;

  // The same column with `NoiseBasedChunkGenerator.applyCarvers` run over it:
  // the surface output in `out` is rewritten in place by the biome's configured
  // cave and canyon carvers. One `WgNoiseChunk` serves both stages, which is
  // vanilla's own shape (`applyCarvers` is handed the same `NoiseChunk`
  // `buildSurface` read): the carve state asks that chunk's aquifer at every
  // cell it replaces, and the aquifer's caches, the wrapped router's flat
  // caches and the biome memo are all warm from the noise fill and the surface
  // rules by then. A second chunk would be correct — every read is pure — but
  // cold, and its 4 x 4 x 8 aquifer lattice plus the router it wraps are most
  // of the carve stage's cost. A/B arm `PLUSMC_CARVE_CHUNK_REUSE=off` builds
  // the second chunk, which is what this entry point used to do.
  void fill_carved_chunk(int32_t chunk_x, int32_t chunk_z, uint8_t* out) const;

private:
  // The two halves of `fill_carved_chunk`, split so one chunk can carry both:
  // the noise fill and surface pass into `out`, then the carvers over the same
  // `out` and the same chunk.
  void fill_surface_with(WgNoiseChunk& chunk, int32_t chunk_x, int32_t chunk_z, uint8_t* out) const;
  void carve_with(WgNoiseChunk& chunk, int32_t chunk_x, int32_t chunk_z, uint8_t* out) const;
  // The biome cells through one chunk's own `biome_at_quart` (the memo and the
  // graph `fill_biome_chunk` would otherwise rebuild).
  void fill_biomes_with(WgNoiseChunk& chunk, int32_t chunk_x, int32_t chunk_z,
                        const char** out) const;
  int64_t seed_ = 0;
  WgRandomState state_;
  const WgDensityTable* table_ = nullptr;
  mutable std::shared_ptr<const WgSurfaceRuleTree> surface_rules_;
  mutable std::shared_ptr<const WgStructurePlacementState> structure_placement_state_;
};

// How many point-query climate graphs this thread has built. A point query
// reuses one marker-transparent graph, so a run of `biome_at_quart` calls
// builds at most one; the per-query arm (`PLUSMC_BIOME_POINT_GRAPH=pre`) builds
// one for every call. `tests/worldgen_test.cpp` pins the first number.
uint64_t point_climate_graph_builds();

} // namespace mc::world
