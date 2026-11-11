// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/memory_world.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_set>
#include <vector>

#include "core/log.h"
#include "world/block_states.h"
#include "world/chunk.h"
#include "world/generated_chunk.h"
#include "world/item.h"
#include "world/world_light.h"
#include "world/worldgen_chunk.h"

namespace mc::world {

namespace {

int floor_div16(int v) {
  return v >= 0 ? v / 16 : (v - 15) / 16;
}

// Generates an all-air Chunk object with the correct section layout for the
// vanilla overworld (min_y=-64, 24 sections).  Biome defaults to plains (id 0)
// and block state defaults to air (id 0).
Chunk make_air_chunk(int cx, int cz, int min_section_y, int section_count) {
  Chunk chunk;
  chunk.x = cx;
  chunk.z = cz;
  chunk.min_section_y = min_section_y;
  chunk.sections.resize(section_count);
  for (int i = 0; i < section_count; ++i) {
    auto& sec = chunk.sections[i];
    sec.y = min_section_y + i;
    sec.has_blocks = true;
    sec.block_palette = {{"minecraft:air", ""}};
    sec.block_cells.assign(4096, 0);
    sec.has_biomes = true;
    // Must match the void_chunk wire body (the_void id 58 in the advertised
    // biome registry), so clients see the same biome for MemoryWorld and
    // void_chunk fallbacks. Flat generation always advertises plain biomes;
    // this is the void column, not a plains column.
    sec.biome_palette = {"minecraft:the_void"};
    sec.biome_cells.assign(64, 0);
  }
  // Zero-heightmaps (all air columns).
  chunk.heightmaps.emplace_back(HeightmapType::MotionBlockingNoLeaves, std::vector<int64_t>(37, 0));
  chunk.heightmaps.emplace_back(HeightmapType::MotionBlocking, std::vector<int64_t>(37, 0));
  chunk.heightmaps.emplace_back(HeightmapType::WorldSurface, std::vector<int64_t>(37, 0));
  return chunk;
}

} // namespace

std::optional<Chunk> MemoryWorld::load_chunk(int cx, int cz) const {
  const int64_t k = key(cx, cz);
  {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto it = chunks_.find(k);
    if (it != chunks_.end()) {
      it->second.lru_stamp = ++lru_clock_;
      // The copy escapes (the wire encoder, the light ring) and carries
      // heightmaps current with its blocks — `Chunk::snapshot` is that boundary
      // everywhere.
      return it->second.snapshot();
    }
  }
  if (generator_ == nullptr)
    return make_air_chunk(cx, cz, min_section_y_, section_count_);
  // A generated column nobody has needed yet: built as a value (outside the
  // lock, unlike the map hit above) and not stored — a load is a read, and the
  // first write to the position, or the send path's relight taking a live
  // reference, is what stores it (see the header note on load_chunk).
  return generate_chunk(cx, cz);
}

std::optional<ChunkLease> MemoryWorld::modify_chunk(int cx, int cz) {
  const int64_t k = key(cx, cz);
  // The stored column first, under the lock the miss path stores with: a borrow
  // that finds an existing column pins it with the lock held, so nothing can
  // evict it between the find and the pin. This is every repeated borrow — which
  // is what the send path's relight does, nine times per relight — so it is the
  // path that must not do anything but find and pin.
  {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (auto lease = borrow_chunk_locked(k))
      return lease;
  }
  // A miss: build the column the world would fabricate *outside* the lock, then
  // publish-or-lose and pin under it — `stored_chunk`/`store_or_find_locked`, the
  // same two phases `set_block_state` uses, for the same reason: a fill is tens
  // of milliseconds of noise and the lock is every connection's.
  //
  // This path used to build under the lock, on the argument that a lease the
  // caller holds must not be able to observe a column that another thread
  // replaced in between. The two-phase shape does not have that hole: the
  // publish and the pin happen under one hold of the lock, so the column handed
  // back cannot be evicted between them, and a fresh build that loses the race
  // is dropped rather than stored twice (`store_or_find_locked`). What the old
  // shape cost was the whole of generation for the caller's *own* miss — and the
  // relight is the caller that misses most: a generator world answers
  // `has_chunk` true for terrain it has not built, so the ring's eight
  // neighbours and its centre are nine builds, each of which held the store's
  // mutex while it ran, serialising every other connection behind them.
  Chunk fresh = fresh_chunk(cx, cz);
  const std::size_t incoming = fresh.estimated_bytes();
  std::lock_guard<std::recursive_mutex> lock(mu_);
  (void)store_or_find_locked(k, std::move(fresh), incoming);
  return borrow_chunk_locked(k);
}

std::optional<ChunkLease> MemoryWorld::borrow_chunk_locked(int64_t k) {
  auto it = chunks_.find(k);
  if (it == chunks_.end())
    return std::nullopt;
  // Served: the LRU tick moves, the byte estimate is brought up to date (a
  // borrowed column is the one the relight is about to add light arrays to), and
  // the key is pinned for as long as the lease lives. Both go through the
  // iterator this lookup already produced: the stamp is the column's own field
  // and the estimate is read from the column it is about, so a borrow costs one
  // hash lookup rather than three.
  it->second.lru_stamp = ++lru_clock_;
  note_column_bytes_locked(k, it->second);
  ++pin_counts_[k];
  return ChunkLease(&it->second, &MemoryWorld::release_pin_cb, this, k);
}

void MemoryWorld::release_pin(int64_t k) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  auto it = pin_counts_.find(k);
  if (it == pin_counts_.end())
    return; // a pin that was never taken: an unpinned lease from a no-op world
  if (--it->second <= 0)
    pin_counts_.erase(it);
  // A borrow ending is the moment the budget can next be enforced — but it is
  // not by itself a *new episode*. The state the flag reports (over a ceiling
  // with nothing left it may take) outlives any one borrow, and the borrows in a
  // flood are temporary: releasing one used to re-arm the warning, so a store
  // held over its ceiling by a persistent borrow — or by a written column a
  // caller keeps — logged a line per insert (`tests/chunk_cache_test.cpp` caught
  // twelve for twelve). Clear it only once the store is genuinely back under its
  // ceilings.
  if (!over_budget_now_locked())
    over_budget_warned_ = false;
}

void MemoryWorld::release_pin_cb(void* ctx, int64_t k) {
  static_cast<MemoryWorld*>(ctx)->release_pin(k);
}

// Whether the store is over a ceiling *right now*, as opposed to
// `over_budget_locked`'s question about one more column (`incoming_bytes`). Only
// the warning's episode state needs this one: it answers "has the condition the
// warning reported actually cleared?".
bool MemoryWorld::over_budget_now_locked() const {
  if (budget_.columns != 0 && chunks_.size() > budget_.columns)
    return true;
  return budget_.bytes != 0 && resident_bytes_ > budget_.bytes;
}

void MemoryWorld::note_column_bytes_locked(int64_t k, const Chunk& column) {
  const std::size_t now = column.estimated_bytes();
  auto& counted = chunk_bytes_[k];
  resident_bytes_ -= counted;
  resident_bytes_ += now;
  counted = now;
}

void MemoryWorld::erase_column_locked(int64_t k) {
  auto it = chunk_bytes_.find(k);
  if (it != chunk_bytes_.end()) {
    resident_bytes_ -= it->second;
    chunk_bytes_.erase(it);
  }
  chunks_.erase(k);
  mutated_.erase(k);
  // The version goes with the column. A rebuild mints a fresh one from
  // `next_content_version_` (which only counts up), so a relight holding the
  // erased column's version can never match the rebuilt column — even though a
  // regenerable column rebuilds to the same blocks, the *relight* cannot know
  // that, and re-copying is cheap next to serving light for a column that is
  // no longer the one it read.
  content_version_.erase(k);
}

uint64_t MemoryWorld::version_locked(int64_t k) const {
  if (chunks_.find(k) == chunks_.end())
    return 0; // no column: the ring reads it as air (world/world_interface.h)
  const auto it = content_version_.find(k);
  if (it != content_version_.end())
    return it->second;
  const uint64_t minted = next_content_version_++;
  content_version_.emplace(k, minted);
  return minted;
}

uint64_t MemoryWorld::mint_store_token() {
  static std::atomic<uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

uint64_t MemoryWorld::chunk_content_version(int cx, int cz) const {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  return version_locked(key(cx, cz));
}

Chunk* MemoryWorld::stored_chunk(int cx, int cz) {
  const int64_t k = key(cx, cz);
  std::lock_guard<std::recursive_mutex> lock(mu_);
  auto it = chunks_.find(k);
  if (it == chunks_.end())
    return nullptr;
  // The newest tick, and the pin that goes with it: the returned pointer is only
  // handed to a caller that will write through it (set_block_state), and a pin
  // taken here under the same lock means the column cannot be evicted between
  // this return and that write — the caller's `release_pin(k)` closes the
  // borrow. Taking it here rather than at the call site is what makes the window
  // empty: the lock is already held, and no eviction can interleave.
  it->second.lru_stamp = ++lru_clock_;
  ++pin_counts_[k];
  return &it->second;
}

Chunk* MemoryWorld::store_or_find_locked(int64_t k, Chunk&& fresh, std::size_t incoming_bytes) {
  auto it = chunks_.find(k);
  if (it != chunks_.end()) {
    it->second.lru_stamp = ++lru_clock_;
    note_column_bytes_locked(k, it->second);
    return &it->second;
  }
  evict_if_needed_locked(incoming_bytes);
  auto [ins, _] = chunks_.emplace(k, std::move(fresh));
  // One more column this store has built, whether or not a budget will make it
  // give the column back later (world_interface.h's `columns_stored`).
  columns_stored_.fetch_add(1, std::memory_order_relaxed);
  ins->second.lru_stamp = ++lru_clock_;
  // The pinned case: the store could not get under its ceiling (a borrow is
  // outstanding), so this insert goes *over* it rather than evicting a column a
  // caller is holding. Counting it is still the honest thing to do — the budget
  // is a target the store returns to, not a promise it breaks a borrow for.
  chunk_bytes_[k] = incoming_bytes;
  resident_bytes_ += incoming_bytes;
  return &ins->second;
}

std::size_t MemoryWorld::resident_columns() const {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  return chunks_.size();
}

std::size_t MemoryWorld::resident_bytes() const {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  return resident_bytes_;
}

std::size_t MemoryWorld::columns_with_writes() const {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  return mutated_.size();
}

// The idle release: give back what this store can rebuild, keep the floor, and
// never touch a written or a borrowed column (world/world_interface.h carries
// the contract; tests/chunk_cache_test.cpp pins it).
//
// It is the point of the borrow handle as much as the budget is. Without them a
// world this server had streamed stayed resident for the process's lifetime, and
// a 40-bot spread-256 session left ~2 GB behind ten seconds after the last
// player disconnected — measured, docs/PHASE5.md §4 — because eviction only runs
// when a column is *stored*, and nothing stores one once the population is gone.
// This is the other trigger: nobody is watching, so the store can go back to a
// floor and the OS can have the pages.
// The idle release: give back the columns this store can rebuild, down to a
// floor, and answer how many went. The victim choice is the eviction's own
// (least-recently-*served* first, never a written column, never a borrowed one),
// so the columns that survive `keep` are the ones something read most recently —
// which is what makes a small floor worth keeping: a player who reconnects
// between two passes finds the terrain it was looking at still built.
//
// `keep` counts the *releasable* columns, not every resident one: a written or
// borrowed column is not this method's to give back, so charging it against the
// floor would mean a world with `keep` writes could never keep a single column —
// the floor would silently stop existing exactly where the store is least able to
// shrink (world/world_interface.h says the same from the caller's side). The
// releasable count is taken once instead of per victim because nothing here
// inserts: the loop can only remove candidates.
std::size_t MemoryWorld::release_idle_columns(std::size_t keep) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  // One candidate walk and one selection, instead of one walk per victim. The
  // old loop rescanned the whole map to find each next-oldest column, which at
  // the 4096-column ceiling is ~16M comparisons under the store's mutex — on
  // the tick thread, at the moment a population has just disconnected and the
  // release is the one thing that has to happen. `nth_element` puts the
  // `releasable - keep` oldest candidates in the prefix in O(n) instead.
  //
  // The columns themselves carry the stamp, so this walks the one map that has
  // to exist (`chunks_`) rather than a second map kept in step.
  std::vector<std::pair<uint64_t, int64_t>> candidates; // (lru_stamp, key)
  candidates.reserve(chunks_.size());
  for (const auto& [k, column] : chunks_) {
    if (mutated_.count(k) != 0)
      continue; // the world's own state, and the map is its only copy
    if (pin_counts_.count(k) != 0)
      continue; // a live borrow is reading it
    candidates.emplace_back(column.lru_stamp, k);
  }
  if (candidates.size() <= keep)
    return 0;
  const std::size_t release_count = candidates.size() - keep;
  std::nth_element(candidates.begin(), candidates.begin() + static_cast<ptrdiff_t>(release_count),
                   candidates.end());
  for (std::size_t i = 0; i < release_count; ++i)
    erase_column_locked(candidates[i].second);
  return release_count;
}

// The budget question, asked once per insert: is the store over a ceiling even
// before `incoming_bytes` lands on it? Both dimensions are opt-in (a zero field
// means "not bounded here"), the count is exact and the byte side is the sum of
// the columns' own estimates, so neither has to walk the map.
auto MemoryWorld::over_budget_locked(std::size_t incoming_bytes) const -> bool {
  if (budget_.columns != 0 && chunks_.size() + 1 > budget_.columns)
    return true;
  return budget_.bytes != 0 && resident_bytes_ + incoming_bytes > budget_.bytes;
}

void MemoryWorld::evict_if_needed_locked(std::size_t incoming_bytes) {
  if (!over_budget_locked(incoming_bytes))
    return;
  // One linear scan over the ticks per victim, and only while the store is at a
  // ceiling: a population whose views never reach it never pays for this at all,
  // and one that does pays it against a fresh column that costs ~100x a scan
  // entry. The loop is for the byte side — dropping a ~100 KB column to make
  // room for a ~500 KB one needs several — and it stops when there is nothing
  // left it is allowed to take (empty, or every candidate pinned).
  while (!chunks_.empty() && over_budget_locked(incoming_bytes)) {
    int64_t regenerable = 0;
    bool have_regenerable = false;
    uint64_t best = UINT64_MAX;
    int64_t written = 0;
    bool have_written = false;
    uint64_t written_best = UINT64_MAX;
    std::size_t pinned = 0;
    for (const auto& [k, column] : chunks_) {
      if (pin_counts_.count(k) != 0) {
        ++pinned;
        continue; // a live Chunk* into this column: not a candidate, ever
      }
      if (column.lru_stamp < written_best) {
        written_best = column.lru_stamp;
        written = k;
        have_written = true;
      }
      if (mutated_.count(k) != 0)
        continue;
      if (column.lru_stamp < best) {
        best = column.lru_stamp;
        regenerable = k;
        have_regenerable = true;
      }
    }
    int64_t victim = regenerable;
    bool have_victim = have_regenerable;
    if (!have_victim && have_written) {
      // Every unpinned resident column carries a write, so *something*
      // irreplaceable has to go or the budget does not hold. The budget is the
      // harder promise (the point of this slice): say what was dropped rather
      // than lose it silently — once per episode, not once per insert.
      victim = written;
      have_victim = true;
      if (!over_budget_warned_)
        log::warn("memory world: all ", chunks_.size(),
                  " resident columns carry a write; dropping the least recently used to "
                  "stay inside the chunk budget (this world has no region file, so its "
                  "writes are lost)");
    }
    if (!have_victim) {
      // Every resident column is pinned by a live borrow. Growing past the
      // budget is the *correct* answer here: the alternative is freeing a column
      // a caller is reading. The metrics line and this warning are how the state
      // is visible.
      if (!over_budget_warned_)
        log::warn("memory world: ", chunks_.size(), " resident columns (", pinned,
                  " pinned by live borrows) are over the chunk budget; growing rather "
                  "than evicting a column a caller holds");
      over_budget_warned_ = true;
      return;
    }
    erase_column_locked(victim);
    if (!have_regenerable)
      over_budget_warned_ = true;
  }
  over_budget_warned_ = false;
}
Chunk MemoryWorld::generate_chunk(int cx, int cz) const {
  // Counted before the build, and counted even if the build is dropped by a
  // publish race a moment later: this is the number generation *cost*, not the
  // number the store kept (world_interface.h's `generated_columns`).
  generated_columns_.fetch_add(1, std::memory_order_relaxed);
  Chunk chunk;
  if (generator_->table().dimension != WgDimensionKind::kOverworld) {
    // The Nether has its own surface rule tree; the End's surface is its
    // default end-stone block. Both then run the same biome feature stage the
    // overworld uses (and, for the End, the 256-high stack its dimension type
    // has), so glowstone/ores/vegetation and the exit platform/spikes exist in
    // the served column.
    fill_generated_dimension_decorated_chunk(*generator_, cx, cz, chunk);
    return chunk;
  }
  // The server's world is vanilla's decorated terrain: the surface rules turn
  // stone columns into the biome's own materials, the carvers cut the caves and
  // canyons out of that surface, and the feature stage applies the biome's own
  // decoration (ores, trees, lakes, vegetation). The noise-, surface- and
  // carved-stage entry points stay for the milestone-K fixtures
  // (world/generated_chunk.h).
  //
  // `PLUSMC_WORLDGEN_STAGE=carved` is the parity arm: the only stage this
  // engine can compare against a vanilla dump is the post-carver one (the
  // decoration stream is this port's own), so `worldgen-wire` asks for it from
  // the same binary rather than the test pinning a second server. Unset (and
  // any other value) is the real, decorated world a player joins.
  static const bool kCarvedOnly = [] {
    const char* stage = std::getenv("PLUSMC_WORLDGEN_STAGE");
    return stage != nullptr && std::strcmp(stage, "carved") == 0;
  }();
  if (kCarvedOnly)
    fill_generated_carved_chunk(*generator_, cx, cz, chunk);
  else
    fill_generated_decorated_chunk(*generator_, cx, cz, chunk);
  return chunk;
}

Chunk MemoryWorld::fresh_chunk(int cx, int cz) const {
  if (generator_ != nullptr)
    return generate_chunk(cx, cz);
  return make_air_chunk(cx, cz, min_section_y_, section_count_);
}

BlockStateKey MemoryWorld::get_block_state(int cx, int cz, int x, int y, int z) const {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  auto it = chunks_.find(key(cx, cz));
  if (it == chunks_.end())
    return BlockStateKey{"minecraft:air", ""}; // generated-but-unmodified = air
  it->second.lru_stamp = ++lru_clock_;
  return it->second.block_state_at(x, y, z);
}

// The same read as `get_block_state` with the value left in the palette: the
// cell is found the same way (`Chunk::cell_index_at`, the cheap half of
// `block_state_at`) and the palette entry's kinds are answered in place, so
// nothing is copied and nothing beyond the entry's own strings is touched.
// The two answers cannot drift: this is the read's state with `light_kinds`
// applied to it, and every branch that answers air below answers the read's
// synthetic air state.
int MemoryWorld::light_kinds_at(int cx, int cz, int x, int y, int z) const {
  static const BlockStateKey kAir{"minecraft:air", ""};
  std::lock_guard<std::recursive_mutex> lock(mu_);
  auto it = chunks_.find(key(cx, cz));
  if (it == chunks_.end())
    return light_kinds(kAir); // generated-but-unmodified = air
  it->second.lru_stamp = ++lru_clock_;
  const Chunk& chunk = it->second;
  const Chunk::CellIndex index = chunk.cell_index_at(x, y, z);
  if (index.section < 0 || index.palette < 0)
    return light_kinds(kAir);
  return light_kinds(chunk.sections[static_cast<size_t>(index.section)]
                         .block_palette[static_cast<size_t>(index.palette)]);
}

// The stored-column question, without the generation `load_chunk` does for a
// chunk it does not have.  In the void mode a column this world has never been
// asked to mutate is generated air, which is what a caller that reads "no
// column" as air already expects; with a generator there is real terrain at
// every position, so the same caller must build the column instead of skipping
// it (world/world_interface.h's `has_chunk` note, world/world_light.cpp's
// light ring).
// The warm pass (world/world_interface.h): the two phases `modify_chunk` uses on
// a miss, minus the lease at the end. Nothing here is handed to a caller, so no
// column is ever read outside the store's mutex — the relight's exclusion still
// covers every *use* of the region, it just no longer pays for the builds.
void MemoryWorld::ensure_chunk_stored(int cx, int cz) {
  const int64_t k = key(cx, cz);
  {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    // The ring's own condition, answered from the map rather than through
    // `has_chunk` (see the header note): a stored column needs no build, and in
    // a void world an absent one is air the ring already reads as air.
    auto it = chunks_.find(k);
    if (it != chunks_.end()) {
      it->second.lru_stamp = ++lru_clock_;
      return;
    }
    if (generator_ == nullptr)
      return;
  }
  Chunk fresh = generate_chunk(cx, cz);
  const std::size_t incoming = fresh.estimated_bytes();
  std::lock_guard<std::recursive_mutex> lock(mu_);
  (void)store_or_find_locked(k, std::move(fresh), incoming);
}

bool MemoryWorld::has_chunk(int cx, int cz) const {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  const int64_t k = key(cx, cz);
  auto it = chunks_.find(k);
  if (it == chunks_.end())
    return generator_ != nullptr;
  it->second.lru_stamp = ++lru_clock_;
  return true;
}

// `load_chunk` fabricates the generated column whenever the map has nothing at
// (cx, cz) — `make_air_chunk` cannot fail — so a load yields a column at every
// position, which is the constant this answers with (world/world_interface.h).
// It is *not* `has_chunk`: that asks whether a column is stored, and for terrain
// this world was never asked to mutate the two answers differ.
bool MemoryWorld::can_load_chunk(int /*cx*/, int /*cz*/) const {
  return true;
}

uint64_t MemoryWorld::random_tick_sections(int cx, int cz) const {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  auto it = chunks_.find(key(cx, cz));
  if (it == chunks_.end())
    return 0; // generated-but-unmodified = air, which never ticks
  it->second.lru_stamp = ++lru_clock_;
  uint64_t mask = 0;
  const int count = std::min(section_count_, 64);
  for (int i = 0; i < count; ++i) {
    if (static_cast<size_t>(i) < it->second.sections.size() &&
        it->second.sections[static_cast<size_t>(i)].random_tick_blocks > 0)
      mask |= (1ull << i);
  }
  return mask;
}

bool MemoryWorld::has_chunk_light(int cx, int cz) const {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  auto it = chunks_.find(key(cx, cz));
  if (it == chunks_.end())
    return false; // generated-but-unmodified = air, which carries no light
  it->second.lru_stamp = ++lru_clock_;
  return chunk_has_light(it->second);
}

int MemoryWorld::raw_brightness(int x, int y, int z, int sky_darken) const {
  const int cx = floor_div16(x);
  const int cz = floor_div16(z);
  std::lock_guard<std::recursive_mutex> lock(mu_);
  auto it = chunks_.find(key(cx, cz));
  if (it == chunks_.end())
    return 0; // generated-but-unmodified = air, which carries no light
  it->second.lru_stamp = ++lru_clock_;
  return chunk_raw_brightness(it->second, x - cx * 16, y, z - cz * 16, sky_darken);
}

int MemoryWorld::light_level(LightLayer layer, int x, int y, int z) const {
  const int cx = floor_div16(x);
  const int cz = floor_div16(z);
  std::lock_guard<std::recursive_mutex> lock(mu_);
  auto it = chunks_.find(key(cx, cz));
  if (it == chunks_.end())
    return 0; // generated-but-unmodified = air, which carries no light
  it->second.lru_stamp = ++lru_clock_;
  return chunk_light_level(it->second, x - cx * 16, y, z - cz * 16, layer);
}

int MemoryWorld::motion_blocking_height(int x, int z) const {
  const int cx = floor_div16(x);
  const int cz = floor_div16(z);
  const int64_t k = key(cx, cz);
  std::lock_guard<std::recursive_mutex> lock(mu_);
  // The refreshable borrow: the read brings a marked column current (chunk.h).
  Chunk* chunk = refreshable_chunk_locked(k);
  if (chunk == nullptr)
    return min_section_y_ * 16; // generated-but-unmodified = all air
  chunk->lru_stamp = ++lru_clock_;
  return chunk_motion_blocking_height(*chunk, x - cx * 16, z - cz * 16);
}

int MemoryWorld::world_surface_height(int x, int z) const {
  const int cx = floor_div16(x);
  const int cz = floor_div16(z);
  const int64_t k = key(cx, cz);
  std::lock_guard<std::recursive_mutex> lock(mu_);
  Chunk* chunk = refreshable_chunk_locked(k); // the same refreshable borrow
  if (chunk == nullptr)
    return min_section_y_ * 16;
  chunk->lru_stamp = ++lru_clock_;
  return chunk_world_surface_height(*chunk, x - cx * 16, z - cz * 16);
}

std::string MemoryWorld::biome_name_at(int x, int y, int z) const {
  const int cx = floor_div16(x);
  const int cz = floor_div16(z);
  {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto it = chunks_.find(key(cx, cz));
    if (it != chunks_.end()) {
      it->second.lru_stamp = ++lru_clock_;
      return chunk_biome_at(it->second, x - cx * 16, y, z - cz * 16);
    }
  }
  // The unstored column's own answer: with a generator that is the biome
  // source's answer for the cell this block falls in (`QuartPos.fromBlock`,
  // then the block origin of that cell), which the climate alone decides —
  // no terrain needed. Without one it is the void column's
  // `minecraft:the_void`.
  //
  // Answered outside `mu_`: the generator is immutable after construction and
  // each thread owns its point-query graph (`worldgen_chunk.cpp`), so this read
  // touches no shared state. Holding the world lock through the climate sample
  // serialised every natural-spawn position query against every other world
  // read, which is the one part of this path a CPU-time probe cannot show.
  return generator_ != nullptr ? generator_->biome_at_quart(x >> 2, y >> 2, z >> 2)
                               : "minecraft:the_void";
}

bool MemoryWorld::set_block_state(int cx, int cz, int x, int y, int z, const BlockStateKey& key) {
  const int64_t k = MemoryWorld::key(cx, cz);
  // Phase one: the stored column, or the fresh one the write lands on (built
  // outside the lock; see the header note on `stored_chunk` for why the
  // pointer stays valid across the re-lock below).  With a generator the fresh
  // column is real terrain, so a write into a region nobody has visited yet
  // lands on the surface instead of at the bottom of an air column.
  Chunk* chunk = stored_chunk(cx, cz);
  std::optional<Chunk> fresh;
  std::size_t incoming = 0;
  if (chunk == nullptr) {
    fresh = fresh_chunk(cx, cz);
    // Read *before* the move below: the two are arguments of the same call, and
    // the order they are evaluated in is the compiler's choice.
    incoming = fresh->estimated_bytes();
  }
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (chunk == nullptr) {
    chunk = store_or_find_locked(k, std::move(*fresh), incoming);
    // Pinned under the same lock that published it, so from the store's point of
    // view this column was never unpinned and evictable.
    ++pin_counts_[k];
  }
  // Either way the pin is held now, and it is what the borrow handle exists for:
  // `stored_chunk`'s pointer used to be safe only because eviction takes the
  // LRU-oldest entry and this column had just been moved to the front — an
  // argument phrased in *inserts per borrow window*, which inverts the moment the
  // budget can also be a small byte ceiling. The pin replaces the argument with a
  // fact: an eviction cannot take this column while the write is in flight.
  // A real block change removes any container that lived there (vanilla drops
  // the block entity with the block); re-typing the same block keeps it.
  const BlockStateKey previous = chunk->block_state_at(x, y, z);
  if (previous.name != key.name) {
    const BlockPosKey bep{cx * 16 + x, y, cz * 16 + z};
    auto cit = containers_.find(bep);
    if (cit != containers_.end())
      unregister_container_locked(*cit->second);
  }
  const bool changed = chunk->set_block_state(x, y, z, key);
  // A write is what makes this column irreplaceable, so the budget's eviction
  // takes a column that was only ever *sent* instead (see
  // `evict_if_needed_locked`). Marked only when the write changed something:
  // re-typing the same state leaves the column exactly as generated.
  if (changed)
    mutated_.insert(k);
  // The light-input version moves *after* the write has landed, and only when it
  // changed something: a relight that copied the column before this bump can
  // never be committed over the new blocks (world/world_interface.h's
  // `chunk_content_version`), while re-typing the same state leaves a copy
  // valid. The write is under the same mutex the relight's exclusion takes, so
  // the bump and the copy cannot interleave.
  if (changed)
    content_version_[k] = next_content_version_++;
  // The column may have grown (a new palette entry, a new section's cells), so
  // the byte budget's view of it is brought up to date before the pin goes.
  note_column_bytes_locked(k, *chunk);
  release_pin(k);
  return changed;
}

void MemoryWorld::register_container_locked(const std::shared_ptr<BlockContainer>& c) {
  for (const BlockPosKey& p : c->parts)
    containers_[p] = c;
}

void MemoryWorld::unregister_container_locked(const BlockContainer& c) {
  for (const BlockPosKey& p : c.parts) {
    auto it = containers_.find(p);
    if (it != containers_.end() && it->second.get() == &c)
      containers_.erase(it);
  }
}

namespace {
// The item list a position's persisted block entity holds (empty when there is
// none). Callers hold the world lock.
std::vector<ItemStack> be_items(const std::unordered_map<int64_t, Chunk>& chunks, int64_t key,
                                int x, int y, int z, int size) {
  std::vector<ItemStack> items(static_cast<size_t>(size));
  auto cit = chunks.find(key);
  if (cit == chunks.end())
    return items;
  for (const nbt::Value& be : cit->second.block_entities) {
    // x/z are absolute here (the caller passes world coordinates); a legacy
    // chunk-local record is accepted too. `y` is absolute.
    if (block_entity_matches(be, x, y, z))
      return read_block_entity_items(be, size);
  }
  return items;
}

// The positions the container at (x, y, z) must cover for `state`.
std::vector<BlockPosKey> container_parts(const std::unordered_map<int64_t, Chunk>& chunks, int x,
                                         int y, int z, const BlockStateKey& state) {
  const BlockPosKey pos{x, y, z};
  std::vector<BlockPosKey> parts{pos};
  if (!is_double_block(state.name) || chest_type_is(state.properties, "single"))
    return parts;
  const auto partner = chest_partner_pos(pos, state.properties);
  if (!partner)
    return parts;
  const int pcx = floor_div16(partner->x);
  const int pcz = floor_div16(partner->z);
  const int64_t pkey =
      (static_cast<int64_t>(static_cast<uint32_t>(pcx)) << 32) | static_cast<uint32_t>(pcz);
  auto cit = chunks.find(pkey);
  if (cit == chunks.end())
    return parts;
  const BlockStateKey other =
      cit->second.block_state_at(partner->x - pcx * 16, partner->y, partner->z - pcz * 16);
  if (other.name != state.name)
    return parts;
  const std::string my_type = chest_type_is(state.properties, "left") ? "left" : "right";
  const std::string want = my_type == "left" ? "right" : "left";
  if (!chest_type_is(other.properties, want))
    return parts;
  const std::string facing = chest_facing(state.properties);
  if (facing.empty() || chest_facing(other.properties) != facing)
    return parts;
  if (my_type == "right")
    parts = {pos, *partner};
  else
    parts = {*partner, pos};
  return parts;
}
} // namespace

std::shared_ptr<BlockContainer> MemoryWorld::container_at(int x, int y, int z,
                                                          const BlockStateKey& state) {
  auto spec = container_spec_for_block(state.name);
  if (!spec)
    return nullptr;
  std::lock_guard<std::recursive_mutex> lock(mu_);
  const BlockPosKey pos{x, y, z};
  const std::vector<BlockPosKey> parts = container_parts(chunks_, x, y, z, state);
  auto it = containers_.find(pos);
  if (it != containers_.end() && it->second->parts == parts)
    return it->second;
  std::shared_ptr<BlockContainer> existing = it != containers_.end() ? it->second : nullptr;
  auto container = std::make_shared<BlockContainer>(std::string(state.name), *spec, parts);
  for (size_t i = 0; i < parts.size(); ++i) {
    std::vector<ItemStack> items;
    const int idx = existing ? existing->part_index(parts[i]) : -1;
    if (idx >= 0) {
      items = existing->part_items(idx);
    } else {
      auto lit = containers_.find(parts[i]);
      if (lit != containers_.end() && lit->second->total_slots() == spec->slots) {
        items = lit->second->inventory.slots();
      } else {
        items = be_items(chunks_, key(floor_div16(parts[i].x), floor_div16(parts[i].z)), parts[i].x,
                         parts[i].y, parts[i].z, spec->slots);
      }
    }
    const int base = static_cast<int>(i) * spec->slots;
    for (int s = 0; s < spec->slots && s < static_cast<int>(items.size()); ++s)
      container->inventory.set(base + s, items[s]);
  }
  if (existing)
    unregister_container_locked(*existing);
  for (const BlockPosKey& p : parts) {
    auto lit = containers_.find(p);
    if (lit != containers_.end() && lit->second != container)
      unregister_container_locked(*lit->second);
  }
  register_container_locked(container);
  return container;
}

std::vector<std::shared_ptr<BlockContainer>> MemoryWorld::live_containers() const {
  std::vector<std::shared_ptr<BlockContainer>> out;
  std::lock_guard<std::recursive_mutex> lock(mu_);
  out.reserve(containers_.size());
  // Hash-set dedup, not a linear scan (see AnvilWorld::live_containers): the
  // tick calls this every tick and a scan is O(containers^2).
  std::unordered_set<const BlockContainer*> seen;
  seen.reserve(containers_.size());
  for (const auto& entry : containers_) {
    const std::shared_ptr<BlockContainer>& c = entry.second;
    if (seen.insert(c.get()).second)
      out.push_back(c);
  }
  return out;
}

std::vector<ItemStack> MemoryWorld::detach_container_part(int x, int y, int z,
                                                          const BlockStateKey& state) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  const BlockPosKey pos{x, y, z};
  auto it = containers_.find(pos);
  if (it == containers_.end()) {
    auto spec = container_spec_for_block(state.name);
    if (!spec)
      return {};
    return be_items(chunks_, key(floor_div16(x), floor_div16(z)), x, y, z, spec->slots);
  }
  auto container = it->second;
  const int idx = container->part_index(pos);
  if (idx < 0)
    return {};
  std::vector<ItemStack> dropped = container->part_items(idx);
  unregister_container_locked(*container);
  if (container->is_double()) {
    const BlockPosKey survivor = container->parts[1 - idx];
    auto single = std::make_shared<BlockContainer>(container->block, container->spec,
                                                   std::vector<BlockPosKey>{survivor});
    const std::vector<ItemStack> kept = container->part_items(1 - idx);
    for (int i = 0; i < single->spec.slots && i < static_cast<int>(kept.size()); ++i)
      single->inventory.set(i, kept[i]);
    register_container_locked(single);
  }
  return dropped;
}

} // namespace mc::world
