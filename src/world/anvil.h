// SPDX-License-Identifier: GPL-3.0-or-later
//
// Anvil region reader/writer for 26.2 worlds (src/world/anvil.h is the loader the
// server uses to serve real chunks; see docs/PHASE2.md milestone A/E).
//
// Layout ground truth (verified against a live 26.2 world and the committed
// fixture, docs/PHASE2.md):
//   world layout: 26.2 stores each dimension under
//     <world>/dimensions/<namespace>/<path>/  with region/ (and poi/,
//     entities/) subfolders; level.dat (gzip NBT) sits at the world root.
//   region file (.mca): 4 KiB header = 1024 x u32 location
//     (sector_offset << 8 | sector_count; 0 = absent) + 1024 x u32 timestamps,
//     then 4 KiB sectors. A chunk record is [u32 length][u8 compression][NBT]
//     where compression is 1 = gzip, 2 = zlib, 3 = none (26.2 writes zlib).
//   chunk NBT: root { DataVersion, xPos/zPos, sections[24], Heightmaps,
//     ... } parsed by world/chunk.h parse_chunk_nbt().
//
// Loads are thread-safe: region files and parsed chunks are cached behind one
// mutex (interim single-machine design; the async phase owns the world store).
// Each load returns a copy of the parsed Chunk so callers never share mutable
// state — milestone A is read-only streaming; place/break (milestone C) makes
// the store the owner of chunk state. Milestone E adds the write path: dirty
// chunks are tracked, serialized via serialize_chunk_nbt(), deflated (zlib),
// and flushed to region files atomically (header patch + sector allocation
// + fsync via temp+rename). level.dat handling is also here.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "world/chunk.h"
#include "world/container.h"
#include "world/world_interface.h"

namespace mc::world {

// Overworld dimension geometry (26.2 ground truth, docs/PHASE2.md).
constexpr int kOverworldMinSectionY = -4;  // min_y -64
constexpr int kOverworldSectionCount = 24; // 384 blocks

// Reads one dimension's region folder. Region files are opened lazily (by
// region coordinate) and parsed chunks are cached; absent files or chunks
// yield std::nullopt from load_chunk, which callers treat as "not generated"
// (the server sends its void fallback for those).
class AnvilWorld : public WorldInterface {
public:
  // Resolves the overworld region folder of a world directory: prefers the
  // modern 26.2 layout <world>/dimensions/minecraft/overworld/region, and
  // falls back to <world>/region for legacy flat-layout worlds. If the
  // world_dir does not exist, it is created (mkdir -p) so --world can be
  // used to bootstrap a fresh world. Returns nullptr only on I/O errors
  // that prevent directory creation. A missing region folder is fine — the
  // world may simply have no generated chunks yet.
  static std::shared_ptr<AnvilWorld> open_world(const std::filesystem::path& world_dir,
                                                int min_section_y = kOverworldMinSectionY,
                                                int section_count = kOverworldSectionCount);

  // Opens an explicit region folder (fixture tests point at
  // tests/fixtures/ directly). Returns nullptr when `region_dir` is missing
  // (strict; does not create).
  static std::shared_ptr<AnvilWorld> open_region_dir(const std::filesystem::path& region_dir,
                                                     int min_section_y = kOverworldMinSectionY,
                                                     int section_count = kOverworldSectionCount);

  // Loads chunk (cx, cz), parsing it from its region file on a miss. Returns
  // nullopt when the chunk does not exist on disk or its NBT is malformed.
  // Thread-safe; cached state lives behind the shared Impl so const worlds
  // (shared across connections) still load.
  std::optional<Chunk> load_chunk(int cx, int cz) const override;

  // Direct single-block read from the cache/dirty store (no chunk copy). Hot
  // on the block-break path: the default WorldInterface implementation would
  // copy the whole column just to inspect one cell.
  BlockStateKey get_block_state(int cx, int cz, int x, int y, int z) const override;
  // The light kinds of a cell, from the cached column's palette entry, without
  // the copy `get_block_state` makes of it (world/world_interface.h says why
  // this exists). Same lookup, so a cell with no column behind it answers air's
  // kinds exactly as the read answers the synthetic air state.
  int light_kinds_at(int cx, int cz, int x, int y, int z) const override;

  // The same direct read for weather's two level questions — the column's
  // motion-blocking height and the biome at a position — both of which the
  // weather tick and fire's rain branches ask repeatedly.
  int motion_blocking_height(int x, int z) const override;
  int world_surface_height(int x, int z) const override;
  std::string biome_name_at(int x, int y, int z) const override;

  // The random-tick pass's section gate, from the cached column rather than a
  // copy of it (see the note on WorldInterface::random_tick_sections).
  uint64_t random_tick_sections(int cx, int cz) const override;
  // The chunk-send path's light question, from the cached column: a chunk read
  // off disk carries whatever light the file stored, and one only this server
  // has touched carries light once the light engine has computed it.
  bool has_chunk_light(int cx, int cz) const override;
  // Whether the world holds a column, answered from the cache/region file
  // without copying it out (see the note on WorldInterface::has_chunk).
  bool has_chunk(int cx, int cz) const override;
  // Whether the world *holds* a column, in residency terms: an LRU residence (or
  // the shared dirty object) and nothing else. `has_chunk` above is a different
  // question — it loads a column it does not have, so it answers for the region
  // file — and a caller asking "do you hold this?" must not pay a disk read to
  // find out. The natural spawn pass asks once per candidate chunk of an
  // 8-chunk square (docs/PHASE5.md's disk-world A/B), where the lazy answer
  // pulled the whole square in; `can_load_chunk` is the question for "would a
  // load yield one?". A column on disk but not resident answers false.
  // `PLUSMC_ANVIL_STORED_RESIDENT=off` is the same binary's old answer (the
  // interface default, `has_chunk`) for the A/B.
  bool has_stored_column(int cx, int cz) const override;
  // And whether a *load* yields one: a column present on disk or in the cache
  // answers yes and so does this, but the column is left where it is instead of
  // being copied out for a caller that only wants the boolean (see the note on
  // WorldInterface::can_load_chunk).
  bool can_load_chunk(int cx, int cz) const override;
  // And the same for the random-tick pass's light gate, which asks per sampled
  // crop cell (see the note on WorldInterface::raw_brightness).
  int raw_brightness(int x, int y, int z, int sky_darken) const override;
  // One layer's own value, for the natural spawner's light gate (see the note on
  // WorldInterface::light_level): the same cached column, read per layer.
  int light_level(LightLayer layer, int x, int y, int z) const override;

  // The light engine's exclusion (world/world_interface.h): the relight borrows
  // nine live cached columns and writes one of them, so it must hold the same
  // mutex every reader here takes — for the whole borrow-and-write. Re-entrant,
  // because the relight's own borrows (`has_chunk`/`modify_chunk`, which may load
  // a column off disk) come straight back through the same mutex. Defined out of
  // line because `Impl` is incomplete here.
  void lock_relight() override;
  void unlock_relight() override;

  // The light-input version (world/world_interface.h): so a relight of an
  // on-disk world gets the same two-phase commit the in-memory store does —
  // copy the 3x3 inputs under the exclusion, compute with it released, commit
  // only if every column still carries the version it was copied with. A
  // resident column answers its version (minted on first ask); a column that is
  // not resident answers 0, as does one whose residence is a cached miss, so the
  // relight warms it (through the interface's default `ensure_chunk_stored`)
  // and asks again. An evicted column drops its version, so a re-read mints a
  // new one even though the blocks are the same — a retry is cheap next to
  // serving light for a column the relight can no longer prove it copied.
  uint64_t chunk_content_version(int cx, int cz) const override;
  // This store's identity, for the relight's cross-call copy reuse
  // (world/world_interface.h's `content_store_token`). Unique per instance for
  // the process' lifetime, so a copy taken from a world that has since closed
  // can never be mistaken for a copy of this one.
  uint64_t content_store_token() const override;

  // Mutable access to a loaded chunk (for block mutation).  Returns nullopt
  // when the chunk is absent.  The returned reference is valid until the
  // next load/modify on any thread (callers must serialize mutation +
  // re-encode). Prefer set_block_state for cross-thread safety.
  //
  // The lease this hands back carries no pin, which is the one place the two
  // stores differ (see `ChunkLease`, world/world_interface.h). It does not need
  // one: this world's cache is backed by region files and its eviction rule
  // already prefers a clean victim, so a column it drops is a *reread*, and the
  // callers that hold a reference across other world operations are the relight
  // and the spawn search, both of which take `lock_relight`'s mutex for the
  // whole borrow. MemoryWorld is the store with no file behind it, and the pin
  // is what makes a budget safe there.
  std::optional<ChunkLease> modify_chunk(int cx, int cz) override;
  bool set_block_state(int cx, int cz, int x, int y, int z, const BlockStateKey& key) override;

  // Block-backed containers (Phase 3 milestone C): the live container at a
  // world position, seeded from the chunk's block-entity list on first access.
  // A chest state with `type=left|right` resolves to the combined 54-slot
  // container over both halves. mark_container_dirty queues the owning chunk
  // (both, for a double chest) for the next flush.
  std::shared_ptr<BlockContainer> container_at(int x, int y, int z,
                                               const BlockStateKey& state) override;
  void mark_container_dirty(int x, int y, int z) override;
  std::vector<ItemStack> detach_container_part(int x, int y, int z,
                                               const BlockStateKey& state) override;
  // Every live container (one entry per container, not per part), for the
  // furnace tick.
  std::vector<std::shared_ptr<BlockContainer>> live_containers() const override;

  // Reads the world's spawn point from <world>/level.dat (gzip NBT root
  // SpawnX/SpawnY/SpawnZ) when this world was opened via open_world and the
  // file exists; returns nullopt otherwise.
  std::optional<std::tuple<double, double, double>> world_spawn() const;

  // Reads the seed the world was generated from out of the same level.dat
  // (`Data.WorldGenSettings.seed`, or the legacy `Data.Seed` / `Data.RandomSeed`).
  // This is the number Join Game's `hashedSeed` is `obfuscate_seed()` of, and the
  // reason a `--world` server reads it at all: without it a client's biome
  // manager samples its unloaded-chunk fallback from the wrong seed. Nullopt
  // when the file is absent or carries no seed (e.g. one this server wrote).
  std::optional<int64_t> level_seed() const;

  // The weather fields level.dat carries (`rainTime`, `thunderTime`, `raining`,
  // `thundering`), plus +MC's own additive `rainLevel` / `thunderLevel` (vanilla
  // ignores unknown tags), so a restart resumes a fade mid-ramp instead of
  // snapping to full strength. A file written before those two existed answers
  // 0 and the restore falls back to the flags (a storm -> 1.0).
  struct LevelWeather {
    int rain_time = 0;
    int thunder_time = 0;
    bool raining = false;
    bool thundering = false;
    float rain_level = 0.0f;
    float thunder_level = 0.0f;
  };
  // Reads those fields from <world>/level.dat; nullopt when the file is absent
  // or does not parse. A file without them answers the defaults (clear).
  std::optional<LevelWeather> level_weather() const;
  // Merges the four fields into the existing level.dat and replaces the file
  // atomically, preserving every other field (seed, spawn, generator settings,
  // a tool's own additions). Returns false when the file is absent or cannot be
  // written; an absent file is not created here.
  bool save_level_weather(const LevelWeather& weather);

  // Persistence (Milestone E): flush dirty chunks to region files. Thread-safe
  // via the Impl mutex. save_chunk flushes one column; save_all flushes every
  // dirty column and level.dat. Both perform an atomic temp+rename per region
  // file so a crash never leaves a torn header. Returns false on any I/O
  // error (the dirty set is retained so a retry can succeed).
  bool save_chunk(int cx, int cz) override;
  bool save_all() override;
  bool flush() override {
    return save_all();
  }
  // Ensures level.dat exists (creating a minimal one when missing). Returns
  // false only on I/O error. Called on world creation; idempotent. When a seed
  // is given, the minimal file also carries `WorldGenSettings.seed`, so the
  // world's own generator seed is on disk and `level_seed()` reads it back
  // (an existing level.dat is never rewritten).
  bool ensure_level_dat(int data_version = 5023, std::optional<int64_t> seed = std::nullopt);

  bool valid() const {
    return valid_;
  }
  const std::filesystem::path& region_dir() const {
    return region_dir_;
  }
  const std::filesystem::path& world_root() const;
  int min_section_y() const override {
    return min_section_y_;
  }
  int section_count() const override {
    return section_count_;
  }

  // Parses one region record payload (raw NBT bytes) into a Chunk. Exposed
  // for tests. Returns nullopt on any shape error.
  static std::optional<Chunk> parse_chunk_record(const uint8_t* nbt, size_t len, int min_section_y,
                                                 int section_count);

private:
  AnvilWorld(std::filesystem::path region_dir, int min_section_y, int section_count);
  struct Impl;
  // The positions the container at (x, y, z) must cover for `state`: itself, or
  // both halves of a chest whose partner is really there (callers hold the
  // world lock).
  std::vector<BlockPosKey> container_parts_locked(int x, int y, int z, const BlockStateKey& state);
  // The cached column at (cx, cz) with its LRU position refreshed, or nullptr
  // when the column is absent on disk (a cached miss) or unreadable. Mirrors
  // `get_block_state`'s lookup order: a dirty (mutated, not yet saved) column is
  // authoritative. A *read-only* borrow: only the two height reads need a column
  // they may refresh, and they go through `refreshable_chunk_locked` below.
  // Callers hold `impl_->mu`.
  const Chunk* cached_chunk_locked(int cx, int cz) const;
  // The same borrow for the two height reads, which owe the column's lazy
  // heightmaps a refresh before they answer (chunk.h). Nothing else calls it, so
  // the compiler keeps every other reader from writing into a cached column.
  // Callers hold `impl_->mu`.
  Chunk* refreshable_chunk_locked(int cx, int cz) const;
  std::shared_ptr<Impl> impl_;
  bool valid_ = false;
  std::filesystem::path region_dir_;
  int min_section_y_ = 0;
  int section_count_ = 0;
};

} // namespace mc::world
