// SPDX-License-Identifier: GPL-3.0-or-later
//
// Milestone-A encoder test: loads chunk (0,0) from the trimmed fixture region
// (tests/fixtures/r.0.0.mca, byte-laid-out like a real 26.3 region),
// re-encodes it to the clientbound level_chunk_with_light body, and asserts
// the result is byte-identical to the body a live vanilla 26.3 server sent
// for the same chunk (tests/fixtures/chunk000_0_0_body.hex, extracted from
// chunk_capture_26_flat.json).
//
// The comparison is strict: our encoder emits heightmaps in the same order as
// the committed capture (5,4,1); vanilla's own order varies run-to-run, so a
// regenerated capture may need re-extracting the reference hex. Everything
// else (sections, container packing, light masks/arrays) is deterministic.
//
// Usage: encode_fixture_test /path/to/tests/fixtures /path/to/chunk000_0_0_body.hex

#include "world/anvil.h"
#include "world/chunk.h"
#include "world/registry.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
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

// The biome registry the fixture chunk's single biome (plains) needs: the
// advertised worldgen/biome order has minecraft:plains at index 41 (verified
// against data/vanilla/26.3/clientconfig/registries/worldgen_biome.txt; the
// fixture capture's biome singleton is exactly 41). Names before it are never
// looked up — only the index matters to the encoder.
std::vector<std::string> fixture_biome_order() {
  std::vector<std::string> names;
  for (int i = 0; i < 41; ++i)
    names.push_back("minecraft:placeholder_" + std::to_string(i));
  names.push_back("minecraft:plains");
  return names;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <region dir> <reference .hex>\n", argv[0]);
    return 2;
  }
  auto world = mc::world::AnvilWorld::open_region_dir(argv[1]);
  CHECK(world != nullptr);
  if (!world)
    return 1;

  auto chunk = world->load_chunk(0, 0);
  CHECK(chunk.has_value());
  if (!chunk)
    return 1;
  CHECK(chunk->x == 0 && chunk->z == 0);
  CHECK(chunk->sections.size() == 24);

  // The terrain section (Y -4) holds 256 bedrock + 512 dirt + 256 grass.
  const mc::world::BlockStateTable& blocks = mc::world::milestone_a_block_states();
  const auto& s0 = chunk->sections[0];
  CHECK(s0.has_blocks);
  int non_air = 0;
  bool has_grass = false, has_dirt = false, has_bedrock = false;
  for (std::size_t i = 0; i < s0.block_cells.size(); ++i) {
    const int32_t cell = s0.block_cells[i];
    if (static_cast<size_t>(cell) >= s0.block_palette.size()) {
      CHECK(false);
      break;
    }
    const auto id = blocks.lookup(s0.block_palette[static_cast<size_t>(cell)]);
    CHECK(id.has_value());
    if (!id)
      return 1;
    if (*id == 9)
      has_grass = true;
    if (*id == 10)
      has_dirt = true;
    if (*id == 88)
      has_bedrock = true;
    if (*id != 0)
      ++non_air;
  }
  CHECK(non_air == 1024);
  CHECK(has_grass && has_dirt && has_bedrock);

  // Encoder output must equal the captured vanilla bytes.
  mc::world::NameListBiomeTable biomes(fixture_biome_order());
  auto body = mc::world::encode_chunk_body(*chunk, blocks, biomes);
  CHECK(body.has_value());
  if (!body)
    return 1;

  std::ifstream ref_file(argv[2]);
  std::string ref_hex((std::istreambuf_iterator<char>(ref_file)), std::istreambuf_iterator<char>());
  // Trim trailing whitespace.
  while (!ref_hex.empty() && (ref_hex.back() == '\n' || ref_hex.back() == '\r'))
    ref_hex.pop_back();
  const std::string encoded = to_hex(*body);
  CHECK(encoded.size() == ref_hex.size());
  if (encoded != ref_hex) {
    std::fprintf(stderr, "FAIL: encoded body differs from the vanilla capture\n");
    std::size_t mismatch = 0;
    while (mismatch < encoded.size() && mismatch < ref_hex.size() &&
           encoded[mismatch] == ref_hex[mismatch])
      ++mismatch;
    const std::size_t start = mismatch > 24 ? mismatch - 24 : 0;
    std::fprintf(stderr, "  first mismatch at hex offset %zu\n", mismatch);
    std::fprintf(stderr, "  encoded: %.48s\n", encoded.substr(start, 48).c_str());
    std::fprintf(stderr, "  capture: %.48s\n", ref_hex.substr(start, 48).c_str());
    std::fprintf(stderr, "  encoded (%zu bytes): %.80s...\n", body->size(), encoded.c_str());
    std::fprintf(stderr, "  capture (%zu bytes): %.80s...\n", ref_hex.size() / 2, ref_hex.c_str());
    ++failures;
  }

  // Heightmap entries must be 37 longs each on the wire, like the capture.
  // (Structural check only; byte equality above already covers content.)
  if (body->size() != ref_hex.size() / 2)
    std::fprintf(stderr, "  sizes: encoded %zu vs capture %zu\n", body->size(), ref_hex.size() / 2);

  if (failures == 0) {
    std::printf("encode fixture test: OK (%zu bytes byte-identical to vanilla)\n", body->size());
    return 0;
  }
  std::fprintf(stderr, "encode fixture test: %d failure(s)\n", failures);
  return 1;
}
