// SPDX-License-Identifier: GPL-3.0-or-later
//
// The carver stage: `WorldCarver`'s cave, canyon and nether-cave shapes and the
// driver `NoiseBasedChunkGenerator.applyCarvers` runs them with.
//
// Vanilla carves a chunk *after* `buildSurface` and *before* decoration. The
// driver is not local: for the chunk being generated it walks the 17 x 17
// square of source chunks around it, takes each source chunk's biome at its own
// minimum corner, and for every configured carver on that biome's list reseeds
// a `WorldgenRandom` with `seed + index` and lets it carve into the *target*
// chunk. A carver whose tunnels miss the target is pruned by `canReach`, so the
// per-chunk cost is the start rolls plus the ellipsoids that actually reach.
//
// The carving itself writes the noise stage's ten codes plus the surface
// stage's materials: a replaced cell becomes the aquifer's substance at that
// point (air, water or lava), or lava below the carver's own `lava_level`.
//
// `CarvingContext.topMaterial` is carried as a callback: when a tunnel opens a
// grass ceiling, vanilla asks the surface system to re-top the dirt underneath
// (`SurfaceSystem.topMaterial`), and `WgSurfaceTopMaterial` in
// worldgen_surface.h is that tree evaluated at one cell.
//
// Deliberately not modelled: `CarverDebugSettings` (`SharedConstants.DEBUG_CARVERS`
// is off; the pack's configs carry debug states but not debug mode). The nether
// carver is table-carried and driven by the dimension bridge.
#pragma once

#include <cstdint>

namespace mc::world {

class WgAquifer;

// One chunk's inputs. `codes` is 16 x 16 x `height` in the noise stage's layout
// (`(x*16 + z) * height + (y - min_y)`), holding the surface stage's output,
// and is rewritten in place. `aquifer` must be the chunk's own (the same one
// `NoiseBasedChunkGenerator.applyCarvers` reads through `noiseChunk.aquifer()`).
struct WgCarverChunk {
  int32_t chunk_x = 0;
  int32_t chunk_z = 0;
  int32_t min_y = -64;
  int32_t height = 384;
  uint8_t* codes = nullptr;
  WgAquifer* aquifer = nullptr;
  void* user = nullptr;
  const char* (*biome_at_quart)(void* user, int32_t quart_x, int32_t quart_y,
                                int32_t quart_z) = nullptr;
  // `CarvingContext.topMaterial` at one cell (see the file comment).
  // `top_material_user` is that system's own handle; unset means the re-top is
  // skipped, which is what a caller with no surface stage gets.
  void* top_material_user = nullptr;
  bool (*top_material)(void* user, int32_t block_x, int32_t block_y, int32_t block_z,
                       bool under_fluid, uint8_t* out) = nullptr;
};

// `NoiseBasedChunkGenerator.applyCarvers` for one chunk, minus the
// `StructureManager`/`WorldGenRegion` plumbing (the driver only needs the source
// chunks' biomes). Returns the number of (source chunk, carver) pairs whose
// start roll passed and whose `carve` ran.
uint32_t wg_apply_carvers(int64_t seed, const WgCarverChunk& chunk);

// The carver list one biome owns, in the biome JSON's order (the index is part
// of the seed). Empty when the biome is not in the table or lists no carvers.
struct WgCarverRef {
  const char* name;
};
uint32_t wg_biome_carvers(const char* biome, const char** out, uint32_t capacity);

} // namespace mc::world
