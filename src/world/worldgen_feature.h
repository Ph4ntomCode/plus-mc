// SPDX-License-Identifier: GPL-3.0-or-later
//
// The overworld's **feature (decoration) stage**, milestone K's open piece.
//
// `worldgen_chunk.{h,cpp}` reproduces the noise, biome and surface stages;
// this is the stage vanilla runs after them: for each chunk, walk the eleven
// `GenerationStep.Decoration` steps, and for every placed feature the chunk's
// biomes list at that step, run its placement modifiers (`count`,
// `rarity_filter`, `in_square`, `height_range`, `heightmap`, `random_offset`,
// `biome`) to turn the chunk corner into candidate positions, then apply the
// configured feature at each.
//
// What is applied, exactly:
//
//   * **ore** (`OreFeature.doPlace`) — the direction ellipse, the size-long
//     spine with its radius shrink, the prune pass, and the cellwise ellipsoid
//     with the target predicate and the air-exposure discard — and **scattered
//     ore** (`ScatteredOreFeature.place`), the same targets at `size + 1`
//     bounded triangular offsets from the origin;
//   * **block column** (`BlockColumnFeature`) — each layer's height sampled from
//     its `IntProvider` (`constant`, `uniform`, `biased_to_bottom`, weighted
//     list), the column truncated by its own `allowed_placement` predicate from
//     the tip (or from the base when `prioritize_tip`), and every layer's state
//     sampled from its `BlockStateProvider` (a simple state, a weighted list, or
//     a `randomized_int_state_provider`'s drawn integer property);
//   * **simple block** (`SimpleBlockFeature`), **block pile**
//     (`BlockPileFeature`), **disk** (`DiskFeature`), **spring**
//     (`SpringFeature`);
//   * **monster room** (`MonsterRoomFeature`) — the sanity scan (a solid floor
//     and ceiling and one to five holes in the walls), then the room's own
//     carve: cobblestone (mossy on three of the four floor rows) for the shell,
//     cave air inside, up to two chests, and a spawner at the origin. Its
//     block entities are not modelled (an empty spawner, a chest with no loot
//     table), which the file says where it matters;
//   * **freeze top layer** (`SnowAndFreezeFeature`) — the whole 16x16 chunk:
//     for each column the first free cell (`motion_blocking_height`) gets a
//     snow layer when the biome there precipitates as snow, and the block the
//     snow lands on gets `snowy=true` when it carries that property; a water
//     block at that cell becomes ice when the biome is cold enough to snow.
//     `Biome.shouldSnow`/`shouldFreeze` also gate on block light < 10 — at
//     generation time no block-light source exists yet, so that reading is 0
//     and the gate is satisfied by construction (the runtime says so); the
//     tool is dispatched by the bare `biome` placement, so it runs once per
//     chunk and consumes no random draws;
//   * **vines** (`VinesFeature`) — an air cell with a full neighbour face gets
//     one `minecraft:vine` with that face set, trying vanilla's own direction
//     order (up, north, south, west, east); the face test is the same
//     motion-blocking approximation the two support rules above use;
//   * **seagrass** (`SeagrassFeature`) — one candidate cell per application in
//     the origin's own 8x8 neighbourhood (per horizontal axis), at the column's
//     `OCEAN_FLOOR` height, on a sturdy support; a probability draw picks
//     `tall_seagrass`'s two halves over the one-block plant, and the cell must
//     hold water either way;
//   * **trees** (`TreeFeature`) over the generator's decoded trunk and foliage
//     placers — straight/fancy/dark-oak/mega-jungle/giant trunks and
//     blob/fancy/pine/spruce/acacia/dark-oak/jungle/cherry/bush/mega-pine
//     crowns, with each placer's own `shouldSkipLocation`;
//   * **fallen trees** (`FallenTreeFeature`) — the stump at the origin, then a
//     horizontal direction, a log length (the config's own provider sampled and
//     reduced by two) and a start two or three cells out; that start walks one
//     cell up and up to five down for a valid tree cell over solid ground, and
//     the fall is written only when every cell of it is such a cell with no run
//     of three unsupported ones. Each fallen log's `axis` becomes the fall's
//     own. The stump's `trunk_vine` decorator and the logs' `attached_to_logs`
//     one (a weighted mushroom above a log, at the config's own probability)
//     are applied with vanilla's draws and states; the section names the two
//     approximations it makes (the decorated set's iteration order, and
//     `markAboveForPostProcessing`) and why the test pins this kind on a
//     constructed chunk: its child placement is a `would_survive` sapling
//     predicate, and the chunk this stage is fed carries the noise and biome
//     stages but not the surface one, so the soil a sapling roots in has to be
//     constructed for the kind to be reachable at all;
//   * the three **selectors** (`random_selector`, `simple_random_selector`,
//     `random_boolean_selector`), recursing into a child placed feature's own
//     placement before its configured feature;
//   * **multiface growth** (`MultifaceGrowthFeature`) — glow lichen's
//     `multiface_growth`: one growth block per application, placed against the
//     first neighbour in `can_be_placed_on` the config's valid directions reach
//     (from the origin, or one cell out along a direction, vanilla's own search
//     loop), with the face set, `waterlogged` when the cell held source water,
//     and the config's `chance_of_spreading` roll that follows the default
//     `MultifaceSpreader`'s SAME_POSITION / SAME_PLANE / WRAP_AROUND order.
//     `canAttachTo` is the motion-blocking full-cube approximation `vines` and
//     the snow support rule use, which docs/PHASE4.md names;
//   * **ice spike** (`SpikeFeature`, the `minecraft:spike` kind) — the walk down
//     to the first non-air cell, the config's `can_place_on` gate there, the
//     shrunken-ellipse stack above and below the start (its border cells thinned
//     by the 0.75 roll) and the trunk that carries the spike down through air or
//     the `ice_spike_replaceable` tag until it meets a block it may not replace;
//   * **geode** (`GeodeFeature`) — three or four distribution points, the
//     optional crack, the noise-displaced shell sum over every cell of the
//     config's real `[-16, 16]` box, and the five layers (air filling, amethyst,
//     budding amethyst, calcite, smooth basalt), then the inner placements
//     against the first direction `BuddingAmethystBlock.canClusterGrowAtState`
//     accepts. Its noise is this port's `WgNormalNoise` seeded from the world
//     seed (vanilla builds a legacy-`RandomSource` one), which the section says
//     out loud.
//
// The placement modifiers applied are `count`, `count_on_every_layer`,
// `rarity_filter`, `in_square`, `height_range`, `heightmap`, `biome`,
// `random_offset`, `surface_water_depth_filter`, `block_predicate_filter`
// (over all nine predicate kinds the generator flattens) and
// `noise_threshold_count`. Every configured feature in the 26.2 pack is applied.
// The two **fossils** (`minecraft:fossil`) place structure templates and live in
// `world/worldgen_fossil.{h,cpp}` — the fossil-shaped half of the template
// engine the carried structures still need. A kind the stage *does* apply whose
// config the generator could not flatten (an unparsed state provider, say) is
// counted in `WgDecorationStats::skipped_kind` rather than dropped, so a
// carried feature never disappears silently.
//
// Two honest approximations, both named rather than hidden:
//
//   * **The random sequence is not vanilla's.** Vanilla seeds decoration from
//     the chunk's `WorldgenRandom` with a salt per step and feature. This port
//     derives a deterministic per-(feature, step, chunk) stream from the
//     generator's own `named_random`, so a chunk is reproducible but is not
//     the chunk a vanilla server would build. Decoration is therefore *not*
//     byte-comparable to a vanilla dump the way the noise/surface stages are.
//   * **A feature is confined to the chunk it is decorating.** A candidate
//     position that falls outside is dropped (counted), where vanilla would
//     place into the neighbour. With ores this only trims the edge; a tree at
//     the chunk boundary loses the leaves that would hang over it.
//   * **`count_on_every_layer`** uses the bounded spread the source names
//     (`CountOnEveryLayerPlacement` is deprecated in vanilla and its own
//     `findOnGroundYPosition` layer walk is not modelled).
#pragma once

#include <cstdint>

#include "world/chunk.h"
#include "world/worldgen_dimension.h"
#include "world/worldgen_feature_table.h"
#include "world/worldgen_random.h"

namespace mc::world {

class WgOverworldGenerator;

// What one chunk's decoration did, so a caller (or a test) can see that the
// carried-but-unapplied kinds are counted and not silently skipped.
// The kinds `WgFeatureKind` can hold, as a fixed array bound (the generated
// header's own `kWgFeatureKindCount`; worldgen_feature.cpp static_asserts they
// agree). One of them, `kOther`, is by definition never applied.
constexpr uint32_t kFeatureKindSlots = kWgFeatureKindCount;

struct WgDecorationStats {
  uint32_t steps = 0;      // decoration steps walked
  uint32_t considered = 0; // placed-feature applications considered
  uint32_t applied = 0;    // of those, of an applied feature kind
  // Of `applied`, how many were this kind (`WgFeatureKind`'s own order), so a
  // caller can see that a kind *is* dispatched rather than only that some kind
  // was.
  uint32_t applied_by_kind[kFeatureKindSlots] = {};
  uint32_t skipped_kind = 0;     // of those, a kind not applied yet
  uint32_t skipped_modifier = 0; // of those, using a modifier not applied yet
  uint32_t positions = 0;        // candidate positions the placement produced
  uint32_t out_of_chunk = 0;     // candidate positions dropped outside the chunk
  uint32_t blocks_placed = 0;    // blocks the features wrote
};

// Applies the feature stage to an already noise- and biome-filled chunk (the
// output of `fill_generated_chunk`). Deterministic for a given generator and
// chunk. `stats` may be null.
//
// `region` is the optional 3x3 neighbourhood, in row-major order around
// `chunk` (dz -1..1, dx -1..1), that the feature stage reads and writes. Vanilla
// runs features against a `WorldGenRegion`, not one chunk: a vein, tree or disk
// can cross a border, and a feature at a border sees the neighbour's blocks.
// Passing the region makes this call reproduce that; leaving it null keeps the
// old single-chunk entry point. The caller owns the chunks and must settle leaf
// distances itself when a region is supplied.
void decorate_generated_chunk(const WgOverworldGenerator& generator, Chunk& chunk,
                              WgDecorationStats* stats = nullptr, Chunk* const* region = nullptr);

// The served Overworld column's pre-`FeatureSorter` stream, kept byte-stable
// for the pinned served-world parity. Dimension generation uses the region-aware
// entry point above instead.
void decorate_generated_chunk_legacy(const WgOverworldGenerator& generator, Chunk& chunk,
                                     WgDecorationStats* stats = nullptr);

// Which feature stream a dimension's generated column must use. The served
// Overworld is pinned to the legacy stream; every other dimension uses the
// region-aware stream. Exposed so the regression test can pin the mapping as
// well as the served column's output.
enum class WgDecorationStream { kLegacy, kRegionAware };
WgDecorationStream wg_decoration_stream(WgDimensionKind dimension);

// The per-step `FeatureSorter` order the stage applies, exposed so the test can
// pin it against the order the 26.2 generator itself reports. `index` is the
// value `WorldgenRandom.setFeatureSeed` salts with, so an order bug is a parity
// bug even when every feature implementation is exact.
uint32_t wg_feature_order_length(WgDimensionKind dimension, uint32_t step);
const char* wg_feature_order_feature(WgDimensionKind dimension, uint32_t step, uint32_t index);

// `PlacedFeature.place` from an explicit origin, for a structure piece:
// `FeaturePoolElement.place` runs its placed feature at the piece cell rather
// than at a chunk corner, so the decoration stage's per-chunk entry point is the
// wrong shape for it. Returns the number of blocks the feature wrote; positions
// that fall outside `chunk` are dropped, as the server's per-chunk
// `WorldGenRegion` drops them.
uint32_t wg_place_placed_feature_at(Chunk& chunk, const char* placed_name, WgRandom& rng,
                                    int64_t world_seed, int x, int y, int z);

} // namespace mc::world
