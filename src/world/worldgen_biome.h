// SPDX-License-Identifier: GPL-3.0-or-later
//
// The biome stage of the overworld: `MultiNoiseBiomeSource` over the
// `minecraft:overworld` multi-noise preset. A climate sample — the six router
// entries temperature, vegetation, continents, erosion, depth and ridges at a
// quart cell's block origin — is quantized (`Climate.target`) and searched
// against the preset's parameter list (`Climate.ParameterList.findValue`); the
// biome of the nearest parameter point is the answer.
//
// The parameter list itself is generated data (world/worldgen_biome_table.h),
// dumped from the game's own registry, so this header only carries the sample
// shape and the two pure steps around the search.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mc::world {

// One `Climate.TargetPoint`: the six quantized climate coordinates, in
// `Sampler.sample`'s order (temperature, humidity, continentalness, erosion,
// depth, weirdness).
struct WgClimateTarget {
  int64_t temperature = 0;
  int64_t humidity = 0;
  int64_t continentalness = 0;
  int64_t erosion = 0;
  int64_t depth = 0;
  int64_t weirdness = 0;
};

// `Climate.quantizeCoord(float)`: `(long)(coord * 10000.0f)`. Takes the float
// `Sampler.sample` narrows each density value to, and multiplies in binary32,
// so the truncation sees the same product the game's does.
int64_t wg_quantize_climate_coord(float coord);

// `Climate.target(...)`: quantize one sample's six climate values.
WgClimateTarget wg_climate_target(float temperature, float humidity, float continentalness,
                                  float erosion, float depth, float weirdness);

// `Climate.ParameterList.findValue(target)`: the biome of the nearest parameter
// point in the overworld preset. Ties go to the earlier point in the preset's
// order (see worldgen_biome.cpp for the search and its one deliberate
// divergence — no `RTree.lastResult` warm start, which would make the answer
// depend on which chunks a worker happened to fill before).
const char* wg_select_biome(const WgClimateTarget& target);

// `TheEndBiomeSource.getNoiseBiome`: the central island is `the_end`; every
// other section is classified from `end_islands` at its doubled origin.
const char* wg_select_end_biome(int32_t section_x, int32_t section_z, double erosion);

// `MultiNoiseBiomeSource` over the `minecraft:nether` preset. Its five
// parameter points are all point intervals, so the nearest-point search is a
// five-row scan rather than the overworld's RTree.
const char* wg_select_nether_biome(const WgClimateTarget& target);

// The canonical pointer for one biome name: every name the preset's table
// carries (and every equal string) maps to the same address, so two biomes can
// be compared with `==` instead of a `strcmp`. `wg_select_biome` already
// returns a canonical pointer; a caller holding its own name (the surface
// rule tree's "is this biome" conditions) interns it once when it builds them.
// A name the preset does not carry (a void world's `minecraft:the_void`) is its
// own canonical form: the pointer it was given.
const char* wg_canonical_biome(const char* name);

// The row that selection picked, and `Climate.ParameterPoint.fitness` of one
// row against a target. Together they let a caller (the worldgen test's own
// linear scan) pin the search to the minimum without depending on which of
// several tied rows won it.
size_t wg_select_biome_index(const WgClimateTarget& target);
int64_t wg_biome_fitness(const WgClimateTarget& target, size_t parameter_index);

// The per-thread memo's own accounting, as one line for the shutdown report:
// hits, cold searches and the hit rate they imply, how many entries the live
// threads are holding, and how often a full memo was dropped (worldgen_biome.cpp
// says why it counts itself). Empty when this process never asked a biome
// question — a void world asks none, and a line of zeroes would read as a memo
// that ran and missed everything.
std::string wg_biome_memo_report();

} // namespace mc::world
