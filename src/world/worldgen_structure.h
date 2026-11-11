// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 4's structure application, the placement half: **where** a structure set
// puts its starts, and **which biome** a start must stand in. The registries
// already carry each set's members; this adds the placement config the pack
// ships beside them (`spacing`, `separation`, `salt`, the spread type, the
// frequency gate, the concentric-ring fields and the resolved biome sets) and
// ports `RandomSpreadStructurePlacement.getPotentialStructureChunk`,
// `StructurePlacement.isStructureChunk`, `ChunkGeneratorStructureState`'s
// concentric-ring list and `Structure.isValidBiome`.
//
// The boundary is deliberate. This header answers "is this chunk one of the
// set's starts" (random-spread candidates, the frequency gate, the exclusion
// zone and the stronghold rings) and "does the start's generation point stand
// in a biome the structure allows"; it does not build pieces, and it does not
// choose a non-ring structure's generation point. The generation point is the
// structure type's own `findGenerationPoint` (the jigsaw/template slice), and
// `wg_structure_start_biome_is_valid` takes it as a block position rather than
// guessing it. `docs/PHASE4.md` names the gap.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "world/worldgen_structure_table.h"

namespace mc::world {

class WgClimateSampler;
class WgOverworldGenerator;

// `RandomSpreadType`'s two values, as the generated table stores them.
constexpr uint8_t kWgSpreadLinear = 0;
constexpr uint8_t kWgSpreadTriangular = 1;

struct WgStructureChunk {
  int32_t x = 0;
  int32_t z = 0;
};

// `Math.floorDiv` for the chunk -> spacing-square division. C++'s `/` truncates
// toward zero, so chunk -1 would land in region 0; vanilla puts it in region -1.
int32_t wg_structure_floor_div(int32_t a, int32_t b);

// `Structure.biomes().contains(biome)`: the resolved `HolderSet` the generated
// table carries. An unknown biome is not in the set.
bool wg_structure_biome_allowed(const WgStructure& structure, const char* biome);

// `ChunkGeneratorStructureState.hasBiomesForStructureSet`: whether any member
// structure allows a biome the overworld's biome source can produce. Vanilla
// drops a set from the overworld's `possibleStructureSets` when this is false
// (the end and nether sets, here).
bool wg_structure_set_has_biomes(const WgStructureSet& set);

// `Structure.isValidBiome`: the noise biome at the start's generation point,
// looked up at `QuartPos.fromBlock` on each axis, has to be in the structure's
// own biome set. `block_y` is the generation point's y, which each structure
// type's `findGenerationPoint` chooses; the pieces slice supplies it.
bool wg_structure_start_biome_is_valid(WgClimateSampler& sampler, const WgStructure& structure,
                                       int32_t block_x, int32_t block_y, int32_t block_z);

// `RandomSpreadStructurePlacement.getPotentialStructureChunk(seed, chunkX,
// chunkZ)`: the one chunk in the spacing square that may hold this set's start.
// The draws are two (linear) or four (triangular) `nextInt(spacing -
// separation)` calls from a `WorldgenRandom(LegacyRandomSource(0))` seeded with
// `regionX * 341873128712 + regionZ * 132897987541 + seed + salt`.
//
// Only a `minecraft:random_spread` row has an answer; a concentric-ring row
// (spacing 0) returns the input chunk unchanged, and a caller must check the
// type rather than treat that as a candidate.
WgStructureChunk wg_structure_candidate(int64_t seed, const WgStructurePlacement& placement,
                                        int32_t chunk_x, int32_t chunk_z);

// `StructurePlacement.applyAdditionalChunkRestrictions`: the frequency gate a
// set with `frequency < 1` puts on top of its candidate chunk. The four
// reducers are vanilla's, not one shared draw: the pack's three gated rows use
// `legacy_type_2` (buried treasure, a `setLargeFeatureWithSalt` + `nextFloat`),
// `legacy_type_1` (pillager outposts, a region-seeded `nextInt(1/frequency)`)
// and `legacy_type_3` (mineshafts, `setLargeFeatureSeed` + `nextDouble`). A row
// without a gate answers true without touching its RNG, and an unknown
// reduction name is refused by the generator, so this cannot silently become a
// default draw.
bool wg_structure_passes_frequency(int64_t seed, const WgStructurePlacement& placement,
                                   int32_t chunk_x, int32_t chunk_z);

// The generated set whose name matches, or nullptr.
const WgStructureSet* wg_structure_set_by_name(const char* name);

// The generated structure whose name matches, or nullptr.
const WgStructure* wg_structure_by_name(const char* name);

// `StructurePlacement.isStructureChunk` (the `ChunkGeneratorStructureState`
// overload vanilla's `isStructureChunk` calls): the candidate chunk has to name
// itself, the frequency gate has to pass, and — for a row that carries one —
// the exclusion zone has to find no start of the named other set within
// `exclusion_chunks` chunks. The checks are in that order because vanilla's are:
// the gate is a cheap draw and the exclusion scan is 441 candidate tests.
//
// A concentric-ring row answers false here. Its starts are the stronghold ring
// list, which needs the biome source (and therefore a `WgClimateSampler`);
// `WgStructurePlacementState` carries that list and answers the same question
// with it. Answering "the candidate is this chunk" (what
// `wg_structure_candidate` returns for a zero spacing) would claim every
// stronghold chunk.
bool wg_structure_is_chunk(int64_t seed, const WgStructureSet& set, int32_t chunk_x,
                           int32_t chunk_z);

// `ChunkGeneratorStructureState.hasStructureChunkInRange(set, chunkX, chunkZ,
// distance)`: whether any chunk of the `(2 * distance + 1)`-square around this
// one — both bounds inclusive — is a start of `set_name`, each tested with
// `wg_structure_is_chunk`. Vanilla's exclusion zone calls it with the zone's
// own `chunk_count`; an unknown set name answers false, which the generator's
// validation (every `exclusion_zone.other_set` must name a set, and the chain
// must not loop) keeps from happening. A concentric-ring set answers false for
// the same reason `wg_structure_is_chunk` does; use the state below.
bool wg_structure_has_chunk_in_range(int64_t seed, const char* set_name, int32_t chunk_x,
                                     int32_t chunk_z, int32_t distance);

// `ChunkGeneratorStructureState.generateRingPositions` for one concentric-ring
// placement, in vanilla's own order:
//
//   angle = nextDouble() * 2pi
//   dist  = 4 * distance + distance * circle * 6
//           + (nextDouble() - 0.5) * (distance * 2.5)
//   (x, z) = round(cos(angle) * dist), round(sin(angle) * dist)   // chunks
//
// then `findBiomeHorizontal(x * 16 + 8, 0, z * 16 + 8, 112, preferred)` over the
// forked `LegacyRandomSource`, whose answer (if any) replaces the ring point
// with the found block's chunk. The `spread`/`circle` walk, including the
// post-fork `nextDouble()` at each ring boundary, is vanilla's. The returned
// list is in generation order and may repeat a chunk.
//
// `sampler` is `RandomState.sampler()`: the raw six-entry climate sampler with
// no chunk-local caches, which is the one `findBiomeHorizontal` uses.
std::vector<WgStructureChunk> wg_structure_ring_positions(int64_t seed,
                                                          const WgStructurePlacement& placement,
                                                          WgClimateSampler& sampler);

// `ChunkGeneratorStructureState`, for one generator: the per-placement ring
// lists (generated lazily once, like vanilla's `ringPositions` futures) and the
// two start queries over them. The random-spread answers are the same functions
// above; the concentric-ring answers are the cached list's membership. A ring
// set named by an exclusion zone is handled correctly here, unlike the free
// seed-only functions.
class WgStructurePlacementState {
public:
  explicit WgStructurePlacementState(const WgOverworldGenerator& generator);
  ~WgStructurePlacementState();

  WgStructurePlacementState(const WgStructurePlacementState&) = delete;
  WgStructurePlacementState& operator=(const WgStructurePlacementState&) = delete;

  int64_t seed() const {
    return seed_;
  }

  bool is_structure_chunk(const WgStructureSet& set, int32_t chunk_x, int32_t chunk_z) const;
  bool has_structure_chunk_in_range(const char* set_name, int32_t chunk_x, int32_t chunk_z,
                                    int32_t distance) const;

  // The ring list for one `kWgStructurePlacements` row, generated on first use.
  const std::vector<WgStructureChunk>& ring_positions(uint32_t placement_index) const;

private:
  bool is_structure_chunk_at(const WgStructureSet& set, int32_t chunk_x, int32_t chunk_z) const;

  int64_t seed_ = 0;
  std::unique_ptr<WgClimateSampler> sampler_;
  mutable std::mutex mutex_;
  mutable std::vector<std::vector<WgStructureChunk>> rings_;
  mutable std::vector<char> rings_known_;
};

} // namespace mc::world
