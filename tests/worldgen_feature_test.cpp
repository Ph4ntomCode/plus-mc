// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 4 milestone K's feature stage: the registries the generator now carries
// (configured/placed features, carvers, structures, structure sets) and the
// decoration pass that applies the ore kind.
//
// The registry checks pin the *carry*: every pack id is in the generated table,
// every placed feature names a configured one, every biome decoration entry
// names a placed feature, and the counts cannot silently shrink.
//
// The decoration checks pin the *stage*: filling chunk (0,0) at seed 42 and
// decorating it changes only cells whose new state one of the applied feature
// kinds' configs names, applies every kind the stage claims (ore, scattered
// ore, simple block, block pile, disk, spring, block column, monster room, tree,
// seagrass, fallen tree, the three selectors, ice spike, geode), pins the
// seagrass rule on the river-at-surface chunk (8,7), pins the fallen-tree rule
// -- and both of its decorators -- on a constructed `wooded_badlands` chunk (the
// stage is fed a noise+biome chunk, which has no surface layer, so the soil its
// placement demands has to be constructed before the kind can be reached at
// all), pins the ice spike on a constructed `ice_spikes` chunk and the geode on
// a generated chunk advanced to its rarity draw, pins the fossil kind against
// the 26.2 server's own template-transform table and a constructed column (both
// processor passes, the protected-block gate, the empty-corner rejection, and a
// decorated chunk where the rarity draw lets the kind run), and records the
// kinds it still does not apply instead of dropping them, and is deterministic
// (a second fill+decorate is byte-identical). It is
// *not* compared to a vanilla dump — the random sequence is this port's own,
// which the header names — so instead it is compared to a checked-in **golden
// dump of this runtime's own output** (`feature_meta.tsv` plus one gzipped cell
// file per chunk, written by the `plusmc-feature-dump` tool): that is what keeps
// a change to the decoration stream from landing silently, and it is the only
// kind of fixture the feature stage can have.
//
// One thing the decoration half deliberately does *not* assert is a natural
// block-column *placement* in this region: of the pack's eight block_column
// features, `cactus` needs a badlands/desert biome (absent at seed 42 near the
// origin), `cave_vine`/`cave_vine_in_moss` need the `environment_scan` modifier
// the stage does not apply yet, and `sugar_cane` needs a shoreline cell inside
// its twenty-candidate spread. What *is* asserted is the dispatch: the kind's
// own application counter shows its placement ran in chunk (0,0), and every
// state the kind can write is in the checked set of legal new states.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <zlib.h>

#include "world/block_tags.h"
#include "world/chunk.h"
#include "world/container.h" // block_property
#include "world/generated_chunk.h"
#include "world/leaves.h"
#include "world/living_entity_table.h"
#include "world/registry.h" // full_block_states, NameListBiomeTable
#include "world/worldgen_carver_table.h"
#include "world/worldgen_chunk.h"
#include "world/worldgen_feature.h"
#include "world/worldgen_feature_table.h"
#include "world/worldgen_fossil.h"
#include "world/worldgen_fossil_table.h"
#include "world/worldgen_structure.h"
#include "world/worldgen_structure_piece.h"
#include "world/worldgen_structure_piece_table.h"
#include "world/worldgen_structure_table.h"

using namespace mc::world;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    ++checks;                                                                                      \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECK_EQ(a, b)                                                                             \
  do {                                                                                             \
    ++checks;                                                                                      \
    const long long va = static_cast<long long>(a), vb = static_cast<long long>(b);                \
    if (va != vb) {                                                                                \
      std::fprintf(stderr, "FAIL %s:%d: %s == %lld, want %lld\n", __FILE__, __LINE__, #a, va, vb); \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

namespace {

template <typename Row> bool sorted_by_name(const Row* rows, size_t count) {
  for (size_t i = 1; i < count; ++i)
    if (std::strcmp(rows[i - 1].name, rows[i].name) >= 0)
      return false;
  return true;
}

template <typename Row> const Row* find_by_name(const Row* rows, size_t count, const char* name) {
  const Row* end = rows + count;
  const Row* it = std::lower_bound(rows, end, name, [](const Row& row, const char* key) {
    return std::strcmp(row.name, key) < 0;
  });
  if (it == end || std::strcmp(it->name, name) != 0)
    return nullptr;
  return it;
}

// The decoration step a biome's own feature list places this feature at, or -1
// when the biome does not list it. Used to tie a kind's reachability to the
// biome the pinned chunk actually carries.
int placed_step_for_biome(const char* biome, const char* placed_name) {
  const WgBiomeFeatures* end = kWgBiomeFeatures + std::size(kWgBiomeFeatures);
  const WgBiomeFeatures* row =
      std::lower_bound(kWgBiomeFeatures, end, biome, [](const WgBiomeFeatures& r, const char* key) {
        return std::strcmp(r.biome, key) < 0;
      });
  if (row == end || std::strcmp(row->biome, biome) != 0)
    return -1;
  for (uint32_t i = 0; i < row->count; ++i) {
    const WgBiomeFeatureEntry& entry = kWgBiomeFeatureEntries[row->offset + i];
    if (std::strcmp(entry.placed_feature, placed_name) == 0)
      return static_cast<int>(entry.step);
  }
  return -1;
}

// Every state a state-provider row can write, following a drawn-integer
// provider to its own source.
void collect_provider_states(uint32_t index, std::set<std::string>& out) {
  if (index >= std::size(kWgStateProviders))
    return;
  const WgStateProvider& p = kWgStateProviders[index];
  if (p.kind == WgStateProviderKind::kWeighted) {
    for (uint32_t i = 0; i < p.count; ++i)
      out.insert(kWgWeightedStates[p.offset + i].state.name);
    return;
  }
  if (p.kind == WgStateProviderKind::kRandomizedInt) {
    collect_provider_states(p.source, out);
    return;
  }
  if (p.state.name[0] != '\0')
    out.insert(p.state.name);
}

// A `|`-joined name list membership test (the generated tables' own form).
bool name_in_pipe_list(std::string_view list, std::string_view name) {
  size_t start = 0;
  while (start <= list.size()) {
    const size_t bar = list.find('|', start);
    const std::string_view piece =
        list.substr(start, bar == std::string_view::npos ? std::string_view::npos : bar - start);
    if (piece == name)
      return true;
    if (bar == std::string_view::npos)
      break;
    start = bar + 1;
  }
  return false;
}

std::string cell_name(const Chunk& chunk, size_t section, int cell) {
  if (section >= chunk.sections.size())
    return "minecraft:air";
  const Section& s = chunk.sections[section];
  if (!s.has_blocks || s.block_cells.size() != static_cast<size_t>(kBlockCellsPerSection))
    return "minecraft:air";
  const int32_t p = s.block_cells[static_cast<size_t>(cell)];
  if (p < 0 || static_cast<size_t>(p) >= s.block_palette.size())
    return "minecraft:air";
  return s.block_palette[static_cast<size_t>(p)].name;
}

// How many cells in a chunk carry a block of this name (property-blind, which
// is all `cell_name` can say; the golden dump compares canonical states).
int count_named(const Chunk& chunk, const char* name) {
  int n = 0;
  for (size_t si = 0; si < chunk.sections.size(); ++si)
    for (int cell = 0; cell < kBlockCellsPerSection; ++cell)
      if (cell_name(chunk, si, cell) == name)
        ++n;
  return n;
}

// What one chunk's decoration left behind of a monster room. The feature writes
// within `xr + 1` (at most 4) of its origin in x/z and `-1..4` in y, and leaves
// a spawner at the origin, so every *shell* block has to sit inside that box
// around the spawner: a block outside it means the carve leaked.
struct RoomScan {
  int spawners = 0;
  int chests = 0;
  int cobble = 0;
  int mossy = 0;
  int cave_air = 0;
  int outside_footprint = 0;
};

// ---------------------------------------------------------------------------
// The golden decoration dump.
// ---------------------------------------------------------------------------

// The fixtures dir, from argv[1] (CMake passes tests/fixtures/worldgen).
std::string g_fixtures;

std::vector<std::string> split_tabs(const std::string& line) {
  std::vector<std::string> fields;
  size_t start = 0;
  while (true) {
    const size_t tab = line.find('\t', start);
    if (tab == std::string::npos) {
      fields.push_back(line.substr(start));
      break;
    }
    fields.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
  return fields;
}

int32_t parse_int(const std::string& text) {
  return static_cast<int32_t>(std::strtol(text.c_str(), nullptr, 10));
}

std::string canonical_state_of(const BlockStateKey& key) {
  std::string text = key.name;
  if (!key.properties.empty()) {
    text += '[';
    text += key.properties;
    text += ']';
  }
  return text;
}

// The `;`-separated `code=state` palette of one meta row. The separator is ';'
// rather than ',' because a state's own property list is comma-separated
// (`minecraft:oak_leaves[distance=7,persistent=false,waterlogged=false]`), which
// a decorated chunk's palette is full of.
std::vector<std::string> parse_state_codes(const std::string& text) {
  std::vector<std::string> palette;
  size_t start = 0;
  while (true) {
    const size_t semi = text.find(';', start);
    const std::string entry =
        semi == std::string::npos ? text.substr(start) : text.substr(start, semi - start);
    if (!entry.empty()) {
      const size_t equals = entry.find('=');
      const size_t code = static_cast<size_t>(parse_int(entry.substr(0, equals)));
      if (code >= palette.size())
        palette.resize(code + 1);
      palette[code] = entry.substr(equals + 1);
    }
    if (semi == std::string::npos)
      break;
    start = semi + 1;
  }
  return palette;
}

std::vector<uint8_t> read_gzip(const std::string& name) {
  const std::string path = g_fixtures + "/" + name;
  gzFile file = gzopen(path.c_str(), "rb");
  if (file == nullptr) {
    std::fprintf(stderr, "FAIL cannot open fixture %s\n", path.c_str());
    ++failures;
    return {};
  }
  std::vector<uint8_t> data;
  uint8_t buffer[4096];
  int got = 0;
  while ((got = gzread(file, buffer, sizeof(buffer))) > 0)
    data.insert(data.end(), buffer, buffer + got);
  gzclose(file);
  return data;
}

RoomScan scan_room(const Chunk& chunk) {
  RoomScan r;
  int sx = 0;
  int sy = 0;
  int sz = 0;
  for (size_t si = 0; si < chunk.sections.size(); ++si)
    for (int cell = 0; cell < kBlockCellsPerSection; ++cell)
      if (cell_name(chunk, si, cell) == "minecraft:spawner") {
        ++r.spawners;
        sx = cell % 16;
        sz = (cell % 256) / 16;
        sy = (chunk.min_section_y + static_cast<int>(si)) * 16 + cell / 256;
      }
  if (r.spawners != 1)
    return r;
  for (size_t si = 0; si < chunk.sections.size(); ++si)
    for (int cell = 0; cell < kBlockCellsPerSection; ++cell) {
      const std::string name = cell_name(chunk, si, cell);
      const bool shell = name == "minecraft:chest" || name == "minecraft:cobblestone" ||
                         name == "minecraft:mossy_cobblestone";
      if (name == "minecraft:chest")
        ++r.chests;
      else if (name == "minecraft:cobblestone")
        ++r.cobble;
      else if (name == "minecraft:mossy_cobblestone")
        ++r.mossy;
      else if (name == "minecraft:cave_air")
        ++r.cave_air;
      else
        continue;
      const int lx = cell % 16;
      const int lz = (cell % 256) / 16;
      const int wy = (chunk.min_section_y + static_cast<int>(si)) * 16 + cell / 256;
      if (shell && (std::abs(lx - sx) > 4 || std::abs(lz - sz) > 4 || wy < sy - 1 || wy > sy + 4))
        ++r.outside_footprint;
    }
  return r;
}

// Re-decorate every chunk the golden dump names and compare it cell for cell,
// plus the ledger the dump recorded beside the cells. The ports that predate
// this file compared against the game jar's own dumps; the feature stage cannot
// (the random stream is this port's own), so the fixture it compares against is
// this runtime's own frozen output.
void test_golden_decoration() {
  const std::string meta_path = g_fixtures + "/feature_meta.tsv";
  std::ifstream meta(meta_path);
  if (!meta) {
    std::fprintf(stderr, "FAIL cannot open fixture %s\n", meta_path.c_str());
    ++failures;
    return;
  }
  size_t chunks = 0;
  size_t cells_total = 0;
  std::string line;
  while (std::getline(meta, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    const std::vector<std::string> f = split_tabs(line);
    if (f.size() < 15) {
      std::fprintf(stderr, "FAIL feature_meta.tsv row has %zu fields, want 15\n", f.size());
      ++failures;
      continue;
    }
    CHECK_EQ(parse_int(f[0]), 42);
    const int32_t chunk_x = parse_int(f[1]);
    const int32_t chunk_z = parse_int(f[2]);
    CHECK_EQ(parse_int(f[3]), kWgMinY);
    CHECK_EQ(parse_int(f[4]), kWgHeight);
    const size_t cells = static_cast<size_t>(16) * 16 * static_cast<size_t>(kWgHeight);
    CHECK_EQ(parse_int(f[5]), static_cast<int32_t>(cells));
    CHECK_EQ(parse_int(f[6]), 2); // the codes are uint16 wide
    const std::vector<std::string> palette = parse_state_codes(f[7]);
    CHECK(palette.size() > 1);

    const std::string file = "feature_seed" + std::to_string(parse_int(f[0])) + "_c" +
                             std::to_string(chunk_x) + "_" + std::to_string(chunk_z) + ".bin.gz";
    const std::vector<uint8_t> bytes = read_gzip(file);
    CHECK_EQ(bytes.size(), static_cast<long long>(cells) * 2);
    if (bytes.size() != cells * 2)
      continue;

    WgOverworldGenerator generator(42);
    Chunk chunk;
    fill_generated_chunk(generator, chunk_x, chunk_z, chunk);
    WgDecorationStats stats;
    decorate_generated_chunk(generator, chunk, &stats);

    // The ledger travels with the cells, so a change that moves an application
    // between `applied` and `skipped_kind` without moving a block still fails.
    CHECK_EQ(parse_int(f[8]), static_cast<int32_t>(stats.considered));
    CHECK_EQ(parse_int(f[9]), static_cast<int32_t>(stats.applied));
    CHECK_EQ(parse_int(f[10]), static_cast<int32_t>(stats.skipped_kind));
    CHECK_EQ(parse_int(f[11]), static_cast<int32_t>(stats.skipped_modifier));
    CHECK_EQ(parse_int(f[12]), static_cast<int32_t>(stats.positions));
    CHECK_EQ(parse_int(f[13]), static_cast<int32_t>(stats.out_of_chunk));
    CHECK_EQ(parse_int(f[14]), static_cast<int32_t>(stats.blocks_placed));

    size_t mismatches = 0;
    for (size_t x = 0; x < 16; ++x) {
      for (size_t z = 0; z < 16; ++z) {
        for (size_t y = 0; y < static_cast<size_t>(kWgHeight); ++y) {
          const size_t i =
              x * (static_cast<size_t>(kWgHeight) * 16) + z * static_cast<size_t>(kWgHeight) + y;
          const uint16_t code = static_cast<uint16_t>(bytes[i * 2] | (bytes[i * 2 + 1] << 8));
          const std::string want =
              code < palette.size() ? palette[code] : std::string("<bad-fixture-code>");
          const std::string actual = canonical_state_of(chunk.block_state_at(
              static_cast<int>(x), static_cast<int>(y) + kWgMinY, static_cast<int>(z)));
          if (actual == want)
            continue;
          if (mismatches < 12) {
            std::fprintf(stderr, "FAIL %s (%zu, %d, %zu): got %s want %s\n", file.c_str(), x,
                         static_cast<int>(y) + kWgMinY, z, actual.c_str(), want.c_str());
          }
          ++mismatches;
        }
      }
    }
    CHECK_EQ(mismatches, 0);
    if (mismatches == 0)
      std::printf("    %s: %zu cells state-identical\n", file.c_str(), cells);
    else
      std::printf("    %s: %zu/%zu cells differ\n", file.c_str(), mismatches, cells);
    ++chunks;
    cells_total += cells;
  }
  CHECK_EQ(chunks, 6); // four warm chunks + the two cold ones (snow and ice)
  std::printf("  golden decoration: %zu chunks, %zu cells compared\n", chunks, cells_total);
}

// ---- the generated canopy's leaf distances ----------------------------------

// A square of generated columns addressed in world coordinates. The feature
// stage writes only into the column it decorates (`worldgen_feature.cpp`'s
// `try_set`), so a square built chunk by chunk is exactly what a world holds,
// and a canopy that spans a chunk border is judged with the neighbours it has
// instead of against a column-local fiction.
struct ChunkSquare {
  // Row-major over the square: `(z - origin_z) * side + (x - origin_x)` chunks.
  std::vector<const Chunk*> chunks;
  int side = 0;
  int origin_x = 0;
  int origin_z = 0;
  int min_y = 0;
  int height = 0;

  bool in_range(int y) const {
    return y >= min_y && y < min_y + height;
  }

  const Chunk* chunk_at(int world_x, int world_z) const {
    const int ci = ((world_z >> 4) - origin_z) * side + ((world_x >> 4) - origin_x);
    if (ci < 0 || ci >= static_cast<int>(chunks.size()))
      return nullptr;
    const Chunk* chunk = chunks[static_cast<size_t>(ci)];
    if (chunk == nullptr || chunk->x != (world_x >> 4) || chunk->z != (world_z >> 4))
      return nullptr;
    return chunk;
  }

  bool is_leaf(int x, int y, int z) const {
    const Chunk* chunk = chunk_at(x, z);
    return chunk != nullptr && in_range(y) &&
           leaves_is_leaves(chunk->block_state_at(x & 15, y, z & 15).name);
  }

  bool is_anchor(int x, int y, int z) const {
    const Chunk* chunk = chunk_at(x, z);
    return chunk != nullptr && in_range(y) &&
           block_has_tag(kBlockTagPreventsNearbyLeafDecay,
                         chunk->block_state_at(x & 15, y, z & 15).name);
  }
};

// `LeavesBlock.updateDistance`'s fixed point over a square, computed from the
// cells alone — no stored distance is read, which is what makes it an
// independent answer: every `#minecraft:prevents_nearby_leaf_decay` cell is a
// source at distance 0, a leaf one step from a cell at `d` is at `d + 1`, and a
// leaf no anchor reaches within the decay distance stays there. Indexed
// `((y - min_y) * wide + (z - origin_z * 16)) * wide + (x - origin_x * 16)` with
// `wide = side * 16`.
std::vector<int> leaf_distance_fixed_point(const ChunkSquare& square) {
  const int wide = square.side * 16;
  const int x0 = square.origin_x * 16;
  const int z0 = square.origin_z * 16;
  const auto index = [&](int x, int y, int z) {
    return static_cast<size_t>((y - square.min_y) * wide * wide + (z - z0) * wide + (x - x0));
  };
  std::vector<int> dist(static_cast<size_t>(wide) * wide * square.height, kLeavesDecayDistance);
  std::vector<std::array<int, 3>> queue;
  for (int y = square.min_y; y < square.min_y + square.height; ++y)
    for (int z = z0; z < z0 + wide; ++z)
      for (int x = x0; x < x0 + wide; ++x)
        if (square.is_anchor(x, y, z)) {
          dist[index(x, y, z)] = 0;
          queue.push_back({x, y, z});
        }
  static constexpr int kSteps[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                       {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
  for (size_t head = 0; head < queue.size(); ++head) {
    const int x = queue[head][0], y = queue[head][1], z = queue[head][2];
    const int here = dist[index(x, y, z)];
    if (here >= kLeavesDecayDistance - 1)
      continue;
    for (const auto& step : kSteps) {
      const int nx = x + step[0], ny = y + step[1], nz = z + step[2];
      if (nx < x0 || nx >= x0 + wide || nz < z0 || nz >= z0 + wide || !square.in_range(ny))
        continue;
      if (!square.is_leaf(nx, ny, nz))
        continue;
      int& d = dist[index(nx, ny, nz)];
      if (d <= here + 1)
        continue;
      d = here + 1;
      queue.push_back({nx, ny, nz});
    }
  }
  return dist;
}

// The leaf state a generated canopy carries. Vanilla reaches the propagated
// distances from the block updates a feature's `setBlock` runs: a leaf whose
// neighbour changed schedules `LeavesBlock.tick`, the ticks walk the distance
// out from the trunk one layer at a time, and the chunk a server persists — and
// streams — carries distances 1..7. This generator writes cells directly and
// ticks nothing, so before `settle_generated_leaf_distances` every leaf a tree
// placed kept the registry default `distance=7,persistent=false`, which
// `leaves_decaying` answers true for: a freshly generated canopy was one every
// random tick would eat, and its first drop woke the standing reschedule load
// `docs/PHASE3.md` records (`docs/PHASE5.md` measures what that did to a served
// world).
//
// The check is the rule replayed over the finished cells, cell for cell, from
// this file's own breadth-first walk rather than from the settle's code. Each
// column is judged against its *own* cells — the only cells generation can see
// when it settles one — and the square's answer is reported beside it, because a
// leaf whose only path to wood crosses a border is one its own column cannot
// anchor.
//
// What the assertions deliberately leave alone: a leaf no anchor reaches within
// the decay distance is decaying *by the rule* (vanilla's random tick drops it,
// and so does this engine's), so the residual the summary line prints is not a
// defect the generator can settle away. `docs/PHASE5.md` records its size and
// why the shapes have it at all.
void test_generated_canopy_distances() {
  constexpr int kRadius = 1; // 3x3: the canopy at seed 42 (dark oak) spans borders
  constexpr int64_t kSeed = 42;
  const int side = 2 * kRadius + 1;
  WgOverworldGenerator generator(kSeed);
  std::vector<Chunk> columns;
  for (int cz = -kRadius; cz <= kRadius; ++cz)
    for (int cx = -kRadius; cx <= kRadius; ++cx) {
      Chunk chunk;
      fill_generated_chunk(generator, cx, cz, chunk);
      decorate_generated_chunk(generator, chunk, nullptr);
      columns.push_back(std::move(chunk));
    }

  ChunkSquare square;
  square.side = side;
  square.origin_x = -kRadius;
  square.origin_z = -kRadius;
  square.min_y = columns.front().min_section_y * 16;
  square.height = 16 * static_cast<int>(columns.front().sections.size());
  for (const Chunk& column : columns)
    square.chunks.push_back(&column);
  const std::vector<int> world_dist = leaf_distance_fixed_point(square);

  long long leaves = 0;
  long long reachable = 0;              // leaves the column's own cells reach from wood
  long long reachable_decaying = 0;     // ... left decaying: the defect this stage had
  long long reachable_wrong = 0;        // ... settled to something the rule does not say
  long long unanchored = 0;             // leaves no wood reaches within the decay distance
  long long unanchored_settled = 0;     // ... left at the decay distance, as the rule says
  long long anchored_in_world_only = 0; // ... of those, anchored by a neighbour column
  for (int cz = -kRadius; cz <= kRadius; ++cz)
    for (int cx = -kRadius; cx <= kRadius; ++cx) {
      const Chunk& chunk = columns[static_cast<size_t>((cz + kRadius) * side + (cx + kRadius))];
      ChunkSquare column;
      column.chunks.push_back(&chunk);
      column.side = 1;
      column.origin_x = cx;
      column.origin_z = cz;
      column.min_y = square.min_y;
      column.height = square.height;
      const std::vector<int> dist = leaf_distance_fixed_point(column);
      for (int y = square.min_y; y < square.min_y + square.height; ++y)
        for (int z = 0; z < 16; ++z)
          for (int x = 0; x < 16; ++x) {
            const BlockStateKey state = chunk.block_state_at(x, y, z);
            if (!leaves_is_leaves(state.name))
              continue;
            ++leaves;
            const int want = dist[static_cast<size_t>((y - square.min_y) * 256 + z * 16 + x)];
            const int stored = leaves_distance(state);
            if (want < kLeavesDecayDistance) {
              ++reachable;
              if (stored == want)
                continue;
              if (leaves_decaying(state))
                ++reachable_decaying;
              else
                ++reachable_wrong;
              continue;
            }
            ++unanchored;
            if (stored == kLeavesDecayDistance)
              ++unanchored_settled;
            const int wide = side * 16;
            if (world_dist[static_cast<size_t>((y - square.min_y) * wide * wide +
                                               (z + kRadius * 16) * wide + (x + kRadius * 16))] <
                kLeavesDecayDistance)
              ++anchored_in_world_only;
          }
    }

  CHECK(leaves > 1000); // non-vacuous: seed 42's square is a real canopy
  CHECK(reachable > 0);
  CHECK_EQ(reachable_decaying, 0);          // no leaf the column can anchor is left decaying
  CHECK_EQ(reachable_wrong, 0);             // and every stored distance is the rule's own
  CHECK_EQ(unanchored_settled, unanchored); // 7 is left only where no wood reaches
  std::printf("  generated canopy: %lld leaves, %lld anchored and settled against the rule, "
              "%lld/%lld residual at distance %d (%lld of them anchored only through a "
              "neighbour column)\n",
              leaves, reachable, unanchored, leaves, kLeavesDecayDistance, anchored_in_world_only);
}

// ---- fossils ---------------------------------------------------------------

struct FossilScan {
  int bone = 0;
  int coal = 0;
  int diamond = 0;
  int other = 0;
  int min_y = kWgMinY + kWgHeight;
  int max_y = kWgMinY;
  uint64_t digest = 1469598103934665603ull;
};

void scan_fossil(const Chunk& chunk, FossilScan* out) {
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z)
      for (int y = kWgMinY; y < kWgMinY + kWgHeight; ++y) {
        const BlockStateKey state = chunk.block_state_at(x, y, z);
        if (state.name == "minecraft:bone_block")
          ++out->bone;
        else if (state.name == "minecraft:coal_ore")
          ++out->coal;
        else if (state.name == "minecraft:deepslate_diamond_ore")
          ++out->diamond;
        else
          continue;
        out->min_y = std::min(out->min_y, y);
        out->max_y = std::max(out->max_y, y);
        // The digest folds the state, not only the counts, so two placements
        // that swap an axis or move a cell still differ.
        for (const char c : state.name)
          out->digest = (out->digest ^ static_cast<uint8_t>(c)) * 1099511628211ull;
        for (const char c : state.properties)
          out->digest = (out->digest ^ static_cast<uint8_t>(c)) * 1099511628211ull;
        out->digest = (out->digest ^ static_cast<uint64_t>(x)) * 1099511628211ull;
        out->digest = (out->digest ^ static_cast<uint64_t>(z)) * 1099511628211ull;
        out->digest = (out->digest ^ static_cast<uint64_t>(y + 64)) * 1099511628211ull;
      }
}

// A constructed column the height/empty-corner scans can read: stone through
// `top`, air above, one biome in every section.
void fill_fossil_chunk(Chunk& chunk, int top, const char* biome_name) {
  // A default Chunk has no section stack; `set_block_state` cannot write into
  // one. Build the same 24-section column `fill_generated_chunk` builds.
  chunk.min_section_y = kWgMinY / 16;
  chunk.sections.assign(static_cast<size_t>(kWgHeight / 16), Section{});
  for (int i = 0; i < kWgHeight / 16; ++i) {
    Section& section = chunk.sections[static_cast<size_t>(i)];
    section.y = chunk.min_section_y + i;
    section.has_blocks = true;
    section.block_cells.assign(kBlockCellsPerSection, 0);
    section.has_biomes = true;
    section.biome_cells.assign(kBiomeCellsPerSection, 0);
  }
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z) {
      for (int y = kWgMinY; y <= top; ++y)
        chunk.set_block_state(x, y, z, BlockStateKey{"minecraft:stone", ""});
      for (int y = top + 1; y < kWgMinY + kWgHeight; ++y)
        chunk.set_block_state(x, y, z, BlockStateKey{"minecraft:air", ""});
    }
  for (Section& section : chunk.sections) {
    section.has_biomes = true;
    section.biome_palette.assign(1, biome_name);
    section.biome_cells.assign(kBiomeCellsPerSection, 0);
  }
}

void test_fossils() {
  // `StructureTemplate.getSize`/`getZeroPositionWithTransform` for Mirror.NONE,
  // pinned against the values the 26.2 server's own class prints. A template
  // 3 x 4 x 13: a quarter turn swaps the horizontal extents, and the zero
  // position is what puts the rotated origin back over the footprint.
  {
    const WgFossilTransform t0 = wg_fossil_transform(3, 4, 13, 0);
    CHECK_EQ(t0.size_x, 3);
    CHECK_EQ(t0.size_y, 4);
    CHECK_EQ(t0.size_z, 13);
    CHECK_EQ(t0.zero_x, 0);
    CHECK_EQ(t0.zero_z, 0);
    const WgFossilTransform t1 = wg_fossil_transform(3, 4, 13, 1);
    CHECK_EQ(t1.size_x, 13);
    CHECK_EQ(t1.size_z, 3);
    CHECK_EQ(t1.zero_x, 12);
    CHECK_EQ(t1.zero_z, 0);
    const WgFossilTransform t2 = wg_fossil_transform(3, 4, 13, 2);
    CHECK_EQ(t2.size_x, 3);
    CHECK_EQ(t2.size_z, 13);
    CHECK_EQ(t2.zero_x, 2);
    CHECK_EQ(t2.zero_z, 12);
    const WgFossilTransform t3 = wg_fossil_transform(3, 4, 13, 3);
    CHECK_EQ(t3.size_x, 13);
    CHECK_EQ(t3.size_z, 3);
    CHECK_EQ(t3.zero_x, 0);
    CHECK_EQ(t3.zero_z, 2);
  }

  // The two configured features are fossil-kind and carry the pack's processor
  // integrity (0.9 skeleton, 0.1 ore overlay) and protected list.
  {
    const WgConfiguredFeature* coal = find_by_name(
        kWgConfiguredFeatures, std::size(kWgConfiguredFeatures), "minecraft:fossil_coal");
    const WgConfiguredFeature* diamonds = find_by_name(
        kWgConfiguredFeatures, std::size(kWgConfiguredFeatures), "minecraft:fossil_diamonds");
    CHECK(coal != nullptr && diamonds != nullptr);
    if (coal != nullptr && diamonds != nullptr) {
      CHECK(coal->kind == WgFeatureKind::kFossil);
      CHECK(diamonds->kind == WgFeatureKind::kFossil);
      CHECK_EQ(coal->config, 0);
      CHECK_EQ(diamonds->config, 1);
    }
    CHECK_EQ(std::size(kWgFossilTemplates), 16);
    CHECK_EQ(std::size(kWgFossilConfigs), 2);
    CHECK_EQ(std::size(kWgFossilRules), 1);
    CHECK_EQ(kWgFossilConfigs[0].structure_count, 8);
    CHECK_EQ(kWgFossilConfigs[0].max_empty_corners, 4);
    CHECK(kWgFossilConfigs[0].structure_integrity > 0.89f &&
          kWgFossilConfigs[0].structure_integrity < 0.91f);
    CHECK(kWgFossilConfigs[0].overlay_integrity > 0.09f &&
          kWgFossilConfigs[0].overlay_integrity < 0.11f);
    CHECK(std::strcmp(kWgFossilRules[0].output.name, "minecraft:deepslate_diamond_ore") == 0);
  }

  // A stone column at y <= 60: the fossil sinks 15-24 blocks under the lowest
  // ocean-floor column and both processor passes write. Coal is the upper
  // variant, so its overlay is coal ore.
  {
    Chunk chunk;
    fill_fossil_chunk(chunk, 60, "minecraft:plains");
    WgRandom rng(12345);
    const uint32_t placed = place_fossil(chunk, kWgFossilConfigs[0], rng, 8, 60, 8);
    CHECK(placed > 0);
    FossilScan scan;
    scan_fossil(chunk, &scan);
    CHECK(scan.bone > 0);
    CHECK(scan.coal > 0);
    CHECK_EQ(scan.diamond, 0);
    // The seed's own answer, pinned: 58 writes, 51 bone, 4 coal, y 37..40. The
    // template is the 5x4x5 skull (the stream picks it), sunk under the y=61
    // ocean floor.
    CHECK_EQ(scan.bone, 51);
    CHECK_EQ(scan.coal, 4);
    CHECK_EQ(scan.min_y, 37);
    CHECK_EQ(scan.max_y, 40);
    std::printf("  fossil coal: %zu placed, %d bone, %d coal, y=%d..%d\n",
                static_cast<size_t>(placed), scan.bone, scan.coal, scan.min_y, scan.max_y);

    // Deterministic: same seed, fresh identical column, same cells.
    Chunk again;
    fill_fossil_chunk(again, 60, "minecraft:plains");
    WgRandom rng2(12345);
    const uint32_t placed2 = place_fossil(again, kWgFossilConfigs[0], rng2, 8, 60, 8);
    CHECK_EQ(placed2, placed);
    FossilScan scan2;
    scan_fossil(again, &scan2);
    CHECK_EQ(scan2.digest, scan.digest);
    CHECK_EQ(scan2.bone, scan.bone);
    CHECK_EQ(scan2.coal, scan.coal);
  }

  // The diamonds variant's overlay is a `block_match` rule: every surviving
  // overlay block becomes deepslate diamond ore.
  {
    Chunk chunk;
    fill_fossil_chunk(chunk, 60, "minecraft:plains");
    WgRandom rng(777);
    const uint32_t placed = place_fossil(chunk, kWgFossilConfigs[1], rng, 8, 60, 8);
    CHECK(placed > 0);
    FossilScan scan;
    scan_fossil(chunk, &scan);
    CHECK(scan.bone > 0);
    CHECK(scan.diamond > 0);
    CHECK_EQ(scan.coal, 0);
    CHECK_EQ(scan.bone, 34);
    CHECK_EQ(scan.diamond, 3);
    std::printf("  fossil diamonds: %zu placed, %d bone, %d diamond\n", static_cast<size_t>(placed),
                scan.bone, scan.diamond);
  }

  // `features_cannot_replace`: a column of bedrock refuses every write.
  {
    Chunk chunk;
    for (int x = 0; x < 16; ++x)
      for (int z = 0; z < 16; ++z)
        for (int y = kWgMinY; y < 60; ++y)
          chunk.set_block_state(x, y, z, BlockStateKey{"minecraft:bedrock", ""});
    WgRandom rng(12345);
    CHECK_EQ(place_fossil(chunk, kWgFossilConfigs[0], rng, 8, 60, 8), 0u);
  }

  // More than `max_empty_corners_allowed` empty corners rejects the site before
  // any template block is written.
  {
    Chunk chunk;
    WgRandom rng(12345);
    CHECK_EQ(place_fossil(chunk, kWgFossilConfigs[0], rng, 8, 60, 8), 0u);
  }

  // The decoration stage dispatches the kind: decorate constructed desert
  // columns until the stage leaves a fossil's bones, and read them back. A
  // rarity-only search is not enough: a fossil placement can be considered and
  // still find its in-square position outside the single column the stage is
  // decorating, so the candidate itself has to be checked for the effect. This
  // is the wire between the registry row and the template placement, not only
  // the function below it.
  {
    WgOverworldGenerator generator(42);
    int hit = -1;
    uint32_t hit_applied = 0;
    int hit_bone = 0;
    for (int cx = 0; cx < 512 && hit < 0; ++cx) {
      Chunk candidate;
      candidate.x = cx;
      candidate.z = 0;
      // Desert is one of the biomes whose own decoration list carries the fossil
      // features (plains does not), so the `biome` modifier can pass.
      fill_fossil_chunk(candidate, 60, "minecraft:desert");
      WgDecorationStats candidate_stats;
      decorate_generated_chunk(generator, candidate, &candidate_stats);
      FossilScan candidate_scan;
      scan_fossil(candidate, &candidate_scan);
      if (candidate_scan.bone == 0)
        continue;
      hit = cx;
      hit_applied = candidate_stats.applied_by_kind[static_cast<uint32_t>(WgFeatureKind::kFossil)];
      hit_bone = candidate_scan.bone;
    }
    CHECK(hit >= 0);
    CHECK(hit_applied >= 1);
    CHECK(hit_bone > 0);
    std::printf("  fossil dispatch: chunk %d applied %u fossil(s), %d bone\n", hit, hit_applied,
                hit_bone);
  }
}

// ---- structure placement ----------------------------------------------------

const WgStructurePlacement* placement_of(const char* set_name) {
  const WgStructureSet* set = find_by_name(kWgStructureSets, std::size(kWgStructureSets), set_name);
  if (set == nullptr || set->placement >= std::size(kWgStructurePlacements))
    return nullptr;
  return &kWgStructurePlacements[set->placement];
}

// The first half of the structure slice: each set's placement config, the
// candidate chunk `RandomSpreadStructurePlacement.getPotentialStructureChunk`
// names, and the frequency gate `StructurePlacement.applyAdditionalChunkRestrictions`
// puts on top of it. The field values are the pack's own; the candidate rows and
// the gate's digests were read off the 26.2 server's own reducers (a Java probe
// reflecting into `StructurePlacement`'s private static methods), not re-derived
// here. The exclusion zone and the pieces themselves are the next slice.
void test_structure_placements() {
  CHECK_EQ(std::size(kWgStructurePlacements), 20);
  size_t bad_index = 0;
  for (const WgStructureSet& set : kWgStructureSets)
    bad_index += set.placement >= std::size(kWgStructurePlacements);
  CHECK_EQ(bad_index, 0);

  // The form of every row: one set of rings, the rest random spreads, and the
  // three frequency-gated sets the pack ships (buried treasure, mineshafts,
  // pillager outposts) are exactly the ones with frequency < 1.
  size_t rings = 0;
  size_t gated = 0;
  for (const WgStructurePlacement& placement : kWgStructurePlacements) {
    if (std::strcmp(placement.type, "minecraft:concentric_rings") == 0) {
      ++rings;
      CHECK_EQ(placement.spacing, 0);
      CHECK_EQ(placement.distance, 32);
      CHECK_EQ(placement.spread, 3);
      CHECK_EQ(placement.count, 128);
      continue;
    }
    CHECK(std::strcmp(placement.type, "minecraft:random_spread") == 0);
    CHECK(placement.spacing > 0);
    CHECK(placement.separation < placement.spacing);
    CHECK(placement.spread_type == kWgSpreadLinear || placement.spread_type == kWgSpreadTriangular);
    CHECK(placement.frequency > 0.0f && placement.frequency <= 1.0f);
    if (placement.frequency < 1.0f)
      ++gated;
  }
  CHECK_EQ(rings, 1);
  CHECK_EQ(gated, 3);

  const WgStructurePlacement* desert = placement_of("minecraft:desert_pyramids");
  const WgStructurePlacement* end_cities = placement_of("minecraft:end_cities");
  const WgStructurePlacement* villages = placement_of("minecraft:villages");
  const WgStructurePlacement* mansions = placement_of("minecraft:woodland_mansions");
  const WgStructurePlacement* buried = placement_of("minecraft:buried_treasures");
  const WgStructurePlacement* mineshafts = placement_of("minecraft:mineshafts");
  const WgStructurePlacement* outposts = placement_of("minecraft:pillager_outposts");
  const WgStructurePlacement* strongholds = placement_of("minecraft:strongholds");
  CHECK(desert && end_cities && villages && mansions && buried && mineshafts && outposts &&
        strongholds);
  if (desert == nullptr || end_cities == nullptr || villages == nullptr || mansions == nullptr ||
      buried == nullptr || mineshafts == nullptr || outposts == nullptr || strongholds == nullptr)
    return;

  // The pack's own values, including the fields the default-frequency sets use.
  CHECK_EQ(desert->spacing, 32);
  CHECK_EQ(desert->separation, 8);
  CHECK_EQ(desert->salt, 14357617u);
  CHECK_EQ(desert->spread_type, kWgSpreadLinear);
  CHECK_EQ(end_cities->spacing, 20);
  CHECK_EQ(end_cities->separation, 11);
  CHECK_EQ(end_cities->spread_type, kWgSpreadTriangular);
  CHECK_EQ(villages->spacing, 34);
  CHECK_EQ(villages->salt, 10387312u);
  CHECK_EQ(mansions->spacing, 80);
  CHECK_EQ(mansions->separation, 20);
  CHECK_EQ(mansions->spread_type, kWgSpreadTriangular);
  CHECK_EQ(buried->frequency, 0.01f);
  CHECK(std::strcmp(buried->frequency_reduction, "legacy_type_2") == 0);
  CHECK_EQ(buried->locate_x, 9);
  CHECK_EQ(buried->locate_z, 9);
  CHECK_EQ(mineshafts->frequency, 0.004f);
  CHECK(std::strcmp(mineshafts->frequency_reduction, "legacy_type_3") == 0);
  CHECK_EQ(outposts->frequency, 0.2f);
  CHECK(std::strcmp(outposts->exclusion_set, "minecraft:villages") == 0);
  CHECK_EQ(outposts->exclusion_chunks, 10);
  CHECK(std::strcmp(strongholds->type, "minecraft:concentric_rings") == 0);
  CHECK(std::strcmp(strongholds->preferred_biomes, "#minecraft:stronghold_biased_to") == 0);

  // `Math.floorDiv` on negative chunk coordinates, which is what puts chunk -1
  // in region -1 rather than region 0.
  CHECK_EQ(wg_structure_floor_div(0, 32), 0);
  CHECK_EQ(wg_structure_floor_div(31, 32), 0);
  CHECK_EQ(wg_structure_floor_div(32, 32), 1);
  CHECK_EQ(wg_structure_floor_div(-1, 32), -1);
  CHECK_EQ(wg_structure_floor_div(-32, 32), -1);
  CHECK_EQ(wg_structure_floor_div(-33, 32), -2);

  // The candidate rows. A spacing square is named by any chunk inside it, so
  // the inputs are chunk coordinates that floor-divide to the region the probe
  // read; the expected start is the probe's own answer.
  struct Pin {
    const WgStructurePlacement* placement;
    int64_t seed;
    int32_t chunk_x, chunk_z;
    int32_t want_x, want_z;
  };
  const Pin pins[] = {
      {desert, 42, 0, 0, 1, 7},
      {desert, 42, 40, 100, 37, 101},   // region (1,3)
      {desert, 42, -40, 100, -51, 112}, // region (-2,3)
      {end_cities, 42, 0, 0, 7, 3},
      {end_cities, 42, 100, -1, 103, -16}, // region (5,-1)
      {villages, 42, 238, 238, 262, 241},  // region (7,7)
      {villages, 0, 0, 0, 15, 2},
      {villages, -12345, -102, 68, -94, 69}, // region (-3,2)
      {mansions, 42, 160, -320, 170, -288},  // region (2,-4)
      {placement_of("minecraft:nether_fossils"), 42, 200, -14, 200, -14},
      {placement_of("minecraft:nether_complexes"), -1, -27, -27, -24, -8},
  };
  for (const Pin& pin : pins) {
    CHECK(pin.placement != nullptr);
    if (pin.placement == nullptr)
      continue;
    const WgStructureChunk got =
        wg_structure_candidate(pin.seed, *pin.placement, pin.chunk_x, pin.chunk_z);
    CHECK_EQ(got.x, pin.want_x);
    CHECK_EQ(got.z, pin.want_z);
  }

  // A candidate is inside its spacing square and leaves the separation margin
  // empty, for every random-spread row and both signs of region.
  size_t checked = 0;
  for (const WgStructurePlacement& placement : kWgStructurePlacements) {
    if (placement.spacing == 0)
      continue;
    for (int32_t region : {-3, 0, 4}) {
      const int32_t chunk = region * static_cast<int32_t>(placement.spacing);
      const WgStructureChunk got = wg_structure_candidate(1337, placement, chunk, chunk);
      CHECK(got.x >= region * placement.spacing);
      CHECK(got.x < region * placement.spacing + placement.spacing - placement.separation);
      CHECK(got.z >= region * placement.spacing);
      CHECK(got.z < region * placement.spacing + placement.spacing - placement.separation);
      ++checked;
    }
  }
  CHECK_EQ(checked, (std::size(kWgStructurePlacements) - 1) * 3);

  // The frequency gate, against the 26.2 server's four reducers. A gate that
  // drew from the wrong seeded source, used `next(31)` where vanilla uses a
  // float, or read the pillager row per chunk instead of per 16-chunk region
  // would move the digest without changing any field above.
  constexpr int32_t kCoords[] = {-41, -35, -29, -23, -17, -11, -5, 1, 7, 13, 19, 25, 31, 37, 41};
  const auto frequency_digest = [&kCoords](const WgStructurePlacement& placement, int64_t seed,
                                           size_t* passed) {
    uint64_t digest = 1469598103934665603ull;
    *passed = 0;
    for (int32_t x : kCoords) {
      for (int32_t z : kCoords) {
        const bool pass = wg_structure_passes_frequency(seed, placement, x, z);
        if (pass)
          ++*passed;
        digest = (digest ^ (pass ? 1u : 0u)) * 1099511628211ull;
      }
    }
    return digest;
  };

  // `default` (`probabilityReducer`) is not used by a gated pack row, so its
  // row is the probe's own combination over the real `minecraft:buried_treasures`
  // fields with the method and frequency swapped.
  WgStructurePlacement default_row = *buried;
  default_row.frequency = 0.2f;
  default_row.frequency_reduction = "default";
  default_row.salt = 20083232u;
  struct GatePin {
    const WgStructurePlacement* placement;
    int64_t seed;
    size_t passed;
    uint64_t digest;
  };
  const GatePin gates[] = {
      {&default_row, 42, 23, 0x5608976019b9a2aeull},
      {&default_row, -1, 28, 0x0725e110d2cbfc53ull},
      {&default_row, 1234567890123LL, 66, 0x40fcd755062a104dull},
      {outposts, 42, 75, 0xb1286fffbd26c888ull},
      {outposts, -1, 32, 0xa0180e68ec8fcef3ull},
      {outposts, 1234567890123LL, 42, 0x9ded507a1c0a45b7ull},
      {buried, 42, 3, 0x5c0f0436cb625b60ull},
      {buried, -1, 5, 0x8125f731927886beull},
      {buried, 1234567890123LL, 1, 0xfda0efdc997173d8ull},
      {mineshafts, 42, 2, 0xdbebcce4e2d4e4b1ull},
      {mineshafts, -1, 3, 0x95db259009a2af9eull},
      {mineshafts, 1234567890123LL, 0, 0x46edff2e6e288f19ull},
  };
  for (const GatePin& gate : gates) {
    size_t passed = 0;
    CHECK_EQ(frequency_digest(*gate.placement, gate.seed, &passed), gate.digest);
    CHECK_EQ(passed, gate.passed);
  }

  // The same three gated pack rows over the 16 x 16 chunk square a single
  // pillager region covers, as the probe read them: the outpost gate answers
  // the same way 64 times per region, and the first mineshaft chunk that passes
  // is (4,0).
  struct SquarePin {
    const WgStructurePlacement* placement;
    size_t passed;
    uint64_t digest;
    int32_t first_x, first_z;
  };
  const SquarePin squares[] = {
      {buried, 0, 0xdce53c1df8560f83ull, 0, 0},
      {outposts, 64, 0x9fddfa39b3813bc3ull, 0, -8},
      {mineshafts, 1, 0xc25cf66a7028b4a2ull, 4, 0},
  };
  for (const SquarePin& square : squares) {
    uint64_t digest = 1469598103934665603ull;
    size_t passed = 0;
    int32_t first_x = 0;
    int32_t first_z = 0;
    bool have_first = false;
    for (int32_t x = -8; x < 8; ++x) {
      for (int32_t z = -8; z < 8; ++z) {
        const bool pass = wg_structure_passes_frequency(42, *square.placement, x, z);
        if (pass) {
          ++passed;
          if (!have_first) {
            first_x = x;
            first_z = z;
            have_first = true;
          }
        }
        digest = (digest ^ (pass ? 1u : 0u)) * 1099511628211ull;
      }
    }
    CHECK_EQ(digest, square.digest);
    CHECK_EQ(passed, square.passed);
    CHECK_EQ(first_x, square.first_x);
    CHECK_EQ(first_z, square.first_z);
  }

  // A row without a gate never draws: its answer is true for every chunk and
  // seed, including the extreme ones where a hidden draw would show up as a
  // rejection.
  for (const WgStructurePlacement& placement : kWgStructurePlacements) {
    if (placement.frequency < 1.0f)
      continue;
    for (int64_t seed : {int64_t{0}, int64_t{-1}, std::numeric_limits<int64_t>::min()}) {
      CHECK(wg_structure_passes_frequency(seed, placement, -37, 41));
      CHECK(wg_structure_passes_frequency(seed, placement, 0, 0));
    }
  }

  std::printf("  structure placement: %zu sets, %zu candidates, %zu gates checked\n",
              std::size(kWgStructurePlacements), std::size(pins), std::size(gates));
}

// The exclusion zone: `StructurePlacement.isStructureChunk` on top of the
// candidate and the gate, and `ChunkGeneratorStructureState.hasStructureChunkInRange`
// as the zone calls it. The expected counts, digests and window answers are the
// 26.2 server's own (`Probe8`: two `RandomSpreadStructurePlacement`s built from
// the pack's numbers, the village set handed to the zone through `holder.direct`,
// and `isStructureChunk` run against a hollow `ChunkGeneratorStructureState`
// whose only live field is the seed).
void test_structure_exclusion() {
  const WgStructureSet* outposts = wg_structure_set_by_name("minecraft:pillager_outposts");
  const WgStructureSet* villages = wg_structure_set_by_name("minecraft:villages");
  CHECK(outposts != nullptr && villages != nullptr);
  if (outposts == nullptr || villages == nullptr)
    return;
  const WgStructurePlacement& outpost_placement = kWgStructurePlacements[outposts->placement];

  // The pack ships one zone, and it names a set this table carries.
  size_t zoned = 0;
  for (const WgStructureSet& set : kWgStructureSets) {
    const WgStructurePlacement& placement = kWgStructurePlacements[set.placement];
    if (placement.exclusion_set[0] == '\0')
      continue;
    ++zoned;
    CHECK(wg_structure_set_by_name(placement.exclusion_set) != nullptr);
    CHECK(placement.exclusion_chunks >= 1 && placement.exclusion_chunks <= 16);
  }
  CHECK_EQ(zoned, 1);
  CHECK(std::strcmp(outpost_placement.exclusion_set, villages->name) == 0);

  // `hasStructureChunkInRange` walks an inclusive square, and its distance is
  // measured in whole chunks: ten chunks away answers true on either axis and
  // on the corner, eleven does not. The village start at (262, 241) is the one
  // the candidate pins in the test above; the answers are the server's.
  const WgStructureChunk village_start =
      wg_structure_candidate(42, kWgStructurePlacements[villages->placement], 238, 238);
  CHECK_EQ(village_start.x, 262);
  CHECK_EQ(village_start.z, 241);
  struct RangePin {
    int32_t x, z;
    bool want;
  };
  const RangePin range_pins[] = {
      {262, 241, true},  // the start itself, distance 0
      {272, 241, true},  // +10 in x
      {273, 241, false}, // +11 in x
      {252, 241, true},  // -10 in x
      {251, 241, false}, // -11 in x
      {262, 231, true},  // -10 in z
      {262, 251, true},  // +10 in z
      {272, 251, true},  // +10, +10
      {272, 252, false}, // +10, +11
      {-150, 74, true},  // the seed-42 first excluded start, ten chunks from a village
      {-149, 74, true},
  };
  for (const RangePin& pin : range_pins)
    CHECK_EQ(wg_structure_has_chunk_in_range(42, villages->name, pin.x, pin.z, 10), pin.want);
  CHECK_EQ(wg_structure_has_chunk_in_range(42, villages->name, 262, 241, 0), true);
  CHECK_EQ(wg_structure_has_chunk_in_range(42, villages->name, 263, 241, 0), false);
  CHECK_EQ(wg_structure_has_chunk_in_range(42, "minecraft:not_a_set", 0, 0, 10), false);

  // The zone's effect over a 320 x 320 chunk grid, three seeds. `raw` counts the
  // chunks the candidate and the gate alone accept; `final` what survives the
  // exclusion scan. The digest is over `final`, so a scan that used an exclusive
  // bound, the wrong distance or the wrong other set would move it.
  struct GridPin {
    int64_t seed;
    size_t raw, final_count, excluded;
    uint64_t digest;
    int32_t first_x, first_z;
  };
  const GridPin grids[] = {
      {42, 17, 8, 9, 0xd7ecf18cc6e35c41ull, -160, 74},
      {-1, 13, 10, 3, 0x77c706713a974ae3ull, -128, -50},
      {1234567890123LL, 19, 12, 7, 0xb967d8aad246b4dull, -122, 141},
  };
  for (const GridPin& grid : grids) {
    uint64_t digest = 1469598103934665603ull;
    size_t raw = 0;
    size_t final_count = 0;
    size_t excluded = 0;
    int32_t first_x = 0;
    int32_t first_z = 0;
    bool have_first = false;
    for (int32_t x = -160; x < 160; ++x) {
      for (int32_t z = -160; z < 160; ++z) {
        const bool is_final = wg_structure_is_chunk(grid.seed, *outposts, x, z);
        const WgStructureChunk candidate =
            wg_structure_candidate(grid.seed, outpost_placement, x, z);
        const bool is_raw = candidate.x == x && candidate.z == z &&
                            wg_structure_passes_frequency(grid.seed, outpost_placement, x, z);
        if (is_raw)
          ++raw;
        if (is_final) {
          ++final_count;
          // Every surviving start is clear of every village start, which is the
          // one thing the zone promises.
          CHECK(!wg_structure_has_chunk_in_range(grid.seed, villages->name, x, z, 10));
        }
        if (is_raw && !is_final) {
          ++excluded;
          if (!have_first) {
            first_x = x;
            first_z = z;
            have_first = true;
          }
        }
        digest = (digest ^ (is_final ? 1ull : 0ull)) * 1099511628211ull;
      }
    }
    CHECK_EQ(raw, grid.raw);
    CHECK_EQ(final_count, grid.final_count);
    CHECK_EQ(excluded, grid.excluded);
    CHECK_EQ(digest, grid.digest);
    CHECK_EQ(first_x, grid.first_x);
    CHECK_EQ(first_z, grid.first_z);
    // The zone does something on every seed, not only on paper.
    CHECK(grid.excluded > 0);
  }

  // A concentric-ring row has no per-chunk answer from a seed alone: its starts
  // are the ring list, which needs the biome source. "The candidate is this
  // chunk" would claim every stronghold chunk, so the answer is false rather
  // than a wrong true; `WgStructurePlacementState` answers it with the list.
  const WgStructureSet* strongholds = wg_structure_set_by_name("minecraft:strongholds");
  CHECK(strongholds != nullptr);
  if (strongholds != nullptr)
    CHECK(!wg_structure_is_chunk(42, *strongholds, 0, 0));

  std::printf("  structure exclusion: %zu grids, %zu window answers\n", std::size(grids),
              std::size(range_pins));
}

// The start-chunk biome test and the stronghold ring list. The expected ring
// chunks, digests and biome answers are the 26.2 server's own: a probe boots
// the pack's registries and runs the game's `generateRingPositions`/`RTree`
// over the pack's own parameter list, clearing `RTree.lastResult` before every
// sample so the answer is the cold (port-reproducible) one; it asks
// `Structure.isValidBiome` at the same generation points this test uses.
void test_structure_biomes_and_rings() {
  CHECK_EQ(std::size(kWgStructureBiomes), 294);
  CHECK_EQ(std::size(kWgRingPreferredBiomes), 38);

  const WgStructure* stronghold = wg_structure_by_name("minecraft:stronghold");
  const WgStructure* ancient_city = wg_structure_by_name("minecraft:ancient_city");
  const WgStructure* desert_pyramid = wg_structure_by_name("minecraft:desert_pyramid");
  const WgStructure* trial_chambers = wg_structure_by_name("minecraft:trial_chambers");
  const WgStructure* outpost = wg_structure_by_name("minecraft:pillager_outpost");
  const WgStructure* village_plains = wg_structure_by_name("minecraft:village_plains");
  CHECK(stronghold && ancient_city && desert_pyramid && trial_chambers && outpost &&
        village_plains);
  if (stronghold == nullptr || ancient_city == nullptr || desert_pyramid == nullptr ||
      trial_chambers == nullptr || outpost == nullptr || village_plains == nullptr)
    return;

  // The pack's resolved sets: the four counts the probe's own `HolderSet`s
  // report, and the form every row has to keep (sorted, unique, in range).
  CHECK_EQ(stronghold->biome_count, 55);
  CHECK_EQ(trial_chambers->biome_count, 54);
  CHECK_EQ(outpost->biome_count, 12);
  CHECK_EQ(ancient_city->biome_count, 1);
  CHECK_EQ(desert_pyramid->biome_count, 1);
  CHECK_EQ(village_plains->biome_count, 2);
  size_t biome_total = 0;
  for (const WgStructure& structure : kWgStructures) {
    CHECK(structure.biome_offset + structure.biome_count <= std::size(kWgStructureBiomes));
    for (uint32_t i = 0; i < structure.biome_count; ++i) {
      const char* name = kWgStructureBiomes[structure.biome_offset + i];
      if (i > 0)
        CHECK(std::strcmp(kWgStructureBiomes[structure.biome_offset + i - 1], name) < 0);
    }
    biome_total += structure.biome_count;
  }
  CHECK_EQ(biome_total, std::size(kWgStructureBiomes));
  CHECK(wg_structure_biome_allowed(*stronghold, "minecraft:deep_dark"));
  CHECK(wg_structure_biome_allowed(*stronghold, "minecraft:warm_ocean"));
  CHECK(!wg_structure_biome_allowed(*stronghold, "minecraft:nether_wastes"));
  CHECK(wg_structure_biome_allowed(*ancient_city, "minecraft:deep_dark"));
  CHECK(!wg_structure_biome_allowed(*ancient_city, "minecraft:plains"));
  CHECK(wg_structure_biome_allowed(*village_plains, "minecraft:meadow"));
  CHECK(!wg_structure_biome_allowed(*village_plains, "minecraft:desert"));

  // `ChunkGeneratorStructureState.createForNormal` keeps only sets with a
  // biome the overworld source can produce: 17 of the pack's 20.
  size_t available = 0;
  for (const WgStructureSet& set : kWgStructureSets)
    available += wg_structure_set_has_biomes(set) ? 1 : 0;
  CHECK_EQ(available, 17);
  CHECK(!wg_structure_set_has_biomes(*wg_structure_set_by_name("minecraft:end_cities")));
  CHECK(!wg_structure_set_has_biomes(*wg_structure_set_by_name("minecraft:nether_complexes")));

  // The raw `RandomState.sampler()` path: these are the probe's own
  // `MultiNoiseBiomeSource.getNoiseBiome` answers at the same quart cells.
  WgOverworldGenerator generator(42);
  WgClimateSampler sampler(generator.state());
  struct BiomePin {
    int32_t x, y, z;
    const char* biome;
  };
  const BiomePin biomes[] = {
      {0, 0, 0, "minecraft:lush_caves"},       {0, 64, 0, "minecraft:dark_forest"},
      {8, 64, 8, "minecraft:river"},           {4192, 0, 3856, "minecraft:warm_ocean"},
      {100, 64, 100, "minecraft:dark_forest"}, {1000, 62, 1000, "minecraft:ocean"},
      {-1000, 62, -1000, "minecraft:river"},   {2000, -30, 2000, "minecraft:lush_caves"},
  };
  for (const BiomePin& pin : biomes) {
    CHECK(std::strcmp(sampler.biome_at_quart(pin.x >> 2, pin.y >> 2, pin.z >> 2), pin.biome) == 0);
  }

  // `Structure.isValidBiome` over 34 structures at the same eight points, in
  // the table's sorted order. The digest is the probe's answer bits.
  uint64_t biome_mask_digest = 1469598103934665603ull;
  for (const BiomePin& pin : biomes) {
    for (const WgStructure& structure : kWgStructures) {
      const bool valid = wg_structure_start_biome_is_valid(sampler, structure, pin.x, pin.y, pin.z);
      biome_mask_digest = (biome_mask_digest ^ (valid ? 1ull : 0ull)) * 1099511628211ull;
    }
  }
  CHECK_EQ(biome_mask_digest, 1544909500894453941ull);
  CHECK(wg_structure_start_biome_is_valid(sampler, *stronghold, 0, 0, 0));
  CHECK(wg_structure_start_biome_is_valid(sampler, *stronghold, 4192, 0, 3856));
  CHECK(!wg_structure_start_biome_is_valid(sampler, *ancient_city, 0, 0, 0));
  CHECK(wg_structure_start_biome_is_valid(sampler, *trial_chambers, 1000, 62, 1000));
  CHECK(wg_structure_start_biome_is_valid(sampler, *wg_structure_by_name("minecraft:shipwreck"),
                                          1000, 62, 1000));
  CHECK(!wg_structure_start_biome_is_valid(sampler, *wg_structure_by_name("minecraft:shipwreck"), 0,
                                           64, 0));

  const WgStructureSet* strongholds = wg_structure_set_by_name("minecraft:strongholds");
  const WgStructureSet* villages = wg_structure_set_by_name("minecraft:villages");
  CHECK(strongholds != nullptr && villages != nullptr);
  if (strongholds == nullptr || villages == nullptr)
    return;
  const uint32_t ring_index = strongholds->placement;
  const WgStructurePlacement& ring_placement = kWgStructurePlacements[ring_index];

  const auto digest_of = [](const std::vector<WgStructureChunk>& list) {
    uint64_t hash = 1469598103934665603ull;
    for (const WgStructureChunk& chunk : list) {
      hash = (hash ^ static_cast<uint64_t>(static_cast<int64_t>(chunk.x))) * 1099511628211ull;
      hash = (hash ^ static_cast<uint64_t>(static_cast<int64_t>(chunk.z))) * 1099511628211ull;
    }
    return hash;
  };

  // Seed 42's list, first eight chunks and the whole-list digest, read off the
  // server's own `generateRingPositions`. The list is 128 strongholds.
  const std::vector<WgStructureChunk> rings42 =
      wg_structure_ring_positions(42, ring_placement, sampler);
  CHECK_EQ(rings42.size(), 128);
  const WgStructureChunk first_eight[] = {
      {-25, -141}, {107, 48},   {-121, 96}, {324, 13},
      {170, 312},  {-170, 289}, {-288, -5}, {-142, -261},
  };
  for (size_t i = 0; i < std::size(first_eight) && i < rings42.size(); ++i) {
    CHECK_EQ(rings42[i].x, first_eight[i].x);
    CHECK_EQ(rings42[i].z, first_eight[i].z);
  }
  // The probe clears `Climate.RTree.lastResult` before every sample, i.e. it
  // pins the server's *cold* search. The live tree's thread-local warm start
  // makes its own `getRingPositionsFor` depend on the async task history (six
  // seed-42 positions move by one chunk); `wg_select_biome` is the cold search,
  // so the cold list is the deterministic answer this port can — and does —
  // reproduce. The warm-start divergence is named in docs/PHASE4.md.
  CHECK(digest_of(rings42) == 6426093696122114914ull);
  // Determinism: the same seed and sampler rebuild the same list.
  CHECK(digest_of(wg_structure_ring_positions(42, ring_placement, sampler)) == digest_of(rings42));

  // Every ring point is accepted by the placement state and by the start
  // biome test at the stronghold's generation point (the chunk's min corner at
  // y = 0), and a non-ring chunk is not.
  WgStructurePlacementState state(generator);
  CHECK_EQ(state.seed(), 42);
  const std::vector<WgStructureChunk>& cached = state.ring_positions(ring_index);
  CHECK_EQ(cached.size(), rings42.size());
  CHECK(digest_of(cached) == digest_of(rings42));
  CHECK(&state.ring_positions(ring_index) == &cached);
  for (const WgStructureChunk& chunk : cached) {
    CHECK(state.is_structure_chunk(*strongholds, chunk.x, chunk.z));
    CHECK(wg_structure_start_biome_is_valid(sampler, *stronghold, chunk.x * 16, 0, chunk.z * 16));
    CHECK(state.has_structure_chunk_in_range(strongholds->name, chunk.x, chunk.z, 0));
  }
  CHECK(!state.is_structure_chunk(*strongholds, 0, 0));
  CHECK(!state.is_structure_chunk(*strongholds, 1, 0));
  CHECK(state.has_structure_chunk_in_range(strongholds->name, rings42[0].x, rings42[0].z, 0));
  CHECK(!state.has_structure_chunk_in_range(strongholds->name, -3000, -3000, 3));
  CHECK(!state.is_structure_chunk(*strongholds, rings42[0].x + 1, rings42[0].z));
  CHECK(!state.is_structure_chunk(*strongholds, rings42[0].x, rings42[0].z + 1));

  // The free seed-only functions keep answering false for a ring row (they have
  // no biome source); the state is the one that carries the list.
  CHECK(!wg_structure_is_chunk(42, *strongholds, rings42[0].x, rings42[0].z));
  CHECK(!wg_structure_has_chunk_in_range(42, strongholds->name, rings42[0].x, rings42[0].z, 0));

  // The random-spread half through the state is the free function's answer.
  for (int32_t x = 232; x < 280; x += 7) {
    for (int32_t z = 232; z < 280; z += 7)
      CHECK_EQ(state.is_structure_chunk(*villages, x, z),
               wg_structure_is_chunk(42, *villages, x, z));
  }

  // Two more seeds' whole-list digests, from the same cold probe.
  for (const auto& pin : {std::pair<int64_t, uint64_t>{-1, 7903115709579460572ull},
                          std::pair<int64_t, uint64_t>{0, 1837920566000557008ull}}) {
    WgOverworldGenerator seed_generator(pin.first);
    WgClimateSampler seed_sampler(seed_generator.state());
    const std::vector<WgStructureChunk> rings =
        wg_structure_ring_positions(pin.first, ring_placement, seed_sampler);
    CHECK_EQ(rings.size(), 128);
    CHECK(digest_of(rings) == pin.second);
  }

  std::printf("  structure biomes: %zu structures, %zu biome names, %zu sets available\n",
              std::size(kWgStructures), std::size(kWgStructureBiomes), available);
  std::printf("  structure rings: %zu stronghold chunks, digest %llu\n", rings42.size(),
              static_cast<unsigned long long>(digest_of(rings42)));
}

// ---- shipwreck pieces ------------------------------------------------------

uint64_t fnv1a(std::string_view text) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (unsigned char byte : text) {
    hash ^= byte;
    hash *= 0x100000001b3ull;
  }
  return hash;
}

std::string state_text(const WgState& state) {
  std::string out = state.name;
  if (state.properties[0] != '\0') {
    out.push_back('[');
    out += state.properties;
    out.push_back(']');
  }
  return out;
}

// The changed cells of a shipwreck application, in the scan order this test
// uses: `x,y,z|before-name[before-props]>after-name[after-props]` per cell.
std::string changed_cells(const Chunk& before, const Chunk& after) {
  std::string out;
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z)
      for (int y = kWgMinY; y < kWgMinY + kWgHeight; ++y) {
        const BlockStateKey a = before.block_state_at(x, y, z);
        const BlockStateKey b = after.block_state_at(x, y, z);
        if (a == b)
          continue;
        out += std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z) + "|" +
               a.name + "[" + a.properties + "]>" + b.name + "[" + b.properties + "]\n";
      }
  return out;
}

int changed_count(const Chunk& before, const Chunk& after) {
  int count = 0;
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z)
      for (int y = kWgMinY; y < kWgMinY + kWgHeight; ++y)
        if (!(before.block_state_at(x, y, z) == after.block_state_at(x, y, z)))
          ++count;
  return count;
}

void test_shipwreck_pieces() {
  // ---- the generated table ------------------------------------------------
  struct TemplatePin {
    const char* name;
    int32_t size_x;
    int32_t size_y;
    int32_t size_z;
    int32_t palettes;
    int32_t blocks;
  };
  // Sizes, palette counts and block counts read from the 26.2 server's own
  // `StructureTemplate` for these twenty NBT files.
  static const TemplatePin kTemplates[] = {
      {"minecraft:shipwreck/with_mast", 9, 21, 28, 8, 729},
      {"minecraft:shipwreck/upsidedown_full", 9, 9, 28, 8, 601},
      {"minecraft:shipwreck/upsidedown_fronthalf", 9, 9, 22, 8, 332},
      {"minecraft:shipwreck/upsidedown_backhalf", 9, 9, 16, 8, 387},
      {"minecraft:shipwreck/sideways_full", 9, 9, 28, 8, 641},
      {"minecraft:shipwreck/sideways_fronthalf", 9, 9, 24, 8, 321},
      {"minecraft:shipwreck/sideways_backhalf", 9, 9, 17, 8, 381},
      {"minecraft:shipwreck/rightsideup_full", 9, 9, 28, 8, 662},
      {"minecraft:shipwreck/rightsideup_fronthalf", 9, 9, 24, 8, 355},
      {"minecraft:shipwreck/rightsideup_backhalf", 9, 9, 16, 8, 414},
      {"minecraft:shipwreck/with_mast_degraded", 9, 21, 28, 8, 652},
      {"minecraft:shipwreck/upsidedown_full_degraded", 9, 9, 28, 8, 546},
      {"minecraft:shipwreck/upsidedown_fronthalf_degraded", 9, 9, 22, 8, 300},
      {"minecraft:shipwreck/upsidedown_backhalf_degraded", 9, 9, 16, 8, 362},
      {"minecraft:shipwreck/sideways_full_degraded", 9, 9, 28, 8, 583},
      {"minecraft:shipwreck/sideways_fronthalf_degraded", 9, 9, 24, 8, 246},
      {"minecraft:shipwreck/sideways_backhalf_degraded", 9, 9, 17, 8, 340},
      {"minecraft:shipwreck/rightsideup_full_degraded", 9, 9, 28, 8, 619},
      {"minecraft:shipwreck/rightsideup_fronthalf_degraded", 9, 9, 24, 8, 299},
      {"minecraft:shipwreck/rightsideup_backhalf_degraded", 9, 9, 16, 8, 385},
  };
  // The table carries every piece kind's templates now, not only the ships and
  // igloos read below: the full pack is 981, of which the first
  // `std::size(kTemplates) + 3` are the twenty ships and the three igloo pieces.
  // The latest growth is the 167 `bastion/**` and 20 `end_city/**` templates,
  // after the 84 `trail_ruins/**/*.nbt` and 59 `ancient_city/**/*.nbt` ones;
  // before those the table was 651 (640 + the eleven outpost templates).
  CHECK_EQ(std::size(kWgPieceTemplates), 981);
  for (size_t i = 0; i < std::size(kTemplates) && i < std::size(kWgPieceTemplates); ++i) {
    const WgPieceTemplate& t = kWgPieceTemplates[i];
    CHECK(std::strcmp(t.name, kTemplates[i].name) == 0);
    CHECK_EQ(t.size_x, kTemplates[i].size_x);
    CHECK_EQ(t.size_y, kTemplates[i].size_y);
    CHECK_EQ(t.size_z, kTemplates[i].size_z);
    CHECK_EQ(t.palette_count, kTemplates[i].palettes);
    CHECK_EQ(t.block_count, kTemplates[i].blocks);
  }
  // The three igloo templates follow the ships, with `IglooPieces.PIVOTS` and
  // one palette each. Their block counts are the server's own.
  struct IglooRow {
    const char* name;
    int16_t sx, sy, sz;
    int16_t px, py, pz;
    uint16_t blocks;
  };
  static const IglooRow kIglooRows[] = {
      {"minecraft:igloo/top", 7, 5, 8, 3, 5, 5, 152},
      {"minecraft:igloo/middle", 3, 3, 3, 1, 3, 1, 15},
      {"minecraft:igloo/bottom", 7, 6, 9, 3, 6, 7, 244},
  };
  CHECK_EQ(std::size(kWgIglooTemplates), std::size(kIglooRows));
  for (size_t i = 0; i < std::size(kIglooRows); ++i) {
    const WgPieceTemplate& t = kWgPieceTemplates[std::size(kTemplates) + i];
    CHECK(std::strcmp(t.name, kIglooRows[i].name) == 0);
    CHECK_EQ(t.size_x, kIglooRows[i].sx);
    CHECK_EQ(t.size_y, kIglooRows[i].sy);
    CHECK_EQ(t.size_z, kIglooRows[i].sz);
    CHECK_EQ(t.pivot_x, kIglooRows[i].px);
    CHECK_EQ(t.pivot_y, kIglooRows[i].py);
    CHECK_EQ(t.pivot_z, kIglooRows[i].pz);
    CHECK_EQ(t.block_count, kIglooRows[i].blocks);
    CHECK_EQ(t.palette_count, 1u);
  }
  // The eleven pillager-outpost templates. Their pivots are all zero in the
  // pack, and the full-volume block counts are the server's own NBT sizes
  // (base_plate is 16 x 30 x 16 = 7,680 rows, watchtower 15 x 21 x 15 = 4,725).
  // The watchtower pair is a `list_pool_element`, so both entries share the
  // tower's jigsaw connectors through the same pool row.
  struct OutpostRow {
    const char* name;
    int16_t sx, sy, sz;
    uint16_t blocks;
    uint16_t jigsaws;
  };
  static const OutpostRow kOutpostRows[] = {
      {"minecraft:pillager_outpost/base_plate", 16, 30, 16, 7680, 5},
      {"minecraft:pillager_outpost/feature_cage1", 7, 4, 7, 196, 5},
      {"minecraft:pillager_outpost/feature_cage2", 7, 4, 7, 196, 5},
      {"minecraft:pillager_outpost/feature_cage_with_allays", 7, 4, 7, 196, 5},
      {"minecraft:pillager_outpost/feature_logs", 6, 3, 7, 126, 4},
      {"minecraft:pillager_outpost/feature_plate", 16, 4, 32, 2048, 16},
      {"minecraft:pillager_outpost/feature_targets", 3, 3, 7, 63, 5},
      {"minecraft:pillager_outpost/feature_tent1", 6, 4, 7, 168, 4},
      {"minecraft:pillager_outpost/feature_tent2", 6, 4, 7, 168, 4},
      {"minecraft:pillager_outpost/watchtower", 15, 21, 15, 4725, 1},
      {"minecraft:pillager_outpost/watchtower_overgrown", 15, 23, 15, 5175, 1},
  };
  for (const OutpostRow& row : kOutpostRows) {
    const WgPieceTemplate* templ = wg_piece_template_by_name(row.name);
    CHECK(templ != nullptr);
    if (templ == nullptr)
      continue;
    CHECK(std::strcmp(templ->name, row.name) == 0);
    CHECK_EQ(templ->size_x, row.sx);
    CHECK_EQ(templ->size_y, row.sy);
    CHECK_EQ(templ->size_z, row.sz);
    CHECK_EQ(templ->pivot_x, 0);
    CHECK_EQ(templ->pivot_y, 0);
    CHECK_EQ(templ->pivot_z, 0);
    CHECK_EQ(templ->block_count, row.blocks);
    CHECK_EQ(templ->jigsaw_count, row.jigsaws);
  }
  // The widest template box reaches into a chunk 3 away from its source chunk's
  // minimum corner, so the scan window `apply_generated_structures` uses has to
  // be that wide. This was 1 while only the ships and igloos were in the table,
  // then 2 with the village/outpost/mansion sets; the ancient-city templates the
  // generator now emits are what widened it to 3.
  CHECK_EQ(kWgPieceChunkReach, 3);
  // The two list orders are `ShipwreckPieces`' own.
  CHECK_EQ(std::size(kWgShipwreckOcean), 20);
  CHECK_EQ(std::size(kWgShipwreckBeached), 11);
  CHECK_EQ(kWgShipwreckOcean[0], 0u);   // with_mast
  CHECK_EQ(kWgShipwreckOcean[5], 5u);   // sideways_fronthalf
  CHECK_EQ(kWgShipwreckBeached[0], 0u); // with_mast
  CHECK_EQ(kWgShipwreckBeached[1], 4u); // sideways_full

  // Every block/palette row is inside its template, and every palette row has
  // the same width as the block list's state range.
  size_t bad_rows = 0;
  for (const WgPieceTemplate& t : kWgPieceTemplates) {
    for (uint32_t p = 0; p < t.palette_count; ++p) {
      const WgPiecePalette& palette = kWgPiecePalettes[t.palette_offset + p];
      if (palette.state_offset + palette.state_count > std::size(kWgPiecePaletteStates))
        ++bad_rows;
      for (uint32_t b = 0; b < t.block_count; ++b) {
        const WgPieceBlock& block = kWgPieceBlocks[t.block_offset + b];
        if (block.x < 0 || block.x >= t.size_x || block.y < 0 || block.y >= t.size_y ||
            block.z < 0 || block.z >= t.size_z)
          ++bad_rows;
        if (block.state >= palette.state_count)
          ++bad_rows;
        const uint16_t state = kWgPiecePaletteStates[palette.state_offset + block.state];
        if (state >= std::size(kWgPieceStates))
          ++bad_rows;
      }
    }
  }
  CHECK_EQ(bad_rows, 0);

  // The 20 x 4 pivoted boxes, pinned against the server's
  // `StructureTemplate.getBoundingBox(position, rotation, (4,0,15), NONE)`.
  std::string box_text;
  const char* const kTemplatePrefix = "minecraft:shipwreck/";
  for (const WgPieceTemplate& t : kWgPieceTemplates) {
    const char* stem = t.name;
    if (std::strncmp(stem, kTemplatePrefix, std::strlen(kTemplatePrefix)) != 0)
      continue;
    stem += std::strlen(kTemplatePrefix);
    for (uint32_t rotation = 0; rotation < 4; ++rotation) {
      const WgPieceBox box = wg_piece_box(t, rotation, 0, 0, 0);
      char line[256];
      std::snprintf(line, sizeof(line), "%s|%u|%dx%dx%d|%d,%d,%d\n", stem, rotation,
                    box.max_x - box.min_x + 1, box.max_y - box.min_y + 1, box.max_z - box.min_z + 1,
                    box.min_x, box.min_y, box.min_z);
      box_text += line;
    }
  }
  CHECK_EQ(fnv1a(box_text), 17061999913940596987ull);

  // The 160 palettes, each sorted to the server's own block order, pinned by
  // the digest of its `state@x,y,z` rows.
  std::string palette_text;
  for (const WgPieceTemplate& t : kWgPieceTemplates) {
    const char* stem = t.name;
    if (std::strncmp(stem, kTemplatePrefix, std::strlen(kTemplatePrefix)) != 0)
      continue;
    stem += std::strlen(kTemplatePrefix);
    for (uint32_t p = 0; p < t.palette_count; ++p) {
      const WgPiecePalette& palette = kWgPiecePalettes[t.palette_offset + p];
      std::vector<std::string> rows;
      rows.reserve(t.block_count);
      for (uint32_t b = 0; b < t.block_count; ++b) {
        const WgPieceBlock& block = kWgPieceBlocks[t.block_offset + b];
        const WgState& state =
            kWgPieceStates[kWgPiecePaletteStates[palette.state_offset + block.state]];
        std::string row = state_text(state);
        row += "@" + std::to_string(block.x) + "," + std::to_string(block.y) + "," +
               std::to_string(block.z);
        rows.push_back(std::move(row));
      }
      std::sort(rows.begin(), rows.end());
      std::string joined;
      for (const std::string& row : rows) {
        if (!joined.empty())
          joined.push_back('\n');
        joined += row;
      }
      palette_text += stem;
      palette_text.push_back('|');
      palette_text += std::to_string(p);
      palette_text.push_back('|');
      palette_text += std::to_string(fnv1a(joined));
      palette_text.push_back('\n');
    }
  }
  CHECK_EQ(fnv1a(palette_text), 2865186356112623631ull);

  // `BlockState.rotate` for the shipwreck vocabulary.
  CHECK(
      wg_piece_rotate_properties("facing=north,half=top,shape=outer_right,waterlogged=false", 1) ==
      "facing=east,half=top,shape=outer_right,waterlogged=false");
  CHECK(wg_piece_rotate_properties("facing=north,type=single,waterlogged=false", 2) ==
        "facing=south,type=single,waterlogged=false");
  CHECK(wg_piece_rotate_properties("facing=west", 3) == "facing=south");
  CHECK(wg_piece_rotate_properties("axis=x", 1) == "axis=z");
  CHECK(wg_piece_rotate_properties("axis=x", 2) == "axis=x");
  CHECK(wg_piece_rotate_properties("axis=y", 3) == "axis=y");
  // A fence's connections move with the shape: CW90 turns north into east.
  CHECK(wg_piece_rotate_properties("east=false,north=true,south=false,waterlogged=false,west=false",
                                   1) ==
        "east=true,north=false,south=false,waterlogged=false,west=false");

  // ---- starts, pinned against the 26.2 server -----------------------------
  struct StartPin {
    int64_t seed;
    int32_t cx;
    int32_t cz;
    const char* structure;
    const char* templ;
    uint32_t rotation;
    int32_t center_y; // `getFirstOccupiedHeight` at the chunk's middle column
    int32_t span_x;
    int32_t span_y;
    int32_t span_z;
    int32_t min_x;
    int32_t min_y;
    int32_t min_z;
  };
  static const StartPin kStarts[] = {
      {42, 12, 28, "minecraft:shipwreck", "minecraft:shipwreck/sideways_full", 1, 48, 28, 9, 9, 184,
       90, 459},
      {-1, -30, 8, "minecraft:shipwreck", "minecraft:shipwreck/upsidedown_fronthalf_degraded", 1,
       35, 22, 9, 9, -482, 90, 139},
      {-1, -22, -12, "minecraft:shipwreck", "minecraft:shipwreck/sideways_full", 1, 47, 28, 9, 9,
       -360, 90, -181},
      {-1, -11, 19, "minecraft:shipwreck", "minecraft:shipwreck/rightsideup_fronthalf_degraded", 1,
       36, 24, 9, 9, -180, 90, 315},
      {-1, 7, -10, "minecraft:shipwreck", "minecraft:shipwreck/rightsideup_backhalf", 3, 42, 16, 9,
       9, 101, 90, -149},
      {-1, 8, 0, "minecraft:shipwreck", "minecraft:shipwreck/upsidedown_fronthalf_degraded", 1, 41,
       22, 9, 9, 126, 90, 11},
      {-1, 28, -21, "minecraft:shipwreck", "minecraft:shipwreck/sideways_full", 1, 43, 28, 9, 9,
       440, 90, -325},
      {0, 14, 8, "minecraft:shipwreck_beached", "minecraft:shipwreck/sideways_full", 1, 62, 28, 9,
       9, 216, 90, 139},
  };
  struct InvalidPin {
    int64_t seed;
    int32_t cx;
    int32_t cz;
  };
  static const InvalidPin kInvalid[] = {
      {42, 15, -11},     // not a candidate chunk
      {42, 11, 7},       // candidate, but neither member's biome set accepts it
      {42, 3, -11},      // candidate, same answer
      {0, -19, -8},      // candidate, no ocean or beach biome
      {123456789, 3, 3}, // candidate, no ocean or beach biome
  };
  for (int64_t seed : {int64_t{42}, int64_t{-1}, int64_t{0}, int64_t{123456789}}) {
    WgOverworldGenerator generator(seed);
    WgGeneratedHeights heights(generator);
    WgClimateSampler sampler(generator.state());
    for (const StartPin& pin : kStarts) {
      if (pin.seed != seed)
        continue;
      const WgShipwreckStart start = wg_shipwreck_start(seed, pin.cx, pin.cz, sampler, heights);
      CHECK(start.valid);
      if (!start.valid)
        continue;
      CHECK(std::strcmp(start.structure->name, pin.structure) == 0);
      CHECK(std::strcmp(start.templ->name, pin.templ) == 0);
      CHECK_EQ(start.rotation, pin.rotation);
      CHECK_EQ(start.x, pin.cx * 16);
      CHECK_EQ(start.y, 90);
      CHECK_EQ(start.z, pin.cz * 16);
      CHECK_EQ(start.min_x, pin.min_x);
      CHECK_EQ(start.min_y, pin.min_y);
      CHECK_EQ(start.min_z, pin.min_z);
      CHECK_EQ(start.max_x - start.min_x + 1, pin.span_x);
      CHECK_EQ(start.max_y - start.min_y + 1, pin.span_y);
      CHECK_EQ(start.max_z - start.min_z + 1, pin.span_z);
      const bool beached = std::strcmp(pin.structure, "minecraft:shipwreck_beached") == 0;
      CHECK_EQ(heights.height(pin.cx * 16 + 8, pin.cz * 16 + 8,
                              beached ? WgStructureHeightmap::kWorldSurface
                                      : WgStructureHeightmap::kOceanFloor) -
                   1,
               pin.center_y);
    }
    for (const InvalidPin& pin : kInvalid) {
      if (pin.seed != seed)
        continue;
      const WgShipwreckStart start = wg_shipwreck_start(seed, pin.cx, pin.cz, sampler, heights);
      CHECK(!start.valid);
    }
  }

  // ---- the writer, on the one ship seed 42 puts at (12, 28) --------------
  // The pivoted box covers x 184..211 and z 459..467, i.e. chunks (11..13, 28)
  // and (11..13, 29): the piece really does cross three columns and two rows of
  // chunks, which is what the writer's clipping has to stitch back together.
  WgOverworldGenerator generator(42);
  const WgStructureSet* shipwreck_set = wg_structure_set_by_name("minecraft:shipwrecks");
  CHECK(shipwreck_set != nullptr);
  if (shipwreck_set != nullptr)
    CHECK(wg_structure_is_chunk(42, *shipwreck_set, 12, 28));

  std::vector<Chunk> before_chunks;
  std::vector<Chunk> after_chunks;
  std::string all_changes;
  uint32_t total_blocks = 0;
  for (int32_t dz = 0; dz <= 1; ++dz) {
    for (int32_t dx = -1; dx <= 1; ++dx) {
      const int32_t cx = 12 + dx;
      const int32_t cz = 28 + dz;
      Chunk before;
      fill_generated_carved_chunk(generator, cx, cz, before);
      decorate_generated_chunk(generator, before, nullptr);
      before.rebuild_after_bulk_write();
      Chunk after = before;
      const WgStructureApplyStats stats = apply_generated_structures(generator, after);
      CHECK_EQ(stats.starts, 1);
      CHECK(stats.candidates >= 1);
      const int changed = changed_count(before, after);
      CHECK_EQ(stats.blocks, static_cast<uint32_t>(changed));
      // A box corner can be all margin: the hull is 28 x 9 but its bow tapers,
      // so (13, 29) intersects the box and receives nothing. A piece that did
      // write is counted, one that did not is not.
      CHECK_EQ(stats.pieces, changed > 0 ? 1u : 0u);
      total_blocks += stats.blocks;
      all_changes += changed_cells(before, after);
      before_chunks.push_back(std::move(before));
      after_chunks.push_back(std::move(after));
    }
  }
  std::printf("  shipwreck (12,28) 3x2: %u cells, digest %llu\n", total_blocks,
              static_cast<unsigned long long>(fnv1a(all_changes)));
  CHECK_EQ(fnv1a(all_changes), 14961003614981406088ull);

  // The bridge is the same writer: the one chunk it also lights must carry the
  // same block cells.
  {
    Chunk bridged;
    fill_generated_decorated_chunk(generator, 12, 28, bridged);
    // The (0, 0) entry of the loop above is the chunk itself.
    Chunk direct;
    fill_generated_carved_chunk(generator, 12, 28, direct);
    // The served bridge uses the Overworld's legacy stream; compare it with the
    // same stream, not with the region-aware dimension path.
    decorate_generated_chunk_legacy(generator, direct);
    direct.rebuild_after_bulk_write();
    apply_generated_structures(generator, direct);
    size_t bridge_mismatch = 0;
    for (int x = 0; x < 16; ++x)
      for (int z = 0; z < 16; ++z)
        for (int y = kWgMinY; y < kWgMinY + kWgHeight; ++y)
          if (!(direct.block_state_at(x, y, z) == bridged.block_state_at(x, y, z)))
            ++bridge_mismatch;
    CHECK_EQ(bridge_mismatch, 0);
  }

  const WgPieceTemplate* templ = wg_piece_template_by_name("minecraft:shipwreck/sideways_full");
  CHECK(templ != nullptr);
  if (templ != nullptr) {
    int ignored = 0;
    const WgPiecePalette& palette = kWgPiecePalettes[templ->palette_offset];
    for (uint32_t b = 0; b < templ->block_count; ++b) {
      const WgPieceBlock& block = kWgPieceBlocks[templ->block_offset + b];
      const WgState& state =
          kWgPieceStates[kWgPiecePaletteStates[palette.state_offset + block.state]];
      if (std::strcmp(state.name, "minecraft:structure_block") == 0 ||
          std::strcmp(state.name, "minecraft:air") == 0)
        ++ignored;
    }
    // Every non-ignored template block landed in the six chunks, and none was
    // written twice: the cells are disjoint by construction.
    CHECK_EQ(total_blocks, static_cast<uint32_t>(templ->block_count) - ignored);
  }

  // No structure marker or air was written, and a cell that held water is the
  // only cell whose waterlogged property may be true.
  int markers = 0;
  int bad_waterlog = 0;
  int chests = 0;
  for (size_t c = 0; c < before_chunks.size(); ++c) {
    for (int x = 0; x < 16; ++x)
      for (int z = 0; z < 16; ++z)
        for (int y = kWgMinY; y < kWgMinY + kWgHeight; ++y) {
          const BlockStateKey before_state = before_chunks[c].block_state_at(x, y, z);
          const BlockStateKey after_state = after_chunks[c].block_state_at(x, y, z);
          if (before_state == after_state)
            continue;
          if (after_state.name == "minecraft:structure_block" ||
              after_state.name == "minecraft:air")
            ++markers;
          if (after_state.name == "minecraft:chest")
            ++chests;
          // Only a state that carries the property can be waterlogged; planks
          // and logs simply displace the water.
          if (after_state.properties.find("waterlogged=") == std::string::npos)
            continue;
          const bool waterlogged =
              after_state.properties.find("waterlogged=true") != std::string::npos;
          if (waterlogged != (before_state.name == "minecraft:water"))
            ++bad_waterlog;
        }
  }
  CHECK_EQ(markers, 0);
  CHECK_EQ(bad_waterlog, 0);
  CHECK(chests > 0);
}

// The feature stage's 3x3 region is thread-local. A process-global region
// pointer would let one generation worker overwrite another's while both are
// decorating, and the victim would read or write the wrong neighbour columns.
// This test keeps each worker's nine chunks private, starts every worker on the
// same barrier, and compares all nine outputs with the same region decorated
// sequentially. A global pointer fails here; a thread-local one reproduces the
// baseline exactly.
void test_concurrent_decoration_regions() {
  constexpr size_t kWorkers = 8;
  constexpr int kIterations = 4;
  using Region = std::array<Chunk, 9>;
  const std::array<std::pair<int, int>, kWorkers> centers = {
      {{0, 0}, {64, 0}, {-64, 0}, {0, 64}, {0, -64}, {128, 128}, {-128, 64}, {96, -96}}};

  auto fill_region = [](const WgOverworldGenerator& generator, const std::pair<int, int>& center,
                        Region& region) {
    for (int dz = -1; dz <= 1; ++dz)
      for (int dx = -1; dx <= 1; ++dx) {
        Chunk& chunk = region[static_cast<size_t>((dz + 1) * 3 + (dx + 1))];
        fill_generated_chunk(generator, center.first + dx, center.second + dz, chunk);
      }
  };
  auto make_ptrs = [](Region& region, Chunk*(&ptrs)[9]) {
    for (size_t i = 0; i < region.size(); ++i)
      ptrs[i] = &region[i];
  };

  std::array<Region, kWorkers> pristine{};
  std::array<Region, kWorkers> baseline{};
  WgOverworldGenerator baseline_generator(42);
  for (size_t i = 0; i < kWorkers; ++i) {
    fill_region(baseline_generator, centers[i], pristine[i]);
    baseline[i] = pristine[i];
    Chunk* ptrs[9];
    make_ptrs(baseline[i], ptrs);
    decorate_generated_chunk(baseline_generator, baseline[i][4], nullptr, ptrs);
  }
  CHECK(changed_count(pristine[0][4], baseline[0][4]) > 0);
  // Different centres must produce different columns, or a worker using its
  // neighbour's region could compare equal by accident.
  CHECK(changed_count(baseline[0][4], baseline[1][4]) > 0);

  // libc++ on the supported macOS toolchain has `<barrier>` but marks its
  // members unavailable at this deployment target, so a tiny reusable spin
  // barrier keeps the test portable to the C++20 standard library here.
  struct SpinBarrier {
    explicit SpinBarrier(int participants_in) : participants(participants_in) {}

    void wait() {
      const int observed = generation.load(std::memory_order_acquire);
      if (count.fetch_add(1, std::memory_order_acq_rel) + 1 == participants) {
        count.store(0, std::memory_order_relaxed);
        generation.fetch_add(1, std::memory_order_release);
        return;
      }
      while (generation.load(std::memory_order_acquire) == observed)
        std::this_thread::yield();
    }

    std::atomic<int> count{0};
    std::atomic<int> generation{0};
    int participants;
  };

  std::array<int, kWorkers> mismatches{};
  SpinBarrier start(static_cast<int>(kWorkers));
  SpinBarrier finish(static_cast<int>(kWorkers));
  std::vector<std::thread> workers;
  workers.reserve(kWorkers);
  for (size_t i = 0; i < kWorkers; ++i) {
    workers.emplace_back([&, i]() {
      WgOverworldGenerator generator(42);
      for (int iteration = 0; iteration < kIterations; ++iteration) {
        Region work = pristine[i];
        Chunk* ptrs[9];
        make_ptrs(work, ptrs);
        start.wait();
        decorate_generated_chunk(generator, work[4], nullptr, ptrs);
        finish.wait();
        for (size_t j = 0; j < work.size(); ++j)
          mismatches[i] += changed_count(work[j], baseline[i][j]);
      }
    });
  }
  for (std::thread& worker : workers)
    worker.join();

  int mismatch = 0;
  for (const int count : mismatches)
    mismatch += count;
  std::printf("  concurrent regions: %zu workers x %d rounds, %d mismatched cells\n", kWorkers,
              kIterations, mismatch);
  CHECK_EQ(mismatch, 0);
}

// The server's actual Overworld entry point (`fill_generated_decorated_chunk`)
// is a single-column path, not the 3x3 region above. Exercise that path from
// several workers at once and compare each finished column with the same
// column generated sequentially. This catches shared state in terrain,
// structures, or decoration that the region test cannot see.
void test_concurrent_decorated_columns() {
  constexpr size_t kWorkers = 4;
  constexpr int kRounds = 3;
  const std::array<std::pair<int, int>, kWorkers> centers = {{{0, 0}, {1, 0}, {-1, -1}, {0, 2}}};

  std::array<Chunk, kWorkers> baseline;
  WgOverworldGenerator baseline_generator(42);
  for (size_t i = 0; i < kWorkers; ++i)
    fill_generated_decorated_chunk(baseline_generator, centers[i].first, centers[i].second,
                                   baseline[i]);
  CHECK(changed_count(baseline[0], baseline[1]) > 0);

  struct SpinBarrier {
    explicit SpinBarrier(int participants_in) : participants(participants_in) {}
    void wait() {
      const int observed = generation.load(std::memory_order_acquire);
      if (count.fetch_add(1, std::memory_order_acq_rel) + 1 == participants) {
        count.store(0, std::memory_order_relaxed);
        generation.fetch_add(1, std::memory_order_release);
        return;
      }
      while (generation.load(std::memory_order_acquire) == observed)
        std::this_thread::yield();
    }
    std::atomic<int> count{0};
    std::atomic<int> generation{0};
    int participants;
  };

  std::array<int, kWorkers> mismatches{};
  SpinBarrier start(static_cast<int>(kWorkers));
  SpinBarrier finish(static_cast<int>(kWorkers));
  std::vector<std::thread> workers;
  workers.reserve(kWorkers);
  for (size_t i = 0; i < kWorkers; ++i) {
    workers.emplace_back([&, i]() {
      WgOverworldGenerator generator(42);
      for (int round = 0; round < kRounds; ++round) {
        Chunk chunk;
        start.wait();
        fill_generated_decorated_chunk(generator, centers[i].first, centers[i].second, chunk);
        finish.wait();
        mismatches[i] += changed_count(chunk, baseline[i]);
      }
    });
  }
  for (std::thread& worker : workers)
    worker.join();

  int mismatch = 0;
  for (const int count : mismatches)
    mismatch += count;
  std::printf("  concurrent served columns: %zu workers x %d rounds, %d mismatched cells\n",
              kWorkers, kRounds, mismatch);
  CHECK_EQ(mismatch, 0);
}

// The served Overworld column and the dimension path intentionally use different
// decoration streams. Pin the mapping, then pin the served column's output
// against the legacy stream and show that the region-aware stream really is a
// different output on at least one of these chunks. A later switch of the
// served bridge to `decorate_generated_chunk` therefore fails here even if the
// golden feature fixtures are regenerated.
void test_decoration_stream_selection() {
  CHECK(wg_decoration_stream(WgDimensionKind::kOverworld) == WgDecorationStream::kLegacy);
  CHECK(wg_decoration_stream(WgDimensionKind::kNether) == WgDecorationStream::kRegionAware);
  CHECK(wg_decoration_stream(WgDimensionKind::kEnd) == WgDecorationStream::kRegionAware);

  WgOverworldGenerator generator(42);
  const std::array<std::pair<int, int>, 3> chunks = {{{0, 0}, {1, 0}, {-1, -1}}};
  bool region_aware_differs = false;
  for (const auto& [cx, cz] : chunks) {
    Chunk served;
    fill_generated_decorated_chunk(generator, cx, cz, served);

    Chunk legacy;
    fill_generated_carved_chunk(generator, cx, cz, legacy);
    decorate_generated_chunk_legacy(generator, legacy);
    apply_generated_structures(generator, legacy);
    legacy.rebuild_after_bulk_write();
    CHECK_EQ(changed_count(served, legacy), 0);

    Chunk region_aware;
    fill_generated_carved_chunk(generator, cx, cz, region_aware);
    decorate_generated_chunk(generator, region_aware);
    apply_generated_structures(generator, region_aware);
    region_aware.rebuild_after_bulk_write();
    if (changed_count(served, region_aware) > 0)
      region_aware_differs = true;
  }
  CHECK(region_aware_differs);
}

} // namespace

int main(int argc, char** argv) {
  // CMake passes tests/fixtures/worldgen; the default is for a hand run from the
  // repository root.
  g_fixtures = argc > 1 ? argv[1] : "tests/fixtures/worldgen";

  // ---- the carried registries -------------------------------------------
  // 226 pack configured features plus the inline definitions hoisted out of
  // the selector configs (names under `minecraft:inline/`).
  CHECK_EQ(std::size(kWgConfiguredFeatures), 242);
  size_t pack_configured = 0;
  size_t inline_configured = 0;
  for (const WgConfiguredFeature& cf : kWgConfiguredFeatures)
    (std::strncmp(cf.name, "minecraft:inline/", 17) == 0 ? inline_configured : pack_configured)++;
  CHECK_EQ(pack_configured, 226);
  CHECK_EQ(inline_configured, 16);
  CHECK_EQ(std::size(kWgPlacedFeatures), 262);
  CHECK_EQ(std::size(kWgCarvers), 4);
  CHECK_EQ(std::size(kWgStructures), 34);
  CHECK_EQ(std::size(kWgStructureSets), 20);
  CHECK_EQ(std::size(kLivingEntityTypes), 93);
  CHECK(sorted_by_name(kWgConfiguredFeatures, std::size(kWgConfiguredFeatures)));
  CHECK(sorted_by_name(kWgPlacedFeatures, std::size(kWgPlacedFeatures)));
  CHECK(sorted_by_name(kWgStructures, std::size(kWgStructures)));

  // Every placed feature names a configured one.
  size_t bad_placed = 0;
  for (const WgPlacedFeature& placed : kWgPlacedFeatures)
    if (find_by_name(kWgConfiguredFeatures, std::size(kWgConfiguredFeatures), placed.feature) ==
        nullptr)
      ++bad_placed;
  CHECK_EQ(bad_placed, 0);

  // Every biome decoration entry names a placed feature.
  size_t bad_entry = 0;
  for (const WgBiomeFeatureEntry& entry : kWgBiomeFeatureEntries)
    if (find_by_name(kWgPlacedFeatures, std::size(kWgPlacedFeatures), entry.placed_feature) ==
        nullptr)
      ++bad_entry;
  CHECK_EQ(bad_entry, 0);

  // Every structure set entry names a structure.
  size_t bad_set = 0;
  for (const WgStructureSetEntry& entry : kWgStructureSetEntries)
    if (find_by_name(kWgStructures, std::size(kWgStructures), entry.structure) == nullptr)
      ++bad_set;
  CHECK_EQ(bad_set, 0);

  // Every selector child and every selector default resolves against the
  // registry its own `kind` names.
  size_t bad_child = 0;
  for (const WgSelectorChild& child : kWgSelectorChildren) {
    const bool resolved =
        child.kind == 0 ? find_by_name(kWgPlacedFeatures, std::size(kWgPlacedFeatures),
                                       child.feature) != nullptr
                        : find_by_name(kWgConfiguredFeatures, std::size(kWgConfiguredFeatures),
                                       child.feature) != nullptr;
    if (!resolved)
      ++bad_child;
  }
  CHECK_EQ(bad_child, 0);
  size_t bad_default = 0;
  for (const WgSelector& sel : kWgSelectors) {
    if (sel.default_feature[0] == '\0')
      continue;
    if (find_by_name(kWgPlacedFeatures, std::size(kWgPlacedFeatures), sel.default_feature) ==
            nullptr &&
        find_by_name(kWgConfiguredFeatures, std::size(kWgConfiguredFeatures),
                     sel.default_feature) == nullptr)
      ++bad_default;
  }
  CHECK_EQ(bad_default, 0);

  // Every parsed per-kind config index is in range.
  size_t bad_config = 0;
  for (const WgConfiguredFeature& cf : kWgConfiguredFeatures) {
    if (cf.config == UINT32_MAX)
      continue;
    switch (cf.kind) {
    case WgFeatureKind::kOre:
      bad_config += cf.config >= std::size(kWgOreConfigs);
      break;
    case WgFeatureKind::kSimpleBlock:
      bad_config += cf.config >= std::size(kWgSimpleBlockConfigs);
      break;
    case WgFeatureKind::kBlockPile:
      bad_config += cf.config >= std::size(kWgBlockPileConfigs);
      break;
    case WgFeatureKind::kDisk:
      bad_config += cf.config >= std::size(kWgDiskConfigs);
      break;
    case WgFeatureKind::kSpring:
      bad_config += cf.config >= std::size(kWgSpringConfigs);
      break;
    case WgFeatureKind::kTree:
      bad_config += cf.config >= std::size(kWgTreeConfigs);
      break;
    case WgFeatureKind::kScatteredOre:
      bad_config += cf.config >= std::size(kWgOreConfigs);
      break;
    case WgFeatureKind::kBlockColumn:
      bad_config += cf.config >= std::size(kWgColumnConfigs);
      break;
    case WgFeatureKind::kRandomSelector:
    case WgFeatureKind::kSimpleRandomSelector:
    case WgFeatureKind::kRandomBooleanSelector:
      bad_config += cf.config >= std::size(kWgSelectors);
      break;
    default:
      break;
    }
  }
  CHECK_EQ(bad_config, 0);

  // ---- the block-column tables ------------------------------------------
  // The kind is only as good as the indirections resolve: a layer names a state
  // provider and an IntProvider, the provider names its own state (or its
  // weighted entries, or the source and the values provider of a drawn integer
  // property), a weighted IntProvider names its entries, and the config's
  // predicate exists.
  CHECK(std::size(kWgColumnConfigs) > 0);
  CHECK(std::size(kWgColumnLayers) >= std::size(kWgColumnConfigs));
  size_t bad_column_config = 0;
  for (const WgColumnConfig& cfg : kWgColumnConfigs) {
    if (cfg.layer_count == 0 || cfg.layer_offset + cfg.layer_count > std::size(kWgColumnLayers))
      ++bad_column_config;
    if (cfg.direction >= 6 || cfg.prioritize_tip > 1)
      ++bad_column_config;
    if (cfg.allowed_placement >= std::size(kWgPredicates))
      ++bad_column_config;
  }
  CHECK_EQ(bad_column_config, 0);
  size_t bad_column_layer = 0;
  for (const WgColumnLayer& layer : kWgColumnLayers)
    if (layer.provider >= std::size(kWgStateProviders) ||
        layer.height >= std::size(kWgIntProviders))
      ++bad_column_layer;
  CHECK_EQ(bad_column_layer, 0);
  size_t bad_provider = 0;
  for (const WgStateProvider& p : kWgStateProviders) {
    if (p.kind == WgStateProviderKind::kSimple) {
      bad_provider += p.state.name[0] == '\0';
      continue;
    }
    if (p.kind == WgStateProviderKind::kWeighted) {
      bad_provider += p.offset + p.count > std::size(kWgWeightedStates);
      continue;
    }
    bad_provider += p.source >= std::size(kWgStateProviders);
    bad_provider += p.values >= std::size(kWgIntProviders);
    bad_provider += p.property[0] == '\0';
  }
  CHECK_EQ(bad_provider, 0);
  size_t bad_int_provider = 0;
  for (const WgIntProvider& p : kWgIntProviders) {
    if (p.kind == WgIntProviderKind::kWeightedList) {
      if (p.offset + p.count > std::size(kWgWeightedInts))
        ++bad_int_provider;
      for (uint32_t i = 0; i < p.count; ++i)
        if (kWgWeightedInts[p.offset + i].provider >= std::size(kWgIntProviders))
          ++bad_int_provider;
      continue;
    }
    bad_int_provider += p.lo > p.hi;
  }
  CHECK_EQ(bad_int_provider, 0);

  // The pack's own data drives every sampler the kind has: all four IntProvider
  // kinds and all three state providers are named by a real layer, so the ported
  // sampling runs rather than sitting dormant.
  bool int_kinds[4] = {false, false, false, false};
  for (const WgIntProvider& p : kWgIntProviders)
    int_kinds[static_cast<uint32_t>(p.kind)] = true;
  CHECK(int_kinds[0] && int_kinds[1] && int_kinds[2] && int_kinds[3]);
  bool provider_kinds[3] = {false, false, false};
  for (const WgStateProvider& p : kWgStateProviders)
    provider_kinds[static_cast<uint32_t>(p.kind)] = true;
  CHECK(provider_kinds[0] && provider_kinds[1] && provider_kinds[2]);

  // Every `block_column` and `scattered_ore` feature in the pack was flattenable:
  // the kind is applied for all of them, not only for the easy ones.
  size_t column_rows = 0;
  size_t scattered_rows = 0;
  size_t unparsed_rows = 0;
  for (const WgConfiguredFeature& cf : kWgConfiguredFeatures) {
    if (cf.kind == WgFeatureKind::kBlockColumn) {
      ++column_rows;
      unparsed_rows += cf.config == UINT32_MAX;
    }
    if (cf.kind == WgFeatureKind::kScatteredOre) {
      ++scattered_rows;
      unparsed_rows += cf.config == UINT32_MAX;
    }
  }
  CHECK_EQ(column_rows, 8);
  CHECK_EQ(scattered_rows, 2);
  CHECK_EQ(unparsed_rows, 0);

  // A block predicate carries its own offset and the runtime applies it:
  // sugar cane's water check asks about the cell beside the plant, which is the
  // only reason it can pass at all.
  size_t offset_predicates = 0;
  for (const WgPredicate& p : kWgPredicates)
    if (p.ox != 0 || p.oy != 0 || p.oz != 0)
      ++offset_predicates;
  CHECK(offset_predicates > 0);

  // ---- the monster-room kind ---------------------------------------------
  // The applied kinds with **no config table**: `minecraft:monster_room` and
  // `minecraft:freeze_top_layer` both carry `{}` (`NoneFeatureConfiguration`),
  // so there is nothing to flatten and their rows' `config` slots are
  // legitimately UINT32_MAX. The stage therefore has to dispatch those kinds on
  // their `kind` alone, and the pack's placements (`monster_room`,
  // `monster_room_deep`, the bare-`biome` `freeze_top_layer`) all reach them.
  // The kind count tracks the generated table (the stats' per-kind array is
  // sized from it) and `kOther` has to stay last: it is the one kind the stage
  // never applies, and every kind before it keeps its ordinal.
  CHECK_EQ(static_cast<uint32_t>(kWgFeatureKindCount),
           static_cast<uint32_t>(WgFeatureKind::kOther) + 1);
  CHECK(kWgFeatureKindCount >= 18);
  // ...and no pack row falls back to it. The generator refuses a configured
  // feature type with no runtime kind, so `kOther` is a sentinel rather than a
  // place a silently unapplied feature can hide.
  size_t other_rows = 0;
  for (const WgConfiguredFeature& cf : kWgConfiguredFeatures)
    other_rows += cf.kind == WgFeatureKind::kOther;
  CHECK_EQ(other_rows, 0);
  size_t monster_room_rows = 0;
  for (const WgConfiguredFeature& cf : kWgConfiguredFeatures) {
    if (cf.kind != WgFeatureKind::kMonsterRoom)
      continue;
    ++monster_room_rows;
    CHECK_EQ(cf.config, UINT32_MAX); // no per-kind config to point at
  }
  CHECK_EQ(monster_room_rows, 1);
  for (const char* placed_name : {"minecraft:monster_room", "minecraft:monster_room_deep"}) {
    const WgPlacedFeature* placed =
        find_by_name(kWgPlacedFeatures, std::size(kWgPlacedFeatures), placed_name);
    CHECK(placed != nullptr);
    if (placed == nullptr)
      continue;
    const WgConfiguredFeature* cf =
        find_by_name(kWgConfiguredFeatures, std::size(kWgConfiguredFeatures), placed->feature);
    CHECK(cf != nullptr && cf->kind == WgFeatureKind::kMonsterRoom);
  }

  // The second config-less kind: `minecraft:freeze_top_layer`'s configured
  // feature also carries `{}`, so its row is UINT32_MAX too. It has one
  // placement, whose only modifier is the bare `biome` filter, so it runs once
  // per chunk in every biome that lists it.
  size_t freeze_rows = 0;
  for (const WgConfiguredFeature& cf : kWgConfiguredFeatures) {
    if (cf.kind != WgFeatureKind::kFreezeTopLayer)
      continue;
    ++freeze_rows;
    CHECK_EQ(cf.config, UINT32_MAX);
  }
  CHECK_EQ(freeze_rows, 1);
  {
    const WgPlacedFeature* placed =
        find_by_name(kWgPlacedFeatures, std::size(kWgPlacedFeatures), "minecraft:freeze_top_layer");
    CHECK(placed != nullptr);
    if (placed != nullptr) {
      const WgConfiguredFeature* cf =
          find_by_name(kWgConfiguredFeatures, std::size(kWgConfiguredFeatures), placed->feature);
      CHECK(cf != nullptr && cf->kind == WgFeatureKind::kFreezeTopLayer);
    }
  }

  // ---- the seagrass kind --------------------------------------------------
  // The heaviest of the kinds the stage did not apply before this slice: four
  // configured features (the probabilities 0.3/0.4/0.6/0.8 the pack's oceans
  // and rivers place) and eight placed features over ten biomes. Its config is
  // one float, so every row points at a real `kWgSeagrassConfigs` entry and the
  // probability is a chance. The placed half is pinned by name too: `river`
  // lists `minecraft:seagrass_river` at the decoration step, which is what
  // makes the kind reachable in the pinned chunk (0,0) below.
  size_t seagrass_rows = 0;
  for (const WgConfiguredFeature& cf : kWgConfiguredFeatures) {
    if (cf.kind != WgFeatureKind::kSeagrass)
      continue;
    ++seagrass_rows;
    CHECK(cf.config < std::size(kWgSeagrassConfigs));
    if (cf.config < std::size(kWgSeagrassConfigs)) {
      const float probability = kWgSeagrassConfigs[cf.config].probability;
      CHECK(probability > 0.0f && probability <= 1.0f);
    }
  }
  CHECK_EQ(seagrass_rows, 4);
  size_t seagrass_placed = 0;
  for (const WgPlacedFeature& placed : kWgPlacedFeatures) {
    const WgConfiguredFeature* cf =
        find_by_name(kWgConfiguredFeatures, std::size(kWgConfiguredFeatures), placed.feature);
    if (cf != nullptr && cf->kind == WgFeatureKind::kSeagrass)
      ++seagrass_placed;
  }
  CHECK_EQ(seagrass_placed, 8);
  {
    const WgPlacedFeature* river_plant =
        find_by_name(kWgPlacedFeatures, std::size(kWgPlacedFeatures), "minecraft:seagrass_river");
    CHECK(river_plant != nullptr);
    if (river_plant != nullptr) {
      const WgConfiguredFeature* cf = find_by_name(
          kWgConfiguredFeatures, std::size(kWgConfiguredFeatures), river_plant->feature);
      CHECK(cf != nullptr && cf->kind == WgFeatureKind::kSeagrass);
      CHECK_EQ(placed_step_for_biome("minecraft:river", "minecraft:seagrass_river"), 9);
    }
  }

  // ---- the fallen-tree kind ------------------------------------------------
  // Five configured features and five placed features, in the overworld's
  // forests and taigas. Unlike every kind before it this one is reached only as
  // a *selector child*: `trees_plains` and its siblings list
  // `minecraft:fallen_oak_tree` (and the other four) among their children, so
  // the child's own placement -- a `would_survive` sapling predicate -- runs
  // first and the kind is applied to whatever cell survives it. Its config is
  // two state providers (the trunk, and the `attached_to_logs` mushroom), one
  // log-length provider and the two decorator slots, and the pack's five rows
  // are the whole kind, so a config the generator could not flatten would show
  // here as a row pointing nowhere.
  size_t fallen_rows = 0;
  size_t fallen_vines = 0;
  for (const WgConfiguredFeature& cf : kWgConfiguredFeatures) {
    if (cf.kind != WgFeatureKind::kFallenTree)
      continue;
    ++fallen_rows;
    CHECK(cf.config < std::size(kWgFallenTreeConfigs));
    if (cf.config < std::size(kWgFallenTreeConfigs)) {
      const WgFallenTreeConfig& cfg = kWgFallenTreeConfigs[cf.config];
      CHECK(cfg.trunk < std::size(kWgStateProviders));
      CHECK(cfg.log_length < std::size(kWgIntProviders));
      CHECK(cfg.log_block < std::size(kWgStateProviders));
      CHECK(cfg.probability > 0.0f && cfg.probability <= 1.0f);
      CHECK(cfg.stump_vine <= 1);
      fallen_vines += cfg.stump_vine;
    }
  }
  CHECK_EQ(fallen_rows, 5);
  // Two of the five stumps carry `trunk_vine` (oak and jungle), the other
  // three carry none: the pack's own split, not a default the generator
  // invented, and exactly the two decorator species the file's comments name.
  CHECK_EQ(fallen_vines, 2);
  size_t fallen_placed = 0;
  for (const WgPlacedFeature& placed : kWgPlacedFeatures) {
    const WgConfiguredFeature* cf =
        find_by_name(kWgConfiguredFeatures, std::size(kWgConfiguredFeatures), placed.feature);
    if (cf != nullptr && cf->kind == WgFeatureKind::kFallenTree)
      ++fallen_placed;
  }
  CHECK_EQ(fallen_placed, 5);
  // Reachability: the selectors name the placed rows with kind 0, which is the
  // code that makes the runtime run that row's own placement before the
  // configured feature (kind 1 would place it bare).
  size_t fallen_children = 0;
  for (const WgSelectorChild& child : kWgSelectorChildren) {
    const WgPlacedFeature* placed =
        find_by_name(kWgPlacedFeatures, std::size(kWgPlacedFeatures), child.feature);
    if (placed == nullptr)
      continue;
    const WgConfiguredFeature* cf =
        find_by_name(kWgConfiguredFeatures, std::size(kWgConfiguredFeatures), placed->feature);
    if (cf != nullptr && cf->kind == WgFeatureKind::kFallenTree) {
      ++fallen_children;
      CHECK_EQ(static_cast<uint32_t>(child.kind), 0u);
    }
  }
  CHECK(fallen_children >= 10);
  {
    const WgPlacedFeature* trees =
        find_by_name(kWgPlacedFeatures, std::size(kWgPlacedFeatures), "minecraft:trees_plains");
    CHECK(trees != nullptr);
    CHECK_EQ(placed_step_for_biome("minecraft:plains", "minecraft:trees_plains"), 9);
  }

  // ---- the multiface-growth kind ------------------------------------------
  // Two configured features share the type: glow lichen (the one every
  // overworld biome lists, at the underground step 9) and sculk vein (deep
  // dark). Their config is not a state provider but a block plus the three
  // face flags `validDirections` is built from, so each row is checked field by
  // field against the pack's own JSON — including the flag that differs between
  // them: glow lichen grows on ceilings and walls but not the floor, sculk vein
  // on all three. The block lists are the pack's own ten and eight.
  size_t multiface_rows = 0;
  for (const WgConfiguredFeature& cf : kWgConfiguredFeatures) {
    if (cf.kind != WgFeatureKind::kMultifaceGrowth)
      continue;
    ++multiface_rows;
    CHECK(cf.config < std::size(kWgMultifaceGrowthConfigs));
    if (cf.config >= std::size(kWgMultifaceGrowthConfigs))
      continue;
    const WgMultifaceGrowthConfig& cfg = kWgMultifaceGrowthConfigs[cf.config];
    CHECK_EQ(cfg.search_range, 20);
    CHECK_EQ(cfg.can_place_on_ceiling, 1);
    CHECK_EQ(cfg.can_place_on_wall, 1);
    size_t listed = 1;
    for (const char* p = cfg.can_be_placed_on; *p != '\0'; ++p)
      listed += *p == '|';
    CHECK(name_in_pipe_list(cfg.can_be_placed_on, "minecraft:stone"));
    CHECK(name_in_pipe_list(cfg.can_be_placed_on, "minecraft:deepslate"));
    CHECK(!name_in_pipe_list(cfg.can_be_placed_on, "minecraft:dirt"));
    if (std::strcmp(cfg.state.name, "minecraft:glow_lichen") == 0) {
      CHECK_EQ(cfg.can_place_on_floor, 0);
      CHECK(cfg.chance_of_spreading > 0.49f && cfg.chance_of_spreading < 0.51f);
      CHECK_EQ(listed, 10);
    } else {
      CHECK(std::strcmp(cfg.state.name, "minecraft:sculk_vein") == 0);
      CHECK_EQ(cfg.can_place_on_floor, 1);
      CHECK(cfg.chance_of_spreading > 0.99f);
      CHECK_EQ(listed, 8);
    }
  }
  CHECK_EQ(multiface_rows, 2);
  {
    const WgPlacedFeature* lichen =
        find_by_name(kWgPlacedFeatures, std::size(kWgPlacedFeatures), "minecraft:glow_lichen");
    CHECK(lichen != nullptr);
    if (lichen != nullptr) {
      const WgConfiguredFeature* cf =
          find_by_name(kWgConfiguredFeatures, std::size(kWgConfiguredFeatures), lichen->feature);
      CHECK(cf != nullptr && cf->kind == WgFeatureKind::kMultifaceGrowth);
      // The placement's own modifiers, in the pack's order: count, height_range,
      // in_square, then the `surface_relative_threshold_filter` this slice adds
      // — the row that keeps the growth 13 blocks below the ocean floor.
      bool saw_threshold = false;
      for (uint32_t i = 0; i < lichen->placement_count; ++i) {
        const WgPlacement& m = kWgPlacements[lichen->placement_offset + i];
        if (m.kind != WgPlacementKind::kSurfaceRelativeThresholdFilter)
          continue;
        saw_threshold = true;
        CHECK_EQ(static_cast<uint32_t>(m.i0), 3u); // OCEAN_FLOOR_WG
        CHECK_EQ(m.i2, 0);                         // no min_inclusive
        CHECK_EQ(m.i3, -13);                       // max_inclusive
        CHECK_EQ(m.i4, 1);                         // ...and it is present
      }
      CHECK(saw_threshold);
    }
    CHECK_EQ(placed_step_for_biome("minecraft:plains", "minecraft:glow_lichen"), 9);
  }

  // ---- the ice-spike kind --------------------------------------------------
  // The last algorithmic kind the stage did not apply: one configured feature
  // (`minecraft:ice_spike`), one placement (`count 3`, `in_square`, a
  // `MOTION_BLOCKING` heightmap and the `biome` filter), and one flat config row
  // whose `a` is the spike's state, `ref` the block it may stand on and `ref2`
  // the tag its writes may replace. It has no ints or floats: the height curve
  // is `SpikeFeature`'s own draws, not a config.
  size_t spike_rows = 0;
  for (const WgConfiguredFeature& cf : kWgConfiguredFeatures) {
    if (cf.kind != WgFeatureKind::kSpike)
      continue;
    ++spike_rows;
    CHECK(cf.config < std::size(kWgExtraConfigs));
    if (cf.config < std::size(kWgExtraConfigs)) {
      const WgExtraConfig& cfg = kWgExtraConfigs[cf.config];
      CHECK(std::strcmp(cfg.a.name, "minecraft:packed_ice") == 0);
      CHECK(name_in_pipe_list(cfg.ref, "minecraft:snow_block"));
      CHECK(name_in_pipe_list(cfg.ref2, "minecraft:snow_block"));
      CHECK(name_in_pipe_list(cfg.ref2, "minecraft:dirt"));
      // The tag is not "any overworld block": stone is what stops the trunk.
      CHECK(!name_in_pipe_list(cfg.ref2, "minecraft:stone"));
      CHECK_EQ(cfg.int_count, 0);
      CHECK_EQ(cfg.float_count, 0);
    }
  }
  CHECK_EQ(spike_rows, 1);
  {
    const WgPlacedFeature* ice_spike =
        find_by_name(kWgPlacedFeatures, std::size(kWgPlacedFeatures), "minecraft:ice_spike");
    CHECK(ice_spike != nullptr);
    if (ice_spike != nullptr) {
      const WgConfiguredFeature* cf =
          find_by_name(kWgConfiguredFeatures, std::size(kWgConfiguredFeatures), ice_spike->feature);
      CHECK(cf != nullptr && cf->kind == WgFeatureKind::kSpike);
      // The step that makes the kind reachable on the constructed chunk below.
      CHECK_EQ(placed_step_for_biome("minecraft:ice_spikes", "minecraft:ice_spike"), 4);
    }
  }

  // ---- the geode kind ------------------------------------------------------
  // One configured feature and one placement. Its config is the only one that
  // needs all five flat state slots (filling, inner, alternate inner, middle,
  // outer), both name lists (the cannot-replace gate and the validity scan) and
  // the inner placements; the pack leaves most of its numbers to the codec's
  // defaults, so a zero in those slots would be the generator's bug, not the
  // pack's choice.
  size_t geode_rows = 0;
  for (const WgConfiguredFeature& cf : kWgConfiguredFeatures) {
    if (cf.kind != WgFeatureKind::kGeode)
      continue;
    ++geode_rows;
    CHECK(cf.config < std::size(kWgExtraConfigs));
    if (cf.config < std::size(kWgExtraConfigs)) {
      const WgExtraConfig& cfg = kWgExtraConfigs[cf.config];
      CHECK(std::strcmp(cfg.a.name, "minecraft:air") == 0);
      CHECK(std::strcmp(cfg.b.name, "minecraft:amethyst_block") == 0);
      CHECK(std::strcmp(cfg.c.name, "minecraft:budding_amethyst") == 0);
      CHECK(std::strcmp(cfg.d.name, "minecraft:calcite") == 0);
      CHECK(std::strcmp(cfg.e.name, "minecraft:smooth_basalt") == 0);
      CHECK(name_in_pipe_list(cfg.ref, "minecraft:bedrock"));
      CHECK(name_in_pipe_list(cfg.ref2, "minecraft:water"));
      CHECK(cfg.child2 != nullptr);
      CHECK(std::strstr(cfg.child2, "minecraft:amethyst_cluster") != nullptr);
      CHECK_EQ(cfg.int_count, 9);    // outer wall, points, offset, gen offsets, threshold
      CHECK_EQ(cfg.float_count, 11); // noise, chances, layers, crack
    }
  }
  CHECK_EQ(geode_rows, 1);
  {
    const WgPlacedFeature* geode =
        find_by_name(kWgPlacedFeatures, std::size(kWgPlacedFeatures), "minecraft:amethyst_geode");
    CHECK(geode != nullptr);
    if (geode != nullptr) {
      const WgConfiguredFeature* cf =
          find_by_name(kWgConfiguredFeatures, std::size(kWgConfiguredFeatures), geode->feature);
      CHECK(cf != nullptr && cf->kind == WgFeatureKind::kGeode);
      CHECK_EQ(placed_step_for_biome("minecraft:ice_spikes", "minecraft:amethyst_geode"), 2);
    }
  }

  // The three non-mob living types the AI roster excludes are still named.
  bool has_player = false, has_armor_stand = false, has_mannequin = false;
  for (const LivingEntityType& type : kLivingEntityTypes) {
    has_player |= std::strcmp(type.name, "minecraft:player") == 0;
    has_armor_stand |= std::strcmp(type.name, "minecraft:armor_stand") == 0;
    has_mannequin |= std::strcmp(type.name, "minecraft:mannequin") == 0;
  }
  CHECK(has_player && has_armor_stand && has_mannequin);

  // ---- the decoration stage ---------------------------------------------
  WgOverworldGenerator generator(42);
  Chunk before;
  fill_generated_chunk(generator, 0, 0, before);
  Chunk after = before;
  WgDecorationStats stats;
  decorate_generated_chunk(generator, after, &stats);

  CHECK_EQ(stats.steps, 11);
  CHECK(stats.considered > 0);
  CHECK(stats.applied > 0);
  // The block-column kind is dispatched in this chunk, not parked among the
  // skipped ones: its own counter sees the application its placement produced.
  CHECK(stats.applied_by_kind[static_cast<uint32_t>(WgFeatureKind::kBlockColumn)] >= 1);
  // The config-less kinds too. This is the counter that would read zero if the
  // stage mistook a kind's unset config slot (UINT32_MAX by design) for a
  // config it could not flatten and counted the applications as skipped.
  CHECK(stats.applied_by_kind[static_cast<uint32_t>(WgFeatureKind::kMonsterRoom)] >= 1);
  // The freeze-top-layer kind is dispatched in this chunk as well, and because
  // it draws no random numbers it cannot shift any other kind's stream: the
  // warm chunk's golden dump differs only in this counter and `applied`.
  CHECK(stats.applied_by_kind[static_cast<uint32_t>(WgFeatureKind::kFreezeTopLayer)] >= 1);
  // The seagrass kind is dispatched here through the chunk's river cells, and
  // unlike freeze top layer it draws: the placed half of this kind's fixture
  // moves by a ledger row or two per chunk, and the chunks whose surface river
  // cells hold water move cells as well — which is why the fixture is re-cut
  // rather than "ledger only" the way the config-less kind's was.
  CHECK(stats.applied_by_kind[static_cast<uint32_t>(WgFeatureKind::kSeagrass)] >= 1);
  CHECK(stats.blocks_placed > 0);
  CHECK(stats.positions > 0);
  // The applications whose config the generator could not flatten (`kOther`)
  // are counted, not silently dropped.
  CHECK(stats.skipped_kind > 0);

  // Every changed cell's new state is one the applied kinds' configs name.
  std::set<std::string> applied_states;
  for (const WgOreTarget& target : kWgOreTargets)
    applied_states.insert(target.state.name);
  for (const WgState& s : kWgSimpleBlockConfigs)
    applied_states.insert(s.name);
  for (const WgState& s : kWgBlockPileConfigs)
    applied_states.insert(s.name);
  for (const WgDiskConfig& d : kWgDiskConfigs)
    applied_states.insert(d.state.name);
  for (const WgSpringConfig& s : kWgSpringConfigs)
    applied_states.insert(s.state.name);
  for (const WgTreeConfig& t : kWgTreeConfigs) {
    applied_states.insert(t.trunk.name);
    applied_states.insert(t.foliage.name);
  }
  // The block column's layers, through their two indirections.
  for (const WgColumnLayer& layer : kWgColumnLayers)
    collect_provider_states(layer.provider, applied_states);
  applied_states.insert("minecraft:dirt"); // the trunk's `setDirtAt`
  applied_states.insert("minecraft:air");
  // The monster room's own blocks (`MonsterRoomFeature.place`).
  applied_states.insert("minecraft:cobblestone");
  applied_states.insert("minecraft:mossy_cobblestone");
  applied_states.insert("minecraft:cave_air");
  applied_states.insert("minecraft:chest");
  applied_states.insert("minecraft:spawner");
  // The freeze-top-layer kind's own two blocks (`SnowAndFreezeFeature`).
  applied_states.insert("minecraft:snow");
  applied_states.insert("minecraft:ice");
  // The seagrass kind's two (`SeagrassFeature`).
  applied_states.insert("minecraft:seagrass");
  applied_states.insert("minecraft:tall_seagrass");
  // The sulfur-spike column kind's state.
  applied_states.insert("minecraft:sulfur_spike");
  // The fallen-tree kind's own states: the trunk provider (the stump's state,
  // and the fallen logs' with their `axis` retargeted by `with_property`), the
  // `attached_to_logs` mushroom provider, and the vine a `trunk_vine` stump
  // writes.
  for (const WgFallenTreeConfig& f : kWgFallenTreeConfigs) {
    collect_provider_states(f.trunk, applied_states);
    if (f.log_block != UINT32_MAX)
      collect_provider_states(f.log_block, applied_states);
  }
  applied_states.insert("minecraft:vine");
  // The multiface-growth kind's two blocks (glow lichen and sculk vein).
  for (const WgMultifaceGrowthConfig& g : kWgMultifaceGrowthConfigs)
    applied_states.insert(g.state.name);
  // The fossil kind's template states and the diamonds overlay's rule output.
  for (const WgState& s : kWgFossilStates)
    applied_states.insert(s.name);
  for (const WgFossilRule& r : kWgFossilRules)
    applied_states.insert(r.output.name);

  auto count_changes = [&](const Chunk& from, const Chunk& to, size_t& changed_out,
                           size_t& unexpected_out) {
    changed_out = 0;
    unexpected_out = 0;
    for (size_t si = 0; si < to.sections.size(); ++si) {
      for (int cell = 0; cell < kBlockCellsPerSection; ++cell) {
        const std::string a = cell_name(from, si, cell);
        const std::string b = cell_name(to, si, cell);
        if (a == b)
          continue;
        ++changed_out;
        if (applied_states.find(b) == applied_states.end())
          ++unexpected_out;
      }
    }
  };
  size_t changed = 0;
  size_t unexpected = 0;
  count_changes(before, after, changed, unexpected);
  CHECK(changed > 0);
  CHECK_EQ(unexpected, 0);

  // The tree kind really ran: a surface chunk with a forest biome places logs
  // and leaves, not only ore. The sample chunk's own tree draws moved in the
  // current decoration stream, so use a chunk whose fixture carries a tree.
  Chunk tree_chunk;
  fill_generated_chunk(generator, -1, -1, tree_chunk);
  decorate_generated_chunk(generator, tree_chunk, nullptr);
  size_t logs = 0;
  size_t leaves = 0;
  for (size_t si = 0; si < tree_chunk.sections.size(); ++si)
    for (int cell = 0; cell < kBlockCellsPerSection; ++cell) {
      const std::string b = cell_name(tree_chunk, si, cell);
      if (b.size() > 4 && b.substr(b.size() - 4) == "_log")
        ++logs;
      if (b.size() > 7 && b.substr(b.size() - 7) == "_leaves")
        ++leaves;
    }
  CHECK(logs > 0);
  CHECK(leaves > 0);
  // Every write succeeded, but a write over an equal state (an ore target of
  // granite over noise-stage granite) shows no name change, so the visible
  // diff is at most the write count.
  CHECK(changed <= stats.blocks_placed);

  // ---- the seagrass kind is really placed, not only dispatched -------------
  // The chunk that carries the *effect* is (-13,-11), not the sample (0,0): its
  // surface river cells hold water at the `OCEAN_FLOOR` height, while (0,0)'s
  // river biome cells are underground (its quart-column biomes are 825
  // dark_forest, 379 river, 332 lush_caves) so its surface candidates find no
  // water. (0,0) still dispatches the kind — the counter above is the evidence —
  // and this block checks that whatever it did obeys the same invariants.
  //
  // The invariants are the rule's own: a new plant sits only in a cell that
  // held water before decoration and only on a motion-blocking support that is
  // not magma block, and a tall plant is always both halves, never a lone upper
  // or lower one.
  struct PlantScan {
    int plants = 0;
    int tall_lower = 0;
    int tall_upper = 0;
    int wrong_cell = 0;
    int wrong_support = 0;
    int wrong_upper = 0;
  };
  auto scan_plants = [](const Chunk& from, const Chunk& to) {
    PlantScan out;
    for (int x = 0; x < 16; ++x)
      for (int z = 0; z < 16; ++z)
        for (int y = kWgMinY; y < kWgMinY + kWgHeight; ++y) {
          const BlockStateKey now = to.block_state_at(x, y, z);
          if (now.name != "minecraft:seagrass" && now.name != "minecraft:tall_seagrass")
            continue;
          if (from.block_state_at(x, y, z).name != "minecraft:water")
            ++out.wrong_cell;
          if (now.name == "minecraft:seagrass") {
            ++out.plants;
            const std::string support = to.block_state_at(x, y - 1, z).name;
            if (!block_blocks_motion(support) || support == "minecraft:magma_block")
              ++out.wrong_support;
          } else if (now.properties == "half=lower") {
            ++out.tall_lower;
            // The support rule is the *plant's*, so it is checked on the base
            // cell; an upper half's support is its own lower half.
            const std::string support = to.block_state_at(x, y - 1, z).name;
            if (!block_blocks_motion(support) || support == "minecraft:magma_block")
              ++out.wrong_support;
            const BlockStateKey up = to.block_state_at(x, y + 1, z);
            if (up.name != "minecraft:tall_seagrass" || up.properties != "half=upper")
              ++out.wrong_upper;
          } else if (now.properties == "half=upper") {
            ++out.tall_upper;
            const BlockStateKey down = to.block_state_at(x, y - 1, z);
            if (down.name != "minecraft:tall_seagrass" || down.properties != "half=lower")
              ++out.wrong_upper;
          } else {
            ++out.wrong_upper; // a tall plant with no half at all
          }
        }
    return out;
  };
  {
    const PlantScan sample = scan_plants(before, after);
    CHECK_EQ(sample.wrong_cell, 0);
    CHECK_EQ(sample.wrong_support, 0);
    CHECK_EQ(sample.wrong_upper, 0);

    Chunk river_before;
    fill_generated_chunk(generator, -13, -11, river_before);
    Chunk river_after = river_before;
    WgDecorationStats river_stats;
    decorate_generated_chunk(generator, river_after, &river_stats);
    CHECK(river_stats.applied_by_kind[static_cast<uint32_t>(WgFeatureKind::kSeagrass)] >= 1);
    const PlantScan river = scan_plants(river_before, river_after);
    CHECK(river.plants + river.tall_lower > 0);  // the kind really planted here
    CHECK(river.tall_upper == river.tall_lower); // and wrote the pairs whole
    CHECK_EQ(river.wrong_cell, 0);               // nowhere a cell did not hold water
    CHECK_EQ(river.wrong_support, 0);            // and never on a bed the rule refuses
    CHECK_EQ(river.wrong_upper, 0);
  }

  // ---- the fallen-tree kind is really placed, not only wired ---------------
  // The kind's one placement modifier is a `would_survive` sapling predicate,
  // and it can only pass where the block under the candidate is soil. The chunk
  // this stage is fed has no surface layer (`fill_generated_chunk` runs the
  // noise and biome stages only; the surface stage is its own entry point), so a
  // *generated* chunk never satisfies it -- which is why no fixture chunk
  // carries a fallen tree and none can -- and the rule is pinned on a
  // constructed chunk instead, the way the `snowy` property write above is:
  // every column's top block becomes `minecraft:sand` (in the plant-soil tag)
  // and the chunk's biomes are forced to `minecraft:wooded_badlands`, whose own
  // `trees_badlands` placement already guarantees a sapling spot and lists
  // `minecraft:fallen_oak_tree` among its selector children. The terrain, the
  // heightmaps, the placement draws and the decoration stream are the real
  // chunk's; only the top layer and the biome answer are constructed.
  //
  // The invariants are the rule's own: a fallen log is a straight contiguous
  // run along one axis at one height, with a same-species `axis=y` stump two to
  // three cells back along that axis and no more than two unsupported cells
  // under it; a mushroom sits directly above a fallen log; and a `trunk_vine`
  // vine lies beside the stump.
  struct FallScan {
    int sideways = 0;     // cells holding a log with a horizontal axis
    int runs = 0;         // contiguous runs of them
    int bad_run = 0;      // a run whose cells are not one straight log run
    int bad_stump = 0;    // a run with no `axis=y` stump 2-3 cells behind it
    int unsupported = 0;  // a run over more than two unsupported cells
    int mushrooms = 0;    // new mushroom cells (the log decorator's writes)
    int bad_mushroom = 0; // a new mushroom not above a fallen log
    int vines = 0;        // new vine cells (the stump decorator's writes)
    int bad_vine = 0;     // a new vine not beside a stump log
  };
  auto scan_falls = [](const Chunk& from, const Chunk& to) {
    FallScan out;
    const auto is_log = [](const BlockStateKey& k) {
      return k.name.size() > 4 && k.name.substr(k.name.size() - 4) == "_log";
    };
    const auto sideways = [&](const BlockStateKey& k) {
      return is_log(k) && (k.properties.find("axis=x") != std::string::npos ||
                           k.properties.find("axis=z") != std::string::npos);
    };
    std::set<std::tuple<int, int, int>> seen;
    for (int x = 0; x < 16; ++x)
      for (int z = 0; z < 16; ++z)
        for (int y = kWgMinY; y < kWgMinY + kWgHeight; ++y) {
          const BlockStateKey here = to.block_state_at(x, y, z);
          if (!sideways(here) || seen.count({x, y, z}) != 0)
            continue;
          const bool along_x = here.properties.find("axis=x") != std::string::npos;
          const int dx = along_x ? 1 : 0;
          const int dz = along_x ? 0 : 1;
          int sx = x;
          int sz = z;
          while (sideways(to.block_state_at(sx - dx, y, sz - dz)) &&
                 to.block_state_at(sx - dx, y, sz - dz).name == here.name) {
            sx -= dx;
            sz -= dz;
          }
          int run_length = 0;
          int gaps = 0;
          bool bad = false;
          for (int i = 0;; ++i) {
            const int cx2 = sx + dx * i;
            const int cz2 = sz + dz * i;
            if (cx2 < 0 || cx2 >= 16 || cz2 < 0 || cz2 >= 16)
              break;
            const BlockStateKey k = to.block_state_at(cx2, y, cz2);
            if (!sideways(k) || k.name != here.name || k.properties != here.properties)
              break;
            seen.insert({cx2, y, cz2});
            ++run_length;
            if (!block_blocks_motion(to.block_state_at(cx2, y - 1, cz2).name) && ++gaps > 2)
              bad = true;
          }
          ++out.runs;
          out.sideways += run_length;
          // The rule's own length bound: the config's provider samples 4..7 and
          // the feature takes two off it, so a run is never longer than five.
          if (run_length > 5)
            ++out.bad_run;
          if (bad)
            ++out.unsupported;
          // The stump: two or three cells off *either* end of the run along its
          // own axis, the same log with a vertical axis. Which end depends on
          // the fall's direction (the origin is behind the first log written),
          // and the rule's own `2 + nextInt(2)` is what makes the distance two
          // or three.
          int stumps = 0;
          for (int end = 0; end < 2; ++end) {
            const int ex = end == 0 ? sx : sx + dx * (run_length - 1);
            const int ez = end == 0 ? sz : sz + dz * (run_length - 1);
            for (int step = 2; step <= 3; ++step) {
              const int bx = ex + (end == 0 ? -dx * step : dx * step);
              const int bz = ez + (end == 0 ? -dz * step : dz * step);
              if (bx < 0 || bx >= 16 || bz < 0 || bz >= 16)
                continue;
              const BlockStateKey k = to.block_state_at(bx, y, bz);
              if (is_log(k) && k.name == here.name &&
                  k.properties.find("axis=y") != std::string::npos)
                ++stumps;
            }
          }
          if (stumps == 0)
            ++out.bad_stump;
        }
    // The decorators: a mushroom directly above a fallen log, a vine beside a
    // stump log (a new cell, so the chunk's own flora is not counted).
    for (int x = 0; x < 16; ++x)
      for (int z = 0; z < 16; ++z)
        for (int y = kWgMinY; y < kWgMinY + kWgHeight; ++y) {
          const BlockStateKey now = to.block_state_at(x, y, z);
          const std::string was = from.block_state_at(x, y, z).name;
          if (now.name == "minecraft:red_mushroom" || now.name == "minecraft:brown_mushroom") {
            if (was == now.name)
              continue;
            ++out.mushrooms;
            if (!sideways(to.block_state_at(x, y - 1, z)))
              ++out.bad_mushroom;
          }
          if (now.name == "minecraft:vine" && was != "minecraft:vine") {
            ++out.vines;
            bool beside_stump = false;
            for (const auto& step : {std::pair<int, int>{-1, 0}, {1, 0}, {0, -1}, {0, 1}}) {
              const int vx = x + step.first;
              const int vz = z + step.second;
              if (vx < 0 || vx >= 16 || vz < 0 || vz >= 16)
                continue;
              const BlockStateKey k = to.block_state_at(vx, y, vz);
              if (is_log(k) && k.properties.find("axis=y") != std::string::npos)
                beside_stump = true;
            }
            if (!beside_stump)
              ++out.bad_vine;
          }
        }
    return out;
  };
  {
    // The chunk the probe found: at seed 42, a constructed `wooded_badlands`
    // chunk here plants a fallen oak run with a mushroom above one log and
    // vines beside its stump.
    const int cx = -25;
    const int cz = -5;
    Chunk constructed;
    fill_generated_chunk(generator, cx, cz, constructed);
    for (int lx = 0; lx < 16; ++lx)
      for (int lz = 0; lz < 16; ++lz) {
        int top = -1;
        for (int y = kWgMinY + kWgHeight - 1; y >= kWgMinY; --y)
          if (block_blocks_motion(constructed.block_state_at(lx, y, lz).name)) {
            top = y;
            break;
          }
        if (top >= 0)
          constructed.set_block_state(lx, top, lz, BlockStateKey{"minecraft:sand", ""});
      }
    for (Section& section : constructed.sections) {
      if (!section.has_biomes)
        continue;
      section.biome_palette.assign(1, "minecraft:wooded_badlands");
      section.biome_cells.assign(mc::world::kBiomeCellsPerSection, 0);
    }
    const Chunk constructed_before = constructed;
    WgDecorationStats constructed_stats;
    decorate_generated_chunk(generator, constructed, &constructed_stats);
    const FallScan falls = scan_falls(constructed_before, constructed);
    CHECK(falls.sideways >= 1); // the kind really planted here
    CHECK(falls.runs >= 1);     // with a stump behind each fall
    // Both decorators really ran, not only without violating their rules: this
    // is what a mutation that drops either one has to break.
    CHECK(falls.mushrooms >= 1);
    CHECK(falls.vines >= 1);
    CHECK_EQ(falls.bad_run, 0);
    CHECK_EQ(falls.unsupported, 0);
    CHECK_EQ(falls.bad_stump, 0);
    CHECK_EQ(falls.bad_mushroom, 0);
    CHECK_EQ(falls.bad_vine, 0);
    // And the same chunk without the construction carries no fallen tree: the
    // kind is gated on the soil the noise+biome chunk does not have.
    Chunk generated_biome;
    fill_generated_chunk(generator, cx, cz, generated_biome);
    for (Section& section : generated_biome.sections) {
      if (!section.has_biomes)
        continue;
      section.biome_palette.assign(1, "minecraft:wooded_badlands");
      section.biome_cells.assign(mc::world::kBiomeCellsPerSection, 0);
    }
    const Chunk generated_biome_before = generated_biome;
    decorate_generated_chunk(generator, generated_biome, nullptr);
    const FallScan bare = scan_falls(generated_biome_before, generated_biome);
    CHECK_EQ(bare.sideways, 0);
  }

  // Determinism: a fresh fill and decorate of the same chunk is identical.
  Chunk again;
  fill_generated_chunk(generator, 0, 0, again);
  decorate_generated_chunk(generator, again, nullptr);
  size_t differences = 0;
  for (size_t si = 0; si < after.sections.size(); ++si)
    for (int cell = 0; cell < kBlockCellsPerSection; ++cell)
      if (cell_name(after, si, cell) != cell_name(again, si, cell))
        ++differences;
  CHECK_EQ(differences, 0);

  // ---- the room is really built, not merely dispatched --------------------
  // `MonsterRoomFeature` needs a solid floor and ceiling across a 9x9x6 box
  // *and* one to five holes in its walls (air at the wall line with air above).
  // This test decorates the *noise* column (`fill_generated_chunk`), which has
  // no cave pass — the carvers run in the served bridge
  // (`fill_generated_carved_chunk`), not here — so rooms are rare on it: at
  // seed 42 they land in chunks (-62,39) and (-56,32), and *not* in (0,0).
  // Those two are pinned below, which is what makes the kind's *effect*
  // testable and not only its dispatch. If the decoration stream moves, this is
  // the check that says so.
  size_t rooms_seen = 0;
  for (const auto& chunk_at : {std::pair<int, int>{-62, 39}, std::pair<int, int>{-56, 32}}) {
    Chunk room_chunk;
    fill_generated_chunk(generator, chunk_at.first, chunk_at.second, room_chunk);
    // None of the room's blocks is in the noise or surface stage's output, so
    // everything scanned below is the feature stage's own work.
    const RoomScan bare = scan_room(room_chunk);
    CHECK_EQ(bare.spawners, 0);
    CHECK_EQ(bare.cobble + bare.mossy, 0);
    const Chunk room_before = room_chunk;
    WgDecorationStats room_stats;
    decorate_generated_chunk(generator, room_chunk, &room_stats);
    CHECK(room_stats.applied_by_kind[static_cast<uint32_t>(WgFeatureKind::kMonsterRoom)] >= 1);
    const RoomScan room = scan_room(room_chunk);
    CHECK_EQ(room.spawners, 1);
    CHECK(room.cobble >= 50);
    CHECK(room.mossy >= 20);
    CHECK(room.cave_air >= 50); // the carved interior, not a solid box
    CHECK_EQ(room.outside_footprint, 0);
    size_t room_changed = 0;
    size_t room_unexpected = 0;
    count_changes(room_before, room_chunk, room_changed, room_unexpected);
    CHECK(room_changed > 0);
    CHECK_EQ(room_unexpected, 0); // every new state is one an applied kind names
    rooms_seen += room.spawners == 1 ? 1 : 0;
  }
  CHECK_EQ(rooms_seen, 2);

  // ---- the freeze-top-layer kind is really placed, not only dispatched -----
  // The four chunks above are warm, where the kind is dispatched and writes
  // nothing at all (temperature >= 0.15 at every column top), so the effect
  // needs cold chunks. Two are pinned in the golden dump for exactly this:
  //
  //   * (199,432) is entirely `minecraft:snowy_plains` and the surface stage
  //     leaves exposed stone there: a snow LAYER lands on all 256 columns; and
  //   * (201,442) is `minecraft:frozen_ocean`: each column's surface water
  //     becomes ICE instead, and no snow layer follows it, because ice is in
  //     `#minecraft:cannot_support_snow_layer`.
  //
  // Neither chunk has a snow layer or ice in the surface stage's own output, so
  // every cell counted here is this kind's work. The `snowy` property branch is
  // *not* exercised by these two (their supports are stone and ice); it is
  // pinned separately, just below.
  struct ColdCase {
    int cx;
    int cz;
    int columns;
  };
  for (const ColdCase& c : {ColdCase{199, 432, 256}, ColdCase{201, 442, 256}}) {
    Chunk cold;
    fill_generated_chunk(generator, c.cx, c.cz, cold);
    CHECK_EQ(count_named(cold, "minecraft:snow"), 0);
    CHECK_EQ(count_named(cold, "minecraft:ice"), 0);
    const Chunk cold_before = cold;
    WgDecorationStats cold_stats;
    decorate_generated_chunk(generator, cold, &cold_stats);
    CHECK(cold_stats.applied_by_kind[static_cast<uint32_t>(WgFeatureKind::kFreezeTopLayer)] >= 1);
    const int snow = count_named(cold, "minecraft:snow");
    const int ice = count_named(cold, "minecraft:ice");
    CHECK_EQ(snow + ice, c.columns); // one write on every column, and only one
    CHECK(snow == 0 || ice == 0);    // the two branches are mutually exclusive

    // The per-cell invariants the rule implies: a new snow layer sits where air
    // was and on a support that blocks motion; new ice sits where water was.
    int wrong_snow = 0;
    int wrong_ice = 0;
    for (int x = 0; x < 16; ++x)
      for (int z = 0; z < 16; ++z)
        for (int y = kWgMinY; y < kWgMinY + kWgHeight; ++y) {
          const std::string now = cold.block_state_at(x, y, z).name;
          if (now == "minecraft:snow") {
            const std::string was = cold_before.block_state_at(x, y, z).name;
            if (was != "minecraft:air" && was != "minecraft:cave_air")
              ++wrong_snow;
            if (!block_blocks_motion(cold.block_state_at(x, y - 1, z).name))
              ++wrong_snow;
          } else if (now == "minecraft:ice") {
            if (cold_before.block_state_at(x, y, z).name != "minecraft:water")
              ++wrong_ice;
          }
        }
    CHECK_EQ(wrong_snow, 0);
    CHECK_EQ(wrong_ice, 0);
  }

  // ---- the `snowy` property write, which generated terrain never reaches ---
  // `SnowAndFreezeFeature` ends by setting `snowy=true` on the block a new snow
  // layer lands on when that block carries the property (`SnowyBlock`: grass
  // block, podzol, mycelium). No chunk this port generates reaches that line:
  // the surface stage is pinned byte-for-byte against the real server and gives
  // a snowing biome's floor as snow block, stone, gravel or water, never a
  // SnowyBlock. That is measured, not assumed — a `minecraft:snowy_taiga` sweep
  // over 12 chunks (2,608 of 3,072 columns biome-checked as snowy_taiga) found
  // `grass_tops == 0`, and the two pinned cold chunks' supports are exposed
  // stone and ice.
  //
  // The line is still this kind's, so it is pinned here against one
  // *constructed* column instead of a generated one: the real `snowy_plains`
  // chunk, decorated after its support cell is replaced with the
  // property-bearing `minecraft:grass_block[snowy=false]` that the surface
  // stage never leaves there. Only that one cell is synthetic, so the biome, the
  // heightmap, the temperature answer and the rule's every other input are the
  // real chunk's.
  {
    Chunk constructed;
    fill_generated_chunk(generator, 199, 432, constructed);
    const int cx = 0;
    const int cz = 0;
    const int top = chunk_motion_blocking_height(constructed, cx, cz);
    const int below = top - 1;
    CHECK(constructed.block_state_at(cx, top, cz).name == "minecraft:air");
    CHECK(block_blocks_motion(constructed.block_state_at(cx, below, cz).name));
    // The point of the construction: the generated column does not already
    // carry the property, so a `snowy=true` result can only be this kind's.
    CHECK(constructed.block_state_at(cx, below, cz).name != "minecraft:grass_block");
    CHECK(constructed.set_block_state(cx, below, cz,
                                      BlockStateKey{"minecraft:grass_block", "snowy=false"}));
    CHECK(constructed.block_state_at(cx, below, cz).properties == "snowy=false");
    const Chunk constructed_before = constructed;

    WgDecorationStats constructed_stats;
    decorate_generated_chunk(generator, constructed, &constructed_stats);
    CHECK(
        constructed_stats.applied_by_kind[static_cast<uint32_t>(WgFeatureKind::kFreezeTopLayer)] >=
        1);
    // The layer lands on the constructed column...
    CHECK(constructed.block_state_at(cx, top, cz).name == "minecraft:snow");
    CHECK(constructed.block_state_at(cx, top, cz).properties == "layers=1");
    // ...and the support it landed on is flipped, not rewritten as a new state.
    CHECK(constructed.block_state_at(cx, below, cz).name == "minecraft:grass_block");
    CHECK(constructed.block_state_at(cx, below, cz).properties == "snowy=true");
    // Nothing else in the column moved: the write is a property change only.
    CHECK(constructed.block_state_at(cx, below - 1, cz).name ==
          constructed_before.block_state_at(cx, below - 1, cz).name);
  }

  // ---- the multiface-growth kind is really placed, not only dispatched -----
  // The stage decorates a noise+biome chunk: the carvers are the served
  // bridge's job, so the chunk has no underground air at all, and glow
  // lichen's `surface_relative_threshold_filter` (at most 13 below the ocean
  // floor) keeps it out of the sky. The generated chunks therefore *dispatch*
  // the kind and write nothing — the applied counter moves, the cells do not,
  // which is exactly the failure a dispatch-only check cannot see. So the
  // effect is pinned on a constructed chunk: every second cell below the
  // surface becomes air, giving each candidate a cell with six motion-blocking
  // neighbours to grow on while keeping the chunk's biome, its surface
  // heightmap and the placement stream the real one's.
  {
    Chunk growth;
    fill_generated_chunk(generator, 0, 0, growth);
    int hollowed = 0;
    for (int x = 0; x < 16; ++x) {
      for (int z = 0; z < 16; ++z) {
        const int top = chunk_motion_blocking_height(growth, x, z);
        for (int y = kWgMinY; y < top - 14; ++y) {
          if (((x + y + z) & 1) != 0)
            continue;
          if (growth.set_block_state(x, y, z, BlockStateKey{"minecraft:air", ""}))
            ++hollowed;
        }
      }
    }
    CHECK(hollowed > 10000);
    WgDecorationStats growth_stats;
    decorate_generated_chunk(generator, growth, &growth_stats);
    CHECK(growth_stats.applied_by_kind[static_cast<uint32_t>(WgFeatureKind::kMultifaceGrowth)] >=
          1);

    // Every glow lichen cell: a complete block state (the filled defaults are
    // what makes it resolvable at all), at least one face set, the face's
    // neighbour a full block, no waterlog (every cell the growth reached was
    // made air), and inside the filter's window below the ocean floor.
    int lichen = 0;
    int bad_face = 0;
    int bad_support = 0;
    int bad_window = 0;
    int incomplete = 0;
    int waterlogged = 0;
    int multi_face = 0;
    static constexpr const char* kFaces[6] = {"down", "up", "north", "south", "west", "east"};
    static constexpr int kDx[6] = {0, 0, 0, 0, -1, 1};
    static constexpr int kDy[6] = {-1, 1, 0, 0, 0, 0};
    static constexpr int kDz[6] = {0, 0, -1, 1, 0, 0};
    for (int x = 0; x < 16; ++x) {
      for (int z = 0; z < 16; ++z) {
        for (int y = kWgMinY; y < kWgMinY + kWgHeight; ++y) {
          const BlockStateKey state = growth.block_state_at(x, y, z);
          if (state.name != "minecraft:glow_lichen")
            continue;
          ++lichen;
          int faces = 0;
          for (int f = 0; f < 6; ++f) {
            const std::string face = block_property(state.properties, kFaces[f]);
            if (face.empty())
              ++incomplete;
            if (face != "true")
              continue;
            ++faces;
            if (!block_blocks_motion(
                    growth.block_state_at(x + kDx[f], y + kDy[f], z + kDz[f]).name))
              ++bad_support;
          }
          if (faces == 0)
            ++bad_face;
          if (faces >= 2)
            ++multi_face;
          if (block_property(state.properties, "waterlogged") != "false")
            ++waterlogged;
          // `OCEAN_FLOOR_WG` answered from the cells, the way the filter does:
          // the first free y above the highest motion-blocking block.
          int surface = kWgMinY;
          for (int sy = kWgMinY + kWgHeight - 1; sy >= kWgMinY; --sy)
            if (block_blocks_motion(growth.block_state_at(x, sy, z).name)) {
              surface = sy + 1;
              break;
            }
          if (y > surface - 13)
            ++bad_window;
        }
      }
    }
    CHECK(lichen > 0);
    CHECK_EQ(bad_face, 0);
    CHECK_EQ(bad_support, 0);
    CHECK_EQ(bad_window, 0);
    CHECK_EQ(incomplete, 0);
    CHECK_EQ(waterlogged, 0);
    // The default spreader's SAME_POSITION branch adds a second face to the
    // cell it just placed; with ~130 applications and a 0.5 chance it fires.
    CHECK(multi_face > 0);

    // And the whole decorated column still encodes: every state a feature wrote
    // resolves against the block-state table. Before the canonical-property
    // fix the partial `up=true` a vine wrote aborted the body and the server
    // sent a void chunk instead.
    std::vector<std::string> biome_names;
    for (const Section& s : growth.sections)
      for (const std::string& name : s.biome_palette)
        if (std::find(biome_names.begin(), biome_names.end(), name) == biome_names.end())
          biome_names.push_back(name);
    NameListBiomeTable biomes(biome_names);
    CHECK(mc::world::encode_chunk_body(growth, mc::world::full_block_states(), biomes).has_value());
  }

  // ---- the ice-spike kind is really placed, not only dispatched ------------
  // No seed-42 fixture chunk lies in the `ice_spikes` biome, so the kind is
  // pinned on a constructed chunk: a generated chunk with every biome cell
  // rewritten to `minecraft:ice_spikes` and every column's top motion-blocking
  // block replaced with `snow_block` (the placement's `can_place_on`). The
  // biome's own feature list — the spike's `count 3` placement included — is
  // what the stage then walks, while the heightmap, the surfaces and the whole
  // placement stream stay the generated chunk's.
  {
    Chunk spike;
    fill_generated_chunk(generator, 0, 0, spike);
    for (Section& section : spike.sections) {
      section.has_biomes = true;
      section.biome_palette.assign(1, "minecraft:ice_spikes");
      section.biome_cells.assign(kBiomeCellsPerSection, 0);
    }
    for (int x = 0; x < 16; ++x)
      for (int z = 0; z < 16; ++z)
        spike.set_block_state(x, chunk_motion_blocking_height(spike, x, z) - 1, z,
                              BlockStateKey{"minecraft:snow_block", ""});
    const Chunk spike_before = spike;

    WgDecorationStats spike_stats;
    decorate_generated_chunk(generator, spike, &spike_stats);
    CHECK(spike_stats.applied_by_kind[static_cast<uint32_t>(WgFeatureKind::kSpike)] >= 1);

    // Every packed-ice cell is the kind's own write and replaced air or a block
    // in its `can_replace` tag. Both halves matter: the body (the shrunken
    // ellipses) lands in air, and the trunk walks down through the tag until it
    // meets something it may not replace.
    const WgConfiguredFeature* spike_cf = nullptr;
    for (const WgConfiguredFeature& cf : kWgConfiguredFeatures)
      if (cf.kind == WgFeatureKind::kSpike)
        spike_cf = &cf;
    CHECK(spike_cf != nullptr);
    const char* replaceable = spike_cf != nullptr && spike_cf->config < std::size(kWgExtraConfigs)
                                  ? kWgExtraConfigs[spike_cf->config].ref2
                                  : "";
    int packed = 0;
    int air_pre = 0;
    int bad_pre = 0;
    int deepest = kWgMinY + kWgHeight;
    for (int x = 0; x < 16; ++x)
      for (int z = 0; z < 16; ++z)
        for (int y = kWgMinY; y < kWgMinY + kWgHeight; ++y) {
          if (spike.block_state_at(x, y, z).name != "minecraft:packed_ice")
            continue;
          ++packed;
          deepest = std::min(deepest, y);
          const BlockStateKey was = spike_before.block_state_at(x, y, z);
          if (was.name == "minecraft:air")
            ++air_pre;
          else if (!name_in_pipe_list(replaceable, was.name))
            ++bad_pre;
        }
    CHECK(packed > 20);
    CHECK_EQ(bad_pre, 0);
    CHECK(air_pre > 0);
    CHECK(deepest > 50); // the trunk's own hard floor (`SpikeFeature`'s `y > 50`)
  }

  // ---- the geode kind is really placed, not only dispatched ----------------
  // `amethyst_geode` is a 1-in-24 placement, and no single constructed chunk can
  // be relied on to draw the hit. The base is therefore one generated chunk
  // (with its biome rewritten to `ice_spikes`, whose step 2 lists the geode)
  // copied to successive chunk coordinates — the copy keeps the stone column,
  // the biome and the heightmaps, and only the coordinate the stream is keyed on
  // moves — until the rarity draw passes. Decoration is the only cost.
  {
    Chunk base;
    fill_generated_chunk(generator, 0, 0, base);
    for (Section& section : base.sections) {
      section.has_biomes = true;
      section.biome_palette.assign(1, "minecraft:ice_spikes");
      section.biome_cells.assign(kBiomeCellsPerSection, 0);
    }

    // The first geode the rarity draw reaches at (53,0) is clipped by the
    // column edge, so its shell's neighbour ratios are not the rule's own.
    // Keep looking for one whose amethyst/calcite bands are still mostly
    // adjacent; the tally below then pins that same, unclipped shell.
    auto layered_geode = [](const Chunk& candidate) {
      static constexpr int kDx[6] = {0, 0, 0, 0, -1, 1};
      static constexpr int kDy[6] = {-1, 1, 0, 0, 0, 0};
      static constexpr int kDz[6] = {0, 0, -1, 1, 0, 0};
      auto near = [&](int x, int y, int z, const char* name) {
        for (int d = 0; d < 6; ++d) {
          const int px = x + kDx[d], py = y + kDy[d], pz = z + kDz[d];
          if (px < 0 || px >= 16 || pz < 0 || pz >= 16)
            continue;
          if (candidate.block_state_at(px, py, pz).name == name)
            return true;
        }
        return false;
      };
      int amethyst = 0;
      int calcite = 0;
      int amethyst_with_calcite = 0;
      int calcite_with_basalt = 0;
      for (int x = 0; x < 16; ++x)
        for (int z = 0; z < 16; ++z)
          for (int y = kWgMinY; y < kWgMinY + kWgHeight; ++y) {
            const std::string now = candidate.block_state_at(x, y, z).name;
            if (now == "minecraft:amethyst_block") {
              ++amethyst;
              amethyst_with_calcite += near(x, y, z, "minecraft:calcite") ? 1 : 0;
            } else if (now == "minecraft:calcite") {
              ++calcite;
              calcite_with_basalt += near(x, y, z, "minecraft:smooth_basalt") ? 1 : 0;
            }
          }
      return amethyst > 0 && calcite > 0 && amethyst_with_calcite * 2 >= amethyst &&
             calcite_with_basalt * 3 >= calcite * 2;
    };
    Chunk geode;
    WgDecorationStats geode_stats;
    bool found = false;
    for (int cz = 0; cz < 8 && !found; ++cz) {
      for (int cx = 0; cx < 64 && !found; ++cx) {
        Chunk candidate = base;
        candidate.x = cx;
        candidate.z = cz;
        WgDecorationStats candidate_stats;
        decorate_generated_chunk(generator, candidate, &candidate_stats);
        if (candidate_stats.applied_by_kind[static_cast<uint32_t>(WgFeatureKind::kGeode)] > 0 &&
            count_named(candidate, "minecraft:smooth_basalt") > 0 && layered_geode(candidate)) {
          geode = std::move(candidate);
          geode_stats = candidate_stats;
          found = true;
        }
      }
    }
    CHECK(found);
    if (found) {
      CHECK(geode_stats.blocks_placed > 0);

      // The five materials the config names are all present, and each one sits
      // in a cell the undecorated column held solid rock (the geode replaces
      // its host; it never writes into an air or an already-invalid cell).
      const char* kGeodeStates[] = {"minecraft:smooth_basalt", "minecraft:calcite",
                                    "minecraft:amethyst_block", "minecraft:budding_amethyst"};
      int counts[4] = {0, 0, 0, 0};
      int hollow = 0;
      for (int x = 0; x < 16; ++x)
        for (int z = 0; z < 16; ++z)
          for (int y = kWgMinY; y < kWgMinY + kWgHeight; ++y) {
            const std::string now = geode.block_state_at(x, y, z).name;
            const std::string was = base.block_state_at(x, y, z).name;
            if (now == "minecraft:air" && was != "minecraft:air")
              ++hollow;
            for (int i = 0; i < 4; ++i)
              if (now == kGeodeStates[i])
                ++counts[i];
          }
      CHECK(counts[0] > 100); // smooth basalt shell
      CHECK(counts[1] > 100); // calcite
      CHECK(counts[2] > 50);  // amethyst block
      CHECK(counts[3] >= 0);  // budding amethyst is a chance, not a guarantee
      CHECK(hollow > 100);    // the filling provider's air core

      // The exact tally, not just its shape: this is the guard against the
      // geode's noise reading outside its amplitude list. `place_geode` builds
      // the list in a local array and samples the noise after that scope ends;
      // while `WgPerlinNoise` borrowed the pointer, gcc/libstdc++ read whatever
      // took the stack slot next and placed the shell a cell over, so these
      // counts differed between compilers (docs/PHASE5.md's platform check).
      // The noise owns its amplitudes now, so the shell is the same on every
      // compiler; this tally is the local witness, and CI runs this test under
      // both gcc and clang. A deliberate change to the geode's placement moves
      // it, and re-recording is then the same explicit edit a fixture needs.
      CHECK_EQ(counts[0], 143);
      CHECK_EQ(counts[1], 103);
      CHECK_EQ(counts[2], 82);
      CHECK_EQ(counts[3], 9);
      CHECK_EQ(hollow, 110);

      // The layers are ordered: an amethyst cell is bounded by calcite on the
      // outside (the two bands are adjacent in the shell sum), and a calcite
      // cell by smooth basalt. This is a majority property, not a universal one:
      // a geode is wider than a 16-block column and this stage is confined to the
      // column it decorates (the header's own approximation), so the rim's
      // neighbours are dropped on every side, and a crack removes more.
      static constexpr int kDx[6] = {0, 0, 0, 0, -1, 1};
      static constexpr int kDy[6] = {-1, 1, 0, 0, 0, 0};
      static constexpr int kDz[6] = {0, 0, -1, 1, 0, 0};
      auto has_neighbour = [&](int x, int y, int z, const char* name) {
        for (int d = 0; d < 6; ++d) {
          const int px = x + kDx[d], py = y + kDy[d], pz = z + kDz[d];
          if (px < 0 || px >= 16 || pz < 0 || pz >= 16)
            continue;
          if (geode.block_state_at(px, py, pz).name == name)
            return true;
        }
        return false;
      };
      int amethyst_with_calcite = 0;
      int calcite_with_basalt = 0;
      int amethyst = 0;
      int calcite = 0;
      for (int x = 0; x < 16; ++x)
        for (int z = 0; z < 16; ++z)
          for (int y = kWgMinY; y < kWgMinY + kWgHeight; ++y) {
            const std::string now = geode.block_state_at(x, y, z).name;
            if (now == "minecraft:amethyst_block") {
              ++amethyst;
              amethyst_with_calcite += has_neighbour(x, y, z, "minecraft:calcite") ? 1 : 0;
            } else if (now == "minecraft:calcite") {
              ++calcite;
              calcite_with_basalt += has_neighbour(x, y, z, "minecraft:smooth_basalt") ? 1 : 0;
            }
          }
      CHECK(amethyst > 0 && amethyst_with_calcite * 2 >= amethyst);
      CHECK(calcite > 0 && calcite_with_basalt * 3 >= calcite * 2);

      // Determinism: the same coordinate decorates to the same cells.
      Chunk again = base;
      again.x = geode.x;
      again.z = geode.z;
      decorate_generated_chunk(generator, again, nullptr);
      int mismatch = 0;
      for (int x = 0; x < 16; ++x)
        for (int z = 0; z < 16; ++z)
          for (int y = kWgMinY; y < kWgMinY + kWgHeight; ++y)
            if (again.block_state_at(x, y, z).name != geode.block_state_at(x, y, z).name ||
                again.block_state_at(x, y, z).properties !=
                    geode.block_state_at(x, y, z).properties)
              ++mismatch;
      CHECK_EQ(mismatch, 0);
    }
  }

  // ---- fossils -----------------------------------------------------------
  test_fossils();
  // ---- structure placement ----------------------------------------------
  test_structure_placements();
  test_structure_exclusion();
  test_structure_biomes_and_rings();
  // ---- the first structure pieces -----------------------------------------
  test_shipwreck_pieces();

  // ---- the served/dimension stream split --------------------------------
  test_decoration_stream_selection();

  // ---- concurrent decoration regions --------------------------------------
  test_concurrent_decoration_regions();
  test_concurrent_decorated_columns();

  // ---- the golden decoration dump -----------------------------------------
  // Every chunk in `feature_meta.tsv` is re-decorated here and compared cell for
  // cell, with the run's own ledger beside it. This is the pin that a decoration
  // change cannot move without failing the build.
  test_golden_decoration();

  // ---- the generated canopy's leaf distances ------------------------------
  // The rule itself, replayed over a generated square: the settle is correct
  // when every leaf the rule can anchor sits at the distance it gives it.
  test_generated_canopy_distances();

  if (failures != 0) {
    std::fprintf(stderr, "%d/%d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("worldgen-feature: %d checks passed (%zu changed cells, %zu monster rooms)\n", checks,
              changed, rooms_seen);
  return 0;
}
