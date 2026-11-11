// SPDX-License-Identifier: GPL-3.0-or-later
//
// The generation-time light seam as a repeatable number, over many seeds and
// chunks. `tests/world_light_test.cpp` pins the two columns the worldgen
// fixtures describe; this probe measures the same seam on a grid per seed and
// records it, so a generator or light-engine change can be read as a delta
// instead of a single pair of counts that either still matches or does not.
//
//   ./build/plusmc-light-seam                        # seeds 42,7,2026, radius 2
//   ./build/plusmc-light-seam --seeds 42,7 --radius 1
//   ./build/plusmc-light-seam --chunks 0,0;-1,-1;3,7 --json /tmp/seam.json
//
// The measurement is `tools/light_seam.h`'s, the same one the test asserts on:
// for each column, generate it, keep the light generation wrote, relight it over
// its eight generated neighbours, and walk both layers cell for cell. The tally
// splits the sky disagreements into the chunk's edge — the half a neighbouring
// chunk's light can reach — and the column's interior. The probe also
// cross-checks the two pinned columns against the test's own expectations, so
// the number recorded here stays tied to the one the test enforces.
//
// It exits 0 when every structural claim holds and 1 when one does not. The
// structural claims are the ones the approximation actually makes:
//
//   * the column carries light when it is generated (the send path's premise);
//   * the run disagrees *somewhere*: a relight that returned without computing
//     would agree with generation on every cell of both layers, so a run with no
//     disagreement at all cannot be told apart from a no-op. Per-column
//     `relit_sky` is recorded rather than asserted — a column the fill answers
//     exactly is a good outcome, not a broken one, and at generation the fill
//     now writes the column's own answer, so exact columns do occur;
//   * no disagreement at or above the column's own surface — that half is exact;
//   * no cell brighter than the engine, anywhere: the fill is a subset of the
//     engine's pass (its own sources only), so its answer is at or below;
//   * no brighter cell in the block layer at all — the fill lights only the
//     column's own emitters and stops at its border, so the ring's emitters can
//     only add.
//
// The counts themselves are *not* asserted here: a deliberate generator change
// is expected to move them. Record them with `bench/seam_track.py --save` and
// compare with a later run.
//
// `--json` writes the record to a file; the human summary still goes to stdout.
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "light_seam.h"

namespace {

using mc::bench::SeamColumn;
using mc::bench::SeamTally;
using mc::world::WgOverworldGenerator;

constexpr long long kCellsPerLayer = 16LL * 16 * 384;

int parse_int(const char* text, int fallback) {
  if (text == nullptr || *text == '\0')
    return fallback;
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (end == text || *end != '\0' || value < -64 || value > 64)
    return fallback;
  return static_cast<int>(value);
}

long long parse_seed(const char* text) {
  char* end = nullptr;
  const long long value = std::strtoll(text, &end, 10);
  if (end == text)
    return 0;
  return value;
}

// `42,7,2026`, or `42` repeated. Empty parts are skipped so a trailing comma is
// harmless.
std::vector<long long> parse_seeds(const std::string& text) {
  std::vector<long long> seeds;
  std::string::size_type begin = 0;
  while (begin <= text.size()) {
    const std::string::size_type comma = text.find(',', begin);
    const std::string part =
        text.substr(begin, comma == std::string::npos ? std::string::npos : comma - begin);
    if (!part.empty())
      seeds.push_back(parse_seed(part.c_str()));
    if (comma == std::string::npos)
      break;
    begin = comma + 1;
  }
  if (seeds.empty())
    seeds.push_back(mc::bench::kPinnedSeed);
  return seeds;
}

// `0,0;-1,-1;3,7`, in the order given.
bool parse_chunks(const std::string& text, std::vector<std::pair<int, int>>& out) {
  std::string::size_type begin = 0;
  while (begin <= text.size()) {
    const std::string::size_type semi = text.find(';', begin);
    const std::string part =
        text.substr(begin, semi == std::string::npos ? std::string::npos : semi - begin);
    if (!part.empty()) {
      const std::string::size_type comma = part.find(',');
      if (comma == std::string::npos)
        return false;
      char* end = nullptr;
      const long cx = std::strtol(part.c_str(), &end, 10);
      if (end == part.c_str() || *end != ',')
        return false;
      const long cz = std::strtol(end + 1, &end, 10);
      if (*end != '\0')
        return false;
      out.emplace_back(static_cast<int>(cx), static_cast<int>(cz));
    }
    if (semi == std::string::npos)
      break;
    begin = semi + 1;
  }
  return !out.empty();
}

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

void write_tally(std::ostream& out, const SeamTally& tally, const char* indent) {
  out << indent << "\"cells\": " << tally.cells << ",\n";
  out << indent << "\"agree\": " << tally.agree << ",\n";
  out << indent << "\"brighter\": " << tally.brighter << ",\n";
  out << indent << "\"dimmer\": " << tally.dimmer << ",\n";
  out << indent << "\"max_brighter\": " << tally.max_brighter << ",\n";
  out << indent << "\"max_dimmer\": " << tally.max_dimmer << ",\n";
  out << indent << "\"above_surface\": " << tally.above_surface << ",\n";
  out << indent << "\"border\": " << tally.border << ",\n";
  out << indent << "\"interior\": " << tally.interior << ",\n";
  out << indent << "\"brighter_border\": " << tally.brighter_border << ",\n";
  out << indent << "\"in_opaque\": " << tally.in_opaque << "\n";
}

// What one run measured, so the summary and the JSON are written from one place.
struct SeedRecord {
  long long seed = 0;
  SeamTally sky;
  SeamTally block;
  std::vector<std::pair<int, std::pair<int, SeamColumn>>> columns;
};

const mc::bench::SeamPinnedSky* pinned_for(int cx, int cz) {
  for (const auto& pin : mc::bench::kPinnedSky) {
    if (pin.cx == cx && pin.cz == cz)
      return &pin;
  }
  return nullptr;
}

} // namespace

int main(int argc, char** argv) {
  std::vector<long long> seeds = {mc::bench::kPinnedSeed, 7, 2026};
  int radius = 2;
  std::vector<std::pair<int, int>> chunks;
  std::string json_path;
  bool from_grid = true;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--seeds" && i + 1 < argc) {
      seeds = parse_seeds(argv[++i]);
    } else if (arg == "--radius" && i + 1 < argc) {
      const int parsed = parse_int(argv[++i], radius);
      if (parsed < 0 || parsed > 16) {
        std::fprintf(stderr, "--radius takes 0..16\n");
        return 2;
      }
      radius = parsed;
    } else if (arg == "--chunks" && i + 1 < argc) {
      from_grid = false;
      if (!parse_chunks(argv[++i], chunks)) {
        std::fprintf(stderr, "--chunks takes 'x,z;x,z;...'\n");
        return 2;
      }
    } else if (arg == "--json" && i + 1 < argc) {
      json_path = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::printf("usage: %s [--seeds a,b,c] [--radius N | --chunks x,z;x,z] [--json FILE]\n",
                  argv[0]);
      std::printf("  default: seeds %s, radius 2 (a 5x5 grid per seed)\n", "42,7,2026");
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument %s (try --help)\n", arg.c_str());
      return 2;
    }
  }
  if (chunks.empty())
    chunks = grid_chunks(radius);

  int failures = 0;
  long long pinned_checked = 0;
  long long pinned_mismatches = 0;
  long long relit_columns = 0;
  std::vector<SeedRecord> records;
  SeamTally sky_total, block_total;
  const auto started = std::chrono::steady_clock::now();

  for (const long long seed : seeds) {
    const auto generator = std::make_shared<const WgOverworldGenerator>(seed);
    SeedRecord record;
    record.seed = seed;
    for (const auto& [cx, cz] : chunks) {
      const SeamColumn column = mc::bench::seam_measure(generator, cx, cz);
      record.columns.emplace_back(cx, std::make_pair(cz, column));
      if (!column.measured) {
        std::fprintf(stderr,
                     "FAIL seed %lld chunk (%d, %d): could not generate or read the column\n", seed,
                     cx, cz);
        ++failures;
        continue;
      }
      record.sky.add(column.sky);
      record.block.add(column.block);

      char where[96];
      std::snprintf(where, sizeof(where), "seed %lld chunk (%d, %d)", seed, cx, cz);
      if (!column.generated_light) {
        std::fprintf(stderr, "FAIL %s: the generated column carries no light\n", where);
        ++failures;
      }
      if (column.relit_sky)
        ++relit_columns;
      if (column.sky.cells != kCellsPerLayer || column.block.cells != kCellsPerLayer) {
        std::fprintf(stderr, "FAIL %s: %lld sky / %lld block cells, expected %lld each\n", where,
                     column.sky.cells, column.block.cells, kCellsPerLayer);
        ++failures;
      }
      // Exact above the surface: an unobstructed cell reads 15 from both, and
      // that is the half a viewer standing on the terrain is served.
      if (column.sky.above_surface != 0) {
        std::fprintf(stderr, "FAIL %s: %lld sky disagreements at or above the surface\n", where,
                     column.sky.above_surface);
        ++failures;
      }
      // The fill is a subset of the engine's pass: it seeds the column's own
      // sources and nothing else, so no cell of it can read brighter than a
      // relight that also sees the ring.
      if (column.sky.brighter != 0) {
        std::fprintf(stderr, "FAIL %s: %lld sky cells brighter than the engine\n", where,
                     column.sky.brighter);
        ++failures;
      }
      // Generation lights the column's own emitters and nothing across the
      // border, so the engine's answer can only be at or above it.
      if (column.block.brighter != 0) {
        std::fprintf(stderr, "FAIL %s: %lld block cells brighter than the engine\n", where,
                     column.block.brighter);
        ++failures;
      }

      // The tie to the test: the same two columns, the same pinned counts.
      if (seed == mc::bench::kPinnedSeed) {
        if (const mc::bench::SeamPinnedSky* pin = pinned_for(cx, cz)) {
          ++pinned_checked;
          const bool match =
              column.sky.agree == pin->agree && column.sky.brighter == pin->brighter &&
              column.sky.dimmer == pin->dimmer && column.sky.border == pin->border &&
              column.sky.interior == pin->interior && column.sky.in_opaque == pin->in_opaque;
          if (!match) {
            ++pinned_mismatches;
            ++failures;
            std::fprintf(stderr,
                         "FAIL %s: pinned sky tally moved (agree %lld vs %lld, +%lld vs +%lld, "
                         "-%lld vs -%lld, border %lld vs %lld, interior %lld vs %lld, "
                         "opaque %lld vs %lld)\n",
                         where, column.sky.agree, pin->agree, column.sky.brighter, pin->brighter,
                         column.sky.dimmer, pin->dimmer, column.sky.border, pin->border,
                         column.sky.interior, pin->interior, column.sky.in_opaque, pin->in_opaque);
          }
        }
      }
    }
    sky_total.add(record.sky);
    block_total.add(record.block);
    records.push_back(std::move(record));
  }

  // See the claims at the top of the file: no disagreement anywhere means the
  // run is indistinguishable from a relight that never computed, and there is
  // nothing for a baseline to track either.
  if (sky_total.disagreements() == 0 && block_total.disagreements() == 0) {
    std::fprintf(stderr,
                 "FAIL: generation and the engine agree on every cell of the run; a relight "
                 "that never computed would look the same\n");
    ++failures;
  }

  for (const SeedRecord& record : records) {
    std::printf("seed %lld: %zu column%s  sky %lld/%lld agree (%.2f%% differ, max +%d/-%d)  "
                "block %lld/%lld agree (%.2f%% differ)\n",
                record.seed, record.columns.size(), record.columns.size() == 1 ? "" : "s",
                record.sky.agree, record.sky.cells, 100.0 * record.sky.error_fraction(),
                record.sky.max_brighter, record.sky.max_dimmer, record.block.agree,
                record.block.cells, 100.0 * record.block.error_fraction());
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::printf("%zu seeds, %zu columns, %lld cells per layer: sky %lld/%lld agree (%.2f%% differ), "
              "block %lld/%lld agree (%.2f%% differ)\n",
              records.size(), records.size() * chunks.size(), kCellsPerLayer, sky_total.agree,
              sky_total.cells, 100.0 * sky_total.error_fraction(), block_total.agree,
              block_total.cells, 100.0 * block_total.error_fraction());
  std::printf("  sky disagreements: %lld above the surface, %lld on the chunk edge, %lld "
              "interior, %lld inside an opaque block; max +%d/-%d\n",
              sky_total.above_surface, sky_total.border, sky_total.interior, sky_total.in_opaque,
              sky_total.max_brighter, sky_total.max_dimmer);
  std::printf("  relight: engine moved sky light in %lld/%zu columns\n", relit_columns,
              records.size() * chunks.size());
  std::printf("  pinned seed %lld: %lld/%zu columns checked, %lld mismatch%s (%.1f s)\n",
              mc::bench::kPinnedSeed, pinned_checked, std::size(mc::bench::kPinnedSky),
              pinned_mismatches, pinned_mismatches == 1 ? "" : "es", seconds);

  if (!json_path.empty()) {
    std::ostringstream out;
    out << "{\n";
    out << " \"kind\": \"light-seam\",\n";
    out << " \"format\": 1,\n";
    out << " \"seeds\": [";
    for (std::size_t i = 0; i < records.size(); ++i)
      out << (i == 0 ? "" : ", ") << records[i].seed;
    out << "],\n";
    out << " \"grid_radius\": " << (from_grid ? std::to_string(radius) : "null") << ",\n";
    out << " \"columns\": " << records.size() * chunks.size() << ",\n";
    out << " \"cells_per_layer\": " << kCellsPerLayer << ",\n";
    out << " \"pinned\": {\"seed\": " << mc::bench::kPinnedSeed
        << ", \"checked\": " << pinned_checked << ", \"mismatches\": " << pinned_mismatches
        << "},\n";
    out << " \"sky\": {\n";
    write_tally(out, sky_total, "  ");
    out << " },\n";
    out << " \"block\": {\n";
    write_tally(out, block_total, "  ");
    out << " },\n";
    out << " \"per_seed\": [\n";
    for (std::size_t s = 0; s < records.size(); ++s) {
      const SeedRecord& record = records[s];
      out << "  {\n";
      out << "   \"seed\": " << record.seed << ",\n";
      out << "   \"sky\": {\n";
      write_tally(out, record.sky, "    ");
      out << "   },\n";
      out << "   \"block\": {\n";
      write_tally(out, record.block, "    ");
      out << "   },\n";
      out << "   \"per_column\": [\n";
      for (std::size_t c = 0; c < record.columns.size(); ++c) {
        const auto& [cx, rest] = record.columns[c];
        const auto& [cz, column] = rest;
        out << "    {\"cx\": " << cx << ", \"cz\": " << cz
            << ", \"measured\": " << (column.measured ? "true" : "false")
            << ", \"generated_light\": " << (column.generated_light ? "true" : "false")
            << ", \"relit_sky\": " << (column.relit_sky ? "true" : "false") << ", \"sky\": {\n";
        write_tally(out, column.sky, "      ");
        out << "     }, \"block\": {\n";
        write_tally(out, column.block, "      ");
        out << "     }}";
        out << (c + 1 == record.columns.size() ? "\n" : ",\n");
      }
      out << "   ]\n";
      out << "  }" << (s + 1 == records.size() ? "\n" : ",\n");
    }
    out << " ]\n";
    out << "}\n";

    std::FILE* file = std::fopen(json_path.c_str(), "wb");
    if (file == nullptr) {
      std::fprintf(stderr, "cannot write %s: %s\n", json_path.c_str(), std::strerror(errno));
      return 1;
    }
    const std::string text = out.str();
    const std::size_t written = std::fwrite(text.data(), 1, text.size(), file);
    const bool closed = std::fclose(file) == 0;
    if (written != text.size() || !closed) {
      std::fprintf(stderr, "short write to %s\n", json_path.c_str());
      return 1;
    }
    std::printf("record: %s\n", json_path.c_str());
  }

  if (failures != 0) {
    std::fprintf(stderr, "%d structural failure%s\n", failures, failures == 1 ? "" : "s");
    return 1;
  }
  return 0;
}
