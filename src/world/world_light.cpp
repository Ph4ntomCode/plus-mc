// SPDX-License-Identifier: GPL-3.0-or-later
//
// See world/world_light.h for what this is and where its boundary sits.
#include "world/world_light.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>

namespace mc::world {
namespace {

int words_for(int bits) {
  return std::max(1, (bits + 63) / 64);
}

bool set_bit(std::vector<uint64_t>& words, int bit) {
  const int word = bit / 64;
  if (word < 0 || word >= static_cast<int>(words.size()))
    return false;
  const uint64_t mask = 1ULL << (bit % 64);
  if (words[static_cast<size_t>(word)] & mask)
    return false;
  words[static_cast<size_t>(word)] |= mask;
  return true;
}

bool get_bit(const std::vector<uint64_t>& words, int bit) {
  const int word = bit / 64;
  if (word < 0 || word >= static_cast<int>(words.size()))
    return false;
  return (words[static_cast<size_t>(word)] & (1ULL << (bit % 64))) != 0;
}

// Chunk coordinates as one map key: x in the high 32 bits, z in the low. Signed
// coordinates pack straight through `uint32_t` and the pair is unique.
uint64_t token_key(int cx, int cz) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) | static_cast<uint32_t>(cz);
}

// The light a chunk holds now, per section: the diff's "before" side. The
// copies are `LightArray`s like the originals, so a snapshot of a 17-array column
// is 17 pool slots, not 17 heap blocks that outlive the column they came from.
std::vector<std::optional<LightArray>> snapshot_light(const Chunk& chunk, LightLayer layer) {
  std::vector<std::optional<LightArray>> out(chunk.sections.size());
  for (size_t i = 0; i < chunk.sections.size(); ++i)
    out[i] =
        layer == LightLayer::kSky ? chunk.sections[i].sky_light : chunk.sections[i].block_light;
  return out;
}

// What a recompute moved, in packet terms, against the snapshot taken before it.
//
// A section whose light is **absent** and one whose light is an all-zero array
// are the same thing to everyone downstream: the engine drops an all-zero array
// rather than storing it, the chunk encoder writes both as an empty section, and
// the client paints both as dark. So they compare equal here — a section that was
// absent and is absent again has not moved, which is what makes a relight of an
// unchanged column report nothing at all (and a `light_update` with nothing to
// say send no packet). Comparing them as different was this module's own first
// bug: it made every relight of a chunk that carries no light at all look like it
// had changed every section in the column.
LightChange diff_light(const Chunk& chunk, const std::vector<std::optional<LightArray>>& before_sky,
                       const std::vector<std::optional<LightArray>>& before_block) {
  const LightSectionRange range = chunk_light_section_range(chunk);
  LightChange change;
  change.first_section_y = range.first_section_y;
  change.slot_count = std::min(range.count, 63); // the packet's masks are one uint64 here
  change.sky_changed.assign(static_cast<size_t>(words_for(change.slot_count)), 0);
  change.block_changed.assign(static_cast<size_t>(words_for(change.slot_count)), 0);
  // Absent and all-zero are the same to the client (the comment above), so the
  // comparison is written without materialising a zeros array: doing that would
  // take a slot from the light pool on every relight just to compare against it.
  const auto differs = [](const std::optional<LightArray>& was,
                          const std::optional<LightArray>& now) {
    if (!was && !now)
      return false;
    if (was && now)
      return *was != *now;
    for (const uint8_t b : (was ? *was : *now))
      if (b != 0)
        return true;
    return false;
  };
  const int count = static_cast<int>(chunk.sections.size());
  for (int slot = 0; slot < change.slot_count; ++slot) {
    const int index = range.first_section_y + slot - chunk.min_section_y;
    if (index < 0 || index >= count)
      continue; // the range's two padding sections, which this engine never lights
    const Section& section = chunk.sections[static_cast<size_t>(index)];
    if (differs(before_sky[static_cast<size_t>(index)], section.sky_light))
      set_bit(change.sky_changed, slot);
    if (differs(before_block[static_cast<size_t>(index)], section.block_light))
      set_bit(change.block_changed, slot);
  }
  return change;
}

// The overworld day timeline's `SKY_LIGHT_LEVEL` multiply track
// (`Timelines.OVERWORLD_DAY`): keyframes at 133/11867 ticks of 1.0 and at
// 13670/22330 of 0.26666668, LINEAR easing, period 24000. Returned as the
// *factor* the attribute's base 15 is multiplied by.
float day_sky_light_factor(int64_t world_time) {
  const int64_t period = 24000;
  const int64_t s = ((world_time % period) + period) % period;
  const float night = 0.26666668f;
  const auto lerp = [](float a, float from, float to) { return from + a * (to - from); };
  // The baked segments, in sampler order: the wrap, then the four keyframes.
  if (s < 133) // [22330 - 24000, 133)
    return lerp(static_cast<float>(s + 1670) / 1803.0f, night, 1.0f);
  if (s < 11867)
    return 1.0f;
  if (s < 13670) // the dusk ramp
    return lerp(static_cast<float>(s - 11867) / 1803.0f, 1.0f, night);
  if (s < 22330)
    return night;
  // [22330, 24000): the dawn ramp, back up to the first keyframe.
  return lerp(static_cast<float>(s - 22330) / 1803.0f, night, 1.0f);
}

} // namespace

int sky_darken_for(int64_t world_time, float rain_level, float thunder_level) {
  float level = 15.0f * day_sky_light_factor(world_time);
  // `WeatherAttributes.addLayer`: the rain entry is applied with the *rain*
  // level net of the thunder level, the thunder entry with the thunder level,
  // and each is `FloatModifier.ALPHA_BLEND` (blend the attribute's own 4.0 in
  // by 0.3125 / 0.52734375) followed by a lerp of the result by the level.
  const float rain = rain_level - thunder_level;
  if (rain > 0.0f) {
    const float rain_value = level + 0.3125f * (4.0f - level);
    level = level + rain * (rain_value - level);
  }
  if (thunder_level > 0.0f) {
    const float thunder_value = level + 0.52734375f * (4.0f - level);
    level = level + thunder_level * (thunder_value - level);
  }
  return static_cast<int>(15.0f - level);
}

int WorldInterface::raw_brightness(int x, int y, int z, int sky_darken) const {
  const int cx = x >= 0 ? x / 16 : (x - 15) / 16;
  const int cz = z >= 0 ? z / 16 : (z - 15) / 16;
  const auto chunk = load_chunk(cx, cz);
  if (!chunk)
    return 0;
  return chunk_raw_brightness(*chunk, x - cx * 16, y, z - cz * 16, sky_darken);
}

int WorldInterface::light_kinds_at(int cx, int cz, int x, int y, int z) const {
  // The fallback for a world that cannot read its own palette in place: the
  // same answer as the read, through the read (see the note on the declaration,
  // world/world_interface.h).
  return light_kinds(get_block_state(cx, cz, x, y, z));
}

int WorldInterface::light_level(LightLayer layer, int x, int y, int z) const {
  const int cx = x >= 0 ? x / 16 : (x - 15) / 16;
  const int cz = z >= 0 ? z / 16 : (z - 15) / 16;
  const auto chunk = load_chunk(cx, cz);
  if (!chunk)
    return 0;
  return chunk_light_level(*chunk, x - cx * 16, y, z - cz * 16, layer);
}

LightSectionRange chunk_light_section_range(const Chunk& chunk) {
  // `LevelLightEngine`: `getMinLightSection()` is the world's minimum section
  // minus one and `getLightSectionCount()` is the section count *plus two*, so
  // the light sections are `[min - 1, min + count]` inclusive and a light
  // packet's index `i` names the section at `min - 1 + i`
  // (`ChunkHolder.sectionLightChanged`, which accepts any index in that range).
  //
  // This is deliberately wider than the range a *chunk* packet carries. The
  // captured 26.2 body for the ow3 fixture proves the chunk packet's own light
  // data covers only the sections the level holds a data layer for — that chunk
  // names 12 of its 26 (sky slots 0..11, real from slot 8) — because a section
  // the light engine never touched has no data layer and is in no mask at all.
  // An *update* is not so constrained: it may name any section in the engine's
  // range, which is what relighting a chunk we lit ourselves has to do (every
  // section of it holds light, and the client was sent only the narrow range).
  // Only the index *base* has to agree between the two, and it does.
  LightSectionRange range;
  range.first_section_y = chunk.min_section_y - 1;
  range.count = static_cast<int>(chunk.sections.size()) + 2;
  return range;
}

bool LightChange::any() const {
  for (const uint64_t w : sky_changed)
    if (w != 0)
      return true;
  for (const uint64_t w : block_changed)
    if (w != 0)
      return true;
  return false;
}

bool LightChange::sky_at(int slot) const {
  return get_bit(sky_changed, slot);
}

bool LightChange::block_at(int slot) const {
  return get_bit(block_changed, slot);
}

void copy_chunk_light(const Chunk& from, Chunk& to) {
  const size_t count = std::min(from.sections.size(), to.sections.size());
  for (size_t i = 0; i < count; ++i) {
    to.sections[i].sky_light = from.sections[i].sky_light;
    to.sections[i].block_light = from.sections[i].block_light;
  }
}

// The 3x3 region a chunk is computed over, held as an object so a caller can
// build it *before* taking a live reference to the centre chunk: the world's
// own rule is that a `modify_chunk` reference lasts only until the next world
// operation, and borrowing the neighbours is eight more of them.
//
// The eight neighbours are *borrowed* from the world, not copied out of it.
// What the engine reads through a region entry is the cell's palette index
// (`cell_index_at`), the kinds of that palette entry (`kinds_for`) and the
// column's motion-blocking height (`chunk_motion_blocking_height`, which may
// bring a marked column's lazy heightmaps current) — never a neighbour's light
// arrays: the recompute seeds from the sources it scans, not from what a
// neighbour already holds. All three live in the stored column, so the ring
// points at the stored column. `load_chunk` would hand out `Chunk::snapshot()`
// instead — a ~400 KiB, ~100-allocation copy per neighbour per relight — and,
// because a snapshot refreshes the *copy* and leaves the store's own marks
// where they were, the same marked column would pay its heightmap recompute
// again on every relight that copied it (see `bench_reload`).
//
// The loan is `modify_chunk`, the same live borrow `relight_world_chunk` takes
// for the centre — but here the *lease* is kept, one per neighbour, for as long
// as the ring lives. That is the difference the borrow handle makes: the pointer
// an entry holds used to be protected only by an argument about the LRU ("the
// loan touches recency, and eviction takes the oldest residence"), which holds at
// a 4096-column cap and stops holding the moment the cap can be a small byte
// ceiling. A held lease is a pin (world/world_interface.h's `ChunkLease`), so the
// store cannot take these eight columns while the region is in use, whatever its
// budget says, and the same is true of the centre the caller borrows.
//
// A world that does not hold a column is skipped, exactly as before: the region
// answers air for it (see the note on `WorldInterface::has_chunk`). A world with
// a real generator answers that question true for terrain it has not built yet,
// and that is the point: the eight neighbours' blocks are what this chunk's edge
// light is computed against, and each neighbour is built and stored once — work
// the send path was about to do for it anyway.
class LightRegionRing {
public:
  LightRegionRing(int cx, int cz, WorldInterface& world) {
    region_.centre_x = cx;
    region_.centre_z = cz;
    for (int dz = -1; dz <= 1; ++dz) {
      for (int dx = -1; dx <= 1; ++dx) {
        if (dx == 0 && dz == 0)
          continue;
        // Ask before borrowing: a void world that *fabricates* a column for a
        // chunk it has never seen would otherwise build and *store* nine full
        // columns per relight, and every cell and column height it fabricates
        // is the answer this region already gives for an absent neighbour —
        // air, and the dimension's minimum. A generator world answers true
        // here instead, on purpose: its miss is real terrain, and the ring is
        // the caller that has to build it (see `WorldInterface::has_chunk`).
        if (!world.has_chunk(cx + dx, cz + dz))
          continue;
        auto borrowed = world.modify_chunk(cx + dx, cz + dz);
        if (!borrowed)
          continue;
        region_.set(dx, dz, borrowed->ptr());
        // The pin moves into the ring and lives as long as it does. The
        // pointer above stays valid because it is into the store's own map node,
        // which nothing moves; what the lease adds is that the node stays *in*
        // the map.
        leases_[leases_used_++] = std::move(borrowed);
      }
    }
  }

  void bind_centre(Chunk& centre) {
    region_.set(0, 0, &centre);
  }
  LightRegion& region() {
    return region_;
  }
  // Whether every neighbour this ring borrowed is still pinned — true by
  // construction, and asserted by tests/light_relight_race_test.cpp through the
  // store's own accounting rather than taken on faith.
  std::size_t pinned_neighbours() const {
    std::size_t pinned = 0;
    for (const auto& lease : leases_)
      if (lease && lease->pinned())
        ++pinned;
    return pinned;
  }

private:
  LightRegion region_;
  // At most eight, one per neighbour; `std::array` because the ring is built per
  // relight and an allocation per neighbour is exactly the kind of cost this
  // class already avoids (it borrows instead of copying).
  std::array<std::optional<ChunkLease>, 8> leases_{};
  std::size_t leases_used_ = 0;
};

// The store's exclusion, held for the whole of a borrow-and-write: the ring's
// eight neighbours *and* the centre are live columns of `world`, and every other
// reader of them reaches the store through a method that takes the same mutex
// (see the note on `WorldInterface::lock_relight`, world/world_interface.h).
// Held by the RAII object rather than by hand so no early return can leave it
// taken.
class RelightExclusion {
public:
  explicit RelightExclusion(WorldInterface& world) : world_(world) {
    world_.lock_relight();
  }
  ~RelightExclusion() {
    world_.unlock_relight();
  }
  RelightExclusion(const RelightExclusion&) = delete;
  RelightExclusion& operator=(const RelightExclusion&) = delete;

private:
  WorldInterface& world_;
};

// Materialises the columns the ring is about to borrow *before* the exclusion is
// taken.
//
// The ring borrows live columns and the exclusion is what makes reading them
// safe, so the exclusion has to be held for the whole borrow. What it must not
// be held for is *building* a column: a generator world answers `has_chunk` true
// for terrain it has not built, so the ring's first touch of a fresh region is
// up to nine full fills — and each of those used to run with the store's mutex
// held, serialising every connection in the process behind one player's first
// view. This pass touches each position for the same reason the ring would (and
// under the same condition: a world that does not hold the column is skipped, so
// a void world still fabricates nothing) through `ensure_chunk_stored`, which
// builds and hands nothing out — nothing borrowed here is read, which is what
// leaves the exclusion's contract (no live column used outside it) intact. The
// fill therefore happens outside the exclusion; the ring's own borrow afterwards
// is a hit — a pin, not a build.
//
// A column built here can still be evicted before the ring reaches it (the
// store's budget takes the LRU-oldest column), in which case the ring's borrow
// misses and builds under the exclusion the way it always did. That is the
// fallback, not a hole: `store_or_find_locked` publishes-or-loses, so a fill
// that loses a race is dropped rather than stored twice.
// Whether the warm pass runs. It can be turned off for the A/B arm that asks
// what the pass is worth (`PLUSMC_LIGHT_WARM=off`), the same shape as
// session.cpp's `PLUSMC_WRITE_LOCK` arm: measuring "before" on the same binary
// rather than against a rebuild that differs in a dozen other files. The default
// is on, and nothing but a measurement should turn it off — with it off, a fresh
// generated region's first relight builds its nine columns under the store's
// exclusion again.
bool light_warm_enabled() {
  static const bool kEnabled = [] {
    const char* text = std::getenv("PLUSMC_LIGHT_WARM");
    return text == nullptr || std::strcmp(text, "off") != 0;
  }();
  return kEnabled;
}

void warm_light_region(int cx, int cz, WorldInterface& world, bool include_centre) {
  if (!light_warm_enabled())
    return;
  for (int dz = -1; dz <= 1; ++dz) {
    for (int dx = -1; dx <= 1; ++dx) {
      if (dx == 0 && dz == 0 && !include_centre)
        continue;
      world.ensure_chunk_stored(cx + dx, cz + dz);
    }
  }
}

LightChange compute_light_against(Chunk& chunk, WorldInterface& world) {
  LightChange change;
  if (chunk.sections.empty())
    return change;

  // The caller's chunk may itself be a world column (the relight's own file
  // passes the stored one), and the eight neighbours below always are: take the
  // store's exclusion before borrowing any of them. The neighbours are warmed
  // first, so the borrows under the exclusion are pins rather than fills.
  warm_light_region(chunk.x, chunk.z, world, /*include_centre=*/false);
  RelightExclusion exclusion(world);
  LightRegionRing ring(chunk.x, chunk.z, world);
  ring.bind_centre(chunk);

  LightEnvironment env;
  env.min_y = chunk.min_section_y * 16;
  env.max_y = (chunk.min_section_y + static_cast<int>(chunk.sections.size())) * 16;
  env.region = &ring.region();

  // What the client has for this chunk is what the world holds now, so the diff
  // is against the arrays as they are, before the recompute.
  const std::vector<std::optional<LightArray>> before_sky = snapshot_light(chunk, LightLayer::kSky);
  const std::vector<std::optional<LightArray>> before_block =
      snapshot_light(chunk, LightLayer::kBlock);

  compute_chunk_light(chunk, env);
  return diff_light(chunk, before_sky, before_block);
}

// How many times the two-phase relight re-copies its region before it gives up
// and computes under the exclusion. A retry means a write landed in the 3x3
// region while the light was being computed: the copy is private, so nothing was
// torn and the result is not *wrong*, it is only not provably current. Three is
// enough for the ordinary churn a busy region sees (the exclusion is held for the
// copy and the commit only, and a write is a single cell), and bounded because an
// unbounded retry under a write flood would turn a stall into a livelock. The
// fallback is the original in-lock compute, which needs no version check because
// nothing can move under it.
constexpr int kRelightCommitAttempts = 3;

// What a two-phase relight copies out of the store: the light *inputs* of the
// 3x3 region.
//
// It is deliberately not `Chunk::snapshot`: the engine reads three things from a
// column — the packed block cells, the palette each cell indexes and the
// motion-blocking height — and a snapshot would also copy 17 light arrays per
// column, the biomes and the block entities, none of which the compute touches.
// The light arrays are the expensive half (~100 KiB per full column), which is
// exactly the copy `LightRegionRing` exists to avoid paying under the lock.
// Copying the inputs instead costs a few tens of KiB per column — and most
// sections are uniform, so they copy no buffer at all (`SectionCells`) — and it
// is what makes the compute safe to run with the exclusion released: the copy is
// private, and no other thread can reach it.
struct LightInputs {
  std::array<Chunk, 9> chunks;
  std::array<bool, 9> present{};
  std::array<uint64_t, 9> versions{};
  // What each slot's copy was taken from: the column's coordinates packed next
  // to the version it had at copy time. The scratch outlives one relight, so a
  // slot whose identity is unchanged is *still* a faithful copy of the column —
  // the version says its inputs have not moved — and re-copying it would be
  // paying for bytes that are already here. This is the common case: a write
  // bumps the centre, and the eight neighbours of a chunk being edited under a
  // builder's feet are re-lighting over and over without changing. A slot that
  // misses (a different column, or a moved version) is copied in full; nothing
  // is ever read out of the store outside the exclusion.
  struct CopyTag {
    int64_t key = 0;      // (cx << 32) | cz, sign-extended
    uint64_t version = 0; // the column's version when the copy was taken
    bool valid = false;
  };
  std::array<CopyTag, 9> copied{};
  // Which store those copies came from. The scratch is per thread and a thread
  // can serve more than one world (a test does; a host with two dimensions
  // would), and two stores mint their versions from independent counters — so
  // the same coordinate and the same number in another store says nothing about
  // the bytes. A changed token invalidates every tag, and the slots are copied
  // again from the world that is being relit. Zero means the store has no
  // identity to give, and then nothing is ever reused.
  uint64_t store_token = 0;
};

int64_t column_key(int cx, int cz) {
  return (static_cast<int64_t>(cx) << 32) | static_cast<int64_t>(static_cast<uint32_t>(cz));
}

// Copies the cells, palettes and heightmaps of `from` into `to`, and nothing
// else.
void copy_light_inputs(const Chunk& from, Chunk& to) {
  to.x = from.x;
  to.z = from.z;
  to.min_section_y = from.min_section_y;
  to.sections.resize(from.sections.size());
  for (size_t i = 0; i < from.sections.size(); ++i) {
    const Section& src = from.sections[i];
    Section& dst = to.sections[i];
    dst.y = src.y;
    dst.has_blocks = src.has_blocks;
    dst.block_palette = src.block_palette;
    dst.block_cells = src.block_cells;
  }
  // The heightmap cache and its dirty marks travel with the cells, so
  // `chunk_motion_blocking_height` on the copy answers from the copy's own
  // blocks exactly as the stored column would — the engine's column scan asks
  // for one height per column of the 48x48 region. Only the motion-blocking map
  // is copied: it is the only height the engine reads, and the other three are
  // 6 KB per column that no cell of the compute touches. A marked column's
  // refresh on the copy then recomputes the one map it has, which is the one it
  // was asked for.
  to.heightmaps.clear();
  for (const auto& [type, values] : from.heightmaps) {
    if (type == HeightmapType::MotionBlocking) {
      to.heightmaps.emplace_back(type, values);
      break;
    }
  }
  to.heightmap_dirty_top = from.heightmap_dirty_top;
}

// The two-phase relight's working copy, kept per thread the way the light
// engine keeps its region arena (world/light.cpp). The copy is a few hundred KB
// of cells and palettes, and a fresh one per relight was ~326 allocations and as
// many frees — the mmap/munmap traffic `sample` attributed to the relight. Kept
// here, `copy_light_inputs` resizes in place and assigns into vectors that
// already have the capacity, so a steady-state relight allocates only the one
// heightmap it keeps per column.
//
// It is safe to keep across calls because nothing outside a relight ever sees
// it: the capture fills it, the compute reads it, the commit copies the result
// out. A retry re-fills it from the store; a fallback to the in-lock arm simply
// leaves it as it was.
LightInputs& light_inputs_scratch() {
  static thread_local LightInputs scratch;
  return scratch;
}

// The copy phase, under the exclusion. Answers false when the centre has no
// version — a world that cannot say (`WorldInterface::chunk_content_version`'s
// default), or a column the store does not hold, which the warm pass should have
// built.
bool capture_light_inputs(int cx, int cz, WorldInterface& world, LightInputs& out) {
  // Buffers are reused, so only the *state* is reset (see
  // `light_inputs_scratch`): `copy_light_inputs` overwrites every field of every
  // slot it fills and resizes the section list, and a slot it does not fill is
  // marked absent.
  out.present = {};
  out.versions = {};
  const uint64_t store = world.content_store_token();
  if (store == 0 || store != out.store_token) {
    out.store_token = store;
    for (LightInputs::CopyTag& tag : out.copied)
      tag.valid = false;
  }
  for (int dz = -1; dz <= 1; ++dz) {
    for (int dx = -1; dx <= 1; ++dx) {
      const size_t slot = static_cast<size_t>((dz + 1) * 3 + (dx + 1));
      const int sx = cx + dx;
      const int sz = cz + dz;
      out.versions[slot] = world.chunk_content_version(sx, sz);
      const bool centre = dx == 0 && dz == 0;
      // Zero is "absent" for a neighbour (the ring reads it as air) and "this
      // world cannot say" for the centre, which is the one that decides whether
      // the two-phase path is available at all.
      if (out.versions[slot] == 0) {
        if (centre)
          return false;
        out.present[slot] = false;
        out.copied[slot].valid = false;
        continue;
      }
      // The copy from the previous relight is still this column at this
      // version: the version's whole contract is that its inputs have not
      // moved, so re-copying would only spend bandwidth on bytes already here.
      const int64_t key = column_key(sx, sz);
      const LightInputs::CopyTag& tag = out.copied[slot];
      if (tag.valid && tag.key == key && tag.version == out.versions[slot]) {
        out.present[slot] = true;
        continue;
      }
      auto borrowed = world.modify_chunk(sx, sz);
      if (!borrowed) {
        // The column went between the version read and the borrow. Treat it as
        // absent; the commit check sees the same zero and retries.
        out.versions[slot] = 0;
        out.present[slot] = false;
        out.copied[slot].valid = false;
        continue;
      }
      copy_light_inputs(borrowed->get(), out.chunks[slot]);
      out.present[slot] = true;
      out.copied[slot] = LightInputs::CopyTag{key, out.versions[slot], true};
    }
  }
  return true;
}

// The commit phase's question: is the region still the one the copy was taken
// from? Nine versions, because a write in a neighbour moves that neighbour's
// light without moving the centre's. Callers hold the exclusion, which is what
// makes the reads atomic against every store write.
bool light_inputs_current(int cx, int cz, const LightInputs& in, WorldInterface& world) {
  for (int dz = -1; dz <= 1; ++dz) {
    for (int dx = -1; dx <= 1; ++dx) {
      const size_t slot = static_cast<size_t>((dz + 1) * 3 + (dx + 1));
      if (world.chunk_content_version(cx + dx, cz + dz) != in.versions[slot])
        return false;
    }
  }
  return true;
}

// The original relight: borrow the nine live columns under the exclusion and
// compute against them. It is the fallback for a world that cannot version its
// columns, and the guaranteed-progress arm once the two-phase path has lost its
// race `kRelightCommitAttempts` times. The exclusion makes it safe and holds
// every writer off while it runs, so it needs no version check — at the price of
// the ~7 ms compute being one more thing the store's mutex is held for.
LightChange relight_locked(int cx, int cz, WorldInterface& world) {
  RelightExclusion exclusion(world);
  // The ring is loaded first and the centre borrowed second. That order is no
  // longer load-bearing (the ring *holds* its neighbours' leases, and this one is
  // a lease too, so neither can be evicted while it is in use), but it still
  // saves the store work: a ring that borrowed first and then found the centre
  // absent would have pinned eight columns for a relight that returns nothing.
  LightRegionRing ring(cx, cz, world);
  auto ref = world.modify_chunk(cx, cz);
  if (!ref)
    return {};
  Chunk& chunk = ref->get();
  if (chunk.sections.empty())
    return {};
  chunk.x = cx;
  chunk.z = cz;
  ring.bind_centre(chunk);

  LightEnvironment env;
  env.min_y = chunk.min_section_y * 16;
  env.max_y = (chunk.min_section_y + static_cast<int>(chunk.sections.size())) * 16;
  env.region = &ring.region();

  const std::vector<std::optional<LightArray>> before_sky = snapshot_light(chunk, LightLayer::kSky);
  const std::vector<std::optional<LightArray>> before_block =
      snapshot_light(chunk, LightLayer::kBlock);

  compute_chunk_light(chunk, env);
  return diff_light(chunk, before_sky, before_block);
}

LightChange relight_world_chunk(int cx, int cz, WorldInterface& world) {
  // Build what is not built yet *before* the exclusion: a fresh region's nine
  // columns are nine fills, and a fill is tens of milliseconds (see the note on
  // `warm_light_region`).
  warm_light_region(cx, cz, world, /*include_centre=*/true);

  // The two-phase path (world/world_interface.h's `chunk_content_version`):
  // copy the light inputs under the exclusion, compute with it released, and
  // write the result back only if every column of the region still carries the
  // version it had when the copy was taken. What this buys is the 7 ms compute:
  // it used to be one more thing the store's mutex was held for, so a join's
  // first relight serialised every other connection behind it. What it costs is
  // the copy, which is the light *inputs* of nine columns — not their light.
  for (int attempt = 0; attempt < kRelightCommitAttempts; ++attempt) {
    LightInputs& inputs = light_inputs_scratch();
    {
      RelightExclusion exclusion(world);
      if (!capture_light_inputs(cx, cz, world, inputs))
        break; // a world that cannot version: the in-lock arm below
    }

    Chunk& computed = inputs.chunks[4];
    if (computed.sections.empty())
      return {};
    LightRegion region;
    region.centre_x = cx;
    region.centre_z = cz;
    for (int dz = -1; dz <= 1; ++dz)
      for (int dx = -1; dx <= 1; ++dx) {
        const size_t slot = static_cast<size_t>((dz + 1) * 3 + (dx + 1));
        if (inputs.present[slot])
          region.set(dx, dz, &inputs.chunks[slot]);
      }
    LightEnvironment env;
    env.min_y = computed.min_section_y * 16;
    env.max_y = (computed.min_section_y + static_cast<int>(computed.sections.size())) * 16;
    env.region = &region;

    // Nothing in this call takes the store's mutex: the region is the private
    // copy above, and every world read it makes is a read of that copy.
    compute_chunk_light(computed, env);

    {
      RelightExclusion exclusion(world);
      if (!light_inputs_current(cx, cz, inputs, world))
        continue; // a write landed while we computed: take the region again
      auto ref = world.modify_chunk(cx, cz);
      if (!ref)
        return {};
      Chunk& stored = ref->get();
      if (stored.sections.size() != computed.sections.size())
        continue; // not reachable today (a write never changes the column's
                  // section count), and a retry is the safe answer if it ever is
      // The diff's "before" side is taken *here*, not at copy time: with the
      // versions matching, this is the light the client has, and taking it at
      // the last moment is what keeps it honest if a sibling relight of the same
      // column committed while this one was computing (its light is in the
      // arrays now, so this diff — and the packet the caller builds from it —
      // describes what actually moved). It also keeps a lost attempt from paying
      // for the snapshot at all.
      const std::vector<std::optional<LightArray>> before_sky =
          snapshot_light(stored, LightLayer::kSky);
      const std::vector<std::optional<LightArray>> before_block =
          snapshot_light(stored, LightLayer::kBlock);
      copy_chunk_light(computed, stored);
      return diff_light(computed, before_sky, before_block);
    }
  }

  return relight_locked(cx, cz, world);
}

void LightWorld::note_edit(int cx, int cz, int lx, int y, int lz, int before_kinds) {
  const int x = cx * 16 + lx;
  const int z = cz * 16 + lz;
  std::lock_guard<std::mutex> lock(edits_mu_);
  for (const LightEdit& edit : edits_) {
    if (edit.x == x && edit.y == y && edit.z == z)
      return; // the first before-state is where the replay starts
  }
  edits_.push_back({x, y, z, before_kinds});
}

std::vector<LightEdit> LightWorld::take_edits_in_region(int cx, int cz) {
  const int x0 = (cx - 2) * 16;
  const int z0 = (cz - 2) * 16;
  const int x1 = x0 + 80;
  const int z1 = z0 + 80;
  std::vector<LightEdit> out;
  std::lock_guard<std::mutex> lock(edits_mu_);
  auto keep = edits_.begin();
  for (auto it = edits_.begin(); it != edits_.end(); ++it) {
    if (it->x >= x0 && it->x < x1 && it->z >= z0 && it->z < z1)
      out.push_back(*it);
    else
      *keep++ = *it;
  }
  edits_.erase(keep, edits_.end());
  return out;
}

bool LightWorld::has_edits_in_region(int cx, int cz) {
  const int x0 = (cx - 2) * 16;
  const int z0 = (cz - 2) * 16;
  const int x1 = x0 + 80;
  const int z1 = z0 + 80;
  std::lock_guard<std::mutex> lock(edits_mu_);
  for (const LightEdit& edit : edits_) {
    if (edit.x >= x0 && edit.x < x1 && edit.z >= z0 && edit.z < z1)
      return true;
  }
  return false;
}

bool relight_world_delta(int cx, int cz, LightWorld& recorder, WorldInterface& world,
                         std::vector<ChunkLightChange>& out) {
  out.clear();
  // Warm outside the exclusion: a fresh column is a fill, and a fill under the
  // store's mutex serialises every connection behind it.
  for (int dz = -2; dz <= 2; ++dz)
    for (int dx = -2; dx <= 2; ++dx)
      world.ensure_chunk_stored(cx + dx, cz + dz);

  for (int attempt = 0; attempt < kRelightCommitAttempts; ++attempt) {
    RelightExclusion exclusion(world);
    if (recorder.edits_in_flight() != 0)
      continue;
    std::vector<LightEdit> edits = recorder.take_edits_in_region(cx, cz);
    if (edits.empty())
      return false;

    std::array<std::optional<ChunkLease>, 25> leases{};
    std::array<Chunk*, 25> chunks{};
    for (int dz = -2; dz <= 2; ++dz) {
      for (int dx = -2; dx <= 2; ++dx) {
        if (!world.has_chunk(cx + dx, cz + dz))
          continue;
        auto ref = world.modify_chunk(cx + dx, cz + dz);
        if (!ref)
          continue;
        const size_t slot = static_cast<size_t>((dz + 2) * 5 + (dx + 2));
        chunks[slot] = ref->ptr();
        leases[slot] = std::move(ref);
      }
    }
    if (chunks[12] == nullptr || chunks[12]->sections.empty())
      return false;

    std::array<std::vector<std::optional<LightArray>>, 25> before_sky;
    std::array<std::vector<std::optional<LightArray>>, 25> before_block;
    for (size_t slot = 0; slot < chunks.size(); ++slot) {
      if (chunks[slot] == nullptr)
        continue;
      before_sky[slot] = snapshot_light(*chunks[slot], LightLayer::kSky);
      before_block[slot] = snapshot_light(*chunks[slot], LightLayer::kBlock);
    }

    std::vector<LightDeltaEdit> replay;
    replay.reserve(edits.size());
    for (const LightEdit& edit : edits)
      replay.push_back({edit.x, edit.y, edit.z, edit.before_kinds});
    if (!compute_region_light_delta(chunks, cx, cz, replay))
      return false;

    for (size_t slot = 0; slot < chunks.size(); ++slot) {
      Chunk* chunk = chunks[slot];
      if (chunk == nullptr)
        continue;
      LightChange change = diff_light(*chunk, before_sky[slot], before_block[slot]);
      if (change.any())
        out.push_back({chunk->x, chunk->z, std::move(change)});
    }
    return true;
  }
  return false;
}

void LightWorld::note_write(int cx, int cz, int lx, int lz, bool whole_chunk) {
  // (Bumping the content token is deliberately not here: `note_write` is only
  // reached when the write moved the *light*, and a body carries the blocks
  // too. `set_block_state` bumps for every write it lands.)
  // Vanilla re-lights a 3x3 *light section* region around a change; the same
  // reach in chunks is this one. A neighbour is only marked when the change is
  // within `kMaxLightLevel - 1` cells of it — a cell 15 steps away can only hold
  // 0, so it cannot move — which is what keeps a mid-chunk write at five chunks
  // (itself and the four face neighbours) instead of nine.
  const int reach = kMaxLightLevel - 1;
  std::lock_guard<std::mutex> lock(mu_);
  for (int dz = -1; dz <= 1; ++dz) {
    for (int dx = -1; dx <= 1; ++dx) {
      if (whole_chunk || (dx == 0 && dz == 0)) {
        dirty_.emplace(cx + dx, cz + dz);
        continue;
      }
      const int x_gap = dx > 0 ? 16 - lx : (dx < 0 ? lx + 1 : 0);
      const int z_gap = dz > 0 ? 16 - lz : (dz < 0 ? lz + 1 : 0);
      if (x_gap + z_gap <= reach)
        dirty_.emplace(cx + dx, cz + dz);
    }
  }
}

uint64_t LightWorld::token_for_locked(uint64_t key) {
  const auto it = content_token_.find(key);
  if (it != content_token_.end())
    return it->second; // a real token, or 0 for a reference that escaped
  const uint64_t token = next_token_++;
  store_token_locked(key, token);
  return token;
}

void LightWorld::store_token_locked(uint64_t key, uint64_t token) {
  content_token_[key] = token;
  content_token_order_.push_back(key);
  // The order holds each key at most once: `store_token_locked` is only reached
  // for an absent key or the one withdraw that can reinstate an evicted one, and
  // every other move updates an entry that already has its place.
  while (content_token_order_.size() > kContentTokenEntries) {
    const uint64_t oldest = content_token_order_.front();
    content_token_order_.pop_front();
    content_token_.erase(oldest);
  }
}

uint64_t LightWorld::chunk_content_token(int cx, int cz) {
  std::lock_guard<std::mutex> lock(mu_);
  return token_for_locked(token_key(cx, cz));
}

void LightWorld::bump_content_token(int cx, int cz) {
  std::lock_guard<std::mutex> lock(mu_);
  const uint64_t key = token_key(cx, cz);
  const uint64_t token = next_token_++;
  const auto it = content_token_.find(key);
  if (it == content_token_.end())
    store_token_locked(key, token);
  else
    it->second = token;
}

void LightWorld::withdraw_content_token(int cx, int cz) {
  std::lock_guard<std::mutex> lock(mu_);
  const uint64_t key = token_key(cx, cz);
  const auto it = content_token_.find(key);
  if (it == content_token_.end())
    store_token_locked(key, 0);
  else
    it->second = 0;
}

std::vector<std::pair<int, int>> LightWorld::take_dirty_chunks() {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::pair<int, int>> out(dirty_.begin(), dirty_.end());
  dirty_.clear();
  return out;
}

} // namespace mc::world
