// SPDX-License-Identifier: GPL-3.0-or-later
//
// In-memory chunk model and the clientbound chunk-body encoder for 26.2
// (play 0x2d "level chunk with light").
//
// Disk NBT (see world/nbt.h) is parsed into a Chunk holding the raw sections,
// heightmaps and light arrays; encode_chunk_body() turns that into the exact
// wire bytes a vanilla 26.2 server would send for the same chunk. The encoder
// re-packs section containers from their expanded cells the same way vanilla
// does (first-appearance palette order, LSB-first packing, per-registry bit
// widths from the decompiled Strategy/Configuration), so a chunk loaded from
// a vanilla-generated region reproduces the captured wire bytes.
//
// Milestone E adds the reverse: serialize_chunk_nbt() packs an in-memory
// Chunk back to the Value tree that the region writer (AnvilWorld) compresses
// and stores. The codec is the exact inverse of parse_chunk_nbt() so a
// parse → serialize → parse round-trip is identity for every section layout
// (singleton, linear palettes at 1..8 bits, and direct/global where the
// encoder already handles >256 states — though the disk format linearizes
// before saving, so >8-bit palettes still map to a linear palette + data
// longs on disk).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/slab_pool.h"
#include "world/nbt.h"
#include "world/section_cells.h"

namespace mc::world {

// A section's light array — 4,096 nibbles, 2,048 bytes — **or**, when every
// cell of the array holds the same level, just that level and no buffer at all
// (derived light: the open-air sky of a generated column is one repeated byte,
// and it is the majority of the arrays a lit column used to hold).
//
// The buffer, when there is one, is stored in a pool that packs two arrays into
// one 4 KiB page and unmaps the page when both are free (`core/slab_pool.h` says
// why, and measures the alternative).
//
// The uniform case is invisible to every reader: `size()` is still 2,048 and
// `data()`/`begin()`/`end()` answer a pointer into a process-wide table of the
// sixteen possible uniform patterns, so `light_nibble`, the packet encoders and
// the Anvil writer take the same bytes they always took and allocate nothing for
// them. What changes is the column's own footprint (`storage_bytes()` is 0 for a
// uniform array) and the write-out in world/light.cpp, which produces a uniform
// array directly instead of filling one. A caller that needs to *write* into the
// array must not do it through `begin()` on a uniform one — the table is shared
// and read-only — so the write path is `assign_from` (which re-detects
// uniformity) rather than a raw copy.
class LightArray {
public:
  using Buffer = std::vector<uint8_t, mc::core::LightSlabAllocator<uint8_t>>;
  static constexpr std::size_t kBytes = 2048;

  LightArray() = default;

  // A real buffer of `count` bytes, always non-uniform: this is the shape the
  // caller asked for (the slab-pool test asks for a 2 KiB slot and checks the
  // page arithmetic), and compressing it to uniform would change what it means.
  LightArray(std::size_t count, uint8_t value) : bytes_(std::make_shared<Buffer>(count, value)) {}

  // From raw bytes (a disk array, a computed write-out, a decoded packet): a run
  // that is one repeated byte becomes uniform, everything else is copied.
  template <typename It> LightArray(It first, It last) {
    assign_from(first, last);
  }

  static LightArray uniform(uint8_t level) {
    LightArray out;
    out.is_uniform_ = true;
    out.uniform_ = static_cast<uint8_t>(level & 0x0F);
    return out;
  }

  bool uniform() const noexcept {
    return is_uniform_;
  }
  uint8_t uniform_level() const noexcept {
    return uniform_;
  }

  std::size_t size() const noexcept {
    return is_uniform_ ? kBytes : (bytes_ ? bytes_->size() : 0);
  }
  bool empty() const noexcept {
    return size() == 0;
  }
  const uint8_t* data() const noexcept {
    return (bytes_ && !is_uniform_) ? bytes_->data() : uniform_bytes(uniform_);
  }
  const uint8_t* begin() const noexcept {
    return data();
  }
  const uint8_t* end() const noexcept {
    return data() + size();
  }

  // What this array costs the column: nothing when uniform, the buffer's own
  // bytes otherwise (the store's budget counts this). A shared buffer is counted
  // once per array that names it, like the estimate it feeds: the budget only
  // has to move when the column does.
  std::size_t storage_bytes() const noexcept {
    return (is_uniform_ || !bytes_) ? 0 : bytes_->size();
  }

  // The one write path: replace the content with [first, last), re-detecting
  // uniformity. The buffer is copy-on-write — a `Chunk` copy (every
  // `load_chunk`) shares the arrays instead of copying 2 KiB each, and a relight
  // that rewrites an array with this one as its only owner writes into the
  // buffer it already has. A rewrite of a shared array detaches first, so the
  // copy the caller already handed out keeps the light it was handed.
  template <typename It> void assign_from(It first, It last) {
    const std::size_t count = static_cast<std::size_t>(std::distance(first, last));
    if (count == 0) {
      bytes_.reset();
      is_uniform_ = false;
      uniform_ = 0;
      return;
    }
    const uint8_t first_byte = *first;
    bool same = true;
    for (It it = first; it != last; ++it) {
      if (*it != first_byte) {
        same = false;
        break;
      }
    }
    // One repeated byte is a uniform array only when its two nibbles agree:
    // 0x0F is a level-0 cell beside a level-15 one, not a uniform level.
    if (same && (first_byte >> 4) == (first_byte & 0x0F)) {
      is_uniform_ = true;
      uniform_ = static_cast<uint8_t>(first_byte & 0x0F);
      bytes_.reset();
      return;
    }
    is_uniform_ = false;
    if (bytes_ && bytes_.use_count() == 1 && bytes_->size() == count) {
      std::copy(first, last, bytes_->begin());
    } else {
      bytes_ = std::make_shared<Buffer>(first, last);
    }
  }

  // Nibble-wise equality: a uniform array equals the byte array that spells the
  // same levels, which is what makes a relight's diff (world_light.cpp) report
  // nothing when a section's light changed representation but not content. Two
  // arrays over the same buffer are equal without looking at it, which is the
  // common case after a copy.
  bool operator==(const LightArray& other) const {
    if (is_uniform_ && other.is_uniform_)
      return uniform_ == other.uniform_;
    if (is_uniform_)
      return other.all_bytes_equal_to(static_cast<uint8_t>(uniform_ | (uniform_ << 4)));
    if (other.is_uniform_)
      return all_bytes_equal_to(static_cast<uint8_t>(other.uniform_ | (other.uniform_ << 4)));
    if (bytes_ == other.bytes_)
      return true;
    if (!bytes_ || !other.bytes_)
      return size() == other.size();
    return *bytes_ == *other.bytes_;
  }
  bool operator!=(const LightArray& other) const {
    return !(*this == other);
  }

private:
  bool all_bytes_equal_to(uint8_t byte) const {
    if (is_uniform_)
      return uniform_ == (byte & 0x0F);
    if (!bytes_)
      return false;
    for (const uint8_t b : *bytes_)
      if (b != byte)
        return false;
    return true;
  }

  // The sixteen uniform patterns as real bytes, built once: a reader that needs
  // the array as bytes gets them without an allocation and without the writer
  // ever materialising one. Plain heap (not the light pool) on purpose — this is
  // one 32 KiB table for the whole process, not a per-column cost.
  static const uint8_t* uniform_bytes(uint8_t level) {
    static const std::array<std::vector<uint8_t>, 16> table = [] {
      std::array<std::vector<uint8_t>, 16> out;
      for (int level = 0; level < 16; ++level) {
        const uint8_t byte = static_cast<uint8_t>(level | (level << 4));
        out[static_cast<std::size_t>(level)] = std::vector<uint8_t>(kBytes, byte);
      }
      return out;
    }();
    return table[static_cast<std::size_t>(level & 0x0F)].data();
  }

  std::shared_ptr<Buffer> bytes_;
  bool is_uniform_ = false;
  uint8_t uniform_ = 0;
};

// Heightmap types kept on disk. Only the CLIENT subset (WORLD_SURFACE,
// MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES) goes on the wire.
enum class HeightmapType : uint8_t {
  WorldSurface = 1,
  OceanFloor = 3, // on disk (LIVE_WORLD), never sent
  MotionBlocking = 4,
  MotionBlockingNoLeaves = 5,
};

// A block state as stored in a section palette: name + flattened properties.
// The property string is the canonical sorted "k=v,k=v" form (disk order
// normalized to sorted keys at parse time); resolution to a network id is the
// registry's job (see world/block_states.h).
struct BlockStateKey {
  std::string name;       // "minecraft:grass_block"
  std::string properties; // "snowy=false" (empty when none)

  bool operator==(const BlockStateKey&) const = default;
};

// A section's block cells: 4096 palette indices, stored packed — 4, 8 or 32 bits
// each, or as no buffer at all when every cell is the same state
// (world/section_cells.h is the type and its measurement; this used to be a flat
// `int32_t` per cell, 384 KiB per full-height column and the largest single item
// in the process).
//
// The buffer it does own comes from `core/page_alloc.h`'s page-granular
// allocator, because a released column's bulk buffers have to be *returnable* to
// the operating system, and a shared-heap block cannot be given back whole once
// the store's own churn has left live allocations in every region around it
// (measured; that header says what the unload returned before this change and
// why). Sub-page buffers keep the default allocator on purpose — mapping a
// 2048-byte light array alone would cost a whole resident page to save half of
// one — and the packed widths land on both sides of that cutoff by design.

// One 16x16x16 section of a chunk column. Cells are palette indices expanded
// from the disk container at parse time: 4096 for blocks (y-major: index =
// (y*16+z)*16+x), 64 for biomes (4^3, same indexing at half resolution).
struct Section {
  int y = 0; // section Y coordinate (e.g. -4..19 for the overworld)

  // Block-state container. `has_blocks` is false when the disk section
  // omitted block_states entirely (pure air column).
  bool has_blocks = false;
  std::vector<BlockStateKey> block_palette;
  SectionCells block_cells; // 4096 palette indices, packed (world/section_cells.h)

  // Biome container (4x4x4 = 64 cells). The cells use the same packed
  // container as the blocks (`SectionCells`): 64 indices at 4 bits is 32 bytes
  // where the old `std::vector<int32_t>` was 256, the second-largest per-section
  // buffer in a column and one that every `load_chunk` copy paid for. The
  // palette is at most a handful of entries, so 4 bits is also the width
  // vanilla's own biome container would pick for a linear palette.
  bool has_biomes = false;
  std::vector<std::string> biome_palette;
  SectionCells biome_cells; // 64 palette indices, packed

  // Light data: present when the section carries a 2048-byte array, and stored
  // two-arrays-to-a-page (`LightArray` above, `core/slab_pool.h`).
  std::optional<LightArray> sky_light;
  std::optional<LightArray> block_light;

  // How many of the 4096 cells hold a randomly-ticking block — `LevelChunkSection
  // .tickingBlockCount`, which is what `isRandomlyTicking()` reads and therefore
  // what lets the random-tick pass skip a section of stone without reading it.
  // Maintained by `Chunk::set_block_state` and counted by `parse_chunk_nbt`
  // (world/random_tick.h's `block_is_randomly_ticking` answers per *state*, so a
  // mature crop does not count and a leaf at distance 7 does).
  int random_tick_blocks = 0;
};

// One cell per 16x16x16 block section.
constexpr int kBlockCellsPerSection = 4096;
// One cell per 4x4x4 biome section.
constexpr int kBiomeCellsPerSection = 64;

struct Chunk {
  int x = 0;
  int z = 0;
  // The store's LRU stamp for this column (`MemoryWorld`): the tick of the last
  // read or borrow. It lives on the column rather than in a side map so a read
  // costs one hash lookup instead of two — the hit it already made, and a
  // second node-map access whose only job was to record that the hit happened.
  // `mutable` because a read *is* what moves it, and reads take a const column.
  // It is bookkeeping, not content: the encoders, the writer and every equality
  // comparison of a column's data ignore it (a `snapshot` copy carries whatever
  // it had, which is harmless).
  mutable uint64_t lru_stamp = 0;
  // Sections in ascending Y order. The dimension's min section Y is stored so
  // section[i].y == min_y + i; gaps are filled with all-air sections.
  int min_section_y = 0;
  std::vector<Section> sections;

  // Heightmaps keyed by HeightmapType (disk raw long arrays; the wire takes
  // only the CLIENT subset).
  std::vector<std::pair<HeightmapType, std::vector<int64_t>>> heightmaps;
  // Columns whose heightmaps are behind their blocks, and *how far* behind: the
  // highest cell a heightmap-moving write touched since the column was last
  // brought current, in the heightmap's own relative code (`y + 1 - min_y`), or
  // 0 for a clean column. `set_block_state` records it only when a heightmap
  // predicate (all name-based, see `is_opaque_for`) would answer differently.
  // The heightmaps are a cache of the blocks and this is the only record of what
  // the cache is behind on, so the *one* rule for bringing one current lives
  // here (mark + `refresh_heightmap_*`, below) and every reader goes through the
  // query helpers, which apply it. The two places a *copy* leaves a store
  // (`load_chunk` in both of them, and the region flush's serializer snapshot)
  // apply it too, so the arrays the wire encoder reads are current as well.
  //
  // The recorded cell is what makes the refresh cheap: the cached heights are the
  // *previous* answer, so a cell that did not change cannot move one, and the
  // search starts at the recorded cell instead of at the top of the world — a
  // column whose surface sits 250 cells under the section ceiling used to walk
  // those 250 air cells four times over on every refresh, and the light ring
  // refreshes its whole 3x3 region on every relight. Index z*16+x; a clean
  // column costs one load.
  std::array<int16_t, 256> heightmap_dirty_top{};

  // Block entities, kept as the *verbatim* NBT compound the region record
  // carried. Container contents ("Items") are the only part the server reads
  // and writes; every other field — sign text, spawner data, ... — survives a
  // load/save round-trip untouched. Milestone C owns the container subset
  // (see world/container.h); the chunk encoder deliberately sends an empty
  // block-entity list on the wire, because the client creates its own block
  // entity from the block state and gets contents from container_set_content.
  std::vector<nbt::Value> block_entities;

  // Mutate a single block in the chunk (in-memory only; Anvil write arrives
  // with milestone E). Returns false when (x,y,z) is out of column range.
  // The block is set to `key`; the palette is extended if needed.
  bool set_block_state(int x, int y, int z, const BlockStateKey& key);

  // The one way a column leaves a store: a copy whose heightmaps are current
  // with its blocks. Every boundary a column crosses — `load_chunk` in both
  // world stores, and the flush's serializer copy — is this call, so a new
  // store or a new copy point cannot skip the refresh the way a hand-written
  // call could (and forgetting is the shape of the read bug this rule came from).
  // Const, because the caller gets a *copy*: the refresh lands on that copy and
  // the column it came from is left exactly as it was.
  Chunk snapshot() const;

  // Roughly what this column's own buffers cost, in bytes — the unit the
  // store's *byte* budget is kept in (world/memory_world.h's `ChunkBudget`).
  //
  // It counts what this object allocates for itself: the per-section block
  // cells and palettes, the biome cells and palettes, the light arrays, the
  // heightmap long arrays, and the block-entity list's own entries (their NBT
  // payloads are not walked — the list is the server's, the trees inside it are
  // the file's). Section headers and the vector's own slack are not counted.
  //
  // An *estimate* is deliberate. Exact accounting needs per-allocation
  // bookkeeping and a stable allocator, and the budget does not need to be
  // exact: it needs to be a bound a host can plan with and a number that moves
  // when the column does, in the direction it moves. A column with its light
  // arrays is ~5x what the same cells cost without them (measured: ~460 KB
  // against ~95 KB for a full-height overworld column, docs/PHASE5.md §4), so a
  // budget that ignored them would be off by the factor that matters most.
  std::size_t estimated_bytes() const;

  // The bulk-write counterpart of `set_block_state`, for a caller that writes
  // whole sections directly instead of one cell at a time (the world
  // generator's fill — 98k `set_block_state` calls per column is not a price a
  // join can pay). It brings the column to exactly the state that series of
  // writes would have left it in: every column's heightmaps current with the
  // blocks and every section's `random_tick_blocks` recounted. The per-cell
  // marks are the lazy version of the first half (`heightmap_dirty_top`), so
  // this is those marks made all at once — one pass over 256 columns rather
  // than one per cell — followed by the same refresh every reader uses.
  //
  // `column_tops` is the caller's knowledge of the highest non-air cell in each
  // column, relative (`y + 1 - min_section_y * 16`, indexed `z * 16 + x`). A
  // bulk writer that walked every cell it wrote already knows it, and no
  // heightmap can sit above it (its answer is an opaque cell, hence non-air), so
  // the refresh starts there instead of at the top of the section stack. Null
  // is the old shape: every column starts at the column's full height.
  void rebuild_after_bulk_write(const int16_t* column_tops = nullptr);

  // The one height read: bring the column current and answer from it. Local x/z
  // are 0..15; `chunk_motion_blocking_height` / `chunk_world_surface_height`
  // below are the named questions callers ask, and both are this function.
  int height_at(HeightmapType type, int x, int z);

  // Reads the block state at (x, y, z) with local x/z in 0..15. Returns a
  // synthetic air state for cells in an all-air section (which store no
  // block_states container) and for coordinates outside the column.
  BlockStateKey block_state_at(int x, int y, int z) const;

  // Where a cell's state lives, for a caller that memoizes an answer per
  // *palette entry* instead of asking per cell: the section's index in
  // `sections`, and the palette index within it. Both are -1 for a cell outside
  // the column or in a section carrying no block container (which reads air).
  // This is the cheap half of `block_state_at`, and what that function is
  // written in terms of, so the two cannot drift apart.
  struct CellIndex {
    int section = -1;
    int palette = -1;
  };
  // Inline, not merely because it is small: the light engine calls it once per
  // cell of a 48x48x384 region (hundreds of thousands of times per relight),
  // and as an out-of-line function in chunk.cpp every one of those was a call
  // with its own argument setup and no chance to hoist the section index out of
  // a row or a column walk. `sample` on the relight put it at roughly a fifth of
  // the whole bench's leaf samples. The body is unchanged, checks included.
  CellIndex cell_index_at(int x, int y, int z) const {
    if (x < 0 || x >= 16 || z < 0 || z >= 16)
      return {};
    const int sec_y = (y >= 0) ? y / 16 : (y - 15) / 16;
    const int sec_idx = sec_y - min_section_y;
    if (sec_idx < 0 || sec_idx >= static_cast<int>(sections.size()))
      return {};
    const Section& s = sections[static_cast<size_t>(sec_idx)];
    if (!s.has_blocks || s.block_palette.empty() || s.block_cells.empty())
      return {sec_idx, -1};
    const int local_y = static_cast<int>(static_cast<unsigned>(y) & 0xFU);
    const int cell = ((local_y * 16) + z) * 16 + x;
    if (cell < 0 || static_cast<size_t>(cell) >= s.block_cells.size())
      return {sec_idx, -1};
    const int pi = s.block_cells[static_cast<size_t>(cell)];
    if (pi < 0 || static_cast<size_t>(pi) >= s.block_palette.size())
      return {sec_idx, -1};
    return {sec_idx, pi};
  }

private:
  // The rule's primitives. Private on purpose: a column leaves a store through
  // `snapshot` and a read asks through `height_at`, so no caller can re-derive
  // the refresh — or forget it — the way the three hand-written sites and the
  // two overloads these replaced could. `set_block_state` is the one write path
  // that marks a column; every predicate is name-based, so it marks only where a
  // height can really move (`heightmap_predicates_differ`).
  void mark_heightmap_dirty(int x, int z, int y);
  void refresh_heightmap_column(int x, int z);
  void refresh_heightmaps();
};

// Does this chunk carry light of its own — a 2048-byte light array in any of
// its sections? A column read from disk answers with whatever the file stored
// (`SerializableChunkData` writes only the sections that had light); a column
// this server created answers false until the light engine computes it
// (world/world_light.h owns that step). An all-zero array counts as absent:
// the encoder writes both as an empty section, so nothing downstream can tell
// them apart.
bool chunk_has_light(const Chunk& chunk);

// Rebuilds every section's `random_tick_blocks` count from its cells. The parse
// path calls this for each section it decodes (a column from disk has no
// counter to trust), and it is what a test that builds a section by hand wants
// before asking `WorldInterface::random_tick_sections` about it.
void chunk_recount_random_tick_blocks(Chunk& chunk);

// Extracts a Chunk from parsed chunk NBT. `min_section_y`/`section_count` come
// from the dimension type (overworld: -4 and 24). Returns false when the NBT
// lacks the expected shape (xPos/zPos, sections). Gaps in the section list
// are filled with empty sections so the column always spans the full range.
bool parse_chunk_nbt(const nbt::Value& root, int min_section_y, int section_count, Chunk* out);

// The same parse, straight from the decompressed record bytes.
//
// `parse_nbt_to_chunk` used to build the whole `nbt::Value` tree first and walk
// it for the dozen fields a chunk has, which was 0.56 ms of a 1.47 ms cold
// column against a 4.7 KB record: a vector and a string for every tag in a
// 58 KB payload, thrown away once the column was in `Chunk` form. This reader
// checks bounds at every step, skips fields it does not know (a newer format's,
// a mod's) by their declared size, and fills the same `Chunk` the tree path
// does — palette canonicalization, `assign_packed` unpacking and the
// random-tick recount included. Returns false on anything malformed, and the
// caller falls back to the tree path for shapes this one does not model.
bool parse_chunk_nbt_direct(const uint8_t* data, std::size_t len, int min_section_y,
                            int section_count, Chunk* out);

// Serializes `chunk` to the Value tree that the region writer stores (the
// root compound of a region record). Inverse of parse_chunk_nbt(): the
// returned tree, when compressed and read back, parses to an identical Chunk.
// Big-endian, same palette/data geometry as the disk format (SimpleBitStorage).
// `data_version` is written as the root int (5023 for 26.3); callers that
// load from fixture files should pass the fixture's DataVersion.
nbt::Value serialize_chunk_nbt(const Chunk& chunk, int32_t data_version = 5023);

// The same record as bytes, without the Value tree in between.
//
// A save is the inverse of a load and used to pay for both halves of the tree:
// `serialize_chunk_nbt` builds a `nbt::Value` for every tag (three vectors and a
// string per section palette, per heightmap, per light array), then
// `nbt::serialize` walks it into bytes. This writes the chunk's own fields in
// the format's order and encoding directly — the same bytes, which the region
// writer's own round-trip and the fixture tests compare — and only the block
// entities still go through a `Value`, because those are a `Value` in the chunk
// (a container's unmodelled fields survive exactly as they were read).
std::vector<uint8_t> serialize_chunk_nbt_bytes(const Chunk& chunk, int32_t data_version = 5023);

// Registry lookups used at encode time.
class BlockStateTable {
public:
  virtual ~BlockStateTable() = default;
  // Network id for a block state; std::nullopt when unknown (with the full
  // 26.2 table wired in, every vanilla state resolves; an unknown one voids
  // the chunk rather than emitting a wrong id).
  virtual std::optional<int32_t> lookup(const BlockStateKey& key) const = 0;
};

class BiomeNameTable {
public:
  virtual ~BiomeNameTable() = default;
  virtual std::optional<int32_t> lookup(const std::string& name) const = 0;
  // Number of entries in the registry the ids index into. Direct/global
  // containers (more than 8 distinct biomes per section) store the raw
  // registry index at ceil(log2(registry_size())) bits, so the encoder must
  // know the size of the order advertised during configuration.
  virtual size_t registry_size() const = 0;
};

// The four light masks at the end of a chunk body. 26.3's
// `ByteBufCodecs.BIT_SET` is a length-prefixed byte array (`BitSet.toByteArray`,
// little-endian bit order), while 26.1/26.2's `FriendlyByteBuf.writeBitSet` is a
// varint-counted long array. An empty mask is `0` in both, so the all-air
// fallback body needs no version choice.
enum class LightMaskEncoding { ByteArray, LongArray };

// Encodes the body of play 0x2e for `chunk` (everything after the packet id):
// x/z, heightmaps, section bytes, block entities (none), light data. Returns
// nullopt when a block/biome state has no network id or the container is
// malformed. The heightmap emission order is the committed fixture
// capture's (5,4,1) — clients route entries by type varint so any order is
// valid (vanilla's own order varies run to run); byte comparisons against
// captures must either match this order or normalize heightmap order.
// The canonical 26.3 wire uses `ByteArray`; a translated 26.1/26.2 client
// passes `LongArray`.
std::optional<std::vector<uint8_t>>
encode_chunk_body(const Chunk& chunk, const BlockStateTable& blocks, const BiomeNameTable& biomes,
                  LightMaskEncoding light_masks = LightMaskEncoding::ByteArray);

// `BlockState.blocksMotion()` for a block name — the predicate the two
// MOTION_BLOCKING heightmaps (and `OCEAN_FLOOR`'s MATERIAL_MOTION_BLOCKING) are
// built from, generated from the vanilla registry into world/collision_table.h.
//
// It is the answer to "does this block stop the rain", which is what the
// weather asks through the motion-blocking height: a fire, a torch, a flower or
// a carpet of grass is not a roof, so a rained-on fire is one this reports
// false for. A name the table does not carry collides, with the one residual
// approximation the generated header documents (a small collision shape whose
// block sets neither `forceSolidOn` nor `forceSolidOff`).
bool block_blocks_motion(std::string_view block_name);

// `BlockState.canOcclude()` is FALSE: the block was registered `noOcclusion()`
// (generated table, world/occlusion_table.h). This is the other half of
// `isSolidRender()` — a full cube that does not occlude (glass, ice, a closed
// shulker box) passes light where a full cube that does (stone) stops it — which
// is what the light engine's dampening asks, in world/light.cpp.
bool block_no_occlusion(std::string_view block_name);

// `LevelReader.getHeight(Heightmap.Types.MOTION_BLOCKING, x, z)`: the y one past
// the highest motion-blocking block of the column, i.e. the first cell rain
// reaches — the dimension's min build height for an all-air column, which is
// what `Heightmap` answers with no entry. Read from the chunk's *own*
// heightmap, through `Chunk::height_at`, so it is O(1) for a clean column (one
// word test) and always the value the blocks imply: the client was sent it.
//
// This is the *only* way to read a height, which is deliberate: it used to come
// in a `(const Chunk&)` form that answered the stale stored entry beside a
// `(Chunk&)` form that refreshed, so correctness rested on which overload a
// caller happened to bind — and the two world stores bound different ones (a
// `mutable` map made MemoryWorld's const readers pick the refreshing form while
// AnvilWorld's took the stale one). A caller holding only a `const Chunk&` must
// now get a mutable one; the refresh is a cache update, and the three owners of
// a column (both world stores under their lock, `LightRegion`) each hand out the
// mutable reference they own — the two stores through the private
// `refreshable_chunk_locked`. Local x/z are 0..15.
int chunk_motion_blocking_height(Chunk& chunk, int x, int z);

// `LevelReader.getHeight(Heightmap.Types.WORLD_SURFACE, x, z)`: the y one past
// the highest *non-air* block of the column (a plant counts, where the
// motion-blocking map ignores it), or the min build height for an all-air
// column. This is the height natural spawning's random position is drawn up to
// (`NaturalSpawner.getRandomPosWithin`) — not a sky/rain question, so it is not
// the same number `motion_blocking_height` answers. Single form, same rule, for
// the same reason as above.
int chunk_world_surface_height(Chunk& chunk, int x, int z);

// `Level.getBiome(pos).value()`: the biome name at a position, from the
// section's 4x4x4 biome container (local x/z 0..15, absolute y). Precipitation —
// and therefore rain, snow, a rained-out fire and a filling cauldron — is a
// property of this biome (world/biome_table.h).
std::string chunk_biome_at(const Chunk& chunk, int x, int y, int z);

} // namespace mc::world
