// SPDX-License-Identifier: GPL-3.0-or-later
//
// Per-dimension worldgen against `tools/gen_worldgen_dump.py --dimension`.
// The overworld fixture already pins the shared seed stack; this test pins what
// is *not* shared: the seeded `minecraft:end_islands` density function, the
// per-dimension noise settings and default fluids, the legacy Nether/End
// blended noise, and both non-overworld biome sources.
#include <zlib.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "world/generated_chunk.h"
#include "world/worldgen_chunk.h"
#include "world/worldgen_density.h"
#include "world/worldgen_dimension.h"
#include "world/worldgen_feature.h"
#include "world/worldgen_state.h"
#include "world/worldgen_structure.h"
#include "world/worldgen_structure_piece.h"
#include "world/worldgen_surface.h"
#include "world/worldgen_table.h"

using namespace mc::world;

static int failures = 0;
static int checks = 0;
static std::string g_fixtures;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    ++checks;                                                                                      \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

static std::vector<std::string> split_tabs(const std::string& line) {
  std::vector<std::string> fields;
  std::string field;
  std::istringstream stream(line);
  while (std::getline(stream, field, '\t'))
    fields.push_back(field);
  return fields;
}

static std::vector<std::vector<std::string>> read_tsv(const std::string& path) {
  std::vector<std::vector<std::string>> rows;
  std::ifstream in(path);
  CHECK(in.good());
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    rows.push_back(split_tabs(line));
  }
  return rows;
}

static bool read_gz(const std::string& path, std::vector<uint8_t>& out) {
  gzFile file = gzopen(path.c_str(), "rb");
  if (file == nullptr)
    return false;
  out.clear();
  uint8_t buffer[1 << 16];
  int count = 0;
  while ((count = gzread(file, buffer, sizeof(buffer))) > 0)
    out.insert(out.end(), buffer, buffer + count);
  gzclose(file);
  return count == 0;
}

static const char* const kRouterFields[15] = {
    "barrier",
    "fluid_level_floodedness",
    "fluid_level_spread",
    "lava",
    "temperature",
    "vegetation",
    "continents",
    "erosion",
    "depth",
    "ridges",
    "preliminary_surface_level",
    "final_density",
    "vein_toggle",
    "vein_ridged",
    "vein_gap",
};

static int router_index(const std::string& name) {
  if (name == "preliminary_surface")
    return 10; // the dump's short name for `preliminary_surface_level`
  for (int i = 0; i < 15; ++i)
    if (name == kRouterFields[i])
      return i;
  return -1;
}

static void check_router(const WgRandomState& state, const WgDensityTable& table,
                         const std::string& path) {
  const auto rows = read_tsv(path);
  CHECK(!rows.empty());
  WgChunkCursor cursor;
  WgDensityGraph graph(state, cursor, /*wrap_markers=*/false, table);
  int matched = 0;
  for (const auto& row : rows) {
    if (row.size() < 6)
      continue;
    const int field = router_index(row[1]);
    CHECK(field >= 0);
    if (field < 0)
      continue;
    const int32_t x = std::atoi(row[2].c_str());
    const int32_t y = std::atoi(row[3].c_str());
    const int32_t z = std::atoi(row[4].c_str());
    const double expected = std::strtod(row[5].c_str(), nullptr);
    const double actual = graph.compute(graph.wrapped(table.router[static_cast<size_t>(field)]),
                                        WgDfContext::point(x, y, z));
    CHECK(actual == expected);
    if (actual != expected && matched < 5) {
      std::fprintf(stderr, "  %s router %s at (%d,%d,%d): %.17g != %.17g\n", path.c_str(),
                   row[1].c_str(), x, y, z, actual, expected);
    }
    ++matched;
  }
  CHECK(matched == static_cast<int>(rows.size()));
}

static void check_chunks(const WgDensityTable& table, const std::string& dir,
                         const std::string& prefix) {
  const WgOverworldGenerator generator(42, table);
  for (const auto& [cx, cz] : {std::pair{0, 0}, std::pair{1, 0}}) {
    char name[128];
    std::snprintf(name, sizeof(name), "/%s/noise_seed42_c%d_%d.bin.gz", dir.c_str(), cx, cz);
    std::vector<uint8_t> expected;
    CHECK(read_gz(g_fixtures + name, expected));
    CHECK(expected.size() == static_cast<size_t>(16 * 16 * table.height));
    std::vector<uint8_t> actual(expected.size(), 0);
    generator.fill_noise_chunk(cx, cz, actual.data());
    size_t mismatches = 0;
    size_t first = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
      // The dump palettes the default fluid and the aquifer fluid separately.
      // In the Nether they are both lava, and with aquifers disabled every
      // fluid cell is the default one (fixture slot 2, our semantic kLava).
      const bool default_lava_alias =
          expected[i] == 2 && actual[i] == 3 && table.default_fluid_code == 3;
      if (expected[i] != actual[i] && !default_lava_alias) {
        if (mismatches == 0) {
          first = i;
          const int y = table.min_y + static_cast<int>(i % table.height);
          const int z = static_cast<int>((i / table.height) % 16);
          const int x = static_cast<int>(i / table.height / 16);
          std::fprintf(stderr, "  %s chunk %d,%d first mismatch at (%d,%d,%d): %u != %u\n",
                       prefix.c_str(), cx, cz, x, y, z, actual[i], expected[i]);
        }
        ++mismatches;
      }
    }
    CHECK(mismatches == 0);
    std::printf("%s chunk %d,%d: %zu/%zu mismatches\n", prefix.c_str(), cx, cz, mismatches,
                expected.size());
  }
}

static void check_generated_chunks(const WgDensityTable& table, const std::string& dir,
                                   const std::string& prefix) {
  const WgOverworldGenerator generator(42, table);
  for (const auto& [cx, cz] : {std::pair{0, 0}, std::pair{1, 0}}) {
    char name[128];
    std::snprintf(name, sizeof(name), "/%s/noise_seed42_c%d_%d.bin.gz", dir.c_str(), cx, cz);
    std::vector<uint8_t> expected;
    CHECK(read_gz(g_fixtures + name, expected));
    CHECK(expected.size() == static_cast<size_t>(16 * 16 * table.height));

    Chunk actual;
    fill_generated_dimension_chunk(generator, cx, cz, actual);
    CHECK(actual.min_section_y == table.min_y / 16);
    CHECK(actual.sections.size() == static_cast<size_t>(table.height / 16));

    size_t mismatches = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
      const int y = table.min_y + static_cast<int>(i % table.height);
      const int z = static_cast<int>((i / table.height) % 16);
      const int x = static_cast<int>(i / table.height / 16);
      const uint8_t code = expected[i];
      const char* expected_name = nullptr;
      if (code == 0) {
        expected_name = "minecraft:air";
      } else if (code == 1 && table.default_block != nullptr) {
        expected_name = table.default_block;
      } else if (code == 2 && table.default_fluid_code == 3) {
        expected_name = "minecraft:lava";
      } else {
        expected_name = wg_surface_state(code).name;
      }
      const BlockStateKey state = actual.block_state_at(x, y, z);
      if (state.name != expected_name) {
        if (mismatches == 0) {
          std::fprintf(stderr, "  %s bridge %d,%d first mismatch at (%d,%d,%d): %s != %s\n",
                       prefix.c_str(), cx, cz, x, y, z, state.name.c_str(), expected_name);
        }
        ++mismatches;
      }
    }
    CHECK(mismatches == 0);
    std::printf("%s bridge %d,%d: %zu/%zu mismatches\n", prefix.c_str(), cx, cz, mismatches,
                expected.size());
  }
}

static std::vector<std::string> parse_state_codes(const std::string& text) {
  std::vector<std::string> names;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t comma = text.find(',', start);
    const std::string entry =
        text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    const size_t equals = entry.find('=');
    if (equals != std::string::npos)
      names.push_back(entry.substr(equals + 1));
    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }
  return names;
}

static void check_nether_surface(const WgDensityTable& table) {
  const auto meta = read_tsv(g_fixtures + "/nether/surface_meta.tsv");
  CHECK(!meta.empty());
  if (meta.empty() || meta[0].size() < 9)
    return;
  const std::vector<std::string> names = parse_state_codes(meta[0][8]);
  CHECK(!names.empty());
  const WgOverworldGenerator generator(42, table);
  for (const auto& [cx, cz] : {std::pair{0, 0}, std::pair{1, 0}}) {
    char path[128];
    std::snprintf(path, sizeof(path), "/nether/surface_seed42_c%d_%d.bin.gz", cx, cz);
    std::vector<uint8_t> expected;
    CHECK(read_gz(g_fixtures + path, expected));
    CHECK(expected.size() == static_cast<size_t>(16 * 16 * table.height));
    std::vector<uint8_t> actual(expected.size(), 0);
    generator.fill_surface_chunk(cx, cz, actual.data());
    Chunk bridge;
    fill_generated_dimension_surface_chunk(generator, cx, cz, bridge);
    size_t bridge_mismatches = 0;
    size_t mismatches = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
      const int y = table.min_y + static_cast<int>(i % table.height);
      const int z = static_cast<int>((i / table.height) % 16);
      const int x = static_cast<int>(i / table.height / 16);
      if (expected[i] >= names.size())
        continue;
      std::string expected_name = names[expected[i]];
      const size_t properties = expected_name.find('[');
      if (properties != std::string::npos)
        expected_name.resize(properties);
      const uint8_t code = actual[i];
      const char* actual_name = (code == 1 && table.default_block != nullptr)
                                    ? table.default_block
                                    : wg_surface_state(code).name;
      const BlockStateKey bridge_state = bridge.block_state_at(x, y, z);
      if (bridge_state.name != expected_name)
        ++bridge_mismatches;
      if (expected_name != actual_name) {
        if (mismatches == 0) {
          std::fprintf(stderr, "  nether surface %d,%d first mismatch at (%d,%d,%d): %s != %s\n",
                       cx, cz, x, y, z, actual_name, expected_name.c_str());
        }
        ++mismatches;
      }
    }
    CHECK(mismatches == 0);
    CHECK(bridge_mismatches == 0);
    std::printf("nether surface %d,%d: %zu/%zu mismatches, bridge %zu\n", cx, cz, mismatches,
                expected.size(), bridge_mismatches);
  }
}

static bool chunk_has_block(const Chunk& chunk, const char* name) {
  for (const Section& section : chunk.sections)
    for (const BlockStateKey& state : section.block_palette)
      if (state.name == name)
        return true;
  return false;
}

static void check_dimension_decoration(const WgDensityTable& nether_table,
                                       const WgDensityTable& end_table) {
  const WgOverworldGenerator nether(42, nether_table);
  bool nether_ore = false;
  bool nether_complex = false;
  for (int cx = 0; cx < 3; ++cx) {
    for (int cz = 0; cz < 3; ++cz) {
      Chunk chunk;
      WgDecorationStats stats;
      fill_generated_dimension_decorated_chunk(nether, cx, cz, chunk, &stats);
      CHECK(chunk.sections.size() == static_cast<size_t>(nether_table.height / 16));
      CHECK(stats.applied > 0);
      nether_ore = nether_ore || chunk_has_block(chunk, "minecraft:nether_quartz_ore") ||
                   chunk_has_block(chunk, "minecraft:nether_gold_ore") ||
                   chunk_has_block(chunk, "minecraft:magma_block");
      if (wg_nether_complex_kind(42, 0, 0) == WgNetherComplexKind::kFortress)
        nether_complex = nether_complex || chunk_has_block(chunk, "minecraft:nether_bricks");
      else
        nether_complex =
            nether_complex || chunk_has_block(chunk, "minecraft:polished_blackstone_bricks");
    }
  }
  CHECK(nether_ore);
  CHECK(nether_complex);

  // The Nether decoration path runs the carved stage before features. Compare
  // the same column's surface and carved arrays to prove the table-carried
  // `minecraft:nether_cave` carvers actually rewrite blocks.
  std::vector<uint8_t> surface_codes(static_cast<size_t>(16) * 16 * nether_table.height);
  std::vector<uint8_t> carved_codes(surface_codes.size());
  nether.fill_surface_chunk(0, 0, surface_codes.data());
  nether.fill_carved_chunk(0, 0, carved_codes.data());
  CHECK(std::memcmp(surface_codes.data(), carved_codes.data(), surface_codes.size()) != 0);

  const WgOverworldGenerator end(42, end_table);
  Chunk spike_chunk;
  WgDecorationStats spike_stats;
  fill_generated_dimension_decorated_chunk(end, 0, -3, spike_chunk, &spike_stats);
  CHECK(spike_stats.applied > 0);
  CHECK(spike_chunk.sections.size() == 16);
  CHECK(spike_chunk.min_section_y == 0);
  CHECK(chunk_has_block(spike_chunk, "minecraft:obsidian"));

  // `end_platform` is a fixed placement at (100, 49, 0); its 5 x 5 obsidian
  // floor sits at y 48 and the interior above it is air.
  Chunk platform_chunk;
  WgDecorationStats platform_stats;
  fill_generated_dimension_decorated_chunk(end, 6, 0, platform_chunk, &platform_stats);
  CHECK(platform_stats.applied > 0);
  CHECK(platform_chunk.sections.size() == 16);
  CHECK(platform_chunk.block_state_at(4, 48, 0).name == "minecraft:obsidian");
  CHECK(platform_chunk.block_state_at(4, 49, 0).name == "minecraft:air");

  // Find a real (biome-gated) End city on the outer islands, then make sure
  // the decorated bridge writes it rather than only building the start.
  const WgStructureSet* end_cities = wg_structure_set_by_name("minecraft:end_cities");
  CHECK(end_cities != nullptr);
  WgClimateSampler end_sampler(end.state(), end.table());
  WgGeneratedHeights end_heights(end);
  WgJigsawStart city;
  if (end_cities != nullptr) {
    for (int cz = -120; cz <= 120 && !city.valid; ++cz) {
      for (int cx = -120; cx <= 120 && !city.valid; ++cx) {
        if (!wg_structure_is_chunk(42, *end_cities, cx, cz))
          continue;
        city = wg_end_city_start(42, cx, cz, end_sampler, end_heights, false);
      }
    }
  }
  CHECK(city.valid);
  if (city.valid) {
    Chunk city_chunk;
    fill_generated_dimension_decorated_chunk(end, city.chunk_x, city.chunk_z, city_chunk);
    CHECK(chunk_has_block(city_chunk, "minecraft:purpur_block") ||
          chunk_has_block(city_chunk, "minecraft:end_stone_bricks"));
  }
}

static int dimension_chunk_mismatches(const Chunk& a, const Chunk& b);

// The dimension bridge owns a different region shape per dimension: the Nether
// builds a 3x3 stack and decorates all nine columns, while the End deliberately
// keeps its single-column path. Both entry points run the same feature stage,
// so this test drives one of each concurrently and compares every result with
// its sequential baseline. With a process-global region pointer the End workers
// would null or replace the Nether workers' regions (and vice versa); with the
// thread-local pointer each dimension stays private.
static void check_concurrent_dimension_decoration(const WgDensityTable& nether_table,
                                                  const WgDensityTable& end_table) {
  enum class Kind { kNether, kEnd };
  struct Job {
    Kind kind;
    int cx;
    int cz;
  };
  constexpr size_t kWorkers = 8;
  constexpr int kIterations = 3;
  const Job jobs[kWorkers] = {
      {Kind::kNether, 0, 0},  {Kind::kNether, 16, 0}, {Kind::kNether, -16, 0},
      {Kind::kNether, 0, 16}, {Kind::kEnd, 0, -3},    {Kind::kEnd, 6, 0},
      {Kind::kEnd, 32, 0},    {Kind::kEnd, 0, 32},
  };

  std::array<Chunk, kWorkers> baselines{};
  WgOverworldGenerator baseline_nether(42, nether_table);
  WgOverworldGenerator baseline_end(42, end_table);
  for (size_t i = 0; i < kWorkers; ++i) {
    WgOverworldGenerator& generator =
        jobs[i].kind == Kind::kNether ? baseline_nether : baseline_end;
    fill_generated_dimension_decorated_chunk(generator, jobs[i].cx, jobs[i].cz, baselines[i]);
  }
  // Distinct columns make a wrong-region result visible rather than accidentally
  // identical. The Nether and End baselines are checked within each dimension.
  CHECK(dimension_chunk_mismatches(baselines[0], baselines[1]) > 0);
  CHECK(dimension_chunk_mismatches(baselines[4], baselines[5]) > 0);

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
      WgOverworldGenerator generator(42, jobs[i].kind == Kind::kNether ? nether_table : end_table);
      for (int iteration = 0; iteration < kIterations; ++iteration) {
        Chunk work;
        start.wait();
        fill_generated_dimension_decorated_chunk(generator, jobs[i].cx, jobs[i].cz, work);
        finish.wait();
        if (mismatches[i] == 0)
          mismatches[i] = dimension_chunk_mismatches(work, baselines[i]);
      }
    });
  }
  for (std::thread& worker : workers)
    worker.join();

  int mismatch = 0;
  for (size_t i = 0; i < kWorkers; ++i) {
    if (mismatches[i] != 0)
      std::fprintf(stderr, "  %s (%d,%d): %d mismatched cells\n",
                   jobs[i].kind == Kind::kNether ? "nether" : "end", jobs[i].cx, jobs[i].cz,
                   mismatches[i]);
    mismatch += mismatches[i];
  }
  std::printf("  concurrent dimensions: %zu workers x %d rounds, %d mismatched cells\n", kWorkers,
              kIterations, mismatch);
  CHECK(mismatch == 0);
}

static int dimension_chunk_mismatches(const Chunk& a, const Chunk& b) {
  if (a.min_section_y != b.min_section_y || a.sections.size() != b.sections.size())
    return 1;
  const int min_y = a.min_section_y * 16;
  const int max_y = min_y + static_cast<int>(a.sections.size()) * 16;
  int mismatches = 0;
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z)
      for (int y = min_y; y < max_y; ++y)
        if (!(a.block_state_at(x, y, z) == b.block_state_at(x, y, z)))
          ++mismatches;
  return mismatches;
}

static void check_biomes(const WgRandomState& state, const WgDensityTable& table,
                         const std::string& path) {
  const auto rows = read_tsv(path);
  CHECK(!rows.empty());
  WgClimateSampler sampler(state, table);
  int matched = 0;
  for (const auto& row : rows) {
    if (row.size() < 13)
      continue;
    const int32_t quart_x = std::atoi(row[3].c_str());
    const int32_t quart_y = std::atoi(row[4].c_str());
    const int32_t quart_z = std::atoi(row[5].c_str());
    const char* actual = sampler.biome_at_quart(quart_x, quart_y, quart_z);
    CHECK(std::strcmp(actual, row[12].c_str()) == 0);
    if (std::strcmp(actual, row[12].c_str()) != 0 && matched < 5) {
      std::fprintf(stderr, "  %s at (%d,%d,%d): %s != %s\n", path.c_str(), quart_x, quart_y,
                   quart_z, actual, row[12].c_str());
    }
    ++matched;
  }
  CHECK(matched > 0);
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <fixtures/worldgen>\n", argv[0]);
    return 2;
  }
  g_fixtures = argv[1];
  WgRandomState state(42);

  check_router(state, wg_end_table(), g_fixtures + "/end/router.tsv");
  for (int cx = 0; cx < 2; ++cx) {
    char path[128];
    std::snprintf(path, sizeof(path), "/end/biomes_seed42_c%d_0.tsv", cx);
    check_biomes(state, wg_end_table(), g_fixtures + path);
  }
  check_biomes(state, wg_end_table(), g_fixtures + "/end/biomes_seed42_c100_0.tsv");
  check_biomes(state, wg_end_table(), g_fixtures + "/end/biomes_seed42_c0_100.tsv");
  check_chunks(wg_end_table(), "end", "end");
  check_generated_chunks(wg_end_table(), "end", "end");

  check_router(state, wg_nether_table(), g_fixtures + "/nether/router.tsv");
  for (int cx = 0; cx < 2; ++cx) {
    char path[128];
    std::snprintf(path, sizeof(path), "/nether/biomes_seed42_c%d_0.tsv", cx);
    check_biomes(state, wg_nether_table(), g_fixtures + path);
  }
  check_biomes(state, wg_nether_table(), g_fixtures + "/nether/biomes_seed42_c100_0.tsv");
  check_biomes(state, wg_nether_table(), g_fixtures + "/nether/biomes_seed42_c0_100.tsv");
  check_chunks(wg_nether_table(), "nether", "nether");
  check_generated_chunks(wg_nether_table(), "nether", "nether");
  check_nether_surface(wg_nether_table());
  check_dimension_decoration(wg_nether_table(), wg_end_table());
  check_concurrent_dimension_decoration(wg_nether_table(), wg_end_table());

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
