// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/anvil.h"

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/log.h"
#include "world/nbt.h"
#include "world/world_light.h"

namespace mc::world {

namespace {

// Region geometry: 32x32 chunks per region; header entry for (lx, lz) lives
// at byte (lz * 32 + lx) * 4; data sectors start after the 4 KiB header.
constexpr int kRegionSize = 32;
// The header's location word stores the sector count in its low 8 bits, so a
// record can never legitimately span more than 255 sectors (1044480 bytes).
// Allowing more would silently wrap the count to 0 (chunk reads back absent)
// and overlap the next record.
constexpr uint32_t kMaxRecordSectors = 255;
constexpr size_t kMaxRecordBytes = static_cast<size_t>(kMaxRecordSectors) * 4096;
// Upper bound on decompressed chunk NBT (defense in depth against malicious region files).
constexpr size_t kMaxDecompressedBytes = 16 * 1024 * 1024;

int floor_div(int a, int b) {
  int q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0)))
    --q;
  return q;
}

// Decompresses one region record payload. Handles zlib (id 2), gzip (id 1)
// and raw (id 3); returns false on a corrupt stream. windowBits 15+32 lets
// zlib auto-detect both zlib and gzip headers.
bool inflate_payload(uint8_t compression, const uint8_t* in, size_t in_len,
                     std::vector<uint8_t>* out) {
  // Never leave a failed attempt's partial bytes behind: callers chain codecs
  // (zlib then gzip) and would otherwise parse the concatenation.
  out->clear();
  if (compression == 3) {
    out->assign(in, in + in_len);
    return true;
  }
  if (compression != 1 && compression != 2)
    return false;
  z_stream zs{};
  if (inflateInit2(&zs, 15 + 32) != Z_OK)
    return false;
  zs.next_in = const_cast<uint8_t*>(in);
  zs.avail_in = static_cast<uInt>(in_len);
  // Inflate straight into the caller's buffer, growing it 64 KiB at a time.
  // Region NBT compresses about 5.5:1, so the reserve usually covers a chunk in
  // one shot; the old loop inflated into a scratch block and copied it out on
  // every 64 KiB, which is a second pass over the whole payload on the one path
  // a joining player waits on.
  constexpr size_t kStep = 1 << 16;
  out->reserve(std::min(kMaxDecompressedBytes, in_len * 6 + kStep));
  int rc = Z_OK;
  while (rc == Z_OK) {
    const size_t filled = out->size();
    out->resize(filled + kStep);
    zs.next_out = out->data() + filled;
    zs.avail_out = static_cast<uInt>(kStep);
    rc = inflate(&zs, Z_NO_FLUSH);
    const size_t produced = kStep - zs.avail_out;
    if (filled + produced > kMaxDecompressedBytes) {
      inflateEnd(&zs);
      out->clear();
      return false; // decompressed payload exceeds safety cap
    }
    out->resize(filled + produced);
  }
  inflateEnd(&zs);
  if (rc != Z_STREAM_END) {
    out->clear();
    return false;
  }
  return true;
}

bool deflate_payload(const std::vector<uint8_t>& in, std::vector<uint8_t>* out, int window_bits) {
  z_stream zs{};
  // window_bits: 15 for zlib, 15+16 for gzip
  if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY) !=
      Z_OK)
    return false;
  zs.next_in = const_cast<uint8_t*>(in.data());
  zs.avail_in = static_cast<uInt>(in.size());
  std::vector<uint8_t> buf(1 << 16);
  int rc = Z_OK;
  while (rc == Z_OK) {
    zs.next_out = buf.data();
    zs.avail_out = static_cast<uInt>(buf.size());
    rc = deflate(&zs, Z_FINISH);
    if (rc == Z_STREAM_ERROR) {
      deflateEnd(&zs);
      return false;
    }
    const size_t produced = buf.size() - zs.avail_out;
    out->insert(out->end(), buf.data(), buf.data() + produced);
    if (rc == Z_STREAM_END)
      break;
  }
  deflateEnd(&zs);
  return rc == Z_STREAM_END;
}

bool zlib_compress(const std::vector<uint8_t>& in, std::vector<uint8_t>* out) {
  return deflate_payload(in, out, 15);
}

// A whole file in one read. The region and level.dat paths used
// `istreambuf_iterator`, which pulls the file through one `sgetc` at a time; a
// cold region open is the first thing a joining player pays for, so it is a
// `seekg`/`tellg` and a single `read` instead. A failure leaves `*out` empty,
// which every caller already reads as "no such file".
void read_file_bytes(const std::filesystem::path& path, std::vector<uint8_t>* out) {
  out->clear();
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f)
    return;
  const std::streamoff size = f.tellg();
  if (size <= 0)
    return;
  out->resize(static_cast<size_t>(size));
  f.seekg(0, std::ios::beg);
  f.read(reinterpret_cast<char*>(out->data()), size);
  const std::streamsize got = f.gcount();
  if (got < size)
    out->resize(static_cast<size_t>(got < 0 ? 0 : got));
}
bool gzip_compress(const std::vector<uint8_t>& in, std::vector<uint8_t>* out) {
  return deflate_payload(in, out, 15 + 16);
}

// Parses one chunk record body (the full NBT payload) into a Chunk.
//
// The direct reader is the loader's path: it walks the record's tags once and
// writes them straight into the column, where the tree path below builds a
// `nbt::Value` for every tag in the payload first and then walks that. It is
// also the narrower reader — a record whose sections carry a shape it does not
// model fails and is retried on the tree, so a new field on disk can never make
// a column unreadable.
bool parse_nbt_to_chunk(const std::vector<uint8_t>& nbt, int min_section_y, int section_count,
                        Chunk* out) {
  if (parse_chunk_nbt_direct(nbt.data(), nbt.size(), min_section_y, section_count, out))
    return true;
  *out = Chunk{};
  auto root = nbt::parse(nbt.data(), nbt.size());
  if (!root)
    return false;
  return parse_chunk_nbt(*root, min_section_y, section_count, out);
}

// Sets (or adds) the x/y/z fields of a block-entity compound, preserving every
// other field: a container record may carry a custom name, a lock, ... that
// this server does not model and must not drop on save.
void set_block_entity_pos(nbt::Value* be, const BlockPosKey& pos) {
  if (be == nullptr || be->type != nbt::Tag::Compound)
    return;
  const char* names[3] = {"x", "y", "z"};
  const int coords[3] = {pos.x, pos.y, pos.z};
  for (int i = 0; i < 3; ++i) {
    bool found = false;
    for (auto& [name, value] : be->fields) {
      if (name == names[i]) {
        value = nbt::make_int(coords[i]);
        found = true;
        break;
      }
    }
    if (!found)
      be->fields.emplace_back(names[i], nbt::make_int(coords[i]));
  }
}

Chunk make_fresh_chunk(int cx, int cz, int min_section_y, int section_count) {
  Chunk chunk;
  chunk.x = cx;
  chunk.z = cz;
  chunk.min_section_y = min_section_y;
  chunk.sections.resize(static_cast<size_t>(section_count));
  for (int i = 0; i < section_count; ++i) {
    auto& sec = chunk.sections[static_cast<size_t>(i)];
    sec.y = min_section_y + i;
    sec.has_blocks = true;
    sec.block_palette = {{"minecraft:air", ""}};
    sec.block_cells.assign(4096, 0);
    sec.has_biomes = true;
    sec.biome_palette = {"minecraft:the_void"};
    sec.biome_cells.assign(64, 0);
  }
  chunk.heightmaps.emplace_back(HeightmapType::MotionBlockingNoLeaves, std::vector<int64_t>(37, 0));
  chunk.heightmaps.emplace_back(HeightmapType::MotionBlocking, std::vector<int64_t>(37, 0));
  chunk.heightmaps.emplace_back(HeightmapType::WorldSurface, std::vector<int64_t>(37, 0));
  return chunk;
}

// Region header helpers
uint32_t read_be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
void write_be32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

} // namespace

// Reads and caches a whole region file in memory (a region is at most a few
// MiB; serving from memory keeps chunk loads allocation-light). Thread
// safety is provided by the AnvilWorld mutex around all Impl state.
struct AnvilWorld::Impl {
  // ---- configuration: fixed when the world is opened, read-only after ----
  std::filesystem::path region_dir;
  std::optional<std::filesystem::path> world_root; // set by open_world
  int min_section_y = 0;
  int section_count = 0;
  std::recursive_mutex mu;
  // ---- region-file bytes: the on-disk read cache (IO concern) ----
  // Region coordinate -> whole-file bytes (loaded on first chunk access).
  std::map<std::pair<int, int>, std::vector<uint8_t>> regions;

  // LRU chunk cache: at most kMaxCachedChunks entries.  A miss evicts the
  // least-recently-used entry so the cache stays bounded during long sessions
  // with players exploring far from spawn.
  static constexpr size_t kMaxCachedChunks = 4096;
  using LruKey = std::pair<int, int>;
  // A null residence is a cached miss: the column is known absent on disk.
  // Dirty columns are *shared*, never copied: dirty_store below holds the same
  // object the LRU residence (when present) points at, so a write mutates one
  // Chunk in place, an LRU eviction can never destroy a dirty column, and
  // there is no second copy to keep in sync.
  using LruList = std::list<std::pair<LruKey, std::shared_ptr<Chunk>>>;
  LruList lru_order; // front = most recent
  std::map<LruKey, LruList::iterator> lru_map;

  // Dirty tracking (Milestone E): mutated chunks not yet flushed to disk.
  // Presence in this map IS the dirty flag — one owner for both the data and
  // the dirtiness, so readers, writers and the flusher cannot disagree.
  std::map<LruKey, std::shared_ptr<Chunk>> dirty_store;

  // The light-input versions (world/world_interface.h's
  // `chunk_content_version`): one per resident column, minted on first ask and
  // moved by every write that lands. They live beside the caches rather than in
  // `Chunk` because a column can leave and re-enter the LRU: an evicted column
  // drops its version (`evict_lru_locked`), and the re-read mints a new one —
  // conservative on purpose, because the relight cannot know that the column it
  // reads from disk is the one it copied.
  std::map<LruKey, uint64_t> versions;
  uint64_t next_version = 1;
  // This store's identity for the relight's cross-call copy reuse
  // (world/world_interface.h's `content_store_token`): unique for the process'
  // lifetime, so a column copied from a store that has since gone away can
  // never be mistaken for a copy of this one.
  const uint64_t store_token = [] {
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
  }();

  uint64_t version_locked(const LruKey& key) {
    const auto it = versions.find(key);
    if (it != versions.end())
      return it->second;
    const uint64_t minted = next_version++;
    versions.emplace(key, minted);
    return minted;
  }

  // ---- block-backed containers (Phase 3 milestone C) ----------------------
  // Live containers by absolute block position. This map — not the chunk copy —
  // is authoritative for container contents, so two viewers of the same chest
  // always see the same inventory and an LRU eviction never loses items. The
  // chunk is only the persistence medium: at save time the live contents are
  // injected back into the chunk's verbatim block-entity list.
  std::map<BlockPosKey, std::shared_ptr<BlockContainer>> containers;
  std::map<LruKey, std::set<BlockPosKey>> containers_by_chunk;
  // Chunks whose block-entity list changed and must be rewritten even when no
  // block was mutated (a chest can be emptied without the block changing).
  std::set<LruKey> container_dirty_chunks;

  // ---- coordinates and the lock-free single-cell reads (callers hold mu) -
  // Splits a world position into its chunk + local coordinates.
  static void chunk_of(int x, int z, int* cx, int* cz, int* lx, int* lz) {
    *cx = floor_div(x, 16);
    *cz = floor_div(z, 16);
    *lx = x - *cx * 16;
    *lz = z - *cz * 16;
  }

  // Reads a single block state without taking the lock (callers hold it).
  BlockStateKey block_state_locked(int x, int y, int z) {
    static const BlockStateKey kAir{"minecraft:air", ""};
    int cx = 0, cz = 0, lx = 0, lz = 0;
    chunk_of(x, z, &cx, &cz, &lx, &lz);
    auto chunk = load_locked(cx, cz);
    if (!chunk)
      return kAir;
    return chunk->block_state_at(lx, y, lz);
  }

  // Reads a position's persisted block-entity item list without taking the
  // lock (callers hold it).
  std::vector<ItemStack> be_items_locked(int x, int y, int z, int size) {
    int cx = 0, cz = 0, lx = 0, lz = 0;
    chunk_of(x, z, &cx, &cz, &lx, &lz);
    auto chunk = load_locked(cx, cz);
    if (!chunk)
      return std::vector<ItemStack>(static_cast<size_t>(size));
    for (const nbt::Value& be : chunk->block_entities)
      if (block_entity_matches(be, x, y, z))
        return read_block_entity_items(be, size);
    return std::vector<ItemStack>(static_cast<size_t>(size));
  }

  // Reads a position's persisted furnace timers without taking the lock
  // (callers hold it). A record with no timers leaves a cold furnace, which is
  // what a hand-written record means too.
  void be_furnace_locked(int x, int y, int z, FurnaceState* state) {
    int cx = 0, cz = 0, lx = 0, lz = 0;
    chunk_of(x, z, &cx, &cz, &lx, &lz);
    auto chunk = load_locked(cx, cz);
    if (!chunk)
      return;
    for (const nbt::Value& be : chunk->block_entities)
      if (block_entity_matches(be, x, y, z)) {
        read_block_entity_furnace(be, state);
        return;
      }
  }

  // Reads a position's persisted brewing counters the same way: a record with
  // neither `BrewTime` nor `Fuel` leaves an idle stand.
  void be_brewing_locked(int x, int y, int z, BrewingState* state) {
    int cx = 0, cz = 0, lx = 0, lz = 0;
    chunk_of(x, z, &cx, &cz, &lx, &lz);
    auto chunk = load_locked(cx, cz);
    if (!chunk)
      return;
    for (const nbt::Value& be : chunk->block_entities)
      if (block_entity_matches(be, x, y, z)) {
        read_block_entity_brewing(be, state);
        return;
      }
  }

  // ---- container registry (callers hold mu) -------------------------------
  // Registers a live container under every position it occupies (a double
  // chest registers under both halves, each in its own chunk's set).
  void register_container_locked(const std::shared_ptr<BlockContainer>& c) {
    for (const BlockPosKey& p : c->parts) {
      int cx = 0, cz = 0, lx = 0, lz = 0;
      chunk_of(p.x, p.z, &cx, &cz, &lx, &lz);
      containers[p] = c;
      containers_by_chunk[std::make_pair(cx, cz)].insert(p);
    }
  }

  // Removes a live container from every position it occupied.
  void unregister_container_locked(const BlockContainer& c) {
    for (const BlockPosKey& p : c.parts) {
      auto it = containers.find(p);
      if (it == containers.end() || it->second.get() != &c)
        continue;
      containers.erase(it);
      int cx = 0, cz = 0, lx = 0, lz = 0;
      chunk_of(p.x, p.z, &cx, &cz, &lx, &lz);
      auto cit = containers_by_chunk.find(std::make_pair(cx, cz));
      if (cit != containers_by_chunk.end()) {
        cit->second.erase(p);
        if (cit->second.empty())
          containers_by_chunk.erase(cit);
      }
    }
  }

  // Writes the live containers that live in this chunk back into its block
  // entity list, creating records for containers that never had one. Each
  // position gets its *own* part: a double chest keeps its first half in
  // parts[0]'s record and its second half in parts[1]'s (which may sit in a
  // different chunk — both chunks are registered in containers_by_chunk).
  void inject_containers_locked(int cx, int cz, Chunk* chunk) const {
    auto it = containers_by_chunk.find(std::make_pair(cx, cz));
    if (it == containers_by_chunk.end())
      return;
    for (const BlockPosKey& pos : it->second) {
      auto cit = containers.find(pos);
      if (cit == containers.end())
        continue;
      const BlockContainer& c = *cit->second;
      const int index = c.part_index(pos);
      if (index < 0)
        continue;
      const std::vector<ItemStack> items = c.part_items(index);
      bool found = false;
      for (nbt::Value& be : chunk->block_entities) {
        // Absolute world coordinates, like vanilla; a legacy local-coordinate
        // record from an older build is upgraded in place here too.
        if (block_entity_matches(be, pos.x, pos.y, pos.z)) {
          write_block_entity_items(&be, items);
          // A cooking container's block entity carries its four timers and the
          // RecipesUsed map as well as the items (milestone E); a brewing
          // stand's carries `BrewTime` and `Fuel`.
          if (c.is_cooking())
            write_block_entity_furnace(&be, c.furnace);
          if (c.is_brewing())
            write_block_entity_brewing(&be, c.brewing);
          // Normalize a legacy local-coordinate record in place: the x/z fields
          // become absolute (vanilla's form), everything else survives.
          set_block_entity_pos(&be, pos);
          found = true;
          break;
        }
      }
      if (!found) {
        nbt::Value record = make_container_block_entity(c.block, pos, items);
        if (c.is_cooking())
          write_block_entity_furnace(&record, c.furnace);
        if (c.is_brewing())
          write_block_entity_brewing(&record, c.brewing);
        chunk->block_entities.push_back(std::move(record));
      }
    }
  }

  // ---- chunk cache: the one eviction rule + reads (callers hold mu) ------

  // The eviction rule, extracted from the five hand-rolled copies of this
  // loop that used to live at every insert site. When the cache is full, drop
  // the least-recently-used column that is *not* dirty — a null cached-miss
  // residence counts as evictable — falling back to the oldest entry when
  // every resident is dirty. A dirty victim loses only its residence:
  // dirty_store below holds the same object (shared, never copied), so the
  // data survives and the next access finds it there — the dirty-store-ownership
  // invariant, untouched. Callers must hold `mu`.
  void evict_lru_locked() {
    if (lru_map.size() < kMaxCachedChunks)
      return;
    auto victim = std::prev(lru_order.end());
    for (auto rit = lru_order.rbegin(); rit != lru_order.rend(); ++rit) {
      if (dirty_store.find(rit->first) == dirty_store.end()) {
        victim = std::prev(rit.base());
        break;
      }
    }
    // The version goes with the column. A re-read mints a fresh one, so a
    // relight that copied the evicted column can never match the re-read one
    // (see `versions` above).
    versions.erase(victim->first);
    lru_map.erase(victim->first);
    lru_order.erase(victim);
  }

  // Returns the shared column; null is a cached miss (known absent on disk).
  // Touches recency and never copies — a dirty residence *is* the dirty_store
  // object, so the read path no longer resyncs a mirror per block read.
  std::shared_ptr<Chunk> load_locked(int cx, int cz) {
    const std::pair<int, int> key = std::make_pair(cx, cz);
    auto dit = dirty_store.find(key);
    if (dit != dirty_store.end()) {
      auto it = lru_map.find(key);
      if (it != lru_map.end())
        lru_order.splice(lru_order.begin(), lru_order, it->second);
      return dit->second;
    }
    auto it = lru_map.find(key);
    if (it != lru_map.end()) {
      lru_order.splice(lru_order.begin(), lru_order, it->second);
      return it->second->second;
    }
    std::shared_ptr<Chunk> found;
    if (std::optional<Chunk> disk = load_from_disk(cx, cz))
      found = std::make_shared<Chunk>(std::move(*disk));
    // Evict if needed (prefer clean victims)
    evict_lru_locked();
    lru_order.emplace_front(key, found);
    lru_map[key] = lru_order.begin();
    return found;
  }

  // ---- region IO: file bytes in, records out (callers hold mu) -----------
  // Reads + parses chunk (cx, cz) without consulting the chunk cache.
  std::optional<Chunk> load_from_disk(int cx, int cz) {
    const int rx = floor_div(cx, kRegionSize);
    const int rz = floor_div(cz, kRegionSize);
    const int lx = cx - rx * kRegionSize;
    const int lz = cz - rz * kRegionSize;
    const auto rkey = std::make_pair(rx, rz);
    const std::vector<uint8_t>* file = nullptr;
    auto rit = regions.find(rkey);
    if (rit == regions.end()) {
      const std::filesystem::path path =
          region_dir / ("r." + std::to_string(rx) + "." + std::to_string(rz) + ".mca");
      std::vector<uint8_t> bytes;
      read_file_bytes(path, &bytes);
      if (bytes.empty()) {
        // No region file for this coordinate: the chunk does not exist.
        regions.emplace(rkey, std::vector<uint8_t>());
        return std::nullopt;
      }
      regions.emplace(rkey, std::move(bytes));
      file = &regions.find(rkey)->second;
    } else {
      file = &rit->second;
    }
    if (file->empty())
      return std::nullopt;

    // Header entry: location = sector << 8 | sector count (0 = absent).
    const size_t entry_off = static_cast<size_t>(lz * kRegionSize + lx) * 4;
    if (entry_off + 4 > file->size())
      return std::nullopt;
    const uint32_t location = read_be32(file->data() + entry_off);
    const uint32_t sector = location >> 8;
    const uint32_t count = location & 0xFF;
    if (sector == 0 || count == 0)
      return std::nullopt;
    const size_t record_off = static_cast<size_t>(sector) * 4096;
    if (record_off + 5 > file->size())
      return std::nullopt;
    const uint32_t length = read_be32(file->data() + record_off);
    if (length < 2 || length > kMaxRecordBytes ||
        record_off + 4 + static_cast<size_t>(length) > file->size())
      return std::nullopt;
    const uint8_t compression = (*file)[record_off + 4];
    const uint8_t* payload = file->data() + record_off + 5;
    const size_t payload_len = length - 1;

    std::vector<uint8_t> nbt;
    if (!inflate_payload(compression, payload, payload_len, &nbt))
      return std::nullopt;
    Chunk chunk;
    if (!parse_nbt_to_chunk(nbt, min_section_y, section_count, &chunk))
      return std::nullopt;
    if (chunk.x != cx || chunk.z != cz)
      return std::nullopt; // header lied (should never happen)
    return chunk;
  }

  // Ensure region file bytes are loaded into regions[rkey]. Missing file -> empty vector.
  void ensure_region_loaded_locked(int rx, int rz) {
    auto rkey = std::make_pair(rx, rz);
    if (regions.find(rkey) != regions.end())
      return;
    std::filesystem::path path =
        region_dir / ("r." + std::to_string(rx) + "." + std::to_string(rz) + ".mca");
    std::vector<uint8_t> bytes;
    read_file_bytes(path, &bytes);
    regions.emplace(rkey, std::move(bytes));
  }

  // Build a single region record for `chunk`: [u32 length][u8 2][zlib(NBT)].
  // Returns the full on-disk record including the length prefix.
  bool build_record(const Chunk& chunk, std::vector<uint8_t>* out) {
    // Straight to bytes: a save used to build the whole `nbt::Value` tree and
    // then walk it, which was two passes over a 58 KB payload per column (the
    // reader lost the same tree on 22 Oct; `serialize_chunk_nbt_bytes` is the
    // writer's half). The bytes are identical — the fixture and round-trip tests
    // compare them — and block entities still go through their `Value`.
    const std::vector<uint8_t> nbt_bytes = serialize_chunk_nbt_bytes(chunk, 5023);
    std::vector<uint8_t> compressed;
    if (!zlib_compress(nbt_bytes, &compressed))
      return false;
    if (compressed.size() + 1 > kMaxRecordBytes)
      return false;
    uint32_t length = static_cast<uint32_t>(compressed.size() + 1);
    out->resize(4 + 1 + compressed.size());
    write_be32(out->data(), length);
    (*out)[4] = 2; // zlib
    std::memcpy(out->data() + 5, compressed.data(), compressed.size());
    return true;
  }

  // Rebuild one region file atomically from dirty chunks + existing file's other records.
  // `dirty_for_region`: map from local chunk index (0..1023) to the column to
  // write, by reference — the flush holds this mutex for the whole rebuild, so
  // the `with_entities` copy below is the one each column pays (the old
  // by-value grouping paid a staging copy on top of it).
  bool rebuild_region_locked(int rx, int rz, const std::map<int, const Chunk*>& dirty_for_region) {
    ensure_region_loaded_locked(rx, rz);
    auto rkey = std::make_pair(rx, rz);
    const std::vector<uint8_t>& old_file = regions[rkey];

    // Extract existing header if present
    uint32_t old_locs[1024] = {};
    uint32_t old_times[1024] = {};
    if (old_file.size() >= 8192) {
      for (int i = 0; i < 1024; ++i)
        old_locs[i] = read_be32(old_file.data() + i * 4);
      for (int i = 0; i < 1024; ++i)
        old_times[i] = read_be32(old_file.data() + 4096 + i * 4);
    }

    // Collect existing records' raw bytes for indices not being overwritten
    std::map<int, std::vector<uint8_t>> existing_recs;
    if (old_file.size() >= 8192) {
      for (int i = 0; i < 1024; ++i) {
        if (dirty_for_region.find(i) != dirty_for_region.end())
          continue;
        uint32_t loc = old_locs[i];
        uint32_t sector = loc >> 8;
        uint32_t count = loc & 0xFF;
        if (sector == 0 || count == 0)
          continue;
        size_t off = static_cast<size_t>(sector) * 4096;
        if (off + 5 > old_file.size())
          continue;
        uint32_t length = read_be32(old_file.data() + off);
        if (length < 2 || length > kMaxRecordBytes)
          continue;
        if (off + 4 + length > old_file.size())
          continue;
        // Copy entire record including length prefix
        std::vector<uint8_t> rec(old_file.data() + off, old_file.data() + off + 4 + length);
        existing_recs[i] = std::move(rec);
      }
    }

    // Prepare new file
    std::vector<uint8_t> new_file;
    new_file.assign(8192, 0);
    uint32_t new_locs[1024] = {};
    uint32_t new_times[1024] = {};
    // Preserve timestamps for untouched chunks
    for (int i = 0; i < 1024; ++i) {
      if (dirty_for_region.find(i) == dirty_for_region.end() &&
          existing_recs.find(i) != existing_recs.end()) {
        new_times[i] = old_times[i];
      }
    }
    // Timestamp for dirty chunks = now
    uint32_t now = static_cast<uint32_t>(std::time(nullptr));
    for (auto& [idx, _] : dirty_for_region)
      new_times[idx] = now;

    // Sector allocator
    size_t next_sector = 2; // header occupies 0,1
    // We will grow new_file as needed; ensure header size.
    auto ensure_size = [&](size_t sectors_needed) {
      size_t needed = (next_sector + sectors_needed) * 4096;
      if (new_file.size() < needed)
        new_file.resize(needed, 0);
    };

    // Build final set of records indexed 0..1023 in file order (0 first)
    // Keep order by index for determinism (vanilla also writes by discovery order, but any is
    // fine).
    std::map<int, std::vector<uint8_t>> final_recs = existing_recs;
    for (auto& [idx, chunk] : dirty_for_region) {
      // The one copy the flush pays: the injected block entities must stay out
      // of the caller's column (which the flush clears afterwards), and
      // `snapshot` makes the serializer's Heightmaps match the blocks it writes.
      Chunk with_entities = chunk->snapshot();
      const int cx = rx * kRegionSize + (idx % kRegionSize);
      const int cz = rz * kRegionSize + (idx / kRegionSize);
      inject_containers_locked(cx, cz, &with_entities);
      std::vector<uint8_t> rec;
      if (!build_record(with_entities, &rec)) {
        log::error("anvil: failed to serialize chunk ", chunk->x, ",", chunk->z);
        return false;
      }
      final_recs[idx] = std::move(rec);
    }

    for (int i = 0; i < 1024; ++i) {
      auto it = final_recs.find(i);
      if (it == final_recs.end())
        continue;
      const std::vector<uint8_t>& rec = it->second;
      const size_t sectors = (rec.size() + 4095) / 4096;
      if (sectors > kMaxRecordSectors) {
        // Would overflow the 8-bit sector count in the header.
        log::error("anvil: chunk record for idx ", i, " needs ", sectors, " sectors (max ",
                   kMaxRecordSectors, "); region not written");
        return false;
      }
      ensure_size(sectors);
      size_t off = next_sector * 4096;
      std::memcpy(new_file.data() + off, rec.data(), rec.size());
      // padding already zero
      new_locs[i] = static_cast<uint32_t>((next_sector << 8) | sectors);
      next_sector += sectors;
    }

    // Write header
    for (int i = 0; i < 1024; ++i)
      write_be32(new_file.data() + i * 4, new_locs[i]);
    for (int i = 0; i < 1024; ++i)
      write_be32(new_file.data() + 4096 + i * 4, new_times[i]);

    // Ensure directory exists
    std::error_code ec;
    std::filesystem::create_directories(region_dir, ec);
    if (ec) {
      log::error("anvil: cannot create region dir ", region_dir.string(), ": ", ec.message());
      return false;
    }

    // Atomic write: temp + rename
    std::filesystem::path tmp =
        region_dir / ("r." + std::to_string(rx) + "." + std::to_string(rz) + ".mca.tmp");
    std::filesystem::path dst =
        region_dir / ("r." + std::to_string(rx) + "." + std::to_string(rz) + ".mca");
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (!out) {
        log::error("anvil: cannot open ", tmp.string(), " for writing");
        return false;
      }
      out.write(reinterpret_cast<const char*>(new_file.data()),
                static_cast<std::streamsize>(new_file.size()));
      if (!out) {
        log::error("anvil: write failed for ", tmp.string());
        return false;
      }
    }
    // fsync temp? best effort via rename atomicity; on POSIX rename is atomic.
    std::error_code rename_ec;
    std::filesystem::rename(tmp, dst, rename_ec);
    if (rename_ec) {
      // Fallback: remove dst and retry
      std::filesystem::remove(dst, ec);
      std::filesystem::rename(tmp, dst, rename_ec);
      if (rename_ec) {
        log::error("anvil: rename ", tmp.string(), " -> ", dst.string(), ": ", rename_ec.message());
        return false;
      }
    }
    // Update in-memory cache
    regions[rkey] = std::move(new_file);
    return true;
  }
};

AnvilWorld::AnvilWorld(std::filesystem::path region_dir, int min_section_y, int section_count)
    : impl_(std::make_shared<Impl>()), region_dir_(std::move(region_dir)) {
  impl_->region_dir = region_dir_;
  impl_->min_section_y = min_section_y;
  impl_->section_count = section_count;
  min_section_y_ = min_section_y;
  section_count_ = section_count;
}

std::shared_ptr<AnvilWorld> AnvilWorld::open_world(const std::filesystem::path& world_dir,
                                                   int min_section_y, int section_count) {
  std::error_code ec;
  // If world_dir does not exist, create it (fresh world bootstrap)
  if (!std::filesystem::exists(world_dir, ec)) {
    std::filesystem::create_directories(world_dir, ec);
    if (ec)
      return nullptr;
  } else if (!std::filesystem::is_directory(world_dir, ec)) {
    return nullptr;
  }
  if (ec)
    return nullptr;
  // Modern 26.2 layout; fall back to legacy root region/ folder.
  std::filesystem::path region_dir =
      world_dir / "dimensions" / "minecraft" / "overworld" / "region";
  // For a fresh world, ensure the region directory exists
  if (!std::filesystem::exists(region_dir, ec)) {
    // Check legacy path
    std::filesystem::path legacy = world_dir / "region";
    if (std::filesystem::is_directory(legacy, ec)) {
      region_dir = legacy;
    } else {
      // Create modern layout
      std::filesystem::create_directories(region_dir, ec);
      if (ec) {
        // Try legacy as last resort
        region_dir = legacy;
        std::filesystem::create_directories(region_dir, ec);
        if (ec)
          return nullptr;
      }
    }
  }
  auto world =
      std::shared_ptr<AnvilWorld>(new AnvilWorld(region_dir, min_section_y, section_count));
  world->impl_->world_root = std::filesystem::absolute(world_dir, ec);
  if (ec)
    world->impl_->world_root = world_dir;
  world->valid_ = true;
  // Ensure minimal level.dat so vanilla tools recognise the world
  world->ensure_level_dat();
  return world;
}

std::shared_ptr<AnvilWorld> AnvilWorld::open_region_dir(const std::filesystem::path& region_dir,
                                                        int min_section_y, int section_count) {
  std::error_code ec;
  if (!std::filesystem::is_directory(region_dir, ec))
    return nullptr;
  auto world =
      std::shared_ptr<AnvilWorld>(new AnvilWorld(region_dir, min_section_y, section_count));
  world->valid_ = true;
  return world;
}

std::optional<Chunk> AnvilWorld::load_chunk(int cx, int cz) const {
  if (!valid_)
    return std::nullopt;
  std::lock_guard<std::recursive_mutex> lock(impl_->mu);
  std::shared_ptr<Chunk> chunk = impl_->load_locked(cx, cz);
  if (!chunk)
    return std::nullopt;
  // The copy escapes (the wire encoder, the light ring) and carries heightmaps
  // current with its blocks — `Chunk::snapshot` is that boundary everywhere.
  return chunk->snapshot();
}

const Chunk* AnvilWorld::cached_chunk_locked(int cx, int cz) const {
  const std::pair<int, int> key = std::make_pair(cx, cz);
  // A dirty (mutated, not yet saved) column is authoritative.
  auto dit = impl_->dirty_store.find(key);
  if (dit != impl_->dirty_store.end())
    return dit->second.get();
  auto it = impl_->lru_map.find(key);
  if (it != impl_->lru_map.end()) {
    if (!it->second->second)
      return nullptr; // cached miss: the chunk is known absent on disk
    impl_->lru_order.splice(impl_->lru_order.begin(), impl_->lru_order, it->second);
    return &*it->second->second;
  }
  // A miss populates the LRU (or caches the miss); the entry then stays put
  // until the caller releases the lock. `load_locked` may evict a *different*
  // column, never this one, so the pointer is stable for the caller's use.
  impl_->load_locked(cx, cz);
  it = impl_->lru_map.find(key);
  if (it == impl_->lru_map.end() || !it->second->second)
    return nullptr;
  return &*it->second->second;
}

// The borrow for the two height reads, which bring the column's lazy heightmaps
// current before they answer (chunk.h) — the only two callers, so a pure reader
// still cannot write into a cached column through `cached_chunk_locked`. The cast
// is sound: the column is the world's own mutable state, reached under `mu`
// exactly as `mu` itself is.
Chunk* AnvilWorld::refreshable_chunk_locked(int cx, int cz) const {
  return const_cast<Chunk*>(cached_chunk_locked(cx, cz));
}

BlockStateKey AnvilWorld::get_block_state(int cx, int cz, int x, int y, int z) const {
  static const BlockStateKey kAir{"minecraft:air", ""};
  if (!valid_)
    return kAir;
  std::lock_guard<std::recursive_mutex> lock(impl_->mu);
  const Chunk* chunk = cached_chunk_locked(cx, cz);
  if (chunk == nullptr)
    return kAir;
  return chunk->block_state_at(x, y, z);
}

// The same read as `get_block_state` with the value left in the cached column:
// the same `valid_` test, the same lock, the same `cached_chunk_locked` lookup
// and the same air fallback, then the palette entry's kinds in place instead of
// a copy of its two strings (world/world_interface.h).
int AnvilWorld::light_kinds_at(int cx, int cz, int x, int y, int z) const {
  static const BlockStateKey kAir{"minecraft:air", ""};
  if (!valid_)
    return light_kinds(kAir);
  std::lock_guard<std::recursive_mutex> lock(impl_->mu);
  const Chunk* chunk = cached_chunk_locked(cx, cz);
  if (chunk == nullptr)
    return light_kinds(kAir);
  const Chunk::CellIndex index = chunk->cell_index_at(x, y, z);
  if (index.section < 0 || index.palette < 0)
    return light_kinds(kAir);
  return light_kinds(chunk->sections[static_cast<size_t>(index.section)]
                         .block_palette[static_cast<size_t>(index.palette)]);
}

uint64_t AnvilWorld::random_tick_sections(int cx, int cz) const {
  if (!valid_)
    return 0;
  std::lock_guard<std::recursive_mutex> lock(impl_->mu);
  const Chunk* chunk = cached_chunk_locked(cx, cz);
  if (chunk == nullptr)
    return 0;
  uint64_t mask = 0;
  const int count = std::min(section_count(), 64);
  for (int i = 0; i < count; ++i) {
    if (static_cast<size_t>(i) < chunk->sections.size() &&
        chunk->sections[static_cast<size_t>(i)].random_tick_blocks > 0)
      mask |= (1ull << i);
  }
  return mask;
}

bool AnvilWorld::has_chunk_light(int cx, int cz) const {
  if (!valid_)
    return false;
  std::lock_guard<std::recursive_mutex> lock(impl_->mu);
  const Chunk* chunk = cached_chunk_locked(cx, cz);
  return chunk != nullptr && chunk_has_light(*chunk);
}

// Whether a column is held, without the copy `load_chunk` makes of it: the same
// cached-column lookup, so a caller that only wants the answer never pays for
// the value (world/world_interface.h).
bool AnvilWorld::has_chunk(int cx, int cz) const {
  if (!valid_)
    return false;
  std::lock_guard<std::recursive_mutex> lock(impl_->mu);
  return cached_chunk_locked(cx, cz) != nullptr;
}

// The residency question (anvil.h): the LRU or the dirty store, and never a
// load. This is `cached_chunk_locked`'s question-shaped half — that read loads
// the column to answer, which is what a caller asking "do you hold this?" must
// not pay across a spawn square (docs/PHASE5.md).
bool AnvilWorld::has_stored_column(int cx, int cz) const {
  // The A/B arm: `off` answers the interface default, whose `has_chunk` loads a
  // column the store does not hold — the answer the natural spawn pass got
  // before this override existed.
  static const bool kResidentOnly = [] {
    const char* text = std::getenv("PLUSMC_ANVIL_STORED_RESIDENT");
    return text == nullptr || std::strcmp(text, "off") != 0;
  }();
  if (!kResidentOnly)
    return has_chunk(cx, cz);
  if (!valid_)
    return false;
  std::lock_guard<std::recursive_mutex> lock(impl_->mu);
  const std::pair<int, int> key = std::make_pair(cx, cz);
  // A dirty column is held even when its LRU residence is gone: the write is
  // the store's state, and `dirty_store` holds the same shared object.
  if (impl_->dirty_store.find(key) != impl_->dirty_store.end())
    return true;
  const auto it = impl_->lru_map.find(key);
  if (it == impl_->lru_map.end() || !it->second->second)
    return false; // never loaded, or a cached miss (known absent on disk)
  // A hit is a use: the same recency rule every other cache read follows.
  impl_->lru_order.splice(impl_->lru_order.begin(), impl_->lru_order, it->second);
  return true;
}

// Whether a load at (cx, cz) yields a column, answered by the load's own first
// two steps and none of its last: the same `valid_` test and the same
// `load_locked` (which reads the region file on a miss, populates the cache and
// touches recency exactly as the load does), then the column is dropped instead
// of copied out. So this is `load_chunk(cx, cz).has_value()` by construction —
// every branch of the load that answers has a column is the same branch here —
// minus the ~400 KiB copy and the ~100 allocations that answering the question
// out of the value costs (world/world_interface.h).
bool AnvilWorld::can_load_chunk(int cx, int cz) const {
  if (!valid_)
    return false;
  std::lock_guard<std::recursive_mutex> lock(impl_->mu);
  return impl_->load_locked(cx, cz) != nullptr;
}

int AnvilWorld::raw_brightness(int x, int y, int z, int sky_darken) const {
  if (!valid_)
    return 0;
  const int cx = x >= 0 ? x / 16 : (x - 15) / 16;
  const int cz = z >= 0 ? z / 16 : (z - 15) / 16;
  std::lock_guard<std::recursive_mutex> lock(impl_->mu);
  const Chunk* chunk = cached_chunk_locked(cx, cz);
  if (chunk == nullptr)
    return 0;
  return chunk_raw_brightness(*chunk, x - cx * 16, y, z - cz * 16, sky_darken);
}

int AnvilWorld::light_level(LightLayer layer, int x, int y, int z) const {
  if (!valid_)
    return 0;
  const int cx = x >= 0 ? x / 16 : (x - 15) / 16;
  const int cz = z >= 0 ? z / 16 : (z - 15) / 16;
  std::lock_guard<std::recursive_mutex> lock(impl_->mu);
  const Chunk* chunk = cached_chunk_locked(cx, cz);
  if (chunk == nullptr)
    return 0;
  return chunk_light_level(*chunk, x - cx * 16, y, z - cz * 16, layer);
}

int AnvilWorld::motion_blocking_height(int x, int z) const {
  const int min_y = min_section_y_ * 16;
  if (!valid_)
    return min_y;
  const int cx = x >= 0 ? x / 16 : (x - 15) / 16;
  const int cz = z >= 0 ? z / 16 : (z - 15) / 16;
  std::lock_guard<std::recursive_mutex> lock(impl_->mu);
  // The refreshable borrow: the read brings a marked column current (chunk.h) —
  // the very read that used to answer a file-backed world's pre-write height.
  Chunk* chunk = refreshable_chunk_locked(cx, cz);
  if (chunk == nullptr)
    return min_y; // no column on disk: the fallback emitter's all-air column
  return chunk_motion_blocking_height(*chunk, x - cx * 16, z - cz * 16);
}

int AnvilWorld::world_surface_height(int x, int z) const {
  const int min_y = min_section_y_ * 16;
  if (!valid_)
    return min_y;
  const int cx = x >= 0 ? x / 16 : (x - 15) / 16;
  const int cz = z >= 0 ? z / 16 : (z - 15) / 16;
  std::lock_guard<std::recursive_mutex> lock(impl_->mu);
  Chunk* chunk = refreshable_chunk_locked(cx, cz); // the same refreshable borrow
  if (chunk == nullptr)
    return min_y;
  return chunk_world_surface_height(*chunk, x - cx * 16, z - cz * 16);
}

std::string AnvilWorld::biome_name_at(int x, int y, int z) const {
  if (!valid_)
    return "minecraft:the_void";
  const int cx = x >= 0 ? x / 16 : (x - 15) / 16;
  const int cz = z >= 0 ? z / 16 : (z - 15) / 16;
  std::lock_guard<std::recursive_mutex> lock(impl_->mu);
  const Chunk* chunk = cached_chunk_locked(cx, cz);
  if (chunk == nullptr)
    return "minecraft:the_void";
  return chunk_biome_at(*chunk, x - cx * 16, y, z - cz * 16);
}

// The light engine's exclusion: the relight borrows nine live cached columns
// and writes one of them, and every reader here takes `impl_->mu` for the whole
// of its own call — so the relight must hold that same mutex for the whole of
// its borrow-and-write (world/world_interface.h's
// note on `lock_relight`). Re-entrant, because the borrows below (`has_chunk`,
// `modify_chunk`) come straight back through it, and because a relight of a
// column not in the cache loads it from disk inside the same thread.
void AnvilWorld::lock_relight() {
  impl_->mu.lock();
}

void AnvilWorld::unlock_relight() {
  impl_->mu.unlock();
}

// The light-input version (world/world_interface.h). Only a resident column has
// one: a miss is 0, which the relight reads as "the warm pass did not put this
// column there" — and it warms through the interface's default
// `ensure_chunk_stored` before trying again. `dirty_store` answers first because
// a dirty column is authoritative over its LRU residence (they share the same
// object anyway), and a cached miss (a null residence) answers 0 for the same
// reason it reads as absent everywhere else in this store.
uint64_t AnvilWorld::content_store_token() const {
  return impl_->store_token;
}

uint64_t AnvilWorld::chunk_content_version(int cx, int cz) const {
  if (!valid_)
    return 0;
  std::lock_guard<std::recursive_mutex> lock(impl_->mu);
  const std::pair<int, int> key = std::make_pair(cx, cz);
  if (impl_->dirty_store.find(key) != impl_->dirty_store.end())
    return impl_->version_locked(key);
  const auto it = impl_->lru_map.find(key);
  if (it == impl_->lru_map.end() || !it->second->second)
    return 0;
  return impl_->version_locked(key);
}

// The lease this returns is unpinned on purpose (`ChunkLease` builds one with a
// null release): this world evicts by count from a cache whose victims are
// re-readable from the region file, and the callers that hold a live reference
// across further world operations hold `lock_relight`'s mutex for the whole
// borrow. The store that needed a pin is MemoryWorld, where the map is the only
// copy.
std::optional<ChunkLease> AnvilWorld::modify_chunk(int cx, int cz) {
  if (!valid_)
    return std::nullopt;
  std::lock_guard<std::recursive_mutex> lock(impl_->mu);
  const std::pair<int, int> key = std::make_pair(cx, cz);
  // The chunk a reader is answered with: `dirty_store` wins when it holds the
  // case (`cached_chunk_locked`, `load_locked`), the LRU entry otherwise. With
  // shared residences those are the *same object*, so the reference handed to
  // the caller can never be dropped by a later mirror resync — the hazard this
  // function used to document.
  //
  // The reference is *live*, and heights stay someone else's job: a column a
  // write marked answers through the query helpers (chunk.h), which is the one
  // place a marked column comes current. Only a snapshot that leaves the store
  // is refreshed at its own boundary (`load_chunk`, the flush's serializer
  // copy); refreshing here too would be a third, redundant application.
  auto dit = impl_->dirty_store.find(key);
  if (dit != impl_->dirty_store.end()) {
    auto it = impl_->lru_map.find(key);
    if (it == impl_->lru_map.end()) {
      // Need space
      impl_->evict_lru_locked();
      impl_->lru_order.emplace_front(key, dit->second);
      impl_->lru_map[key] = impl_->lru_order.begin();
    } else {
      impl_->lru_order.splice(impl_->lru_order.begin(), impl_->lru_order, it->second);
    }
    return ChunkLease(*dit->second);
  }
  auto it = impl_->lru_map.find(key);
  if (it == impl_->lru_map.end()) {
    // Load from disk
    auto found = impl_->load_from_disk(cx, cz);
    std::shared_ptr<Chunk> ptr;
    // If absent, synthesize a fresh empty chunk so mutation can proceed (new column)
    if (!found) {
      ptr = std::make_shared<Chunk>(
          make_fresh_chunk(cx, cz, impl_->min_section_y, impl_->section_count));
      // Insert as present
      impl_->evict_lru_locked();
      impl_->lru_order.emplace_front(key, ptr);
      impl_->lru_map[key] = impl_->lru_order.begin();
      // Mark dirty creation — the same object, shared, not copied.
      impl_->dirty_store[key] = ptr;
      return ChunkLease(*ptr);
    }
    // Found on disk: insert
    impl_->evict_lru_locked();
    ptr = std::make_shared<Chunk>(std::move(*found));
    impl_->lru_order.emplace_front(key, ptr);
    impl_->lru_map[key] = impl_->lru_order.begin();
    it = impl_->lru_map.find(key);
  }
  impl_->lru_order.splice(impl_->lru_order.begin(), impl_->lru_order, it->second);
  auto& resident = it->second->second;
  if (!resident)
    return std::nullopt;
  return ChunkLease(*resident);
}

bool AnvilWorld::set_block_state(int cx, int cz, int x, int y, int z, const BlockStateKey& key) {
  if (!valid_)
    return false;
  std::lock_guard<std::recursive_mutex> lock(impl_->mu);
  const std::pair<int, int> ckey = std::make_pair(cx, cz);
  // Resolve the one object for this column: dirty_store's when already dirty,
  // otherwise the LRU residence (loading from disk or synthesizing a fresh
  // column on a miss). Nothing is copied — the mutation below lands in place
  // and "marking dirty" is sharing the pointer into dirty_store.
  std::shared_ptr<Chunk> ptr;
  auto dit = impl_->dirty_store.find(ckey);
  if (dit != impl_->dirty_store.end()) {
    ptr = dit->second;
    auto it = impl_->lru_map.find(ckey);
    if (it != impl_->lru_map.end())
      impl_->lru_order.splice(impl_->lru_order.begin(), impl_->lru_order, it->second);
  } else {
    auto it = impl_->lru_map.find(ckey);
    if (it != impl_->lru_map.end() && it->second->second) {
      impl_->lru_order.splice(impl_->lru_order.begin(), impl_->lru_order, it->second);
      ptr = it->second->second;
    } else {
      if (std::optional<Chunk> disk = impl_->load_from_disk(cx, cz))
        ptr = std::make_shared<Chunk>(std::move(*disk));
      else
        // Create fresh column for previously ungenerated chunk
        ptr = std::make_shared<Chunk>(
            make_fresh_chunk(cx, cz, impl_->min_section_y, impl_->section_count));
      if (it != impl_->lru_map.end()) {
        it->second->second = ptr; // replace a cached miss
        impl_->lru_order.splice(impl_->lru_order.begin(), impl_->lru_order, it->second);
      } else {
        impl_->evict_lru_locked();
        impl_->lru_order.emplace_front(ckey, ptr);
        impl_->lru_map[ckey] = impl_->lru_order.begin();
      }
    }
  }
  if (!ptr)
    return false;
  Chunk& mutated = *ptr;
  // Vanilla swaps the block entity only when the *block* changes: re-typing one
  // half of a chest into a pair (chest -> chest with another `type`) keeps its
  // items, while chest -> air drops them with the block.
  const BlockStateKey previous = mutated.block_state_at(x, y, z);
  const bool block_type_changed = previous.name != key.name;
  if (!mutated.set_block_state(x, y, z, key))
    return false;
  // The light-input version moves after the write has landed, and only when it
  // changed something: a relight that copied this column before the bump can
  // never be committed over the new blocks (world/world_interface.h's
  // `chunk_content_version`), while re-typing the same state leaves a copy
  // valid. The write is under the same mutex the relight's exclusion takes, so
  // the bump and the copy cannot interleave.
  impl_->versions[ckey] = impl_->next_version++;

  // A real block change removes any block entity that lived there — vanilla
  // drops the block entity with the block, so a broken chest must not leave its
  // contents behind in the record (the caller drops the items).
  const BlockPosKey bep{cx * 16 + x, y, cz * 16 + z};
  if (block_type_changed) {
    for (auto it = mutated.block_entities.begin(); it != mutated.block_entities.end();) {
      if (block_entity_matches(*it, cx * 16 + x, y, cz * 16 + z))
        it = mutated.block_entities.erase(it);
      else
        ++it;
    }
    if (impl_->containers.erase(bep) > 0) {
      impl_->containers_by_chunk[ckey].erase(bep);
      impl_->container_dirty_chunks.insert(ckey);
    }
  }

  // Mark dirty by sharing the object just mutated — no copy, and any LRU
  // residence already points at it, so readers see the change immediately.
  if (dit == impl_->dirty_store.end())
    impl_->dirty_store[ckey] = ptr;
  return true;
}

// The positions this container must cover for the block state at (x, y, z):
// just itself for a single block, or both halves of a chest whose `type`
// property names a real partner (vanilla's DoubleBlockCombiner, which accepts a
// double only when the two blocks are the same block, carry the same facing and
// complementary types).
std::vector<BlockPosKey> AnvilWorld::container_parts_locked(int x, int y, int z,
                                                            const BlockStateKey& state) {
  std::vector<BlockPosKey> parts;
  const BlockPosKey key{x, y, z};
  parts.push_back(key);
  if (!is_double_block(state.name) || chest_type_is(state.properties, "single"))
    return parts;
  const auto partner = chest_partner_pos(key, state.properties);
  if (!partner)
    return parts;
  const BlockStateKey other = impl_->block_state_locked(partner->x, partner->y, partner->z);
  if (other.name != state.name)
    return parts;
  const std::string my_type = chest_type_is(state.properties, "left") ? "left" : "right";
  const std::string want = my_type == "left" ? "right" : "left";
  if (!chest_type_is(other.properties, want))
    return parts;
  // Same facing, or vanilla treats them as separate chests.
  const std::string facing = chest_facing(state.properties);
  if (facing.empty() || chest_facing(other.properties) != facing)
    return parts;
  // Menu order is vanilla's (FIRST = type right): slots [0, 27) in the right
  // half, [27, 54) in the left.
  if (my_type == "right")
    parts = {key, *partner};
  else
    parts = {*partner, key};
  return parts;
}

std::shared_ptr<BlockContainer> AnvilWorld::container_at(int x, int y, int z,
                                                         const BlockStateKey& state) {
  if (!valid_)
    return nullptr;
  std::lock_guard<std::recursive_mutex> lock(impl_->mu);
  auto spec = container_spec_for_block(state.name);
  if (!spec)
    return nullptr;
  const BlockPosKey key{x, y, z};
  const std::vector<BlockPosKey> parts = container_parts_locked(x, y, z, state);
  auto it = impl_->containers.find(key);
  if (it != impl_->containers.end() && it->second->parts == parts)
    return it->second; // already the container this state calls for

  // (Re)build: what this position's state -- which may have just gained or lost
  // a partner -- should resolve to. Live data wins over the persisted record,
  // so items that only exist in memory survive a pair/unpair.
  std::shared_ptr<BlockContainer> existing = it != impl_->containers.end() ? it->second : nullptr;
  auto container = std::make_shared<BlockContainer>(std::string(state.name), *spec, parts);
  for (size_t i = 0; i < parts.size(); ++i) {
    std::vector<ItemStack> items;
    const int idx = existing ? existing->part_index(parts[i]) : -1;
    if (idx >= 0) {
      items = existing->part_items(idx);
    } else {
      auto lit = impl_->containers.find(parts[i]);
      if (lit != impl_->containers.end() && lit->second->total_slots() == spec->slots) {
        items = lit->second->inventory.slots();
      } else {
        items = impl_->be_items_locked(parts[i].x, parts[i].y, parts[i].z, spec->slots);
      }
    }
    const int base = static_cast<int>(i) * spec->slots;
    for (int s = 0; s < spec->slots && s < static_cast<int>(items.size()); ++s)
      container->inventory.set(base + s, items[s]);
  }
  // A cooking container seeds its counters from the persisted record (there is
  // only ever one part, so it is `parts[0]`'s record). A rebuild keeps live
  // state: `existing` already carries what the last save has not written yet.
  if (spec->cooking && existing == nullptr)
    impl_->be_furnace_locked(parts[0].x, parts[0].y, parts[0].z, &container->furnace);
  if (spec->brewing && existing == nullptr)
    impl_->be_brewing_locked(parts[0].x, parts[0].y, parts[0].z, &container->brewing);
  if (existing)
    impl_->unregister_container_locked(*existing);
  // Any single container the new parts used to have is absorbed by this one.
  for (const BlockPosKey& p : parts) {
    auto lit = impl_->containers.find(p);
    if (lit != impl_->containers.end() && lit->second != container)
      impl_->unregister_container_locked(*lit->second);
  }
  impl_->register_container_locked(container);
  return container;
}

std::vector<ItemStack> AnvilWorld::detach_container_part(int x, int y, int z,
                                                         const BlockStateKey& state) {
  if (!valid_)
    return {};
  std::lock_guard<std::recursive_mutex> lock(impl_->mu);
  const BlockPosKey key{x, y, z};
  auto it = impl_->containers.find(key);
  if (it == impl_->containers.end()) {
    // Never opened: the persisted record is the truth, and the half being
    // destroyed has its own record (a double chest's two block entities hold
    // the two halves separately).
    auto spec = container_spec_for_block(state.name);
    if (!spec)
      return {};
    return impl_->be_items_locked(x, y, z, spec->slots);
  }
  auto container = it->second;
  const int idx = container->part_index(key);
  if (idx < 0)
    return {};
  std::vector<ItemStack> dropped = container->part_items(idx);
  impl_->unregister_container_locked(*container);
  for (const BlockPosKey& p : container->parts)
    impl_->container_dirty_chunks.insert(std::make_pair(floor_div(p.x, 16), floor_div(p.z, 16)));
  if (container->is_double()) {
    // The other half survives as a single container holding its own items
    // (vanilla drops only the removed block entity's contents).
    const BlockPosKey survivor = container->parts[1 - idx];
    auto single = std::make_shared<BlockContainer>(container->block, container->spec,
                                                   std::vector<BlockPosKey>{survivor});
    const std::vector<ItemStack> kept = container->part_items(1 - idx);
    for (int i = 0; i < single->spec.slots && i < static_cast<int>(kept.size()); ++i)
      single->inventory.set(i, kept[i]);
    impl_->register_container_locked(single);
  }
  return dropped;
}

std::vector<std::shared_ptr<BlockContainer>> AnvilWorld::live_containers() const {
  std::vector<std::shared_ptr<BlockContainer>> out;
  if (!valid_)
    return out;
  std::lock_guard<std::recursive_mutex> lock(impl_->mu);
  out.reserve(impl_->containers.size());
  // One entry per *container*: a double chest is registered under both halves,
  // so the same shared_ptr turns up twice. Dedup by raw pointer in a hash set
  // rather than a linear scan of `out` — the tick calls this every tick, and a
  // scan made it O(containers^2) on the world mutex.
  std::unordered_set<const BlockContainer*> seen;
  seen.reserve(impl_->containers.size());
  for (const auto& entry : impl_->containers) {
    const std::shared_ptr<BlockContainer>& c = entry.second;
    if (seen.insert(c.get()).second)
      out.push_back(c);
  }
  return out;
}

void AnvilWorld::mark_container_dirty(int x, int y, int z) {
  if (!valid_)
    return;
  std::lock_guard<std::recursive_mutex> lock(impl_->mu);
  auto it = impl_->containers.find(BlockPosKey{x, y, z});
  if (it != impl_->containers.end()) {
    for (const BlockPosKey& p : it->second->parts)
      impl_->container_dirty_chunks.insert(std::make_pair(floor_div(p.x, 16), floor_div(p.z, 16)));
    return;
  }
  impl_->container_dirty_chunks.insert(std::make_pair(floor_div(x, 16), floor_div(z, 16)));
}

bool AnvilWorld::save_chunk(int cx, int cz) {
  if (!valid_)
    return false;
  std::lock_guard<std::recursive_mutex> lock(impl_->mu);
  const auto key = std::make_pair(cx, cz);
  const bool block_dirty = impl_->dirty_store.find(key) != impl_->dirty_store.end();
  const bool container_dirty =
      impl_->container_dirty_chunks.find(key) != impl_->container_dirty_chunks.end();
  if (!block_dirty && !container_dirty) {
    // Nothing to save; if chunk exists in LRU but not dirty, consider it clean.
    return true;
  }
  // Hand the rebuild the column itself: grouping is by reference, so the
  // flush takes one copy per column at the injection boundary (rebuild's
  // `with_entities`) instead of snapshotting it into a staging map as well.
  // The lock is held until rebuild returns, so `src` cannot be evicted or
  // cleared under it.
  const Chunk* src = nullptr;
  std::shared_ptr<Chunk> loaded_keep;
  if (block_dirty) {
    auto it = impl_->dirty_store.find(key);
    if (it == impl_->dirty_store.end())
      return false;
    src = it->second.get();
  } else {
    // Only the block entities changed: rewrite the on-disk column unchanged.
    loaded_keep = impl_->load_locked(cx, cz);
    if (!loaded_keep)
      return true; // nothing persisted yet, nothing to rewrite
    src = loaded_keep.get();
  }

  int rx = floor_div(cx, kRegionSize);
  int rz = floor_div(cz, kRegionSize);
  int lx = cx - rx * kRegionSize;
  int lz = cz - rz * kRegionSize;
  int idx = lz * kRegionSize + lx;
  std::map<int, const Chunk*> one;
  one[idx] = src;
  if (!impl_->rebuild_region_locked(rx, rz, one))
    return false;
  impl_->dirty_store.erase(key);
  impl_->container_dirty_chunks.erase(key);
  return true;
}

bool AnvilWorld::save_all() {
  if (!valid_)
    return false;
  // Hold the lock for the whole flush so concurrent set_block_state cannot
  // insert a new dirty entry between our snapshot and the post-flush clear.
  // Auto-save and game threads both route through this single mutex, so
  // blocking here is bounded (region rebuild is a few ms per region).
  std::lock_guard<std::recursive_mutex> lock(impl_->mu);
  if (impl_->dirty_store.empty() && impl_->container_dirty_chunks.empty())
    return true;
  // Group by reference, not by value: the rebuild serializes from these
  // pointers under this same lock, so each column's one copy happens at the
  // injection boundary instead of in a staging map first. The container-only
  // loads are kept alive by shared ownership until the rebuilds finish (they
  // hold no lock of their own, and the columns are read, never written).
  std::map<std::pair<int, int>, std::map<int, const Chunk*>> by_region;
  std::vector<std::shared_ptr<Chunk>> keepalive;
  keepalive.reserve(impl_->container_dirty_chunks.size());
  for (auto& [ckey, chunk] : impl_->dirty_store) {
    int cx = ckey.first, cz = ckey.second;
    int rx = floor_div(cx, kRegionSize);
    int rz = floor_div(cz, kRegionSize);
    int lx = cx - rx * kRegionSize;
    int lz = cz - rz * kRegionSize;
    int idx = lz * kRegionSize + lx;
    by_region[{rx, rz}][idx] = chunk.get();
  }
  // Container-only changes still need their column rewritten.
  for (const auto& ckey : impl_->container_dirty_chunks) {
    if (impl_->dirty_store.find(ckey) != impl_->dirty_store.end())
      continue; // already queued via the block-dirty set
    auto loaded = impl_->load_locked(ckey.first, ckey.second);
    if (!loaded)
      continue;
    int cx = ckey.first, cz = ckey.second;
    int rx = floor_div(cx, kRegionSize);
    int rz = floor_div(cz, kRegionSize);
    int lx = cx - rx * kRegionSize;
    int lz = cz - rz * kRegionSize;
    int idx = lz * kRegionSize + lx;
    by_region[{rx, rz}][idx] = loaded.get();
    keepalive.push_back(std::move(loaded));
  }
  for (auto& [rkey, chunks] : by_region) {
    if (!impl_->rebuild_region_locked(rkey.first, rkey.second, chunks)) {
      log::error("anvil: save_all failed for region r.", rkey.first, ".", rkey.second, ".mca");
      return false;
    }
  }
  // Clear only the chunks we actually flushed. A container-only chunk whose
  // load failed never made it into by_region, so it stays dirty and is retried
  // on the next save instead of being silently dropped.
  for (auto& [rkey, chunks] : by_region) {
    for (auto& [idx, _] : chunks) {
      int lz = idx / kRegionSize;
      int lx = idx % kRegionSize;
      int rx = rkey.first, rz = rkey.second;
      int cx = rx * kRegionSize + lx;
      int cz = rz * kRegionSize + lz;
      auto k = std::make_pair(cx, cz);
      impl_->dirty_store.erase(k);
      impl_->container_dirty_chunks.erase(k);
    }
  }
  return true;
}

namespace {

// <world>/level.dat, inflated and parsed. Nullopt when the file is absent, is
// not gzip/zlib NBT, or does not parse. ensure_level_dat(), world_spawn() and
// level_seed() are the only readers, and all want the same root.
std::optional<nbt::Value> read_level_dat(const std::filesystem::path& path) {
  std::vector<uint8_t> raw;
  read_file_bytes(path, &raw);
  if (raw.empty())
    return std::nullopt;
  std::vector<uint8_t> nbt;
  if (!inflate_payload(2, raw.data(), raw.size(), &nbt) &&
      !inflate_payload(1, raw.data(), raw.size(), &nbt)) {
    log::warn("anvil: level.dat is not gzip/zlib NBT");
    return std::nullopt;
  }
  return nbt::parse(nbt.data(), nbt.size());
}

// The level compound of a parsed level.dat: `Data` in both layouts, the root
// itself for a bare one.
nbt::Value* level_compound(nbt::Value* root) {
  for (auto& field : root->fields) {
    if (field.first == "Data" && field.second.type == nbt::Tag::Compound)
      return &field.second;
  }
  return root;
}

int level_int(const nbt::Value& level, const char* key, int fallback) {
  const nbt::Value* value = nbt::child(level, key);
  if (value == nullptr)
    return fallback;
  const auto parsed = nbt::as_int(*value);
  return parsed ? static_cast<int>(*parsed) : fallback;
}

bool level_bool(const nbt::Value& level, const char* key) {
  const nbt::Value* value = nbt::child(level, key);
  if (value == nullptr)
    return false;
  const auto parsed = nbt::as_int(*value);
  return parsed && *parsed != 0;
}

float level_float(const nbt::Value& level, const char* key, float fallback) {
  const nbt::Value* value = nbt::child(level, key);
  if (value == nullptr)
    return fallback;
  const auto parsed = nbt::as_float(*value);
  return parsed ? static_cast<float>(*parsed) : fallback;
}

void set_level_field(nbt::Value* level, const std::string& key, nbt::Value value) {
  for (auto& field : level->fields) {
    if (field.first == key) {
      field.second = std::move(value);
      return;
    }
  }
  level->fields.emplace_back(key, std::move(value));
}

// Serialize, gzip and atomically replace one level.dat. The creation path and
// the weather writer share it, so gzip + temp + rename is spelled once.
bool write_level_dat(const std::filesystem::path& path, const nbt::Value& root) {
  std::vector<uint8_t> nbt_bytes = nbt::serialize(root, "");
  std::vector<uint8_t> gz;
  if (!gzip_compress(nbt_bytes, &gz)) {
    log::error("anvil: failed to compress level.dat");
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    log::error("anvil: cannot create ", path.parent_path().string(), ": ", ec.message());
    return false;
  }
  const std::filesystem::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      log::error("anvil: cannot open ", tmp.string(), " for writing");
      return false;
    }
    out.write(reinterpret_cast<const char*>(gz.data()), static_cast<std::streamsize>(gz.size()));
    if (!out)
      return false;
  }
  std::error_code rename_ec;
  std::filesystem::rename(tmp, path, rename_ec);
  if (rename_ec) {
    std::filesystem::remove(path, ec);
    std::filesystem::rename(tmp, path, rename_ec);
    if (rename_ec) {
      log::error("anvil: rename level.dat tmp failed: ", rename_ec.message());
      return false;
    }
  }
  return true;
}

} // namespace

bool AnvilWorld::ensure_level_dat(int data_version, std::optional<int64_t> seed) {
  if (!valid_ || !impl_->world_root)
    return false;
  std::filesystem::path path = *impl_->world_root / "level.dat";
  std::error_code ec;
  const bool existed = std::filesystem::exists(path, ec);
  // Root "" : compound { Data: compound { ... } }
  using namespace nbt;
  Value root;
  if (existed) {
    // An existing file is left alone except for one addition: a seed the caller
    // supplies and the file does not carry. open_world() writes a minimal
    // level.dat on open, so a world a tool has just authored has none yet — and
    // without a seed the client's biome fallback samples the wrong world. A file
    // that does not parse (or already has a seed) is not ours to rewrite.
    if (!seed)
      return true;
    auto parsed = read_level_dat(path);
    if (!parsed || level_seed())
      return true;
    root = std::move(*parsed);
    // Both layouts keep the level fields under Data; a bare root is tolerated
    // (world_spawn() reads that shape too).
    Value* level = &root;
    for (auto& field : root.fields) {
      if (field.first == "Data" && field.second.type == Tag::Compound) {
        level = &field.second;
        break;
      }
    }
    bool inserted = false;
    for (auto& field : level->fields) {
      if (field.first == "WorldGenSettings" && field.second.type == Tag::Compound) {
        field.second.fields.emplace_back("seed", make_long(*seed));
        inserted = true;
        break;
      }
    }
    if (!inserted)
      level->fields.emplace_back("WorldGenSettings",
                                 make_compound({{"seed", make_long(*seed)},
                                                {"generate_features", make_byte(1)},
                                                {"bonus_chest", make_byte(0)}}));
  } else {
    // Build minimal level.dat
    std::vector<std::pair<std::string, Value>> data_fields;
    data_fields.emplace_back("DataVersion", make_int(data_version));
    data_fields.emplace_back("GameType", make_int(1)); // creative by default
    data_fields.emplace_back("LevelName", make_string("world"));
    data_fields.emplace_back("Time", make_long(0));
    data_fields.emplace_back("DayTime", make_long(0));
    data_fields.emplace_back("SpawnX", make_int(8));
    data_fields.emplace_back("SpawnY", make_int(100));
    data_fields.emplace_back("SpawnZ", make_int(8));
    data_fields.emplace_back("SpawnAngle", make_float(0.0f));
    // Modern spawn compound for 26.x
    data_fields.emplace_back("spawn", make_compound({
                                          {"pos", make_int_array({8, 100, 8})},
                                          {"pitch", make_float(0.0f)},
                                          {"yaw", make_float(0.0f)},
                                          {"dimension", make_string("minecraft:overworld")},
                                      }));
    data_fields.emplace_back("Version", make_compound({
                                            {"Id", make_int(data_version)},
                                            {"Name", make_string("26.3")},
                                            {"Series", make_string("main")},
                                            {"Snapshot", make_byte(0)},
                                        }));
    // The seed the terrain came from, in 26.x's own place. Without it a file
    // this server authored has no seed to advertise in Join Game's `hashedSeed`
    // (see level_seed()); a caller that generated terrain writes it here.
    if (seed)
      data_fields.emplace_back("WorldGenSettings",
                               make_compound({{"seed", make_long(*seed)},
                                              {"generate_features", make_byte(1)},
                                              {"bonus_chest", make_byte(0)}}));
    data_fields.emplace_back("WasModded", make_byte(0));
    data_fields.emplace_back("initialized", make_byte(1));
    data_fields.emplace_back("allowCommands", make_byte(1));
    data_fields.emplace_back("Difficulty", make_byte(1));
    Value data_comp = make_compound(std::move(data_fields));
    root = make_compound({{"Data", std::move(data_comp)}});
  }
  if (!write_level_dat(path, root))
    return false;
  log::info(existed ? "updated " : "created ", path.string());
  return true;
}

const std::filesystem::path& AnvilWorld::world_root() const {
  static const std::filesystem::path empty;
  if (impl_ && impl_->world_root)
    return *impl_->world_root;
  return empty;
}

std::optional<std::tuple<double, double, double>> AnvilWorld::world_spawn() const {
  if (!valid_ || !impl_->world_root)
    return std::nullopt;
  auto root = read_level_dat(*impl_->world_root / "level.dat");
  if (!root)
    return std::nullopt;
  // Level data: the world-root compound is "Data" in both layouts; older
  // worlds put spawn in Data.SpawnX/Y/Z, 26.x nests it in Data.spawn.pos
  // (int[3]) with the player's facing alongside.
  const nbt::Value* data = &*root;
  if (nbt::child(*data, "SpawnX") == nullptr && nbt::child(*data, "spawn") == nullptr) {
    const nbt::Value* inner = nbt::child(*data, "Data");
    if (inner == nullptr)
      return std::nullopt;
    data = inner;
  }
  if (const nbt::Value* spawn = nbt::child(*data, "spawn"); spawn != nullptr) {
    const nbt::Value* pc = nbt::child(*spawn, "pos");
    const auto* pos = pc ? nbt::as_ints(*pc) : nullptr;
    if (pos == nullptr || pos->size() != 3)
      return std::nullopt;
    return std::tuple<double, double, double>(static_cast<double>((*pos)[0]) + 0.5,
                                              static_cast<double>((*pos)[1]),
                                              static_cast<double>((*pos)[2]) + 0.5);
  }
  auto sx = nbt::as_int(*nbt::child(*data, "SpawnX"));
  auto sy = nbt::as_int(*nbt::child(*data, "SpawnY"));
  auto sz = nbt::as_int(*nbt::child(*data, "SpawnZ"));
  if (!sx || !sy || !sz)
    return std::nullopt;
  return std::tuple<double, double, double>(
      static_cast<double>(*sx) + 0.5, static_cast<double>(*sy), static_cast<double>(*sz) + 0.5);
}

std::optional<int64_t> AnvilWorld::level_seed() const {
  if (!valid_ || !impl_->world_root)
    return std::nullopt;
  auto root = read_level_dat(*impl_->world_root / "level.dat");
  if (!root)
    return std::nullopt;
  // The world's generator settings sit under "Data" in both layouts; a root
  // that already carries them is tolerated, as world_spawn() does. 26.x writes
  // Data.WorldGenSettings.seed; 1.16-1.17 wrote Data.Seed; pre-1.16 worlds wrote
  // Data.RandomSeed. The first that parses as a long wins — the same seed the
  // file's terrain was generated from, which is what Join Game hashes.
  const nbt::Value* levels[2] = {&*root, nbt::child(*root, "Data")};
  for (const nbt::Value* level : levels) {
    if (level == nullptr)
      continue;
    if (const nbt::Value* settings = nbt::child(*level, "WorldGenSettings")) {
      if (const nbt::Value* seed = nbt::child(*settings, "seed")) {
        if (auto value = nbt::as_int(*seed))
          return value;
      }
    }
    for (const char* key : {"Seed", "RandomSeed"}) {
      if (const nbt::Value* seed = nbt::child(*level, key)) {
        if (auto value = nbt::as_int(*seed))
          return value;
      }
    }
  }
  return std::nullopt;
}

std::optional<AnvilWorld::LevelWeather> AnvilWorld::level_weather() const {
  if (!valid_ || !impl_->world_root)
    return std::nullopt;
  auto root = read_level_dat(*impl_->world_root / "level.dat");
  if (!root)
    return std::nullopt;
  const nbt::Value* level = level_compound(&*root);
  LevelWeather weather;
  weather.rain_time = level_int(*level, "rainTime", 0);
  weather.thunder_time = level_int(*level, "thunderTime", 0);
  weather.raining = level_bool(*level, "raining");
  weather.thundering = level_bool(*level, "thundering");
  weather.rain_level = level_float(*level, "rainLevel", 0.0f);
  weather.thunder_level = level_float(*level, "thunderLevel", 0.0f);
  return weather;
}

bool AnvilWorld::save_level_weather(const LevelWeather& weather) {
  if (!valid_ || !impl_->world_root)
    return false;
  const std::filesystem::path path = *impl_->world_root / "level.dat";
  auto root = read_level_dat(path);
  // A world with no level.dat yet is not ours to invent here: open_world()
  // creates the minimal file before this is ever called.
  if (!root)
    return false;
  nbt::Value* level = level_compound(&*root);
  set_level_field(level, "rainTime", nbt::make_int(weather.rain_time));
  set_level_field(level, "thunderTime", nbt::make_int(weather.thunder_time));
  set_level_field(level, "raining", nbt::make_byte(weather.raining ? 1 : 0));
  set_level_field(level, "thundering", nbt::make_byte(weather.thundering ? 1 : 0));
  // +MC's additive pair: the ramped levels themselves, so a restart resumes
  // the fade where it stopped. Vanilla ignores the two unknown tags.
  set_level_field(level, "rainLevel", nbt::make_float(weather.rain_level));
  set_level_field(level, "thunderLevel", nbt::make_float(weather.thunder_level));
  return write_level_dat(path, *root);
}

std::optional<Chunk> AnvilWorld::parse_chunk_record(const uint8_t* nbt, size_t len,
                                                    int min_section_y, int section_count) {
  Chunk chunk;
  auto root = nbt::parse(nbt, len);
  if (!root)
    return std::nullopt;
  if (!parse_chunk_nbt(*root, min_section_y, section_count, &chunk))
    return std::nullopt;
  return chunk;
}

} // namespace mc::world
