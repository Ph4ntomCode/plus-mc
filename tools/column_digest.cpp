// SPDX-License-Identifier: GPL-3.0-or-later
//
// A decorated column as a digest, so two builds' output can be compared without
// shipping the columns themselves.
//
// The feature stage's random stream is this port's own, so this is not a vanilla
// comparison: it is a pin of *this runtime's* decorated cells. CI runs the tool
// once per compiler (gcc, clang, macOS clang), uploads the three records, and
// `tools/compare_column_digests.py` fails the push if any column's digest
// differs between them. That is the check a same-machine golden cannot make: a
// value that depends on what the stack held, or on an evaluation order the
// standard leaves unspecified, is stable on one compiler and moves on another
// (the geode's borrowed amplitude list did, docs/PHASE5.md).
//
// The grid is the light seam's: seeds 42, 7 and 2026 x a 5x5 chunk grid, so the
// same columns the light records cover are the ones compared here.
//
//   ./build/plusmc-column-digest --json /tmp/columns-gcc.json
//   ./build/plusmc-column-digest --seeds 42 --radius 1 --json /tmp/one.json
//   tools/check_column_digests_linux.sh          the same record under Linux gcc
//
// The digest is FNV-1a 64 over every cell's state (name, properties, then its
// x/y/z as fixed little-endian 32-bit values), in column order bottom-up, z
// then x. Cells are hashed one at a time rather than through the section
// palettes so the digest is a statement about the cells a player would see,
// independent of how the palette happened to pack them.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "world/chunk.h"
#include "world/generated_chunk.h"
#include "world/worldgen_chunk.h"

namespace {

constexpr long long kCellsPerColumn = 16LL * 16 * 384;
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

std::uint64_t hash_bytes(std::uint64_t hash, const void* data, std::size_t size) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= kFnvPrime;
  }
  return hash;
}

// Fixed width and byte order, so the digest does not pick up the host's `int`
// or endianness.
std::uint64_t hash_int(std::uint64_t hash, int value) {
  const std::uint32_t bits = static_cast<std::uint32_t>(value);
  const unsigned char bytes[4] = {
      static_cast<unsigned char>(bits & 0xffU),
      static_cast<unsigned char>((bits >> 8) & 0xffU),
      static_cast<unsigned char>((bits >> 16) & 0xffU),
      static_cast<unsigned char>((bits >> 24) & 0xffU),
  };
  return hash_bytes(hash, bytes, sizeof(bytes));
}

std::vector<long long> parse_seeds(const std::string& text) {
  std::vector<long long> seeds;
  std::string::size_type begin = 0;
  while (begin <= text.size()) {
    const std::string::size_type comma = text.find(',', begin);
    const std::string part =
        text.substr(begin, comma == std::string::npos ? std::string::npos : comma - begin);
    if (!part.empty()) {
      char* end = nullptr;
      const long long value = std::strtoll(part.c_str(), &end, 10);
      if (end == part.c_str() || *end != '\0') {
        std::fprintf(stderr, "--seeds takes comma-separated integers (got %s)\n", part.c_str());
        return {};
      }
      seeds.push_back(value);
    }
    if (comma == std::string::npos)
      break;
    begin = comma + 1;
  }
  if (seeds.empty())
    std::fprintf(stderr, "--seeds names no seed\n");
  return seeds;
}

struct Column {
  long long seed = 0;
  int cx = 0;
  int cz = 0;
  std::uint64_t digest = 0;
};

std::vector<std::pair<int, int>> grid_chunks(int radius) {
  std::vector<std::pair<int, int>> chunks;
  chunks.reserve(static_cast<std::size_t>(2 * radius + 1) *
                 static_cast<std::size_t>(2 * radius + 1));
  for (int cz = -radius; cz <= radius; ++cz) {
    for (int cx = -radius; cx <= radius; ++cx)
      chunks.emplace_back(cx, cz);
  }
  return chunks;
}

std::uint64_t column_digest(const mc::world::Chunk& chunk) {
  std::uint64_t hash = kFnvOffset;
  const int min_y = chunk.min_section_y * 16;
  for (int y = min_y; y < min_y + 384; ++y) {
    for (int z = 0; z < 16; ++z) {
      for (int x = 0; x < 16; ++x) {
        const mc::world::BlockStateKey key = chunk.block_state_at(x, y, z);
        hash = hash_bytes(hash, key.name.data(), key.name.size());
        const char separator = '|';
        hash = hash_bytes(hash, &separator, 1);
        hash = hash_bytes(hash, key.properties.data(), key.properties.size());
        const char terminator = '\0';
        hash = hash_bytes(hash, &terminator, 1);
        hash = hash_int(hash, x);
        hash = hash_int(hash, y);
        hash = hash_int(hash, z);
      }
    }
  }
  return hash;
}

std::string compiler_name() {
#if defined(__clang__)
  return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
  return std::string("gcc ") + __VERSION__;
#else
  return "unknown";
#endif
}

bool write_json(const std::string& path, const std::vector<long long>& seeds, int radius,
                const std::vector<Column>& columns) {
  std::string out;
  out.reserve(columns.size() * 96 + 256);
  char line[160];
  out += "{\n \"kind\": \"column-digest\",\n";
  out += " \"format\": 1,\n";
  out += " \"compiler\": \"" + compiler_name() + "\",\n";
  out += " \"seeds\": [";
  for (std::size_t i = 0; i < seeds.size(); ++i)
    out += (i == 0 ? "" : ", ") + std::to_string(seeds[i]);
  out += "],\n";
  std::snprintf(line, sizeof(line), " \"grid_radius\": %d,\n \"cells_per_column\": %lld,\n", radius,
                kCellsPerColumn);
  out += line;
  out += " \"columns\": [\n";
  for (std::size_t i = 0; i < columns.size(); ++i) {
    const Column& column = columns[i];
    std::snprintf(line, sizeof(line),
                  "  {\"seed\": %lld, \"cx\": %d, \"cz\": %d, \"digest\": \"%016llx\"}%s\n",
                  column.seed, column.cx, column.cz, static_cast<unsigned long long>(column.digest),
                  i + 1 == columns.size() ? "" : ",");
    out += line;
  }
  out += " ]\n}\n";

  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    std::fprintf(stderr, "cannot write %s\n", path.c_str());
    return false;
  }
  const bool wrote = std::fwrite(out.data(), 1, out.size(), file) == out.size();
  const bool closed = std::fclose(file) == 0;
  if (!wrote || !closed) {
    std::fprintf(stderr, "short write to %s\n", path.c_str());
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  std::vector<long long> seeds = {42, 7, 2026};
  int radius = 2;
  std::string json_path;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--seeds" && i + 1 < argc) {
      seeds = parse_seeds(argv[++i]);
      if (seeds.empty())
        return 2;
    } else if (arg == "--radius" && i + 1 < argc) {
      char* end = nullptr;
      const long value = std::strtol(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0' || value < 0 || value > 16) {
        std::fprintf(stderr, "--radius takes 0..16\n");
        return 2;
      }
      radius = static_cast<int>(value);
    } else if (arg == "--json" && i + 1 < argc) {
      json_path = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::printf("usage: %s [--seeds a,b,c] [--radius N] [--json FILE]\n", argv[0]);
      std::printf("  default: seeds 42,7,2026, radius 2 (a 5x5 grid per seed)\n");
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument %s (try --help)\n", arg.c_str());
      return 2;
    }
  }

  const std::vector<std::pair<int, int>> chunks = grid_chunks(radius);
  std::vector<Column> columns;
  columns.reserve(seeds.size() * chunks.size());
  std::uint64_t run_hash = kFnvOffset;

  for (const long long seed : seeds) {
    const auto generator = std::make_shared<const mc::world::WgOverworldGenerator>(seed);
    for (const auto& [cx, cz] : chunks) {
      mc::world::Chunk chunk;
      mc::world::fill_generated_decorated_chunk(*generator, cx, cz, chunk);
      const std::uint64_t digest = column_digest(chunk);
      columns.push_back(Column{seed, cx, cz, digest});
      run_hash = hash_bytes(run_hash, &digest, sizeof(digest));
    }
  }

  std::printf("%zu columns, %lld cells each, %zu seeds, radius %d\n", columns.size(),
              kCellsPerColumn, seeds.size(), radius);
  std::printf("  column digests: %016llx\n", static_cast<unsigned long long>(run_hash));
  if (!json_path.empty()) {
    if (!write_json(json_path, seeds, radius, columns))
      return 1;
    std::printf("record: %s\n", json_path.c_str());
  }
  return 0;
}
