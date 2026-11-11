// SPDX-License-Identifier: GPL-3.0-or-later
//
// The light engine (Phase 4 milestone A).
//
// Until this module the engine had no light level at all. The one thing that
// needed one — `CropBlock.hasSufficientLight` / `randomTick`'s `>= 9` gate —
// answered from the column's motion-blocking height and said so in its own
// comment ("this engine has no light engine at all"), and a torch-lit crop
// underground was the visible price. This module is that level: the two layers
// vanilla's `LevelLightEngine` keeps, computed for a chunk column from the
// blocks themselves.
//
// What it models, and from where:
//   * `BlockBehaviour.BlockStateBase.getLightDampening()` — 15 for an opaque
//     full cube, `LeavesBlock`'s 1 and `TintedGlassBlock`'s 15, and otherwise
//     the `propagatesSkylightDown` split (0 for a shape that is not a full block
//     with no fluid, 1 for everything else — which is what makes water and lava
//     cost a level and glass cost none);
//   * `BlockState.getLightEmission()` — the `lightLevel(...)` each block is
//     registered with in the decompiled `Blocks.java`, including every
//     state-shaped form vanilla has: `litBlockEmission(n)` (`lit=true`, else 0),
//     the `candles` / `pickles` / `level` / charge scales, the copper bulbs'
//     four weathering levels, `CaveVines.emission(14)` (`berries`),
//     `GlowLichenBlock.emission(7)` (`hasAnyFace`), and
//     `TrialSpawnerState.lightLevel()` / `VaultState.LightLevel` (their own
//     state properties). A row with no state dependence is a table entry; a
//     partly conditional one is a function, and the two lists are kept apart so
//     no constant can quietly swallow a condition — which is exactly how the
//     fixture caught `cave_vines` emitting 14 while picked;
//   * the propagation itself — a level-ordered flood that keeps a full 15 going
//     *straight down* through cells that cost nothing (why a vertical shaft is
//     lit to its floor and the ground under a canopy is not: leaves cost 1).
//
// What it does NOT model, all of it deliberate:
//   * **the incremental update.** Vanilla keeps a 3x3 chunk region of light
//     sections and re-lights only the cells a change can reach.
//     `compute_region_light_delta` does that over a 5x5 arena. Source-stable
//     edits are replayed by the removal/increase passes; a source move rebuilds
//     the arena's whole sky layer from the live columns, so the 3x3 is exact
//     but the sky work is the region's, not one section's.
//   * **the sky-darken term.** `Level.getRawBrightness(pos, 0)` is what the crop
//     gate asks, so nothing here dims at night; a caller that wants dusk passes
//     its own `sky_darken` to `chunk_raw_brightness`.
//   * **`isSolidRender` as a real shape test.** It is code, not data, so the
//     generated `blocksMotion()` table stands in for it — the same table, and
//     the same documented approximation, the heightmaps and entity collision
//     already use.
//   * **light sources that are entities or block entities with state this
//     engine does not carry** (a beacon's beam, an active sculk sensor's
//     shimmer, a lit trial spawner's cycle): their `lightLevel` is state-shaped
//     in a way the block-state table does not express, so they read as 0.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include "world/anvil.h"
#include "world/chunk.h"

namespace mc::world {

// `LightLayer`: the two levels vanilla keeps per section.
enum class LightLayer : uint8_t { kSky = 0, kBlock = 1 };

// `LightEngine.MAX_LEVEL`.
constexpr int kMaxLightLevel = 15;

// One light section is a 16 x 16 x 16 nibble array in the disk's own order:
// `DataLayer.getIndex(x, y, z)` is `y << 8 | z << 4 | x`, and the byte at
// `index >> 1` carries the even index in its low nibble and the odd index in its
// high one. The chunk packet and the Anvil writer already carry these arrays
// verbatim, which is why the engine computes into them instead of a parallel
// store: a lit chunk is then saved lit and sent lit with no translation.
constexpr int kLightArrayBytes = 2048;
static_assert(kLightArrayBytes == static_cast<int>(LightArray::kBytes),
              "a light array's byte view is 2,048 bytes whether or not it holds a buffer");
static_assert(kLightArrayBytes == mc::core::LightSlabPool::kSlotBytes,
              "the slab pool's slot is exactly one light array (core/slab_pool.h)");
int light_nibble_index(int x, int y, int z);
// The raw forms: any 2,048-byte buffer — a section's `LightArray` storage, a
// region's flat light buffer, a disk NBT byte array.
int light_nibble_bytes(const uint8_t* data, std::size_t size, int x, int y, int z);
void set_light_nibble_bytes(uint8_t* data, std::size_t size, int x, int y, int z, int level);
// The container forms, for every byte container the engine holds an array in:
// `LightArray` (pool-backed, chunk.h) and a plain `std::vector<uint8_t>`.
template <typename Bytes> int light_nibble(const Bytes& data, int x, int y, int z) {
  return light_nibble_bytes(data.data(), data.size(), x, y, z);
}
template <typename Bytes> void set_light_nibble(Bytes& data, int x, int y, int z, int level) {
  set_light_nibble_bytes(data.data(), data.size(), x, y, z, level);
}

// `BlockState.getLightDampening()`: the level a cell costs to pass through.
int light_dampening(const BlockStateKey& state);
// `dampening == 15`: no light passes, and a sky column stops here.
bool light_is_opaque(const BlockStateKey& state);

// `BlockState.getLightEmission()`.
int light_emission(const BlockStateKey& state);

// Both of the above, packed as `(emission << 4) | dampening`, from a *single*
// registry lookup. They are independent functions in vanilla and independent
// lookups here, but the callers that need the pair — the flood's per-palette
// table and `LightWorld`'s "can this write move light?" — asked twice for the
// same state.
int light_kinds(const BlockStateKey& state);

// A 3x3 block of chunks — the region the engine reads the world through, and
// the same 3x3 vanilla keeps, because 15 cells of reach fit inside it.
//
// It exists because the world is a slow way to answer "what block is here":
// the server's `get_block_state` takes a lock and a palette lookup per cell, and
// a whole-chunk recompute asks for hundreds of thousands of them. A caller that
// already has the chunks (the server loads the nine it needs) hands them over
// here instead, and the engine reads them directly. A null entry is air: a chunk
// that does not exist is air as far as light is concerned, and so is the ring
// beyond this one.
struct LightRegion {
  // Non-const because a column height read owes the lazy heightmaps a refresh
  // (chunk.h). The region is a view over columns the caller owns — the centre
  // chunk and the neighbours the ring borrowed from the world rather than
  // copied out of it (world/world_light.cpp).
  Chunk* chunks[9] = {};
  // The chunk the centre entry belongs to (so `at()` can be indexed by the
  // offset the engine computes from a world coordinate).
  int centre_x = 0;
  int centre_z = 0;

  Chunk* at(int dx, int dz) const {
    if (dx < -1 || dx > 1 || dz < -1 || dz > 1)
      return nullptr;
    return chunks[(dz + 1) * 3 + (dx + 1)];
  }
  void set(int dx, int dz, Chunk* chunk) {
    chunks[(dz + 1) * 3 + (dx + 1)] = chunk;
  }
};

// What `compute_chunk_light` reads the world through. `get_block` may be asked
// for cells outside the chunk (a torch on the far side of a border lights this
// chunk: vanilla keeps a 3x3 region for the same reason), and `height` is
// `LevelReader.getHeight(Heightmap.Types.MOTION_BLOCKING, x, z)` — the y one
// past the column's highest motion-blocking block, which is where full sky
// light starts.
//
// When `region` is set it answers both of those and the two functions are
// ignored (`height` then comes from the chunk's own column, which is the same
// number).
struct LightEnvironment {
  std::function<BlockStateKey(int, int, int)> get_block;
  std::function<int(int, int)> height;
  const LightRegion* region = nullptr;
  int min_y = kOverworldMinSectionY * 16;
  int max_y = (kOverworldMinSectionY + kOverworldSectionCount) * 16;
  // For a caller that wants the chunk lit on its own — the generation-time fill
  // is the one. Every column outside the chunk is unmodelled: the halo slots of
  // a region the caller supplied are null, and a caller with no region at all
  // has nothing beyond `get_block`/`height`. So the pass seeds no source there
  // and the flood does not enter it. Without this the engine's fixed 48x48 read
  // area treats the halo as columns that were never scanned but are fully open
  // to the sky, seeds them 15, and lights the chunk's border from light no
  // relight would put there. A full 3x3 region needs no flag: its ring is real.
  bool centre_only = false;
};

// Recomputes `chunk`'s light from scratch and writes it into the chunk's own
// sections (dropping an all-zero array rather than storing it — the encoder
// treats absent and all-zero identically). The read region is the 3x3 chunks
// around it, because a light source up to 15 blocks away reaches the border;
// only `chunk`'s own arrays are written.
void compute_chunk_light(Chunk& chunk, const LightEnvironment& env);

// One block write a delta relight replays: the cell's world position and the
// packed kinds (`light_kinds`) it had before the write. The world holds the new
// state; the old emission is what a recompute reads back from the blocks.
struct LightDeltaEdit {
  int x = 0;
  int y = 0;
  int z = 0;
  int before_kinds = 0;
};

// The incremental relight over a 5x5 chunk region: the centre 3x3 is the
// result, the ring is the halo that makes a neighbour's border exact when part
// of its light comes from the chunk beyond the 3x3 (the 3x3-only version's
// drill failure is recorded in docs/PHASE5.md).
//
// Source-stable edits are replayed by the removal/increase passes. When an edit
// moves its column's sky source (the first dampening cell from the top), the
// whole 5x5 sky layer is rebuilt from the live columns before those passes: the
// column's solar run and the cells it fed are then exact, and block light still
// stays on the incremental path. It refuses, writing nothing, only when the
// region is missing light or a cell is outside the arena.
//
// `chunks` is row-major from (centre_cx - 2, centre_cz - 2); a null entry reads
// as air. Every present chunk must already carry light. The caller holds the
// store's relight exclusion and diffs/publishes all 25 chunks.
bool compute_region_light_delta(const std::array<Chunk*, 25>& chunks, int centre_cx, int centre_cz,
                                const std::vector<LightDeltaEdit>& edits);

// A cell's level out of a chunk's arrays. `x`/`z` are local 0..15; 0 is returned
// for a section that has no array and for a y outside the column.
int chunk_light_level(const Chunk& chunk, int x, int y, int z, LightLayer layer);

// `Level.getRawBrightness(pos, skyDarken)` = `max(block, sky - skyDarken)`.
int chunk_raw_brightness(const Chunk& chunk, int x, int y, int z, int sky_darken);

} // namespace mc::world
