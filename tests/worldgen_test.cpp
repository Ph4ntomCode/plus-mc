// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 4 milestone K: the overworld world generator against a real vanilla
// dump. `tools/gen_worldgen_dump.py` boots the 26.2 server jar's own worldgen
// registries, seeds them with 42, and writes what it finds into
// `tests/fixtures/worldgen`. This test reads those files back and pins, in
// order of how much of the stack they cover:
//
//   * `primitives.tsv` — the seed stack itself: `XoroshiroRandomSource`'s longs,
//     bounded ints, doubles (the `(double)1.110223E-16f` unit constant), floats
//     and booleans; `PositionalRandomFactory.fromHashOf` / `fromSeed` / `at`.
//   * the same file's `noise` rows — every one of the 63 noise parameter sets
//     in the pack, `NormalNoise.getValue` at five points (315 values).
//   * the `blended` rows — `overworld/base_3d_noise`, whose random comes from
//     `fromHashOf("minecraft:terrain")`, at five points.
//   * `router.tsv` — twelve router entries at 5 x 11 x 4 points, which is the
//     whole spline/density algebra with no chunk involved.
//   * `named.tsv` — every `density_function` in the pack, by name, at 3 x 11 x 3
//     points, each one wired the way `RandomState` wires the router (a bare
//     registry function is unwired and answers zeros). The table carries the 19
//     the overworld router reaches; the rest are outside that closure, and the
//     test checks their names so a repacked fixture cannot shrink it silently.
//   * `interpolated_seed42_c*_*.tsv` — `getInterpolatedNoiseValue`, i.e. the
//     density the fill loop would use, for two chunks at 8 x 8 x 26 points.
//   * `noise_seed42_c*_*.bin.gz` — the *whole* noise stage for those two
//     chunks: 16 x 16 x 384 cells each, one byte per cell, compared
//     byte-for-byte. Everything above has to be right for this to pass.
//
// Doubles are compared by bit pattern (`==` on the parsed hex value), not by a
// tolerance: the point of the milestone is that both sides are the same
// arithmetic, and the reference values are written as hex literals precisely so
// this comparison is exact.
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "world/chunk.h"
#include "world/generated_chunk.h"
#include "world/memory_world.h"
#include "world/worldgen_biome.h"
#include "world/worldgen_biome_table.h"
#include "world/worldgen_chunk.h"
#include "world/worldgen_math.h"
#include "world/worldgen_noise.h"
#include "world/worldgen_random.h"
#include "world/worldgen_state.h"
#include "world/worldgen_surface.h"
#include "world/worldgen_table.h"

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

// `Double.toHexString` round-trips through `strtod` (C99 hex floats), and the
// fixture only ever writes finite values.
double parse_double(const std::string& text) {
  return std::strtod(text.c_str(), nullptr);
}

int32_t parse_int(const std::string& text) {
  return static_cast<int32_t>(std::strtol(text.c_str(), nullptr, 10));
}

int64_t parse_long(const std::string& text) {
  return static_cast<int64_t>(std::strtoll(text.c_str(), nullptr, 10));
}

std::ifstream open_fixture(const std::string& name) {
  const std::string path = g_fixtures + "/" + name;
  std::ifstream file(path);
  if (!file) {
    std::fprintf(stderr, "FAIL cannot open fixture %s\n", path.c_str());
    ++failures;
  }
  return file;
}

void report(const std::string& label, int checked) {
  std::printf("  %-34s %6d values\n", label.c_str(), checked);
}

// Every line of a dump, split into fields, with the `#` header skipped.
void for_each_row(const std::string& file_name,
                  const std::function<void(const std::vector<std::string>&)>& row_fn) {
  std::ifstream file = open_fixture(file_name);
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    row_fn(split_tabs(line));
  }
}

int32_t noise_index_by_name(const std::string& name) {
  for (size_t i = 0; i < sizeof(kWgNoiseParams) / sizeof(kWgNoiseParams[0]); ++i) {
    if (name == kWgNoiseParams[i].name)
      return static_cast<int32_t>(i);
  }
  return -1;
}

// The table node for a registry name, or -1 for a function outside the
// overworld router's closure (the table only carries what the router reaches).
int32_t named_node_by_name(const std::string& name) {
  for (size_t i = 0; i < sizeof(kWgNamed) / sizeof(kWgNamed[0]); ++i) {
    if (name == kWgNamed[i].name)
      return static_cast<int32_t>(kWgNamed[i].node);
  }
  return -1;
}

WgRouterField router_field_by_name(const std::string& name) {
  if (name == "barrier")
    return WgRouterField::kBarrier;
  if (name == "fluid_level_floodedness")
    return WgRouterField::kFluidLevelFloodedness;
  if (name == "fluid_level_spread")
    return WgRouterField::kFluidLevelSpread;
  if (name == "lava")
    return WgRouterField::kLava;
  if (name == "temperature")
    return WgRouterField::kTemperature;
  if (name == "vegetation")
    return WgRouterField::kVegetation;
  if (name == "continents")
    return WgRouterField::kContinents;
  if (name == "erosion")
    return WgRouterField::kErosion;
  if (name == "depth")
    return WgRouterField::kDepth;
  if (name == "ridges")
    return WgRouterField::kRidges;
  if (name == "preliminary_surface")
    return WgRouterField::kPreliminarySurfaceLevel;
  if (name == "final_density")
    return WgRouterField::kFinalDensity;
  std::fprintf(stderr, "FAIL unknown router entry %s\n", name.c_str());
  ++failures;
  return WgRouterField::kBarrier;
}

// ---------------------------------------------------------------------------
// The seed stack.
// ---------------------------------------------------------------------------

void test_random_primitives() {
  int checked = 0;
  for_each_row("primitives.tsv", [&](const std::vector<std::string>& fields) {
    if (fields[0] != "random")
      return;
    const int64_t seed = parse_long(fields[2]);
    const std::string& key = fields[1];
    if (key == "longs") {
      WgRandom random(seed);
      const std::string& joined = fields[5];
      size_t start = 0;
      while (true) {
        const size_t comma = joined.find(',', start);
        const std::string part =
            comma == std::string::npos ? joined.substr(start) : joined.substr(start, comma - start);
        CHECK_EQ(static_cast<int64_t>(random.next_long()), parse_long(part));
        ++checked;
        if (comma == std::string::npos)
          break;
        start = comma + 1;
      }
    } else if (key == "ints") {
      WgRandom random(seed);
      std::string text = fields[5];
      size_t start = 0;
      while (true) {
        const size_t comma = text.find(',', start);
        const std::string part =
            comma == std::string::npos ? text.substr(start) : text.substr(start, comma - start);
        CHECK_EQ(random.next_int(1000), parse_int(part));
        ++checked;
        if (comma == std::string::npos)
          break;
        start = comma + 1;
      }
    } else if (key == "doubles") {
      WgRandom random(seed);
      std::string text = fields[5];
      size_t start = 0;
      while (true) {
        const size_t comma = text.find(',', start);
        const std::string part =
            comma == std::string::npos ? text.substr(start) : text.substr(start, comma - start);
        const double expected = parse_double(part);
        const double actual = random.next_double();
        CHECK(actual == expected);
        if (actual != expected) {
          std::fprintf(stderr, "    seed %lld double: got %a want %a\n",
                       static_cast<long long>(seed), actual, expected);
        }
        ++checked;
        if (comma == std::string::npos)
          break;
        start = comma + 1;
      }
    } else if (key == "floats") {
      WgRandom random(seed);
      std::string text = fields[5];
      size_t start = 0;
      while (true) {
        const size_t comma = text.find(',', start);
        const std::string part =
            comma == std::string::npos ? text.substr(start) : text.substr(start, comma - start);
        const float expected = std::strtof(part.c_str(), nullptr);
        CHECK(random.next_float() == expected);
        ++checked;
        if (comma == std::string::npos)
          break;
        start = comma + 1;
      }
    } else if (key == "bools") {
      WgRandom random(seed);
      std::string text = fields[5];
      size_t start = 0;
      while (true) {
        const size_t comma = text.find(',', start);
        const std::string part =
            comma == std::string::npos ? text.substr(start) : text.substr(start, comma - start);
        CHECK_EQ(random.next_boolean() ? 1 : 0, parse_int(part));
        ++checked;
        if (comma == std::string::npos)
          break;
        start = comma + 1;
      }
    }
  });
  report("random primitives", checked);
}

void test_positional() {
  const int64_t seed = 42;
  WgPositionalFactory root = WgRandom(seed).fork_positional();
  WgPositionalFactory aquifer = root.from_hash_of("minecraft:aquifer").fork_positional();
  WgPositionalFactory ore = root.from_hash_of("minecraft:ore").fork_positional();
  int checked = 0;
  for_each_row("primitives.tsv", [&](const std::vector<std::string>& fields) {
    if (fields[0] != "positional")
      return;
    if (fields[1] == "fromHashOf") {
      // `key` is the name, the value is `fromHashOf(name).nextLong()`.
      WgRandom random = root.from_hash_of(fields[2]);
      CHECK_EQ(static_cast<int64_t>(random.next_long()), parse_long(fields[5]));
      ++checked;
    } else if (fields[1] == "fromSeed") {
      WgRandom random = root.from_seed(7);
      CHECK_EQ(static_cast<int64_t>(random.next_long()), parse_long(fields[5]));
      ++checked;
    } else if (fields[1] == "at" || fields[1] == "aquifer.at" || fields[1] == "ore.at") {
      // The dump's `at` rows carry the point as `x` and `y;z`, with `-` in the
      // name column.
      const int32_t x = parse_int(fields[3]);
      const size_t semi = fields[4].find(';');
      const int32_t y = parse_int(fields[4].substr(0, semi));
      const int32_t z = parse_int(fields[4].substr(semi + 1));
      WgPositionalFactory* factory = &root;
      if (fields[1] == "aquifer.at")
        factory = &aquifer;
      if (fields[1] == "ore.at")
        factory = &ore;
      WgRandom random = factory->at(x, y, z);
      CHECK_EQ(static_cast<int64_t>(random.next_long()), parse_long(fields[5]));
      ++checked;
    }
  });
  report("positional factories", checked);
}

// ---------------------------------------------------------------------------
// The noise layer: every parameter set, then the blended terrain noise.
// ---------------------------------------------------------------------------

void test_noise_samples() {
  WgRandomState state(42);
  int checked = 0;
  int names = 0;
  std::vector<uint8_t> seen(sizeof(kWgNoiseParams) / sizeof(kWgNoiseParams[0]), 0);
  for_each_row("primitives.tsv", [&](const std::vector<std::string>& fields) {
    if (fields[0] != "noise")
      return;
    const int32_t index = noise_index_by_name(fields[1]);
    CHECK(index >= 0);
    if (index < 0)
      return;
    seen[static_cast<size_t>(index)] = 1;
    const double x = parse_double(fields[2]);
    const double y = parse_double(fields[3]);
    const double z = parse_double(fields[4]);
    const double expected = parse_double(fields[5]);
    const double actual = state.noise(index).get_value(x, y, z);
    CHECK(actual == expected);
    if (actual != expected) {
      std::fprintf(stderr, "    %s at (%a, %a, %a): got %a want %a\n", fields[1].c_str(), x, y, z,
                   actual, expected);
    }
    ++checked;
  });
  for (uint8_t present : seen) {
    if (present != 0)
      ++names;
  }
  CHECK_EQ(names, static_cast<int>(sizeof(kWgNoiseParams) / sizeof(kWgNoiseParams[0])));
  report("normal noise samples", checked);
}

// The amplitude lists are nullable by declaration (`const double*`), and the
// constructors index them once per octave. A caller that means "no octaves"
// hands null rather than an array of zeroes; both spellings must answer zero,
// and the all-zero list must not reach the `max - min` span computation with
// its two sentinels still in place (that subtraction overflows int32).
void test_noise_null_amplitudes() {
  WgRandom random(42);
  WgPerlinNoise perlin(random, 0, nullptr, 4, /*use_new_initialization=*/true);
  CHECK(perlin.get_value(1.0, 2.0, 3.0) == 0.0);
  CHECK(perlin.octave_noise(0) == nullptr);

  WgNormalNoise normal;
  normal.init(random, 0, nullptr, 4);
  CHECK(normal.get_value(1.0, 2.0, 3.0) == 0.0);

  // The same absent-octave set spelled as an explicit zero list, through both
  // the new-style and the legacy construction.
  const double zeros[4] = {0.0, 0.0, 0.0, 0.0};
  WgNormalNoise zero_normal;
  zero_normal.init(random, 0, zeros, 4);
  CHECK(zero_normal.get_value(1.0, 2.0, 3.0) == 0.0);
  WgPerlinNoise zero_perlin(random, 0, zeros, 4, /*use_new_initialization=*/false);
  CHECK(zero_perlin.get_value(1.0, 2.0, 3.0) == 0.0);
}

void test_blended_noise() {
  WgRandomState state(42);
  int checked = 0;
  for_each_row("primitives.tsv", [&](const std::vector<std::string>& fields) {
    if (fields[0] != "blended")
      return;
    // The dump floored its samples into block coordinates before the call.
    const int32_t x = wg_floor(parse_double(fields[2]));
    const int32_t y = wg_floor(parse_double(fields[3]));
    const int32_t z = wg_floor(parse_double(fields[4]));
    const double expected = parse_double(fields[5]);
    const double actual = state.blended(0).compute(x, y, z);
    CHECK(actual == expected);
    if (actual != expected) {
      std::fprintf(stderr, "    base_3d_noise at (%d, %d, %d): got %a want %a\n", x, y, z, actual,
                   expected);
    }
    ++checked;
  });
  report("blended noise samples", checked);
}

// ---------------------------------------------------------------------------
// The router, with no chunk in the picture: the splines alone.
// ---------------------------------------------------------------------------

void test_router() {
  WgOverworldGenerator generator(42);
  int checked = 0;
  for_each_row("router.tsv", [&](const std::vector<std::string>& fields) {
    CHECK_EQ(parse_long(fields[0]), 42);
    const WgRouterField field = router_field_by_name(fields[1]);
    const int32_t x = parse_int(fields[2]);
    const int32_t y = parse_int(fields[3]);
    const int32_t z = parse_int(fields[4]);
    const double expected = parse_double(fields[5]);
    const double actual = generator.router_value(field, x, y, z);
    CHECK(actual == expected);
    if (actual != expected) {
      std::fprintf(stderr, "    %s at (%d, %d, %d): got %a want %a\n", fields[1].c_str(), x, y, z,
                   actual, expected);
    }
    ++checked;
  });
  report("router entries", checked);
}

// ---------------------------------------------------------------------------
// The named density functions: the same raw read, one subtree at a time.
// ---------------------------------------------------------------------------

// The registry's other 16 functions: outside the overworld router's closure,
// so not in the table. `minecraft:zero` is one of the router's own constants
// inlined as a literal, and the rest are the end, nether, amplified and
// large-biomes variants — the last two would compile, but nothing here needs
// them, and `end/sloped_cheese` would drag in `end_islands`.
const char* const kUnreachedNamed[] = {
    "minecraft:end/base_3d_noise",
    "minecraft:end/sloped_cheese",
    "minecraft:nether/base_3d_noise",
    "minecraft:overworld_amplified/depth",
    "minecraft:overworld_amplified/factor",
    "minecraft:overworld_amplified/jaggedness",
    "minecraft:overworld_amplified/offset",
    "minecraft:overworld_amplified/sloped_cheese",
    "minecraft:overworld_large_biomes/continents",
    "minecraft:overworld_large_biomes/depth",
    "minecraft:overworld_large_biomes/erosion",
    "minecraft:overworld_large_biomes/factor",
    "minecraft:overworld_large_biomes/jaggedness",
    "minecraft:overworld_large_biomes/offset",
    "minecraft:overworld_large_biomes/sloped_cheese",
    "minecraft:zero",
};

// Four of the named functions are exactly the router entries the settings JSON
// names, so both reads of one have to agree. That is also what says the fixture
// is *wired*: an unwired registry function answers 0.0 for every noise it owns,
// and `continents` here is a noise.
const char* const kRouterAliasedNamed[] = {
    "minecraft:overworld/continents",
    "minecraft:overworld/erosion",
    "minecraft:overworld/depth",
    "minecraft:overworld/ridges",
};

void test_named_functions() {
  WgOverworldGenerator generator(42);
  std::vector<std::string> compared;
  std::vector<std::string> unreached;
  int checked = 0;
  for_each_row("named.tsv", [&](const std::vector<std::string>& fields) {
    const std::string& name = fields[0];
    const int32_t node = named_node_by_name(name);
    if (node < 0) {
      if (std::find(unreached.begin(), unreached.end(), name) == unreached.end()) {
        unreached.push_back(name);
      }
      return;
    }
    if (std::find(compared.begin(), compared.end(), name) == compared.end()) {
      compared.push_back(name);
    }
    const int32_t x = parse_int(fields[1]);
    const int32_t y = parse_int(fields[2]);
    const int32_t z = parse_int(fields[3]);
    const double expected = parse_double(fields[4]);
    const double actual = generator.node_value(static_cast<uint32_t>(node), x, y, z);
    CHECK(actual == expected);
    if (actual != expected) {
      std::fprintf(stderr, "    %s at (%d, %d, %d): got %a want %a\n", name.c_str(), x, y, z,
                   actual, expected);
    }
    for (const char* alias : kRouterAliasedNamed) {
      if (name != alias)
        continue;
      const double router = generator.router_value(
          router_field_by_name(name.substr(name.find_last_of('/') + 1)), x, y, z);
      CHECK(router == actual);
      break;
    }
    ++checked;
  });
  CHECK_EQ(compared.size(), sizeof(kWgNamed) / sizeof(kWgNamed[0]));
  CHECK_EQ(unreached.size(), sizeof(kUnreachedNamed) / sizeof(kUnreachedNamed[0]));
  for (const char* name : kUnreachedNamed) {
    CHECK(std::find(unreached.begin(), unreached.end(), name) != unreached.end());
  }
  report("named density functions", checked);
}

// ---------------------------------------------------------------------------
// The interpolated density: the fill loop's own read.
// ---------------------------------------------------------------------------

void test_interpolated_density() {
  WgOverworldGenerator generator(42);
  int checked = 0;
  for (const std::string& file :
       {"interpolated_seed42_c0_0.tsv", "interpolated_seed42_c-1_-1.tsv"}) {
    for_each_row(file, [&](const std::vector<std::string>& fields) {
      const int32_t x = parse_int(fields[3]);
      const int32_t y = parse_int(fields[4]);
      const int32_t z = parse_int(fields[5]);
      const double expected = parse_double(fields[6]);
      const double actual = generator.interpolated_density(x, y, z);
      CHECK(actual == expected);
      if (actual != expected) {
        std::fprintf(stderr, "    %s (%d, %d, %d): got %a want %a\n", file.c_str(), x, y, z, actual,
                     expected);
      }
      ++checked;
    });
  }
  report("interpolated densities", checked);
}

// ---------------------------------------------------------------------------
// The whole thing: the noise stage of two chunks, byte for byte.
// ---------------------------------------------------------------------------

std::vector<uint8_t> read_gzip(const std::string& name) {
  const std::string path = g_fixtures + "/" + name;
  gzFile file = gzopen(path.c_str(), "rb");
  if (file == nullptr) {
    std::fprintf(stderr, "FAIL cannot open %s\n", path.c_str());
    ++failures;
    return {};
  }
  std::vector<uint8_t> data;
  uint8_t buffer[4096];
  int read = 0;
  while ((read = gzread(file, buffer, sizeof(buffer))) > 0) {
    data.insert(data.end(), buffer, buffer + read);
  }
  gzclose(file);
  return data;
}

struct ChunkCase {
  const char* file;
  int32_t chunk_x;
  int32_t chunk_z;
};

void test_noise_chunks() {
  const ChunkCase cases[] = {{"noise_seed42_c0_0.bin.gz", 0, 0},
                             {"noise_seed42_c-1_-1.bin.gz", -1, -1},
                             {"noise_seed42_c-229_-256.bin.gz", -229, -256},
                             {"noise_seed42_c201_442.bin.gz", 201, 442}};
  for (const ChunkCase& test_case : cases) {
    const std::vector<uint8_t> expected = read_gzip(test_case.file);
    const size_t cells = static_cast<size_t>(16) * 16 * kWgHeight;
    CHECK_EQ(expected.size(), static_cast<long long>(cells));
    if (expected.size() != cells)
      continue;

    std::vector<uint8_t> actual(cells, 0);
    WgOverworldGenerator generator(42);
    generator.fill_noise_chunk(test_case.chunk_x, test_case.chunk_z, actual.data());

    size_t mismatches = 0;
    // The two fixtures' own block-code maps: 0 air, 1 stone, 2 water, 3 lava,
    // 4 granite, 5 copper ore, 6 raw copper, 7 tuff, 8 deepslate iron, 9 raw
    // iron — the same order as `WgNoiseBlock`.
    for (size_t i = 0; i < cells; ++i) {
      if (actual[i] == expected[i])
        continue;
      if (mismatches < 400) {
        const size_t y = i % kWgHeight;
        const size_t x = i / (kWgHeight * 16);
        const size_t z = (i / kWgHeight) % 16;
        std::fprintf(stderr, "FAIL %s (%zu, %lld, %zu): got %u want %u\n", test_case.file, x,
                     static_cast<long long>(y) + kWgMinY, z, actual[i], expected[i]);
      }
      ++mismatches;
      ++failures;
    }
    ++checks;
    report(test_case.file, static_cast<int>(cells));
    if (mismatches == 0) {
      std::printf("    %s: %zu cells byte-identical\n", test_case.file, cells);
    } else {
      std::printf("    %s: %zu/%zu cells differ\n", test_case.file, mismatches, cells);
    }
  }
}

// ---------------------------------------------------------------------------
// The biome stage: the parameter list, the six quantized climate coordinates
// per cell, and the selected biome, against the game's own fill.
// ---------------------------------------------------------------------------

void test_biome_parameters() {
  const size_t count = sizeof(kWgBiomeParameters) / sizeof(kWgBiomeParameters[0]);
  size_t rows = 0;
  for_each_row("biome_parameters.tsv", [&](const std::vector<std::string>& fields) {
    const size_t index = static_cast<size_t>(parse_int(fields[0]));
    CHECK(index < count);
    if (index >= count)
      return;
    const WgBiomeParameter& parameter = kWgBiomeParameters[index];
    // The dump's column order: index, then each parameter's min and max in
    // `parameterSpace`'s order (temperature, humidity, continentalness,
    // erosion, depth, weirdness), then the offset, then the biome.
    for (int d = 0; d < 6; ++d) {
      CHECK_EQ(parameter.min[d], parse_long(fields[1 + 2 * d]));
      CHECK_EQ(parameter.max[d], parse_long(fields[2 + 2 * d]));
    }
    CHECK_EQ(parameter.offset, parse_long(fields[13]));
    CHECK(std::strcmp(parameter.biome, fields[14].c_str()) == 0);
    ++rows;
  });
  CHECK_EQ(rows, count);
  report("biome parameter points", static_cast<int>(rows));
}

// A deterministic spread of targets for the search's own check: the fixture
// below only covers 3072 real queries, and the RTree has to be right off them
// too.
std::vector<WgClimateTarget> biome_search_targets() {
  WgRandom random(20261002);
  std::vector<WgClimateTarget> targets;
  targets.reserve(512);
  for (int i = 0; i < 512; ++i) {
    // `quantizeCoord`'s reach is about [-2, 2] * 10000, so sample wide enough
    // to land in the oceans, the coast and the inland bands alike.
    const auto draw = [&]() { return static_cast<int64_t>(random.next_int(40001)) - 20000; };
    targets.push_back(WgClimateTarget{draw(), draw(), draw(), draw(), draw(), draw()});
  }
  return targets;
}

void test_biome_search() {
  const size_t count = sizeof(kWgBiomeParameters) / sizeof(kWgBiomeParameters[0]);
  int checked = 0;
  for (const WgClimateTarget& target : biome_search_targets()) {
    // The tree's pick must be one of the globally closest rows; recomputing the
    // minimum fitness by linear scan is tie-order-independent, so this pins the
    // search itself rather than the fixture.
    const size_t picked = wg_select_biome_index(target);
    int64_t best = std::numeric_limits<int64_t>::max();
    for (size_t i = 0; i < count; ++i) {
      best = std::min(best, wg_biome_fitness(target, i));
    }
    CHECK_EQ(wg_biome_fitness(target, picked), best);
    CHECK(std::strcmp(wg_select_biome(target), kWgBiomeParameters[picked].biome) == 0);
    ++checked;
  }
  report("biome search targets", checked);
}

// The surface stage's biome conditions compare `const char*` identity, not the
// text; `wg_canonical_biome` is what makes that sound. Every preset name must
// intern to one pointer from any address (the table's own literal, a fresh
// `std::string`, the selection's answer), and a name the preset does not carry
// must stay itself rather than silently becoming some other biome.
void test_biome_identity() {
  const size_t count = sizeof(kWgBiomeParameters) / sizeof(kWgBiomeParameters[0]);
  std::vector<const char*> distinct;
  for (size_t i = 0; i < count; ++i) {
    const char* name = kWgBiomeParameters[i].biome;
    const char* canonical = wg_canonical_biome(name);
    CHECK(canonical != nullptr);
    CHECK(std::strcmp(canonical, name) == 0);
    // Idempotent: interning a canonical pointer answers the same pointer.
    CHECK(wg_canonical_biome(canonical) == canonical);
    if (std::find(distinct.begin(), distinct.end(), canonical) == distinct.end())
      distinct.push_back(canonical);
  }
  CHECK(!distinct.empty());

  // Equal text at a different address is the same biome. This is exactly the
  // comparison a surface rule makes: its literal vs the manager's answer.
  std::vector<std::string> copies;
  copies.reserve(distinct.size());
  for (const char* name : distinct)
    copies.emplace_back(name);
  for (size_t i = 0; i < distinct.size(); ++i)
    CHECK(wg_canonical_biome(copies[i].c_str()) == distinct[i]);

  // Selection answers with a canonical pointer for every target, so the rule
  // tree's `==` can never miss for a biome it names.
  for (const WgClimateTarget& target : biome_search_targets()) {
    const char* biome = wg_select_biome(target);
    CHECK(biome == wg_canonical_biome(biome));
  }

  static const char kVoidBiome[] = "minecraft:the_void";
  CHECK(wg_canonical_biome(kVoidBiome) == kVoidBiome);
  CHECK(wg_canonical_biome(nullptr) == nullptr);
  report("distinct biomes", static_cast<int>(distinct.size()));
}

struct BiomeChunkCase {
  const char* file;
  int32_t chunk_x;
  int32_t chunk_z;
};

void test_biome_chunks() {
  // `biome_meta.tsv` is the dump's own summary: every chunk's fill is 1536
  // cells, and the game's RTree chose the same biome a linear scan does on all
  // of them (the one place the two could legitimately differ is an exact tie).
  for_each_row("biome_meta.tsv", [&](const std::vector<std::string>& fields) {
    CHECK_EQ(parse_int(fields[5]), 1536);
    CHECK_EQ(parse_int(fields[6]), 0);
  });

  const BiomeChunkCase cases[] = {{"biomes_seed42_c0_0.tsv", 0, 0},
                                  {"biomes_seed42_c-1_-1.tsv", -1, -1}};
  WgOverworldGenerator generator(42);
  const uint64_t point_graphs_before = point_climate_graph_builds();
  for (const BiomeChunkCase& test_case : cases) {
    // The bulk path first (the array `fill_generated_chunk` copies from): one
    // bare graph answers the whole chunk in the chunk model's cell order.
    std::vector<const char*> chunk_names(static_cast<size_t>(kWgHeight / 16) *
                                         kBiomeCellsPerSection);
    generator.fill_biome_chunk(test_case.chunk_x, test_case.chunk_z, chunk_names.data());

    int checked = 0;
    size_t ties = 0;
    for_each_row(test_case.file, [&](const std::vector<std::string>& fields) {
      const int32_t quart_x = parse_int(fields[3]);
      const int32_t quart_y = parse_int(fields[4]);
      const int32_t quart_z = parse_int(fields[5]);
      const WgClimateTarget want{parse_long(fields[6]),  parse_long(fields[7]),
                                 parse_long(fields[8]),  parse_long(fields[9]),
                                 parse_long(fields[10]), parse_long(fields[11])};
      const WgClimateTarget actual = generator.climate_target_at_quart(quart_x, quart_y, quart_z);
      CHECK_EQ(actual.temperature, want.temperature);
      CHECK_EQ(actual.humidity, want.humidity);
      CHECK_EQ(actual.continentalness, want.continentalness);
      CHECK_EQ(actual.erosion, want.erosion);
      CHECK_EQ(actual.depth, want.depth);
      CHECK_EQ(actual.weirdness, want.weirdness);

      const std::string& biome = fields[12];
      if (biome != fields[13])
        ++ties;
      CHECK(std::string(wg_select_biome(actual)) == biome);
      // The bulk path's cell for the same quart position: section
      // `quart_y >> 2`, then `(y*4 + z)*4 + x` inside it.
      const size_t section = static_cast<size_t>((quart_y >> 2) - kWgMinY / 16);
      const size_t cell =
          (static_cast<size_t>(quart_y & 3) * 4 + static_cast<size_t>(quart_z & 3)) * 4 +
          static_cast<size_t>(quart_x & 3);
      CHECK(std::string(chunk_names[section * kBiomeCellsPerSection + cell]) == biome);
      // The single-cell wrapper `MemoryWorld` answers with, on every cell: the
      // point path must agree with the chunk's own fill at every quart cell,
      // including the y-dependent cave biomes, not just a sample of three.
      CHECK(std::string(generator.biome_at_quart(quart_x, quart_y, quart_z)) == biome);
      ++checked;
    });
    CHECK_EQ(checked, 1536);
    CHECK_EQ(ties, 0);
    report(test_case.file, checked);
  }
  // One marker-transparent graph per thread serves every point query above;
  // the per-query arm would build 3,072. A regression to a graph per call is
  // what this pins (values are pinned cell for cell above).
  CHECK(point_climate_graph_builds() - point_graphs_before <= 1);
}

// ---------------------------------------------------------------------------
// The surface stage: the classic simplex stack its conditions read, then the
// finished post-surface chunk, cell for cell, for twelve biome-shaped chunks.
// ---------------------------------------------------------------------------

void test_surface_noise() {
  // `Biome`'s three static legacy noises, built the way the surface rules'
  // own `legacy_noises()` builds them: seed, octave list, and the draw order
  // of `PerlinSimplexNoise`'s constructor.
  const int32_t temperature_octaves[] = {0};
  const int32_t frozen_octaves[] = {-2, -1, 0};
  const int32_t info_octaves[] = {0};
  WgPerlinSimplexNoise temperature;
  WgLegacyRandom temperature_random(1234);
  temperature.init(temperature_random, temperature_octaves, 1);
  WgPerlinSimplexNoise frozen;
  WgLegacyRandom frozen_random(3456);
  frozen.init(frozen_random, frozen_octaves, 3);
  WgPerlinSimplexNoise info;
  WgLegacyRandom info_random(2345);
  info.init(info_random, info_octaves, 1);

  int checked = 0;
  for_each_row("surface_noise.tsv", [&](const std::vector<std::string>& fields) {
    const double x = parse_double(fields[1]);
    const double z = parse_double(fields[2]);
    const double expected = parse_double(fields[3]);
    const WgPerlinSimplexNoise* noise = nullptr;
    if (fields[0] == "temperature")
      noise = &temperature;
    else if (fields[0] == "frozen")
      noise = &frozen;
    else if (fields[0] == "info")
      noise = &info;
    CHECK(noise != nullptr);
    if (noise == nullptr)
      return;
    const double actual = noise->get_value(x, z);
    CHECK(actual == expected);
    if (actual != expected) {
      std::fprintf(stderr, "    %s at (%a, %a): got %a want %a\n", fields[0].c_str(), x, z, actual,
                   expected);
    }
    ++checked;
  });
  CHECK_EQ(checked, 18);
  report("surface legacy noise", checked);
}

std::string canonical_state(const WgSurfaceState& state) {
  std::string text = state.name;
  if (state.properties[0] != '\0') {
    text += '[';
    text += state.properties;
    text += ']';
  }
  return text;
}

std::string canonical_state(const BlockStateKey& state) {
  std::string text = state.name;
  if (!state.properties.empty()) {
    text += '[';
    text += state.properties;
    text += ']';
  }
  return text;
}

// `surface_meta.tsv` / `noise_meta.tsv`'s `stateCodes` column: the file's own
// code -> canonical state name, assigned in first-appearance order so it only
// decodes its own dump.
std::vector<std::string> parse_state_codes(const std::string& text) {
  std::vector<std::string> names;
  size_t start = 0;
  while (true) {
    const size_t comma = text.find(',', start);
    const std::string pair =
        comma == std::string::npos ? text.substr(start) : text.substr(start, comma - start);
    const size_t equals = pair.find('=');
    const size_t code = static_cast<size_t>(parse_int(pair.substr(0, equals)));
    if (code >= names.size())
      names.resize(code + 1);
    names[code] = pair.substr(equals + 1);
    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }
  return names;
}

void test_surface_chunks() {
  // Each row of `surface_meta.tsv` names a chunk the probe dumped *after*
  // running the game's own surface pass, with its state-code map (assigned in
  // first-appearance order, so it only decodes its own file).
  int chunks = 0;
  size_t cells_total = 0;
  for_each_row("surface_meta.tsv", [&](const std::vector<std::string>& fields) {
    CHECK_EQ(parse_int(fields[0]), 42);
    const int32_t chunk_x = parse_int(fields[1]);
    const int32_t chunk_z = parse_int(fields[2]);
    CHECK_EQ(parse_int(fields[3]), kWgMinY);
    CHECK_EQ(parse_int(fields[4]), kWgHeight);
    const size_t cells = static_cast<size_t>(16) * 16 * kWgHeight;
    CHECK_EQ(parse_int(fields[5]), static_cast<int32_t>(cells));
    CHECK(fields[6] == kWgDefaultBlock);
    CHECK_EQ(parse_int(fields[7]), kWgSeaLevel);

    const std::vector<std::string> expected_names = parse_state_codes(fields[8]);

    const std::string file = std::string("surface_seed42_c") + std::to_string(chunk_x) + "_" +
                             std::to_string(chunk_z) + ".bin.gz";
    const std::vector<uint8_t> expected = read_gzip(file);
    CHECK_EQ(expected.size(), static_cast<long long>(cells));
    if (expected.size() != cells)
      return;

    std::vector<uint8_t> actual(cells, 0);
    WgOverworldGenerator generator(42);
    generator.fill_surface_chunk(chunk_x, chunk_z, actual.data());

    // Both sides are compared as canonical state names: the fixture numbers
    // its codes by first appearance, which is not the port's numbering (the
    // first ten codes agree, the rest do not).
    std::vector<std::string> actual_names(kWgSurfaceBlockCount);
    for (size_t code = 0; code < kWgSurfaceBlockCount; ++code) {
      actual_names[code] = canonical_state(wg_surface_state(static_cast<uint8_t>(code)));
    }

    size_t mismatches = 0;
    for (size_t i = 0; i < cells; ++i) {
      const uint8_t code = actual[i];
      const std::string actual_name =
          code < kWgSurfaceBlockCount ? actual_names[code] : std::string("<bad-code>");
      const std::string want = expected[i] < expected_names.size()
                                   ? expected_names[expected[i]]
                                   : std::string("<bad-fixture-code>");
      if (actual_name == want)
        continue;
      if (mismatches < 12) {
        const size_t y = i % kWgHeight;
        const size_t x = i / (kWgHeight * 16);
        const size_t z = (i / kWgHeight) % 16;
        std::fprintf(stderr, "FAIL %s (%zu, %lld, %zu): got %s want %s\n", file.c_str(), x,
                     static_cast<long long>(y) + kWgMinY, z, actual_name.c_str(), want.c_str());
      }
      ++mismatches;
      ++failures;
    }
    ++checks;
    ++chunks;
    cells_total += cells;
    report(file, static_cast<int>(cells));
    if (mismatches == 0) {
      std::printf("    %s: %zu cells state-identical\n", file.c_str(), cells);
    } else {
      std::printf("    %s: %zu/%zu cells differ\n", file.c_str(), mismatches, cells);
    }
  });
  CHECK_EQ(chunks, 12);
  std::printf("  surface chunks: %d chunks, %zu cells compared\n", chunks, cells_total);
}

// The post-carver column, against the game's own `applyCarvers` output. The
// fixture is the same chunk the surface fixture is cut from, so a difference
// here is the carve stage and nothing above it: the probe applies the surface
// pass first and then lets the server's own `CaveWorldCarver`/`CanyonWorldCarver`
// rewrite the cells.
void test_carved_chunks() {
  int chunks = 0;
  size_t cells_total = 0;
  for_each_row("carved_meta.tsv", [&](const std::vector<std::string>& fields) {
    CHECK_EQ(parse_int(fields[0]), 42);
    const int32_t chunk_x = parse_int(fields[1]);
    const int32_t chunk_z = parse_int(fields[2]);
    CHECK_EQ(parse_int(fields[3]), kWgMinY);
    CHECK_EQ(parse_int(fields[4]), kWgHeight);
    const size_t cells = static_cast<size_t>(16) * 16 * kWgHeight;
    CHECK_EQ(parse_int(fields[5]), static_cast<int32_t>(cells));
    CHECK(fields[6] == kWgDefaultBlock);
    CHECK_EQ(parse_int(fields[7]), kWgSeaLevel);

    const std::vector<std::string> expected_names = parse_state_codes(fields[8]);
    const std::string file = std::string("carved_seed42_c") + std::to_string(chunk_x) + "_" +
                             std::to_string(chunk_z) + ".bin.gz";
    const std::vector<uint8_t> expected = read_gzip(file);
    CHECK_EQ(expected.size(), static_cast<long long>(cells));
    if (expected.size() != cells)
      return;

    // A carved dump that says nothing happened is the failure this fixture has
    // already had once. `tools/gen_worldgen_dump.py` decodes the carvers with
    // the pack's static tags visible, but until it *commits* them
    // (`Registry.PendingTags.apply`) `CarverConfiguration.replaceable` is an
    // unbound `HolderSet.Named`: `WorldCarver.canReplaceBlock` matches nothing,
    // every carve reports success and changes no cell, and the "after carving"
    // dump is the surface dump under a second name. Pinning that the carve
    // stage did change cells is what makes the byte comparison below a test of
    // the carver rather than a test of `buildSurface` twice.
    const std::string surface_file = std::string("surface_seed42_c") + std::to_string(chunk_x) +
                                     "_" + std::to_string(chunk_z) + ".bin.gz";
    const std::vector<uint8_t> surface = read_gzip(surface_file);
    CHECK_EQ(surface.size(), static_cast<long long>(cells));
    if (surface.size() == cells) {
      std::vector<std::string> surface_names;
      for_each_row("surface_meta.tsv", [&](const std::vector<std::string>& row) {
        if (surface_names.empty() && parse_int(row[1]) == chunk_x && parse_int(row[2]) == chunk_z)
          surface_names = parse_state_codes(row[8]);
      });
      CHECK(!surface_names.empty());
      if (!surface_names.empty()) {
        size_t carved_cells = 0;
        for (size_t i = 0; i < cells; ++i) {
          if (surface[i] >= surface_names.size() || expected[i] >= expected_names.size()) {
            ++carved_cells; // an out-of-range code is a mismatch too
            continue;
          }
          if (surface_names[surface[i]] != expected_names[expected[i]])
            ++carved_cells;
        }
        CHECK(carved_cells > 0);
        std::printf("    %s: %zu cells rewritten by the carve stage\n", file.c_str(), carved_cells);
      }
    }

    std::vector<uint8_t> actual(cells, 0);
    WgOverworldGenerator generator(42);
    generator.fill_carved_chunk(chunk_x, chunk_z, actual.data());

    std::vector<std::string> actual_names(kWgSurfaceBlockCount);
    for (size_t code = 0; code < kWgSurfaceBlockCount; ++code) {
      actual_names[code] = canonical_state(wg_surface_state(static_cast<uint8_t>(code)));
    }

    size_t mismatches = 0;
    for (size_t i = 0; i < cells; ++i) {
      const uint8_t code = actual[i];
      const std::string actual_name =
          code < kWgSurfaceBlockCount ? actual_names[code] : std::string("<bad-code>");
      const std::string want = expected[i] < expected_names.size()
                                   ? expected_names[expected[i]]
                                   : std::string("<bad-fixture-code>");
      if (actual_name == want)
        continue;
      if (mismatches < 12) {
        const size_t y = i % kWgHeight;
        const size_t x = i / (kWgHeight * 16);
        const size_t z = (i / kWgHeight) % 16;
        std::fprintf(stderr, "FAIL %s (%zu, %lld, %zu): got %s want %s\n", file.c_str(), x,
                     static_cast<long long>(y) + kWgMinY, z, actual_name.c_str(), want.c_str());
      }
      ++mismatches;
      ++failures;
    }
    ++checks;
    ++chunks;
    cells_total += cells;
    report(file, static_cast<int>(cells));
    if (mismatches == 0) {
      std::printf("    %s: %zu cells state-identical after carving\n", file.c_str(), cells);
    } else {
      std::printf("    %s: %zu/%zu cells differ after carving\n", file.c_str(), mismatches, cells);
    }
  });
  CHECK_EQ(chunks, 2);
  std::printf("  carved chunks: %d chunks, %zu cells compared\n", chunks, cells_total);
}

// ---------------------------------------------------------------------------
// The bridge to the chunk model: the same noise stage, as a `Chunk` with
// palettes, biomes, heightmaps and ticking counts.
// ---------------------------------------------------------------------------

int wg_code_for_state(const BlockStateKey& key) {
  if (key.name == "minecraft:air")
    return 0;
  if (key.name == "minecraft:stone")
    return 1;
  if (key.name == "minecraft:water")
    return 2;
  if (key.name == "minecraft:lava")
    return 3;
  if (key.name == "minecraft:granite")
    return 4;
  if (key.name == "minecraft:copper_ore")
    return 5;
  if (key.name == "minecraft:raw_copper_block")
    return 6;
  if (key.name == "minecraft:tuff")
    return 7;
  if (key.name == "minecraft:deepslate_iron_ore")
    return 8;
  if (key.name == "minecraft:raw_iron_block")
    return 9;
  return -1;
}

void test_generated_chunk() {
  const ChunkCase cases[] = {{"noise_seed42_c0_0.bin.gz", 0, 0},
                             {"noise_seed42_c-1_-1.bin.gz", -1, -1}};
  WgOverworldGenerator generator(42);
  for (const ChunkCase& test_case : cases) {
    const std::vector<uint8_t> expected = read_gzip(test_case.file);
    if (expected.size() != static_cast<size_t>(16) * 16 * kWgHeight)
      continue;
    Chunk chunk;
    fill_generated_chunk(generator, test_case.chunk_x, test_case.chunk_z, chunk);
    CHECK_EQ(chunk.x, test_case.chunk_x);
    CHECK_EQ(chunk.z, test_case.chunk_z);
    CHECK_EQ(chunk.min_section_y, kWgMinY / 16);
    CHECK_EQ(chunk.sections.size(), static_cast<long long>(kWgHeight / 16));
    // The three client heightmaps were created by the bulk-write rebuild.
    CHECK_EQ(chunk.heightmaps.size(), 3);

    // Cell for cell: the chunk's blocks are the fixture's codes.  Both fixture
    // chunks carry only air, stone and water, so a state with no code here is
    // itself a failure (the bridge invented a state the stage cannot emit).
    size_t mismatches = 0;
    size_t unmapped = 0;
    for (int y = kWgMinY; y < kWgMinY + kWgHeight; ++y) {
      for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
          const int code = wg_code_for_state(chunk.block_state_at(x, y, z));
          if (code < 0) {
            ++unmapped;
            continue;
          }
          const uint8_t want =
              expected[(static_cast<size_t>(x) * 16 + z) * kWgHeight + (y - kWgMinY)];
          if (static_cast<uint8_t>(code) != want)
            ++mismatches;
        }
      }
    }
    CHECK_EQ(unmapped, 0);
    CHECK_EQ(mismatches, 0);

    // The ticking count is the write path's other derived store: water is a
    // randomly-ticking block (world/random_tick_table.h), and water is the only
    // non-stone code either fixture chunk carries, so each section's count is
    // exactly its water cells.
    for (int sy = 0; sy < static_cast<int>(chunk.sections.size()); ++sy) {
      const int min_y = (chunk.min_section_y + sy) * 16;
      int water_cells = 0;
      for (int y = min_y; y < min_y + 16; ++y)
        for (int z = 0; z < 16; ++z)
          for (int x = 0; x < 16; ++x)
            if (expected[(static_cast<size_t>(x) * 16 + z) * kWgHeight + (y - kWgMinY)] == 2)
              ++water_cells;
      CHECK_EQ(chunk.sections[static_cast<size_t>(sy)].random_tick_blocks, water_cells);
    }

    report(std::string("generated ") + test_case.file, 16 * 16 * kWgHeight);
  }

  // The heightmaps a generated column carries are derived from its blocks, not
  // copied from the stage: chunk (0,0) is open ocean (water to y 62), chunk
  // (-1,-1)'s column (6,0) is stone to y 64.
  Chunk ocean;
  fill_generated_chunk(generator, 0, 0, ocean);
  CHECK_EQ(chunk_motion_blocking_height(ocean, 8, 8), 63);
  CHECK_EQ(chunk_world_surface_height(ocean, 8, 8), 63);
  CHECK(chunk_biome_at(ocean, 8, 70, 8) == std::string(generator.biome_at_quart(2, 17, 2)));
  Chunk land;
  fill_generated_chunk(generator, -1, -1, land);
  CHECK_EQ(chunk_motion_blocking_height(land, 6, 0), 65);
  CHECK_EQ(chunk_world_surface_height(land, 6, 0), 65);
  CHECK(land.block_state_at(6, 64, 0).name == "minecraft:stone");
  CHECK(chunk_biome_at(land, 6, 70, 0) == std::string(generator.biome_at_quart(-3, 17, -4)));
}

// ---------------------------------------------------------------------------
// The server's own column: the same dumps the stage-level tests just compared,
// but through the bridge into a `Chunk` — the exact bridge `MemoryWorld`
// fabricates a `--worldgen` column with, and the exact stage the encoder sends.
// Every cell must be the dump's state (name *and* properties), and every biome
// cell the biome source's own answer, so a palette or cell-ordering bug in the
// bridge cannot hide behind the stage-level comparison above.
//
// Run twice: once for the twelve post-surface dumps through
// `fill_generated_surface_chunk`, and once for the two post-carver dumps through
// `fill_generated_carved_chunk` (the column `--worldgen` actually serves).  The
// carved arm is the strongest parity the engine can claim; structures and
// decoration are still absent (docs/WORLDGEN_PARITY.md).
// ---------------------------------------------------------------------------

void test_generated_stage(const char* stage, const char* meta_file, int expected_chunks,
                          void (*fill)(const WgOverworldGenerator&, int, int, Chunk&)) {
  WgOverworldGenerator generator(42);
  int chunks = 0;
  size_t cells_total = 0;
  size_t biomes_total = 0;
  for_each_row(meta_file, [&](const std::vector<std::string>& fields) {
    const int32_t chunk_x = parse_int(fields[1]);
    const int32_t chunk_z = parse_int(fields[2]);
    const std::string file = std::string(stage) + "_seed42_c" + std::to_string(chunk_x) + "_" +
                             std::to_string(chunk_z) + ".bin.gz";
    const std::vector<uint8_t> expected = read_gzip(file);
    CHECK_EQ(expected.size(), static_cast<long long>(16) * 16 * kWgHeight);
    if (expected.size() != static_cast<size_t>(16) * 16 * kWgHeight)
      return;
    const std::vector<std::string> expected_names = parse_state_codes(fields[8]);

    Chunk chunk;
    fill(generator, chunk_x, chunk_z, chunk);
    CHECK_EQ(chunk.sections.size(), static_cast<long long>(kWgHeight / 16));
    CHECK_EQ(chunk.heightmaps.size(), 3);

    size_t mismatches = 0;
    for (int sy = 0; sy < static_cast<int>(chunk.sections.size()); ++sy) {
      const Section& section = chunk.sections[static_cast<size_t>(sy)];
      std::vector<std::string> actual_names;
      actual_names.reserve(section.block_palette.size());
      for (const BlockStateKey& key : section.block_palette)
        actual_names.push_back(canonical_state(key));
      const int min_y = (chunk.min_section_y + sy) * 16;
      for (int ly = 0; ly < 16; ++ly) {
        const int y = min_y + ly;
        for (int z = 0; z < 16; ++z) {
          for (int x = 0; x < 16; ++x) {
            const int32_t palette_index =
                section.block_cells[(static_cast<size_t>(ly) * 16 + z) * 16 + x];
            const uint8_t code = expected[(static_cast<size_t>(x) * 16 + z) * kWgHeight +
                                          static_cast<size_t>(y - kWgMinY)];
            const std::string want =
                code < expected_names.size() ? expected_names[code] : std::string("<bad-code>");
            const std::string got =
                palette_index >= 0 && static_cast<size_t>(palette_index) < actual_names.size()
                    ? actual_names[static_cast<size_t>(palette_index)]
                    : std::string("<bad-palette-index>");
            if (got == want)
              continue;
            if (mismatches < 12) {
              std::fprintf(stderr, "FAIL bridge %s (%d, %d, %d): got %s want %s\n", file.c_str(), x,
                           y, z, got.c_str(), want.c_str());
            }
            ++mismatches;
            ++failures;
          }
        }
      }
    }
    ++checks;

    // The biome half: every 4 x 4 x 4 cell of the chunk's biome container is
    // the biome source's own answer for that quart, as the game dumped it.
    std::vector<std::string> expected_biomes(static_cast<size_t>(kWgHeight / 4) * 16 * 16);
    const std::string biome_file = std::string("biomes_seed42_c") + std::to_string(chunk_x) + "_" +
                                   std::to_string(chunk_z) + ".tsv";
    for_each_row(biome_file, [&](const std::vector<std::string>& biome_fields) {
      const int32_t quart_x = parse_int(biome_fields[3]);
      const int32_t quart_y = parse_int(biome_fields[4]);
      const int32_t quart_z = parse_int(biome_fields[5]);
      const size_t index = (static_cast<size_t>(quart_y - kWgMinY / 4) * 16 +
                            static_cast<size_t>(quart_z - chunk_z * 4)) *
                               16 +
                           static_cast<size_t>(quart_x - chunk_x * 4);
      expected_biomes[index] = biome_fields[12];
    });
    size_t biome_mismatches = 0;
    for (int sy = 0; sy < static_cast<int>(chunk.sections.size()); ++sy) {
      const Section& section = chunk.sections[static_cast<size_t>(sy)];
      const int section_y = chunk.min_section_y + sy;
      for (int y = 0; y < 4; ++y) {
        for (int z = 0; z < 4; ++z) {
          for (int x = 0; x < 4; ++x) {
            const size_t cell = static_cast<size_t>((y * 4 + z) * 4 + x);
            const int32_t palette_index = section.biome_cells[cell];
            const size_t index = (static_cast<size_t>(section_y * 4 + y - kWgMinY / 4) * 16 +
                                  static_cast<size_t>(z)) *
                                     16 +
                                 static_cast<size_t>(x);
            const std::string want = expected_biomes[index];
            const std::string got = palette_index >= 0 && static_cast<size_t>(palette_index) <
                                                              section.biome_palette.size()
                                        ? section.biome_palette[static_cast<size_t>(palette_index)]
                                        : std::string("<bad-palette-index>");
            if (got == want)
              continue;
            if (biome_mismatches < 12) {
              std::fprintf(stderr, "FAIL bridge biome %s section %d (%d,%d,%d): got %s want %s\n",
                           file.c_str(), section_y, x, y, z, got.c_str(), want.c_str());
            }
            ++biome_mismatches;
            ++failures;
          }
        }
      }
    }
    ++checks;
    if (mismatches == 0 && biome_mismatches == 0) {
      std::printf("    %s: %zu blocks and 1536 biomes state-identical\n", file.c_str(),
                  static_cast<size_t>(16) * 16 * kWgHeight);
    } else {
      std::printf("    %s: %zu/%zu blocks and %zu/1536 biomes differ\n", file.c_str(), mismatches,
                  static_cast<size_t>(16) * 16 * kWgHeight, biome_mismatches);
    }
    ++chunks;
    cells_total += static_cast<size_t>(16) * 16 * kWgHeight;
    biomes_total += 1536;
  });
  CHECK_EQ(chunks, expected_chunks);
  std::printf("  generated %s chunks: %d chunks, %zu blocks and %zu biomes compared\n", stage,
              chunks, cells_total, biomes_total);
}

void test_generated_surface_chunk() {
  test_generated_stage("surface", "surface_meta.tsv", 12, fill_generated_surface_chunk);
}

void test_generated_carved_chunk() {
  // The stage `MemoryWorld::generate_chunk` serves. Its two fixture chunks are
  // the two the wire test sees, so a bridge bug that only shows up on the
  // carved column (a stale heightmap over a cave mouth, say) fails here first.
  test_generated_stage("carved", "carved_meta.tsv", 2, fill_generated_carved_chunk);

  // The bridge must not quietly degrade to the surface column: a carved cell
  // has to reach the chunk *as* its carved state, and a column whose carved top
  // is lower than the surface top has to report the carved height in the
  // derived store. (The stage test above already proves the codes differ; this
  // proves the bridge passed the carved array through rather than rebuilding
  // the surface column out of the same noise.)
  WgOverworldGenerator generator(42);
  const size_t cells = static_cast<size_t>(16) * 16 * kWgHeight;
  size_t state_checked = 0;
  size_t surface_moved = 0;
  for (const auto& chunk_pos : {std::pair<int, int>{0, 0}, std::pair<int, int>{-1, -1}}) {
    const int chunk_x = chunk_pos.first;
    const int chunk_z = chunk_pos.second;
    std::vector<uint8_t> carved_codes(cells);
    std::vector<uint8_t> surface_codes(cells);
    generator.fill_carved_chunk(chunk_x, chunk_z, carved_codes.data());
    generator.fill_surface_chunk(chunk_x, chunk_z, surface_codes.data());
    Chunk chunk;
    fill_generated_carved_chunk(generator, chunk_x, chunk_z, chunk);
    for (size_t i = 0; i < cells; ++i) {
      if (carved_codes[i] == surface_codes[i])
        continue;
      if (state_checked < 8) {
        const size_t y = i % kWgHeight;
        const size_t x = i / (kWgHeight * 16);
        const size_t z = (i / kWgHeight) % 16;
        CHECK(canonical_state(chunk.block_state_at(
                  static_cast<int>(x), static_cast<int>(y) + kWgMinY, static_cast<int>(z))) ==
              canonical_state(wg_surface_state(carved_codes[i])));
        ++state_checked;
      }
    }
    // The heightmap is checked on every column of both fixture chunks: a bridge
    // that rebuilt it from the surface array would look right on a chunk whose
    // carve stays underground, and only the (-1,-1) chunk has columns whose top
    // the carve moved (the check below requires that at least one exists).
    for (size_t col = 0; col < 256; ++col) {
      const size_t x = col / 16;
      const size_t z = col % 16;
      int carved_top = kWgMinY - 1;
      int surface_top = kWgMinY - 1;
      for (size_t y = 0; y < kWgHeight; ++y) {
        const size_t i = (x * 16 + z) * kWgHeight + y;
        if (wg_surface_state(carved_codes[i]).name != std::string("minecraft:air"))
          carved_top = static_cast<int>(y) + kWgMinY;
        if (wg_surface_state(surface_codes[i]).name != std::string("minecraft:air"))
          surface_top = static_cast<int>(y) + kWgMinY;
      }
      if (carved_top != surface_top)
        ++surface_moved;
      CHECK_EQ(
          chunk.height_at(HeightmapType::WorldSurface, static_cast<int>(x), static_cast<int>(z)),
          carved_top + 1);
    }
  }
  CHECK(state_checked > 0);
  CHECK(surface_moved > 0);
  std::printf("    carved bridge: %zu columns' surface top moved by the carve\n", surface_moved);
}

// ---------------------------------------------------------------------------
// The world store: an unstored position is terrain this world fabricates on
// demand, and a write lands on it.
// ---------------------------------------------------------------------------

void test_generated_world() {
  auto generator = std::make_shared<WgOverworldGenerator>(42);
  MemoryWorld world(-4, 24, generator);
  CHECK(world.generates_terrain());
  CHECK(world.has_chunk(0, 0)); // terrain is here whether or not it is stored
  CHECK(world.can_load_chunk(0, 0));

  // A load fabricates a generated column but does not store it; the read paths
  // keep the void mode's answers until a write (or the send path) stores the
  // column — which is what keeps a per-tick read like natural spawning's from
  // forcing a ~20 ms generation for every chunk in view.
  auto loaded = world.load_chunk(0, 0);
  CHECK(loaded.has_value());
  // The served column is the post-carver one: deepslate below y 0, not the
  // noise stage's bare stone, with the carvers' tunnels cut through it.
  CHECK(loaded->block_state_at(8, -60, 8).name == "minecraft:deepslate");
  CHECK_EQ(world.motion_blocking_height(8, 8), -64);
  CHECK(world.get_block_state(0, 0, 8, -60, 8).name == "minecraft:air");
  // The biome is the biome source's own answer for the cell and needs no
  // terrain at all.
  CHECK(world.biome_name_at(8, 70, 8) == std::string(generator->biome_at_quart(2, 17, 2)));

  // A write stores the column first.  The placement lands where it was asked
  // to, and the terrain around it — including cells a void column would have
  // answered air for — is still there.
  CHECK(world.set_block_state(0, 0, 8, 200, 8, BlockStateKey{"minecraft:torch", ""}));
  CHECK(world.get_block_state(0, 0, 8, 200, 8).name == "minecraft:torch");
  CHECK(world.get_block_state(0, 0, 8, -60, 8).name == "minecraft:deepslate");
  CHECK(world.get_block_state(0, 0, 8, 62, 8).name == "minecraft:water");
  CHECK_EQ(world.motion_blocking_height(8, 8), 63);

  // The mutation rides along with the generated column through a load.
  auto reloaded = world.load_chunk(0, 0);
  CHECK(reloaded.has_value());
  CHECK(reloaded->block_state_at(8, 200, 8).name == "minecraft:torch");
  CHECK(reloaded->block_state_at(8, -60, 8).name == "minecraft:deepslate");
  CHECK(reloaded->block_state_at(8, 62, 8).name == "minecraft:water");

  // modify_chunk stores the generated column too (the light ring's borrow), so
  // the light engine sees the same terrain the encoder will send.
  auto live = world.modify_chunk(-1, -1);
  CHECK(live.has_value());
  // The land fixture's column (6,0) is grass on dirt at y 64 after the
  // surface pass — the block a vanilla client sees there.
  CHECK(live->get().block_state_at(6, 64, 0).name == "minecraft:grass_block");
  CHECK(live->get().block_state_at(6, 63, 0).name == "minecraft:dirt");
  CHECK(world.has_chunk(-1, -1));
  CHECK(world.biome_name_at(-10, 70, -16) == std::string(generator->biome_at_quart(-3, 17, -4)));

  // The void mode is untouched: no generator, no terrain, no claim to hold a
  // column.
  MemoryWorld void_world;
  CHECK(!void_world.generates_terrain());
  CHECK(!void_world.has_chunk(9, 9));
  auto void_chunk = void_world.load_chunk(9, 9);
  CHECK(void_chunk.has_value());
  CHECK(void_chunk->block_state_at(0, 100, 0).name == "minecraft:air");
}

// ---------------------------------------------------------------------------
// The table's own shape, so a regeneration that drops a node cannot quietly
// shrink the coverage above.
// ---------------------------------------------------------------------------

void test_table_shape() {
  CHECK_EQ(sizeof(kWgDfs) / sizeof(kWgDfs[0]), 330);
  CHECK_EQ(sizeof(kWgRouter) / sizeof(kWgRouter[0]), 15);
  CHECK_EQ(kWgMinY, -64);
  CHECK_EQ(kWgHeight, 384);
  CHECK_EQ(kWgCellWidth, 4);
  CHECK_EQ(kWgCellHeight, 8);
  CHECK_EQ(kWgSeaLevel, 63);
  CHECK(kWgAquifersEnabled);
  CHECK(kWgOreVeinsEnabled);
  CHECK(!kWgLegacyRandomSource);
  CHECK(std::strcmp(kWgDefaultBlock, "minecraft:stone") == 0);
  CHECK(std::strcmp(kWgDefaultFluid, "minecraft:water") == 0);
  CHECK_EQ(kWgNoiseDensityRoot, 329);
  for (uint32_t node : kWgRouter) {
    CHECK(node < 330);
  }
  CHECK_EQ(sizeof(kWgBiomeParameters) / sizeof(kWgBiomeParameters[0]), 7594);
  CHECK(std::strcmp(kWgBiomeParameters[0].biome, "minecraft:mushroom_fields") == 0);
  // A `NoiseChunk` for chunk (0, 0) is the shape the fill loop expects.
  WgRandomState state(42);
  WgNoiseChunk chunk(state, 16 / kWgCellWidth, 0, 0);
  CHECK_EQ(chunk.cursor().cell_count_xz, 4);
  CHECK_EQ(chunk.cursor().cell_count_y, 48);
  CHECK_EQ(chunk.cursor().cell_noise_min_y, -8);
  CHECK_EQ(chunk.cursor().first_noise_x, 0);
  CHECK_EQ(chunk.cursor().first_noise_z, 0);
  CHECK_EQ(chunk.cursor().noise_size_xz, 4);
  CHECK(chunk.graph().interpolator_count() > 0);
  CHECK(chunk.graph().flat_cache_count() > 0);
  CHECK(chunk.graph().cell_cache_count() == 1);
  // `wrap_new` folds constants into the nodes that mention them. The wrapped
  // router is a real graph, so both forms must occur: an operand absorbed into
  // an add/mul/min/max, and at least one node that is constant outright (the
  // zero `blend_offset` multiplying into row 28's product). The digest tests
  // above are what promise the values did not move.
  CHECK(chunk.graph().folded_operands() > 0);
  CHECK(chunk.graph().folded_constants() > 0);
  report("density folded operands", static_cast<int>(chunk.graph().folded_operands()));
  report("density folded constants", static_cast<int>(chunk.graph().folded_constants()));

  // Row 28 is `mul(blend_offset, add(...))`; the blender is empty here, so the
  // folded product is zero at every point, exactly as the interpreted form was.
  const WgOverworldGenerator generator(42);
  CHECK(generator.node_value(28, 7, -20, 11) == 0.0);
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <fixture dir>\n", argv[0]);
    return 2;
  }
  g_fixtures = argv[1];

  test_random_primitives();
  test_positional();
  test_noise_samples();
  test_noise_null_amplitudes();
  test_blended_noise();
  test_router();
  test_named_functions();
  test_interpolated_density();
  test_noise_chunks();
  test_biome_parameters();
  test_biome_search();
  test_biome_identity();
  test_biome_chunks();
  test_surface_noise();
  test_surface_chunks();
  test_carved_chunks();
  test_generated_chunk();
  test_generated_surface_chunk();
  test_generated_carved_chunk();
  test_generated_world();
  test_table_shape();

  if (failures == 0) {
    std::printf("worldgen: all checks passed (%d checks)\n", checks);
  } else {
    std::printf("worldgen: %d of %d checks failed\n", failures, checks);
  }
  return failures == 0 ? 0 : 1;
}
