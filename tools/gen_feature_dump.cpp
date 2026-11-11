// SPDX-License-Identifier: GPL-3.0-or-later
//
// Writes the feature (decoration) stage's golden output for a small set of
// chunks: `feature_meta.tsv` plus one gzipped cell file per chunk,
// `feature_seed<seed>_c<cx>_<cz>.bin.gz`.
//
// This is **not** a vanilla dump, and that is the point of the tool existing as
// C++ rather than as one of `tools/gen_worldgen_dump.py`'s probes. The
// decoration random sequence is this port's own (`world/worldgen_feature.h`
// names that approximation), so there is nothing in the game jar to compare
// against: what this writes is a regression pin of *this* runtime's output, so
// a change to the decoration stream cannot land silently. The vanilla
// comparison fixtures beside it (`noise_*.bin.gz`, `surface_*.bin.gz`) stay the
// jar's.
//
// The cell files use the same shape as the surface fixture: a per-chunk code
// book assigned in first-appearance order (so it decodes only its own file, and
// the reader compares canonical state names rather than numbers), cells packed
// little-endian in the surface fixture's own order
// `i = x * (height * 16) + z * height + y` over `minY .. minY + height - 1`.
// The codes are two bytes wide here rather than the surface stage's one: a
// decorated chunk carries the whole block palette (every ore variant, leaves,
// logs, ...), not a dozen terrain states.
//
// Usage:
//   plusmc-feature-dump <fixtures_dir> [seed] [--diff]
//
// `--diff` writes nothing: it re-decorates every chunk in memory, reads what is
// checked in, and prints a patch of the difference — one `-`/`+` pair per moved
// cell with its coordinate and both states, then the palette and the ledger
// columns. It exits 1 when the fixture differs, so the review is
//
//   ./build/plusmc-feature-dump <fixtures_dir> --diff   # read the patch
//   ./build/plusmc-feature-dump <fixtures_dir>          # then rewrite it
//   ctest -R worldgen-feature                           # and re-pin it
//
// A decoration change that is *intended* needs both steps; the patch is what a
// reviewer reads, and `ctest` alone would only say "some cells moved".
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <zlib.h>

#include "fixture_diff.h"
#include "world/chunk.h"
#include "world/generated_chunk.h"
#include "world/worldgen_chunk.h"
#include "world/worldgen_feature.h"
#include "world/worldgen_feature_table.h"
#include "world/worldgen_table.h"

namespace {

// The chunks the fixture pins. A small, deliberate set rather than a scan: two
// that a monster room builds in at seed 42 ((8,7) and (-13,-11)) and two that
// it does not, including the test's usual sample (0,0) and its neighbour. The
// last two are cold, because `freeze_top_layer` writes nothing in a warm chunk:
// (199,432) is entirely `minecraft:snowy_plains` (a snow layer on every column)
// and (201,442) is `minecraft:frozen_ocean` (a column of surface water frozen
// to ice instead).
constexpr int kChunks[][2] = {{0, 0}, {-1, -1}, {8, 7}, {-13, -11}, {199, 432}, {201, 442}};

// How many differing cells one chunk's patch prints before it elides the rest.
constexpr size_t kMaxCellLines = 24;

const size_t kHeight = static_cast<size_t>(mc::world::kWgHeight);
const size_t kCells = 16 * 16 * kHeight;

// `name[properties]`, the canonical spelling the readers compare.
std::string canonical_state(const mc::world::BlockStateKey& key) {
  std::string text = key.name;
  if (!key.properties.empty()) {
    text += '[';
    text += key.properties;
    text += ']';
  }
  return text;
}

std::string cell_file_name(int64_t seed, int32_t chunk_x, int32_t chunk_z) {
  return "feature_seed" + std::to_string(seed) + "_c" + std::to_string(chunk_x) + "_" +
         std::to_string(chunk_z) + ".bin.gz";
}

bool write_gzip(const std::string& path, const uint8_t* data, size_t size) {
  gzFile file = gzopen(path.c_str(), "wb");
  if (file == nullptr) {
    std::fprintf(stderr, "error: cannot open %s for writing\n", path.c_str());
    return false;
  }
  const int written = gzwrite(file, data, static_cast<unsigned>(size));
  const int closed = gzclose(file);
  if (written != static_cast<int>(size) || closed != Z_OK) {
    std::fprintf(stderr, "error: writing %s failed\n", path.c_str());
    return false;
  }
  return true;
}

// One chunk's worth of output, held in memory so the write path and `--diff`
// take the same decoration pass and can never disagree about it.
struct ChunkDump {
  int32_t chunk_x = 0;
  int32_t chunk_z = 0;
  std::vector<uint16_t> codes;      // one per cell, in the fixture's own order
  std::vector<std::string> palette; // canonical state names, by code
  std::vector<uint8_t> bytes;       // `codes` as little-endian uint16
  mc::world::WgDecorationStats stats;
};

ChunkDump generate_chunk(int64_t seed, int32_t chunk_x, int32_t chunk_z) {
  ChunkDump dump;
  dump.chunk_x = chunk_x;
  dump.chunk_z = chunk_z;

  mc::world::WgOverworldGenerator generator(seed);
  mc::world::Chunk chunk;
  mc::world::fill_generated_chunk(generator, chunk_x, chunk_z, chunk);
  mc::world::decorate_generated_chunk(generator, chunk, &dump.stats);

  // Code the chunk by first appearance, walking the fixture's own order.
  dump.codes.reserve(kCells);
  std::map<std::string, uint16_t> index_of;
  for (size_t x = 0; x < 16; ++x) {
    for (size_t z = 0; z < 16; ++z) {
      for (size_t y = 0; y < kHeight; ++y) {
        const std::string state = canonical_state(chunk.block_state_at(
            static_cast<int>(x), static_cast<int>(y) + mc::world::kWgMinY, static_cast<int>(z)));
        auto it = index_of.find(state);
        uint16_t code = 0;
        if (it == index_of.end()) {
          code = static_cast<uint16_t>(dump.palette.size());
          index_of.emplace(state, code);
          dump.palette.push_back(state);
        } else {
          code = it->second;
        }
        dump.codes.push_back(code);
      }
    }
  }

  dump.bytes.resize(dump.codes.size() * 2);
  for (size_t i = 0; i < dump.codes.size(); ++i) {
    dump.bytes[i * 2] = static_cast<uint8_t>(dump.codes[i] & 0xFFu);
    dump.bytes[i * 2 + 1] = static_cast<uint8_t>(dump.codes[i] >> 8);
  }
  return dump;
}

// ---- the fixture as it is on disk -------------------------------------------

// The ledger columns, in the order the meta writes them (its last seven fields).
constexpr const char* kLedgerColumns[] = {"considered",       "applied",   "skipped_kind",
                                          "skipped_modifier", "positions", "out_of_chunk",
                                          "blocks_placed"};
constexpr size_t kLedgerColumnCount = sizeof(kLedgerColumns) / sizeof(kLedgerColumns[0]);

struct OnDiskChunk {
  bool row = false;                 // `feature_meta.tsv` has a row for this chunk
  bool cells = false;               // its cell file decoded to the expected size
  std::vector<std::string> palette; // canonical state names, by code
  std::vector<long long> ledger;    // parallel to kLedgerColumns
  std::vector<uint16_t> codes;
};

std::vector<std::string> split_tabs(const std::string& line) {
  std::vector<std::string> fields;
  size_t pos = 0;
  while (pos <= line.size()) {
    const size_t tab = line.find('\t', pos);
    if (tab == std::string::npos) {
      fields.push_back(line.substr(pos));
      break;
    }
    fields.push_back(line.substr(pos, tab - pos));
    pos = tab + 1;
  }
  return fields;
}

std::map<std::pair<int32_t, int32_t>, OnDiskChunk> load_on_disk(const std::string& dir,
                                                                int64_t seed) {
  std::map<std::pair<int32_t, int32_t>, OnDiskChunk> out;
  const fixture_diff::Lines meta = fixture_diff::read_lines(dir + "/feature_meta.tsv");
  for (const std::string& line : meta.lines) {
    if (line.empty() || line[0] == '#')
      continue;
    const std::vector<std::string> fields = split_tabs(line);
    if (fields.size() < 8 + kLedgerColumnCount)
      continue;
    if (std::strtoll(fields[0].c_str(), nullptr, 10) != seed)
      continue;

    const int32_t chunk_x = std::atoi(fields[1].c_str());
    const int32_t chunk_z = std::atoi(fields[2].c_str());
    OnDiskChunk chunk;
    chunk.row = true;

    // `stateCodes`: ';'-separated `code=state` in code order, so the entry's own
    // index is the code and only the part after '=' matters.
    size_t pos = 0;
    while (pos <= fields[7].size() && !fields[7].empty()) {
      const size_t semi = fields[7].find(';', pos);
      const size_t end = semi == std::string::npos ? fields[7].size() : semi;
      const std::string entry = fields[7].substr(pos, end - pos);
      const size_t eq = entry.find('=');
      chunk.palette.push_back(eq == std::string::npos ? entry : entry.substr(eq + 1));
      if (semi == std::string::npos)
        break;
      pos = semi + 1;
    }
    for (size_t i = 0; i < kLedgerColumnCount; ++i)
      chunk.ledger.push_back(std::strtoll(fields[8 + i].c_str(), nullptr, 10));

    const std::vector<uint8_t> bytes =
        fixture_diff::read_gzip(dir + "/" + cell_file_name(seed, chunk_x, chunk_z));
    if (bytes.size() == kCells * 2) {
      chunk.codes.reserve(kCells);
      for (size_t i = 0; i < kCells; ++i)
        chunk.codes.push_back(static_cast<uint16_t>(bytes[i * 2] | (bytes[i * 2 + 1] << 8)));
      chunk.cells = true;
    }
    out[{chunk_x, chunk_z}] = chunk;
  }
  return out;
}

std::string code_name(const std::vector<std::string>& palette, uint16_t code) {
  return code < palette.size() ? palette[code] : std::string("<bad-code>");
}

// The coordinate the fixture's walk order puts at cell `i`.
void cell_coords(size_t i, size_t* x, long long* y, size_t* z) {
  *x = i / (kHeight * 16);
  *z = (i / kHeight) % 16;
  *y = static_cast<long long>(i % kHeight) + mc::world::kWgMinY;
}

// ---- the patch --------------------------------------------------------------

// Prints one chunk's patch. Returns how many cells moved; sets `*other_changed`
// when the palette or the ledger did, and `*files_missing` when the chunk's own
// files are absent or unreadable (which is not a cell-by-cell comparison).
size_t diff_chunk(const ChunkDump& dump, const OnDiskChunk& disk, int64_t seed, bool* other_changed,
                  bool* files_missing) {
  const std::string file = cell_file_name(seed, dump.chunk_x, dump.chunk_z);
  std::printf("chunk (%d,%d)\n", dump.chunk_x, dump.chunk_z);
  *other_changed = false;
  *files_missing = false;
  if (!disk.row || !disk.cells) {
    *files_missing = true;
    std::printf("  --- %s (%s)\n", file.c_str(),
                disk.row ? "unreadable or the wrong size" : "no row in feature_meta.tsv");
    std::printf("  +++ %s (regenerated: %zu cells, %zu states)\n", file.c_str(), dump.codes.size(),
                dump.palette.size());
    return 0;
  }

  size_t differing = 0;
  size_t printed = 0;
  std::string patch;
  std::string first_at;
  std::string last_at;
  const size_t cells = std::min(dump.codes.size(), disk.codes.size());
  for (size_t i = 0; i < cells; ++i) {
    const std::string got = code_name(dump.palette, dump.codes[i]);
    const std::string want = code_name(disk.palette, disk.codes[i]);
    if (got == want)
      continue;
    ++differing;
    if (printed >= kMaxCellLines)
      continue;
    size_t x = 0;
    size_t z = 0;
    long long y = 0;
    cell_coords(i, &x, &y, &z);
    char coord[64];
    std::snprintf(coord, sizeof(coord), "(%zu, %lld, %zu)", x, y, z);
    if (printed == 0)
      first_at = coord;
    last_at = coord;
    patch += "  - ";
    patch += coord;
    patch += ": ";
    patch += want;
    patch += "\n  + ";
    patch += coord;
    patch += ": ";
    patch += got;
    patch += '\n';
    ++printed;
  }
  if (differing != 0) {
    std::printf("  --- %s (on disk)\n", file.c_str());
    std::printf("  +++ %s (regenerated)\n", file.c_str());
    std::printf("%s", patch.c_str());
    std::printf("  @@ %zu of %zu cells differ (first %s, last %s) @@\n", differing, cells,
                first_at.c_str(), last_at.c_str());
    if (differing > printed)
      std::printf("  ... %zu more differing cells\n", differing - printed);
  }
  if (dump.codes.size() != disk.codes.size())
    std::printf("  @@ cell count: %zu -> %zu @@\n", disk.codes.size(), dump.codes.size());

  // The code book, as a set difference: a renumbered palette whose states are
  // the same is not a change, which is exactly what comparing cells by *name*
  // above says too.
  std::vector<std::string> added;
  std::vector<std::string> removed;
  {
    std::vector<std::string> before = disk.palette;
    std::vector<std::string> after = dump.palette;
    std::sort(before.begin(), before.end());
    std::sort(after.begin(), after.end());
    std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                        std::back_inserter(added));
    std::set_difference(before.begin(), before.end(), after.begin(), after.end(),
                        std::back_inserter(removed));
  }
  if (!added.empty() || !removed.empty()) {
    std::printf("  @@ palette: %zu -> %zu states @@\n", disk.palette.size(), dump.palette.size());
    for (const std::string& state : removed)
      std::printf("  - %s\n", state.c_str());
    for (const std::string& state : added)
      std::printf("  + %s\n", state.c_str());
  }

  // The ledger, only the columns that moved.
  const long long regenerated[kLedgerColumnCount] = {
      static_cast<long long>(dump.stats.considered),
      static_cast<long long>(dump.stats.applied),
      static_cast<long long>(dump.stats.skipped_kind),
      static_cast<long long>(dump.stats.skipped_modifier),
      static_cast<long long>(dump.stats.positions),
      static_cast<long long>(dump.stats.out_of_chunk),
      static_cast<long long>(dump.stats.blocks_placed)};
  bool ledger_changed = false;
  for (size_t i = 0; i < kLedgerColumnCount; ++i) {
    if (i >= disk.ledger.size() || disk.ledger[i] == regenerated[i])
      continue;
    if (!ledger_changed) {
      std::printf("  @@ ledger @@\n");
      ledger_changed = true;
    }
    std::printf("  - %s: %lld\n", kLedgerColumns[i], disk.ledger[i]);
    std::printf("  + %s: %lld\n", kLedgerColumns[i], regenerated[i]);
  }

  *other_changed = ledger_changed || !added.empty() || !removed.empty();
  if (differing == 0 && !*other_changed)
    std::printf("  in sync: %zu cells, %zu states\n", dump.codes.size(), dump.palette.size());
  return differing;
}

void write_meta_header(std::FILE* meta) {
  std::fprintf(meta, "# Phase 4 milestone K: the feature (decoration) stage's golden output.\n");
  std::fprintf(meta, "#\n");
  std::fprintf(meta, "# NOT a vanilla dump: the decoration random stream is this port's own\n");
  std::fprintf(meta,
               "# (world/worldgen_feature.h names that approximation), so there is nothing\n");
  std::fprintf(meta, "# in the game jar to compare against. This is a regression pin of this\n");
  std::fprintf(meta, "# runtime: `ctest -R worldgen-feature` re-decorates every chunk below and\n");
  std::fprintf(meta,
               "# compares the result cell for cell, so a change to the stream cannot land\n");
  std::fprintf(meta,
               "# silently. Regenerate deliberately with `plusmc-feature-dump <fixtures_dir>`,\n");
  std::fprintf(meta,
               "# reviewing the change first with its `--diff` mode (nothing is written, the\n");
  std::fprintf(meta, "# patch is printed, and the exit status is 1 while the fixture differs).\n");
  std::fprintf(meta, "#\n");
  std::fprintf(meta,
               "# stateCodes: ';'-separated `code=state` entries (a state's own property list\n");
  std::fprintf(meta, "# uses ',' and '=', which is why the entry separator is ';'), assigned in\n");
  std::fprintf(meta, "# first-appearance order, so a code decodes only its own row's file.\n");
  std::fprintf(meta, "# cells: 16*16*height little-endian uint16 codes in the surface fixture's\n");
  std::fprintf(meta,
               "# own order, i = x * (height * 16) + z * height + y over minY..minY+height-1.\n");
  std::fprintf(meta, "# seed\tchunkX\tchunkZ\tminY\theight\tcells\tcellBytes\tstateCodes"
                     "\tconsidered\tapplied\tskipped_kind\tskipped_modifier\tpositions"
                     "\tout_of_chunk\tblocks_placed\n");
}

} // namespace

int main(int argc, char** argv) {
  bool diff = false;
  std::string dir;
  int64_t seed = 42;
  bool have_seed = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--diff") == 0) {
      diff = true;
    } else if (dir.empty()) {
      dir = argv[i];
    } else if (!have_seed) {
      seed = std::strtoll(argv[i], nullptr, 10);
      have_seed = true;
    } else {
      std::fprintf(stderr, "usage: %s <fixtures_dir> [seed] [--diff]\n", argv[0]);
      return 2;
    }
  }
  if (dir.empty()) {
    std::fprintf(stderr, "usage: %s <fixtures_dir> [seed] [--diff]\n", argv[0]);
    return 2;
  }

  std::vector<ChunkDump> dumps;
  dumps.reserve(6);
  for (const auto& coord : kChunks)
    dumps.push_back(generate_chunk(seed, coord[0], coord[1]));
  for (const ChunkDump& dump : dumps) {
    if (dump.palette.size() > 0x10000u) {
      std::fprintf(stderr, "error: chunk (%d,%d) has %zu states, more than a uint16 code\n",
                   dump.chunk_x, dump.chunk_z, dump.palette.size());
      return 1;
    }
  }

  if (diff) {
    const std::map<std::pair<int32_t, int32_t>, OnDiskChunk> disk = load_on_disk(dir, seed);
    std::printf("feature fixture: %zu chunks, %zu cells each\n", dumps.size(), kCells);
    static const OnDiskChunk kMissing;
    size_t moved_chunks = 0;
    size_t moved_cells = 0;
    size_t missing_chunks = 0;
    size_t other_changed_chunks = 0;
    for (const ChunkDump& dump : dumps) {
      const auto it = disk.find({dump.chunk_x, dump.chunk_z});
      bool other_changed = false;
      bool files_missing = false;
      const size_t moved = diff_chunk(dump, it == disk.end() ? kMissing : it->second, seed,
                                      &other_changed, &files_missing);
      if (files_missing)
        ++missing_chunks;
      if (moved != 0)
        ++moved_chunks;
      else if (other_changed && !files_missing)
        ++other_changed_chunks;
      moved_cells += moved;
    }
    if (moved_chunks == 0 && missing_chunks == 0 && other_changed_chunks == 0) {
      std::printf("summary: in sync with this run's output (%zu chunks, %zu cells)\n", dumps.size(),
                  dumps.size() * kCells);
      return 0;
    }
    std::printf("summary: the checked-in fixture is OUT OF SYNC: %zu of %zu chunks differ "
                "(%zu %s moved, %zu missing or unreadable, %zu palette or ledger only)\n",
                moved_chunks + missing_chunks + other_changed_chunks, dumps.size(), moved_cells,
                moved_cells == 1 ? "cell" : "cells", missing_chunks, other_changed_chunks);
    std::printf("re-run without --diff to rewrite it, then `ctest -R worldgen-feature`\n");
    return 1;
  }

  const std::string meta_path = dir + "/feature_meta.tsv";
  std::FILE* meta = std::fopen(meta_path.c_str(), "w");
  if (meta == nullptr) {
    std::fprintf(stderr, "error: cannot write %s\n", meta_path.c_str());
    return 1;
  }
  write_meta_header(meta);
  for (const ChunkDump& dump : dumps) {
    if (!write_gzip(dir + "/" + cell_file_name(seed, dump.chunk_x, dump.chunk_z), dump.bytes.data(),
                    dump.bytes.size())) {
      std::fclose(meta);
      return 1;
    }
    std::string state_codes;
    for (size_t i = 0; i < dump.palette.size(); ++i) {
      if (i != 0)
        state_codes += ';';
      state_codes += std::to_string(i);
      state_codes += '=';
      state_codes += dump.palette[i];
    }
    std::fprintf(meta, "%lld\t%d\t%d\t%d\t%zu\t%zu\t2\t%s\t%u\t%u\t%u\t%u\t%u\t%u\t%u\n",
                 static_cast<long long>(seed), dump.chunk_x, dump.chunk_z, mc::world::kWgMinY,
                 kHeight, kCells, state_codes.c_str(), dump.stats.considered, dump.stats.applied,
                 dump.stats.skipped_kind, dump.stats.skipped_modifier, dump.stats.positions,
                 dump.stats.out_of_chunk, dump.stats.blocks_placed);
    std::printf("wrote %s (%zu cells, %zu states, %u applied, %u blocks)\n",
                cell_file_name(seed, dump.chunk_x, dump.chunk_z).c_str(), kCells,
                dump.palette.size(), dump.stats.applied, dump.stats.blocks_placed);
  }
  std::fclose(meta);
  std::printf("wrote %s\n", meta_path.c_str());
  return 0;
}
