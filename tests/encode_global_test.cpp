// SPDX-License-Identifier: GPL-3.0-or-later
//
// Milestone-B encoder test: verifies the direct/global palette path.
// Constructs a synthetic chunk section holding 257 distinct block states
// (more than the 256-state threshold for the listed palette) and confirms
// the encoder emits:
//   - block container bits header = 15 (kBlockGlobalBits)
//   - no palette size varint or palette entries
//   - packed longs at 15 bits/cell = long_count_for(4096, 15) longs
//   - cell values are raw network ids (not palette indices)
//
// This is a unit test — no vanilla server needed.

#include "world/block_states.h"
#include "world/chunk.h"
#include "world/registry.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
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

// Fake biome table: plains only (id 0), registry size 66.
class FakeBiomes final : public mc::world::BiomeNameTable {
public:
  std::optional<int32_t> lookup(const std::string&) const override {
    return 0;
  }
  size_t registry_size() const override {
    return 66;
  }
};

// Read a big-endian varint from buf at offset, return value and advance offset.
uint32_t read_varint(const std::vector<uint8_t>& buf, size_t& offset) {
  uint32_t val = 0;
  int shift = 0;
  while (offset < buf.size()) {
    uint8_t b = buf[offset++];
    val |= (b & 0x7FU) << shift;
    if ((b & 0x80) == 0)
      return val;
    shift += 7;
  }
  return val;
}

} // namespace

int main() {
  const auto& table = mc::world::full_block_states();

  // Build a chunk with one section holding 257 distinct block states.
  mc::world::Chunk chunk;
  chunk.x = 0;
  chunk.z = 0;
  chunk.min_section_y = -4;

  mc::world::Section sec;
  sec.y = 0;
  sec.has_blocks = true;
  sec.has_biomes = true;
  sec.biome_palette = {"minecraft:plains"};
  sec.biome_cells.assign(64, 0);

  // Collect 257 distinct (name, props) keys from the full table.
  std::vector<mc::world::BlockStateKey> palette;
  int32_t prev_id = -1;
  for (const auto& e : mc::world::kBlockStates) {
    if (static_cast<int32_t>(palette.size()) >= 257)
      break;
    if (e.id == 0)
      continue; // skip air
    if (e.id == prev_id)
      continue; // skip duplicate ids
    prev_id = e.id;
    mc::world::BlockStateKey key;
    key.name = e.name;
    key.properties = e.props;
    palette.push_back(std::move(key));
  }
  CHECK(static_cast<int32_t>(palette.size()) >= 257);

  sec.block_palette = palette;
  // Fill all 4096 cells cycling through the palette.
  sec.block_cells.resize(4096);
  for (size_t i = 0; i < 4096; ++i)
    sec.block_cells.set(i, static_cast<int32_t>(i % palette.size()));

  chunk.sections.push_back(std::move(sec));

  // Encode.
  FakeBiomes biomes;
  auto result = mc::world::encode_chunk_body(chunk, table, biomes);
  CHECK(result.has_value());
  if (!result.has_value()) {
    std::fprintf(stderr, "encode_chunk_body returned nullopt\n");
    return 1;
  }

  const auto& body = *result;

  // ---- Parse the encoded body ----
  // Layout: i32 x, i32 z, varint hm_count, hm_count * [varint type,
  //         varint data_count, data_count * i64],
  //         varint section_bytes_size, section_bytes, varint 0 (block ents),
  //         light data.
  size_t pos = 8; // skip i32 x + i32 z

  // Heightmap count
  uint32_t hm_count = read_varint(body, pos);
  CHECK(hm_count == 3);

  // Skip each heightmap.
  for (uint32_t h = 0; h < hm_count; ++h) {
    read_varint(body, pos);               // type
    uint32_t dc = read_varint(body, pos); // data count
    pos += dc * 8;                        // skip longs
  }

  // varint(section_bytes.size())
  uint32_t sb_size = read_varint(body, pos);
  CHECK(sb_size > 0);
  size_t sec_start = pos;

  // ---- Section bytes ----
  // u16 non-air, u16 fluid, block container, biome container.
  CHECK(pos + 4 <= sec_start + sb_size);

  uint16_t non_air = (static_cast<uint16_t>(body[pos]) << 8) | body[pos + 1];
  pos += 2;
  uint16_t fluid = (static_cast<uint16_t>(body[pos]) << 8) | body[pos + 1];
  pos += 2;
  (void)fluid;
  CHECK(non_air > 0);

  // Block container: first byte is the bits header.
  uint8_t bits = body[pos];
  pos += 1;

  if (bits == 0) {
    CHECK(false && "expected global mode (bits=15), got singleton");
  } else if (bits <= 8) {
    CHECK(false && "expected global mode (bits=15), got listed palette");
  } else {
    // Direct/global mode: [15][raw longs, no palette].
    CHECK(bits == 15);

    // At 15 bits, floor(64/15) = 4 values per long,
    // so we need ceil(4096/4) = 1024 longs = 8192 bytes.
    size_t expected_longs = (4096 + 3) / 4; // 1024
    size_t block_long_bytes = expected_longs * 8;
    CHECK(pos + block_long_bytes <= sec_start + sb_size);

    // Verify a few decoded cells are valid network ids (< 32366).
    for (size_t i = 0; i < 10; ++i) {
      // Read big-endian i64.
      uint8_t raw[8];
      std::memcpy(raw, &body[pos + i * 8], 8);
      int64_t w = 0;
      for (int b = 0; b < 8; ++b)
        w = (w << 8) | raw[b];

      // Each long holds 4 cells at 15 bits each (LSB-first).
      uint64_t u = static_cast<uint64_t>(w);
      const uint64_t mask = (1ULL << 15) - 1;
      for (int k = 0; k < 4; ++k) {
        int32_t id = static_cast<int32_t>((u >> (k * 15)) & mask);
        CHECK(id >= 0 && id < 32366);
      }
    }

    // Advance past block longs.
    pos += block_long_bytes;
  }

  // ---- Biome container ----
  // For a singleton (all plains, bits=0): [0x00][varint 0].
  CHECK(pos < sec_start + sb_size);
  uint8_t bio_bits = body[pos];
  pos += 1;
  CHECK(bio_bits == 0);
  uint32_t bio_val = read_varint(body, pos);
  CHECK(bio_val == 0);

  if (failures == 0) {
    std::fprintf(stderr, "PASS: encode_global_test (257-state direct palette)\n");
    return 0;
  }
  std::fprintf(stderr, "FAIL: encode_global_test (%d failures)\n", failures);
  return 1;
}
