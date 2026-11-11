// SPDX-License-Identifier: GPL-3.0-or-later
//
// Milestone-A.5 encoder test: loads chunk (-2,-1) of a real 26.3 overworld
// from the trimmed fixture region (tests/fixtures/ow3region/r.-1.-1.mca,
// seed-42 spawn area, amethyst-geode/lush-cave terrain), re-encodes it, and
// asserts the clientbound level_chunk_with_light body is byte-identical to
// the 26.3 encoding of the body a live vanilla server sent for the same chunk
// (tests/fixtures/ow3_chunk_n2m1_body.hex).
//
// Unlike the flat fixture (bits 0/4 containers only), this chunk exercises
// the SimpleBitStorage packing geometry that the contiguous-bit-stream
// implementation got wrong: 5-bit (342 longs, 12 values/long) and 6-bit
// (410 longs, 10 values/long) block containers, plus non-trivial non-air /
// fluid counts and multiple biomes per section.
//
// The reference hex has its three heightmaps re-ordered to our canonical
// (5,4,1) emission order (vanilla's own order is a nondeterministic HashMap
// iteration); every other byte is strict.
//
// Usage: encode_ow3_test /path/to/tests/fixtures /path/to/ow3_chunk_n2m1_body.hex

#include "world/anvil.h"
#include "world/chunk.h"
#include "world/registry.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

std::string to_hex(const std::vector<uint8_t>& v) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(v.size() * 2);
  for (const uint8_t b : v) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0xF]);
  }
  return out;
}

// Biome registry prefix whose index positions match the real 26.2 wire ids
// for the two biomes this chunk uses: minecraft:dark_forest = 9 and
// minecraft:lush_caves = 31 (positions in the 26.3 advertised
// clientconfig/registries/worldgen_biome.txt; byte-verified by this test).
// Only those two are ever looked up.
std::vector<std::string> ow3_biome_order() {
  std::vector<std::string> names;
  for (int i = 0; i < 32; ++i)
    names.push_back("minecraft:placeholder_" + std::to_string(i));
  names[9] = "minecraft:dark_forest";
  names[31] = "minecraft:lush_caves";
  return names;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <fixtures dir> <reference .hex>\n", argv[0]);
    return 2;
  }
  const std::string region = std::string(argv[1]) + "/ow3region";
  auto world = mc::world::AnvilWorld::open_region_dir(region);
  CHECK(world != nullptr);
  if (!world)
    return 1;

  auto chunk = world->load_chunk(-2, -1);
  CHECK(chunk.has_value());
  if (!chunk)
    return 1;
  CHECK(chunk->x == -2 && chunk->z == -1);
  CHECK(chunk->sections.size() == 24);

  // The full 26.3 block-state table, the same one the runtime encoder uses.
  const mc::world::BlockStateTable& blocks = mc::world::full_block_states();
  int data_sections = 0;
  for (const auto& s : chunk->sections) {
    if (!s.has_blocks)
      continue;
    ++data_sections;
    // Every palette entry must resolve; a miss means the encode would fall
    // back to the void world for this chunk (the milestone-B table is not
    // complete yet).
    for (const auto& key : s.block_palette) {
      const auto id = blocks.lookup(key);
      if (!id) {
        std::fprintf(stderr, "FAIL: state (%s, %s) missing from the table\n", key.name.c_str(),
                     key.properties.c_str());
        ++failures;
      }
    }
  }
  CHECK(data_sections > 4); // bottom + surface + tree sections

  mc::world::NameListBiomeTable biomes(ow3_biome_order());
  auto body = mc::world::encode_chunk_body(*chunk, blocks, biomes);
  CHECK(body.has_value());
  if (!body)
    return 1;

  std::ifstream ref_file(argv[2]);
  std::string ref_hex((std::istreambuf_iterator<char>(ref_file)), std::istreambuf_iterator<char>());
  while (!ref_hex.empty() && (ref_hex.back() == '\n' || ref_hex.back() == '\r'))
    ref_hex.pop_back();
  const std::string encoded = to_hex(*body);
  if (encoded != ref_hex) {
    std::fprintf(stderr,
                 "FAIL: encoded body differs from the vanilla overworld "
                 "capture (encoded %zu vs reference %zu bytes)\n",
                 body->size(), ref_hex.size() / 2);
    ++failures;
  }

  if (failures == 0) {
    std::printf("encode overworld test: OK (%zu bytes byte-identical to vanilla)\n", body->size());
    return 0;
  }
  std::fprintf(stderr, "encode overworld test: %d failure(s)\n", failures);
  return 1;
}
