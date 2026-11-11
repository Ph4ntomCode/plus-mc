// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase-1 void world: generator for the clientbound "Chunk Data and Update
// Light" (level_chunk_with_light, play 0x2d) payload that lets a client leave
// the "Loading terrain" state and enter the world.
//
// The generated body is byte-identical to the chunk a real vanilla 26.2 void
// world sends for its spawn chunk (captured from a live server, see
// docs/PHASE1.md): three all-zero heightmaps, then 24 all-air sections for the
// vanilla overworld column (-64..320, 384 blocks), no block entities, and
// empty light data. Only the chunk coordinates vary per chunk.
#pragma once

#include <cstdint>
#include <vector>

namespace mc::server {

// Body of the chunk-data packet (everything after the packet id) for an
// all-air chunk at the given chunk coordinates. `biome_id` is the advertised
// biome registry index written into every section; the default is 26.3's
// minecraft:the_void index, and a translated client passes its own registry's
// index instead.
std::vector<uint8_t> make_air_chunk_body(int chunk_x, int chunk_z, int biome_id = 58);

} // namespace mc::server
