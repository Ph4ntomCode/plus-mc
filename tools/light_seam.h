// SPDX-License-Identifier: GPL-3.0-or-later
//
// The generation-time light seam, measured once and shared by its two readers:
// `tests/world_light_test.cpp` (the pinned two-column assertion) and
// `bench/light_seam.cpp` (the multi-seed benchmark and its baseline record).
//
// The send path no longer relights a freshly generated column:
// `world/generated_chunk.cpp` runs the engine's own light pass over the column's
// own terrain, with its halo marked unmodelled (`LightEnvironment::centre_only`).
// Both layers are kept: the sky is what a client sees, and the block layer is
// what a decorated column's lava or glowstone emits.
// That is exact wherever the light comes from the column itself and blind to
// light that enters across a chunk border from a neighbour's source; `seam_measure`
// turns the difference into a count by generating a column, keeping the light
// generation wrote, relighting the same column through `relight_world_chunk`
// over its eight generated neighbours, and walking both layers cell for cell.
//
// If the two readers ever define the comparison differently, the benchmark stops
// being a track of the test's pinned numbers. That is why the tally, the pinned
// expectations and the relight step live here rather than in either caller.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "world/chunk.h"
#include "world/light.h"
#include "world/memory_world.h"
#include "world/world_light.h"
#include "world/worldgen_chunk.h"

namespace mc::bench {

// One layer's worth of disagreement between the light generation wrote and the
// engine's own relight. `cells` counts every cell of the column in that layer.
struct SeamTally {
  long long cells = 0;
  long long agree = 0;
  long long brighter = 0; // generation wrote more light than the relight
  long long dimmer = 0;   // ...and less
  int max_brighter = 0;
  int max_dimmer = 0;
  long long above_surface = 0;   // disagreements at y >= the column's own surface
  long long border = 0;          // ...on a cell of the chunk's edge, the half the
                                 // ring across the border can reach
  long long interior = 0;        // ...everywhere else in the column
  long long brighter_border = 0; // ...brighter and on the edge
  long long in_opaque = 0;       // ...inside an opaque block

  long long disagreements() const {
    return brighter + dimmer;
  }

  // The share of the layer's cells the approximation gets wrong, in [0, 1]. The
  // headline number a benchmark record tracks.
  double error_fraction() const {
    return cells == 0 ? 0.0 : static_cast<double>(disagreements()) / static_cast<double>(cells);
  }

  void add(const SeamTally& other) {
    cells += other.cells;
    agree += other.agree;
    brighter += other.brighter;
    dimmer += other.dimmer;
    max_brighter = std::max(max_brighter, other.max_brighter);
    max_dimmer = std::max(max_dimmer, other.max_dimmer);
    above_surface += other.above_surface;
    border += other.border;
    interior += other.interior;
    brighter_border += other.brighter_border;
    in_opaque += other.in_opaque;
  }
};

// What one column's measurement produced, including the two premises the
// comparison rests on. A column with `measured == false` could not be generated
// or read back, and its tallies must not be counted.
struct SeamColumn {
  bool measured = false;
  bool generated_light = false; // the column carried light before the relight
  bool relit_sky = false;       // the engine actually moved at least one sky level
  SeamTally sky;
  SeamTally block;
};

// One column's two layers, walked cell for cell. `generated` keeps the light the
// generator wrote; `relit` is the same column after `relight_world_chunk`.
inline SeamTally seam_tally(mc::world::Chunk& generated, const mc::world::Chunk& relit,
                            mc::world::LightLayer layer) {
  SeamTally tally;
  const int min_y = generated.min_section_y * 16;
  const int max_y = min_y + static_cast<int>(generated.sections.size()) * 16;
  for (int x = 0; x < 16; ++x) {
    for (int z = 0; z < 16; ++z) {
      const int surface = generated.height_at(mc::world::HeightmapType::WorldSurface, x, z);
      const bool edge = x == 0 || x == 15 || z == 0 || z == 15;
      for (int y = min_y; y < max_y; ++y) {
        const int made = mc::world::chunk_light_level(generated, x, y, z, layer);
        const int engine = mc::world::chunk_light_level(relit, x, y, z, layer);
        ++tally.cells;
        if (made == engine) {
          ++tally.agree;
          continue;
        }
        if (made > engine) {
          ++tally.brighter;
          if (edge)
            ++tally.brighter_border;
          tally.max_brighter = std::max(tally.max_brighter, made - engine);
        } else {
          ++tally.dimmer;
          tally.max_dimmer = std::max(tally.max_dimmer, engine - made);
        }
        if (y >= surface)
          ++tally.above_surface;
        if (edge)
          ++tally.border;
        else
          ++tally.interior;
        if (mc::world::light_is_opaque(generated.block_state_at(x, y, z)))
          ++tally.in_opaque;
      }
    }
  }
  return tally;
}

// Generate the column at (`cx`, `cz`), relight it in the world that holds it —
// the eight neighbours are generated on demand, the same post-carver terrain
// the encoder would send — and compare both layers against what generation wrote.
inline SeamColumn
seam_measure(const std::shared_ptr<const mc::world::WgOverworldGenerator>& generator, int cx,
             int cz) {
  // A fresh world per column, so no column's relight is influenced by another's
  // stored neighbours.
  mc::world::MemoryWorld world(-4, 24, generator);
  auto generated = world.load_chunk(cx, cz);
  if (!generated)
    return {};
  SeamColumn column;
  // The premise of the send-path shortcut: the column already carries light, so
  // neither `chunk_has_light` nor the 3x3 ring runs on the send path.
  column.generated_light = mc::world::chunk_has_light(*generated);

  const mc::world::LightChange change = mc::world::relight_world_chunk(cx, cz, world);
  column.relit_sky = std::any_of(change.sky_changed.begin(), change.sky_changed.end(),
                                 [](std::uint64_t slot) { return slot != 0; });
  const auto relit = world.load_chunk(cx, cz);
  if (!relit)
    return column;
  column.measured = true;
  column.sky = seam_tally(*generated, *relit, mc::world::LightLayer::kSky);
  column.block = seam_tally(*generated, *relit, mc::world::LightLayer::kBlock);
  return column;
}

// The two columns the worldgen fixtures pin: seed 42's ocean column at (0,0) and
// land column at (-1,-1). The tallies are `world_light_test`'s pinned
// expectations, moved here so the benchmark cross-checks the same numbers it
// tracks. Update them only with the recorded baseline, deliberately.
inline constexpr long long kPinnedSeed = 42;

struct SeamPinnedSky {
  int cx;
  int cz;
  long long agree;
  long long brighter;
  long long dimmer;
  long long border;   // dim/bright cells on the chunk's own edge
  long long interior; // ...and the spill that reached further in
  long long in_opaque;
};

// Measured on the current tree (seed 42, the store's decorated column: surface
// rules, carvers and the feature stage) with the centre-only generation pass.
// Both columns are exact above their own surface and neither has a brighter
// cell: the pass seeds only its own sources. Everything counted is spill that
// enters across a chunk border from a neighbour's source, so it is concentrated
// on the edge — 117 of the ocean column's 167 cells and 139 of the land
// column's 296 — with the rest up to 14 blocks in, where the spill had reach.
// The ocean is not exact here because the decorated ring is not the all-water
// ring the carved-stage measurement assumed: obstruction and emitters now stand
// within the 15-block reach. The worst shortfall on either is 8 levels.
inline constexpr SeamPinnedSky kPinnedSky[] = {
    {0, 0, 98137, 0, 167, 117, 50, 0},    // ocean, decorated ring
    {-1, -1, 98008, 0, 296, 139, 157, 0}, // land, decorated ring
};

// Across both pinned columns: the local pass never writes more light than the
// engine's (it seeds only the column's own sources), so the brighter bound is 0,
// and the worst dim cell is 8 levels short where a neighbour's spill reaches in.
inline constexpr int kPinnedMaxBrighter = 0;
inline constexpr int kPinnedMaxDimmer = 8;

// The block layer, measured the same way and pinned for the same reason. A
// decorated column contains emitters (a lava lake, a glowstone cluster), so the
// generation pass keeps the block array it computes instead of dropping it: a
// served column is trusted as lit (`chunk_has_light`), and a dropped layer would
// reach a client as dark lava. The shortfall is the same shape as the sky's and
// larger, because a cave carries a neighbour's lava light further than open sky
// carries its: the ocean column is short 1,621 cells (1,270 of them interior),
// the land column 107. Nothing is brighter, and nothing is lit inside an opaque
// block.
inline constexpr SeamPinnedSky kPinnedBlock[] = {
    {0, 0, 96683, 0, 1621, 351, 1270, 0},
    {-1, -1, 98197, 0, 107, 69, 38, 0},
};

inline constexpr int kPinnedMaxBlockDimmer = 14;

} // namespace mc::bench
