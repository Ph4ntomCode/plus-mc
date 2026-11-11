// SPDX-License-Identifier: GPL-3.0-or-later
//
// `RandomSpreadStructurePlacement.getPotentialStructureChunk`,
// `StructurePlacement.isStructureChunk`, `ChunkGeneratorStructureState`'s ring
// list and `Structure.isValidBiome`, over the generated structure-set rows.
// See worldgen_structure.h for the slice's boundary and docs/PHASE4.md for the
// pinned values.
#include "world/worldgen_structure.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "world/worldgen_biome_table.h"
#include "world/worldgen_chunk.h"
#include "world/worldgen_random.h"

namespace mc::world {

namespace {
// Java's `long` arithmetic wraps; C++'s signed overflow is undefined, so the
// products and sums below are formed in `uint64_t` and narrowed once.
int64_t wrap_mul(int64_t a, int64_t b) {
  return static_cast<int64_t>(static_cast<uint64_t>(a) * static_cast<uint64_t>(b));
}

int64_t wrap_add(int64_t a, int64_t b) {
  return static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b));
}

// `WorldgenRandom.setLargeFeatureSeed(seed, chunkX, chunkZ)`: reseed, take two
// longs from it, then reseed with `(chunkX * first) ^ (chunkZ * second) ^ seed`.
void seed_large_feature(WgLegacyRandom& random, int64_t seed, int32_t x, int32_t z) {
  random.set_seed(seed);
  const int64_t first = random.next_long();
  const int64_t second = random.next_long();
  random.set_seed(wrap_mul(x, first) ^ wrap_mul(z, second) ^ seed);
}

// `WorldgenRandom.setLargeFeatureWithSalt(seed, x, z, salt)`: no draws, just
// `x * 341873128712 + z * 132897987541 + seed + salt`.
void seed_large_feature_with_salt(WgLegacyRandom& random, int64_t seed, int32_t x, int32_t z,
                                  int32_t salt) {
  int64_t mixed = wrap_mul(x, 341873128712LL);
  mixed = wrap_add(mixed, wrap_mul(z, 132897987541LL));
  mixed = wrap_add(mixed, seed);
  random.set_seed(wrap_add(mixed, salt));
}

template <typename T, size_t N>
bool name_in(const char* name, const T (&list)[N], uint32_t offset, uint32_t count) {
  if (offset > N || count > N - offset)
    return false;
  for (uint32_t i = 0; i < count; ++i) {
    if (std::strcmp(name, list[offset + i]) == 0)
      return true;
  }
  return false;
}

bool ring_prefers_biome(const WgStructurePlacement& placement, const char* biome) {
  return name_in(biome, kWgRingPreferredBiomes, placement.preferred_biome_offset,
                 placement.preferred_biome_count);
}

bool overworld_has_biome(const char* name) {
  constexpr size_t kCount = sizeof(kWgBiomeParameters) / sizeof(kWgBiomeParameters[0]);
  for (size_t i = 0; i < kCount; ++i) {
    if (std::strcmp(name, kWgBiomeParameters[i].biome) == 0)
      return true;
  }
  return false;
}

bool contains_chunk(const std::vector<WgStructureChunk>& list, int32_t x, int32_t z) {
  for (const WgStructureChunk& chunk : list) {
    if (chunk.x == x && chunk.z == z)
      return true;
  }
  return false;
}

// `BiomeSource.findBiomeHorizontal(originX, originY, originZ, searchRadius,
// skipSteps=1, allowed, random, findClosest=false, sampler)`. With
// `findClosest=false` the radius loop starts at the outer radius and therefore
// visits exactly one radius: the full `(2 * radiusQuart + 1)` square, in
// `z`-then-`x` order, reservoir-sampling a match with `nextInt(found + 1)`.
// Returns the found block position's chunk, or has no answer.
bool find_biome_horizontal(WgClimateSampler& sampler, const WgStructurePlacement& placement,
                           WgLegacyRandom& random, int32_t origin_x, int32_t origin_z,
                           int32_t search_radius, int32_t* out_x, int32_t* out_z) {
  const int32_t noise_center_x = origin_x >> 2; // QuartPos.fromBlock
  const int32_t noise_center_z = origin_z >> 2;
  const int32_t noise_radius = search_radius >> 2;
  constexpr int32_t kNoiseY = 0; // originY is 0 and `QuartPos.fromBlock(0)` is 0

  bool found_any = false;
  int32_t found = 0;
  int32_t best_x = 0;
  int32_t best_z = 0;
  for (int32_t radius = noise_radius; radius <= noise_radius; ++radius) {
    int32_t z = -radius;
    while (z <= radius) {
      for (int32_t x = -radius; x <= radius; ++x) {
        const char* biome = sampler.biome_at_quart(noise_center_x + x, kNoiseY, noise_center_z + z);
        if (!ring_prefers_biome(placement, biome))
          continue;
        if (!found_any || random.next_int(found + 1) == 0) {
          best_x = (noise_center_x + x) * 4; // QuartPos.toBlock
          best_z = (noise_center_z + z) * 4;
          found_any = true;
        }
        ++found;
      }
      z += 1;
    }
  }
  if (!found_any)
    return false;
  *out_x = best_x;
  *out_z = best_z;
  return true;
}

template <typename RingLookup>
bool has_chunk_in_range_with_rings(int64_t seed, const char* set_name, int32_t chunk_x,
                                   int32_t chunk_z, int32_t distance,
                                   const RingLookup& ring_lookup);

// The shared `isStructureChunk` body. `ring_lookup(placement_index, x, z)` is the
// one piece a seed alone cannot answer: a concentric-ring row's cached list.
template <typename RingLookup>
bool is_structure_chunk_with_rings(int64_t seed, const WgStructureSet& set, int32_t chunk_x,
                                   int32_t chunk_z, const RingLookup& ring_lookup) {
  const WgStructurePlacement& placement = kWgStructurePlacements[set.placement];
  if (std::strcmp(placement.type, "minecraft:concentric_rings") == 0)
    return ring_lookup(set.placement, chunk_x, chunk_z);

  const WgStructureChunk candidate = wg_structure_candidate(seed, placement, chunk_x, chunk_z);
  if (candidate.x != chunk_x || candidate.z != chunk_z)
    return false;

  if (!wg_structure_passes_frequency(seed, placement, chunk_x, chunk_z))
    return false;

  if (placement.exclusion_set[0] != '\0' &&
      has_chunk_in_range_with_rings(seed, placement.exclusion_set, chunk_x, chunk_z,
                                    placement.exclusion_chunks, ring_lookup))
    return false;
  return true;
}

template <typename RingLookup>
bool has_chunk_in_range_with_rings(int64_t seed, const char* set_name, int32_t chunk_x,
                                   int32_t chunk_z, int32_t distance,
                                   const RingLookup& ring_lookup) {
  const WgStructureSet* other = wg_structure_set_by_name(set_name);
  if (other == nullptr || distance < 0)
    return false;
  // Vanilla's loop is inclusive on both bounds, so the square is
  // (2 * distance + 1) chunks wide and a chunk exactly `distance` away counts.
  for (int32_t dx = -distance; dx <= distance; ++dx) {
    for (int32_t dz = -distance; dz <= distance; ++dz) {
      if (is_structure_chunk_with_rings(seed, *other, chunk_x + dx, chunk_z + dz, ring_lookup))
        return true;
    }
  }
  return false;
}
} // namespace

int32_t wg_structure_floor_div(int32_t a, int32_t b) {
  int32_t quotient = a / b;
  const int32_t remainder = a % b;
  // Java's floorDiv: a non-zero remainder with a negative sign steps down.
  if (remainder != 0 && ((remainder < 0) != (b < 0)))
    --quotient;
  return quotient;
}

bool wg_structure_biome_allowed(const WgStructure& structure, const char* biome) {
  return name_in(biome, kWgStructureBiomes, structure.biome_offset, structure.biome_count);
}

bool wg_structure_set_has_biomes(const WgStructureSet& set) {
  for (uint32_t i = 0; i < set.entry_count; ++i) {
    const WgStructureSetEntry& entry = kWgStructureSetEntries[set.entry_offset + i];
    const WgStructure* structure = wg_structure_by_name(entry.structure);
    if (structure == nullptr)
      continue;
    if (structure->biome_offset > sizeof(kWgStructureBiomes) / sizeof(char*) ||
        structure->biome_count >
            sizeof(kWgStructureBiomes) / sizeof(char*) - structure->biome_offset)
      continue;
    for (uint32_t b = 0; b < structure->biome_count; ++b) {
      if (overworld_has_biome(kWgStructureBiomes[structure->biome_offset + b]))
        return true;
    }
  }
  return false;
}

bool wg_structure_start_biome_is_valid(WgClimateSampler& sampler, const WgStructure& structure,
                                       int32_t block_x, int32_t block_y, int32_t block_z) {
  // `QuartPos.fromBlock` is `>> 2`, exactly Java's arithmetic shift.
  const char* biome = sampler.biome_at_quart(block_x >> 2, block_y >> 2, block_z >> 2);
  return wg_structure_biome_allowed(structure, biome);
}

WgStructureChunk wg_structure_candidate(int64_t seed, const WgStructurePlacement& placement,
                                        int32_t chunk_x, int32_t chunk_z) {
  // The concentric-ring rows have no per-chunk candidate; the guard keeps a
  // mis-call from dividing by zero rather than inventing a chunk.
  if (placement.spacing == 0 || placement.separation >= placement.spacing)
    return WgStructureChunk{chunk_x, chunk_z};

  const int32_t spacing = static_cast<int32_t>(placement.spacing);
  const int32_t region_x = wg_structure_floor_div(chunk_x, spacing);
  const int32_t region_z = wg_structure_floor_div(chunk_z, spacing);

  // `WorldgenRandom.setLargeFeatureWithSalt(seed, regionX, regionZ, salt)`.
  WgLegacyRandom random(0);
  random.set_seed(static_cast<int64_t>(region_x) * 341873128712LL +
                  static_cast<int64_t>(region_z) * 132897987541LL + seed +
                  static_cast<int64_t>(placement.salt));

  const int32_t range = spacing - static_cast<int32_t>(placement.separation);
  const auto evaluate = [&random, range](uint8_t type) {
    const int32_t first = random.next_int(range);
    if (type == kWgSpreadLinear)
      return first;
    // `RandomSpreadType.TRIANGULAR`: two draws, averaged with Java's truncating
    // int division (both are non-negative, so this is the same floor).
    return (first + random.next_int(range)) / 2;
  };

  return WgStructureChunk{region_x * spacing + evaluate(placement.spread_type),
                          region_z * spacing + evaluate(placement.spread_type)};
}

bool wg_structure_passes_frequency(int64_t seed, const WgStructurePlacement& placement,
                                   int32_t chunk_x, int32_t chunk_z) {
  if (!(placement.frequency < 1.0f))
    return true;
  const float frequency = placement.frequency;
  WgLegacyRandom random(0);

  // `legacy_type_1` (`legacyPillagerOutpostReducer`): the draw is made once per
  // 16 x 16-chunk region, not per chunk — `chunkX >> 4` and `chunkZ >> 4` are
  // mixed into the seed, one `nextInt()` is discarded, and the gate is a
  // `nextInt((int)(1.0f / frequency)) == 0`, whose answer every chunk in that
  // region shares.
  if (std::strcmp(placement.frequency_reduction, "legacy_type_1") == 0) {
    const int32_t region_x = chunk_x >> 4;
    const int32_t region_z = chunk_z >> 4;
    random.set_seed(static_cast<int64_t>(region_x ^ (region_z << 4)) ^ seed);
    random.next_int();
    return random.next_int(static_cast<int32_t>(1.0f / frequency)) == 0;
  }

  // `legacy_type_2` (`legacyArbitrarySaltProbabilityReducer`, buried treasure):
  // the row's salt is ignored and the constant 10387320 is used instead.
  if (std::strcmp(placement.frequency_reduction, "legacy_type_2") == 0) {
    seed_large_feature_with_salt(random, seed, chunk_x, chunk_z, 10387320);
    return random.next_float() < frequency;
  }

  // `legacy_type_3` (`legacyProbabilityReducerWithDouble`, mineshafts).
  if (std::strcmp(placement.frequency_reduction, "legacy_type_3") == 0) {
    seed_large_feature(random, seed, chunk_x, chunk_z);
    return random.next_double() < static_cast<double>(frequency);
  }

  // `default` (`probabilityReducer`): a float draw over the row's own salt. Its
  // call passes the arguments in a different order from the candidate path —
  // `(seed, salt, chunkX, chunkZ)` into a helper that multiplies its first two
  // ints by the two constants — so the mixed seed is
  // `salt * 341873128712 + chunkX * 132897987541 + seed + chunkZ`. The pinned
  // digest is what says this ordering is the server's, not an accident of names.
  seed_large_feature_with_salt(random, seed, static_cast<int32_t>(placement.salt), chunk_x,
                               chunk_z);
  return random.next_float() < frequency;
}

const WgStructureSet* wg_structure_set_by_name(const char* name) {
  for (const WgStructureSet& set : kWgStructureSets) {
    if (std::strcmp(set.name, name) == 0)
      return &set;
  }
  return nullptr;
}

const WgStructure* wg_structure_by_name(const char* name) {
  for (const WgStructure& structure : kWgStructures) {
    if (std::strcmp(structure.name, name) == 0)
      return &structure;
  }
  return nullptr;
}

bool wg_structure_is_chunk(int64_t seed, const WgStructureSet& set, int32_t chunk_x,
                           int32_t chunk_z) {
  return is_structure_chunk_with_rings(
      seed, set, chunk_x, chunk_z,
      [](uint32_t /*placement*/, int32_t /*x*/, int32_t /*z*/) { return false; });
}

bool wg_structure_has_chunk_in_range(int64_t seed, const char* set_name, int32_t chunk_x,
                                     int32_t chunk_z, int32_t distance) {
  return has_chunk_in_range_with_rings(
      seed, set_name, chunk_x, chunk_z, distance,
      [](uint32_t /*placement*/, int32_t /*x*/, int32_t /*z*/) { return false; });
}

std::vector<WgStructureChunk> wg_structure_ring_positions(int64_t seed,
                                                          const WgStructurePlacement& placement,
                                                          WgClimateSampler& sampler) {
  if (placement.count == 0 || std::strcmp(placement.type, "minecraft:concentric_rings") != 0)
    return {};

  constexpr double kTwoPi = 6.283185307179586476925286766559;
  const int32_t distance = placement.distance;
  const int32_t count = placement.count;
  int32_t spread = placement.spread;
  WgLegacyRandom random(0);
  random.set_seed(seed);
  double angle = random.next_double() * kTwoPi;
  int32_t position_in_circle = 0;
  int32_t circle = 0;

  std::vector<WgStructureChunk> positions;
  positions.reserve(static_cast<size_t>(count));
  for (int32_t i = 0; i < count; ++i) {
    // `dist` is in chunks: `4 * distance` is the first ring's radius, and the
    // jitter is ±1.25 * distance chunks.
    const double dist = static_cast<double>(4 * distance + distance * circle * 6) +
                        (random.next_double() - 0.5) * (static_cast<double>(distance) * 2.5);
    const int32_t initial_x = static_cast<int32_t>(std::floor(std::cos(angle) * dist + 0.5));
    const int32_t initial_z = static_cast<int32_t>(std::floor(std::sin(angle) * dist + 0.5));

    // `random.fork()`: `new LegacyRandomSource(this.nextLong())`.
    WgLegacyRandom biome_search(random.next_long());
    int32_t found_x = 0;
    int32_t found_z = 0;
    const bool found = find_biome_horizontal(sampler, placement, biome_search, initial_x * 16 + 8,
                                             initial_z * 16 + 8, 112, &found_x, &found_z);
    if (found) {
      positions.push_back(WgStructureChunk{wg_structure_floor_div(found_x, 16),
                                           wg_structure_floor_div(found_z, 16)});
    } else {
      positions.push_back(WgStructureChunk{initial_x, initial_z});
    }

    angle += kTwoPi / static_cast<double>(spread);
    if (++position_in_circle != spread)
      continue;
    position_in_circle = 0;
    spread += 2 * spread / (++circle + 1);
    spread = std::min(spread, count - i);
    angle += random.next_double() * kTwoPi;
  }
  return positions;
}

WgStructurePlacementState::WgStructurePlacementState(const WgOverworldGenerator& generator)
    : seed_(generator.seed()), sampler_(std::make_unique<WgClimateSampler>(generator.state())),
      rings_(sizeof(kWgStructurePlacements) / sizeof(kWgStructurePlacements[0])),
      rings_known_(rings_.size(), 0) {}

WgStructurePlacementState::~WgStructurePlacementState() = default;

const std::vector<WgStructureChunk>&
WgStructurePlacementState::ring_positions(uint32_t placement_index) const {
  static const std::vector<WgStructureChunk> kEmpty;
  if (placement_index >= rings_.size())
    return kEmpty;
  std::lock_guard<std::mutex> lock(mutex_);
  if (rings_known_[placement_index] == 0) {
    rings_[placement_index] =
        wg_structure_ring_positions(seed_, kWgStructurePlacements[placement_index], *sampler_);
    rings_known_[placement_index] = 1;
  }
  return rings_[placement_index];
}

bool WgStructurePlacementState::is_structure_chunk_at(const WgStructureSet& set, int32_t chunk_x,
                                                      int32_t chunk_z) const {
  return is_structure_chunk_with_rings(
      seed_, set, chunk_x, chunk_z, [this](uint32_t placement_index, int32_t x, int32_t z) {
        return contains_chunk(ring_positions(placement_index), x, z);
      });
}

bool WgStructurePlacementState::is_structure_chunk(const WgStructureSet& set, int32_t chunk_x,
                                                   int32_t chunk_z) const {
  return is_structure_chunk_at(set, chunk_x, chunk_z);
}

bool WgStructurePlacementState::has_structure_chunk_in_range(const char* set_name, int32_t chunk_x,
                                                             int32_t chunk_z,
                                                             int32_t distance) const {
  return has_chunk_in_range_with_rings(seed_, set_name, chunk_x, chunk_z, distance,
                                       [this](uint32_t placement_index, int32_t x, int32_t z) {
                                         return contains_chunk(ring_positions(placement_index), x,
                                                               z);
                                       });
}

} // namespace mc::world
