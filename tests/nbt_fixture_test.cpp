// SPDX-License-Identifier: GPL-3.0-or-later
//
// Milestone-A fixture test for the NBT parser: reads chunk (0,0) straight out
// of the trimmed region file tests/fixtures/r.0.0.mca (exactly as a real 26.2
// region is laid out), decompresses the record, and asserts the parsed
// structure against ground truth captured from a live vanilla 26.2 server.
//
// Usage: nbt_fixture_test /path/to/tests/fixtures/r.0.0.mca

#include "world/nbt.h"

#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
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

// Extracts one chunk record from a region file: header entry for local chunk
// (0,0), then [u32 length][u8 compression][payload]. Returns the decompressed
// NBT payload (the fixture uses zlib compression id 2).
std::vector<uint8_t> read_chunk_nbt(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  CHECK(static_cast<bool>(f));
  std::vector<uint8_t> header(4096);
  f.read(reinterpret_cast<char*>(header.data()), 4096);
  CHECK(f.gcount() == 4096);
  const uint32_t entry =
      (static_cast<uint32_t>(header[0]) << 24) | (header[1] << 16) | (header[2] << 8) | header[3];
  const uint32_t sector = entry >> 8;
  const uint32_t count = entry & 0xFF;
  CHECK(sector != 0 && count != 0); // chunk (0,0) must be present
  f.seekg(static_cast<std::streamoff>(sector) * 4096);
  uint8_t lenb[4] = {};
  f.read(reinterpret_cast<char*>(lenb), 4);
  const uint32_t length =
      (static_cast<uint32_t>(lenb[0]) << 24) | (lenb[1] << 16) | (lenb[2] << 8) | lenb[3];
  uint8_t compression = 0;
  f.read(reinterpret_cast<char*>(&compression), 1);
  std::vector<uint8_t> body(length - 1);
  f.read(reinterpret_cast<char*>(body.data()), static_cast<std::streamsize>(body.size()));
  CHECK(f.gcount() == static_cast<std::streamsize>(body.size()));

  if (compression == 2) { // zlib
    z_stream zs{};
    CHECK(inflateInit(&zs) == Z_OK);
    std::vector<uint8_t> out;
    zs.next_in = body.data();
    zs.avail_in = static_cast<uInt>(body.size());
    std::vector<uint8_t> chunk(16384);
    for (;;) {
      zs.next_out = chunk.data();
      zs.avail_out = static_cast<uInt>(chunk.size());
      const int rc = inflate(&zs, Z_NO_FLUSH);
      const size_t produced = chunk.size() - zs.avail_out;
      out.insert(out.end(), chunk.data(), chunk.data() + produced);
      if (rc == Z_STREAM_END)
        break;
      if (rc != Z_OK) {
        CHECK(false); // corrupt zlib stream
        break;
      }
    }
    inflateEnd(&zs);
    return out;
  }
  // compression 1 = gzip, 3 = none; the fixture only ever uses zlib.
  CHECK(compression == 2);
  return {};
}

namespace nbt = mc::world::nbt;

void check_chunk(const std::vector<uint8_t>& raw) {
  auto root = nbt::parse(raw.data(), raw.size());
  CHECK(root.has_value());
  if (!root)
    return;

  // Root scalars.
  const auto* status = nbt::child(*root, "Status");
  CHECK(status != nullptr);
  if (status) {
    auto s = nbt::as_string(*status);
    CHECK(s && *s == "minecraft:full");
  }
  auto x = nbt::as_int(*nbt::child(*root, "xPos"));
  auto z = nbt::as_int(*nbt::child(*root, "zPos"));
  auto y = nbt::as_int(*nbt::child(*root, "yPos"));
  CHECK(x && *x == 0);
  CHECK(z && *z == 0);
  CHECK(y && *y == -4); // bottom section of the -64..320 overworld column

  // Heightmaps: four maps on disk, each 37 longs (9 bits x 256 columns).
  const auto* hm = nbt::child(*root, "Heightmaps");
  CHECK(hm != nullptr);
  if (hm) {
    for (const char* name :
         {"WORLD_SURFACE", "MOTION_BLOCKING", "MOTION_BLOCKING_NO_LEAVES", "OCEAN_FLOOR"}) {
      const auto* map = nbt::child(*hm, name);
      CHECK(map != nullptr);
      const auto* longs = nbt::as_longs(*map);
      CHECK(longs != nullptr && longs->size() == 37);
    }
  }

  // Sections: 24 compounds, Y -4..19.
  const auto* sections = nbt::child(*root, "sections");
  CHECK(sections != nullptr && sections->type == nbt::Tag::List);
  CHECK(sections->items.size() == 24);

  const auto& bottom = sections->items[0];
  auto y0 = nbt::as_int(*nbt::child(bottom, "Y"));
  CHECK(y0 && *y0 == -4);

  // block_states palette of the terrain section: bedrock/dirt/grass/air in
  // disk order, grass carrying Properties.snowy = "false".
  const auto* bs = nbt::child(bottom, "block_states");
  CHECK(bs != nullptr);
  if (bs) {
    const auto* palette = nbt::child(*bs, "palette");
    CHECK(palette != nullptr && palette->items.size() == 4);
    if (palette) {
      const char* expected[4] = {"minecraft:bedrock", "minecraft:dirt", "minecraft:grass_block",
                                 "minecraft:air"};
      for (size_t i = 0; i < 4; ++i) {
        const auto& entry = palette->items[i];
        auto name = nbt::as_string(*nbt::child(entry, "Name"));
        CHECK(name && *name == expected[i]);
      }
      const auto& grass = palette->items[2];
      const auto* props = nbt::child(grass, "Properties");
      CHECK(props != nullptr);
      auto snowy = nbt::as_string(*nbt::child(*props, "snowy"));
      CHECK(snowy && *snowy == "false");
    }
    const auto* data = nbt::child(*bs, "data");
    const auto* longs = nbt::as_longs(*data);
    CHECK(longs != nullptr && longs->size() == 256); // 4096 cells at 4 bits
  }

  // biomes: single-entry palette "minecraft:plains".
  const auto* biomes = nbt::child(bottom, "biomes");
  CHECK(biomes != nullptr);
  if (biomes) {
    const auto* palette = nbt::child(*biomes, "palette");
    CHECK(palette != nullptr && palette->items.size() == 1);
    if (palette) {
      auto bname = nbt::as_string(palette->items[0]);
      CHECK(bname && *bname == "minecraft:plains");
    }
  }

  // Light: SkyLight present at Y -4 and Y -3 (2048 bytes), absent at Y -2.
  const auto* sky0 = nbt::child(bottom, "SkyLight");
  const auto* sky0b = nbt::as_bytes(*sky0);
  CHECK(sky0b != nullptr && sky0b->size() == 2048);
  const auto& next = sections->items[1];
  const auto* sky1b = nbt::as_bytes(*nbt::child(next, "SkyLight"));
  CHECK(sky1b != nullptr && sky1b->size() == 2048);
  const auto& above = sections->items[2];
  CHECK(nbt::child(above, "SkyLight") == nullptr);
}

void check_malformed() {
  const uint8_t empty[] = {};
  CHECK(!nbt::parse(empty, 0).has_value());
  const uint8_t garbage[] = {0xff, 0x00, 0x01, 0x02, 0x03};
  CHECK(!nbt::parse(garbage, sizeof(garbage)).has_value());
  // A valid root compound followed by trailing garbage must still parse (the
  // payload ends at the compound's end tag), but truncating it mid-section
  // must fail.
  const uint8_t only_end[] = {0x0a, 0x00, 0x00, 0x00}; // compound, empty name, end
  CHECK(nbt::parse(only_end, sizeof(only_end)).has_value());
  const uint8_t truncated[] = {0x0a, 0x00, 0x00, 0x08, 0x00, 0x03}; // cut mid-string
  CHECK(!nbt::parse(truncated, sizeof(truncated)).has_value());
  // Endless list claims are rejected by the preflight check.
  const uint8_t bad_list[] = {0x0a, 0x00, 0x00,              // root compound
                              0x09, 0x00, 0x01, 'x',         // list "x"
                              0x0a, 0x7f, 0xff, 0xff, 0xff}; // 2^31-1 compounds
  CHECK(!nbt::parse(bad_list, sizeof(bad_list)).has_value());
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <fixture .mca path>\n", argv[0]);
    return 2;
  }
  const std::vector<uint8_t> raw = read_chunk_nbt(argv[1]);
  check_chunk(raw);
  check_malformed();
  if (failures == 0) {
    std::printf("nbt fixture test: OK\n");
    return 0;
  }
  std::fprintf(stderr, "nbt fixture test: %d failure(s)\n", failures);
  return 1;
}
