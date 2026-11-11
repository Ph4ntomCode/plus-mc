// SPDX-License-Identifier: GPL-3.0-or-later
//
// The store's chunk budget: what it holds under pressure, what it must not
// throw away, and what a live borrow forbids.
//
// `MemoryWorld` keeps `kChunkBudget` (4096) columns by default and serves the
// least-recently-served one when another is stored on top. That budget is not a
// detail: it is why a population whose views add up to more chunks than that
// plateaus at ~1.9 GB instead of growing with its view set, and it is the
// number the per-view memory accounting turned up (docs/PHASE5.md §4,
// "Per-view memory": 40 bots spread 256 blocks apart at view distance 8 were
// handed 10,449 distinct chunks and peaked at 2.04 GB — the delivered count, not
// the resident one).
//
// The rule the eviction has to obey is the one this file pins. A stored column
// is one of two things:
//
//   * **regenerable** — it was stored because something *read* it with a live
//     reference (the chunk-send path's relight is the one that does this at
//     scale), and `fresh_chunk` builds exactly the same column again. Dropping
//     it costs a rebuild and no world state.
//   * **irreplaceable** — it carries a write from `set_block_state`, and
//     MemoryWorld has no region file to write that write back to, so the map is
//     the only copy of it that exists.
//
// So the checks are:
//
//   1. **bounded**: after storing far more columns than the budget, the store
//      holds exactly the configured column count;
//   2. **the write survives**: the written column is the *oldest* in the store
//      (the entry a blind LRU takes first) and after ~500 evictions have
//      happened around it, it is still resident and still says what was written
//      — on the pre-fix code this check fails, which is what makes it a
//      regression test rather than a description;
//   3. **a load is not a store**: reading a column that was evicted answers the
//      generated column and leaves the resident count alone, so eviction can
//      never be undone by a reader quietly re-storing what it read;
//   4. **a byte ceiling is enforced too, and it is the *incoming* column's bytes
//      that are checked** — a byte budget that only noticed after the fact would
//      sit over its ceiling for as long as it took to look (the header's note on
//      `store_or_find_locked`). This is the dimension a host actually plans
//      with: 4096 columns is ~1.9 GB for overworld columns and a fraction of
//      that for void air ones.
//   5. **a live borrow is never evicted out from under its holder**, and neither
//      is a written column — the store grows past its ceiling instead, which is
//      the one case where "over budget" is the correct answer (`ChunkLease`,
//      world/world_interface.h). Without the pin, the byte budget's much
//      smaller caps would make a borrow a dangling reference.
//   6. **the idle release gives back only what it may**: it drops regenerable
//      columns down to `keep`, skips written ones *and* borrowed ones, reports
//      how many it took, and stops at the host's floor rather than at zero.
//   7. **the cells are the OS's to take back**: a column's 16 KiB block-cell
//      arrays are mappings of their own rather than blocks of the shared heap,
//      so a released column's frames leave the resident set whole. A host that
//      raised `PLUSMC_PAGE_ALLOC_MIN_KB` past 16 KiB has asked for the heap
//      back and gets this arm skipped, not failed.
//   8. **the light arrays are too, at no extra cost**: a 2 KiB array cannot be
//      mapped on its own without spending a page on half a page, so the pool
//      packs two arrays into one mapping and unmaps it when both slots are free
//      (`core/slab_pool.h`). A host that set `PLUSMC_LIGHT_ALLOC=0` has asked
//      for the heap and gets this arm skipped.
//
// The fallback arm — every resident column written, so one of them has to go and
// the store logs it — is reachable only by writing `kChunkBudget` distinct
// columns (~400 MB of full-height columns), which is not a unit test's business;
// it is left to the soak that owns the budget's size.
//
// The store is built with a single section (a real column has 24) because the
// budget is a *count* by default: one section exercises the same rule at ~1% of
// the memory, which is what lets the flood run in a test. The byte-ceiling arm
// uses a budget derived from the store's own measured column size rather than a
// hard-coded number, so it stays honest if the column's shape changes.
//
// Usage: chunk_cache_test

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <utility>

#include "core/page_alloc.h"
#include "world/block_states.h"
#include "world/chunk.h"
#include "world/memory_world.h"

namespace {

int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

using mc::world::BlockStateKey;
using mc::world::ChunkBudget;
using mc::world::MemoryWorld;

BlockStateKey block(const char* name) {
  return BlockStateKey{name, ""};
}

// Distinct coordinates for the flood, far from the written column's (5, 5): a
// row of 1024 in x, then the next z. No repeats, and no dependence on the
// budget's value.
std::pair<int, int> flood_coord(std::size_t i) {
  return {100 + static_cast<int>(i % 1024), 100 + static_cast<int>(i / 1024)};
}

void budget_check() {
  // The default ceiling the docs quote (~1.9 GB of overworld columns) is the
  // store's own default, so the two cannot drift apart silently.
  CHECK(MemoryWorld().budget().columns == MemoryWorld::kChunkBudget);
  MemoryWorld world(/*min_section_y=*/0, /*section_count=*/1);
  CHECK(world.resident_columns() == 0);
  CHECK(world.columns_with_writes() == 0);

  // The column that gets written to, touched *first*, so it is the oldest entry
  // in the store and the first thing a blind LRU would take.
  CHECK(world.set_block_state(5, 5, 0, 0, 0, block("minecraft:stone")));
  CHECK(world.resident_columns() == 1);
  CHECK(world.columns_with_writes() == 1);
  CHECK(world.columns_stored() == 1);
  // Re-typing the same state changes nothing, and must not turn a generated
  // column into an irreplaceable one -- nor count as another store.
  CHECK(world.set_block_state(5, 5, 0, 0, 0, block("minecraft:stone")));
  CHECK(world.columns_with_writes() == 1);
  CHECK(world.columns_stored() == 1);

  // Store regenerable columns — the shape the send path's relight produces —
  // until the budget has been passed by a margin, so eviction has run many
  // times over the written column's head.
  constexpr std::size_t kExtra = 512;
  for (std::size_t i = 0; i < MemoryWorld::kChunkBudget + kExtra; ++i) {
    const auto [cx, cz] = flood_coord(i);
    CHECK(world.modify_chunk(cx, cz).has_value());
  }

  // 1. Bounded: every insertion past the budget evicted one, so the store is
  // exactly at its ceiling and 512 columns have been dropped.
  CHECK(world.resident_columns() == MemoryWorld::kChunkBudget);
  CHECK(world.columns_with_writes() == 1);
  // ...while the cumulative counter kept every store. This is the property the
  // metrics line's `world_chunks_stored` exists for: the resident gauge cannot
  // say how much was built once the budget has evicted anything.
  CHECK(world.columns_stored() == 1 + MemoryWorld::kChunkBudget + kExtra);

  // 2. The write survived the eviction that took the columns around it.
  {
    const auto kept = world.load_chunk(5, 5);
    CHECK(kept.has_value());
    if (kept)
      CHECK(kept->block_state_at(0, 0, 0).name == "minecraft:stone");
  }

  // 3. The first flooded column is long gone, its coordinates still answer, and
  // the answer is the generated column — a read, not a re-store.
  const std::size_t resident_before = world.resident_columns();
  const auto [fx, fz] = flood_coord(0);
  const auto regenerated = world.load_chunk(fx, fz);
  CHECK(regenerated.has_value());
  if (regenerated) {
    CHECK(regenerated->sections.size() == 1);
    CHECK(regenerated->block_state_at(0, 0, 0).name == "minecraft:air");
  }
  CHECK(world.resident_columns() == resident_before);
  CHECK(world.columns_with_writes() == 1);
  // A read is not a store, resident or rebuilt: only an insertion counts.
  CHECK(world.columns_stored() == 1 + MemoryWorld::kChunkBudget + kExtra);

  // And the store still answers for the *written* column after more pressure,
  // which is the property the budget must not be allowed to trade away.
  for (std::size_t i = 0; i < kExtra; ++i) {
    const auto [cx, cz] = flood_coord(MemoryWorld::kChunkBudget + kExtra + i);
    CHECK(world.modify_chunk(cx, cz).has_value());
  }
  const auto kept_again = world.load_chunk(5, 5);
  CHECK(kept_again.has_value());
  if (kept_again)
    CHECK(kept_again->block_state_at(0, 0, 0).name == "minecraft:stone");
  CHECK(world.resident_columns() == MemoryWorld::kChunkBudget);
  CHECK(world.columns_with_writes() == 1);

  // The counter is cumulative, not another gauge: the idle release and `clear()`
  // take columns back without taking the count back.
  constexpr std::size_t kStored = 1 + MemoryWorld::kChunkBudget + 2 * kExtra;
  CHECK(world.columns_stored() == kStored);
  CHECK(world.release_idle_columns(0) > 0);
  CHECK(world.columns_stored() == kStored);
  world.clear();
  CHECK(world.resident_columns() == 0);
  CHECK(world.columns_stored() == kStored);
}

// One column's own estimate, taken from the store rather than assumed, so the
// byte arm below cannot drift from what `Chunk::estimated_bytes` reports.
std::size_t column_bytes() {
  MemoryWorld probe(/*min_section_y=*/0, /*section_count=*/1);
  CHECK(probe.modify_chunk(0, 0).has_value());
  const std::size_t bytes = probe.resident_bytes();
  CHECK(bytes > 0);
  return bytes;
}

void byte_budget_check() {
  const std::size_t one_column = column_bytes();
  // Room for five columns, and the ceiling is checked *before* each insert:
  // a store that never walks the map can still answer "would this fit".
  const std::size_t ceiling = one_column * 5;
  MemoryWorld world(/*min_section_y=*/0, /*section_count=*/1, /*generator=*/nullptr,
                    ChunkBudget{/*columns=*/0, ceiling});
  CHECK(world.resident_columns() == 0);
  CHECK(world.resident_bytes() == 0);

  for (std::size_t i = 0; i < 40; ++i) {
    const auto [cx, cz] = flood_coord(i);
    CHECK(world.modify_chunk(cx, cz).has_value());
  }
  // The byte side, not the count side, is what bounded this: 40 columns stored
  // against no column ceiling at all, and the store is under its byte ceiling.
  CHECK(world.resident_columns() > 0);
  CHECK(world.resident_columns() < 40);
  CHECK(world.resident_bytes() <= ceiling);
  // The point of the cumulative counter, in the smallest possible form: 40
  // columns went in, the byte ceiling kept fewer resident, and the count still
  // says 40.
  CHECK(world.columns_stored() == 40);

  // A byte ceiling does not protect writes either — but it must never take one
  // while a regenerable column is resident. Write first (oldest in the store),
  // flood past the ceiling, and the write is still there.
  MemoryWorld guarded(/*min_section_y=*/0, /*section_count=*/1, /*generator=*/nullptr,
                      ChunkBudget{/*columns=*/0, ceiling});
  CHECK(guarded.set_block_state(7, 7, 0, 0, 0, block("minecraft:diamond_block")));
  for (std::size_t i = 0; i < 40; ++i) {
    const auto [cx, cz] = flood_coord(1000 + i);
    CHECK(guarded.modify_chunk(cx, cz).has_value());
  }
  CHECK(guarded.columns_with_writes() == 1);
  const auto kept = guarded.load_chunk(7, 7);
  CHECK(kept.has_value());
  if (kept)
    CHECK(kept->block_state_at(0, 0, 0).name == "minecraft:diamond_block");
  CHECK(guarded.resident_bytes() <= ceiling);
}

// The borrow handle (world/world_interface.h's `ChunkLease`): a column a caller
// is holding is not a candidate for eviction, however far over the ceiling the
// store is. Without this, a byte budget would hand a relight a dangling
// reference the first time it filled its 3x3 ring.
void borrow_pin_check() {
  const std::size_t one_column = column_bytes();
  // A ceiling of exactly one column, and a lease held on the column that fills
  // it: from here every insert is over the ceiling by construction, so the store
  // is forced to choose between its budget and the borrow.
  MemoryWorld world(/*min_section_y=*/0, /*section_count=*/1, /*generator=*/nullptr,
                    ChunkBudget{/*columns=*/0, one_column});
  auto held = world.modify_chunk(3, 3);
  CHECK(held.has_value());
  if (!held)
    return;
  CHECK(held->pinned());
  CHECK(world.resident_columns() == 1);
  CHECK(world.resident_bytes() <= one_column);

  // Flood while the lease lives. Every insert that cannot find a victim grows
  // the store instead of touching the borrowed column — the one case where
  // "over budget" is the correct answer, because the alternative is freeing a
  // column a caller is holding a live pointer into.
  for (std::size_t i = 0; i < 12; ++i) {
    const auto [cx, cz] = flood_coord(2000 + i);
    CHECK(world.modify_chunk(cx, cz).has_value());
  }
  // The pinned column is intact and is the *only* thing over the ceiling: the
  // flood columns were still evicted one by one (resident did not grow with the
  // flood, it grew by one), which is the budget doing its job around the pin.
  CHECK(world.resident_columns() == 2);
  CHECK(world.resident_bytes() > one_column);
  // The borrowed column is still the same live object and still writable
  // through the lease: this is the property the pin exists for. (`held` is the
  // optional, so the column is reached through the lease's own `get()`.)
  CHECK(held->get().sections.size() == 1);
  CHECK(held->get().block_state_at(0, 0, 0).name == "minecraft:air");
  CHECK(held->get().set_block_state(0, 0, 0, block("minecraft:emerald_block")));
  CHECK(held->get().block_state_at(0, 0, 0).name == "minecraft:emerald_block");
  // A write through a bare lease is invisible to the store's own bookkeeping —
  // `set_block_state` is the path that records a column as irreplaceable (see
  // the eviction note in the header). That is exactly why the server writes
  // blocks through `set_block_state` and only the light engine writes through a
  // lease (world/world_light.cpp): a light array is recomputed on the next
  // relight, a block write is not.
  CHECK(world.columns_with_writes() == 0);

  // Releasing the lease ends the pin, and the store can now give the column
  // back: it is treated as the regenerable send-path column it was stored as,
  // so the light the relight wrote is dropped rather than the store keeping a
  // column nobody is reading. Everything resident is releasable here.
  held->reset();
  const std::size_t before_release = world.resident_columns();
  CHECK(before_release == 2);
  const std::size_t freed = world.release_idle_columns(0);
  CHECK(freed == before_release);
  CHECK(world.resident_columns() == 0);
  CHECK(world.columns_with_writes() == 0);
  const auto after_release = world.load_chunk(3, 3);
  CHECK(after_release.has_value());
  if (after_release)
    CHECK(after_release->block_state_at(0, 0, 0).name == "minecraft:air");
}

// The documented last resort, pinned so it cannot change silently: when every
// resident column carries a write and the store is over a ceiling, something
// irreplaceable has to go (the store's note on `evict_if_needed_locked`). At the
// 4096-column default this arm is essentially unreachable; a *byte* ceiling
// reaches it the moment the world's own written columns fill the budget, which
// is the honest cost of the tighter knob and the reason the store logs it.
void byte_ceiling_fallback_check() {
  const std::size_t one_column = column_bytes();
  MemoryWorld world(/*min_section_y=*/0, /*section_count=*/1, /*generator=*/nullptr,
                    ChunkBudget{/*columns=*/0, one_column});
  CHECK(world.set_block_state(1, 1, 0, 0, 0, block("minecraft:bedrock")));
  CHECK(world.resident_columns() == 1);
  CHECK(world.columns_with_writes() == 1);

  // One more write does not fit, and there is nothing regenerable to take: the
  // least-recently-served write is dropped (the log says so) and the new one is
  // stored. The store keeps its ceiling and loses the older edit — which is the
  // trade the ceiling makes, printed rather than hidden.
  CHECK(world.set_block_state(1, 2, 0, 0, 0, block("minecraft:dirt")));
  CHECK(world.resident_columns() == 1);
  CHECK(world.columns_with_writes() == 1);
  const auto dropped = world.load_chunk(1, 1);
  CHECK(dropped.has_value());
  if (dropped)
    CHECK(dropped->block_state_at(0, 0, 0).name == "minecraft:air");
  const auto kept = world.load_chunk(1, 2);
  CHECK(kept.has_value());
  if (kept)
    CHECK(kept->block_state_at(0, 0, 0).name == "minecraft:dirt");
}

// The idle release: the other trigger, for the moment nothing is watching.
// Eviction only runs when a column is stored, so a population that disconnects
// leaves its view set resident — measured at ~2 GB ten seconds after the last
// bot left (docs/PHASE5.md §4). `release_idle_columns(keep)` is the store's way
// back down, and its contract is the same one the eviction keeps: written and
// borrowed columns stay.
void idle_release_check() {
  MemoryWorld world(/*min_section_y=*/0, /*section_count=*/1);
  // Written: never released.
  CHECK(world.set_block_state(9, 9, 0, 0, 0, block("minecraft:bedrock")));
  for (std::size_t i = 0; i < 20; ++i) {
    const auto [cx, cz] = flood_coord(3000 + i);
    CHECK(world.modify_chunk(cx, cz).has_value());
  }
  CHECK(world.resident_columns() == 21);

  // A borrow is a stay too: hold one regenerable column and release everything
  // else, then check both the borrowed and the written one survived.
  auto held = world.modify_chunk(4000, 4000);
  CHECK(held.has_value());
  CHECK(world.resident_columns() == 22);

  const std::size_t released = world.release_idle_columns(0);
  CHECK(released == 20);
  CHECK(world.resident_columns() == 2);
  CHECK(world.columns_with_writes() == 1);
  if (held)
    CHECK(held->get().block_state_at(0, 0, 0).name == "minecraft:air");

  // `keep` is a floor on the *releasable* columns, not a promise about the
  // store's size: with two stays resident, asking to keep 5 releases nothing
  // and leaves both.
  CHECK(world.release_idle_columns(5) == 0);
  CHECK(world.resident_columns() == 2);

  // A disk-backed world has nothing to rebuild, so its default answer is 0 and
  // it does nothing (world/world_interface.h).
  held->reset();
  CHECK(world.release_idle_columns(0) == 1);
  CHECK(world.resident_columns() == 1);
  CHECK(world.columns_with_writes() == 1);
}

// The floor the *host* passes (`main.cpp`'s idle-release floor, 64 columns by
// default, `PLUSMC_CHUNK_FLOOR_COLUMNS`). The release above is asked for `keep`
// of zero; the server asks for a small non-zero one so a player reconnecting
// between two passes finds the terrain it was just looking at still built. What
// is checkable from outside is the contract, in counts:
//
//   * the store stops at `keep` + whatever it may not release (a written column
//     is outside the floor — the floor bounds the *releasable* columns);
//   * asking again at the floor releases nothing, so the gesture is idempotent
//     rather than a repeated sweep;
//   * a borrowed column is outside the floor too, and the release still reaches
//     exactly the floor around it.
//
// Which *particular* columns survive is the LRU order's business, and it is
// deliberately not observable here: two regenerable columns are the same column
// to every read this interface offers (world/world_interface.h), so a check that
// named one would be checking the map's iteration order, not the contract. The
// pin in the third arm is what makes the order matter to a count.
void idle_release_floor_check() {
  constexpr std::size_t kFloor = 8;
  MemoryWorld world(/*min_section_y=*/0, /*section_count=*/1);
  CHECK(world.set_block_state(11, 11, 0, 0, 0, block("minecraft:bedrock")));
  for (std::size_t i = 0; i < 30; ++i) {
    const auto [cx, cz] = flood_coord(4000 + i);
    CHECK(world.modify_chunk(cx, cz).has_value());
  }
  CHECK(world.resident_columns() == 31);

  // 30 releasable columns down to a floor of 8 is 22 gone, and the written one
  // does not count against the floor: the store ends at the floor *plus* its
  // stays. (Charging the write against the floor would leave 7, which is the
  // reading the header's contract — a floor on the releasable columns — rules
  // out; this check is what makes the two readings distinguishable.)
  CHECK(world.release_idle_columns(kFloor) == 22);
  CHECK(world.resident_columns() == kFloor + 1);
  CHECK(world.columns_with_writes() == 1);

  // Idempotent at the floor: nothing left above it to take.
  CHECK(world.release_idle_columns(kFloor) == 0);
  CHECK(world.resident_columns() == kFloor + 1);
  // And the floor is not a hiding place for the world's state: the write is
  // still the write (it was never a release candidate in the first place).
  const auto written = world.load_chunk(11, 11);
  CHECK(written.has_value());
  if (written)
    CHECK(written->block_state_at(0, 0, 0).name == "minecraft:bedrock");

  // A borrow sits outside the floor and does not stop the release from reaching
  // it: with the oldest regenerable column held and a floor of 4, the release
  // still reports the 8 it took and the store ends up at 4 + the held one.
  MemoryWorld pinned(/*min_section_y=*/0, /*section_count=*/1);
  auto held = pinned.modify_chunk(flood_coord(0).first, flood_coord(0).second);
  CHECK(held.has_value());
  for (std::size_t i = 1; i < 12; ++i) {
    const auto [cx, cz] = flood_coord(i);
    CHECK(pinned.modify_chunk(cx, cz).has_value());
  }
  CHECK(pinned.resident_columns() == 12);
  CHECK(pinned.release_idle_columns(4) == 7); // 11 releasable minus the floor of 4
  CHECK(pinned.resident_columns() == 5);
  if (held)
    CHECK(held->get().sections.size() == 1);
  held->reset();
  CHECK(pinned.release_idle_columns(4) == 1);
  CHECK(pinned.resident_columns() == 4);
}

// Where a column's cells live, and what they cost while it is kept. Every arm
// above is about *which* columns the store keeps; this one is about what a kept
// column is worth, and what dropping one costs the process.
//
// The cells are packed now (world/section_cells.h): 4, 8 or 32 bits each, or no
// buffer at all for a single-state section. So the old shape of this check —
// "every section owns a page-aligned 16 KiB array" — is gone along with the
// 384 KiB a full-height column used to spend on cells, and what has to be pinned
// in its place is both halves of the new contract:
//
//   uniform   0 B     nothing to unmap, and nothing to fault in (most of a
//                     generated column: an all-air or single-state section)
//   4-bit     2048 B  below `core/page_alloc.h`'s cutoff, an ordinary heap block
//   8-bit     4096 B  exactly one page, its own mapping
//   32-bit   16384 B  four pages, its own mapping
//
// The mapping half is what the idle release leans on: a block out of the shared
// heap cannot be given back once the store's own load/evict churn has left live
// allocations in every region around it, which is why dropping a column used to
// return 269 MB of a 1.83 GB release and returns 1.52 GB of it with the page
// allocator (docs/PHASE5.md §4 has both measurements and the paired pre-change
// binary behind them).
//
// The cutoff is read through the allocator's own predicate rather than
// re-derived, and a host that raised `PLUSMC_PAGE_ALLOC_MIN_KB` — the documented
// way to keep these buffers on the heap — gets the mapping half skipped with a
// reason instead of a failure: it pins the default the server ships.
void cells_storage_check() {
  // The store's own columns are the headline. MemoryWorld fabricates all-air
  // sections, an all-air section is one value, and a single-value section owns no
  // buffer at all — so a freshly loaded column's cells cost nothing, which is the
  // number the 60-player measurement moved.
  MemoryWorld world(/*min_section_y=*/0, /*section_count=*/2);
  const auto chunk = world.load_chunk(7, 7);
  CHECK(chunk.has_value());
  if (!chunk)
    return;
  CHECK(chunk->sections.size() == 2);
  for (const mc::world::Section& s : chunk->sections) {
    CHECK(s.block_cells.size() == static_cast<std::size_t>(mc::world::kBlockCellsPerSection));
    CHECK(s.block_cells.uniform());
    CHECK(s.block_cells.bytes() == 0);
    CHECK(s.block_cells.buffer() == nullptr);
    CHECK(s.block_cells[4095] == 0);
  }

  // The widths, from the container itself. A second value is what materialises
  // the buffer, and the width it materialises at is the smallest that holds both
  // values — vanilla's minimum of four bits, not one.
  mc::world::SectionCells four;
  four.assign(mc::world::kBlockCellsPerSection, 3);
  CHECK(four.uniform());
  CHECK(four.bytes() == 0);
  CHECK(four[0] == 3 && four[4095] == 3);
  four.set(4095, 15);
  CHECK(!four.uniform());
  CHECK(four.bytes() == 2048);
  CHECK(four[0] == 3 && four[4095] == 15);

  mc::world::SectionCells eight;
  eight.assign(mc::world::kBlockCellsPerSection, 0);
  eight.set(0, 200);
  CHECK(eight.bytes() == 4096);
  CHECK(eight[0] == 200 && eight[4095] == 0);

  mc::world::SectionCells wide;
  wide.assign(mc::world::kBlockCellsPerSection, 0);
  wide.set(0, 300);
  CHECK(wide.bytes() == 16384);
  CHECK(wide[0] == 300 && wide[4095] == 0);
  // Grow-only: a value that would have fit at 8 bits must not shrink the buffer
  // back down, because the section that held a big palette once will hold one
  // again and the rebuild is the expensive direction.
  wide.set(1, 7);
  CHECK(wide.bytes() == 16384);
  CHECK(wide[1] == 7);

  // Shrink then grow: `resize` is vector-like, so growing back must not copy
  // past the new buffer (the shrink keeps the larger allocation) and the cells
  // it adds are the zero fill, not whatever the old buffer happened to hold.
  mc::world::SectionCells regrown;
  regrown.assign(mc::world::kBlockCellsPerSection, 1);
  regrown.set(0, 5); // materialise at 4 bits: 256 words
  regrown.resize(16);
  regrown.resize(64); // 4 words: a copy of the old 256 would run past it
  CHECK(regrown.size() == 64);
  CHECK(regrown[0] == 5);
  for (std::size_t i = 16; i < 64; ++i)
    CHECK(regrown[i] == 0);

  // Below the cutoff nothing changed: a section's 2048-byte light array is still
  // an ordinary heap block, which is why the release line separately reports what
  // the allocator gesture found.
  CHECK(!mc::core::buffer_is_page_mapped(2048));
  if (!mc::core::buffer_is_page_mapped(4096)) {
    std::printf("cells_storage_check: mapping half skipped (PLUSMC_PAGE_ALLOC_MIN_KB is "
                "above 4096 bytes, so the cells are the heap's by request)\n");
    return;
  }

  // Past the cutoff the buffer is a mapping of its own: page-aligned, and no page
  // of it is a page the section next to it also uses. Two sections, so the "one
  // mapping each" half is observable.
  MemoryWorld dense(/*min_section_y=*/0, /*section_count=*/2);
  const auto written = dense.modify_chunk(7, 7);
  CHECK(written.has_value());
  if (!written)
    return;
  for (mc::world::Section& s : written->get().sections) {
    CHECK(s.block_cells.uniform());
    // A cell holding palette index 20 needs a byte, so the section materialises
    // at 8 bits: 4096 cells x 1 byte, exactly one page and past the cutoff. (A
    // two-state section would sit at four bits and 2048 bytes, below it — the
    // widths land on both sides by design, and the 4-bit case owns no mapping to
    // check.)
    s.block_cells.set(0, 20);
  }
  const mc::world::SectionCells& lo = written->get().sections[0].block_cells;
  const mc::world::SectionCells& hi = written->get().sections[1].block_cells;
  CHECK(lo.bytes() == 4096 && hi.bytes() == 4096);
  CHECK(lo.page_mapped() && hi.page_mapped());
  CHECK(reinterpret_cast<uintptr_t>(lo.buffer()) % mc::core::page_bytes() == 0);
  CHECK(reinterpret_cast<uintptr_t>(hi.buffer()) % mc::core::page_bytes() == 0);
  // Page-aligned 4 KiB blocks either do not overlap at all or are the same
  // block; the store gave each section its own cells, so it is the former.
  const auto lo_begin = reinterpret_cast<uintptr_t>(lo.buffer());
  const auto hi_begin = reinterpret_cast<uintptr_t>(hi.buffer());
  CHECK(lo_begin + lo.bytes() <= hi_begin || hi_begin + hi.bytes() <= lo_begin);
}

// Where a section's light arrays come from: the arm above is about one buffer per
// column, this one about the 2 KiB arrays a lit column holds 17 of — 34 KB that a
// packed store keeps on the heap and cannot put down. Mapping one array on its
// own would spend a whole page to hold half of one, so the pool packs two arrays
// into a page and unmaps the page when both slots are free (`core/slab_pool.h`).
//
// The page arithmetic is exact only when no light exists yet in this test (the
// worlds above are all air). If a future change lights one of them, the arm still
// checks that every page it handed out comes back, just not which two arrays
// shared it.
void light_slot_check() {
  if (!mc::core::LightSlabPool::enabled()) {
    std::printf("light_slot_check: skipped (PLUSMC_LIGHT_ALLOC=0 keeps the arrays on the heap)\n");
    return;
  }
  const std::size_t page = mc::core::page_bytes();
  const std::size_t slot = mc::core::LightSlabPool::kSlotBytes;
  const std::size_t before = mc::core::LightSlabPool::mapped_pages();
  {
    mc::world::LightArray first(slot, 0);
    mc::world::LightArray second(slot, 0);
    CHECK(first.size() == slot);
    CHECK(second.size() == slot);
    // Two arrays, one mapping: the second allocation takes the first page's free
    // slot rather than a page of its own.
    CHECK(mc::core::LightSlabPool::mapped_pages() == before + 1);
    const auto a = reinterpret_cast<uintptr_t>(first.data());
    const auto b = reinterpret_cast<uintptr_t>(second.data());
    CHECK(a % slot == 0);
    CHECK(b % slot == 0);
    CHECK(a != b);
    if (before == 0) {
      CHECK(a / page == b / page);
      CHECK((a > b ? a - b : b - a) == slot);
    }
    // A buffer that is not one slot does not take a page from the pool.
    std::vector<uint8_t, mc::core::LightSlabAllocator<uint8_t>> smaller(1024, 1);
    CHECK(mc::core::LightSlabPool::mapped_pages() == before + 1);
    first = mc::world::LightArray();
    // One slot still live: the page stays.
    CHECK(mc::core::LightSlabPool::mapped_pages() == before + 1);
  }
  // Both slots free: the mapping goes back, which is the whole point.
  CHECK(mc::core::LightSlabPool::mapped_pages() == before);
}

} // namespace

int main() {
  std::printf("chunk_cache_test\n");
  budget_check();
  byte_budget_check();
  borrow_pin_check();
  byte_ceiling_fallback_check();
  idle_release_check();
  idle_release_floor_check();
  cells_storage_check();
  light_slot_check();
  if (failures != 0) {
    std::fprintf(stderr, "chunk_cache_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("chunk_cache_test: all checks passed\n");
  return 0;
}
