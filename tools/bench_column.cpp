// SPDX-License-Identifier: GPL-3.0-or-later
//
// What one generated column costs: `fill_generated_carved_chunk` (noise, biome,
// surface, carve, palettes, heightmaps, sky light), over a spread of columns.
// The server's store fills its columns one stage further —
// `fill_generated_decorated_chunk`, which adds the feature pass — so this is the
// carved stage's cost, not the whole decorated column's.
//
//   ./build/plusmc-bench-column                 # seed 42, 16 columns, 2 reps
//   ./build/plusmc-bench-column --columns 32 --reps 3
//   ./build/plusmc-bench-column --decorated     # the stage the server's store
//                                               # fills (carved + features)
//
// The point is the A/B arm on the same binary: the surface rules ask the biome
// source at the quart cell covering the block they are looking at, so a column
// asks the same quart cell up to 64 times, and `WgNoiseChunk::biome_at_quart`
// memoises those answers (world/worldgen_chunk.h). With
// `PLUSMC_BIOME_QUART_MEMO=off` the memo is bypassed and every one of them is a
// `cachedClimateSampler` read plus a biome search — the "before" on the same
// build, which is the only form of before this box can be trusted with (the
// measured numbers and why they are CPU seconds rather than wall are in
// docs/PHASE5.md, the quart memo section).
//
// The other arms this binary measures are the carve stage's carried chunk
// (`PLUSMC_CARVE_CHUNK_REUSE=off` builds the second, cold one), the heightmap
// rebuild's combined walk (`PLUSMC_HEIGHTMAP_SINGLE_WALK=off` walks each map
// separately) and top hint (`PLUSMC_HEIGHTMAP_TOP_HINT=off` starts every column
// at the top of the stack), the biome fill's shared chunk
// (`PLUSMC_BIOME_CHUNK_REUSE=off` builds the third graph), and the light
// region (`PLUSMC_LIGHT_REGION=off` reads blocks through the callback that
// copies a state per cell instead of through the chunk's palettes). All are on
// unless the variable says `off`, so an unset environment is the current
// behaviour; the startup line records which arms a saved run used.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

#include "world/chunk.h"
#include "world/generated_chunk.h"
#include "world/worldgen_chunk.h"

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

int parse_positive(const char* text, int fallback) {
  if (text == nullptr)
    return fallback;
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (end == text || value <= 0 || value > 1 << 20)
    return fallback;
  return static_cast<int>(value);
}

} // namespace

int main(int argc, char** argv) {
  int64_t seed = 42;
  int columns = 16;
  int reps = 2;
  bool decorated = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--seed" && i + 1 < argc)
      seed = std::strtoll(argv[++i], nullptr, 10);
    else if (arg == "--columns" && i + 1 < argc)
      columns = parse_positive(argv[++i], columns);
    else if (arg == "--reps" && i + 1 < argc)
      reps = parse_positive(argv[++i], reps);
    else if (arg == "--decorated")
      decorated = true;
    else {
      std::fprintf(stderr, "usage: %s [--seed N] [--columns N] [--reps N] [--decorated]\n",
                   argv[0]);
      return 2;
    }
  }

  // A spread rather than a run, so the surface rules see the generator's own
  // variety (ocean floor, hills, the far terrain) instead of one biome band.
  std::vector<std::pair<int, int>> coords;
  coords.reserve(static_cast<size_t>(columns));
  for (int i = 0; i < columns; ++i)
    coords.emplace_back(i * 37 - 300, i * 53 - 400);

  // Self-describing logs: an unset variable is the arm being measured, so the
  // line names the arm rather than the variable.
  const auto arm = [](const char* name) {
    const char* text = std::getenv(name);
    return (text == nullptr || std::strcmp(text, "off") != 0) ? "on" : "off";
  };
  std::printf("arms: biome-quart-memo=%s carve-chunk-reuse=%s heightmap-single-walk=%s "
              "heightmap-top-hint=%s biome-chunk-reuse=%s surface-rules-cache=%s "
              "light-region=%s\n",
              arm("PLUSMC_BIOME_QUART_MEMO"), arm("PLUSMC_CARVE_CHUNK_REUSE"),
              arm("PLUSMC_HEIGHTMAP_SINGLE_WALK"), arm("PLUSMC_HEIGHTMAP_TOP_HINT"),
              arm("PLUSMC_BIOME_CHUNK_REUSE"), arm("PLUSMC_SURFACE_RULES_CACHE"),
              arm("PLUSMC_LIGHT_REGION"));
  std::printf("stage: %s\n", decorated ? "decorated" : "carved");

  mc::world::WgOverworldGenerator generator(seed);
  double best = 1e300;
  for (int rep = 0; rep < reps; ++rep) {
    mc::world::Chunk chunk;
    const Clock::time_point start = Clock::now();
    for (const auto& [cx, cz] : coords) {
      if (decorated)
        mc::world::fill_generated_decorated_chunk(generator, cx, cz, chunk);
      else
        mc::world::fill_generated_carved_chunk(generator, cx, cz, chunk);
    }
    const double per_column = ms_since(start) / static_cast<double>(coords.size());
    std::printf("rep %d  %8.2f ms/column\n", rep, per_column);
    std::fflush(stdout);
    if (per_column < best)
      best = per_column;
  }
  // Best of the reps: the box this was written on is shared, and a rep that was
  // descheduled says nothing about the column. The arms' CPU seconds, not this
  // line, are the measurement (docs/PHASE5.md).
  std::printf("best %8.2f ms/column\n", best);
  return 0;
}
