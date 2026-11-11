// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `minecraft:fossil` feature and the template-shaped half of the structure
// engine. The pack carries sixteen `structure/fossil/*.nbt` templates: eight
// skeletons and, for each, an ore overlay with the same footprint. Vanilla's
// `FossilFeature.place` picks a rotation and a template pair, sinks the fossil
// 15-24 blocks under the lowest `OCEAN_FLOOR_WG` column of its own footprint,
// rejects the site when more than `max_empty_corners_allowed` of the template
// box's corners are air/water/lava, then places the skeleton and the overlay
// through the templates' processors (`block_rot` integrity, and the diamonds
// overlay's one `block_match` rule) and the `features_cannot_replace`
// protected-block processor. This module is that path, and nothing else; the
// structure piece builders live in worldgen_structure_piece.cpp (docs/PHASE4.md).
//
// The port keeps the vanilla draw order — rotation (`nextInt(4)`), template
// index, depth (`nextInt(10)`), the palette draw, then one `nextFloat` per
// template block per pass — from the decoration stream this port already uses
// (`WgRandom`). The sequence is this port's own, as every feature kind's is.
//
// Two honest limits, both inherited from the stage's chunk confinement:
//
//   * The height scan and the empty-corner scan read neighbouring columns,
//     which this stage does not own. An out-of-column footprint column answers
//     with the nearest own column's height (vanilla asks
//     `LevelReader.getHeight`), and an out-of-column corner is counted as
//     *not* empty rather than as air (vanilla reads the neighbour's block).
//     Writes outside the column are dropped, the same rule every kind here
//     follows.
//   * The template's block entities are not carried. The fossil templates hold
//     none, so this loses nothing today; a template that held one would need
//     the block-entity path the carried structures still wait on.
#pragma once

#include <cstdint>

#include "world/chunk.h"
#include "world/worldgen_fossil_table.h"
#include "world/worldgen_random.h"

namespace mc::world {

// `StructureTemplate.getSize(rotation)` and
// `getZeroPositionWithTransform(pos, Mirror.NONE, rotation)` for a template of
// the given unrotated size. The zero position is what the rotated template's
// origin is offset by so that its block positions land back inside the
// unrotated footprint; exposed so the fossil test can pin the table against
// values read from the 26.2 server's own `StructureTemplate`.
struct WgFossilTransform {
  int size_x = 0;
  int size_y = 0;
  int size_z = 0;
  int zero_x = 0;
  int zero_z = 0;
};

WgFossilTransform wg_fossil_transform(int size_x, int size_y, int size_z, uint32_t rotation);

// `FossilFeature.place` for one application at world position (x, y, z).
// Returns the number of blocks the application actually changed. Deterministic
// for a given chunk and `rng` state.
uint32_t place_fossil(Chunk& chunk, const WgFossilConfig& config, WgRandom& rng, int x, int y,
                      int z);

} // namespace mc::world
