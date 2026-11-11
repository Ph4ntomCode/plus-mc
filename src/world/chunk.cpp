// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/chunk.h"

#include "world/collision_table.h"
#include "world/occlusion_table.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "net/byte_buffer.h"
#include "world/random_tick.h"

namespace mc::world {

namespace {

// ---------------------------------------------------------------------------
// Disk container expansion (LSB-first packed longs, y-major cell order).
// ---------------------------------------------------------------------------

// The disk (and wire) container layout, 26.2's legacy PackedIntegerArray:
// values are packed per-long with no straddling (values_per_long =
// floor(64/bits), the high bits of each long are unused when 64 % bits != 0).
// This wastes ~4 bits/long at 5 bits and ~4 bits/long at 6 bits but matches
// every captured region file (Y=1 pal 31 -> 342 longs, Y=4 pal 49 -> 410 longs).
// Heightmaps also show this (9 bits x 256 = 37 longs, not 36).

// ---------------------------------------------------------------------------
// Wire container encoding (vanilla Strategy/Configuration bit widths).
// ---------------------------------------------------------------------------

int bits_needed(size_t size) {
  int bits = 0;
  while ((size_t{1} << bits) < size)
    ++bits;
  return bits;
}

int block_bits_for_palette_size(size_t size) {
  if (size <= 1)
    return 0;
  if (size <= 16)
    return 4;
  const int b = bits_needed(size);
  return b <= 15 ? b : -1;
}

int biome_bits_for_palette_size(size_t size) {
  if (size <= 1)
    return 0;
  if (size == 2)
    return 1;
  if (size <= 4)
    return 2;
  if (size <= 8)
    return 3;
  const int b = bits_needed(size);
  return b <= 15 ? b : -1;
}

size_t long_count_for(int cells, int bits) {
  if (bits == 0)
    return 0;
  const size_t per_long = static_cast<size_t>(64 / bits);
  return (static_cast<size_t>(cells) + per_long - 1) / per_long;
}

constexpr int kBlockGlobalBits = 15;

// The caller may hand a `SectionCells` (see the save path) or a plain
// `std::vector<int32_t>`, and this only reads through `size()`/`operator[]`.
template <typename Cells>
bool pack_cells(const Cells& values, int bits, std::vector<int64_t>* out,
                size_t limit = static_cast<size_t>(-1)) {
  // One buffer, one pass: the packed words are built directly in `out` (the
  // previous form built a second vector and copied it word by word), and the
  // per-cell range check happens where the cell is already loaded.
  const int per_long = 64 / bits;
  const size_t nlongs = (values.size() + static_cast<size_t>(per_long) - 1) / per_long;
  out->assign(nlongs, 0);
  // The word index and shift are carried forward instead of computed with
  // `/` and `%` per cell: a chunk body is 98,304 cells and an integer division
  // was the largest single cost in the encoder (see the wire bench).
  std::size_t word_index = 0;
  int slot = 0;
  for (size_t i = 0; i < values.size(); ++i) {
    const int32_t v = values[i];
    if (v < 0 || v >= (1LL << bits) || static_cast<size_t>(v) >= limit)
      return false;
    // The word layout is vanilla's: `values` per long, low cell first, no
    // straddling. Reading `out` back as 64-bit words is the same object bytes.
    int64_t& word = (*out)[word_index];
    word = static_cast<int64_t>(static_cast<uint64_t>(word) |
                                (static_cast<uint64_t>(v) << (slot * bits)));
    if (++slot == per_long) {
      slot = 0;
      ++word_index;
    }
  }
  return true;
}

// One section's cells in the wire's packed form, in a single pass.
//
// `rank_of_entry` answers, for a palette index, what the container stores for
// it: the entry's index in the palette the wire writes, or its global state id
// when the container is the global one (`global`). The loop stores each value
// into the long it belongs to as it reads the cell. The previous form filled a
// 16 KiB `std::vector<int32_t>` with the ranks and walked it a second time in
// `pack_cells`, so every section that crossed the wire paid two passes and two
// allocations for one array of numbers — and a chunk body is 24 sections.
//
// A section whose cells are all one value (`SectionCells::uniform`, which is
// most of a generated column's sections) is not read at all: the word repeats
// the same value `per_long` times and the counts are one multiply. That is the
// case the old code still walked 4,096 cells for.
//
// `nonair_of_entry`/`fluid_of_entry` are 0/1 per palette entry, so the section
// header's two counts come out of this same loop. Either pointer may be null
// (the biome container pays for no counts).
// `packed_scratch` is the caller's buffer for the packed words: a chunk body is
// 24 sections and each one needs 2 KiB here, so a fresh vector per section was
// 24 allocations and 24 frees for one send's encode. The caller clears it and
// passes it back; `assign` keeps the capacity when the next section is not
// larger.
bool append_cells_single_pass(mc::net::Writer& w, const SectionCells& cells,
                              const std::vector<int32_t>& rank_of_entry, int bits,
                              const std::vector<int32_t>& ids, bool global,
                              const std::vector<int32_t>* nonair_of_entry,
                              const std::vector<int32_t>* fluid_of_entry, int32_t* non_air,
                              int32_t* fluid, std::vector<int64_t>* packed_scratch) {
  if (bits < 1 || bits > 32 || rank_of_entry.empty())
    return false;
  const int per_long = 64 / bits;
  const size_t n = cells.size();
  const size_t nlongs = (n + static_cast<size_t>(per_long) - 1) / static_cast<size_t>(per_long);
  std::vector<int64_t>& packed = *packed_scratch;
  packed.assign(nlongs, 0);
  const uint64_t limit = (bits == 32) ? 0xFFFFFFFFull : ((1ull << bits) - 1);

  if (cells.uniform()) {
    const size_t p = static_cast<size_t>(cells.uniform_value());
    if (p >= rank_of_entry.size() || rank_of_entry[p] < 0)
      return false;
    const uint64_t v = static_cast<uint64_t>(rank_of_entry[p]);
    if (v > limit)
      return false;
    uint64_t word = 0;
    for (int k = 0; k < per_long; ++k)
      word |= v << (k * bits);
    for (size_t i = 0; i < nlongs; ++i)
      packed[i] = static_cast<int64_t>(word);
    if (nonair_of_entry != nullptr && non_air != nullptr)
      *non_air += static_cast<int32_t>(n) * (*nonair_of_entry)[p];
    if (fluid_of_entry != nullptr && fluid != nullptr)
      *fluid += static_cast<int32_t>(n) * (*fluid_of_entry)[p];
  } else {
    // Local counters: the pointers are loop-invariant, but the compiler cannot
    // prove `*non_air` does not alias `rank_of_entry`, so the old form reloaded
    // and tested them per cell. One addition per section instead.
    int32_t counted_nonair = 0;
    int32_t counted_fluid = 0;
    const bool count_nonair = nonair_of_entry != nullptr && non_air != nullptr;
    const bool count_fluid = fluid_of_entry != nullptr && fluid != nullptr;
    const std::vector<int32_t>& nonair_counts = count_nonair ? *nonair_of_entry : rank_of_entry;
    const std::vector<int32_t>& fluid_counts = count_fluid ? *fluid_of_entry : rank_of_entry;
    // Same carried word/slot as `pack_cells`: no division per cell.
    std::size_t word_index = 0;
    int slot = 0;
    for (size_t i = 0; i < n; ++i) {
      const size_t p = static_cast<size_t>(cells[i]);
      const int32_t rank = p < rank_of_entry.size() ? rank_of_entry[p] : -1;
      if (rank < 0)
        return false;
      const uint64_t v = static_cast<uint64_t>(rank);
      if (v > limit)
        return false;
      int64_t& word = packed[word_index];
      word = static_cast<int64_t>(static_cast<uint64_t>(word) | (v << (slot * bits)));
      if (++slot == per_long) {
        slot = 0;
        ++word_index;
      }
      if (count_nonair)
        counted_nonair += nonair_counts[p];
      if (count_fluid)
        counted_fluid += fluid_counts[p];
    }
    if (count_nonair)
      *non_air += counted_nonair;
    if (count_fluid)
      *fluid += counted_fluid;
  }

  w.u8(static_cast<uint8_t>(bits));
  if (!global) {
    w.varint(static_cast<uint32_t>(ids.size()));
    for (const int32_t id : ids)
      w.varint(static_cast<uint32_t>(id));
  }
  w.i64s(packed.data(), packed.size());
  return true;
}

constexpr int kHeightmapLongs = 37;

const HeightmapType kClientHeightmaps[] = {HeightmapType::MotionBlockingNoLeaves,
                                           HeightmapType::MotionBlocking,
                                           HeightmapType::WorldSurface};

template <typename Bytes> bool all_zero(const Bytes& v) {
  for (const uint8_t b : v)
    if (b != 0)
      return false;
  return true;
}

const std::vector<int64_t>* find_heightmap(const Chunk& chunk, HeightmapType t) {
  for (const auto& [type, data] : chunk.heightmaps) {
    if (type == t)
      return &data;
  }
  return nullptr;
}

std::vector<int64_t>* find_heightmap_mut(Chunk& chunk, HeightmapType t) {
  for (auto& [type, data] : chunk.heightmaps) {
    if (type == t)
      return &data;
  }
  return nullptr;
}

bool is_air_key(const BlockStateKey& k) {
  return k.name == "minecraft:air" || k.name == "minecraft:cave_air" ||
         k.name == "minecraft:void_air";
}

bool is_leaves_key(const BlockStateKey& k) {
  // Vanilla LeavesBlock covers every "*_leaves" block.
  const std::string& n = k.name;
  return n.size() >= 7 && n.compare(n.size() - 7, 7, "_leaves") == 0;
}

// `FluidState.isEmpty()`: the block is a fluid. Both fluid blocks are
// `noCollision()` in vanilla, so the fluid half of what follows is the only
// thing that makes a water or lava cell motion-blocking — which is exactly how
// the heightmap counts an ocean surface.
bool is_fluid_key(const BlockStateKey& k) {
  return k.name == "minecraft:water" || k.name == "minecraft:lava";
}

// One state's answers to the four heightmap predicates, computed once per
// state instead of once per (state, map) pair. Every predicate reads only the
// state's *name* (plus the leaves suffix), so the three name questions and the
// collision-table search are the whole input — and a single block write used to
// ask them up to eight times (`heightmap_predicates_differ` walks four maps
// over the old and the new state). The collision table is a binary search over
// ~150 names, so the per-write saving is not one comparison.
struct HeightmapProfile {
  bool air = false;
  bool leaves = false;
  bool fluid = false;
  bool motion = false; // `BlockState.blocksMotion()`
};

HeightmapProfile heightmap_profile(const BlockStateKey& k) {
  HeightmapProfile p;
  p.air = is_air_key(k);
  if (p.air)
    return p; // opaque for no map; the other three bits cannot matter
  p.leaves = is_leaves_key(k);
  p.fluid = is_fluid_key(k);
  p.motion = block_blocks_motion(k.name);
  return p;
}

bool is_opaque_for(HeightmapType t, const HeightmapProfile& p) {
  if (p.air)
    return false;
  switch (t) {
  case HeightmapType::WorldSurface:
    return true; // NOT_AIR
  case HeightmapType::OceanFloor:
    // `MATERIAL_MOTION_BLOCKING` is `blocksMotion()` alone — no fluid half.
    return p.motion;
  case HeightmapType::MotionBlocking:
    // `blocksMotion() || !fluidState.isEmpty()`.
    return p.motion || p.fluid;
  case HeightmapType::MotionBlockingNoLeaves:
    return (p.motion || p.fluid) && !p.leaves;
  }
  return true;
}

bool is_opaque_for(HeightmapType t, const BlockStateKey& k) {
  return is_opaque_for(t, heightmap_profile(k));
}

// Does a block change move *any* heightmap? Every predicate above reads only the
// state's name, so a same-name retype (a property change — the common case)
// provably moves none, and for a name change the four vanilla maps are the whole
// question. One loop, so a new map cannot be forgotten here.
bool heightmap_predicates_differ(const BlockStateKey& before, const BlockStateKey& after) {
  if (before.name == after.name)
    return false;
  static constexpr HeightmapType kAllHeightmapTypes[] = {
      HeightmapType::WorldSurface, HeightmapType::OceanFloor, HeightmapType::MotionBlocking,
      HeightmapType::MotionBlockingNoLeaves};
  const HeightmapProfile old_profile = heightmap_profile(before);
  const HeightmapProfile new_profile = heightmap_profile(after);
  for (const HeightmapType t : kAllHeightmapTypes) {
    if (is_opaque_for(t, old_profile) != is_opaque_for(t, new_profile))
      return true;
  }
  return false;
}

int heightmap_bits_for_height(int height) {
  int bits = 0;
  // bits = ceil(log2(height+1)) – matches Heightmap(<height+1>).
  while ((1 << bits) < height + 1)
    ++bits;
  if (bits < 1)
    bits = 1;
  return bits;
}

void set_heightmap_entry(std::vector<int64_t>& longs, int idx, int value, int bits) {
  const int per_long = 64 / bits;
  const size_t need = static_cast<size_t>((256 + per_long - 1) / per_long);
  if (longs.size() != need)
    longs.assign(need, 0);
  const int li = idx / per_long;
  const int off = (idx % per_long) * bits;
  const uint64_t mask = (bits >= 64 ? ~0ULL : ((1ULL << bits) - 1ULL));
  uint64_t w = static_cast<uint64_t>(longs[static_cast<size_t>(li)]);
  w &= ~(mask << off);
  w |= (static_cast<uint64_t>(value & static_cast<int>(mask)) << off);
  longs[static_cast<size_t>(li)] = static_cast<int64_t>(w);
}

// The column's stored entry, decoded at the bit width `recompute_column_heightmaps`
// writes with. The one decode: `chunk_heightmap_height` answers through it too.
int chunk_heightmap_value(const std::vector<int64_t>& longs, int idx, int bits) {
  if (longs.empty())
    return 0;
  const int per_long = 64 / bits;
  const size_t li = static_cast<size_t>(idx / per_long);
  if (li >= longs.size())
    return 0;
  const int off = (idx % per_long) * bits;
  const uint64_t mask = (bits >= 64 ? ~0ULL : ((1ULL << bits) - 1ULL));
  return static_cast<int>((static_cast<uint64_t>(longs[li]) >> off) & mask);
}

const BlockStateKey* get_state_at(const Chunk& chunk, int x, int y, int z) {
  // Returns nullptr for air/out-of-range; caller treats nullptr as air.
  const int sec_y = (y >= 0 ? y / 16 : (y - 15) / 16);
  const int sec_idx = sec_y - chunk.min_section_y;
  if (sec_idx < 0 || sec_idx >= static_cast<int>(chunk.sections.size()))
    return nullptr;
  const Section& s = chunk.sections[static_cast<size_t>(sec_idx)];
  if (!s.has_blocks)
    return nullptr;
  if (s.block_palette.empty() || s.block_cells.empty())
    return nullptr;
  const int ly = static_cast<int>(static_cast<unsigned>(y) & 0xFU);
  const int cell = ((ly * 16) + z) * 16 + x;
  if (cell < 0 || static_cast<size_t>(cell) >= s.block_cells.size())
    return nullptr;
  const int pi = s.block_cells[static_cast<size_t>(cell)];
  if (pi < 0 || static_cast<size_t>(pi) >= s.block_palette.size())
    return nullptr;
  return &s.block_palette[static_cast<size_t>(pi)];
}

// `changed_top` is the relative code (`y + 1 - min_y`) of the highest cell a
// heightmap-moving write touched since this column was last current — the one
// cell the search may not start below. It comes from the write that marked the
// column (`Chunk::mark_heightmap_dirty`), and with it the search starts at the
// column's own previous answer instead of at the top of the section stack.
void recompute_column_heightmaps(Chunk& chunk, int col_x, int col_z, int changed_top) {
  if (col_x < 0 || col_x >= 16 || col_z < 0 || col_z >= 16)
    return;
  const int min_y = chunk.min_section_y * 16;
  const int height = static_cast<int>(chunk.sections.size()) * 16;
  const int bits = heightmap_bits_for_height(height);
  const int idx = col_z * 16 + col_x;

  // Ensure client heightmaps exist (so encode never falls back to stale zeros
  // after the first mutation of a column).
  for (HeightmapType t : kClientHeightmaps) {
    if (!find_heightmap_mut(chunk, t)) {
      const int per_long = 64 / bits;
      const size_t need = static_cast<size_t>((256 + per_long - 1) / per_long);
      chunk.heightmaps.emplace_back(t, std::vector<int64_t>(need, 0));
    }
  }
  // Also ensure OCEAN_FLOOR is present if it was on disk – keep it fresh.
  // If absent, we leave it absent (client never sees it).

  static const BlockStateKey kAir{"minecraft:air", ""};
  // One downward walk for every heightmap the chunk carries. Asking them one at
  // a time walked the tens of air cells above the surface once per map — 4 x
  // 384 `get_state_at` calls and string compares per column on a generated
  // chunk. The maps' answers are all "highest cell this map calls opaque", so
  // the walk can start at the highest of their start bounds and stop as soon
  // as each has answered; a cell shared by several maps is decoded once.
  struct Pending {
    std::vector<int64_t>* longs = nullptr;
    HeightmapType type = HeightmapType::WorldSurface;
    int start_rel = 0;
    bool resolved = false;
  };
  std::vector<Pending> pending;
  pending.reserve(chunk.heightmaps.size());
  int max_start_rel = 0;
  for (auto& [type, longs] : chunk.heightmaps) {
    // Where the search may start: the stored entry is the *previous* first
    // opaque cell, and every cell above the highest write since is known not to
    // have changed — so the answer is at or below `max(stored, changed_top)`, and
    // a column whose surface did not move is found on the first cell instead of
    // after walking the air above it. A stored 0 is the fresh column (nothing
    // opaque found yet, or the array has only just been created), where
    // `changed_top` is the only upper bound there is: a bulk writer that knows
    // its highest non-air cell hands it over, and the walk starts there instead
    // of at the top of the section stack. With no write since (`changed_top` 0)
    // the walk still starts at the top.
    const int stored = chunk_heightmap_value(longs, idx, bits);
    int start_rel = height;
    if (stored != 0)
      start_rel = std::max(stored, changed_top);
    else if (changed_top > 0)
      start_rel = changed_top;
    if (start_rel > height)
      start_rel = height;
    pending.push_back(Pending{&longs, type, start_rel, false});
    if (start_rel > max_start_rel)
      max_start_rel = start_rel;
  }
  size_t unresolved = pending.size();
  // The A/B arm for the combined walk (`PLUSMC_HEIGHTMAP_SINGLE_WALK=off`):
  // each map walks the column on its own, which is what this function used to
  // do — one `get_state_at` per cell per map, from each map's own start bound,
  // with no shared decode. The numbers are in docs/PHASE5.md's heightmap
  // section; the predicates and the start bounds are the same on both arms, so
  // the answers are the separate walks' answers.
  static const bool kSingleWalk = [] {
    const char* text = std::getenv("PLUSMC_HEIGHTMAP_SINGLE_WALK");
    return text == nullptr || std::strcmp(text, "off") != 0;
  }();
  if (!kSingleWalk) {
    for (Pending& entry : pending) {
      bool found = false;
      for (int y = min_y + entry.start_rel - 1; y >= min_y; --y) {
        const BlockStateKey* key = get_state_at(chunk, col_x, y, col_z);
        const BlockStateKey& k = key ? *key : kAir;
        if (is_opaque_for(entry.type, k)) {
          set_heightmap_entry(*entry.longs, idx, y + 1 - min_y, bits);
          found = true;
          break;
        }
      }
      if (!found)
        set_heightmap_entry(*entry.longs, idx, 0, bits);
    }
    return;
  }
  // The run of air above a column is one palette entry repeated for dozens of
  // cells; the opacity of the previous key is reused until the key changes.
  const BlockStateKey* last_key = nullptr;
  std::vector<uint8_t> last_mask(pending.size(), 0);
  for (int y = min_y + max_start_rel - 1; y >= min_y && unresolved > 0; --y) {
    const BlockStateKey* key = get_state_at(chunk, col_x, y, col_z);
    const BlockStateKey& k = key ? *key : kAir;
    const int rel = y + 1 - min_y;
    if (key != last_key) {
      // One profile for the cell, four map answers. The maps' predicates share
      // the same name questions, so asking them separately repeated the
      // collision search four times per state change.
      const HeightmapProfile profile = heightmap_profile(k);
      for (size_t i = 0; i < pending.size(); ++i)
        last_mask[i] = is_opaque_for(pending[i].type, profile) ? 1 : 0;
      last_key = key;
    }
    for (size_t i = 0; i < pending.size(); ++i) {
      Pending& entry = pending[i];
      if (entry.resolved || rel > entry.start_rel)
        continue;
      if (last_mask[i]) {
        set_heightmap_entry(*entry.longs, idx, rel, bits);
        entry.resolved = true;
        --unresolved;
      }
    }
  }
  for (size_t i = 0; i < pending.size(); ++i) {
    if (!pending[i].resolved)
      set_heightmap_entry(*pending[i].longs, idx, 0, bits);
  }
}

} // namespace

namespace {

// The generated tables as name sets, built once. The tables are small (151
// collision-free names, ~230 non-occluding ones) but the questions are not: a
// feature's `would_survive`, a plant placement, a mob-spawn gate and every
// light cell ask one of these per position, and the binary search they used to
// cost did a `strlen` at each of ~8 steps (`string_view::compare(const char*)`
// measures the literal first). A hash of the already-measured `string_view` is
// one pass over the name and one probe; the keys are the table's own literals,
// so no string is copied.
const std::unordered_set<std::string_view>& non_motion_blocking_names() {
  static const std::unordered_set<std::string_view> names = [] {
    std::unordered_set<std::string_view> out;
    out.reserve(static_cast<size_t>(kNonMotionBlockingCount) * 2);
    for (int i = 0; i < kNonMotionBlockingCount; ++i)
      out.emplace(kNonMotionBlocking[i]);
    return out;
  }();
  return names;
}

const std::unordered_set<std::string_view>& no_occlusion_names() {
  static const std::unordered_set<std::string_view> names = [] {
    std::unordered_set<std::string_view> out;
    out.reserve(static_cast<size_t>(kNoOcclusionCount) * 2);
    for (int i = 0; i < kNoOcclusionCount; ++i)
      out.emplace(kNoOcclusion[i]);
    return out;
  }();
  return names;
}

} // namespace

bool block_blocks_motion(std::string_view block_name) {
  // The generated table (world/collision_table.h) lists the minority of blocks
  // whose `blocksMotion()` is false; a name it does not carry collides.
  return non_motion_blocking_names().count(block_name) == 0;
}

bool block_no_occlusion(std::string_view block_name) {
  // The same shape over the generated occlusion table (world/occlusion_table.h):
  // the *other* half of `isSolidRender()`, and what keeps glass and ice
  // transparent to light while stone is not (`light_is_opaque` in world/light.h).
  return no_occlusion_names().count(block_name) != 0;
}

namespace {

// A bounds-checked cursor over one decompressed chunk record. Every read checks
// the remaining bytes before it moves; a short read leaves the cursor where it
// was and returns false, which the caller turns into "no chunk" instead of a
// crash. This is what lets the loader read the record once instead of building
// the intermediate `nbt::Value` tree: a cold column's tree was three vectors and
// a string per tag, and the loader only ever wanted the handful of fields in it.
class NbtCursor {
public:
  NbtCursor(const uint8_t* data, std::size_t len) : data_(data), len_(len) {}

  bool u8(uint8_t* out) {
    if (at_ + 1 > len_)
      return false;
    *out = data_[at_++];
    return true;
  }
  bool u16(uint16_t* out) {
    if (at_ + 2 > len_)
      return false;
    *out = static_cast<uint16_t>((data_[at_] << 8) | data_[at_ + 1]);
    at_ += 2;
    return true;
  }
  bool i32(int32_t* out) {
    if (at_ + 4 > len_)
      return false;
    const uint32_t v =
        (static_cast<uint32_t>(data_[at_]) << 24) | (static_cast<uint32_t>(data_[at_ + 1]) << 16) |
        (static_cast<uint32_t>(data_[at_ + 2]) << 8) | static_cast<uint32_t>(data_[at_ + 3]);
    *out = static_cast<int32_t>(v);
    at_ += 4;
    return true;
  }
  bool i64(int64_t* out) {
    if (at_ + 8 > len_)
      return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
      v = (v << 8) | data_[at_ + static_cast<std::size_t>(i)];
    *out = static_cast<int64_t>(v);
    at_ += 8;
    return true;
  }
  bool take(std::size_t n, const uint8_t** out) {
    if (at_ + n > len_)
      return false;
    *out = data_ + at_;
    at_ += n;
    return true;
  }
  bool skip(std::size_t n) {
    if (at_ + n > len_)
      return false;
    at_ += n;
    return true;
  }
  bool string(std::string_view* out) {
    uint16_t n = 0;
    if (!u16(&n) || at_ + n > len_)
      return false;
    *out = std::string_view(reinterpret_cast<const char*>(data_ + at_), n);
    at_ += n;
    return true;
  }

  // Walks the payload of `tag` without interpreting it: a field this loader does
  // not read (a newer format's, a mod's) still costs its own bytes and nothing
  // else. `depth` bounds the recursion a hostile record can ask for.
  bool skip_payload(uint8_t tag, int depth = 0) {
    if (depth > 64)
      return false;
    switch (static_cast<nbt::Tag>(tag)) {
    case nbt::Tag::Byte:
      return skip(1);
    case nbt::Tag::Short:
      return skip(2);
    case nbt::Tag::Int:
    case nbt::Tag::Float:
      return skip(4);
    case nbt::Tag::Long:
    case nbt::Tag::Double:
      return skip(8);
    case nbt::Tag::ByteArray: {
      int32_t n = 0;
      return i32(&n) && n >= 0 && skip(static_cast<std::size_t>(n));
    }
    case nbt::Tag::String: {
      std::string_view s;
      return string(&s);
    }
    case nbt::Tag::List: {
      uint8_t elem = 0;
      int32_t n = 0;
      if (!u8(&elem) || !i32(&n) || n < 0 || n > kMaxListEntries)
        return false;
      if (elem == 0)
        return n == 0;
      for (int32_t i = 0; i < n; ++i)
        if (!skip_payload(elem, depth + 1))
          return false;
      return true;
    }
    case nbt::Tag::Compound:
      for (;;) {
        uint8_t inner = 0;
        std::string_view name;
        if (!u8(&inner))
          return false;
        if (inner == 0)
          return true;
        if (!string(&name) || !skip_payload(inner, depth + 1))
          return false;
      }
    case nbt::Tag::IntArray: {
      int32_t n = 0;
      return i32(&n) && n >= 0 && skip(static_cast<std::size_t>(n) * 4);
    }
    case nbt::Tag::LongArray: {
      int32_t n = 0;
      return i32(&n) && n >= 0 && skip(static_cast<std::size_t>(n) * 8);
    }
    case nbt::Tag::End:
      return false;
    }
    return false;
  }

private:
  static constexpr int32_t kMaxListEntries = 1 << 20;
  const uint8_t* data_;
  std::size_t len_;
  std::size_t at_ = 0;
};

bool is_int_tag(uint8_t tag) {
  return tag == static_cast<uint8_t>(nbt::Tag::Byte) ||
         tag == static_cast<uint8_t>(nbt::Tag::Short) ||
         tag == static_cast<uint8_t>(nbt::Tag::Int) || tag == static_cast<uint8_t>(nbt::Tag::Long);
}

bool read_int(NbtCursor& r, uint8_t tag, int64_t* out) {
  switch (static_cast<nbt::Tag>(tag)) {
  case nbt::Tag::Byte: {
    uint8_t v = 0;
    if (!r.u8(&v))
      return false;
    *out = static_cast<int8_t>(v);
    return true;
  }
  case nbt::Tag::Short: {
    uint16_t v = 0;
    if (!r.u16(&v))
      return false;
    *out = static_cast<int16_t>(v);
    return true;
  }
  case nbt::Tag::Int: {
    int32_t v = 0;
    if (!r.i32(&v))
      return false;
    *out = v;
    return true;
  }
  case nbt::Tag::Long:
    return r.i64(out);
  default:
    return false;
  }
}

// Reads one whole tag into the generic tree. Only `block_entities` needs it: the
// container layer hands those compounds back to the save path, so they have to
// be kept exactly as written, fields this server does not model included.
bool read_value(NbtCursor& r, uint8_t tag, nbt::Value* out, int depth) {
  if (depth > 64)
    return false;
  out->type = static_cast<nbt::Tag>(tag);
  switch (static_cast<nbt::Tag>(tag)) {
  case nbt::Tag::Byte:
  case nbt::Tag::Short:
  case nbt::Tag::Int:
  case nbt::Tag::Long:
    return read_int(r, tag, &out->i);
  case nbt::Tag::Float: {
    int32_t bits = 0;
    if (!r.i32(&bits))
      return false;
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    out->d = f;
    return true;
  }
  case nbt::Tag::Double: {
    int64_t bits = 0;
    if (!r.i64(&bits))
      return false;
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    out->d = d;
    return true;
  }
  case nbt::Tag::ByteArray: {
    int32_t n = 0;
    const uint8_t* p = nullptr;
    if (!r.i32(&n) || n < 0 || !r.take(static_cast<std::size_t>(n), &p))
      return false;
    out->bytes.resize(static_cast<std::size_t>(n));
    if (n > 0)
      std::memcpy(out->bytes.data(), p, static_cast<std::size_t>(n));
    return true;
  }
  case nbt::Tag::String: {
    std::string_view s;
    if (!r.string(&s))
      return false;
    out->s.assign(s);
    return true;
  }
  case nbt::Tag::List: {
    uint8_t elem = 0;
    int32_t n = 0;
    if (!r.u8(&elem) || !r.i32(&n) || n < 0 || n > (1 << 20))
      return false;
    out->list_type = static_cast<nbt::Tag>(elem);
    out->items.resize(static_cast<std::size_t>(n));
    for (int32_t i = 0; i < n; ++i)
      if (!read_value(r, elem, &out->items[static_cast<std::size_t>(i)], depth + 1))
        return false;
    return true;
  }
  case nbt::Tag::Compound:
    for (;;) {
      uint8_t inner = 0;
      std::string_view name;
      if (!r.u8(&inner))
        return false;
      if (inner == 0)
        return true;
      if (!r.string(&name))
        return false;
      nbt::Value value;
      if (!read_value(r, inner, &value, depth + 1))
        return false;
      out->fields.emplace_back(std::string(name), std::move(value));
    }
  case nbt::Tag::IntArray: {
    int32_t n = 0;
    if (!r.i32(&n) || n < 0)
      return false;
    out->ints.resize(static_cast<std::size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
      int32_t v = 0;
      if (!r.i32(&v))
        return false;
      out->ints[static_cast<std::size_t>(i)] = v;
    }
    return true;
  }
  case nbt::Tag::LongArray: {
    int32_t n = 0;
    if (!r.i32(&n) || n < 0)
      return false;
    out->longs.resize(static_cast<std::size_t>(n));
    for (int32_t i = 0; i < n; ++i)
      if (!r.i64(&out->longs[static_cast<std::size_t>(i)]))
        return false;
    return true;
  }
  case nbt::Tag::End:
    return false;
  }
  return false;
}

// A LongArray payload stays in the record until its width is known: the palette
// is what decides the width, and vanilla writes it before `data`, but a record
// is not trusted to. This is the unpack `SectionCells::assign_packed` wants,
// from big-endian bytes, without materialising the array twice.
bool unpack_be_cells(const uint8_t* p, int32_t nlongs, int bits, std::size_t count,
                     SectionCells* cells) {
  if (bits < 1 || p == nullptr || nlongs <= 0)
    return false;
  std::vector<int64_t> longs(static_cast<std::size_t>(nlongs));
  for (int32_t i = 0; i < nlongs; ++i) {
    uint64_t v = 0;
    for (int b = 0; b < 8; ++b)
      v = (v << 8) | p[static_cast<std::size_t>(i) * 8 + static_cast<std::size_t>(b)];
    longs[static_cast<std::size_t>(i)] = static_cast<int64_t>(v);
  }
  return cells->assign_packed(longs.data(), longs.size(), bits, count);
}

// The palette entry's canonical key: name plus the sorted "k=v,k=v" string.
bool read_palette_entry(NbtCursor& r, BlockStateKey* key) {
  for (;;) {
    uint8_t tag = 0;
    std::string_view field;
    if (!r.u8(&tag))
      return false;
    if (tag == 0)
      return true;
    if (!r.string(&field))
      return false;
    // 26.2 spells these `Name`/`Properties`; 26.3's compact compound uses
    // `id`/`properties`, and a property-less state in that form is a single
    // string under the empty field name.
    if ((field == "Name" || field == "id" || field.empty()) &&
        tag == static_cast<uint8_t>(nbt::Tag::String)) {
      std::string_view s;
      if (!r.string(&s))
        return false;
      key->name.assign(s);
    } else if ((field == "Properties" || field == "properties") &&
               tag == static_cast<uint8_t>(nbt::Tag::Compound)) {
      std::vector<std::pair<std::string, std::string>> props;
      for (;;) {
        uint8_t inner = 0;
        std::string_view pname;
        if (!r.u8(&inner))
          return false;
        if (inner == 0)
          break;
        if (!r.string(&pname))
          return false;
        std::string_view value;
        if (inner == static_cast<uint8_t>(nbt::Tag::String) && r.string(&value))
          props.emplace_back(std::string(pname), std::string(value));
        else if (!r.skip_payload(inner, 1))
          return false;
      }
      std::sort(props.begin(), props.end());
      for (std::size_t i = 0; i < props.size(); ++i) {
        if (i)
          key->properties.push_back(',');
        key->properties.append(props[i].first).push_back('=');
        key->properties.append(props[i].second);
      }
    } else if (!r.skip_payload(tag, 1)) {
      return false;
    }
  }
}

bool parse_direct_section(NbtCursor& r, Section* s, int* y) {
  const uint8_t* block_data = nullptr;
  int32_t block_longs = 0;
  const uint8_t* biome_data = nullptr;
  int32_t biome_longs = 0;
  bool saw_blocks = false, saw_biomes = false;

  for (;;) {
    uint8_t tag = 0;
    std::string_view field;
    if (!r.u8(&tag))
      return false;
    if (tag == 0)
      break;
    if (!r.string(&field))
      return false;
    if (field == "Y" && is_int_tag(tag)) {
      int64_t v = 0;
      if (!read_int(r, tag, &v))
        return false;
      *y = static_cast<int>(v);
    } else if (field == "block_states" && tag == static_cast<uint8_t>(nbt::Tag::Compound)) {
      saw_blocks = true;
      for (;;) {
        uint8_t inner = 0;
        std::string_view name;
        if (!r.u8(&inner))
          return false;
        if (inner == 0)
          break;
        if (!r.string(&name))
          return false;
        if (name == "palette" && inner == static_cast<uint8_t>(nbt::Tag::List)) {
          uint8_t elem = 0;
          int32_t n = 0;
          if (!r.u8(&elem) || !r.i32(&n) || n < 0 || n > 4096)
            return false;
          s->block_palette.reserve(static_cast<std::size_t>(n));
          for (int32_t i = 0; i < n; ++i) {
            BlockStateKey key;
            if (elem == static_cast<uint8_t>(nbt::Tag::String)) {
              // 26.3's compact palette: a property-less state is the state
              // string itself, not the 26.2 `{Name: ...}` compound.
              std::string_view state;
              if (!r.string(&state))
                return false;
              key.name.assign(state);
            } else if (elem == static_cast<uint8_t>(nbt::Tag::Compound)) {
              if (!read_palette_entry(r, &key))
                return false;
            } else if (!r.skip_payload(elem, 1)) {
              return false;
            }
            s->block_palette.push_back(std::move(key));
          }
        } else if (name == "data" && inner == static_cast<uint8_t>(nbt::Tag::LongArray)) {
          if (!r.i32(&block_longs) || block_longs < 0 ||
              !r.take(static_cast<std::size_t>(block_longs) * 8, &block_data))
            return false;
        } else if (!r.skip_payload(inner, 1)) {
          return false;
        }
      }
    } else if (field == "biomes" && tag == static_cast<uint8_t>(nbt::Tag::Compound)) {
      saw_biomes = true;
      for (;;) {
        uint8_t inner = 0;
        std::string_view name;
        if (!r.u8(&inner))
          return false;
        if (inner == 0)
          break;
        if (!r.string(&name))
          return false;
        if (name == "palette" && inner == static_cast<uint8_t>(nbt::Tag::List)) {
          uint8_t elem = 0;
          int32_t n = 0;
          if (!r.u8(&elem) || !r.i32(&n) || n < 0 || n > 4096)
            return false;
          s->biome_palette.reserve(static_cast<std::size_t>(n));
          for (int32_t i = 0; i < n; ++i) {
            std::string_view biome;
            if (elem != static_cast<uint8_t>(nbt::Tag::String) || !r.string(&biome))
              return false;
            s->biome_palette.emplace_back(biome);
          }
        } else if (name == "data" && inner == static_cast<uint8_t>(nbt::Tag::LongArray)) {
          if (!r.i32(&biome_longs) || biome_longs < 0 ||
              !r.take(static_cast<std::size_t>(biome_longs) * 8, &biome_data))
            return false;
        } else if (!r.skip_payload(inner, 1)) {
          return false;
        }
      }
    } else if (field == "SkyLight" && tag == static_cast<uint8_t>(nbt::Tag::ByteArray)) {
      int32_t n = 0;
      const uint8_t* p = nullptr;
      if (!r.i32(&n) || n < 0 || !r.take(static_cast<std::size_t>(n), &p))
        return false;
      if (n == static_cast<int32_t>(LightArray::kBytes))
        s->sky_light = LightArray(p, p + n);
    } else if (field == "BlockLight" && tag == static_cast<uint8_t>(nbt::Tag::ByteArray)) {
      int32_t n = 0;
      const uint8_t* p = nullptr;
      if (!r.i32(&n) || n < 0 || !r.take(static_cast<std::size_t>(n), &p))
        return false;
      if (n == static_cast<int32_t>(LightArray::kBytes))
        s->block_light = LightArray(p, p + n);
    } else if (!r.skip_payload(tag, 1)) {
      return false;
    }
  }

  if (saw_blocks) {
    s->has_blocks = true;
    const int bits = block_bits_for_palette_size(s->block_palette.size());
    if (bits < 0)
      return false;
    if (bits == 0) {
      if (block_longs != 0)
        return false;
      s->block_cells.assign(kBlockCellsPerSection, 0);
    } else {
      if (block_longs != static_cast<int32_t>(long_count_for(kBlockCellsPerSection, bits)))
        return false;
      if (!unpack_be_cells(block_data, block_longs, bits, kBlockCellsPerSection, &s->block_cells))
        return false;
    }
  }
  if (saw_biomes) {
    s->has_biomes = true;
    const int bits = biome_bits_for_palette_size(s->biome_palette.size());
    if (bits < 0)
      return false;
    if (bits == 0) {
      if (biome_longs != 0)
        return false;
      s->biome_cells.assign(kBiomeCellsPerSection, 0);
    } else {
      if (biome_longs != static_cast<int32_t>(long_count_for(kBiomeCellsPerSection, bits)))
        return false;
      if (!unpack_be_cells(biome_data, biome_longs, bits, kBiomeCellsPerSection, &s->biome_cells))
        return false;
    }
  }
  return true;
}

} // namespace

bool parse_chunk_nbt_direct(const uint8_t* data, std::size_t len, int min_section_y,
                            int section_count, Chunk* out) {
  NbtCursor r(data, len);
  uint8_t root_tag = 0;
  std::string_view root_name;
  if (!r.u8(&root_tag) || root_tag != static_cast<uint8_t>(nbt::Tag::Compound) ||
      !r.string(&root_name))
    return false;

  out->min_section_y = min_section_y;
  out->sections.assign(static_cast<std::size_t>(section_count), Section{});
  for (int i = 0; i < section_count; ++i)
    out->sections[static_cast<std::size_t>(i)].y = min_section_y + i;

  bool saw_x = false, saw_z = false, saw_sections = false;
  for (;;) {
    uint8_t tag = 0;
    std::string_view field;
    if (!r.u8(&tag))
      return false;
    if (tag == 0)
      break;
    if (!r.string(&field))
      return false;
    if ((field == "xPos" || field == "zPos") && is_int_tag(tag)) {
      int64_t v = 0;
      if (!read_int(r, tag, &v))
        return false;
      if (field == "xPos") {
        out->x = static_cast<int>(v);
        saw_x = true;
      } else {
        out->z = static_cast<int>(v);
        saw_z = true;
      }
    } else if (field == "Heightmaps" && tag == static_cast<uint8_t>(nbt::Tag::Compound)) {
      for (;;) {
        uint8_t inner = 0;
        std::string_view name;
        if (!r.u8(&inner))
          return false;
        if (inner == 0)
          break;
        if (!r.string(&name))
          return false;
        if (inner == static_cast<uint8_t>(nbt::Tag::LongArray)) {
          HeightmapType type;
          bool known = true;
          if (name == "WORLD_SURFACE")
            type = HeightmapType::WorldSurface;
          else if (name == "OCEAN_FLOOR")
            type = HeightmapType::OceanFloor;
          else if (name == "MOTION_BLOCKING")
            type = HeightmapType::MotionBlocking;
          else if (name == "MOTION_BLOCKING_NO_LEAVES")
            type = HeightmapType::MotionBlockingNoLeaves;
          else
            known = false;
          int32_t n = 0;
          if (!r.i32(&n) || n < 0)
            return false;
          if (known) {
            std::vector<int64_t> longs(static_cast<std::size_t>(n));
            for (int32_t i = 0; i < n; ++i)
              if (!r.i64(&longs[static_cast<std::size_t>(i)]))
                return false;
            out->heightmaps.emplace_back(type, std::move(longs));
          } else if (!r.skip(static_cast<std::size_t>(n) * 8)) {
            return false;
          }
        } else if (!r.skip_payload(inner, 1)) {
          return false;
        }
      }
    } else if (field == "sections" && tag == static_cast<uint8_t>(nbt::Tag::List)) {
      saw_sections = true;
      uint8_t elem = 0;
      int32_t n = 0;
      if (!r.u8(&elem) || elem != static_cast<uint8_t>(nbt::Tag::Compound) || !r.i32(&n) || n < 0 ||
          n > 4096)
        return false;
      for (int32_t i = 0; i < n; ++i) {
        Section section;
        int y = 0;
        if (!parse_direct_section(r, &section, &y))
          return false;
        const int idx = y - min_section_y;
        if (idx < 0 || idx >= section_count)
          continue; // a section outside this dimension is skipped, like the tree path
        section.y = y;
        out->sections[static_cast<std::size_t>(idx)] = std::move(section);
      }
    } else if (field == "block_entities" && tag == static_cast<uint8_t>(nbt::Tag::List)) {
      uint8_t elem = 0;
      int32_t n = 0;
      if (!r.u8(&elem) || !r.i32(&n) || n < 0 || n > (1 << 20))
        return false;
      out->block_entities.resize(static_cast<std::size_t>(n));
      for (int32_t i = 0; i < n; ++i)
        if (!read_value(r, elem, &out->block_entities[static_cast<std::size_t>(i)], 1))
          return false;
    } else if (!r.skip_payload(tag)) {
      return false;
    }
  }

  // The tree path requires both coordinates and a section list (a record with
  // neither is not a chunk, and `load_from_disk` reports it as absent); the
  // direct reader has to reject the same records, or a truncated file would
  // load as an all-air column instead of a missing one.
  if (!saw_x || !saw_z || !saw_sections)
    return false;
  chunk_recount_random_tick_blocks(*out);
  return true;
}

bool parse_chunk_nbt(const nbt::Value& root, int min_section_y, int section_count, Chunk* out) {
  const nbt::Value* xc = nbt::child(root, "xPos");
  const nbt::Value* zc = nbt::child(root, "zPos");
  auto xs = xc ? nbt::as_int(*xc) : std::nullopt;
  auto zs = zc ? nbt::as_int(*zc) : std::nullopt;
  if (!xs || !zs)
    return false;
  out->x = static_cast<int>(*xs);
  out->z = static_cast<int>(*zs);
  out->min_section_y = min_section_y;
  out->sections.assign(static_cast<size_t>(section_count), Section{});
  for (int i = 0; i < section_count; ++i)
    out->sections[static_cast<size_t>(i)].y = min_section_y + i;

  if (const auto* hm = nbt::child(root, "Heightmaps"); hm != nullptr) {
    for (const auto& [name, map] : hm->fields) {
      const auto* longs = nbt::as_longs(map);
      if (!longs)
        continue;
      HeightmapType t;
      if (name == "WORLD_SURFACE") {
        t = HeightmapType::WorldSurface;
      } else if (name == "OCEAN_FLOOR") {
        t = HeightmapType::OceanFloor;
      } else if (name == "MOTION_BLOCKING") {
        t = HeightmapType::MotionBlocking;
      } else if (name == "MOTION_BLOCKING_NO_LEAVES") {
        t = HeightmapType::MotionBlockingNoLeaves;
      } else {
        continue;
      }
      out->heightmaps.emplace_back(t, *longs);
    }
  }

  const auto* sections = nbt::child(root, "sections");
  if (sections == nullptr || sections->type != nbt::Tag::List)
    return false;
  for (const auto& sec : sections->items) {
    if (sec.type != nbt::Tag::Compound)
      continue;
    const nbt::Value* yc = nbt::child(sec, "Y");
    auto yv = yc ? nbt::as_int(*yc) : std::nullopt;
    if (!yv)
      continue;
    const int idx = static_cast<int>(*yv) - min_section_y;
    if (idx < 0 || idx >= section_count)
      continue;
    Section& s = out->sections[static_cast<size_t>(idx)];

    if (const auto* bs = nbt::child(sec, "block_states"); bs != nullptr) {
      s.has_blocks = true;
      const auto* palette = nbt::child(*bs, "palette");
      if (palette != nullptr && palette->type == nbt::Tag::List) {
        s.block_palette.reserve(palette->items.size());
        for (const auto& entry : palette->items) {
          BlockStateKey key;
          if (entry.type == nbt::Tag::String) {
            // 26.3's compact palette string for a property-less state.
            if (auto n = nbt::as_string(entry))
              key.name.assign(*n);
            s.block_palette.push_back(std::move(key));
            continue;
          }
          const nbt::Value* nc = nbt::child(entry, "Name");
          if (nc == nullptr)
            nc = nbt::child(entry, "id");
          if (nc == nullptr)
            nc = nbt::child(entry, "");
          if (auto n = nc ? nbt::as_string(*nc) : std::nullopt)
            key.name.assign(*n);
          // The properties are sorted and joined into the canonical string, and
          // the sort is the only reason this needs an intermediate at all. It
          // names the tree's own strings (`string_view`) instead of copying them
          // into a pair of `std::string`s: a palette entry carries a handful of
          // properties with names past the short-string buffer, and the copy was
          // the largest single item in a cold section parse.
          std::vector<std::pair<std::string_view, std::string_view>> props;
          const nbt::Value* p = nbt::child(entry, "Properties");
          if (p == nullptr)
            p = nbt::child(entry, "properties");
          if (p != nullptr && p->type == nbt::Tag::Compound) {
            props.reserve(p->fields.size());
            for (const auto& [k, v] : p->fields) {
              const auto vs = nbt::as_string(v);
              props.emplace_back(std::string_view(k), vs ? *vs : std::string_view());
            }
          }
          std::sort(props.begin(), props.end());
          std::size_t joined = 0;
          for (const auto& [k, v] : props)
            joined += k.size() + v.size() + 2; // "k=v,"
          key.properties.reserve(joined);
          for (size_t i = 0; i < props.size(); ++i) {
            if (i)
              key.properties.push_back(',');
            key.properties.append(props[i].first).push_back('=');
            key.properties.append(props[i].second);
          }
          s.block_palette.push_back(std::move(key));
        }
      }
      const int dbits = block_bits_for_palette_size(s.block_palette.size());
      const auto* data_child = nbt::child(*bs, "data");
      const auto* data = data_child ? nbt::as_longs(*data_child) : nullptr;
      if (dbits == 0) {
        if (data != nullptr && !data->empty())
          return false;
        s.block_cells.assign(kBlockCellsPerSection, 0);
      } else {
        if (data == nullptr || data->size() != long_count_for(kBlockCellsPerSection, dbits))
          return false;
        // One bulk unpack at the container's declared width: `assign_packed`
        // materialises the buffer once and stores each cell directly, where the
        // per-cell `set` had to re-check uniformity and fit 4096 times per
        // section (`SectionCells` says why that is the loader's hot loop).
        if (!s.block_cells.assign_packed(data->data(), data->size(), dbits, kBlockCellsPerSection))
          return false;
      }
    }

    if (const auto* bio = nbt::child(sec, "biomes"); bio != nullptr) {
      s.has_biomes = true;
      if (const auto* palette = nbt::child(*bio, "palette");
          palette != nullptr && palette->type == nbt::Tag::List) {
        s.biome_palette.reserve(palette->items.size());
        for (const auto& entry : palette->items) {
          if (auto n = nbt::as_string(entry))
            s.biome_palette.emplace_back(*n);
        }
      }
      const int dbits = biome_bits_for_palette_size(s.biome_palette.size());
      const auto* data_child = nbt::child(*bio, "data");
      const auto* data = data_child ? nbt::as_longs(*data_child) : nullptr;
      if (dbits == 0) {
        if (data != nullptr && !data->empty())
          return false;
        s.biome_cells.assign(kBiomeCellsPerSection, 0);
      } else {
        if (data == nullptr || data->size() != long_count_for(kBiomeCellsPerSection, dbits) ||
            !s.biome_cells.assign_packed(data->data(), data->size(), dbits, kBiomeCellsPerSection))
          return false;
      }
    }

    const auto* sky_child = nbt::child(sec, "SkyLight");
    if (const auto* sky = sky_child ? nbt::as_bytes(*sky_child) : nullptr)
      s.sky_light = LightArray(sky->begin(), sky->end());
    const auto* bl_child = nbt::child(sec, "BlockLight");
    if (const auto* bl = bl_child ? nbt::as_bytes(*bl_child) : nullptr)
      s.block_light = LightArray(bl->begin(), bl->end());
  }

  // Every section's randomly-ticking count, which the random-tick pass reads as
  // its section gate (`LevelChunkSection.recalcBlockCounts`; the disk format has
  // no counter to trust).
  chunk_recount_random_tick_blocks(*out);

  // Block entities are kept verbatim (see Chunk::block_entities): only the
  // list shape is validated, the compounds themselves are stored as parsed.
  if (const auto* bes = nbt::child(root, "block_entities");
      bes != nullptr && bes->type == nbt::Tag::List) {
    for (const auto& be : bes->items) {
      if (be.type == nbt::Tag::Compound)
        out->block_entities.push_back(be);
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Milestone E: Chunk -> NBT serialization
// ---------------------------------------------------------------------------

nbt::Value serialize_chunk_nbt(const Chunk& chunk, int32_t data_version) {
  using namespace nbt;

  // Heightmaps compound: each entry is a LongArray.
  std::vector<std::pair<std::string, Value>> hm_fields;
  for (const auto& [t, longs] : chunk.heightmaps) {
    std::string name;
    switch (t) {
    case HeightmapType::WorldSurface:
      name = "WORLD_SURFACE";
      break;
    case HeightmapType::OceanFloor:
      name = "OCEAN_FLOOR";
      break;
    case HeightmapType::MotionBlocking:
      name = "MOTION_BLOCKING";
      break;
    case HeightmapType::MotionBlockingNoLeaves:
      name = "MOTION_BLOCKING_NO_LEAVES";
      break;
    }
    hm_fields.emplace_back(name, make_long_array(longs));
  }

  // Sections list: one compound per section Y, each with block_states + biomes if present.
  std::vector<Value> sec_list;
  sec_list.reserve(chunk.sections.size());
  for (const Section& s : chunk.sections) {
    std::vector<std::pair<std::string, Value>> sec_fields;
    sec_fields.emplace_back("Y", make_byte(static_cast<int8_t>(s.y)));

    if (s.has_blocks) {
      // Palette: list of compounds {Name, Properties?}
      std::vector<Value> pal_items;
      pal_items.reserve(s.block_palette.size());
      for (const auto& key : s.block_palette) {
        std::vector<std::pair<std::string, Value>> entry;
        entry.emplace_back("Name", make_string(key.name));
        if (!key.properties.empty()) {
          // Parse "k=v,k=v" back into a compound.
          std::vector<std::pair<std::string, Value>> props;
          std::string rem = key.properties;
          size_t pos = 0;
          while (pos < rem.size()) {
            size_t comma = rem.find(',', pos);
            std::string kv =
                (comma == std::string::npos) ? rem.substr(pos) : rem.substr(pos, comma - pos);
            size_t eq = kv.find('=');
            if (eq != std::string::npos) {
              props.emplace_back(kv.substr(0, eq), make_string(kv.substr(eq + 1)));
            }
            if (comma == std::string::npos)
              break;
            pos = comma + 1;
          }
          entry.emplace_back("Properties", make_compound(std::move(props)));
        }
        pal_items.push_back(make_compound(std::move(entry)));
      }
      std::vector<std::pair<std::string, Value>> bs_fields;
      bs_fields.emplace_back("palette", make_list(Tag::Compound, std::move(pal_items)));
      const int bits = block_bits_for_palette_size(s.block_palette.size());
      if (bits > 0) {
        std::vector<int64_t> packed;
        // Block cells are palette indices (0..palSize-1), safe at `bits` by construction.
        // A caller that mutated through `set_block_state` has already normalized them,
        // so the cells are packed where they live: this used to copy them into a
        // `std::vector<int32_t>` first, which is a 16 KiB allocation per section per
        // save for no change in the bytes written.
        const SectionCells& cells = s.block_cells;
        pack_cells(cells, bits, &packed);
        bs_fields.emplace_back("data", make_long_array(std::move(packed)));
      }
      sec_fields.emplace_back("block_states", make_compound(std::move(bs_fields)));
    }

    if (s.has_biomes) {
      std::vector<Value> pal_items;
      pal_items.reserve(s.biome_palette.size());
      for (const auto& name : s.biome_palette) {
        pal_items.push_back(make_string(name));
      }
      std::vector<std::pair<std::string, Value>> bio_fields;
      bio_fields.emplace_back("palette", make_list(Tag::String, std::move(pal_items)));
      const int bits = biome_bits_for_palette_size(s.biome_palette.size());
      if (bits > 0) {
        std::vector<int64_t> packed;
        pack_cells(s.biome_cells, bits, &packed);
        bio_fields.emplace_back("data", make_long_array(std::move(packed)));
      }
      sec_fields.emplace_back("biomes", make_compound(std::move(bio_fields)));
    }

    // Light arrays are written only when they carry light. An all-zero array is
    // the absence of light, which the format spells by leaving the field out
    // (vanilla does the same: a section's light is written when it has any), and
    // a generated column now keeps one uniform zero array for every section
    // below its surface — 2 KiB of zeros per section that deflate turns into
    // nothing but that no reader needs stored. The loader reads an absent array
    // as `nullopt` and every reader treats that as 0, so the round-trip is
    // unchanged.
    if (s.sky_light && !all_zero(*s.sky_light)) {
      sec_fields.emplace_back("SkyLight",
                              make_byte_array_u8(s.sky_light->data(), s.sky_light->size()));
    }
    if (s.block_light && !all_zero(*s.block_light)) {
      sec_fields.emplace_back("BlockLight",
                              make_byte_array_u8(s.block_light->data(), s.block_light->size()));
    }
    sec_list.push_back(make_compound(std::move(sec_fields)));
  }

  std::vector<std::pair<std::string, Value>> root;
  root.emplace_back("DataVersion", make_int(data_version));
  root.emplace_back("xPos", make_int(chunk.x));
  root.emplace_back("yPos", make_int(chunk.min_section_y));
  root.emplace_back("zPos", make_int(chunk.z));
  root.emplace_back("Status", make_string("minecraft:full"));
  root.emplace_back("LastUpdate", make_long(0));
  root.emplace_back("InhabitedTime", make_long(0));
  root.emplace_back("isLightOn", make_byte(1));
  root.emplace_back("Heightmaps", make_compound(std::move(hm_fields)));
  root.emplace_back("sections", make_list(Tag::Compound, std::move(sec_list)));
  // Block entities are written back exactly as they were read (plus whatever
  // the container layer injected) so fields we do not model survive the save.
  root.emplace_back("block_entities", make_list(Tag::Compound, chunk.block_entities));
  root.emplace_back("block_ticks", make_list(Tag::Compound, {}));
  root.emplace_back("fluid_ticks", make_list(Tag::Compound, {}));
  root.emplace_back("PostProcessing", make_list(Tag::List, {}));
  root.emplace_back("structures", make_compound({
                                      {"starts", make_compound({})},
                                      {"References", make_compound({})},
                                  }));
  return make_compound(std::move(root));
}

namespace {

// NBT's name encoding: a u16 length and the bytes. (The protocol's Writer has
// `string` for VarInt-length strings, which is the packet format, not this one.)
void write_nbt_name(mc::net::Writer& w, std::string_view name) {
  w.u16(static_cast<uint16_t>(name.size()));
  w.bytes(reinterpret_cast<const uint8_t*>(name.data()), name.size());
}

// A compound entry's tag byte and name, with the payload left to the caller.
void write_nbt_header(mc::net::Writer& w, nbt::Tag tag, std::string_view name) {
  w.u8(static_cast<uint8_t>(tag));
  write_nbt_name(w, name);
}

// One tag's payload, byte for byte what `nbt::serialize` writes. The chunk
// writer below never builds a tree, so this exists for the one thing in a
// column that already is one: a block entity, whose unmodelled fields the
// container layer must hand back exactly as they were read.
void write_nbt_value(mc::net::Writer& w, const nbt::Value& v) {
  switch (v.type) {
  case nbt::Tag::End:
    break;
  case nbt::Tag::Byte:
    w.u8(static_cast<uint8_t>(static_cast<int8_t>(v.i)));
    break;
  case nbt::Tag::Short:
    w.u16(static_cast<uint16_t>(static_cast<int16_t>(v.i)));
    break;
  case nbt::Tag::Int:
    w.u32(static_cast<uint32_t>(static_cast<int32_t>(v.i)));
    break;
  case nbt::Tag::Long:
    w.u64(static_cast<uint64_t>(v.i));
    break;
  case nbt::Tag::Float: {
    const float f = static_cast<float>(v.d);
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    w.u32(bits);
    break;
  }
  case nbt::Tag::Double: {
    const double d = v.d;
    uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof(bits));
    w.u64(bits);
    break;
  }
  case nbt::Tag::ByteArray:
    w.u32(static_cast<uint32_t>(v.bytes.size()));
    for (const int8_t b : v.bytes)
      w.u8(static_cast<uint8_t>(b));
    break;
  case nbt::Tag::String:
    write_nbt_name(w, v.s);
    break;
  case nbt::Tag::List:
    w.u8(static_cast<uint8_t>(v.list_type));
    w.u32(static_cast<uint32_t>(v.items.size()));
    for (const auto& item : v.items)
      write_nbt_value(w, item);
    break;
  case nbt::Tag::Compound:
    for (const auto& [name, field] : v.fields) {
      w.u8(static_cast<uint8_t>(field.type));
      write_nbt_name(w, name);
      write_nbt_value(w, field);
    }
    w.u8(0);
    break;
  case nbt::Tag::IntArray:
    w.u32(static_cast<uint32_t>(v.ints.size()));
    for (const int32_t x : v.ints)
      w.u32(static_cast<uint32_t>(x));
    break;
  case nbt::Tag::LongArray:
    w.u32(static_cast<uint32_t>(v.longs.size()));
    for (const int64_t x : v.longs)
      w.u64(static_cast<uint64_t>(x));
    break;
  }
}

// One palette entry: {Name: <state>, Properties: {k: v, ...}} with the
// properties in the canonical string's own (sorted) order. This splits the
// string the same way `serialize_chunk_nbt` rebuilds it, including its
// treatment of a malformed `k` with no `=`: the bytes have to be identical, and
// the string is the only form the chunk carries.
void write_block_palette_entry(mc::net::Writer& w, const BlockStateKey& key) {
  write_nbt_header(w, nbt::Tag::String, "Name");
  write_nbt_name(w, key.name);
  if (!key.properties.empty()) {
    write_nbt_header(w, nbt::Tag::Compound, "Properties");
    std::size_t pos = 0;
    while (pos < key.properties.size()) {
      const std::size_t comma = key.properties.find(',', pos);
      const std::size_t end = comma == std::string::npos ? key.properties.size() : comma;
      const std::string_view pair(key.properties.data() + pos, end - pos);
      const std::size_t eq = pair.find('=');
      if (eq != std::string_view::npos) {
        write_nbt_header(w, nbt::Tag::String, pair.substr(0, eq));
        write_nbt_name(w, pair.substr(eq + 1));
      }
      if (comma == std::string::npos)
        break;
      pos = comma + 1;
    }
    w.u8(0); // end Properties
  }
  w.u8(0); // end the palette entry
}

const char* heightmap_nbt_name(HeightmapType type) {
  switch (type) {
  case HeightmapType::WorldSurface:
    return "WORLD_SURFACE";
  case HeightmapType::OceanFloor:
    return "OCEAN_FLOOR";
  case HeightmapType::MotionBlocking:
    return "MOTION_BLOCKING";
  case HeightmapType::MotionBlockingNoLeaves:
    return "MOTION_BLOCKING_NO_LEAVES";
  }
  return "";
}

} // namespace

std::vector<uint8_t> serialize_chunk_nbt_bytes(const Chunk& chunk, int32_t data_version) {
  mc::net::Writer w;
  w.reserve(64 * 1024); // a decorated column's record is ~58 KiB; one growth
  w.u8(static_cast<uint8_t>(nbt::Tag::Compound));
  w.u16(0); // the root compound's name is empty

  write_nbt_header(w, nbt::Tag::Int, "DataVersion");
  w.u32(static_cast<uint32_t>(data_version));
  write_nbt_header(w, nbt::Tag::Int, "xPos");
  w.u32(static_cast<uint32_t>(chunk.x));
  write_nbt_header(w, nbt::Tag::Int, "yPos");
  w.u32(static_cast<uint32_t>(chunk.min_section_y));
  write_nbt_header(w, nbt::Tag::Int, "zPos");
  w.u32(static_cast<uint32_t>(chunk.z));
  write_nbt_header(w, nbt::Tag::String, "Status");
  write_nbt_name(w, "minecraft:full");
  write_nbt_header(w, nbt::Tag::Long, "LastUpdate");
  w.i64(0);
  write_nbt_header(w, nbt::Tag::Long, "InhabitedTime");
  w.i64(0);
  write_nbt_header(w, nbt::Tag::Byte, "isLightOn");
  w.u8(1);

  write_nbt_header(w, nbt::Tag::Compound, "Heightmaps");
  for (const auto& [type, longs] : chunk.heightmaps) {
    write_nbt_header(w, nbt::Tag::LongArray, heightmap_nbt_name(type));
    w.u32(static_cast<uint32_t>(longs.size()));
    w.i64s(longs.data(), longs.size());
  }
  w.u8(0); // end Heightmaps

  write_nbt_header(w, nbt::Tag::List, "sections");
  w.u8(static_cast<uint8_t>(nbt::Tag::Compound));
  w.u32(static_cast<uint32_t>(chunk.sections.size()));
  for (const Section& s : chunk.sections) {
    write_nbt_header(w, nbt::Tag::Byte, "Y");
    w.u8(static_cast<uint8_t>(static_cast<int8_t>(s.y)));

    if (s.has_blocks) {
      write_nbt_header(w, nbt::Tag::Compound, "block_states");
      write_nbt_header(w, nbt::Tag::List, "palette");
      w.u8(static_cast<uint8_t>(nbt::Tag::Compound));
      w.u32(static_cast<uint32_t>(s.block_palette.size()));
      for (const BlockStateKey& key : s.block_palette)
        write_block_palette_entry(w, key);
      const int bits = block_bits_for_palette_size(s.block_palette.size());
      if (bits > 0) {
        std::vector<int64_t> packed;
        pack_cells(s.block_cells, bits, &packed);
        write_nbt_header(w, nbt::Tag::LongArray, "data");
        w.u32(static_cast<uint32_t>(packed.size()));
        w.i64s(packed.data(), packed.size());
      }
      w.u8(0); // end block_states
    }

    if (s.has_biomes) {
      write_nbt_header(w, nbt::Tag::Compound, "biomes");
      write_nbt_header(w, nbt::Tag::List, "palette");
      w.u8(static_cast<uint8_t>(nbt::Tag::String));
      w.u32(static_cast<uint32_t>(s.biome_palette.size()));
      for (const std::string& name : s.biome_palette)
        write_nbt_name(w, name);
      const int bits = biome_bits_for_palette_size(s.biome_palette.size());
      if (bits > 0) {
        std::vector<int64_t> packed;
        pack_cells(s.biome_cells, bits, &packed);
        write_nbt_header(w, nbt::Tag::LongArray, "data");
        w.u32(static_cast<uint32_t>(packed.size()));
        w.i64s(packed.data(), packed.size());
      }
      w.u8(0); // end biomes
    }

    // Absent means no light, which is exactly the absence of light: the tree
    // serializer omits an all-zero array for the same reason.
    if (s.sky_light && !all_zero(*s.sky_light)) {
      write_nbt_header(w, nbt::Tag::ByteArray, "SkyLight");
      w.u32(static_cast<uint32_t>(s.sky_light->size()));
      w.bytes(s.sky_light->data(), s.sky_light->size());
    }
    if (s.block_light && !all_zero(*s.block_light)) {
      write_nbt_header(w, nbt::Tag::ByteArray, "BlockLight");
      w.u32(static_cast<uint32_t>(s.block_light->size()));
      w.bytes(s.block_light->data(), s.block_light->size());
    }
    w.u8(0); // end section
  }

  write_nbt_header(w, nbt::Tag::List, "block_entities");
  w.u8(static_cast<uint8_t>(nbt::Tag::Compound));
  w.u32(static_cast<uint32_t>(chunk.block_entities.size()));
  for (const nbt::Value& entity : chunk.block_entities)
    write_nbt_value(w, entity);

  write_nbt_header(w, nbt::Tag::List, "block_ticks");
  w.u8(static_cast<uint8_t>(nbt::Tag::Compound));
  w.u32(0);
  write_nbt_header(w, nbt::Tag::List, "fluid_ticks");
  w.u8(static_cast<uint8_t>(nbt::Tag::Compound));
  w.u32(0);
  write_nbt_header(w, nbt::Tag::List, "PostProcessing");
  w.u8(static_cast<uint8_t>(nbt::Tag::List));
  w.u32(0);

  write_nbt_header(w, nbt::Tag::Compound, "structures");
  write_nbt_header(w, nbt::Tag::Compound, "starts");
  w.u8(0);
  write_nbt_header(w, nbt::Tag::Compound, "References");
  w.u8(0);
  w.u8(0); // end structures

  w.u8(0); // end the root compound
  return std::move(w).take();
}

std::optional<std::vector<uint8_t>>
encode_chunk_body(const Chunk& chunk, const BlockStateTable& blocks, const BiomeNameTable& biomes,
                  LightMaskEncoding light_masks) {
  mc::net::Writer w;
  w.u32(static_cast<uint32_t>(chunk.x));
  w.u32(static_cast<uint32_t>(chunk.z));

  w.varint(3);
  for (const HeightmapType t : kClientHeightmaps) {
    const std::vector<int64_t>* data = find_heightmap(chunk, t);
    w.varint(static_cast<uint32_t>(t));
    if (data != nullptr) {
      w.varint(static_cast<uint32_t>(data->size()));
      w.i64s(data->data(), data->size());
    } else {
      w.varint(kHeightmapLongs);
      for (int i = 0; i < kHeightmapLongs; ++i)
        w.i64(0);
    }
  }

  std::vector<uint8_t> section_bytes;
  // Every section writes into `sec` and is appended here; the reserve keeps the
  // body from re-growing this staging buffer once per section.
  section_bytes.reserve(64 * 1024);
  // Section scratch, reused across the loop. Each iteration used to build five
  // palette-index vectors, an 8 KiB `Writer` and a packed-cell array from
  // scratch -- about 200 allocations for one 24-section body, all of them the
  // same handful of shapes. `clear()` plus `assign` keeps the capacity.
  std::vector<int32_t> block_ids;
  std::vector<int32_t> id_of_entry;
  std::vector<int32_t> cell_rank;
  std::vector<int32_t> per_entry_nonair;
  std::vector<int32_t> per_entry_fluid;
  std::vector<int32_t> biome_ids;
  std::vector<int32_t> biome_id_of_entry;
  std::vector<int32_t> biome_rank;
  std::vector<int64_t> packed_scratch;
  mc::net::Writer sec;
  sec.reserve(8 * 1024);
  for (const Section& s : chunk.sections) {
    sec.clear();

    int32_t non_air = 0;
    int32_t fluid = 0;
    if (s.has_blocks) {
      block_ids.clear();
      block_ids.reserve(s.block_palette.size());
      id_of_entry.assign(s.block_palette.size(), -1);
      cell_rank.assign(s.block_palette.size(), -1);
      per_entry_nonair.assign(s.block_palette.size(), 0);
      per_entry_fluid.assign(s.block_palette.size(), 0);
      for (size_t p = 0; p < s.block_palette.size(); ++p) {
        const auto id = blocks.lookup(s.block_palette[p]);
        if (!id)
          return std::nullopt;
        id_of_entry[p] = *id;
        int rank = -1;
        for (size_t q = 0; q < block_ids.size(); ++q) {
          if (block_ids[q] == *id) {
            rank = static_cast<int>(q);
            break;
          }
        }
        if (rank < 0) {
          rank = static_cast<int>(block_ids.size());
          block_ids.push_back(*id);
        }
        cell_rank[p] = rank;
        const BlockStateKey& key = s.block_palette[p];
        const bool air_like = key.name == "minecraft:air" || key.name == "minecraft:cave_air" ||
                              key.name == "minecraft:void_air";
        per_entry_nonair[p] = air_like ? 0 : 1;
        if (key.name == "minecraft:water" || key.name == "minecraft:lava" ||
            key.properties.find("waterlogged=true") != std::string::npos)
          per_entry_fluid[p] = 1;
      }
      if (block_ids.empty())
        return std::nullopt;
      const bool global = block_ids.size() > 256;
      const int bits = global ? kBlockGlobalBits : block_bits_for_palette_size(block_ids.size());
      if (bits < 0)
        return std::nullopt;
      if (bits == 0) {
        // One state for the whole section: the container is that id and there
        // is no array to write, so no cell is read at all. Vanilla's
        // single-value palette is `bits=0` followed by the value itself.
        sec.u8(0);
        sec.varint(static_cast<uint32_t>(block_ids[0]));
        non_air = per_entry_nonair[0] == 0 ? 0 : kBlockCellsPerSection;
        fluid = per_entry_fluid[0] == 0 ? 0 : kBlockCellsPerSection;
      } else if (!append_cells_single_pass(sec, s.block_cells, global ? id_of_entry : cell_rank,
                                           bits, block_ids, global, &per_entry_nonair,
                                           &per_entry_fluid, &non_air, &fluid, &packed_scratch)) {
        return std::nullopt;
      }
    } else {
      sec.u8(0);
      sec.varint(0);
    }

    if (s.has_biomes) {
      biome_ids.clear();
      biome_id_of_entry.assign(s.biome_palette.size(), -1);
      biome_rank.assign(s.biome_palette.size(), -1);
      for (size_t p = 0; p < s.biome_palette.size(); ++p) {
        const auto id = biomes.lookup(s.biome_palette[p]);
        if (!id)
          return std::nullopt;
        biome_id_of_entry[p] = *id;
        int r = -1;
        for (size_t q = 0; q < biome_ids.size(); ++q)
          if (biome_ids[q] == *id) {
            r = static_cast<int>(q);
            break;
          }
        if (r < 0) {
          r = static_cast<int>(biome_ids.size());
          biome_ids.push_back(*id);
        }
        biome_rank[p] = r;
      }
      if (biome_ids.empty())
        return std::nullopt;
      const bool global = biome_ids.size() > 8;
      const int bits = global ? bits_needed(biomes.registry_size())
                              : biome_bits_for_palette_size(biome_ids.size());
      if (bits == 0) {
        sec.u8(0);
        sec.varint(static_cast<uint32_t>(biome_ids[0]));
      } else if (bits < 1 || bits > 8 ||
                 !append_cells_single_pass(
                     sec, s.biome_cells, global ? biome_id_of_entry : biome_rank, bits, biome_ids,
                     global, nullptr, nullptr, nullptr, nullptr, &packed_scratch)) {
        return std::nullopt;
      }
    } else {
      sec.u8(0);
      sec.varint(0);
    }

    const uint16_t fluid_count = static_cast<uint16_t>(fluid > 0xFFFF ? 0xFFFF : fluid);
    const uint16_t block_count = static_cast<uint16_t>(non_air > 0xFFFF ? 0xFFFF : non_air);
    section_bytes.push_back(static_cast<uint8_t>(block_count >> 8));
    section_bytes.push_back(static_cast<uint8_t>(block_count));
    section_bytes.push_back(static_cast<uint8_t>(fluid_count >> 8));
    section_bytes.push_back(static_cast<uint8_t>(fluid_count));
    sec.append_to(&section_bytes);
  }
  w.varint(static_cast<uint32_t>(section_bytes.size()));
  w.bytes(section_bytes);
  w.varint(0);

  int highest_block_section = chunk.min_section_y - 2;
  bool any_disk_light = false;
  for (const Section& s : chunk.sections) {
    if (s.sky_light || s.block_light)
      any_disk_light = true;
    if (!s.has_blocks)
      continue;
    const bool air_only =
        std::all_of(s.block_palette.begin(), s.block_palette.end(), [](const BlockStateKey& k) {
          return k.name == "minecraft:air" || k.name == "minecraft:cave_air" ||
                 k.name == "minecraft:void_air";
        });
    if (!air_only && s.y > highest_block_section)
      highest_block_section = s.y;
  }
  const bool lit = highest_block_section >= chunk.min_section_y || any_disk_light;
  int64_t sky_real = 0, block_real = 0, sky_empty = 0, block_empty = 0;
  std::vector<const LightArray*> sky_arrays, block_arrays;
  if (lit) {
    const int min_light_y = chunk.min_section_y - 1;
    int top_light_y = highest_block_section + 1;
    // ...and as far up as the column holds *light*, whichever is higher. A
    // chunk read from disk carries light for the sections the file stored, which
    // is what the captured 26.2 body for the ow3 fixture shows (its 12 light
    // slots end exactly at one past the highest block section — a coincidence of
    // that chunk's save, and the reason the reference bytes pin the loop above).
    // A chunk the light engine *computed* holds an array for every one of its
    // sections, and its sky-lit air above the terrain is exactly what a client
    // needs to render: without this the sky above a column's terrain would read 0
    // on the client, and a block-less column (the in-memory world) would carry no
    // light at all.
    for (const Section& s : chunk.sections)
      if ((s.sky_light || s.block_light) && s.y > top_light_y)
        top_light_y = s.y;
    const int max_light_y = chunk.min_section_y + static_cast<int>(chunk.sections.size());
    if (top_light_y > max_light_y)
      top_light_y = max_light_y;
    const Section absent;
    for (int y = min_light_y; y <= top_light_y; ++y) {
      const int idx = y - chunk.min_section_y;
      const int slot = y - min_light_y;
      if (slot < 0 || slot > 63)
        continue;
      const uint64_t bit = 1ULL << slot;
      const Section& s = (idx >= 0 && idx < static_cast<int>(chunk.sections.size()))
                             ? chunk.sections[static_cast<size_t>(idx)]
                             : absent;
      if (s.sky_light && !all_zero(*s.sky_light)) {
        sky_real |= static_cast<int64_t>(bit);
        sky_arrays.push_back(&*s.sky_light);
      } else {
        sky_empty |= static_cast<int64_t>(bit);
      }
      if (s.block_light && !all_zero(*s.block_light)) {
        block_real |= static_cast<int64_t>(bit);
        block_arrays.push_back(&*s.block_light);
      } else {
        block_empty |= static_cast<int64_t>(bit);
      }
    }
  }
  auto emit_mask = [&](int64_t m) {
    if (m == 0) {
      w.varint(0);
    } else if (light_masks == LightMaskEncoding::ByteArray) {
      // 26.3 `ByteBufCodecs.BIT_SET` is `FriendlyByteBuf.writeByteArray(
      // value.toByteArray())`: a varint byte length, then the minimal
      // little-endian-bit-order bytes. A mask with bit 24 set is 4 bytes.
      const uint64_t bits = static_cast<uint64_t>(m);
      int length = 0;
      for (uint64_t v = bits; v != 0; v >>= 8)
        ++length;
      w.varint(static_cast<uint32_t>(length));
      for (int i = 0; i < length; ++i)
        w.u8(static_cast<uint8_t>((bits >> (8 * i)) & 0xFF));
    } else {
      w.varint(1);
      w.i64(m);
    }
  };
  emit_mask(sky_real);
  emit_mask(block_real);
  emit_mask(sky_empty);
  emit_mask(block_empty);
  auto emit_arrays = [&](const std::vector<const LightArray*>& arrs) {
    w.varint(static_cast<uint32_t>(arrs.size()));
    for (const auto* a : arrs) {
      w.varint(static_cast<uint32_t>(a->size()));
      w.bytes(a->data(), a->size());
    }
  };
  emit_arrays(sky_arrays);
  emit_arrays(block_arrays);

  return std::move(w).take();
}

BlockStateKey Chunk::block_state_at(int x, int y, int z) const {
  static const BlockStateKey kAir{"minecraft:air", ""};
  const CellIndex index = cell_index_at(x, y, z);
  if (index.section < 0 || index.palette < 0)
    return kAir;
  return sections[static_cast<size_t>(index.section)]
      .block_palette[static_cast<size_t>(index.palette)];
}

bool chunk_has_light(const Chunk& chunk) {
  for (const Section& s : chunk.sections)
    if (s.sky_light || s.block_light)
      return true;
  return false;
}

void chunk_recount_random_tick_blocks(Chunk& chunk) {
  for (Section& s : chunk.sections) {
    if (!s.has_blocks || s.block_palette.empty()) {
      s.random_tick_blocks = 0;
      continue;
    }
    // One predicate call per *palette entry* rather than per cell: the palette
    // is the distinct states of the section (a few dozen at most), the cells
    // are 4096.
    std::vector<char> ticking(s.block_palette.size(), 0);
    bool any_ticking = false;
    for (size_t i = 0; i < s.block_palette.size(); ++i) {
      ticking[i] = block_is_randomly_ticking(s.block_palette[i]) ? 1 : 0;
      any_ticking = any_ticking || ticking[i] != 0;
    }
    // The common section holds no ticking state at all — stone, deepslate, air,
    // water, a single wood type — and then no cell can tick, so the 4096-cell
    // walk answers 0 without happening. This is the load path's share of a cold
    // read (every section of a parsed column asks), so it is worth the one flag.
    int count = 0;
    if (!any_ticking) {
      s.random_tick_blocks = 0;
      continue;
    }
    if (s.block_cells.uniform()) {
      // One state in every cell: the count is all of them or none of them, which
      // is the whole section for an air or stone column and costs one lookup
      // instead of 4096.
      const int32_t index = s.block_cells.uniform_value();
      if (index >= 0 && static_cast<size_t>(index) < ticking.size() &&
          ticking[static_cast<size_t>(index)] != 0)
        count = static_cast<int>(s.block_cells.size());
    } else {
      for (size_t i = 0; i < s.block_cells.size(); ++i) {
        const int32_t index = s.block_cells[i];
        if (index >= 0 && static_cast<size_t>(index) < ticking.size() &&
            ticking[static_cast<size_t>(index)] != 0)
          ++count;
      }
    }
    s.random_tick_blocks = count;
  }
}

bool Chunk::set_block_state(int x, int y, int z, const BlockStateKey& key) {
  if (x < 0 || x >= 16 || z < 0 || z >= 16)
    return false;
  auto floor_div4 = [](int v) -> int {
    int q = v / 16;
    if ((v % 16 != 0) && (v < 0))
      --q;
    return q;
  };
  const int sec_y = floor_div4(y);
  const int sec_idx = sec_y - min_section_y;
  if (sec_idx < 0 || sec_idx >= static_cast<int>(sections.size()))
    return false;
  Section& s = sections[static_cast<size_t>(sec_idx)];
  if (!s.has_blocks) {
    s.has_blocks = true;
    s.block_palette = {{"minecraft:air", ""}};
    s.block_cells.assign(kBlockCellsPerSection, 0);
  }
  const int local_y = static_cast<int>(static_cast<unsigned>(y) & 0xFU);
  const int cell = ((local_y * 16) + z) * 16 + x;
  if (cell < 0 || cell >= kBlockCellsPerSection)
    return false;
  // The state this cell held, read before the palette can grow (a push_back may
  // reallocate it), for the section's ticking count.
  const int32_t old_palette_idx = s.block_cells[static_cast<size_t>(cell)];
  const bool has_old =
      old_palette_idx >= 0 && static_cast<size_t>(old_palette_idx) < s.block_palette.size();
  const bool old_ticking =
      has_old && block_is_randomly_ticking(s.block_palette[static_cast<size_t>(old_palette_idx)]);
  const bool new_ticking = block_is_randomly_ticking(key);
  // Heightmaps move with the write only lazily: mark the column where this write
  // can move a height, and let the next reader (or the copy that escapes a
  // store, or the flush's serializer) recompute once for all the writes since.
  if (has_old &&
      heightmap_predicates_differ(s.block_palette[static_cast<size_t>(old_palette_idx)], key))
    mark_heightmap_dirty(x, z, y);
  int palette_idx = -1;
  for (size_t i = 0; i < s.block_palette.size(); ++i) {
    if (s.block_palette[i] == key) {
      palette_idx = static_cast<int>(i);
      break;
    }
  }
  if (palette_idx < 0) {
    palette_idx = static_cast<int>(s.block_palette.size());
    s.block_palette.push_back(key);
  }
  s.block_cells.set(static_cast<size_t>(cell), palette_idx);
  // `LevelChunkSection.setBlockState`'s tickingBlockCount: the old state stops
  // ticking and the new one starts (or the same one does neither).
  if (old_ticking != new_ticking)
    s.random_tick_blocks += new_ticking ? 1 : -1;
  // Heightmaps: marked above only where a write can move one. The rule's two
  // public ways in are `height_at` (a read) and `snapshot` (a copy).
  return true;
}

void Chunk::mark_heightmap_dirty(int x, int z, int y) {
  if (x < 0 || x >= 16 || z < 0 || z >= 16)
    return;
  // The heightmap's own relative code for the cell that moved: the refresh
  // starts its search there, so the record is "how high up did a write reach",
  // not just "this column is behind".
  const int rel = y + 1 - min_section_y * 16;
  if (rel <= 0) // below the column: cannot move a height
    return;
  int16_t& top = heightmap_dirty_top[static_cast<size_t>(z * 16 + x)];
  if (rel > top)
    top = static_cast<int16_t>(rel);
}

void Chunk::refresh_heightmap_column(int x, int z) {
  if (x < 0 || x >= 16 || z < 0 || z >= 16)
    return;
  int16_t& top = heightmap_dirty_top[static_cast<size_t>(z * 16 + x)];
  if (top == 0)
    return; // clean: one load, no work
  const int changed_top = top;
  top = 0;
  recompute_column_heightmaps(*this, x, z, changed_top);
}

void Chunk::refresh_heightmaps() {
  // The clean columns cost one load each, so the whole-chunk form is the
  // per-column form 256 times — one implementation of the rule, two ways in.
  for (int i = 0; i < 256; ++i)
    refresh_heightmap_column(i & 15, i >> 4);
}

void Chunk::rebuild_after_bulk_write(const int16_t* column_tops) {
  // A bulk write reaches every column, so every column is behind: record the
  // top of the column as the highest cell that moved, the same record a single
  // `set_block_state` makes through `mark_heightmap_dirty`. The refresh then
  // starts its search there — and on a column whose array does not exist yet,
  // `recompute_column_heightmaps` creates it — exactly as it would after a
  // series of writes, so a generated column carries the same heightmaps a
  // hand-built one does. `column_tops` (from a writer that knows them) only
  // narrows where the search starts; a `0` there is a column of air, and the
  // `1` keeps the refresh running so the arrays are created as before.
  const int height = static_cast<int>(sections.size()) * 16;
  for (int i = 0; i < 256; ++i) {
    int16_t top = static_cast<int16_t>(height);
    if (column_tops != nullptr) {
      top = column_tops[i];
      if (top < 1)
        top = 1;
    }
    heightmap_dirty_top[static_cast<size_t>(i)] = top;
  }
  refresh_heightmaps();
  // The ticking counts are the write path's other derived store; a bulk write
  // has no per-cell accounting of them either.
  chunk_recount_random_tick_blocks(*this);
}

// The column's own footprint, in round numbers of bytes (chunk.h says what is
// counted and why an estimate is what this can be). The three string-shaped
// parts are counted by their *contents*, because that is what the allocation
// costs a short-string-buffer-past name; the fixed per-entry term is
// `sizeof(std::string)`, so a palette full of "minecraft:air" is not free.
std::size_t Chunk::estimated_bytes() const {
  std::size_t bytes = sizeof(Chunk);
  for (const Section& section : sections) {
    bytes += section.block_cells.bytes();
    bytes += section.biome_cells.bytes();
    for (const BlockStateKey& state : section.block_palette)
      bytes += sizeof(BlockStateKey) + state.name.size() + state.properties.size();
    for (const std::string& biome : section.biome_palette)
      bytes += sizeof(std::string) + biome.size();
    // A uniform array costs the column nothing (world/chunk.h's `LightArray`):
    // the bytes a reader sees come from the process-wide table, not from here.
    if (section.sky_light)
      bytes += section.sky_light->storage_bytes();
    if (section.block_light)
      bytes += section.block_light->storage_bytes();
  }
  for (const auto& [type, values] : heightmaps)
    bytes += sizeof(type) + values.size() * sizeof(int64_t) +
             sizeof(std::pair<HeightmapType, std::vector<int64_t>>);
  bytes += block_entities.size() * (sizeof(nbt::Value) + 32);
  return bytes;
}

// The one copy boundary every store uses: the copy carries the refresh, so the
// column it came from keeps whatever marks it had.
Chunk Chunk::snapshot() const {
  Chunk copy = *this;
  copy.refresh_heightmaps();
  return copy;
}

// The column's stored entry, decoded at the bit width `recompute_column_heightmaps`
// writes with.
static int chunk_heightmap_height(const Chunk& chunk, HeightmapType type, int x, int z) {
  const int min_y = chunk.min_section_y * 16;
  if (x < 0 || x >= 16 || z < 0 || z >= 16)
    return min_y;
  const std::vector<int64_t>* longs = find_heightmap(chunk, type);
  if (longs == nullptr || longs->empty())
    return min_y;
  // The stored entry is the height relative to min_y (0 = nothing in the
  // column), at the same bit width `recompute_column_heightmaps` writes with.
  const int height = static_cast<int>(chunk.sections.size()) * 16;
  const int bits = heightmap_bits_for_height(height);
  const int idx = z * 16 + x;
  return min_y + chunk_heightmap_value(*longs, idx, bits);
}

// The one height-read rule: bring the column current, then answer from it. There
// is no const form to bind by accident (see the note in chunk.h) — a caller that
// holds the column const has to say so at the borrow boundary instead, where the
// refresh belongs — and no second caller of the refresh, because the primitives
// are private and this is the only public way in.
int Chunk::height_at(HeightmapType type, int x, int z) {
  refresh_heightmap_column(x, z); // a write only marks the column: bring it current
  return chunk_heightmap_height(*this, type, x, z);
}

int chunk_motion_blocking_height(Chunk& chunk, int x, int z) {
  return chunk.height_at(HeightmapType::MotionBlocking, x, z);
}

int chunk_world_surface_height(Chunk& chunk, int x, int z) {
  return chunk.height_at(HeightmapType::WorldSurface, x, z);
}

std::string chunk_biome_at(const Chunk& chunk, int x, int y, int z) {
  static const std::string kVoidBiome = "minecraft:the_void";
  if (x < 0 || x >= 16 || z < 0 || z >= 16)
    return kVoidBiome;
  const int section_number = y >= 0 ? y / 16 : (y - 15) / 16;
  const int sec_idx = section_number - chunk.min_section_y;
  if (sec_idx < 0 || sec_idx >= static_cast<int>(chunk.sections.size()))
    return kVoidBiome;
  const Section& s = chunk.sections[static_cast<size_t>(sec_idx)];
  if (!s.has_biomes || s.biome_palette.empty())
    return kVoidBiome;
  // `PalettedContainer`'s biome index: half resolution, x innermost
  // ((y << 4) | (z << 2) | x).
  const int cell = (((y & 15) >> 2) * 16) + (((z & 15) >> 2) * 4) + ((x & 15) >> 2);
  if (cell < 0 || cell >= kBiomeCellsPerSection ||
      s.biome_cells.size() < static_cast<size_t>(kBiomeCellsPerSection))
    return kVoidBiome;
  const int32_t palette_idx = s.biome_cells[static_cast<size_t>(cell)];
  if (palette_idx < 0 || static_cast<size_t>(palette_idx) >= s.biome_palette.size())
    return kVoidBiome;
  return s.biome_palette[static_cast<size_t>(palette_idx)];
}

} // namespace mc::world
