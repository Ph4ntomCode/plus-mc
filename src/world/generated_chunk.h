// SPDX-License-Identifier: GPL-3.0-or-later
//
// The bridge from the generator's stages (world/worldgen_chunk.h) to the chunk
// model (world/chunk.h): one generated column, ready for the world stores, the
// light engine and the chunk encoder.
//
// The overworld stage's output is a 16 x 16 x 384 grid of block codes (the
// noise stage's ten, rewritten by the surface rules into the biome's own
// materials) and the biome stage's 24 sections of 4 x 4 x 4 biome names; this is
// where both become a `Chunk` — a palette per section from the codes and names
// the section actually holds, heightmaps current with the blocks, ticking counts
// recounted. Everything semantic (which code is which state, what a column's
// height derives from) is fixed here; the world store above it adds no decisions
// of its own.
//
// `fill_generated_carved_chunk` is `buildSurface` then `applyCarvers`: the
// boundary the vanilla probe dumps (`tests/fixtures/worldgen/carved_*`) and the
// parity test compare at. The generator's served column is one stage further
// (`fill_generated_decorated_chunk`); the carved arm stays as the parity arm
// because only it can be compared against a vanilla dump. The decorated stage
// applies the feature kinds and then every structure kind with a piece builder
// (`world/worldgen_structure_piece.h`): shipwreck, swamp hut, igloo, buried
// treasure, desert pyramid, jungle temple, mineshaft, stronghold, ocean ruin,
// ruined portal, village, ocean monument, woodland mansion, pillager outpost,
// trail ruins, ancient city, nether fortress, bastion remnant and end city.
// The last three are Nether/End structures: the dimension bridge applies them
// in their own dimensions, while the overworld stage applies the rest;
// `docs/WORLDGEN_PARITY.md` records that split and how the parity test bounds
// it. The Nether dimension bridge also runs the carved stage before decoration.
//
// `fill_generated_surface_chunk` stops one stage earlier — after `buildSurface`
// and before `applyCarvers` — the boundary the `surface_*` dumps pin. It stays
// a separate entry point so those goldens keep meaning what they meant
// (tests/worldgen_test.cpp's bridge check).
//
// `fill_generated_chunk` is the same bridge with the surface pass *not* run:
// the raw noise-stage column, which the milestone-K fixtures
// (`noise_seed42_*`) and the decoration stage's golden dumps were cut against.
// It stays a separate entry point so those goldens keep meaning what they
// meant (tests/worldgen_test.cpp, tests/worldgen_feature_test.cpp).
#pragma once

#include "world/chunk.h"

namespace mc::world {

class WgOverworldGenerator;
struct WgDecorationStats;

// Fills `out` with chunk (cx, cz) of `generator`'s overworld stage: the section
// stack (min_y -64, 24 sections), each section's block container expanded into
// cells from the ten-state code map, the biome source's per-cell biomes, and
// the two derives (heightmaps current, `random_tick_blocks` recounted). `out`
// is replaced wholesale. Deterministic: the same generator and chunk always
// produce byte-identical columns.
void fill_generated_chunk(const WgOverworldGenerator& generator, int cx, int cz, Chunk& out);

// The same column with vanilla's overworld surface rules applied — the stage
// `NoiseBasedChunkGenerator.buildSurface` produces. Biomes, heightmaps and
// ticking counts are filled exactly as above; only the block codes differ.
void fill_generated_surface_chunk(const WgOverworldGenerator& generator, int cx, int cz,
                                  Chunk& out);

// The same column after `applyCarvers` too: caves and canyons cut out of the
// surface column, with the carver's own re-top where it opens a grass ceiling.
// This is the parity arm (`PLUSMC_WORLDGEN_STAGE=carved`). Biomes are unchanged by carving (the
// game carves blocks, not biome cells); heightmaps, ticking counts and sky light are rebuilt from
// the carved cells, so a cave mouth's lowered surface is what the client's heightmaps see.
void fill_generated_carved_chunk(const WgOverworldGenerator& generator, int cx, int cz, Chunk& out);

// The carved column with the feature (decoration) stage applied: `--worldgen`
// serves this. The stage's random stream is the port's own (worldgen_feature.h
// says so), so the column is vanilla's *shape* — the same terrain, ores, trees,
// lakes and vegetation — but not a byte-for-byte match of a vanilla dump.
// Heightmaps, ticking counts and both light layers are rebuilt from the
// decorated cells, so a lava lake the feature stage leaves is lit when sent.
void fill_generated_decorated_chunk(const WgOverworldGenerator& generator, int cx, int cz,
                                    Chunk& out);

// The noise and biome stages of a non-overworld table, with no surface rules,
// carvers, features or structures: the table's own `min_y`/`height` section
// stack, code 1 as its default block (netherrack, end stone), its default fluid,
// and its biome cells. The overworld entry points above remain the only ones
// with decoration; this is the bridge a Nether/End surface stage will build on.
void fill_generated_dimension_chunk(const WgOverworldGenerator& generator, int cx, int cz,
                                    Chunk& out);

// The same dimension column after its surface stage: for the Nether this is
// `SurfaceRuleData.nether()` (bedrock floor/roof, netherrack, lava holes, soul
// soil, basalt, blackstone, nylium and wart blocks). The End's surface is its
// noise-stage default block, so it uses the entry point above.
void fill_generated_dimension_surface_chunk(const WgOverworldGenerator& generator, int cx, int cz,
                                            Chunk& out);

// The dimension column after the table's carver stage too. Kept as a separate
// stage boundary for parity work: the Nether's carved caves are what the
// feature stage sees, and comparing them against a vanilla carved dump is the
// only way to tell a carver difference from a feature difference. The End has
// no carver stage, so this is the same column as the surface entry point there.
void fill_generated_dimension_carved_chunk(const WgOverworldGenerator& generator, int cx, int cz,
                                           Chunk& out);

// The column the dimension is served from: surface rules (Nether), the
// dimension's biome feature stage (glowstone, vegetation and ores in the
// Nether; the central platform, spikes and outer islands in the End), and the
// derives rebuilt from what the features left. The End is expanded from its
// 128-high noise table to the dimension type's 256-high section stack before
// decoration, so its level geometry is not clipped. The feature stream is the
// port's own, as it is for the overworld.
void fill_generated_dimension_decorated_chunk(const WgOverworldGenerator& generator, int cx, int cz,
                                              Chunk& out, WgDecorationStats* stats = nullptr);

} // namespace mc::world
