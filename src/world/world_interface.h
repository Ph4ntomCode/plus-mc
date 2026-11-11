// SPDX-License-Identifier: GPL-3.0-or-later
//
// Abstract world interface for chunk loading and mutation.  Both AnvilWorld
// (disk-backed) and MemoryWorld (in-memory for the void/flat mode) implement
// this so SessionSettings can hold either.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "world/chunk.h"
#include "world/container.h"

namespace mc::world {

// `world/light.h`'s LightLayer, forward-declared because that header includes
// this one (through world/anvil.h). The underlying type is spelled the same in
// both places, so the two declarations are the same type.
enum class LightLayer : uint8_t;

// A borrowed column, with the borrow *tracked*.
//
// `modify_chunk` hands out a live reference into a store's own map, and a store
// that evicts under a budget must not take that column out from under the caller
// holding it. `ChunkLease` is how a borrow is announced: the store pins the key
// for as long as the lease lives and its eviction skips pinned entries, so the
// old "valid until the next world operation" contract becomes the one a bounded
// store can actually keep — **valid until the lease dies**. Without this, a
// store's safety rests on the argument that no eviction could land inside a
// borrow's window (true at 4096 columns, false at a small or byte-sized cap);
// with it, the eviction is free to drop whatever the budget says, except what
// someone is holding.
//
// Move-only and allocation-free on purpose: one is handed out per chunk send and
// per relight borrow, so it is a pointer, a function pointer, a context and a
// key — not a `std::function` and not a `shared_ptr`. A world that never evicts
// passes no release at all and the lease is a plain pointer.
//
// `get()`/`ptr()` exist so a call site that used to say `chunk_opt->get()` keeps
// compiling unchanged; new code can use `->`/`*`.
class ChunkLease {
public:
  ChunkLease() = default;
  ChunkLease(Chunk* column, void (*release)(void*, int64_t), void* ctx, int64_t key)
      : column_(column), release_(release), ctx_(ctx), key_(key) {}
  // A borrow of a store that has nothing to pin: a plain pointer. Implicit, but
  // note that `return std::ref(column);` does **not** work through it (that is
  // two user-defined conversions, so it does not compile) — a store adapting to
  // this type writes `return ChunkLease(column);`, which is what AnvilWorld's
  // three return sites do.
  ChunkLease(Chunk& column) : column_(&column) {}
  ChunkLease(ChunkLease&& other) noexcept
      : column_(other.column_), release_(other.release_), ctx_(other.ctx_), key_(other.key_) {
    other.column_ = nullptr;
    other.release_ = nullptr;
  }
  ChunkLease& operator=(ChunkLease&& other) noexcept {
    if (this != &other) {
      release();
      column_ = other.column_;
      release_ = other.release_;
      ctx_ = other.ctx_;
      key_ = other.key_;
      other.column_ = nullptr;
      other.release_ = nullptr;
    }
    return *this;
  }
  ChunkLease(const ChunkLease&) = delete;
  ChunkLease& operator=(const ChunkLease&) = delete;
  ~ChunkLease() {
    release();
  }

  bool has_value() const {
    return column_ != nullptr;
  }
  explicit operator bool() const {
    return column_ != nullptr;
  }
  Chunk* operator->() const {
    return column_;
  }
  Chunk& operator*() const {
    return *column_;
  }
  Chunk& get() const {
    return *column_;
  }
  Chunk* ptr() const {
    return column_;
  }

  // Drops the borrow early — the pin is released and the pointer with it. The
  // destructor is the normal path; this is for a caller that wants to say
  // "done with this column" before it goes out of scope (the send path's copy
  // boundary, a ring that is finished with its neighbours).
  void reset() {
    release();
  }

  // Whether this lease is holding a pin, i.e. whether it will call anything at
  // all when it dies. A world that does not evict answers false.
  bool pinned() const {
    return release_ != nullptr;
  }

private:
  void release() {
    if (release_ != nullptr)
      release_(ctx_, key_);
    column_ = nullptr;
    release_ = nullptr;
  }

  Chunk* column_ = nullptr;
  void (*release_)(void*, int64_t) = nullptr;
  void* ctx_ = nullptr;
  int64_t key_ = 0;
};

class WorldInterface {
public:
  virtual ~WorldInterface() = default;

  // Load chunk at (cx, cz).  Returns nullopt when the chunk does not exist.
  virtual std::optional<Chunk> load_chunk(int cx, int cz) const = 0;

  // Mutable access to a chunk at (cx, cz).  If absent, generates/stores one.
  // The returned lease holds a pin on the column for as long as it lives (see
  // `ChunkLease`), so the reference is valid until the lease dies rather than
  // until the next world operation; callers must still not hold one across
  // yields they own.  New code should prefer the atomic set_block_state below.
  virtual std::optional<ChunkLease> modify_chunk(int cx, int cz) = 0;

  // Atomically sets a single block state in the named chunk.  Returns false
  // when the chunk or section does not exist / is out of range, or the state
  // could not be resolved.  Thread-safe — the world holds its own lock.
  virtual bool set_block_state(int cx, int cz, int x, int y, int z, const BlockStateKey& key) {
    // Default: load-modify under the world's modify_chunk lock.
    // Subclasses that can do better should override to avoid exposing refs.
    auto chunk_opt = modify_chunk(cx, cz);
    if (!chunk_opt)
      return false;
    return chunk_opt->get().set_block_state(x, y, z, key);
  }

  virtual int min_section_y() const = 0;
  virtual int section_count() const = 0;

  // ---- what the store is holding (docs/PHASE5.md §4, "Per-view memory") ----
  //
  // The columns a store currently has resident, the same count as an estimate of
  // their own bytes (`Chunk::estimated_bytes`), and how many of them carry a
  // write the store cannot regenerate. A world with no column cache answers zero
  // for the first two: the numbers are the store's own accounting, not a guess
  // about the process (RSS is `mc::metrics::resident_set_bytes`).
  //
  // They exist because a budget nobody can read is a budget nobody can tune: the
  // metrics line reports them (`world_chunks`/`world_bytes`) and the idle release
  // below is what a host's memory planning turns on.
  virtual std::size_t resident_columns() const {
    return 0;
  }
  virtual std::size_t resident_bytes() const {
    return 0;
  }
  virtual std::size_t columns_with_writes() const {
    return 0;
  }
  // Every column this store has stored since it was constructed — a cumulative
  // count, not a gauge, and deliberately *not* the same question as
  // `resident_columns()`. A store under a column or byte budget evicts, so its
  // resident count during a fill is the columns that survived, not the columns
  // that were built: reading generation throughput off the gauge reports a fill
  // smaller than the one that happened exactly when the budget starts to bite.
  // This counter increments at every insertion into the live map (including a
  // column evicted earlier and rebuilt, which is work that really happened), and
  // is never decremented by eviction, the idle release or `clear()`.
  //
  // Zero for a world with no column cache, like the other store accounting. A
  // consumer on an older server sees no `world_chunks_stored` key rather than a
  // wrong one (`bench/soak.py` falls back to the resident gauge).
  virtual uint64_t columns_stored() const {
    return 0;
  }

  // Drops the columns this store can rebuild on demand, down to `keep` of them,
  // and answers how many went. Called when no player is connected — the one
  // moment a server knows nothing is watching what it has streamed — so a big
  // population's view set stops being a permanent footprint.
  //
  // The contract, in the order it matters:
  //
  //   * a column carrying a *write* is never released: it is the world's state
  //     and (for a world with no region file) the map is the only copy of it. So
  //     `keep` is a floor on the regenerable columns, not a ceiling on the store;
  //   * a *borrowed* column is never released (`ChunkLease`);
  //   * a world with nothing to rebuild (a disk-backed one, where a released
  //     column would have to be read back) answers 0 and does nothing.
  virtual std::size_t release_idle_columns(std::size_t /*keep*/) {
    return 0;
  }

  // Reads a single block state at world coordinates. `x`/`z` are the local
  // 0..15 coordinates inside chunk (cx, cz), `y` is absolute. Returns a
  // synthetic air state when the chunk/section/cell does not exist, so
  // callers never have to special-case ungenerated terrain (drops, redstone,
  // ...).
  //
  // The default implementation goes through load_chunk (which copies the
  // column); worlds that own a cache should override it with a direct read.
  virtual BlockStateKey get_block_state(int cx, int cz, int x, int y, int z) const {
    auto chunk = load_chunk(cx, cz);
    if (!chunk)
      return BlockStateKey{"minecraft:air", ""};
    return chunk->block_state_at(x, y, z);
  }

  // The light engine's question about a cell — what `light_kinds` answers for
  // the state there, packed as `(emission << 4) | dampening`, 0 for a cell in a
  // column this world does not hold (air) — *without* materialising that state.
  //
  // It exists because the read above returns a value: two `std::string`s copied
  // out of the section's palette entry, which allocates once per name or
  // property list past a short-string buffer (a redstone wire's is both). The
  // light engine's write gate asks this one question about the cell it is
  // about to overwrite and needs nothing else from it — whether a write can move
  // light is a property of the two states' kinds, not of the states (world/
  // world_light.h) — so for the caller that asks it, on every block write, the
  // copy is pure overhead. Any caller that compares what it read, or asks about
  // dampening or emission alone, has the same shape.
  //
  // The default asks the read, so a world that cannot answer more cheaply stays
  // correct; a world that owns a column cache should answer from the palette
  // entry in place (both stores do). Defined out of line (world/world_light.cpp)
  // for the same reason `raw_brightness` is: it needs `world/light.h`, which
  // cannot be included here (that header includes `world/anvil.h`, which
  // includes this one).
  virtual int light_kinds_at(int cx, int cz, int x, int y, int z) const;

  // `LevelReader.getHeight(Heightmap.Types.MOTION_BLOCKING, x, z)`: the y one
  // past the highest motion-blocking block of the column (the dimension's min
  // build height for an all-air column, which is what `Heightmap` answers with
  // no entry). This is `Level.canSeeSky`'s and therefore
  // `Level.precipitationAt`'s second question: rain reaches a cell only at or
  // above the column's height. The weather tick asks it per sampled column and
  // fire's `isRainingAt` per neighbour, so worlds that can answer from a loaded
  // column override the default, which copies the whole column.
  virtual int motion_blocking_height(int x, int z) const {
    const int cx = x >= 0 ? x / 16 : (x - 15) / 16;
    const int cz = z >= 0 ? z / 16 : (z - 15) / 16;
    // Non-const: the copy is ours and the read brings a marked column current
    // (the query takes the mutable form on purpose, chunk.h).
    auto chunk = load_chunk(cx, cz);
    if (!chunk)
      return min_section_y() * 16;
    return chunk_motion_blocking_height(*chunk, x - cx * 16, z - cz * 16);
  }

  // `LevelReader.getHeight(Heightmap.Types.WORLD_SURFACE, x, z)`: the y one past
  // the highest non-air block of the column. Natural spawning's random position
  // is drawn up to it (`NaturalSpawner.getRandomPosWithin`), which is the only
  // caller — a plant therefore raises the ceiling where the motion-blocking map
  // does not. The default goes through load_chunk like the other fallbacks.
  virtual int world_surface_height(int x, int z) const {
    const int cx = x >= 0 ? x / 16 : (x - 15) / 16;
    const int cz = z >= 0 ? z / 16 : (z - 15) / 16;
    auto chunk = load_chunk(cx, cz);
    if (!chunk)
      return min_section_y() * 16;
    return chunk_world_surface_height(*chunk, x - cx * 16, z - cz * 16);
  }

  // `Level.getRawBrightness(pos, skyDarken)` = `max(blockLight, skyLight -
  // skyDarken)`, from the chunk's own light arrays. This is the crop's growth
  // gate (`CropBlock.randomTick` asks it per sampled cell), and it is the one
  // question the random-tick pass asks about light, so a world that owns a
  // column cache should override the default, which copies the whole column.
  //
  // A cell outside a loaded column reads 0 on both layers, which is what an
  // unlit, ungenerated world answers: nothing grows there either way.
  //
  // Defined out of line (world/world_light.cpp) because the read it needs is the
  // light engine's, and `world/light.h` cannot be included here: it includes
  // `world/anvil.h`, which includes this header.
  virtual int raw_brightness(int x, int y, int z, int sky_darken) const;

  // `Level.getBrightness(LightLayer, pos)`: one layer's own value at a cell, 0
  // on both layers outside a loaded column. This is the read the natural
  // spawner's light gate asks (world/mob_spawn.h): a monster wants the *sky*
  // layer's own value (not the darkened blend) and the block layer under the
  // dimension's limit, and an animal wants the raw brightness with no darkening
  // at all. Same default and same "override me" note as `raw_brightness`, and
  // also defined out of line for the same reason.
  virtual int light_level(LightLayer layer, int x, int y, int z) const;

  // Does the world *hold* a column at (cx, cz), as opposed to generating one on
  // demand? `load_chunk` is not the same question: a world that generates
  // terrain for a chunk it has never seen answers with a full column either way
  // (the in-memory void world's air column: 24 sections, ~100 KiB, allocated and
  // then dropped by a caller that only wanted to know whether anything is
  // there). The light engine's 3x3 read region asks this once per neighbour —
  // eight times per relight — and reads a column it does not hold as air
  // (world/light.h: a null region entry is air, and an all-air column is air
  // with a min_y height, which is the same answer), so a world that can answer
  // without materialising the column should: the default below loads it.  A
  // world whose miss means *real terrain nobody has built yet* answers true
  // instead — MemoryWorld with an overworld generator does — because the light
  // ring's whole question there is "build this neighbour or read it as air",
  // and the answer must be build.
  virtual bool has_chunk(int cx, int cz) const {
    return load_chunk(cx, cz).has_value();
  }

  // Materialises the column at (cx, cz) if this world would fabricate it, and
  // hands nothing back: no lease, no live pointer, nothing a caller can read.
  //
  // It exists for the light engine's 3x3 ring (world/world_light.cpp), which
  // borrows nine live columns under the store's exclusion and must not pay for
  // the *builds* there. A generator world answers `has_chunk` true for terrain
  // it has not built, so a player's first view is up to nine fills, and a fill
  // is tens of milliseconds a *fill*: the relight holding the store's mutex
  // across them serialised every connection in the process behind one player.
  // Warming the region first moves those fills out of the exclusion, and the
  // ring's own borrows afterwards are pins rather than builds.
  //
  // The default is the honest one for a world with no cache of its own to
  // protect — borrow and drop, a fill at this call's expense — and it applies
  // the ring's own condition (`has_chunk`), so a void world still fabricates
  // nothing. A store that owns its columns overrides it with its own two-phase
  // build (generate outside its mutex, publish under it).
  virtual void ensure_chunk_stored(int cx, int cz) {
    if (has_chunk(cx, cz))
      (void)modify_chunk(cx, cz);
  }

  // How many columns this world has *built* since the process started: every
  // generation, whether the column was stored or lost a publish race to another
  // builder (`MemoryWorld::store_or_find_locked` drops the loser). The store's
  // own gauge counts what it *holds*, which is the right number for memory and
  // the wrong one for generation cost — thirty sessions racing over one spawn
  // square hold a handful of columns between them and build hundreds — so a run
  // that wants to say what generation cost needs this counter beside it.
  // Zero for a world that never generates (a disk-backed one).
  virtual uint64_t generated_columns() const {
    return 0;
  }

  // Whether the column at (cx, cz) is *stored* — resident here and readable
  // without a build. Not `has_chunk`: a generator world answers that true for
  // terrain nobody has built yet (the paragraph above is why), and the question
  // this answers is "has the build already happened?".
  //
  // It exists for the generation pool (world/chunk_generation.h) and the stream
  // path that feeds it: a session asks it before sending a column, because a
  // column that is not stored yet is one the send path would have to build
  // inline — which is the stall the pool exists to move off the session thread.
  //
  // The default is `has_chunk` — the honest answer for a world with no cache of
  // its own — but it is the wrong question for a store whose `has_chunk`
  // *loads* the column to answer (world/anvil.h): a caller asking "do you hold
  // this?" must not pull it in. Both stores that ship today override this with a
  // residency lookup (MemoryWorld's map, AnvilWorld's LRU); the default stays
  // for a host that has nothing but `load_chunk`.
  virtual bool has_stored_column(int cx, int cz) const {
    return has_chunk(cx, cz);
  }

  // Whether a load at (cx, cz) would yield a column — the answer
  // `load_chunk(cx, cz).has_value()` gives — without materialising the column to
  // find out. This is deliberately *not* `has_chunk` above: a world that
  // generates terrain on demand fabricates a column for a position it holds
  // nothing at, so it holds nothing and *still* loads a column, and the two
  // questions have different answers there. (The in-memory world is the one that
  // behaves that way: it answers a load everywhere, so this is constant true for
  // it — where `has_chunk` is false for void terrain it has never been asked to
  // mutate, and true for a generated world's unbuilt terrain, whose miss really
  // is real terrain.)
  //
  // It exists because the read above returns a *value*: one overworld column is
  // ~400 KiB, spread over ~100 allocations, and a caller that wants nothing but
  // the boolean should not pay for the value: the default below loads it. The
  // natural-spawn pass used it for `canSpawnEntitiesInChunk` until 27 Oct; that
  // pass now asks `has_stored_column` instead, because a column no store holds
  // cannot be ticking for entities (docs/PHASE5.md), so this has no caller in
  // the engine today — it stays a world-interface question a host may ask.
  virtual bool can_load_chunk(int cx, int cz) const {
    return load_chunk(cx, cz).has_value();
  }

  // Does chunk (cx, cz) carry light of its own — a light array in any of its
  // sections? A column read from disk answers with whatever the file stored; a
  // column this server created answers false until the light engine computes it
  // (world/world_light.h). The chunk-send path asks this so a chunk is lit once
  // rather than on every send, so a world that owns a column cache should answer
  // from it: the default copies the whole column.
  virtual bool has_chunk_light(int cx, int cz) const {
    const auto chunk = load_chunk(cx, cz);
    return chunk && chunk_has_light(*chunk);
  }

  // A token for chunk (cx, cz) that stays equal while the bytes `load_chunk`
  // would return for it stay equal, and moves whenever a write changes them —
  // `LightWorld`'s per-chunk content tokens (world/world_light.h). Zero means
  // "this world cannot say".
  //
  // It exists so one built chunk body can be shared between every viewer of the
  // same chunk: the body is a pure function of the column, so the only thing
  // that can make a cached one wrong is a write to that chunk. The default
  // answers 0, which makes a caller that keys on it cache nothing at all — the
  // correct answer for a world that does not record its writes. Non-const
  // because recording them is.
  virtual uint64_t chunk_content_token(int /*cx*/, int /*cz*/) {
    return 0;
  }

  // A per-column *light-input* version: a number that stays equal while the
  // column's blocks — and the heightmaps they imply — stay equal, and moves
  // whenever a write changes them. Zero means "this world cannot say", which a
  // caller must read as "do it the old way", never as "unchanged".
  //
  // It is the commit half of the relight's two-phase protocol
  // (world/world_light.cpp):
  //
  //   1. under `lock_relight`, read the nine columns' versions and copy their
  //      light *inputs* (block cells, palettes, heightmaps) into a private
  //      snapshot — no light arrays, no biomes, no block entities;
  //   2. release the exclusion and compute the light against that snapshot;
  //   3. under `lock_relight` again, write the light back only if all nine
  //      versions still match the ones the snapshot was taken with. A mismatch
  //      means a write landed in the region while the compute ran, so the
  //      result is stale: take a fresh snapshot and start again, up to a bounded
  //      number of attempts.
  //
  // Why the version has to be the *blocks* and not the light: the engine seeds
  // from the sources it scans, never from a neighbour's light arrays (world/
  // light.h), so a concurrent *relight* cannot make a computed result wrong —
  // two relights of the same blocks compute the same light — while a block
  // write can, and a write in a neighbour moves that neighbour's light without
  // moving the centre's. Hence nine versions, not one.
  //
  // Absent columns answer 0: the ring reads them as air, and a column that
  // appears where there was none carries a non-zero version, so the commit
  // check catches that too (a stored column never has version 0).
  //
  // The default answers 0, which keeps every world that cannot say on the
  // original in-lock path — correct, just not concurrent.
  virtual uint64_t chunk_content_version(int /*cx*/, int /*cz*/) const {
    return 0;
  }

  // An identity for *this store instance*, for a caller that keeps a copy of
  // some of its columns between calls (`world_light.cpp`'s thread-local light
  // inputs). `chunk_content_version` is monotone only *within* one store: two
  // stores mint from independent counters, so the same number at the same
  // coordinate says nothing about the bytes when the world changed underneath
  // the caller — a thread can serve a test's two worlds, or a host's two
  // dimensions. Zero (the default) means "no identity": the reuse is off and
  // every relight copies, which is what a world that does not implement the
  // version hook gets anyway. A store that cannot be confused with another one
  // mints a number no other instance will ever have — the stores count up from
  // a process-wide atomic.
  virtual uint64_t content_store_token() const {
    return 0;
  }

  // ---- the light engine's exclusion ---------------------------------------
  //
  // Every read and write of a stored column goes through one of the methods
  // above, and both stores take their own mutex for the whole of it — so a
  // caller that holds that mutex is the only one touching the column. The one
  // caller that needs this is the relight: `relight_world_chunk`
  // (world/world_light.cpp) borrows a 3x3 region of *live* columns and writes
  // one of them back, and it does the whole thing without a single world
  // operation of its own — `compute_chunk_light` reads the borrowed columns
  // directly (see the note on `LightRegionRing`), which is what makes a relight
  // cost a fraction of nine column copies. That is only sound if no other thread
  // can copy, read, write or evict one of those columns while it runs: without
  // the exclusion a relight that writes a column's light arrays races every
  // concurrent `load_chunk`/`get_block_state`/`light_level` on that column, and
  // two threads sharing a `std::vector` with no ordering is how the chunk-stream
  // SIGSEGVs (three of them, all on session threads: convo/
  // request_All_chunk_stream_light_crash.json) came to be.
  //
  // So the store is the only thing that can hold its own mutex, and these two
  // are how the relight asks it to. A world with no cache has nothing to
  // serialize and leaves the defaults (no-ops); a store that owns its columns
  // locks its own mutex here. The lock must be **re-entrant**: the relight's own
  // borrows go straight back through the same locking accessors, so a store that
  // implements this with a plain mutex deadlocks on its first neighbour. The
  // pair is used through a guard (world_light.cpp's `RelightExclusion`), so a
  // caller cannot leave it taken.
  virtual void lock_relight() {}
  virtual void unlock_relight() {}

  // `LevelChunkSection.isRandomlyTicking()` for every section of chunk
  // (cx, cz), as a bitmask over that chunk's section *indices* (bit i = the
  // section at `min_section_y() + i`; the overworld's 24 sections fit in the
  // low 64 bits, and nothing above is ever set). The random-tick pass
  // (world/random_tick.h) asks this for every chunk in every player's view on
  // every tick, so an implementation that owns a column cache should answer
  // from it: the default copies the whole column.
  virtual uint64_t random_tick_sections(int cx, int cz) const {
    const int sections = section_count();
    if (sections <= 0)
      return 0;
    const auto chunk = load_chunk(cx, cz);
    if (!chunk)
      return 0;
    uint64_t mask = 0;
    const int count = std::min(sections, 64);
    for (int i = 0; i < count; ++i) {
      if (static_cast<size_t>(i) < chunk->sections.size() &&
          chunk->sections[static_cast<size_t>(i)].random_tick_blocks > 0)
        mask |= (1ull << i);
    }
    return mask;
  }

  // `Level.getBiome(pos).value()`: the biome name at a position, i.e. what the
  // chunk's biome container carries there. Precipitation is a property of that
  // biome (world/biome_table.h), which is what makes rain fall in one place and
  // not another — and what makes a world of `minecraft:the_void` (the default
  // in-memory world) weather-proof by definition.
  virtual std::string biome_name_at(int x, int y, int z) const {
    const int cx = x >= 0 ? x / 16 : (x - 15) / 16;
    const int cz = z >= 0 ? z / 16 : (z - 15) / 16;
    const auto chunk = load_chunk(cx, cz);
    if (!chunk)
      return "minecraft:the_void";
    return chunk_biome_at(*chunk, x - cx * 16, y, z - cz * 16);
  }

  // ---- block-backed containers (Phase 3 milestone C) ----------------------
  //
  // The live contents of a container, shared by every viewer of the position.
  // Created on first access and seeded from the chunk's persisted block-entity
  // records. `state` is the block currently at the position: a chest whose
  // properties say `type=left|right` and whose other half is really there
  // resolves to the *combined* 54-slot container, like vanilla's
  // CompoundContainer. Returns nullptr when the block does not host a
  // container.
  virtual std::shared_ptr<BlockContainer> container_at(int /*x*/, int /*y*/, int /*z*/,
                                                       const BlockStateKey& /*state*/) {
    return nullptr;
  }
  // Marks the container at this position as changed so the next flush writes
  // its items back into the chunk's block-entity list. Cheap (no chunk copy on
  // the click path). A double chest marks both halves' chunks.
  virtual void mark_container_dirty(int /*x*/, int /*y*/, int /*z*/) {}

  // Detaches the part of the container at (x, y, z) that is about to be
  // destroyed, returning the stacks that lived in it so the caller can drop
  // them. A single container yields all of its slots. A double chest yields
  // only the broken half: the surviving half keeps its own items, in a fresh
  // container registered at the surviving position (vanilla drops the removed
  // block entity's contents and re-singles the neighbour). A container that
  // was never opened is read straight from its block entity. Returns empty
  // when `state` does not host a container.
  virtual std::vector<ItemStack> detach_container_part(int /*x*/, int /*y*/, int /*z*/,
                                                       const BlockStateKey& /*state*/) {
    return {};
  }

  // Every live container, as a snapshot the caller owns (the shared_ptrs keep
  // each one alive across the call, so the world lock is not held while a
  // caller walks them). The server's tick loop uses this to advance the
  // furnace family (milestone E); a world with no live containers returns none.
  virtual std::vector<std::shared_ptr<BlockContainer>> live_containers() const {
    return {};
  }

  // Persistence (Milestone E). Default no-ops for worlds that have no backing
  // store (e.g. MemoryWorld). AnvilWorld overrides to flush dirty chunks to
  // region files and level.dat atomically.
  virtual bool save_chunk(int /*cx*/, int /*cz*/) {
    return false;
  }
  virtual bool save_all() {
    return true;
  }
  virtual bool flush() {
    return save_all();
  }
};

} // namespace mc::world
