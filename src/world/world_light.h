// SPDX-License-Identifier: GPL-3.0-or-later
//
// The light engine's bridge to a living world (Phase 4 milestone B).
//
// `world/light.h` computes one chunk's light from a caller-supplied
// `LightEnvironment`; this is the caller. It answers the three questions the
// engine has and the server has to answer for it:
//
//   * **where does the world come from?** `compute_light_against` borrows the
//     3x3 columns the engine reads — the centre column and its eight
//     neighbours, which the world lends rather than copies out (see
//     `LightRegionRing` in world/world_light.cpp) — hands the engine a
//     `LightRegion` over them so it never takes a lock per cell, and writes the
//     result into the chunk's own arrays — the same arrays the chunk packet and
//     the region file carry.
//   * **what moved?** `LightChange` is the diff of those arrays against what
//     they were, in *packet* terms: slot `i` is the light section at
//     `chunk_light_section_range(chunk).first_section_y + i`, which is exactly
//     the index the chunk packet's light masks use. So a `light_update` is built
//     from a change with no translation, and a chunk nobody touched produces no
//     packet at all.
//   * **which chunks does a write touch?** `LightWorld` wraps a `WorldInterface`
//     and records every chunk a block write can move light in — the tight
//     version of "light reaches 15 cells", so a write in the middle of a chunk
//     marks five chunks and not nine.
//
// The strategy is the whole-chunk recompute `world/light.h` documents, with an
// incremental fast path: `relight_world_delta` replays a tick's edits over a 5x5
// arena (rebuilding that arena's sky layer when a source moves), and
// `relight_world_chunk` is the fallback for a region with no light yet. Both
// send only the sections that differ; the server drains the set once a tick
// rather than per write.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "world/chunk.h"
#include "world/light.h"
#include "world/world_interface.h"

namespace mc::world {

// `Level.skyDarken`, which is `(int)(15.0f - SKY_LIGHT_LEVEL)` where the
// attribute is the dimension's base 15 multiplied by the overworld day
// timeline's track and then blended toward 4.0 by the weather's own levels
// (world/timeline/Timelines.java's OVERWORLD_DAY track, FloatModifier.MULTIPLY;
// WeatherAttributes.RAIN/THUNDER's ALPHA_BLEND entries, applied last — the layer
// order is `EnvironmentAttributeSystem.addDefaultLayers`).
//
// This is the term `Level.getRawBrightness(pos, 0)` subtracts from a cell's sky
// light, so it is what makes a field under open sky stop growing at night: at
// noon the darken is 0 (the sky reads its full 15), at night it is 11 (the sky
// reads 4, below `CropBlock`'s growth gate of 9).
//
// The track is sampled exactly as `KeyframeTrackSampler` bakes it: linear
// between keyframes, cyclic at 24000 ticks, with the wrap segment running from
// the last keyframe (22330, 0.26666668) at its own ticks minus the period to the
// first one (133, 1.0).
int sky_darken_for(int64_t world_time, float rain_level, float thunder_level);

// (`chunk_has_light` — world/chunk.h — is the "does this chunk carry light yet"
// half of the same milestone, since it is a question about a chunk rather than
// about a world.)
//
// The light sections a chunk packet or a `light_update` indexes into: vanilla's
// `LevelLightEngine` range, `[minSectionY - 1, minSectionY + sectionsCount]`.
// The two packets agree on the base and nothing else — see the definition in
// world_light.cpp for why the *count* here is wider than what the chunk packet
// itself carries.
struct LightSectionRange {
  int first_section_y = 0;
  int count = 0;
};
LightSectionRange chunk_light_section_range(const Chunk& chunk);

// What a recompute moved, in the packet's own indexing (slot 0 is the light
// section at `chunk_light_section_range(chunk).first_section_y`).
struct LightChange {
  int first_section_y = 0;
  int slot_count = 0;
  std::vector<uint64_t> sky_changed;   // bit i: this slot's sky light moved
  std::vector<uint64_t> block_changed; // and its block light

  bool any() const;
  bool sky_at(int slot) const;
  bool block_at(int slot) const;
};

// One block write the delta relight replays.
struct LightEdit {
  int x = 0;
  int y = 0;
  int z = 0;
  int before_kinds = 0;
};

// One chunk whose light a delta relight moved. A delta commits up to 5x5
// chunks, so the caller gets a list rather than one change.
struct ChunkLightChange {
  int cx = 0;
  int cz = 0;
  LightChange change;
};

// Recomputes `chunk`'s light in place, reading `world` for the 3x3 region around
// it: the neighbours are borrowed from the world rather than copied out of it
// (see `LightRegionRing` in world/world_light.cpp), so `world` is the mutable
// world a relight goes through — never the `LightWorld` recorder, which would
// read a borrow as a write and mark the chunk it is about to relight. `chunk`
// must be the chunk at (`chunk.x`, `chunk.z`); only its own arrays are written.
LightChange compute_light_against(Chunk& chunk, WorldInterface& world);

// The same, for a chunk the world owns: the world's chunk is recomputed and left
// holding the new light, and the diff (against what it held) comes back.
//
// This is the two-phase relight, and the reason `chunk_content_version` exists
// (world/world_interface.h). Under the store's exclusion it copies the 3x3
// region's light *inputs* (block cells, palettes, heightmaps — not the light
// arrays), computes against that private copy with the exclusion released, and
// writes the light back only if all nine columns still carry the versions the
// copy was taken with. A write that lands in the region meanwhile makes the
// result stale rather than wrong: the versions no longer match, the region is
// copied again, and after `kRelightCommitAttempts` losses the call falls back to
// computing under the exclusion. A world that answers 0 from
// `chunk_content_version` (the default) always takes that in-lock fallback, so
// this is a pure improvement where the store can support it and no change where
// it cannot.
LightChange relight_world_chunk(int cx, int cz, WorldInterface& world);

// Copies the light arrays of `from` into `to` (one entry per section), for a
// caller that computed on a copy and wants the world to hold the result.
void copy_chunk_light(const Chunk& from, Chunk& to);

// A `WorldInterface` that records the chunks a block write can move light in, so
// the server can relight exactly those. Everything else is forwarded verbatim.
class LightWorld : public WorldInterface {
public:
  explicit LightWorld(std::shared_ptr<WorldInterface> inner) : inner_(std::move(inner)) {}

  // The chunks written to (or reached by a write) since the last drain.
  std::vector<std::pair<int, int>> take_dirty_chunks();
  // The edits since the last drain in the 5x5 around (cx, cz), removed from the
  // log: a block in the ring can light a target's border.
  std::vector<LightEdit> take_edits_in_region(int cx, int cz);
  // Whether there is anything to take. The tick asks this before warming the
  // 5x5: a chunk drained because a *neighbour* was edited has no edits of its
  // own, and paying twenty-five warm borrows to discover that is waste.
  bool has_edits_in_region(int cx, int cz);
  // Writes that have passed the store and not yet logged their edit; the delta
  // checks this under the store's exclusion so the replay cannot miss one.
  int edits_in_flight() const {
    return edits_in_flight_.load(std::memory_order_acquire);
  }
  // The world underneath, which is what the relight itself must go through:
  // `relight_world_chunk` calls `modify_chunk`, and calling it on *this* would
  // mark the chunk it is about to relight as dirty again, once per tick,
  // forever.
  const std::shared_ptr<WorldInterface>& inner() const {
    return inner_;
  }

  // A token for chunk (cx, cz) that stays equal while the column stays equal and
  // moves whenever a write changes it. Zero means "this recorder will not say",
  // which a caller must read as "assume changed".
  //
  // It exists so the server can hand one built chunk body to every player
  // streaming that chunk: the body is a pure function of the column (`load_chunk`
  // hands the encoder a *copy* of it, so the sharing also skips that copy), and
  // the only thing that can make a cached body wrong is a write to the chunk.
  //
  // Every block write in the process funnels through this class — the server,
  // the tick and the commands all call `set_block_state` below — and the bump
  // happens *after* that write lands, so a reader that takes the token before
  // building from the column can never pair a body with a token the write has
  // already moved past.
  //
  // One write this class cannot see: a relight. The light engine is handed the
  // *inner* world (`inner()` above) so that its own reads are not recorded as
  // writes, which means a chunk's light arrays can change behind this class's
  // back. Whoever performs that relight must call `note_relight` below once the
  // light has landed — server/session.cpp's two relight sites do.
  uint64_t chunk_content_token(int cx, int cz) override;

  // A relight's half of the contract above, and the reason it must *move* the
  // token rather than only drop the cached body: a body being built while the
  // relight runs took its token before the light changed, and it stores itself
  // after — so an invalidate can land in between and be undone by that store,
  // leaving the pre-relight light to be served. Moving the token here makes
  // every such store hold a value the next reader no longer sees. Called after
  // the relight's own write, so a build that reads the *new* token also reads
  // the new light.
  void note_relight(int cx, int cz) {
    bump_content_token(cx, cz);
  }

  std::optional<Chunk> load_chunk(int cx, int cz) const override {
    return inner_->load_chunk(cx, cz);
  }
  // The relight's copy identity, through to the store (see the note on
  // `WorldInterface::content_store_token`); a recorder relighting a real world
  // must not look like a store with no identity.
  uint64_t content_store_token() const override {
    return inner_->content_store_token();
  }
  std::optional<ChunkLease> modify_chunk(int cx, int cz) override {
    auto ref = inner_->modify_chunk(cx, cz);
    if (ref) {
      // A live reference is a write this class cannot *time*: the caller may
      // change the column at any point after this returns, so a token handed out
      // now could be older than a body built from the column later, and a cache
      // holding that body would keep serving it. Withdraw the chunk's token
      // instead (`content_token_` entry 0): nothing is cached for it and no
      // cached body is served until a `set_block_state` lands, which is a write
      // this class does see complete. Nothing in the server writes through this
      // reference — the light engine borrows the *inner* world on purpose — so
      // today this only closes the hole for a future caller.
      withdraw_content_token(cx, cz);
      note_write(cx, cz, 0, 0, /*whole_chunk=*/true);
    }
    return ref;
  }
  bool set_block_state(int cx, int cz, int x, int y, int z, const BlockStateKey& key) override {
    // The engine reads exactly two things about a cell: what it costs and what
    // it emits. A write that leaves both where they were cannot move any light,
    // so it does not mark anything — which is what keeps a floor of dirt over a
    // floor of stone (both opaque, neither emitting) free.
    //
    // The cell's own two are asked for as the pair directly (`light_kinds_at`),
    // not as a state to compare afterwards: the read returns two strings by
    // value and this is the one question it would be asked, on every block write
    // (world/world_interface.h).
    const int before = inner_->light_kinds_at(cx, cz, x, y, z);
    ++edits_in_flight_;
    if (!inner_->set_block_state(cx, cz, x, y, z, key)) {
      --edits_in_flight_;
      return false;
    }
    // The chunk's content moved, whatever it was: the body a viewer is sent
    // carries the blocks as well as the light, so a write that leaves the light
    // where it was — a stone floor replaced by a dirt one — still invalidates it.
    // Bumped after the write has landed, which is what makes a token read before
    // a build safe to store with that build (see `chunk_content_token`).
    bump_content_token(cx, cz);
    // The pair comes from one lookup per state (`light_kinds`), not four.
    const int after = light_kinds(key);
    if (before != after) {
      note_edit(cx, cz, x, y, z, before);
      note_write(cx, cz, x, z, /*whole_chunk=*/false);
    }
    --edits_in_flight_;
    return true;
  }

  int min_section_y() const override {
    return inner_->min_section_y();
  }
  int section_count() const override {
    return inner_->section_count();
  }
  BlockStateKey get_block_state(int cx, int cz, int x, int y, int z) const override {
    return inner_->get_block_state(cx, cz, x, y, z);
  }
  int light_kinds_at(int cx, int cz, int x, int y, int z) const override {
    return inner_->light_kinds_at(cx, cz, x, y, z);
  }
  int motion_blocking_height(int x, int z) const override {
    return inner_->motion_blocking_height(x, z);
  }
  int world_surface_height(int x, int z) const override {
    return inner_->world_surface_height(x, z);
  }
  uint64_t random_tick_sections(int cx, int cz) const override {
    return inner_->random_tick_sections(cx, cz);
  }
  bool has_chunk_light(int cx, int cz) const override {
    return inner_->has_chunk_light(cx, cz);
  }
  bool has_chunk(int cx, int cz) const override {
    return inner_->has_chunk(cx, cz);
  }
  bool can_load_chunk(int cx, int cz) const override {
    return inner_->can_load_chunk(cx, cz);
  }
  // Building a column for the generation pool (world/chunk_generation.h) is not
  // a write and must not be recorded as one. The interface's *default* would go
  // through `modify_chunk` above — which withdraws the column's content token
  // and marks it write-dirty for the light pass, once per generated column, for
  // a column nothing wrote to — so the pool's two questions are forwarded to the
  // store verbatim: the store answers them from its own map either way.
  void ensure_chunk_stored(int cx, int cz) override {
    inner_->ensure_chunk_stored(cx, cz);
  }
  bool has_stored_column(int cx, int cz) const override {
    return inner_->has_stored_column(cx, cz);
  }
  // ...and the build counter with them: it is the store's account of what
  // generation cost, which no write can move.
  uint64_t generated_columns() const override {
    return inner_->generated_columns();
  }
  // The store's exclusion is the store's (see the note on
  // WorldInterface::lock_relight): a relight is performed on `inner()` on
  // purpose, so nothing here is exclusive on its own — but a caller that does
  // hand this recorder to `relight_world_chunk` still gets the real mutex taken
  // rather than silence.
  void lock_relight() override {
    inner_->lock_relight();
  }
  void unlock_relight() override {
    inner_->unlock_relight();
  }
  int raw_brightness(int x, int y, int z, int sky_darken) const override {
    return inner_->raw_brightness(x, y, z, sky_darken);
  }
  int light_level(LightLayer layer, int x, int y, int z) const override {
    return inner_->light_level(layer, x, y, z);
  }
  std::string biome_name_at(int x, int y, int z) const override {
    return inner_->biome_name_at(x, y, z);
  }
  std::shared_ptr<BlockContainer> container_at(int x, int y, int z,
                                               const BlockStateKey& state) override {
    return inner_->container_at(x, y, z, state);
  }
  void mark_container_dirty(int x, int y, int z) override {
    inner_->mark_container_dirty(x, y, z);
  }
  std::vector<ItemStack> detach_container_part(int x, int y, int z,
                                               const BlockStateKey& state) override {
    return inner_->detach_container_part(x, y, z, state);
  }
  std::vector<std::shared_ptr<BlockContainer>> live_containers() const override {
    return inner_->live_containers();
  }
  bool save_chunk(int cx, int cz) override {
    return inner_->save_chunk(cx, cz);
  }
  bool save_all() override {
    return inner_->save_all();
  }
  bool flush() override {
    return inner_->flush();
  }

  // The store's own accounting (docs/PHASE5.md §4, "Per-view memory"), through
  // to the store that owns it: this recorder holds no columns, and the interface
  // default answers zero, so without this forwarding the metrics line would
  // report an empty world for every process whose world is wrapped here — which
  // is every process (`main.cpp` wraps the world once, on purpose). The idle
  // release is the same: it is the store's, and a host that asks this recorder
  // to give the view set back must reach the columns.
  std::size_t resident_columns() const override {
    return inner_->resident_columns();
  }
  std::size_t resident_bytes() const override {
    return inner_->resident_bytes();
  }
  std::size_t columns_with_writes() const override {
    return inner_->columns_with_writes();
  }
  uint64_t columns_stored() const override {
    return inner_->columns_stored();
  }
  std::size_t release_idle_columns(std::size_t keep) override {
    return inner_->release_idle_columns(keep);
  }

private:
  void note_write(int cx, int cz, int lx, int lz, bool whole_chunk);
  void note_edit(int cx, int cz, int lx, int y, int lz, int before_kinds);
  // The three ways a chunk's content token moves: `bump` for a write that has
  // landed, `withdraw` for a live reference this class cannot time, and the
  // lazy assignment `chunk_content_token` makes on first ask.
  void bump_content_token(int cx, int cz);
  void withdraw_content_token(int cx, int cz);
  uint64_t token_for_locked(uint64_t key);
  void store_token_locked(uint64_t key, uint64_t token);

  std::shared_ptr<WorldInterface> inner_;
  std::mutex mu_;
  std::set<std::pair<int, int>> dirty_;
  // The delta's edit log and the store-write -> log gap. Separate from `mu_`:
  // a delta takes the store's exclusion and then drains here, while a writer
  // logs after the store write without holding the store lock.
  std::atomic<int> edits_in_flight_{0};
  std::mutex edits_mu_;
  std::vector<LightEdit> edits_;
  // One content token per chunk written (or asked about) since the server
  // started, bounded so a long run's map cannot grow with the world. Evicting an
  // entry is safe: the next ask re-assigns a *higher* token from `next_token_`,
  // so a cached body still holding the old one is a mismatch and is rebuilt. The
  // bound is what makes this cost kb rather than the world's size.
  static constexpr std::size_t kContentTokenEntries = 1024;
  std::unordered_map<uint64_t, uint64_t> content_token_;
  std::deque<uint64_t> content_token_order_;
  uint64_t next_token_ = 1;
};

// The incremental relight for a tick's edits: takes the 5x5 edits, replays them
// over a 5x5 arena (rebuilding the sky layer if a source moved), and commits
// every chunk whose light moved (the centre 3x3 exactly, the halo as the next
// call's boundary). Runs under the store's exclusion; false means the region
// was unlit/missing or the log was empty/busy, and the caller falls back to
// `relight_world_chunk`.
bool relight_world_delta(int cx, int cz, LightWorld& recorder, WorldInterface& world,
                         std::vector<ChunkLightChange>& out);

} // namespace mc::world
