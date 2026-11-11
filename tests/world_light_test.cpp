// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the light engine's bridge to a living world (Phase 4 milestone
// B): `world/world_light.h`.
//
// The engine's own arithmetic is light_test.cpp's business. What is tested here
// is everything between a *world* and that engine:
//
//   * `chunk_light_section_range` — the index base a `light_update` and a chunk
//     packet share, checked against the section index vanilla's own client-side
//     `LevelLightEngine` would give that slot;
//   * `LightWorld`'s write recording — which chunks a write marks, and which
//     writes mark nothing at all;
//   * `relight_world_chunk` over the fixture overworld column: the light it
//     writes into the world, the diff it reports against what the world held,
//     and its idempotence (a relight of an unchanged column reports nothing, so
//     a tick with no light-relevant write sends no packet);
//   * the seam the send path relies on: a freshly generated column carries sky
//     light written at generation time, so this relights the same column and
//     compares both layers cell for cell — the approximation's error as a
//     measured count (the pinned per-chunk numbers below), not an assumption;
//   * the production path's cost, measured on a real full-height overworld
//     column rather than a synthetic one, because that number is what the
//     server's per-tick budget in `src/server/session.cpp` is sized against;
//   * `sky_darken_for` — the clock-and-weather term of `Level.getRawBrightness`,
//     whose track and layer order come from the decompiled 26.2 timeline.
//
// Usage: world_light_test /path/to/tests/fixtures

#include "light_seam.h"
#include "server/entity_packets.h"
#include "world/anvil.h"
#include "world/memory_world.h"
#include "world/world_light.h"
#include "world/worldgen_chunk.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

mc::world::BlockStateKey key(const char* name, const char* props = "") {
  return mc::world::BlockStateKey{name, props};
}

// How many slots a change's mask names.
int named_slots(const std::vector<uint64_t>& words) {
  int n = 0;
  for (const uint64_t w : words)
    for (int bit = 0; bit < 64; ++bit)
      if ((w >> bit) & 1)
        ++n;
  return n;
}

// The section a world y falls in, by floor division — which is what
// `Chunk::cell_index_at` does, and what a plain `/` gets wrong for a negative y
// (half the overworld's height is below y = 0).
int section_of(int y) {
  return y >= 0 ? y / 16 : (y - 15) / 16;
}

// ---- the index base ---------------------------------------------------------

void test_section_range(const mc::world::Chunk& chunk) {
  std::printf("-- the light section range a packet indexes into\n");
  const mc::world::LightSectionRange range = mc::world::chunk_light_section_range(chunk);
  // `LevelLightEngine`: index 0 is the section *below* the world's minimum, and
  // the count is the section count plus two.
  CHECK(range.first_section_y == chunk.min_section_y - 1);
  CHECK(range.count == static_cast<int>(chunk.sections.size()) + 2);
  // The property a `light_update` depends on: a slot and the section its light
  // array came from have to be the same section. `light_test`'s own fixtures
  // read a slot's array through this arithmetic (`slot = section_y - first`),
  // and `make_light_update`'s is the inverse.
  for (int slot = 0; slot < range.count; ++slot) {
    const int section_y = range.first_section_y + slot;
    const int index = section_y - chunk.min_section_y;
    const bool inside = index >= 0 && index < static_cast<int>(chunk.sections.size());
    // Slot 0 and the last slot are the two sections outside the chunk's own
    // list — vanilla's padding, which this engine computes no light for.
    CHECK(inside == (slot > 0 && slot < range.count - 1));
  }
}

// ---- the recorder -----------------------------------------------------------

void test_write_recording() {
  std::printf("-- which writes mark which chunks\n");
  auto inner = std::make_shared<mc::world::MemoryWorld>();
  mc::world::LightWorld light(inner);

  // A write in the middle of a chunk reaches `kMaxLightLevel - 1` cells, so it
  // marks the chunk and all four face neighbours, not the four diagonals.
  CHECK(light.set_block_state(0, 0, 8, 64, 8, key("minecraft:glowstone")));
  std::vector<std::pair<int, int>> marked = light.take_dirty_chunks();
  CHECK(marked.size() == 5);
  const auto has = [&marked](int cx, int cz) {
    for (const auto& p : marked)
      if (p.first == cx && p.second == cz)
        return true;
    return false;
  };
  CHECK(has(0, 0) && has(1, 0) && has(-1, 0) && has(0, 1) && has(0, -1));
  CHECK(!has(1, 1) && !has(-1, -1));
  // Draining empties the set: the tick that drained it owes the relight, and a
  // second drain with no writes in between has nothing.
  CHECK(light.take_dirty_chunks().empty());

  // A write at a corner is near only *two* of its four neighbours: the cell at
  // local x = 0 is a full 16 cells from the chunk on its +x side, which is more
  // than light can carry (15 levels minus 1), so that neighbour is not marked —
  // and neither is the diagonal beyond it. The two it is near are.
  CHECK(light.set_block_state(0, 0, 0, 64, 0, key("minecraft:glowstone")));
  marked = light.take_dirty_chunks();
  CHECK(marked.size() == 4);
  const auto marked_has = [&marked](int cx, int cz) {
    for (const auto& p : marked)
      if (p.first == cx && p.second == cz)
        return true;
    return false;
  };
  CHECK(marked_has(0, 0) && marked_has(-1, 0) && marked_has(0, -1) && marked_has(-1, -1));
  CHECK(!marked_has(1, 0) && !marked_has(0, 1) && !marked_has(1, 1));
  light.take_dirty_chunks();
  // The mirror: at the far corner the +x and +z neighbours are the near ones.
  CHECK(light.set_block_state(0, 0, 15, 64, 15, key("minecraft:glowstone")));
  marked = light.take_dirty_chunks();
  CHECK(marked.size() == 4);
  CHECK(marked_has(0, 0) && marked_has(1, 0) && marked_has(0, 1) && marked_has(1, 1));
  light.take_dirty_chunks();

  // A write that cannot move any light marks nothing: an opaque block replaced
  // by another opaque block changes neither the dampening nor the emission of
  // its cell, so a floor of dirt over a floor of stone is free.
  CHECK(light.set_block_state(0, 0, 4, 64, 4, key("minecraft:stone")));
  light.take_dirty_chunks();
  CHECK(light.set_block_state(0, 0, 4, 64, 4, key("minecraft:dirt")));
  CHECK(light.take_dirty_chunks().empty());

  // But trading the same stone for something translucent, or for a source,
  // does mark: both halves of the engine's per-cell read moved.
  CHECK(light.set_block_state(0, 0, 4, 64, 4, key("minecraft:glass")));
  CHECK(!light.take_dirty_chunks().empty());
  CHECK(light.set_block_state(0, 0, 4, 64, 4, key("minecraft:torch", "torch_type=standing")));
  CHECK(!light.take_dirty_chunks().empty());

  // An unmodified world has no light of its own, and the in-memory world's
  // generated column is air: false until something computes it.
  CHECK(!light.has_chunk_light(0, 0));
}

// ---- relighting a real column ----------------------------------------------

void test_relight_against_a_real_chunk(const std::string& fixtures_dir) {
  std::printf("-- relighting the fixture overworld column\n");
  if (fixtures_dir.empty()) {
    std::printf("  SKIP no fixtures directory given on the command line\n");
    return;
  }
  const std::string region = fixtures_dir + "/ow3region";
  auto world = mc::world::AnvilWorld::open_region_dir(region);
  if (!world) {
    std::printf("  SKIP ow3region fixture missing (%s)\n", region.c_str());
    return;
  }
  auto light = std::make_shared<mc::world::LightWorld>(
      std::static_pointer_cast<mc::world::WorldInterface>(world));

  // The column came from disk with light, so the send path's "does it need
  // light" question answers yes and never computes anything for it.
  CHECK(light->has_chunk_light(-2, -1));

  // A relight of the column as it stands must move nothing: the engine is
  // deterministic, its answer for a chunk is a function of the world, so the
  // diff against the light already in the world is empty. This is the property
  // the server's tick relies on — a tick that touched no light marks nothing, and
  // a chunk that is marked but whose light does not move sends no packet.
  const auto started = std::chrono::steady_clock::now();
  const mc::world::LightChange first = mc::world::relight_world_chunk(-2, -1, *light->inner());
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  std::printf("  (relight of chunk (-2,-1): %.1f ms, %d of %d sky slots moved, %d block)\n", ms,
              named_slots(first.sky_changed), first.slot_count, named_slots(first.block_changed));
  // Not an empty diff, and it should not be: this is the engine's answer for the
  // column against the light *vanilla* wrote to disk, and the fixture's
  // neighbours are missing (the region holds this one chunk), so the boundary
  // columns come out wrong and every section that contains one is named —
  // light_test.cpp prices that same boundary effect at 253 of 16,384 sky cells.
  // A section is named when any of its 4,096 cells moved, and a chunk's border
  // columns run the full height, so one bad column names its whole section.
  // What must hold is that the world's *own* light is a fixed point, which is
  // what the second relight below establishes: whatever the answer is, relighting
  // the same world twice reports nothing the second time, so a tick that changed
  // nothing sends no packet.
  const mc::world::LightChange second = mc::world::relight_world_chunk(-2, -1, *light->inner());
  CHECK(!second.any()); // the world now holds the engine's own answer: a relight is idempotent

  // The on-disk store supports the light-input version too, which is what makes
  // that second relight take the two-phase commit rather than the in-lock
  // fallback (world/world_interface.h's `chunk_content_version`). A resident
  // column answers a non-zero version; 0 is "not resident, or this world cannot
  // say", and the relight treats it as the old path. So this is the check that
  // the fast path on AnvilWorld is live, and the idempotence above is its
  // correctness: a copy that missed a neighbour's block would show up as light
  // moving on a relight of an untouched column.
  CHECK(world->chunk_content_version(-2, -1) != 0);

  // The range the diff reports in is the packet's own: the fixture column's light
  // sections start one below the world's minimum, and the padding slots at either
  // end of that range are never named — this engine computes no light outside the
  // chunk's own sections, and it must not pretend otherwise.
  CHECK(first.sky_at(0) == false);
  CHECK(first.sky_at(first.slot_count - 1) == false);
  CHECK(first.block_at(0) == false);
  CHECK(first.block_at(first.slot_count - 1) == false);
}

// The on-disk store's half of the two-phase protocol: a resident column answers
// a version, a write moves it, a relight does not (light is not a light *input*),
// and an absent or evicted column answers 0 so the relight falls back rather than
// trusting a copy it can no longer prove.
void test_anvil_content_version(const std::string& fixtures_dir) {
  std::printf("-- AnvilWorld's light-input versions\n");
  if (fixtures_dir.empty()) {
    std::printf("  SKIP no fixtures directory given on the command line\n");
    return;
  }
  const std::string region = fixtures_dir + "/ow3region";
  {
    auto world = mc::world::AnvilWorld::open_region_dir(region);
    if (!world) {
      std::printf("  SKIP ow3region fixture missing (%s)\n", region.c_str());
      return;
    }
    // A column the region file does not hold answers 0 and is not minted: the
    // relight reads that as "no resident column", which is exactly what it is.
    CHECK(world->chunk_content_version(1000, 1000) == 0);
    CHECK(world->chunk_content_version(1000, 1000) == 0);

    CHECK(world->load_chunk(-2, -1).has_value());
    const uint64_t first = world->chunk_content_version(-2, -1);
    CHECK(first != 0);
    // Asking twice is not a write: the version is the column's content, not its
    // recency (the LRU touch a read performs must not look like a change).
    CHECK(world->load_chunk(-2, -1).has_value());
    CHECK(world->chunk_content_version(-2, -1) == first);

    // A relight rewrites the column's light arrays, which are not an input of
    // the next compute, so it must leave the input version alone — otherwise a
    // busy viewer's relights would invalidate every other relight's copy.
    mc::world::relight_world_chunk(-2, -1, *world);
    CHECK(world->chunk_content_version(-2, -1) == first);

    // A block write is the event the version exists for: it must move, and to a
    // value this column has never had (versions are minted, never reused).
    CHECK(world->set_block_state(-2, -1, 8, -60, 8, key("minecraft:glowstone")));
    const uint64_t second = world->chunk_content_version(-2, -1);
    CHECK(second != 0);
    CHECK(second != first);
    CHECK(world->set_block_state(-2, -1, 8, -59, 8, key("minecraft:stone")));
    CHECK(world->chunk_content_version(-2, -1) != second);
  }

  // Eviction drops the version with the column: a store that has forgotten the
  // blocks cannot keep vouching for a copy of them. The fixture column is clean
  // here (the block writes above belong to the world that just went away), so it
  // is the first victim a flood of misses can take.
  {
    auto world = mc::world::AnvilWorld::open_region_dir(region);
    if (!world)
      return;
    CHECK(world->load_chunk(-2, -1).has_value());
    CHECK(world->chunk_content_version(-2, -1) != 0);
    constexpr int kResident = 4096; // AnvilWorld::Impl::kMaxCachedChunks
    int misses = 0;
    for (int cz = 0; cz < 64; ++cz)
      for (int cx = 0; cx < 64; ++cx) {
        (void)world->load_chunk(cx + 100, cz + 100); // absent, so each is a cheap miss
        ++misses;
      }
    // A flood that fills the cache, or the column would still be resident and
    // the version check below would pass without eviction ever happening.
    CHECK(misses >= kResident);
    CHECK(world->chunk_content_version(-2, -1) == 0);
    // ...and coming back is a fresh resident with a fresh number, not the old
    // one: a relight holding the old copy can never match it.
    CHECK(world->load_chunk(-2, -1).has_value());
    CHECK(world->chunk_content_version(-2, -1) != 0);
  }
}

// The store's residency question (world/anvil.h): `has_stored_column` answers
// from the cache and never loads, which is what separates it from `has_chunk` on
// a disk-backed store — and what lets the natural spawn pass ask it once per
// candidate chunk of an 8-chunk square without pulling the square off disk
// (docs/PHASE5.md). The fixture column exists on disk at (-2, -1).
void test_anvil_stored_column(const std::string& fixtures_dir) {
  std::printf("-- AnvilWorld's stored-column answer\n");
  if (fixtures_dir.empty()) {
    std::printf("  SKIP no fixtures directory given on the command line\n");
    return;
  }
  const std::string region = fixtures_dir + "/ow3region";
  auto world = mc::world::AnvilWorld::open_region_dir(region);
  if (!world) {
    std::printf("  SKIP ow3region fixture missing (%s)\n", region.c_str());
    return;
  }
  // On disk, not resident: the residency answer is false, and asking twice
  // leaves it false — the ask itself must not load the column.
  CHECK(!world->has_stored_column(-2, -1));
  CHECK(!world->has_stored_column(-2, -1));
  // `has_chunk` is the disk question, and it loads to answer: the column is
  // resident afterwards, which is exactly the per-candidate load the spawn pass
  // must not trigger.
  CHECK(world->has_chunk(-2, -1));
  CHECK(world->has_stored_column(-2, -1));
  // A column the region does not hold answers false either way, and a cached
  // miss must not read as a column on the second ask.
  CHECK(!world->has_stored_column(1000, 1000));
  CHECK(!world->can_load_chunk(1000, 1000));
  CHECK(!world->has_stored_column(1000, 1000));
  // A write makes the store hold the column: the dirty object is the store's
  // state, whether or not the LRU still has a residence for it.
  auto written = mc::world::AnvilWorld::open_region_dir(region);
  CHECK(written->set_block_state(-2, -1, 8, -60, 8, key("minecraft:glowstone")));
  CHECK(written->has_stored_column(-2, -1));
}

// A real source in a column, and the light it must leave: a floor to stand on, a
// glowstone in the air above it, and the diff the relight reports.
void test_relight_after_a_write() {
  std::printf("-- a source placed in the world's own column\n");
  auto inner = std::make_shared<mc::world::MemoryWorld>();
  auto light = std::make_shared<mc::world::LightWorld>(inner);

  // A floor at the bottom of the world. 256 writes in one tick is the case the
  // recorder exists for: the set collapses them to the five chunks they reach,
  // and the relight below is the single one they owe.
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z)
      CHECK(light->set_block_state(0, 0, x, -64, z, key("minecraft:stone")));
  CHECK(light->take_dirty_chunks().size() == 9); // a whole chunk: every neighbour
  const mc::world::LightChange floor_change = mc::world::relight_world_chunk(0, 0, *inner);
  CHECK(floor_change.any());
  // The air above the floor is full sky (the column is unbroken), and the floor
  // itself is opaque: `chunk_light_level` answers 0 there because an opaque cell's
  // own light is written as 0, which is what the flood's cost of 15 means.
  const auto lit = *inner->load_chunk(0, 0);
  CHECK(mc::world::chunk_light_level(lit, 8, -60, 8, mc::world::LightLayer::kSky) == 15);
  CHECK(mc::world::chunk_light_level(lit, 8, -64, 8, mc::world::LightLayer::kBlock) == 0);
  CHECK(mc::world::chunk_raw_brightness(lit, 8, -60, 8, 0) == 15);

  CHECK(light->set_block_state(0, 0, 8, -60, 8, key("minecraft:glowstone")));
  CHECK(!light->take_dirty_chunks().empty());
  const auto started = std::chrono::steady_clock::now();
  const mc::world::LightChange change = mc::world::relight_world_chunk(0, 0, *inner);
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  std::printf("  (relight of a void column after a write: %.2f ms)\n", ms);
  CHECK(change.any());
  // The diff names the section the source is in — and the sky in that section
  // moved too, which is not a bug: a glowstone is a full block, so
  // `light_dampening` is 15 and it takes the sky out of its own cell and the
  // cells below it (vanilla casts the same shadow from the same block). Only the
  // light *unrelated* to the change may never be named, which is what the
  // fixture's idempotence check above establishes.
  const int section_y = section_of(-60);
  const int slot = section_y - change.first_section_y;
  CHECK(change.block_at(slot));
  CHECK(change.sky_at(slot));

  const auto after = *inner->load_chunk(0, 0);
  const int index = section_y - after.min_section_y;
  CHECK(index >= 0 && index < static_cast<int>(after.sections.size()));
  const auto& layer = after.sections[static_cast<size_t>(index)].block_light;
  CHECK(layer.has_value());
  if (layer) {
    const int local_y = -60 & 15;
    // `light_emission(minecraft:glowstone)` is 15, and a step costs one level in
    // air, so the cells beside it read 14 and 13.
    CHECK(mc::world::light_nibble(*layer, 8, local_y, 8) == 15);
    CHECK(mc::world::light_nibble(*layer, 9, local_y, 8) == 14);
    CHECK(mc::world::light_nibble(*layer, 10, local_y, 8) == 13);
    // Its own cell's sky is gone, one block down as well (the shadow the block
    // casts on the air under it), while the sky beside it at the same height is
    // untouched.
    CHECK(mc::world::chunk_light_level(after, 8, -60, 8, mc::world::LightLayer::kSky) == 0);
    CHECK(mc::world::chunk_light_level(after, 9, -60, 8, mc::world::LightLayer::kSky) == 15);
    // And the section is the one the packet's slot names. The two formulas are
    // the relation the whole update path rests on — `LightChange` reports a slot
    // and `make_light_update` looks the array up by section — so they are written
    // out here rather than assumed:
    //   slot      = section_y - first_section_y          (world_light.cpp)
    //   section_y = min_section_y - 1 + slot             (LevelLightEngine)
    CHECK(section_y - change.first_section_y == section_y - after.min_section_y + 1);
  }
  // Breaking it again takes the light back: the diff is bidirectional.
  CHECK(light->set_block_state(0, 0, 8, -60, 8, key("minecraft:air")));
  const mc::world::LightChange gone = mc::world::relight_world_chunk(0, 0, *inner);
  CHECK(gone.any());
  CHECK(gone.block_at(slot));
  const auto empty = *inner->load_chunk(0, 0);
  CHECK(!empty.sections[static_cast<size_t>(index)].block_light.has_value());
}

// ---- the level's sky darken -------------------------------------------------

void test_sky_darken() {
  std::printf("-- sky darken: the clock and the weather\n");
  // Noon: the timeline multiplies the dimension's 15 by its day value of 1.0, so
  // there is nothing to subtract and a crop under open sky sees its full 15.
  CHECK(mc::world::sky_darken_for(6000, 0.0f, 0.0f) == 0);
  CHECK(mc::world::sky_darken_for(6000, 0.0f, 0.0f) == 0);
  // Night: 15 * 0.26666668 = 4.0, so the darken is 11 and an open field reads 4
  // — below `CropBlock`'s gate of 9, which is what stops a field growing at
  // night.
  CHECK(mc::world::sky_darken_for(18000, 0.0f, 0.0f) == 11);
  const int night_sky = 15 - mc::world::sky_darken_for(18000, 0.0f, 0.0f);
  CHECK(night_sky == 4);
  // The clock is cyclic and equal at the same time on any day.
  CHECK(mc::world::sky_darken_for(18000 + 24000, 0.0f, 0.0f) == 11);
  CHECK(mc::world::sky_darken_for(18000 - 24000, 0.0f, 0.0f) == 11);
  // The dusk ramp is between the two, and monotone across it (the timeline is
  // linear from tick 11867 to 13670).
  CHECK(mc::world::sky_darken_for(11867, 0.0f, 0.0f) == 0);
  int previous = 0;
  for (int i = 1; i <= 30; ++i) {
    const int t = 11867 + i * 1803 / 30; // lands on the ramp's last tick at i = 30
    const int darken = mc::world::sky_darken_for(t, 0.0f, 0.0f);
    CHECK(darken >= previous);
    previous = darken;
  }
  CHECK(previous == 11);
  // The dusk ramp ends where the night plateau begins, exactly: the timeline's
  // 13670 and 22330 keyframes hold the same 0.26666668 as every tick between.
  CHECK(mc::world::sky_darken_for(13670, 0.0f, 0.0f) ==
        mc::world::sky_darken_for(22329, 0.0f, 0.0f));
  CHECK(mc::world::sky_darken_for(13670, 0.0f, 0.0f) == 11);
  // Rain and thunder dim the sky further at their own levels: the weather layer
  // is applied last (`EnvironmentAttributeSystem.addDefaultLayers`), so a rainy
  // noon reads darker than a clear one, and a full thunderstorm darker still.
  const int clear = mc::world::sky_darken_for(6000, 0.0f, 0.0f);
  const int rain = mc::world::sky_darken_for(6000, 1.0f, 0.0f);
  const int thunder = mc::world::sky_darken_for(6000, 1.0f, 1.0f);
  CHECK(clear < rain);
  CHECK(rain < thunder);
  // At a full level: the rain entry blends 4.0 in by 0.3125 (15 -> 11.5625), then
  // the level lerp lands on it, so the darken is (int)(15 - 11.5625) = 3.
  CHECK(rain == 3);
  // The level is a level, not a flag: half rain is between none and all of it.
  CHECK(mc::world::sky_darken_for(6000, 0.5f, 0.0f) >= clear);
  CHECK(mc::world::sky_darken_for(6000, 0.5f, 0.0f) <= rain);
  // Rain's fraction is its level *net of* the thunder level
  // (`WeatherAttributes.addLayer`), so a level that is all thunder is not
  // double-counted as rain.
  CHECK(mc::world::sky_darken_for(6000, 1.0f, 1.0f) == mc::world::sky_darken_for(6000, 0.0f, 1.0f));
}

// ---- the packet -------------------------------------------------------------

// A reader for `make_light_update`'s payload, walking it the way a client does:
// `ok` goes false the moment a read would leave the buffer, so a framing slip
// cannot hide behind a length that happens to work.
struct PayloadReader {
  const std::vector<uint8_t>& body;
  size_t pos = 0;
  bool ok = true;

  int varint() {
    int value = 0, shift = 0;
    while (pos < body.size()) {
      const uint8_t byte = body[pos++];
      value |= static_cast<int>(byte & 0x7F) << shift;
      if ((byte & 0x80) == 0)
        return value;
      shift += 7;
      if (shift > 28)
        break;
    }
    ok = false;
    return 0;
  }

  std::vector<uint8_t> bytes(int n) {
    if (n < 0 || pos + static_cast<size_t>(n) > body.size()) {
      ok = false;
      return {};
    }
    std::vector<uint8_t> out(body.begin() + static_cast<long>(pos),
                             body.begin() + static_cast<long>(pos) + n);
    pos += static_cast<size_t>(n);
    return out;
  }

  // 26.3's `ByteBufCodecs.BIT_SET`: a varint byte length followed by
  // `BitSet.toByteArray()` — little-endian bits, trailing zero bytes trimmed.
  int64_t bitset() {
    const int count = varint();
    const std::vector<uint8_t> data = bytes(count);
    if (count < 0 || data.size() != static_cast<size_t>(count)) {
      ok = false;
      return 0;
    }
    int64_t bits = 0;
    for (int i = 0; i < count; ++i) {
      for (int bit = 0; bit < 8; ++bit) {
        const int position = i * 8 + bit;
        if (position >= 64) {
          ok = false;
          return 0;
        }
        if ((data[static_cast<size_t>(i)] >> bit) & 1)
          bits |= static_cast<int64_t>(1) << position;
      }
    }
    return bits;
  }

  // The arrays as the world holds them (`LightArray`, pool-backed): the packet's
  // bytes and the section's must be the same bytes, so the comparison below is
  // the container's own.
  std::vector<mc::world::LightArray> arrays() {
    std::vector<mc::world::LightArray> out;
    const int count = varint();
    for (int i = 0; i < count; ++i) {
      const std::vector<uint8_t> data = bytes(varint());
      out.emplace_back(data.begin(), data.end());
    }
    return out;
  }
};

// The client-visible half of the milestone: the diff of a real relight, framed
// as the packet a vanilla client applies.
void test_light_update_packet() {
  std::printf("-- the light_update payload\n");
  // Two sections, so a change can name one and leave the other out entirely.
  auto world = std::make_shared<mc::world::MemoryWorld>(/*min_section_y=*/0, /*section_count=*/2);
  auto light = std::make_shared<mc::world::LightWorld>(world);
  // A floor in section 0 and a glowstone standing on it in section 1, which is
  // the write whose light the client has to be told about.
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z)
      CHECK(light->set_block_state(0, 0, x, 0, z, key("minecraft:stone")));
  CHECK(light->set_block_state(0, 0, 8, 1, 8, key("minecraft:glowstone")));
  light->take_dirty_chunks();
  const mc::world::LightChange change = mc::world::relight_world_chunk(0, 0, *world);
  CHECK(change.any());
  const auto chunk = world->load_chunk(0, 0);
  CHECK(chunk.has_value());
  if (!chunk)
    return;
  const std::vector<uint8_t> payload = mc::server::make_light_update(0, 0, *chunk, change);
  const auto named_slots_of = [](int64_t bits) {
    int n = 0;
    for (int bit = 0; bit < 64; ++bit)
      n += static_cast<int>((bits >> bit) & 1);
    return n;
  };

  PayloadReader r{payload};
  CHECK(r.varint() == mc::protocol::kPlayLightUpdateId);
  CHECK(r.varint() == 0); // chunk x, as a varint
  CHECK(r.varint() == 0); // chunk z
  const int64_t sky_real = r.bitset();
  const int64_t block_real = r.bitset();
  const int64_t sky_empty = r.bitset();
  const int64_t block_empty = r.bitset();
  const std::vector<mc::world::LightArray> sky_arrays = r.arrays();
  const std::vector<mc::world::LightArray> block_arrays = r.arrays();
  CHECK(r.ok);
  // The framing is exact: nothing left over, nothing short.
  CHECK(r.pos == payload.size());
  // The floor and the glowstone are both in section 0 (y 0..15), and slot `i` is
  // the light section at `first_section_y + i` — so this world's section 0 is
  // slot 1, section 1 is slot 2, and slot 0 is the padding section below the
  // world. Nothing named slot 0: it has no section, so no light of its own moved.
  CHECK(change.first_section_y == -1);
  CHECK(sky_real == 0b110);   // both sections gained sky (neither had any before)
  CHECK(block_real == 0b010); // only the glowstone's own section
  CHECK(sky_empty == 0);      // no named section's sky is dark
  CHECK(block_empty == 0);    // and section 1 was not named at all, on either layer
  // One array per named slot, in slot order, and each is the array the world
  // holds — the packet copies the light rather than computing it a second time.
  const auto& section = chunk->sections[0];
  CHECK(block_arrays.size() == static_cast<size_t>(named_slots_of(block_real)));
  CHECK(sky_arrays.size() == static_cast<size_t>(named_slots_of(sky_real)));
  CHECK(block_arrays.size() == 1);
  if (block_arrays.size() == 1) {
    CHECK(section.block_light.has_value());
    CHECK(block_arrays[0] == *section.block_light);
    // And it carries the light itself: the glowstone's own cell emits 15 and air
    // costs one level per step, so the cells beside it are 14.
    CHECK(mc::world::light_nibble(block_arrays[0], 8, 1, 8) == 15);
    CHECK(mc::world::light_nibble(block_arrays[0], 9, 1, 8) == 14);
  }
  CHECK(sky_arrays.size() == 2);
  if (sky_arrays.size() == 2) {
    const auto& sky_section = chunk->sections[0];
    CHECK(sky_section.sky_light.has_value());
    CHECK(sky_arrays[0] == *sky_section.sky_light);
    // The sky moved with the block: an opaque block takes the sky out of its own
    // cell while the cells beside it stay at 15.
    CHECK(mc::world::light_nibble(sky_arrays[0], 8, 1, 8) == 0);
    CHECK(mc::world::light_nibble(sky_arrays[0], 9, 1, 8) == 15);
  }

  // Breaking the same block again names the *same* slot, but as an empty one: the
  // client is told to paint that section dark, and no array rides along with it.
  CHECK(light->set_block_state(0, 0, 8, 1, 8, key("minecraft:air")));
  light->take_dirty_chunks();
  const mc::world::LightChange gone = mc::world::relight_world_chunk(0, 0, *world);
  CHECK(gone.any());
  const auto after = world->load_chunk(0, 0);
  CHECK(after.has_value());
  if (!after)
    return;
  const std::vector<uint8_t> gone_payload = mc::server::make_light_update(0, 0, *after, gone);
  PayloadReader g{gone_payload};
  CHECK(g.varint() == mc::protocol::kPlayLightUpdateId);
  CHECK(g.varint() == 0 && g.varint() == 0);
  const int64_t gone_sky_real = g.bitset();
  const int64_t gone_block_real = g.bitset();
  const int64_t gone_sky_empty = g.bitset();
  const int64_t gone_block_empty = g.bitset();
  const auto gone_sky = g.arrays();
  const auto gone_block = g.arrays();
  CHECK(g.ok && g.pos == gone_payload.size());
  CHECK(gone_block_real == 0);
  CHECK(gone_block_empty == 0b010);
  CHECK(gone_block.empty());
  // The sky in that same section moved back up (the cell the glowstone was
  // shadowing now sees full sky), so it is named on the *real* layer at once.
  CHECK(gone_sky_real == 0b010);
  CHECK(gone_sky_empty == 0);
  CHECK(gone_sky.size() == 1);
  if (gone_sky.size() == 1) {
    CHECK(mc::world::light_nibble(gone_sky[0], 8, 1, 8) == 15);
  }
}

// ---- the world's answer -----------------------------------------------------

void test_world_raw_brightness() {
  std::printf("-- the random-tick pass's light gate\n");
  auto world = std::make_shared<mc::world::MemoryWorld>();
  // An unmodified in-memory world is generated air with no light computed: the
  // gate reads the absent arrays as 0 on both layers, so nothing grows in it
  // until the light engine has run (which the chunk-send path does for a chunk a
  // client is about to be sent).
  CHECK(world->raw_brightness(0, 64, 0, 0) == 0);
  CHECK(world->has_chunk_light(0, 0) == false);
  // Compute the column's light, then the air above the floor is full sky: the
  // gate answers 15 — and 4 at night, which is the same cell read through the
  // sky darken the level supplies.
  mc::world::relight_world_chunk(0, 0, *world);
  CHECK(world->has_chunk_light(0, 0));
  CHECK(world->raw_brightness(0, 64, 0, 0) == 15);
  CHECK(world->raw_brightness(0, 64, 0, 11) == 4);
  // A cell below the build height has no section at all: 0.
  CHECK(world->raw_brightness(0, -4096, 0, 0) == 0);
}

// ---- the write gate ---------------------------------------------------------

// A stable digest of a chunk's own light arrays, the same mix the bench tools
// use: enough to tell "these arrays moved" from "they did not" without keeping a
// copy of them.
uint64_t light_hash(const mc::world::Chunk& chunk) {
  uint64_t h = 1469598103934665603ull;
  for (const auto& section : chunk.sections) {
    for (const std::optional<mc::world::LightArray>* layer :
         {&section.sky_light, &section.block_light}) {
      if (!*layer) {
        h = (h ^ 0xFFull) * 1099511628211ull;
        continue;
      }
      for (const uint8_t b : **layer)
        h = (h ^ b) * 1099511628211ull;
    }
  }
  return h;
}

// `LightWorld::set_block_state` marks a chunk only when the written state's
// *kinds* (dampening and emission) differ from the state it replaced. That is a
// claim about the engine — two states with the same kinds cannot move any light
// — and this is its falsification test at the real entry point: apply the write,
// relight, and compare the chunk's own arrays with what they held. Light that
// moved while nothing was marked is a chunk the client keeps reading stale until
// some unrelated write happens to relight it.
void test_the_write_gate_against_the_light_itself() {
  struct Case {
    const char* what;
    const char* before;
    const char* after;
    // Whether the light must move. A state swap the engine cannot tell apart by
    // dampening and emission must not; everything else here must, and the emitter
    // cases are the ones that catch a seeding bound that is too low.
    bool moves;
  };
  // A swap at one mid-air cell (y 20) of a column whose only other block is a
  // stone floor at y 0. The first two are the gate's bread and butter; then the
  // writes `light_kinds` does *not* describe — a full cube that blocks motion
  // but not light (glass, ice), one that does neither (a torch) — and the shadow
  // of a block that is both. y 20 is above the whole region's motion-blocking
  // height (the floor's 1), which is what makes the emitter cases adversarial.
  const Case cases[] = {
      {"air->glowstone (emission)", "minecraft:air", "minecraft:glowstone", true},
      {"glowstone->air (removal)", "minecraft:glowstone", "minecraft:air", true},
      {"stone->dirt (same kinds)", "minecraft:stone", "minecraft:dirt", false},
      {"stone->glass (dampening)", "minecraft:stone", "minecraft:glass", true},
      {"glass->stone (dampening)", "minecraft:glass", "minecraft:stone", true},
      {"air->glass (motion only)", "minecraft:air", "minecraft:glass", false},
      {"air->ice (motion only)", "minecraft:air", "minecraft:ice", false},
      {"air->stone (shadow)", "minecraft:air", "minecraft:stone", true},
      {"air->oak_leaves (dampening 1)", "minecraft:air", "minecraft:oak_leaves", true},
      {"air->torch (emitter, no collision)", "minecraft:air", "minecraft:torch", true},
      {"air->soul_torch (emitter, no collision)", "minecraft:air", "minecraft:soul_torch", true},
      {"air->fire (emitter, no collision)", "minecraft:air", "minecraft:fire", true},
  };
  std::printf("-- the write gate against the light itself\n");

  // A four-section world whose chunk (0,0) is a stone floor at y 0 and nothing
  // else: its whole heightmap is the floor's 1, so an emitter anywhere above
  // stands above the entire region's heightmap.
  const auto floored = [] {
    auto world = std::make_shared<mc::world::MemoryWorld>(/*min_section_y=*/0, /*section_count=*/4);
    auto light = std::make_shared<mc::world::LightWorld>(world);
    for (int x = 0; x < 16; ++x)
      for (int z = 0; z < 16; ++z)
        light->set_block_state(0, 0, x, 0, z, key("minecraft:stone"));
    return std::pair{world, light};
  };
  // Relight chunk (0,0) and answer it as the world holds it.
  const auto relit = [](mc::world::MemoryWorld& world) {
    mc::world::relight_world_chunk(0, 0, world);
    return world.load_chunk(0, 0);
  };
  // One cell of a chunk that came back — -1 for a chunk that did not, so a
  // missing chunk fails a check instead of crashing on it.
  const auto level = [](const std::optional<mc::world::Chunk>& chunk, int x, int y, int z,
                        mc::world::LightLayer layer) {
    return chunk ? mc::world::chunk_light_level(*chunk, x, y, z, layer) : -1;
  };

  for (const Case& c : cases) {
    auto [world, light] = floored();
    if (std::string(c.before) != "minecraft:air")
      light->set_block_state(0, 0, 8, 20, 8, key(c.before));
    light->take_dirty_chunks();
    // The world's light is current before the write under test, so a difference
    // afterwards can only come from that write.
    mc::world::relight_world_chunk(0, 0, *world);
    const auto before_chunk = world->load_chunk(0, 0);
    const uint64_t before_hash = before_chunk ? light_hash(*before_chunk) : 0;

    light->set_block_state(0, 0, 8, 20, 8, key(c.after));
    const bool marked = !light->take_dirty_chunks().empty();
    mc::world::relight_world_chunk(0, 0, *world);
    const auto after_chunk = world->load_chunk(0, 0);
    const uint64_t after_hash = after_chunk ? light_hash(*after_chunk) : 0;
    const bool moved = before_hash != after_hash;

    std::printf("  %-42s kinds %3d->%3d  %-8s %s\n", c.what, mc::world::light_kinds(key(c.before)),
                mc::world::light_kinds(key(c.after)), marked ? "marked" : "unmarked",
                moved ? "MOVED" : "same");
    if (moved && !marked)
      std::printf("  MISSED MARK: %s moved the light without marking the chunk\n", c.what);
    if (moved != c.moves)
      std::printf("  %s: expected the light to %s\n", c.what, c.moves ? "move" : "stay");
    CHECK(!moved || marked);
    CHECK(moved == c.moves);
  }

  // The level that light has to be, not merely that it moved: a torch's own cell
  // is its emission (14) and air a step from it is 13, which only a seeding that
  // found the torch *and* a flood that carried it can produce. The pair straddles
  // the old bound exactly — the floor's height is 1, so `top_y` was 2, which made
  // the torch at y 1 the last cell that scan asked and the one at y 2 the first
  // it never did.
  {
    auto [world, light] = floored();
    light->set_block_state(0, 0, 8, 1, 8, key("minecraft:torch"));
    light->set_block_state(0, 0, 8, 2, 8, key("minecraft:torch"));
    const auto centre = relit(*world);
    CHECK(level(centre, 8, 1, 8, mc::world::LightLayer::kBlock) == 14);
    CHECK(level(centre, 8, 2, 8, mc::world::LightLayer::kBlock) == 14);
    CHECK(level(centre, 8, 3, 8, mc::world::LightLayer::kBlock) == 13);
    CHECK(level(centre, 9, 1, 8, mc::world::LightLayer::kBlock) == 13);
    // A torch does not stop the sky, so its own cell keeps the 15 above it.
    CHECK(level(centre, 8, 2, 8, mc::world::LightLayer::kSky) == 15);
  }

  // The new bound's clamp: an emitter in the world's topmost cell (y 63 is the
  // last of a 4-section world), where the section's own top and the region's
  // coincide. A torch, not a glowstone, for the reason above — a glowstone is
  // motion-blocking, so it is in the column's own height and `top_y` finds it
  // with no ceiling at all.
  {
    auto [world, light] = floored();
    light->set_block_state(0, 0, 8, 63, 8, key("minecraft:torch"));
    const auto centre = relit(*world);
    CHECK(level(centre, 8, 63, 8, mc::world::LightLayer::kBlock) == 14);
    CHECK(level(centre, 9, 63, 8, mc::world::LightLayer::kBlock) == 13);
  }

  // And its region-wide half: the ceiling is the highest emitting section over
  // all nine slots, so a torch in a *neighbour* — one cell west of this chunk's
  // edge and 39 cells above its heightmap — is found, and lights this chunk's
  // own edge cells. Both halves of this one matter: the neighbour's heightmap is
  // not this chunk's (that column's own `top_y` is 1), and neither is its
  // section list.
  {
    auto [world, light] = floored();
    CHECK(light->set_block_state(-1, 0, 15, 40, 8, key("minecraft:torch")));
    const auto centre = relit(*world);
    CHECK(level(centre, 0, 40, 8, mc::world::LightLayer::kBlock) == 13);
    CHECK(level(centre, 1, 40, 8, mc::world::LightLayer::kBlock) == 12);
  }
}

// ---- the lazy heightmap refresh ---------------------------------------------

// A four-section all-air column at chunk (0,0), the same shape `MemoryWorld`
// generates: enough for the heightmap path, which needs only sections.
mc::world::Chunk air_column() {
  mc::world::Chunk chunk;
  chunk.x = 0;
  chunk.z = 0;
  chunk.min_section_y = 0;
  chunk.sections.resize(4);
  for (int i = 0; i < 4; ++i) {
    mc::world::Section& section = chunk.sections[static_cast<size_t>(i)];
    section.y = i;
    section.has_blocks = true;
    section.block_palette = {key("minecraft:air")};
    section.block_cells.assign(mc::world::kBlockCellsPerSection, 0);
  }
  return chunk;
}

// `Chunk::set_block_state` marks a column for a heightmap refresh only where
// `heightmap_predicates_differ` says a height can move, and the refresh then
// starts its search at the highest cell a write reached. Both halves are the
// sort of optimization that fails quietly — a missed mark leaves the column
// answering its *previous* surface, and a stale search start leaves it answering
// a plausible wrong one — and the light engine reads these same maps for its sky
// source. So each step of a sequence that raises, drops and raises the column
// again is compared against a column built from nothing but the same final
// blocks, and the two names the engine reads are also checked as concrete
// numbers rather than another run of the same machinery.
void test_the_lazy_heightmap_refresh_reaches_the_truth() {
  struct Move {
    int y;
    const char* block;
  };
  // The heights that matter are the section joins (15/16) and the world's own
  // ends, where an off-by-one still answers something plausible.
  const Move moves[] = {
      {1, "minecraft:stone"},  {63, "minecraft:stone"},      {63, "minecraft:air"},
      {40, "minecraft:glass"}, {40, "minecraft:air"},        {16, "minecraft:water"},
      {16, "minecraft:air"},   {16, "minecraft:oak_leaves"}, {15, "minecraft:stone"},
      {1, "minecraft:air"},
  };
  std::printf("-- the lazy heightmap refresh against a column built from scratch\n");
  mc::world::Chunk lazy = air_column();
  std::array<std::string, 64> column{}; // the cells still set, by y
  for (size_t i = 0; i < std::size(moves); ++i) {
    lazy.set_block_state(8, moves[i].y, 8, key(moves[i].block));
    column[static_cast<size_t>(moves[i].y)] = moves[i].block;

    // Read in place, which is the path a marked column comes current through
    // (`Chunk::height_at`) and the one the light engine's sky source takes on a
    // stored column. Reading a `snapshot` instead would refresh only the copy,
    // leaving this column's own answer at 0 — and `start_rel` for a stored 0 is
    // the top of the section stack, so the copy would walk the whole column and
    // never exercise the search start at all.
    const int got_motion = mc::world::chunk_motion_blocking_height(lazy, 8, 8);
    const int got_surface = mc::world::chunk_world_surface_height(lazy, 8, 8);

    // The oracle shares no history with it: the same blocks, in the same order,
    // into a chunk that has never carried a heightmap, read the same way.
    mc::world::Chunk oracle = air_column();
    for (size_t j = 0; j <= i; ++j)
      oracle.set_block_state(8, moves[j].y, 8, key(moves[j].block));
    mc::world::chunk_motion_blocking_height(oracle, 8, 8);

    // And the rule itself, stated once here rather than asked of the same
    // machinery again: `MOTION_BLOCKING` is one past the highest cell
    // `blocksMotion()` or a fluid occupies, the world surface one past the
    // highest non-air cell.
    int want_motion = 0;
    int want_surface = 0;
    for (int y = 0; y < 64; ++y) {
      const std::string& block = column[static_cast<size_t>(y)];
      if (block.empty() || block == "minecraft:air")
        continue;
      want_surface = y + 1;
      const bool fluid = block == "minecraft:water" || block == "minecraft:lava";
      if (mc::world::block_blocks_motion(block) || fluid)
        want_motion = y + 1;
    }
    if (got_motion != want_motion || got_surface != want_surface ||
        lazy.heightmaps != oracle.heightmaps)
      std::printf("  diverged after move %zu (%s at y %d): motion %d/%d, surface %d/%d\n", i,
                  moves[i].block, moves[i].y, got_motion, want_motion, got_surface, want_surface);
    CHECK(got_motion == want_motion);
    CHECK(got_surface == want_surface);
    CHECK(lazy.heightmaps == oracle.heightmaps);
  }
}

// ---- what the write-out reads -----------------------------------------------

// The write-out emits each of the centre chunk's layer arrays from one array it
// takes once per (section, layer), instead of asking the 3x3 region for every
// one of the 4096 cells. That array is the centre chunk's own only because the
// region's index for a centre-chunk cell *is* the centre chunk's own slot — an
// identity with two independent terms, which chunk and which section. This pins
// both: a wrong chunk term would write a neighbour's light into this chunk and a
// wrong section term another section's, and neither mistake is visible in a
// world whose nine chunks hold the same terrain, so each case below gives the
// region somewhere *else* the value could have come from.
void test_write_out_reads_the_centre() {
  std::printf("-- the write-out reads the centre chunk's own arrays\n");

  // The chunk term. A glowstone in the west neighbour at that chunk's local x 0
  // is 16 cells west of this chunk, one past the 15 a level reaches, so no cell
  // here can see it — while the region's own array for the neighbour holds its
  // 15 at the same (section, local y, local z) this write-out reads. A slot
  // built from the wrong chunk index would carry it across.
  {
    auto world = std::make_shared<mc::world::MemoryWorld>(/*min_section_y=*/0, /*section_count=*/4);
    CHECK(world->set_block_state(-1, 0, 0, 16, 8, key("minecraft:glowstone")));
    // The emitter really is where the distance was measured from.
    CHECK(world->get_block_state(-1, 0, 0, 16, 8).name == "minecraft:glowstone");
    mc::world::relight_world_chunk(0, 0, *world);
    const auto centre = world->load_chunk(0, 0);
    CHECK(centre.has_value());
    if (centre) {
      CHECK(mc::world::chunk_light_level(*centre, 0, 16, 8, mc::world::LightLayer::kBlock) == 0);
      // Every section of this chunk, not just that one: nothing in it is lit.
      for (const auto& section : centre->sections)
        CHECK(!section.block_light.has_value());
    }
  }

  // The section term. A glowstone at the bottom of the top section is 17 cells
  // above the top of section 1, so sections 0 and 1 hold no block light at all
  // while 2 and 3 do. One array per section, taken with that section's index:
  // an array reused across the loop (or indexed by a stale section) would put
  // section 3's light into 0 and 1.
  {
    auto world = std::make_shared<mc::world::MemoryWorld>(/*min_section_y=*/0, /*section_count=*/4);
    CHECK(world->set_block_state(0, 0, 8, 48, 8, key("minecraft:glowstone")));
    mc::world::relight_world_chunk(0, 0, *world);
    const auto centre = world->load_chunk(0, 0);
    CHECK(centre.has_value());
    if (centre) {
      CHECK(!centre->sections[0].block_light.has_value());
      CHECK(!centre->sections[1].block_light.has_value());
      CHECK(centre->sections[2].block_light.has_value());
      CHECK(centre->sections[3].block_light.has_value());
      CHECK(mc::world::chunk_light_level(*centre, 8, 48, 8, mc::world::LightLayer::kBlock) == 15);
      CHECK(mc::world::chunk_light_level(*centre, 9, 48, 8, mc::world::LightLayer::kBlock) == 14);
      // The sky rule's own two ends, in the section the source is in: an opaque
      // block's cell is written dark and the air beside it stays full sky.
      CHECK(mc::world::chunk_light_level(*centre, 8, 48, 8, mc::world::LightLayer::kSky) == 0);
      CHECK(mc::world::chunk_light_level(*centre, 9, 48, 8, mc::world::LightLayer::kSky) == 15);
    }
  }

  // The smallest region there is: one section, whose centre slot is the last one
  // the region has (its layer arrays are 9 * 1 * 2 long). Arithmetic sized for
  // the overworld's 24 would run off the end of this one.
  {
    auto world = std::make_shared<mc::world::MemoryWorld>(/*min_section_y=*/0, /*section_count=*/1);
    CHECK(world->set_block_state(0, 0, 8, 8, 8, key("minecraft:glowstone")));
    mc::world::relight_world_chunk(0, 0, *world);
    const auto centre = world->load_chunk(0, 0);
    CHECK(centre.has_value());
    if (centre) {
      CHECK(centre->sections.size() == 1);
      CHECK(mc::world::chunk_light_level(*centre, 8, 8, 8, mc::world::LightLayer::kBlock) == 15);
      CHECK(mc::world::chunk_light_level(*centre, 9, 8, 8, mc::world::LightLayer::kBlock) == 14);
      CHECK(centre->sections[0].block_light.has_value());
      CHECK(centre->sections[0].sky_light.has_value());
    }
  }

  // Empty input: a world with no sections at all. There is no light to compute
  // and nothing to write out — the caller's own early return, so the loop that
  // would index an empty region never runs.
  {
    auto world = std::make_shared<mc::world::MemoryWorld>(/*min_section_y=*/0, /*section_count=*/0);
    const mc::world::LightChange nothing = mc::world::relight_world_chunk(0, 0, *world);
    CHECK(!nothing.any());
    const auto centre = world->load_chunk(0, 0);
    CHECK(!centre.has_value() || centre->sections.empty());
  }
}

// ---- the region scratch between relights ------------------------------------

// One layer's arrays, as the world holds them, for a byte-for-byte comparison.
std::vector<std::optional<mc::world::LightArray>> light_arrays(const mc::world::Chunk& chunk,
                                                               mc::world::LightLayer layer) {
  std::vector<std::optional<mc::world::LightArray>> out;
  out.reserve(chunk.sections.size());
  for (const mc::world::Section& section : chunk.sections)
    out.push_back(layer == mc::world::LightLayer::kSky ? section.sky_light : section.block_light);
  return out;
}

// The engine no longer builds its region from scratch on every relight: the
// light arrays live in one per-thread scratch that survives between calls and is
// re-sized and re-cleared for whichever chunk comes next. So a relight's answer
// must not depend on what the *previous* relight left in those bytes — including
// a previous relight of a chunk with a different section count, which sizes the
// same buffer differently. This pins both halves through the real entry point:
// a column relit, then a differently-shaped one, then the first again must come
// out identical to a column that never shared the scratch; and two same-shaped
// columns in a row must not leak the first one's light into the second.
void test_the_region_scratch_between_relights() {
  std::printf("-- the region scratch is rebuilt for every relight\n");
  const auto made = [](int sections, const char* block, int y) {
    auto world = std::make_shared<mc::world::MemoryWorld>(/*min_section_y=*/0, sections);
    if (block != nullptr)
      world->set_block_state(0, 0, 8, y, 8, key(block));
    return world;
  };

  // A four-section column with a source in its bottom section.
  auto tall = made(4, "minecraft:glowstone", 8);
  mc::world::relight_world_chunk(0, 0, *tall);
  const auto tall_first = tall->load_chunk(0, 0);
  CHECK(tall_first.has_value());
  CHECK(mc::world::chunk_light_level(*tall_first, 8, 8, 8, mc::world::LightLayer::kBlock) == 15);
  CHECK(mc::world::chunk_light_level(*tall_first, 13, 8, 8, mc::world::LightLayer::kBlock) == 10);
  const auto tall_sky_first = light_arrays(*tall_first, mc::world::LightLayer::kSky);
  const auto tall_block_first = light_arrays(*tall_first, mc::world::LightLayer::kBlock);

  // The one-section column: the same source, a different section count, so the
  // shared buffer is re-sized down and re-cleared.
  auto small = made(1, "minecraft:glowstone", 8);
  mc::world::relight_world_chunk(0, 0, *small);
  const auto small_chunk = small->load_chunk(0, 0);
  CHECK(small_chunk.has_value() && small_chunk->sections.size() == 1);
  CHECK(mc::world::chunk_light_level(*small_chunk, 8, 8, 8, mc::world::LightLayer::kBlock) == 15);
  CHECK(mc::world::chunk_light_level(*small_chunk, 9, 8, 8, mc::world::LightLayer::kBlock) == 14);

  // Back to the tall column: every section of both layers, byte for byte.
  mc::world::relight_world_chunk(0, 0, *tall);
  const auto tall_again = tall->load_chunk(0, 0);
  CHECK(tall_again.has_value());
  CHECK(light_arrays(*tall_again, mc::world::LightLayer::kSky) == tall_sky_first);
  CHECK(light_arrays(*tall_again, mc::world::LightLayer::kBlock) == tall_block_first);

  // And the oracle: the same column in a world that never shared the scratch.
  // Its first relight has to equal the interleaved one, so the checks above pin
  // the *right* answer and not merely that relighting twice agrees with itself.
  auto oracle = made(4, "minecraft:glowstone", 8);
  mc::world::relight_world_chunk(0, 0, *oracle);
  const auto oracle_chunk = oracle->load_chunk(0, 0);
  CHECK(oracle_chunk.has_value());
  CHECK(light_arrays(*oracle_chunk, mc::world::LightLayer::kSky) == tall_sky_first);
  CHECK(light_arrays(*oracle_chunk, mc::world::LightLayer::kBlock) == tall_block_first);

  // Same section count, different content, one after the other. The bytes belong
  // to the region and not to the world, so a relight that re-sized the buffer
  // but did not clear it would carry the lit column's light into the dark one.
  auto lit = made(4, "minecraft:glowstone", 8);
  auto dark = made(4, nullptr, 0);
  mc::world::relight_world_chunk(0, 0, *lit);
  mc::world::relight_world_chunk(0, 0, *dark);
  const auto dark_chunk = dark->load_chunk(0, 0);
  CHECK(dark_chunk.has_value());
  for (const mc::world::Section& section : dark_chunk->sections)
    CHECK(!section.block_light.has_value());
}

// ---- generation-time light against a relight ---------------------------------
//
// The send path no longer relights a freshly generated column:
// `generated_chunk.cpp` runs the engine's own sky pass (the per-column source
// rule and the flood) over the column's own terrain, with
// `LightEnvironment::centre_only` set so the unmodelled halo is neither seeded
// nor entered. It is exact wherever the light comes from the column itself, and
// blind to light that crosses a chunk border from a neighbour's source. The
// notes call that an approximation; this is where it stops being an assumption
// about the size of the error and becomes a count.
//
// The comparison itself — generate, keep the light generation wrote, relight the
// same column over its eight generated neighbours, walk both layers cell for
// cell — is `tools/light_seam.h`, shared with `plusmc-light-seam` so this test's
// pinned numbers and the benchmark's records cannot drift apart. The properties
// asserted here are what the approximation claims:
//   * above the column's own surface the two agree exactly: a cell there has an
//     unobstructed column to the sky and the source rule writes 15 directly, so
//     that half is the answer and not an approximation;
//   * every disagreement is below the surface, and it is always *dimmer*: the
//     pass is a subset of the engine's — the column's own sources, no halo — so
//     it can miss spill a neighbour brings in, but it never invents light (the
//     old one-step border shading did, and it put that light inside solid
//     blocks);
//   * the spill is concentrated on the chunk's edge and reaches at most 14
//     blocks in, which is the reach of a level-15 source before it decays out.
// The pinned numbers are on the two chunks the worldgen fixtures pin: ocean at
// (0,0) and land at (-1,-1), on the store's decorated column (surface rules,
// carvers and the feature stage). They live in `tools/light_seam.h` beside the
// shared tally, and `plusmc-light-seam` cross-checks them too. A change to the
// generator or to the light engine that moves them has to move them
// deliberately.

void print_seam_tally(const char* layer, const mc::bench::SeamTally& tally) {
  std::printf("  %s: %lld/%lld cells agree; %lld too bright (max +%d), %lld too dim "
              "(max -%d)\n",
              layer, tally.agree, tally.cells, tally.brighter, tally.max_brighter, tally.dimmer,
              tally.max_dimmer);
  std::printf("    disagreements: %lld above the surface, %lld on the chunk edge, "
              "%lld interior, %lld inside an opaque block\n",
              tally.above_surface, tally.border, tally.interior, tally.in_opaque);
}

void test_generated_light_seam() {
  std::printf("-- generation-time sky light against the engine's own relight\n");
  // The same seed and the same two chunk positions the worldgen fixtures pin —
  // an ocean column and a land column — generated the way the store builds them
  // (decorated), not as the carved dumps describe them.
  const auto generator = std::make_shared<mc::world::WgOverworldGenerator>(mc::bench::kPinnedSeed);
  const std::pair<int, int> cells[] = {{0, 0}, {-1, -1}};
  static_assert(std::size(mc::bench::kPinnedSky) == std::size(cells),
                "one pinned tally per column");
  static_assert(std::size(mc::bench::kPinnedBlock) == std::size(cells),
                "one pinned block tally per column");
  mc::bench::SeamTally sky_total, block_total;
  int moved_columns = 0;
  for (std::size_t cell_index = 0; cell_index < std::size(cells); ++cell_index) {
    const auto& [cx, cz] = cells[cell_index];
    // A fresh world per cell, so neither column's relight can be influenced by
    // the other's stored neighbours.
    const mc::bench::SeamColumn measured = mc::bench::seam_measure(generator, cx, cz);
    CHECK(measured.measured);
    if (!measured.measured)
      continue;
    // The premise of the send-path shortcut (the column already carries light,
    // so neither `chunk_has_light` nor the 3x3 ring runs). `relit_sky` is not
    // asserted per column any more: the generation pass is exact on the ocean
    // column, so its relight legitimately moves nothing. That the comparison is
    // not a no-op is checked across the pair below.
    CHECK(measured.generated_light);
    moved_columns += measured.relit_sky ? 1 : 0;

    std::printf(" chunk (%d, %d):\n", cx, cz);
    const mc::bench::SeamTally& sky = measured.sky;
    const mc::bench::SeamTally& block = measured.block;
    print_seam_tally("sky", sky);
    print_seam_tally("block", block);

    // The two claims the fill makes, exactly.
    CHECK(sky.above_surface == 0);
    CHECK(block.above_surface == 0);
    CHECK(sky.agree + sky.brighter + sky.dimmer == sky.cells);
    CHECK(block.agree + block.brighter + block.dimmer == block.cells);
    CHECK(sky.cells == static_cast<long long>(16) * 16 * 384);
    CHECK(block.cells == static_cast<long long>(16) * 16 * 384);
    // The generation pass seeds the column's own sources and nothing else, so
    // it is a subset of the engine's answer: it can be dim at a border where a
    // neighbour's light reaches in, but it can never be brighter anywhere.
    CHECK(sky.brighter == 0);
    // Generation writes the block layer too (the decorated stage leaves emitters
    // a client must receive lit), so the same subset argument holds: it seeds and
    // floods only the column's own sources and can be dim where a neighbour's
    // light reaches in, never brighter.
    CHECK(block.brighter == 0);
    // The measured error, pinned per chunk so a change cannot move it between
    // them and leave the aggregate unmoved.
    const mc::bench::SeamPinnedSky& want = mc::bench::kPinnedSky[cell_index];
    CHECK(sky.agree == want.agree);
    CHECK(sky.brighter == want.brighter);
    CHECK(sky.dimmer == want.dimmer);
    // The split of that shortfall: the edge a neighbour's light crosses first,
    // and the spill that reached further in. Pinned separately so a change
    // cannot move cells between the two halves and leave the total unmoved.
    CHECK(sky.border == want.border);
    CHECK(sky.interior == want.interior);
    CHECK(sky.in_opaque == want.in_opaque);

    // The block layer, pinned per chunk for the same reason.
    const mc::bench::SeamPinnedSky& want_block = mc::bench::kPinnedBlock[cell_index];
    CHECK(block.agree == want_block.agree);
    CHECK(block.brighter == want_block.brighter);
    CHECK(block.dimmer == want_block.dimmer);
    CHECK(block.border == want_block.border);
    CHECK(block.interior == want_block.interior);
    CHECK(block.in_opaque == want_block.in_opaque);

    sky_total.add(sky);
    block_total.add(block);
  }
  std::printf(" both chunks:\n");
  print_seam_tally("sky", sky_total);
  print_seam_tally("block", block_total);
  // The comparison is not a no-op: at least one of the two columns has a cell
  // where a neighbour's light crosses the border and the column-local pass does
  // not know it.
  CHECK(moved_columns >= 1);
  // The worst cell either way: the pass never brightens a cell, and the worst
  // shortfall is the decayed spill the ring would have brought in.
  CHECK(sky_total.max_brighter == mc::bench::kPinnedMaxBrighter);
  CHECK(sky_total.max_dimmer == mc::bench::kPinnedMaxDimmer);
  CHECK(block_total.max_brighter == mc::bench::kPinnedMaxBrighter);
  CHECK(block_total.max_dimmer == mc::bench::kPinnedMaxBlockDimmer);
}

} // namespace

int main(int argc, char** argv) {
  const std::string fixtures = argc > 1 ? argv[1] : "";
  std::printf("world_light_test\n");

  {
    // A small in-memory column for the range arithmetic, so the check does not
    // depend on the fixture being present.
    auto world = std::make_shared<mc::world::MemoryWorld>();
    auto chunk = world->load_chunk(0, 0);
    CHECK(chunk.has_value());
    if (chunk)
      test_section_range(*chunk);
  }
  test_write_recording();
  test_sky_darken();
  test_light_update_packet();
  test_world_raw_brightness();
  test_the_write_gate_against_the_light_itself();
  test_the_lazy_heightmap_refresh_reaches_the_truth();
  test_write_out_reads_the_centre();
  test_the_region_scratch_between_relights();
  test_generated_light_seam();
  test_relight_after_a_write();
  test_relight_against_a_real_chunk(fixtures);
  test_anvil_content_version(fixtures);
  test_anvil_stored_column(fixtures);

  if (failures == 0)
    std::printf("world_light_test: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
