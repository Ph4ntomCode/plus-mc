// SPDX-License-Identifier: GPL-3.0-or-later
//
// In-memory mutable chunk store for the void/flat world mode, and for the
// generated overworld (--worldgen).  When no --world is specified the server
// generates terrain on the fly but needs somewhere to persist block mutations;
// MemoryWorld owns that generation and stores any modified chunk in an
// in-memory map.  Without a generator the fabrication is the void air column
// (void_chunk.cpp's world-side twin); with one it is the post-carver
// overworld column (world/generated_chunk.h).  The difference reaches every read that
// used to answer "generated air": with a generator an unstored position is
// terrain the world has not needed yet, so `has_chunk` says it is there and a
// write builds it before applying.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "world/chunk.h"
#include "world/container.h"
#include "world/world_interface.h"

namespace mc::world {

class WgOverworldGenerator;

// How many columns the store may hold, as a count and/or as a byte estimate
// (`Chunk::estimated_bytes`). A zero in either field means that dimension is not
// bounded, so a host can say "4096 columns", "2 GiB", or both.
//
// This is the knob the per-view memory measurements turned into: the default
// (4096 columns, no byte ceiling) is what made a spread-256 population plateau
// at ~2 GB on this engine, and a byte ceiling is the number a host actually
// plans with — 4096 columns is ~1.9 GB for full-height overworld columns and a
// fraction of that for the void mode's air columns, which is exactly the kind of
// surprise a byte budget removes. `main.cpp` reads it from
// PLUSMC_CHUNK_BUDGET_COLUMNS / PLUSMC_CHUNK_BUDGET_MB.
struct ChunkBudget {
  std::size_t columns = 4096;
  std::size_t bytes = 0; // decimal bytes; 0 = this dimension is unbounded
};

class MemoryWorld : public WorldInterface {
public:
  // `generator` is the overworld noise stage, when this world has one
  // (--worldgen).  The type lives in world/worldgen_chunk.h, which is why it
  // is passed as an incomplete shared_ptr here; the .cpp owns the bridge.
  MemoryWorld(int min_section_y = -4, int section_count = 24,
              std::shared_ptr<const WgOverworldGenerator> generator = nullptr,
              ChunkBudget budget = {})
      : budget_(budget), min_section_y_(min_section_y), section_count_(section_count),
        generator_(std::move(generator)) {}

  // Whether this world fabricates real terrain (rather than air) at positions
  // it has never stored — the question the light ring and the chunk-send path
  // are really asking when they read `has_chunk`/`can_load_chunk`.
  bool generates_terrain() const {
    return generator_ != nullptr;
  }

  // Load a chunk.  Returns a stored copy if one has been mutated, otherwise
  // generates a fresh column: the noise stage's terrain when a generator is
  // configured, the void air column otherwise.  A generated load is a *read*
  // and is not stored — the first write to the position (or the send path's
  // relight, which takes a live reference) is what puts it in the map.
  std::optional<Chunk> load_chunk(int cx, int cz) const override;
  std::optional<ChunkLease> modify_chunk(int cx, int cz) override;
  bool set_block_state(int cx, int cz, int x, int y, int z, const BlockStateKey& key) override;

  int min_section_y() const override {
    return min_section_y_;
  }
  int section_count() const override {
    return section_count_;
  }

  // Cheaper than the default: reads the cached column in place instead of
  // copying it (breaks happen on the session path).
  BlockStateKey get_block_state(int cx, int cz, int x, int y, int z) const override;
  // The light kinds of a cell, from the stored section's palette entry: the
  // same answer as the read above, with the two strings left where they are
  // (world/world_interface.h says why this exists).
  int light_kinds_at(int cx, int cz, int x, int y, int z) const override;

  // The same in-place read for weather's two level questions: a chunk never
  // mutated is the generated (all-air, the_void) column, so both answer from
  // the generator's defaults without materialising a chunk.
  int motion_blocking_height(int x, int z) const override;
  int world_surface_height(int x, int z) const override;
  // With a generator, an unstored position answers the biome source's own
  // biome for the cell the block falls in instead of materialising a column for
  // a lookup the climate alone decides; without one it is the void column's
  // own `minecraft:the_void`.
  std::string biome_name_at(int x, int y, int z) const override;
  // The random-tick pass's section gate, from the stored column.  A chunk the
  // world has not stored yet has nothing the pass can tick — generated air has
  // no sections at all, and a generated column's ticking sections are only
  // known once it is materialised (which sending it does).  Answering 0 here is
  // also what keeps the pass from *forcing* a generation for every chunk in
  // view: this is asked once per chunk per tick.
  uint64_t random_tick_sections(int cx, int cz) const override;
  // In-place read for the chunk-send path's light question: a stored column
  // holds light only once the light engine has computed it, and a column the
  // world has not stored yet — generated air, or generated terrain nobody has
  // sent/relit — carries none either.
  bool has_chunk_light(int cx, int cz) const override;
  // And the same in-place read for the random-tick pass's light gate: a column
  // that was never mutated is generated air, whose light is the sky above it and
  // nothing else.
  int raw_brightness(int x, int y, int z, int sky_darken) const override;
  // One layer's own value, for the natural spawner's light gate (see the note on
  // WorldInterface::light_level).
  int light_level(LightLayer layer, int x, int y, int z) const override;

  // The ring's warm pass (world/world_interface.h): build the column under the
  // store's own two phases and hand out no lease. Deliberately *not* routed
  // through `has_chunk`/`modify_chunk`: this is not a borrow, and a caller that
  // instruments borrows (tests/light_relight_race_test.cpp) must not see one.
  void ensure_chunk_stored(int cx, int cz) override;
  // Whether the column is *in the map* — the one answer that separates "this
  // world would build it" from "this world has built it" (world_interface.h).
  // A generated column nobody has stored is not here, which is exactly what the
  // stream path's `stored` question wants: it is about to be built, and the
  // caller should ask the pool rather than pay for it inline.
  bool has_stored_column(int cx, int cz) const override {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    return chunks_.find(key(cx, cz)) != chunks_.end();
  }
  // Every generation, stored or lost to a publish race (world_interface.h).
  uint64_t generated_columns() const override {
    return generated_columns_.load(std::memory_order_relaxed);
  }

  // The light-input version (world/world_interface.h): a column's version moves
  // on every write that lands on it, so a relight that copied the column can
  // tell whether its copy is still what the store holds. A column nobody has
  // written to keeps the version it was first asked for; a column that is not
  // resident answers 0 (air, as the ring reads it), and so does the instant
  // before a fresh column is first asked for — never a version another read
  // returned, because `next_content_version_` only counts up.
  //
  // The one write it cannot see is a caller writing straight through a borrowed
  // `Chunk` (`modify_chunk`'s lease, `chunk_cache_test.cpp` does this). That is
  // deliberate: the hook is for the relight, and every write the *server* makes
  // goes through `set_block_state` — the tick, the commands and the break/place
  // path all do, with `LightWorld` in front of this store recording them. A
  // caller that writes through a lease and then relies on this version is the
  // one shape it does not cover, and it is not a shape the engine has.
  uint64_t chunk_content_version(int cx, int cz) const override;
  // This store's identity, for the relight's cross-call copy reuse
  // (world/world_interface.h's `content_store_token`). Minted from a
  // process-wide counter, so a destroyed store's number is never handed to the
  // next one — which is what keeps a test's successive worlds apart even when
  // the allocator reuses the address.
  uint64_t content_store_token() const override {
    return store_token_;
  }

  // Whether a column is "there": stored, or — with a generator — terrain this
  // world would fabricate on demand.  In the void mode an unmodified chunk is
  // generated air, so a caller that only asks whether anything is there gets an
  // answer from the map instead of a fabricated ~100 KiB column; with a
  // generator there *is* a real column at every position, and a caller that
  // would otherwise read an absent one as air (the light ring) must build it
  // instead of skipping it.  The build happens through `modify_chunk`, so this
  // stays a cheap map lookup.
  bool has_chunk(int cx, int cz) const override;
  // The other half of that pair: whether a *load* yields a column. It always
  // does here — `load_chunk` fabricates the generated (all-air) column for
  // terrain this world has never stored — so the answer is the constant, and a
  // caller asking it pays neither the fabrication nor the copy that reading the
  // expression out of `load_chunk` would cost (world/world_interface.h).
  bool can_load_chunk(int cx, int cz) const override;

  // Block-backed containers (Phase 3 milestone C). MemoryWorld has no region
  // file to write back to, so the live map is the whole story: a chest keeps
  // its contents for the process lifetime even though its column is
  // regenerated on every load. mark_container_dirty is therefore a no-op.
  std::shared_ptr<BlockContainer> container_at(int x, int y, int z,
                                               const BlockStateKey& state) override;
  void mark_container_dirty(int, int, int) override {}
  std::vector<ItemStack> detach_container_part(int x, int y, int z,
                                               const BlockStateKey& state) override;
  // Every live container (one entry per container, not per part), for the
  // furnace tick.
  std::vector<std::shared_ptr<BlockContainer>> live_containers() const override;

  // Drops every cached (possibly mutated) column. Used by tests and by a
  // future world-import path. This *does* drop mutations: the map is the only
  // copy MemoryWorld keeps (there is no region file to write back to), which is
  // exactly why the eviction policy below does not throw a written column away
  // unless there is nothing else to take.
  void clear() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    chunks_.clear();
    mutated_.clear();
    // No pins can be outstanding here: a lease outlives the call at the
    // caller's peril (it would be pointing into a map that is gone), which is
    // the same rule `clear()` has always had.
    pin_counts_.clear();
    content_version_.clear();
    resident_bytes_ = 0;
  }

  // --- the store's own budget (docs/PHASE5.md §4, "Per-view memory") --------
  // What the store is holding — columns, their estimated bytes, and how many of
  // them carry a write this world cannot regenerate — and what its ceiling is.
  // A budget nobody can read is a budget nobody can test: tests/chunk_cache_test
  // .cpp asserts the count stays bounded while the written column survives, and
  // the fan-out soaks report the occupancy through the metrics line so the
  // plateau's cause is visible from the host rather than only as RSS.
  std::size_t resident_columns() const override;
  std::size_t resident_bytes() const override;
  std::size_t columns_with_writes() const override;
  // Every insertion into the map since construction (world_interface.h): the
  // counter a budget cannot make smaller. `clear()` does not reset it either —
  // it says how much this store has built, not what it is holding.
  uint64_t columns_stored() const override {
    return columns_stored_.load(std::memory_order_relaxed);
  }
  // Releases the regenerable columns when nothing is watching (see the note on
  // WorldInterface::release_idle_columns): the LRU order says which ones were
  // streamed longest ago, and the written set is kept whatever `keep` says.
  std::size_t release_idle_columns(std::size_t keep) override;
  const ChunkBudget& budget() const {
    return budget_;
  }
  // The ceiling itself: 4096 columns, the same budget AnvilWorld's live map
  // uses. At this engine's ~460 KB per expanded column (measured, docs/PHASE5.md
  // §4) that is ~1.9 GB, and it is what a population whose views add up to more
  // than that plateaus at. Public because it is part of what a test asserts
  // against and part of the number a host has to plan for.
  static constexpr std::size_t kChunkBudget = 4096;

  // The light engine's exclusion (world/world_interface.h): the relight borrows
  // nine live columns and writes one of them, so it must hold the same mutex
  // every reader here takes — for the whole borrow-and-write, not per column.
  // Re-entrant, because the relight's own borrows come straight back through
  // `has_chunk`/`modify_chunk` below.
  void lock_relight() override {
    mu_.lock();
  }
  void unlock_relight() override {
    mu_.unlock();
  }

private:
  // The storing paths' two phases.  `stored_chunk` takes the lock, finds (and
  // touches) the stored column or returns null; the caller then builds a fresh
  // column outside the lock — a fill is tens of milliseconds of noise and the
  // lock is every connection's — and `store_or_find_locked` (caller holds the
  // lock) stores it
  // or answers with the column that won the race.
  Chunk* stored_chunk(int cx, int cz);
  // `incoming_bytes` is the fresh column's `Chunk::estimated_bytes`, passed in
  // because the byte ceiling has to be checked against what is *about* to be
  // stored — evicting after the fact would leave the store over it for as long
  // as it took to notice.
  Chunk* store_or_find_locked(int64_t k, Chunk&& fresh, std::size_t incoming_bytes);
  // Whether storing one more column of `incoming_bytes` would pass either
  // ceiling. Both dimensions are opt-in: a zero field is unbounded.
  bool over_budget_locked(std::size_t incoming_bytes) const;
  // Whether the store is over a ceiling *right now*: the warning's episode state
  // (`over_budget_warned_`) clears on this question, not on a borrow ending —
  // see the note in `release_pin`.
  bool over_budget_now_locked() const;
  // The fresh column a storing path stores when there is none: generated
  // terrain when this world has a generator, the air column otherwise.
  Chunk fresh_chunk(int cx, int cz) const;
  // The same fabrication `fresh_chunk` does, for `load_chunk`, which hands out
  // the column without storing it.
  Chunk generate_chunk(int cx, int cz) const;

  // --- the borrow handle (world/world_interface.h's `ChunkLease`) -----------
  // A lease over a stored column: the key is pinned in `pin_counts_` and the
  // lease's release callback unpins it. `borrow_chunk_locked` is the body both
  // the public `modify_chunk` and `set_block_state` go through, so every path
  // that hands out or writes a live `Chunk*` is a path that pinned it first.
  // `stored_chunk` (which returns a raw pointer for the storing paths) is the
  // one deliberate exception and says so in its own note: its caller either
  // takes the pin immediately afterwards or is about to publish a *new* column,
  // which no eviction can reach because the pin is taken with the store lock
  // held and nothing between the two calls can evict.
  std::optional<ChunkLease> borrow_chunk_locked(int64_t k);
  void release_pin(int64_t k);
  static void release_pin_cb(void* ctx, int64_t k);

  // Removes one column, keeping every index that mentions it in step. The two
  // places that erase (`evict_if_needed_locked`, `release_idle_columns`) are
  // this one call, so `resident_bytes_` cannot drift from the map.
  void erase_column_locked(int64_t k);
  // The version of a resident column, minted on first ask. Callers hold `mu_`.
  uint64_t version_locked(int64_t k) const;
  // Re-estimates one resident column (`Chunk::estimated_bytes`) and moves the
  // sum by the difference. Called on the paths that can *grow* a column after it
  // was stored — a block write, and any borrow (the relight's light arrays and a
  // heightmap's lazy refresh both land on a borrowed column) — because the
  // alternative is a budget that counts a column at its birth size forever and
  // under-reports exactly the columns the light engine filled in (the ~100 KB of
  // light arrays a full-height column gains after it is stored).
  //
  // Takes the column the caller's lookup already found, so the estimate does not
  // re-enter `chunks_` for a column the caller is holding.
  void note_column_bytes_locked(int64_t k, const Chunk& column);

  // Registers a live container under every position it occupies (callers hold
  // the lock).
  void register_container_locked(const std::shared_ptr<BlockContainer>& c);
  void unregister_container_locked(const BlockContainer& c);

  // Re-entrant: `lock_relight` above holds it across a relight whose borrows
  // re-enter the same accessors (see its note).
  mutable std::recursive_mutex mu_;
  // Not `mutable`: a const read only reads it (the copy a caller escapes with is
  // `Chunk::snapshot`, chunk.h), so the compiler keeps `get_block_state`, the
  // light and biome reads and `has_chunk` from writing into a stored column. The
  // two height reads are the exception and say so through the borrow below.
  std::unordered_map<int64_t, Chunk> chunks_;
  // Live containers by absolute block position (see container_at).
  std::map<BlockPosKey, std::shared_ptr<BlockContainer>> containers_;
  // LRU eviction for unbounded worlds (same budget as AnvilWorld: kChunkBudget).
  // The stamps live on the columns themselves (`Chunk::lru_stamp`): a block read
  // already looks its column up, and a second node-map entry per read — lookup,
  // insert on the first touch, erase on eviction — was pure overhead on the
  // engine's hottest world call. The clock stays here because it is the store's,
  // not the column's.
  mutable uint64_t lru_clock_ = 0;
  // Every key that has been written through `set_block_state` — the columns
  // this world cannot regenerate. See `evict_if_needed_locked`.
  std::unordered_set<int64_t> mutated_;
  // Outstanding borrows per key. A key with a count is never evicted and is
  // never released by the idle release: someone is holding a `Chunk*` into it.
  // A count (rather than a set) because the same column can be borrowed twice
  // in one call stack — the relight borrows its centre while a ring entry is
  // already holding a neighbour's lease, and a re-entrant path may borrow the
  // same column once per nesting level.
  std::unordered_map<int64_t, int> pin_counts_;
  // What each resident column is counted at (`Chunk::estimated_bytes`), and the
  // sum of the values, so the byte budget can ask "am I over?" without walking
  // the map (the walk would be ~470 KB of columns per insert at scale, which is
  // the cost this store exists to avoid). Per key as well as in total because
  // the sum has to be *moved* when a column grows.
  std::unordered_map<int64_t, std::size_t> chunk_bytes_;
  std::size_t resident_bytes_ = 0;
  // The light-input versions (world/world_interface.h's `chunk_content_version`),
  // per resident column, and the counter they are minted from. Kept beside the
  // bytes rather than in `Chunk` because a column leaves and re-enters the map:
  // an evicted column is dropped here too, and a rebuild mints a *new* number
  // (the counter only counts up), so a relight holding the old one can never
  // mistake the rebuilt column for the one it copied.
  mutable std::unordered_map<int64_t, uint64_t> content_version_;
  mutable uint64_t next_content_version_ = 1;
  // This store's identity (world/world_interface.h's `content_store_token`).
  static uint64_t mint_store_token();
  const uint64_t store_token_ = mint_store_token();
  // Relaxed: this is a report counter, not a synchronisation point, and it is
  // read only after the world has stopped being written.
  mutable std::atomic<uint64_t> generated_columns_{0};
  // Insertions into `chunks_`, i.e. columns this store has stored — cumulative
  // across evictions and the idle release (world_interface.h's `columns_stored`).
  // Relaxed for the same reason as the counter above: it is a report number, not
  // a synchronisation point.
  mutable std::atomic<uint64_t> columns_stored_{0};
  // Whether the over-budget warning has already been logged for this episode.
  // The state it reports (every unpinned resident is written, or everything
  // resident is pinned) persists across inserts, so warning per insert would be
  // a log-flood, and staying silent would hide it.
  bool over_budget_warned_ = false;
  // The ceiling, from the constructor (see `ChunkBudget`).
  ChunkBudget budget_;
  int min_section_y_;
  int section_count_;
  // Null for the void world; set for --worldgen (see the class comment).
  std::shared_ptr<const WgOverworldGenerator> generator_;

  // The borrow for the two height reads, which bring the column's lazy
  // heightmaps current before they answer (chunk.h). Nothing else calls it —
  // every other reader binds the const column the map hands a const method. The
  // cast is sound: the map is the world's own mutable state under `mu_`, the
  // same reason `mu_` and the LRU clock are `mutable`. Callers hold `mu_`.
  Chunk* refreshable_chunk_locked(int64_t k) const {
    auto it = chunks_.find(k);
    return it == chunks_.end() ? nullptr : const_cast<Chunk*>(&it->second);
  }

  // Evicts columns, down to the budget, when another one is stored.
  //
  // A column that has been written to is not regenerable — MemoryWorld has no
  // region file, so the map is the world's only copy of that write (the
  // `clear()` note above says the same) — while a column that has only been
  // *sent* (the relight path's live reference stores one, see the class note on
  // `load_chunk`) is exactly what `fresh_chunk` would build again. A column
  // someone is *holding* (`pin_counts_`) is not a candidate at all: the caller
  // has a `Chunk*` into it. So the victim is the least-recently-served column
  // that is neither written nor pinned, and a written one is taken only when
  // every unpinned resident is written, which the log says out loud. Loops,
  // because a byte budget can need more than one victim for one insert.
  // Defined in the .cpp so this header does not pull in the logger.
  void evict_if_needed_locked(std::size_t incoming_bytes);
  static int64_t key(int cx, int cz) {
    // Pack two int32 coords into an int64 without UB (shifting a negative
    // signed value is UB). Cast through uint32 first, then reinterpret.
    return (static_cast<int64_t>(static_cast<uint32_t>(cx)) << 32) | static_cast<uint32_t>(cz);
  }
};

} // namespace mc::world
