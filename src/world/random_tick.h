// SPDX-License-Identifier: GPL-3.0-or-later
//
// The per-chunk random-tick pass, next to the scheduled-tick queue of
// world/block_tick.h (Phase 3 milestone J).
//
// `ServerLevel.tickChunk` is two halves. The first is the precipitation sample
// (world/weather.h); the second is this one: for every chunk section whose
// `LevelChunkSection.isRandomlyTicking()` is true, `randomTickSpeed` samples
// (`Level.getBlockRandomPos` — the *level's* own LCG, not the caller's random),
// and each sampled position ticks when its own state says so:
//
//     for (int sectionIndex = 0; sectionIndex < sections.length; ++sectionIndex) {
//         LevelChunkSection section = sections[sectionIndex];
//         if (!section.isRandomlyTicking()) continue;
//         int sectionY = chunk.getSectionYFromSectionIndex(sectionIndex);
//         int minYInSection = SectionPos.sectionToBlockCoord(sectionY);
//         for (int i = 0; i < tickSpeed; ++i) {
//             BlockPos pos = this.getBlockRandomPos(minX, minYInSection, minZ, 15);
//             BlockState blockState = section.getBlockState(...);
//             if (blockState.isRandomlyTicking()) {
//                 blockState.randomTick(this, pos, this.random);
//             }
//         }
//     }
//
// The section gate is a per-*cell* count in vanilla
// (`LevelChunkSection.tickingBlockCount`, maintained by `setBlockState` and
// rebuilt by `recalcBlockCounts`), and it is what makes the loop cheap: a chunk
// of stone and air is never sampled at all. `Chunk`'s own sections carry the
// same counter (world/chunk.h), so asking a chunk which of its sections tick is
// a bitmask read.
//
// Both halves share one `BlockRandomPos` stream, because `Level.randValue` is a
// single field: the precipitation sample and the block samples of one tick are
// consecutive draws from the same LCG, exactly as in vanilla.
//
// What this module owns is the *dispatch*: `block_is_randomly_ticking` (the
// state-level question, from world/random_tick_table.h plus the crop and leaf
// rules) and `random_tick_block` (which family's tick runs). The two families
// this engine implements are `CropBlock` (world/crop.h) and `LeavesBlock`
// (world/leaves.h); every other randomly-ticking block — 429 of them, from the
// weathering coppers to bamboo — is sampled and then ticks nothing, which is
// the same shape as the mob table's unmodelled spawn rules and keeps the
// sampling faithful (a field of copper does not make a wheat crop grow faster).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

#include "world/chunk.h"
#include "world/random_tick_table.h"
#include "world/weather.h" // BlockRandomPos: `Level.randValue`, one stream per level

namespace mc::world {

// ---- the per-block question -------------------------------------------------

// The generated kind for a block name (`kRandomTickNone` when the block never
// random-ticks at all).
RandomTickKind random_tick_kind(std::string_view block_name);

// `BlockState.isRandomlyTicking()`. State-dependent where vanilla's is:
//   * a crop is ticking while it is below its last age (`CropBlock`);
//   * a leaf is ticking while it is a distance-7, non-persistent leaf
//     (`LeavesBlock`);
//   * every other randomly-ticking block is reported as ticking. For the
//     handful whose vanilla answer is state-dependent (nether wart below age 3,
//     an unlit redstone ore, a potted flower, the growing plants) this can only
//     cost a sample — the tick that follows finds nothing to do.
bool block_is_randomly_ticking(const BlockStateKey& state);

// The integer value of `key` in a canonical properties string
// ("k=v,k=v,k=v", the form world/chunk.h stores and the registry emits), or
// nullopt when the state does not carry the property. Shared with the crop and
// leaf rules, which are all "read one property, write it back" edits.
std::optional<int> state_prop_int(const std::string& properties, std::string_view key);
std::optional<bool> state_prop_bool(const std::string& properties, std::string_view key);
// `state` with `key` set to `value`, the rest of the properties untouched (in
// their canonical order, because the new key is written where the old one was).
// The bool properties are written as `true`/`false` — the table's own spelling,
// which `state_with_prop_int`'s digits would not round-trip — so a state edited
// through `state_with_prop_value` stays a state the registry can look up.
BlockStateKey state_with_prop_value(const BlockStateKey& state, std::string_view key,
                                    std::string_view value);
BlockStateKey state_with_prop_int(const BlockStateKey& state, std::string_view key, int value);

// ---- the level a random tick mutates ----------------------------------------

// The draws a random tick takes, as callbacks so the rules stay free of any one
// RNG: `CropBlock.randomTick`'s one `nextInt`, `LeavesBlock`'s loot rolls.
struct RandomTickRandom {
  std::function<int(int bound)> next_int;
  std::function<float()> next_float;
};

// The world a random tick reads and mutates. Every callback is the vanilla call
// it stands for, and each is only called for a block whose tick this engine
// implements.
struct RandomTickLevel {
  // `LevelChunkSection.isRandomlyTicking()` for chunk (cx, cz), as a bitmask
  // over that chunk's section indices (bit i = the section at
  // `min_section_y + i`, which is how `WorldInterface::random_tick_sections`
  // reports it). Bit 63 and above are never set.
  std::function<uint64_t(int cx, int cz)> ticking_sections;
  // `Level.getBlockState(pos)`.
  std::function<BlockStateKey(int x, int y, int z)> get_block;
  // `Level.setBlock(pos, state, flags)`. `notify` is the client half
  // (`UPDATE_CLIENTS`) — a fire's age-only write does not tell anyone, a leaf's
  // distance and a crop's new age do, and vanilla's own flags for those are 3
  // and 2 respectively. The neighbour half of flag 3 is not a world service
  // here: with no general neighbour-update pass, the leaves module walks the
  // six neighbours itself (see `leaves_notify_neighbours_of_change`).
  std::function<void(int x, int y, int z, const BlockStateKey& state, bool notify)> set_block;
  // `Level.getRawBrightness(pos, 0)`: the crop's light gate. `Level`'s own
  // answer is `max(blockLight, skyLight - skyDarken)`; with no light engine,
  // the caller models it from the column's motion-blocking height (see the
  // boundary note in the session's `random_tick_level_for`).
  std::function<int(int x, int y, int z)> raw_brightness;
  // `Level.scheduleTick(pos, this, 1)` — the leaf's own distance update, which
  // the scheduled-tick queue (world/block_tick.h) holds.
  std::function<void(int x, int y, int z, int delay)> schedule_tick;
  // `Level.dropResources(state, pos)` (no tool, no enchantments): what a
  // decayed leaf leaves on the ground.
  std::function<void(int x, int y, int z, const BlockStateKey& state)> drop_resources;
  // The dimension's bounds, for the section-index → section-Y mapping.
  int min_section_y = 0;
  int section_count = 0;
};

// ---- the pass ---------------------------------------------------------------

// `ServerLevel.tickChunk`'s block half for one chunk: every tick-eligible
// section of the chunk, `tick_speed` sampled positions each, each position
// ticked when its state is randomly ticking. `rand` is the level's own LCG
// stream, shared with the precipitation sample (world/weather.h).
void tick_chunk_random_blocks(const RandomTickLevel& level, const RandomTickRandom& rng,
                              BlockRandomPos& rand, int tick_speed, int chunk_x, int chunk_z);

// `BlockState.randomTick(level, pos, random)` for one sampled position, i.e. the
// dispatch on the block's family. Returns true when a tick actually ran.
bool random_tick_block(const RandomTickLevel& level, const RandomTickRandom& rng,
                       const BlockStateKey& state, int x, int y, int z);

} // namespace mc::world
