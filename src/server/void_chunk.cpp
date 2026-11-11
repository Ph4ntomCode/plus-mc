// SPDX-License-Identifier: GPL-3.0-or-later
#include "server/void_chunk.h"

#include <cstdint>
#include <vector>

#include "net/byte_buffer.h"

namespace mc::server {

namespace {

// Appends the vanilla 26.2 heightmap block for an all-air chunk: type + a
// prefixed long array whose length is 37 (vanilla ground truth for a 384-block
// column) holding all-zero heights. Zero heights mean "empty column", which an
// all-air chunk is.
void append_zero_heightmap(mc::net::Writer& out, uint32_t type) {
  out.varint(type);
  out.varint(37); // long-array length (matches vanilla 26.2 bytes)
  out.bytes(std::vector<uint8_t>(37 * 8, 0));
}

} // namespace

std::vector<uint8_t> make_air_chunk_body(int chunk_x, int chunk_z, int biome_id) {
  mc::net::Writer p;
  // Chunk X / Z are 32-bit ints on the wire (776 schema).
  p.u32(static_cast<uint32_t>(chunk_x));
  p.u32(static_cast<uint32_t>(chunk_z));

  // Heightmaps: MOTION_BLOCKING_NO_LEAVES(5), MOTION_BLOCKING(4),
  // WORLD_SURFACE(1). The order is immaterial to clients (they route each
  // entry by its type varint) but we use the same canonical order as the
  // chunk encoder (5,4,1) for consistency across all chunk paths.
  p.varint(3);
  append_zero_heightmap(p, 5);
  append_zero_heightmap(p, 4);
  append_zero_heightmap(p, 1);

  // Chunk data: 24 all-air sections for the vanilla overworld column
  // (min_y -64, 384 tall => 24 sections). Each section is:
  //   block count (0) + fluid count (0) + block states singleton (air, id 0)
  //   + biomes singleton (the_void, id 58 in the vanilla registry order we
  //   advertise in the configuration phase) — byte-identical to a live
  //   vanilla 26.2 void-world chunk. Each singleton container is
  //   [bits byte 0x00][value varint], so an air section in 26.3's the_void
  //   biome is 8 bytes ending in 0x3a (58). A translated client supplies its
  //   own registry's the_void index instead.
  mc::net::Writer section;
  section.u16(0); // non-air block count
  section.u16(0); // fluid count
  section.u8(0);  // block states: singleton
  section.varint(0);
  section.u8(0); // biomes: singleton
  section.varint(static_cast<uint32_t>(biome_id & 0x7FFFFFFF));
  const std::vector<uint8_t> air_section = std::move(section).take();
  std::vector<uint8_t> data;
  data.reserve(air_section.size() * 24);
  for (int i = 0; i < 24; ++i)
    data.insert(data.end(), air_section.begin(), air_section.end());
  p.varint(static_cast<uint32_t>(data.size()));
  p.bytes(data);

  p.varint(0); // no block entities
  // Light: all four masks empty and no arrays (matches live vanilla void
  // chunks — a fully air column has no light-engine sections at all).
  p.varint(0);
  p.varint(0);
  p.varint(0);
  p.varint(0);
  p.varint(0); // sky light arrays
  p.varint(0); // block light arrays
  return std::move(p).take();
}

} // namespace mc::server
