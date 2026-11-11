// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 4's applied structure pieces. `worldgen_structure_piece.h` names the
// boundary; this file pins the parts the shipwreck templates cannot cover:
//
//   * each start's selection: the set's candidate gate and weighted pick, the
//     context random's draws, and the structure's own height/surface rule
//     (`SwampHutStructure.updateAverageGroundHeight`, the igloo's surface snap,
//     the pyramid's corner height, the mineshaft's room and recursive children);
//   * the orientation transform, which rotates and mirrors *coordinates*
//     (`StructurePiece.getWorldX`/`getWorldZ`) while the block states get their
//     own mirror/rotation pair — the two halves that are easy to swap;
//   * each writer's exact cells, pinned against the 26.2 server's own
//     `postProcess` run over a synthetic level (one probe per family under
//     `build/probes/`): one FNV-1a digest over the changed cells'
//     `name[props]@x,y,z` rows, plus block entities and spawn data where the
//     piece creates them.
//
// The hand-coded probes share the terrain `stone at y <= 64 + (x mod 3) +
// (z mod 2), air above`; the mineshaft one adds a 3D cave mask. Starts are
// checked without a server, a world, or a socket.
//
// Usage: worldgen_structure_test
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "world/chunk.h"
#include "world/container.h"
#include "world/generated_chunk.h"
#include "world/worldgen_biome.h"
#include "world/worldgen_chunk.h"
#include "world/worldgen_loot.h"
#include "world/worldgen_structure.h"
#include "world/worldgen_structure_piece.h"
#include "world/worldgen_structure_table.h"

using namespace mc::world;

namespace {

int failures = 0;
int checks = 0;

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

constexpr int32_t kMinY = kWgMinY;

int32_t floor_mod(int32_t value, int32_t mod) {
  const int32_t remainder = value % mod;
  return remainder < 0 ? remainder + mod : remainder;
}

int32_t floor_div(int32_t value, int32_t divisor) {
  const int32_t quotient = value / divisor;
  return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

// The probe's synthetic terrain: stone through `height - 1`, air above.
int32_t ground_height(int32_t world_x, int32_t world_z) {
  return 64 + floor_mod(world_x, 3) + floor_mod(world_z, 2);
}

// A default `Chunk` has no section stack, so `set_block_state` refuses every
// write. Build the same 24-section column the generator builds.
void reset_chunk(Chunk& chunk, int32_t chunk_x, int32_t chunk_z) {
  chunk.x = chunk_x;
  chunk.z = chunk_z;
  chunk.min_section_y = kWgMinY / 16;
  chunk.sections.assign(static_cast<size_t>(kWgHeight / 16), Section{});
  for (int32_t i = 0; i < kWgHeight / 16; ++i) {
    Section& section = chunk.sections[static_cast<size_t>(i)];
    section.y = chunk.min_section_y + i;
    section.has_blocks = true;
    section.block_palette = {BlockStateKey{"minecraft:air", ""}};
    section.block_cells.assign(kBlockCellsPerSection, 0);
  }
}

void fill_terrain(Chunk& chunk) {
  for (int32_t z = 0; z < 16; ++z) {
    for (int32_t x = 0; x < 16; ++x) {
      const int32_t ground = ground_height(chunk.x * 16 + x, chunk.z * 16 + z);
      for (int32_t y = 60; y <= ground; ++y)
        chunk.set_block_state(x, y, z, BlockStateKey{"minecraft:stone", ""});
    }
  }
}

uint64_t fnv1a_row(uint64_t hash, std::string_view row) {
  for (const char c : row) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 0x100000001b3ull;
  }
  hash ^= '\n';
  hash *= 0x100000001b3ull;
  return hash;
}

std::string cell_name(const BlockStateKey& state) {
  std::string row = state.name;
  if (!state.properties.empty()) {
    row.push_back('[');
    row += state.properties;
    row.push_back(']');
  }
  return row;
}

// Every cell whose state the writer changed, in the probe's sorted-row form.
// The scan is the piece's whole box at the test's ground height (65).
struct Dump {
  std::vector<std::string> rows;
  uint64_t digest = 0xcbf29ce484222325ull;
};

Dump dump_changes(const Chunk& chunk, const WgSwampHutStart& start) {
  Dump dump;
  const int32_t min_x = start.chunk_x * 16;
  const int32_t min_z = start.chunk_z * 16;
  for (int32_t world_y = start.ground_y; world_y < start.ground_y + 8; ++world_y) {
    for (int32_t world_z = min_z; world_z < min_z + 16; ++world_z) {
      for (int32_t world_x = min_x; world_x < min_x + 16; ++world_x) {
        const BlockStateKey now = chunk.block_state_at(world_x - min_x, world_y, world_z - min_z);
        const bool was_stone = world_y <= ground_height(world_x, world_z);
        const BlockStateKey before =
            was_stone ? BlockStateKey{"minecraft:stone", ""} : BlockStateKey{"minecraft:air", ""};
        if (now == before)
          continue;
        std::string row = cell_name(now);
        row += '@';
        row += std::to_string(world_x);
        row.push_back(',');
        row += std::to_string(world_y);
        row.push_back(',');
        row += std::to_string(world_z);
        dump.rows.push_back(std::move(row));
      }
    }
  }
  std::sort(dump.rows.begin(), dump.rows.end());
  for (const std::string& row : dump.rows)
    dump.digest = fnv1a_row(dump.digest, row);
  return dump;
}

// The synthetic heights every new kind's writer is pinned against: the same
// stone/air terrain the Java probes use, expressed as a `WgStructureHeights`
// first-free answer. `height` is block_ground + 1 for both heightmap types.
struct SyntheticHeights final : WgStructureHeights {
  int32_t height(int32_t block_x, int32_t block_z, WgStructureHeightmap) override {
    return ground_height(block_x, block_z) + 1;
  }
};

// Every cell in `chunk` whose state differs from the probe's initial terrain.
Dump dump_terrain_changes(const Chunk& chunk) {
  Dump dump;
  const int32_t min_x = chunk.x * 16;
  const int32_t min_z = chunk.z * 16;
  for (int32_t world_y = kMinY; world_y < kMinY + kWgHeight; ++world_y) {
    for (int32_t x = 0; x < 16; ++x) {
      for (int32_t z = 0; z < 16; ++z) {
        const BlockStateKey now = chunk.block_state_at(x, world_y, z);
        const bool was_stone = world_y <= ground_height(min_x + x, min_z + z);
        const BlockStateKey before =
            was_stone ? BlockStateKey{"minecraft:stone", ""} : BlockStateKey{"minecraft:air", ""};
        if (now == before)
          continue;
        std::string row = cell_name(now);
        row += '@';
        row += std::to_string(min_x + x);
        row.push_back(',');
        row += std::to_string(world_y);
        row.push_back(',');
        row += std::to_string(min_z + z);
        dump.rows.push_back(std::move(row));
      }
    }
  }
  std::sort(dump.rows.begin(), dump.rows.end());
  for (const std::string& row : dump.rows)
    dump.digest = fnv1a_row(dump.digest, row);
  return dump;
}

void fill_synthetic_terrain(Chunk& chunk) {
  for (int32_t x = 0; x < 16; ++x)
    for (int32_t z = 0; z < 16; ++z) {
      const int32_t ground = ground_height(chunk.x * 16 + x, chunk.z * 16 + z);
      for (int32_t y = kMinY; y <= ground; ++y)
        chunk.set_block_state(x, y, z, BlockStateKey{"minecraft:stone", ""});
    }
}

// The mineshaft probe's synthetic terrain: the same surface as the other
// probes, with a 3D cave mask so the corridor's floors, supports and chains
// each see both solid and empty neighbours.
bool cave_solid(int32_t world_x, int32_t world_y, int32_t world_z) {
  if (world_y > ground_height(world_x, world_z))
    return false;
  return floor_mod(13 * world_x + 7 * world_y + 17 * world_z, 23) >= 5;
}

void fill_cave_terrain(Chunk& chunk) {
  for (int32_t x = 0; x < 16; ++x)
    for (int32_t z = 0; z < 16; ++z)
      for (int32_t y = kMinY; y < kMinY + kWgHeight; ++y)
        if (cave_solid(chunk.x * 16 + x, y, chunk.z * 16 + z))
          chunk.set_block_state(x, y, z, BlockStateKey{"minecraft:stone", ""});
}

// The probe's `OCEAN_FLOOR_WG`: one past the highest solid cell of the
// synthetic cave column. The real generated heights are not this terrain's
// heights, so every writer parity check has to use this implementation; with
// `WgGeneratedHeights`, a mesa shaft at y 64..92 saw a real surface well above
// the synthetic one and grew support columns the server did not.
class CaveHeights final : public WgStructureHeights {
public:
  int32_t height(int32_t block_x, int32_t block_z, WgStructureHeightmap) override {
    for (int32_t y = kMinY + kWgHeight - 1; y >= kMinY; --y)
      if (cave_solid(block_x, y, block_z))
        return y + 1;
    return kMinY;
  }
};

Dump dump_cave_changes(const Chunk& chunk) {
  Dump dump;
  const int32_t min_x = chunk.x * 16;
  const int32_t min_z = chunk.z * 16;
  for (int32_t world_y = kMinY; world_y < kMinY + kWgHeight; ++world_y) {
    for (int32_t x = 0; x < 16; ++x) {
      for (int32_t z = 0; z < 16; ++z) {
        const BlockStateKey now = chunk.block_state_at(x, world_y, z);
        const BlockStateKey before = cave_solid(min_x + x, world_y, min_z + z)
                                         ? BlockStateKey{"minecraft:stone", ""}
                                         : BlockStateKey{"minecraft:air", ""};
        if (now == before)
          continue;
        std::string row = cell_name(now);
        row += '@';
        row += std::to_string(min_x + x);
        row.push_back(',');
        row += std::to_string(world_y);
        row.push_back(',');
        row += std::to_string(min_z + z);
        dump.rows.push_back(std::move(row));
      }
    }
  }
  std::sort(dump.rows.begin(), dump.rows.end());
  for (const std::string& row : dump.rows)
    dump.digest = fnv1a_row(dump.digest, row);
  return dump;
}

WgSwampHutStart make_start(uint8_t direction) {
  WgSwampHutStart start;
  start.valid = true;
  start.direction = direction;
  start.chunk_x = 2;
  start.chunk_z = 3;
  start.x = 32;
  start.y = 64;
  start.z = 48;
  start.min_y = 64;
  start.max_y = 70;
  start.ground_y = 65; // the probe's average at chunk (2, 3)
  if (direction == 0 || direction == 2) {
    start.min_x = 32;
    start.max_x = 38;
    start.min_z = 48;
    start.max_z = 56;
  } else {
    start.min_x = 32;
    start.max_x = 40;
    start.min_z = 48;
    start.max_z = 54;
  }
  return start;
}

void test_writer_directions() {
  // Pinned from `build/probes/SwampHutPostProbe.java`: 136 writes per
  // orientation, 133 of them changing the synthetic terrain.
  struct Pin {
    uint8_t direction;
    const char* name;
    uint64_t digest;
  };
  static const Pin kPins[] = {
      {0, "north", 17123989085844517353ull},
      {1, "east", 14095389843895720590ull},
      {2, "south", 3850610672176283463ull},
      {3, "west", 3723906832112027808ull},
  };
  for (const Pin& pin : kPins) {
    Chunk chunk;
    reset_chunk(chunk, 2, 3);
    fill_terrain(chunk);
    const WgSwampHutStart start = make_start(pin.direction);
    CHECK_EQ(wg_place_swamp_hut(chunk, start), 133u);
    const Dump dump = dump_changes(chunk, start);
    if (dump.digest != pin.digest) {
      std::fprintf(stderr, "  %s (%s): %zu changed rows, digest %llu\n", pin.name, pin.name,
                   dump.rows.size(), static_cast<unsigned long long>(dump.digest));
    }
    CHECK_EQ(dump.rows.size(), 133u);
    CHECK_EQ(dump.digest, pin.digest);

    // The same piece applied twice is a no-op: every write lands on the state
    // it already placed.
    Chunk again = chunk;
    CHECK_EQ(wg_place_swamp_hut(again, start), 0u);
  }

  // One state row separately, so a digest mismatch has a place to point. The
  // probe's south piece puts `facing=south,shape=outer_left` at world
  // (32, 69, 49): the local north-facing outer-right corner was mirrored across
  // Z, which is the half a rotation-only writer gets wrong.
  Chunk chunk;
  reset_chunk(chunk, 2, 3);
  fill_terrain(chunk);
  const WgSwampHutStart south = make_start(2);
  wg_place_swamp_hut(chunk, south);
  const BlockStateKey outer = chunk.block_state_at(0, 69, 1);
  CHECK(outer.name == "minecraft:spruce_stairs");
  CHECK(outer.properties == "facing=south,half=bottom,shape=outer_left,waterlogged=false");
}

void test_starts() {
  WgOverworldGenerator generator(42);
  WgGeneratedHeights heights(generator);
  WgClimateSampler sampler(generator.state());

  // Two candidate chunks of the swamp-hut set and their server-generated
  // pieces: (direction, centre-column surface height, average ground height,
  // box). Read from `build/probes/SwampHutProbe.java`, which applies the
  // random-spread gate before generating.
  struct Pin {
    int32_t chunk_x;
    int32_t chunk_z;
    uint8_t direction;
    int32_t center_y;
    int32_t ground_y;
    int32_t min_x;
    int32_t min_z;
    int32_t max_x;
    int32_t max_z;
  };
  static const Pin kPins[] = {
      {-51, -23, 1, 62, 65, -816, -368, -808, -362},
      {-17, 22, 1, 62, 64, -272, 352, -264, 358},
  };
  for (const Pin& pin : kPins) {
    const WgSwampHutStart start =
        wg_swamp_hut_start(42, pin.chunk_x, pin.chunk_z, sampler, heights);
    CHECK(start.valid);
    if (!start.valid)
      continue;
    CHECK_EQ(start.direction, pin.direction);
    CHECK_EQ(start.ground_y, pin.ground_y);
    CHECK_EQ(start.x, pin.chunk_x * 16);
    CHECK_EQ(start.y, 64);
    CHECK_EQ(start.z, pin.chunk_z * 16);
    CHECK_EQ(start.min_x, pin.min_x);
    CHECK_EQ(start.min_y, 64);
    CHECK_EQ(start.min_z, pin.min_z);
    CHECK_EQ(start.max_x, pin.max_x);
    CHECK_EQ(start.max_y, 70);
    CHECK_EQ(start.max_z, pin.max_z);
    CHECK_EQ(heights.height(start.x + 8, start.z + 8, WgStructureHeightmap::kWorldSurface) - 1,
             pin.center_y);
  }

  // A chunk the set does not name is not a start, even if a swamp biome is
  // there; the gate runs before the biome probe.
  const WgStructureSet* set = wg_structure_set_by_name("minecraft:swamp_huts");
  CHECK(set != nullptr);
  if (set != nullptr) {
    CHECK(!wg_structure_is_chunk(42, *set, 0, 0));
  }
  const WgSwampHutStart none = wg_swamp_hut_start(42, 0, 0, sampler, heights);
  CHECK(!none.valid);
}

void test_igloo_and_buried_treasure() {
  WgOverworldGenerator generator(42);
  WgClimateSampler sampler(generator.state());
  WgGeneratedHeights heights(generator);

  // `IglooBuriedStartProbe.java` over the 26.2 server: the set gate, the
  // rotation draw, the 50% laboratory draw, its depth, and every piece's
  // template position and pivoted box.
  struct IglooPin {
    int32_t chunk_x;
    int32_t chunk_z;
    uint32_t rotation;
    uint32_t piece_count;
    int32_t bottom_x, bottom_y, bottom_z;
    int32_t bottom_max_x, bottom_max_y, bottom_max_z;
    int32_t top_x, top_y, top_z;
    int32_t top_min_x, top_min_z;
    int32_t top_max_x, top_max_y, top_max_z;
  };
  static const IglooPin kIgloos[] = {
      {208, -305, 0, 12, 3328, 54, -4882, 3334, 59, -4874, 3328, 90, -4880, 3328, -4880, 3334, 94,
       -4873},
      {428, -84, 0, 12, 6848, 54, -1346, 6854, 59, -1338, 6848, 90, -1344, 6848, -1344, 6854, 94,
       -1337},
      {336, -55, 2, 5, 5376, 75, -876, 5382, 80, -868, 5376, 90, -880, 5376, -877, 5382, 94, -870},
      {367, -55, 2, 6, 5872, 72, -876, 5878, 77, -868, 5872, 90, -880, 5872, -877, 5878, 94, -870},
      {306, -26, 1, 1, 0, 0, 0, 0, 0, 0, 4896, 90, -416, 4897, -414, 4904, 94, -408},
      {450, -26, 3, 1, 0, 0, 0, 0, 0, 0, 7200, 90, -416, 7198, -414, 7205, 94, -408},
  };
  for (const IglooPin& pin : kIgloos) {
    const WgIglooStart start = wg_igloo_start(42, pin.chunk_x, pin.chunk_z, sampler, heights);
    CHECK(start.valid);
    if (!start.valid)
      continue;
    CHECK_EQ(start.rotation, pin.rotation);
    CHECK_EQ(start.piece_count, pin.piece_count);
    if (start.piece_count == 0)
      continue;
    const WgIglooPiece& bottom = start.pieces[0];
    if (pin.piece_count > 1) {
      const WgPieceBox box =
          wg_piece_box(*bottom.templ, bottom.rotation, bottom.x, bottom.y, bottom.z);
      CHECK_EQ(box.min_x, pin.bottom_x);
      CHECK_EQ(box.min_y, pin.bottom_y);
      CHECK_EQ(box.min_z, pin.bottom_z);
      CHECK_EQ(box.max_x, pin.bottom_max_x);
      CHECK_EQ(box.max_y, pin.bottom_max_y);
      CHECK_EQ(box.max_z, pin.bottom_max_z);
    }
    const WgIglooPiece& top = start.pieces[start.piece_count - 1];
    CHECK_EQ(top.x, pin.top_x);
    CHECK_EQ(top.y, pin.top_y);
    CHECK_EQ(top.z, pin.top_z);
    const WgPieceBox top_box = wg_piece_box(*top.templ, top.rotation, top.x, top.y, top.z);
    CHECK_EQ(top_box.min_x, pin.top_min_x);
    CHECK_EQ(top_box.min_y, pin.top_y);
    CHECK_EQ(top_box.min_z, pin.top_min_z);
    CHECK_EQ(top_box.max_x, pin.top_max_x);
    CHECK_EQ(top_box.max_y, pin.top_max_y);
    CHECK_EQ(top_box.max_z, pin.top_max_z);
  }

  // The writer against the synthetic terrain: the probe's per-chunk changed
  // rows and FNV-1a digest. The igloo spans chunks (208,-305) and (208,-306).
  const WgIglooStart igloo = wg_igloo_start(42, 208, -305, sampler, heights);
  CHECK(igloo.valid);
  SyntheticHeights synthetic;
  struct ChunkPin {
    int32_t chunk_z;
    uint32_t rows;
    uint64_t digest;
  };
  static const ChunkPin kChunks[] = {
      {-305, 468, 17888916926252852692ull},
      {-306, 35, 18249758762678837503ull},
  };
  for (const ChunkPin& pin : kChunks) {
    Chunk chunk;
    reset_chunk(chunk, 208, pin.chunk_z);
    fill_synthetic_terrain(chunk);
    const uint32_t placed = wg_place_igloo(chunk, igloo, synthetic);
    CHECK(placed > 0);
    const Dump dump = dump_terrain_changes(chunk);
    CHECK_EQ(dump.rows.size(), pin.rows);
    CHECK_EQ(dump.digest, pin.digest);
  }
  Chunk igloo_chunk;
  reset_chunk(igloo_chunk, 208, -305);
  fill_synthetic_terrain(igloo_chunk);
  wg_place_igloo(igloo_chunk, igloo, synthetic);
  CHECK_EQ(igloo_chunk.block_entities.size(), 1u);
  CHECK(igloo_chunk.block_entities.size() == 1u &&
        block_entity_matches(igloo_chunk.block_entities[0], 3329, 30, -4876));
  CHECK(igloo_chunk.block_state_at(1, 30, 4).name == "minecraft:chest");

  // `BuriedTreasure`: one chest at the chunk's (9, 9) column.
  struct BuriedPin {
    int32_t chunk_x;
    int32_t chunk_z;
    int32_t x;
    int32_t z;
  };
  static const BuriedPin kBuried[] = {
      {127, -197, 2041, -3143},
      {-36, -194, -567, -3095},
      {137, -192, 2201, -3063},
      {95, -189, 1529, -3015},
  };
  for (const BuriedPin& pin : kBuried) {
    const WgBuriedTreasureStart start =
        wg_buried_treasure_start(42, pin.chunk_x, pin.chunk_z, sampler, heights);
    CHECK(start.valid);
    if (!start.valid)
      continue;
    CHECK_EQ(start.x, pin.x);
    CHECK_EQ(start.z, pin.z);
  }

  Chunk buried_chunk;
  reset_chunk(buried_chunk, 127, -197);
  fill_synthetic_terrain(buried_chunk);
  const WgBuriedTreasureStart buried = wg_buried_treasure_start(42, 127, -197, sampler, heights);
  CHECK_EQ(wg_place_buried_treasure(buried_chunk, buried, synthetic), 5u);
  const Dump buried_dump = dump_terrain_changes(buried_chunk);
  CHECK_EQ(buried_dump.rows.size(), 5u);
  CHECK_EQ(buried_dump.digest, 3045940206773582410ull);
  CHECK_EQ(buried_chunk.block_entities.size(), 1u);
  CHECK(buried_chunk.block_entities.size() == 1u &&
        block_entity_matches(buried_chunk.block_entities[0], 2041, 67, -3143));
  CHECK(buried_chunk.block_state_at(9, 67, 9).name == "minecraft:chest");

  // The bridge's entry point runs the same writers after the feature stage.
  Chunk applied;
  fill_generated_surface_chunk(generator, 127, -197, applied);
  const WgStructureApplyStats stats = apply_generated_structures(generator, applied);
  CHECK(stats.starts >= 1u);
  CHECK(stats.pieces >= 1u);
  CHECK(!applied.block_entities.empty());
}

WgJungleTempleStart make_jungle_start(uint8_t direction) {
  WgJungleTempleStart start;
  start.valid = true;
  start.direction = direction;
  start.chunk_x = 2;
  start.chunk_z = 3;
  start.x = 32;
  start.y = 64;
  start.z = 48;
  start.ground_y = 65;
  start.min_y = 64;
  start.max_y = 73;
  if (direction == 0 || direction == 2) {
    start.min_x = 32;
    start.max_x = 43;
    start.min_z = 48;
    start.max_z = 62;
  } else {
    start.min_x = 32;
    start.max_x = 46;
    start.min_z = 48;
    start.max_z = 59;
  }
  return start;
}

void test_jungle_temple() {
  // Pinned from `build/probes/JungleTemplePostProbe.java`: the same synthetic
  // terrain as the swamp hut, 1746 writes per orientation, and the changed
  // rows' FNV-1a digest. The probe's post-process random is a
  // `LegacyRandomSource` seeded with `Mth.getSeed(32, 64, 48)`, which is what
  // the writer uses, so the selector's 40/60 cobble/mossy draw is comparable.
  struct Pin {
    uint8_t direction;
    const char* name;
    uint32_t changed;
    uint64_t digest;
  };
  static const Pin kPins[] = {
      {0, "north", 1462, 16709875502691359872ull},
      {1, "east", 1457, 5655474492452676383ull},
      {2, "south", 1462, 16926410650040627667ull},
      {3, "west", 1459, 6401896556214801053ull},
  };
  for (const Pin& pin : kPins) {
    Chunk chunk;
    reset_chunk(chunk, 2, 3);
    fill_synthetic_terrain(chunk);
    const WgJungleTempleStart start = make_jungle_start(pin.direction);
    const uint32_t placed = wg_place_jungle_temple(chunk, start);
    CHECK(placed > 0);
    const Dump dump = dump_terrain_changes(chunk);
    if (dump.digest != pin.digest) {
      std::fprintf(stderr, "  jungle %s: %zu changed rows, digest %llu\n", pin.name,
                   dump.rows.size(), static_cast<unsigned long long>(dump.digest));
    }
    CHECK_EQ(dump.rows.size(), pin.changed);
    CHECK_EQ(dump.digest, pin.digest);

    // The four containers: two `jungle_temple` chests and two dispenser
    // traps. A second application is a no-op, block entities included.
    CHECK_EQ(chunk.block_entities.size(), 4u);
    size_t chests = 0;
    size_t dispensers = 0;
    for (const nbt::Value& be : chunk.block_entities) {
      const nbt::Value* id = nbt::child(be, "id");
      if (id == nullptr)
        continue;
      const auto name = nbt::as_string(*id);
      if (!name)
        continue;
      if (*name == "minecraft:chest")
        ++chests;
      else if (*name == "minecraft:dispenser")
        ++dispensers;
    }
    CHECK_EQ(chests, 2u);
    CHECK_EQ(dispensers, 2u);
    // A second application leaves the final chunk byte-identical. The return
    // count is not zero: a cell can be written stone early and air later in
    // the same pass, and the second pass starts from the finished temple, so
    // those transient writes count even though the final state does not move.
    Chunk snapshot = chunk;
    (void)wg_place_jungle_temple(chunk, start);
    size_t second_mismatches = 0;
    for (int32_t y = kMinY; y < kMinY + kWgHeight; ++y)
      for (int32_t x = 0; x < 16; ++x)
        for (int32_t z = 0; z < 16; ++z)
          if (!(snapshot.block_state_at(x, y, z) == chunk.block_state_at(x, y, z)))
            ++second_mismatches;
    CHECK_EQ(second_mismatches, 0u);
    CHECK_EQ(chunk.block_entities.size(), 4u);
  }

  // One chest separately, so a digest mismatch has a place to point. The
  // north-facing piece's main chest is at (40, 62, 59) and faces north; the
  // hidden chest at (41, 62, 52) faces south after `reorient`.
  Chunk chunk;
  reset_chunk(chunk, 2, 3);
  fill_synthetic_terrain(chunk);
  wg_place_jungle_temple(chunk, make_jungle_start(0));
  CHECK(chunk.block_state_at(8, 62, 11).name == "minecraft:chest");
  CHECK(chunk.block_state_at(8, 62, 11).properties == "facing=north,type=single,waterlogged=false");
  CHECK(chunk.block_state_at(9, 62, 4).name == "minecraft:chest");
  CHECK(chunk.block_state_at(9, 62, 4).properties == "facing=south,type=single,waterlogged=false");

  // The start: seed 42's dense-jungle candidate at (-105, -502), read from
  // `build/probes/JungleTempleProbe.java`. `getLowestY` sees 98, the biome
  // probe sees the jungle at 101, and the 180-column average is 103.
  WgOverworldGenerator generator(42);
  WgGeneratedHeights heights(generator);
  WgClimateSampler sampler(generator.state());
  const WgJungleTempleStart start = wg_jungle_temple_start(42, -105, -502, sampler, heights);
  CHECK(start.valid);
  if (start.valid) {
    CHECK_EQ(start.direction, 0);
    CHECK_EQ(start.x, -1680);
    CHECK_EQ(start.y, 64);
    CHECK_EQ(start.z, -8032);
    CHECK_EQ(start.min_x, -1680);
    CHECK_EQ(start.max_x, -1669);
    CHECK_EQ(start.min_y, 64);
    CHECK_EQ(start.max_y, 73);
    CHECK_EQ(start.min_z, -8032);
    CHECK_EQ(start.max_z, -8018);
    CHECK_EQ(start.ground_y, 103);
    CHECK_EQ(heights.height(-1680 + 8, -8032 + 8, WgStructureHeightmap::kWorldSurface) - 1, 101);
  }

  // The bridge entry point applies it: one start, one piece, and the block
  // entities land in the chunk that owns the temple.
  Chunk applied;
  fill_generated_surface_chunk(generator, -105, -502, applied);
  const WgStructureApplyStats stats = apply_generated_structures(generator, applied);
  CHECK(stats.candidates >= 1u);
  CHECK(stats.starts >= 1u);
  CHECK(stats.pieces >= 1u);
  CHECK(stats.blocks > 0u);
  Chunk direct;
  fill_generated_surface_chunk(generator, -105, -502, direct);
  CHECK_EQ(wg_place_jungle_temple(direct, start), stats.blocks);
  CHECK(!direct.block_entities.empty());

  const WgStructureSet* set = wg_structure_set_by_name("minecraft:jungle_temples");
  CHECK(set != nullptr);
  if (set != nullptr)
    CHECK(!wg_structure_is_chunk(42, *set, 0, 0));
  const WgJungleTempleStart none = wg_jungle_temple_start(42, 0, 0, sampler, heights);
  CHECK(!none.valid);
}

WgDesertPyramidStart make_desert_start(uint8_t direction) {
  WgDesertPyramidStart start;
  start.valid = true;
  start.world_seed = 42;
  start.direction = direction;
  start.chunk_x = 2;
  start.chunk_z = 3;
  start.x = 32;
  start.y = 64;
  start.z = 48;
  start.ground_y = 64;
  start.min_x = 32;
  start.max_x = 52;
  start.min_y = 64;
  start.max_y = 78;
  start.min_z = 48;
  start.max_z = 68;
  return start;
}

void test_desert_pyramid() {
  // Pinned from `build/probes/DesertPyramidPostProbe.java`: the same synthetic
  // terrain as the other pieces, a 21 x 21 footprint that crosses four chunks,
  // and the per-chunk subset of the probe's finished 21 x 21 box. The probe's
  // post-process random is `LegacyRandomSource(Mth.getSeed(32, 64, 48))`, the
  // stream the port uses for the height draw, the four chest loot draws, the
  // cellar variant, and the collapsed-roof cells.
  struct ChunkPin {
    int32_t chunk_x;
    int32_t chunk_z;
    uint32_t rows;
    uint64_t digest;
  };
  struct Pin {
    uint8_t direction;
    const char* name;
    ChunkPin chunks[4];
  };
  static const Pin kPins[] = {
      {0,
       "north",
       {{2, 3, 2238, 16587110793659506833ull},
        {2, 4, 724, 10269214708873161846ull},
        {3, 3, 566, 12607416944152738789ull},
        {3, 4, 304, 5017707489813072755ull}}},
      {1,
       "east",
       {{2, 3, 2460, 10656628884926143492ull},
        {2, 4, 723, 10224609295496773500ull},
        {3, 3, 504, 6905495648255521325ull},
        {3, 4, 151, 12150951959134494486ull}}},
      {2,
       "south",
       {{2, 3, 2452, 5568005968218384279ull},
        {2, 4, 510, 11066511801960010514ull},
        {3, 3, 719, 1803690778660136865ull},
        {3, 4, 151, 17805066999390738988ull}}},
      {3,
       "west",
       {{2, 3, 2239, 15150595499401547449ull},
        {2, 4, 572, 8961543777731412214ull},
        {3, 3, 733, 3287899177054329375ull},
        {3, 4, 304, 4634849000054209532ull}}},
  };
  SyntheticHeights heights;
  for (const Pin& pin : kPins) {
    const WgDesertPyramidStart start = make_desert_start(pin.direction);
    uint32_t total = 0;
    for (const ChunkPin& chunk_pin : pin.chunks) {
      Chunk chunk;
      reset_chunk(chunk, chunk_pin.chunk_x, chunk_pin.chunk_z);
      fill_synthetic_terrain(chunk);
      total += wg_place_desert_pyramid(chunk, start, heights);
      const Dump dump = dump_terrain_changes(chunk);
      if (dump.digest != chunk_pin.digest)
        std::fprintf(stderr, "  desert %s chunk %d,%d: %zu rows, digest %llu\n", pin.name,
                     chunk_pin.chunk_x, chunk_pin.chunk_z, dump.rows.size(),
                     static_cast<unsigned long long>(dump.digest));
      CHECK_EQ(dump.rows.size(), chunk_pin.rows);
      CHECK_EQ(dump.digest, chunk_pin.digest);

      // A second application leaves the final chunk byte-identical and does
      // not duplicate block entities.
      Chunk snapshot = chunk;
      (void)wg_place_desert_pyramid(chunk, start, heights);
      size_t second_mismatches = 0;
      for (int32_t y = kMinY; y < kMinY + kWgHeight; ++y)
        for (int32_t x = 0; x < 16; ++x)
          for (int32_t z = 0; z < 16; ++z)
            if (!(snapshot.block_state_at(x, y, z) == chunk.block_state_at(x, y, z)))
              ++second_mismatches;
      CHECK_EQ(second_mismatches, 0u);
      CHECK_EQ(chunk.block_entities.size(), snapshot.block_entities.size());
    }
    CHECK(total > 0);
  }

  // The start: four server-generated candidates read from
  // `build/probes/DesertPyramidProbe.java`. `lowestMotion` is the minimum
  // `MOTION_BLOCKING_NO_LEAVES` over the 21 x 21 box; the piece's positional
  // random subtracts its 0..2 offset to get the final base height.
  WgOverworldGenerator generator(42);
  WgGeneratedHeights generated_heights(generator);
  WgClimateSampler sampler(generator.state());
  struct StartPin {
    int32_t chunk_x;
    int32_t chunk_z;
    uint8_t direction;
    int32_t base_y;
    int32_t min_x;
    int32_t min_z;
  };
  static const StartPin kStarts[] = {
      {-86, -110, 1, 72, -1376, -1760},
      {196, 148, 3, 60, 3136, 2368},
      {-277, -320, 0, 66, -4432, -5120},
      {-271, -362, 2, 69, -4336, -5792},
  };
  for (const StartPin& pin : kStarts) {
    const WgDesertPyramidStart start =
        wg_desert_pyramid_start(42, pin.chunk_x, pin.chunk_z, sampler, generated_heights);
    CHECK(start.valid);
    if (!start.valid)
      continue;
    CHECK_EQ(start.direction, pin.direction);
    CHECK_EQ(start.x, pin.min_x);
    CHECK_EQ(start.y, 64);
    CHECK_EQ(start.z, pin.min_z);
    CHECK_EQ(start.ground_y, pin.base_y);
    CHECK_EQ(start.min_x, pin.min_x);
    CHECK_EQ(start.max_x, pin.min_x + 20);
    CHECK_EQ(start.min_y, pin.base_y);
    CHECK_EQ(start.max_y, pin.base_y + 14);
    CHECK_EQ(start.min_z, pin.min_z);
    CHECK_EQ(start.max_z, pin.min_z + 20);
  }

  const WgStructureSet* set = wg_structure_set_by_name("minecraft:desert_pyramids");
  CHECK(set != nullptr);
  if (set != nullptr)
    CHECK(!wg_structure_is_chunk(42, *set, 0, 0));
  CHECK(!wg_desert_pyramid_start(42, 0, 0, sampler, generated_heights).valid);

  // The 21 x 21 footprint crosses four chunks. The bridge scans the source
  // window, so every one of them gets its own slice, and the block entities
  // are distributed instead of all landing in the source chunk.
  const WgDesertPyramidStart start =
      wg_desert_pyramid_start(42, -86, -110, sampler, generated_heights);
  CHECK(start.valid);
  if (start.valid) {
    size_t chests = 0;
    size_t brushables = 0;
    bool matched_direct = false;
    for (int32_t dz = 0; dz <= 1; ++dz) {
      for (int32_t dx = 0; dx <= 1; ++dx) {
        Chunk applied;
        fill_generated_surface_chunk(generator, -86 + dx, -110 + dz, applied);
        const WgStructureApplyStats stats = apply_generated_structures(generator, applied);
        CHECK(stats.starts >= 1u);
        CHECK(stats.pieces >= 1u);
        CHECK(stats.blocks > 0u);
        for (const nbt::Value& be : applied.block_entities) {
          const nbt::Value* id = nbt::child(be, "id");
          if (id == nullptr)
            continue;
          const auto name = nbt::as_string(*id);
          if (!name)
            continue;
          if (*name == "minecraft:chest") {
            ++chests;
            const nbt::Value* items = nbt::child(be, "Items");
            CHECK(items != nullptr && items->type == nbt::Tag::List && !items->items.empty());
          } else if (*name == "minecraft:brushable_block") {
            ++brushables;
            const nbt::Value* table = nbt::child(be, "LootTable");
            const auto table_name = table == nullptr ? std::nullopt : nbt::as_string(*table);
            CHECK(table_name && *table_name == "minecraft:archaeology/desert_pyramid");
            const nbt::Value* seed = nbt::child(be, "LootTableSeed");
            CHECK(seed != nullptr && seed->type == nbt::Tag::Long);
          }
        }
        if (stats.starts == 1u && stats.pieces == 1u) {
          Chunk direct;
          fill_generated_surface_chunk(generator, -86 + dx, -110 + dz, direct);
          const uint32_t placed = wg_place_desert_pyramid(direct, start, generated_heights);
          CHECK_EQ(placed, stats.blocks);
          matched_direct = true;
        }
      }
    }
    CHECK(matched_direct);
    CHECK_EQ(chests, 4u);
    CHECK_EQ(brushables, 6u); // one collapsed-roof cell + this start's five potential cells
  }
}

uint64_t mineshaft_start_digest(const WgMineshaftStart& start) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (const WgMineshaftPiece& piece : start.pieces) {
    std::string row = std::to_string(static_cast<int>(piece.kind));
    row += ':';
    row += std::to_string(piece.type);
    row += ':';
    row += std::to_string(piece.gen_depth);
    row += ':';
    row += std::to_string(piece.direction);
    row += ':';
    row += std::to_string(piece.two_floored);
    row += ':';
    row += std::to_string(piece.has_rails);
    row += ':';
    row += std::to_string(piece.spider_corridor);
    row += ':';
    row += std::to_string(piece.num_sections);
    row += ':';
    row += std::to_string(piece.min_x);
    row += ',';
    row += std::to_string(piece.min_y);
    row += ',';
    row += std::to_string(piece.min_z);
    row += ',';
    row += std::to_string(piece.max_x);
    row += ',';
    row += std::to_string(piece.max_y);
    row += ',';
    row += std::to_string(piece.max_z);
    row += ':';
    for (const WgPieceBox& entrance : piece.entrances) {
      row += std::to_string(entrance.min_x);
      row += ',';
      row += std::to_string(entrance.min_y);
      row += ',';
      row += std::to_string(entrance.min_z);
      row += ',';
      row += std::to_string(entrance.max_x);
      row += ',';
      row += std::to_string(entrance.max_y);
      row += ',';
      row += std::to_string(entrance.max_z);
      row += ';';
    }
    hash = fnv1a_row(hash, row);
  }
  return hash;
}

uint64_t stronghold_start_digest(const WgStrongholdStart& start) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (const WgStrongholdPiece& piece : start.pieces) {
    std::string row = std::to_string(static_cast<int>(piece.kind));
    row += ':';
    row += std::to_string(piece.gen_depth);
    row += ':';
    row += std::to_string(piece.direction);
    row += ':';
    row += std::to_string(piece.entry_door);
    row += ':';
    row += std::to_string(piece.kind == WgStrongholdPieceKind::kStraight ? piece.left : 0);
    row += ':';
    row += std::to_string(piece.kind == WgStrongholdPieceKind::kStraight ? piece.right : 0);
    row += ':';
    row += std::to_string(piece.kind == WgStrongholdPieceKind::kFiveCrossing ? piece.left : 0);
    row += ':';
    row += std::to_string(piece.kind == WgStrongholdPieceKind::kFiveCrossing ? piece.left_high : 0);
    row += ':';
    row += std::to_string(piece.kind == WgStrongholdPieceKind::kFiveCrossing ? piece.right : 0);
    row += ':';
    row +=
        std::to_string(piece.kind == WgStrongholdPieceKind::kFiveCrossing ? piece.right_high : 0);
    row += ':';
    row += std::to_string(piece.room_type);
    row += ':';
    row += std::to_string(piece.is_tall);
    row += ':';
    row += std::to_string(piece.is_source);
    row += ':';
    row += std::to_string(piece.steps);
    row += ':';
    row += std::to_string(piece.min_x);
    row += ',';
    row += std::to_string(piece.min_y);
    row += ',';
    row += std::to_string(piece.min_z);
    row += ',';
    row += std::to_string(piece.max_x);
    row += ',';
    row += std::to_string(piece.max_y);
    row += ',';
    row += std::to_string(piece.max_z);
    hash = fnv1a_row(hash, row);
  }
  return hash;
}

void test_stronghold() {
  // `StrongholdProbe.java 42 -25 -141`: the first ring chunk listed by
  // `RingProbe` for seed 42. The start needed the retry loop until the portal
  // room was placed; the piece list carries 97 pieces and the union box is
  // shifted below sea level by `moveBelowSeaLevel`.
  WgOverworldGenerator generator(42);
  WgClimateSampler sampler(generator.state());
  WgStructurePlacementState placement(generator);
  const WgStructureSet* set = wg_structure_set_by_name("minecraft:strongholds");
  CHECK(set != nullptr);
  if (set != nullptr) {
    bool ring_found = false;
    for (const WgStructureChunk& ring : placement.ring_positions(set->placement))
      if (ring.x == -25 && ring.z == -141)
        ring_found = true;
    CHECK(ring_found);
  }

  const WgStrongholdStart start = wg_stronghold_start(42, -25, -141, sampler);
  CHECK(start.valid);
  CHECK_EQ(start.pieces.size(), 97u);
  CHECK_EQ(stronghold_start_digest(start), 17733785529073386920ull);
  CHECK_EQ(start.min_x, -447);
  CHECK_EQ(start.min_y, -67);
  CHECK_EQ(start.min_z, -2327);
  CHECK_EQ(start.max_x, -337);
  CHECK_EQ(start.max_y, -3);
  CHECK_EQ(start.max_z, -2207);
  CHECK_EQ(start.portal_room, 24);
  if (start.portal_room >= 0) {
    const WgStrongholdPiece& portal = start.pieces[static_cast<size_t>(start.portal_room)];
    CHECK(portal.kind == WgStrongholdPieceKind::kPortalRoom);
    CHECK_EQ(portal.min_x, -387);
    CHECK_EQ(portal.min_y, -27);
    CHECK_EQ(portal.min_z, -2292);
    CHECK_EQ(portal.max_x, -377);
    CHECK_EQ(portal.max_y, -20);
    CHECK_EQ(portal.max_z, -2277);
  }

  // The portal room's chunk: the spawner, its silverfish entity data, and the
  // ring of mostly empty end-portal frames.
  Chunk portal_chunk;
  reset_chunk(portal_chunk, -24, -143);
  fill_synthetic_terrain(portal_chunk);
  const uint32_t portal_blocks = wg_place_stronghold(portal_chunk, start);
  CHECK(portal_blocks > 0u);
  const Dump portal_dump = dump_terrain_changes(portal_chunk);
  CHECK_EQ(portal_dump.digest, 13666362845128196747ull);
  const int32_t spawner_x = -382 - (-24) * 16;
  const int32_t spawner_z = -2283 - (-143) * 16;
  CHECK(portal_chunk.block_state_at(spawner_x, -24, spawner_z).name == "minecraft:spawner");
  const nbt::Value* spawner = nullptr;
  for (const nbt::Value& be : portal_chunk.block_entities)
    if (block_entity_matches(be, -382, -24, -2283))
      spawner = &be;
  CHECK(spawner != nullptr);
  if (spawner != nullptr) {
    const nbt::Value* data = nbt::child(*spawner, "SpawnData");
    const nbt::Value* entity = data == nullptr ? nullptr : nbt::child(*data, "entity");
    const nbt::Value* id = entity == nullptr ? nullptr : nbt::child(*entity, "id");
    const auto name = id == nullptr ? std::nullopt : nbt::as_string(*id);
    CHECK(name && *name == "minecraft:silverfish");
  }

  // A room-crossing chest chunk, with the chest at the crossing's own cell.
  Chunk chest_chunk;
  reset_chunk(chest_chunk, -25, -142);
  fill_synthetic_terrain(chest_chunk);
  const uint32_t chest_blocks = wg_place_stronghold(chest_chunk, start);
  CHECK(chest_blocks > 0u);
  const Dump chest_dump = dump_terrain_changes(chest_chunk);
  CHECK_EQ(chest_dump.digest, 12797717478984946500ull);
  CHECK(chest_chunk.block_state_at(-390 - (-25) * 16, -23, -2262 - (-142) * 16).name ==
        "minecraft:chest");

  // The tall library at (-435, -25, -2272)..(-421, -15, -2259): bookshelf
  // shelves, the cobweb scatter, the upper-floor fence and torch work, and
  // both loot chests. `StrongholdProbe.java 42 -25 -141 -27 -142` pins the
  // whole chunk against the game's own `postProcess`.
  Chunk library_chunk;
  reset_chunk(library_chunk, -27, -142);
  fill_synthetic_terrain(library_chunk);
  const uint32_t library_blocks = wg_place_stronghold(library_chunk, start);
  CHECK(library_blocks > 0u);
  const Dump library_dump = dump_terrain_changes(library_chunk);
  CHECK_EQ(library_dump.digest, 13124720004966124454ull);
  const int32_t library_chest_cells[2][3] = {{-426, -22, -2269}, {-422, -17, -2260}};
  for (const auto& coords : library_chest_cells) {
    const int32_t local_x = coords[0] - (-27) * 16;
    const int32_t local_z = coords[2] - (-142) * 16;
    CHECK(library_chunk.block_state_at(local_x, coords[1], local_z).name == "minecraft:chest");
    const nbt::Value* library_chest = nullptr;
    for (const nbt::Value& be : library_chunk.block_entities)
      if (block_entity_matches(be, coords[0], coords[1], coords[2]))
        library_chest = &be;
    CHECK(library_chest != nullptr);
    if (library_chest != nullptr) {
      const int64_t seed = wg_structure_loot_seed(42, coords[0], coords[1], coords[2],
                                                  "minecraft:chests/stronghold_library");
      const std::vector<ItemStack> expected =
          wg_structure_loot_items("minecraft:chests/stronghold_library", seed);
      CHECK(!expected.empty());
      const nbt::Value expected_be = make_container_block_entity(
          "minecraft:chest", BlockPosKey{coords[0], coords[1], coords[2]}, expected);
      CHECK(nbt::serialize(*library_chest) == nbt::serialize(expected_be));
    }
  }

  // Every chunk the inflated start box touches, against the server's own
  // positional-seed `postProcess` dump (`StrongholdProbe.java 42 bulk -25
  // -141`). The combined FNV digest is over the 63 `x,z:changed:digest` rows
  // in the same z-then-x order the bulk probe prints, so one wrong cell in any
  // room — five-crossing, prison hall, stairs, filler corridor — fails.
  CHECK_EQ(floor_div(start.min_x, 16), -28);
  CHECK_EQ(floor_div(start.max_x, 16), -22);
  CHECK_EQ(floor_div(start.min_z, 16), -146);
  CHECK_EQ(floor_div(start.max_z, 16), -138);
  uint64_t bulk_combined = 0xcbf29ce484222325ull;
  size_t bulk_rows = 0;
  for (int32_t chunk_z = floor_div(start.min_z, 16); chunk_z <= floor_div(start.max_z, 16);
       ++chunk_z) {
    for (int32_t chunk_x = floor_div(start.min_x, 16); chunk_x <= floor_div(start.max_x, 16);
         ++chunk_x) {
      Chunk bulk_chunk;
      reset_chunk(bulk_chunk, chunk_x, chunk_z);
      fill_synthetic_terrain(bulk_chunk);
      const uint32_t placed = wg_place_stronghold(bulk_chunk, start);
      const Dump dump = dump_terrain_changes(bulk_chunk);
      CHECK(placed >= dump.rows.size());
      bulk_rows += dump.rows.size();
      std::string row = std::to_string(chunk_x);
      row.push_back(',');
      row += std::to_string(chunk_z);
      row.push_back(':');
      row += std::to_string(dump.rows.size());
      row.push_back(':');
      row += std::to_string(static_cast<unsigned long long>(dump.digest));
      bulk_combined = fnv1a_row(bulk_combined, row);
    }
  }
  CHECK_EQ(bulk_rows, 31595u);
  CHECK_EQ(bulk_combined, 222035983761887908ull);

  // The scan hook: a generated chunk at the first ring position sees this
  // start, and every cell the direct writer changes is that cell in the
  // applied chunk. Other sets may also write here; only the start's own cells
  // are compared.
  Chunk pristine;
  fill_generated_surface_chunk(generator, -24, -143, pristine);
  Chunk applied = pristine;
  const WgStructureApplyStats stats = apply_generated_structures(generator, applied);
  CHECK(stats.starts >= 1u);
  Chunk direct = pristine;
  wg_place_stronghold(direct, start);
  size_t stronghold_cells = 0;
  size_t mismatches = 0;
  for (int32_t x = 0; x < 16; ++x)
    for (int32_t z = 0; z < 16; ++z)
      for (int32_t y = kMinY; y < kMinY + kWgHeight; ++y) {
        if (direct.block_state_at(x, y, z) == pristine.block_state_at(x, y, z))
          continue;
        ++stronghold_cells;
        if (!(applied.block_state_at(x, y, z) == direct.block_state_at(x, y, z)))
          ++mismatches;
      }
  CHECK(stronghold_cells > 0u);
  CHECK_EQ(mismatches, 0u);
}

std::string monument_piece_row(const WgOceanMonumentStart& start,
                               const WgOceanMonumentPiece& piece) {
  std::string row = std::to_string(static_cast<int>(piece.kind));
  row.push_back(':');
  row += std::to_string(piece.direction);
  row.push_back(':');
  row += std::to_string(piece.main_design);
  row.push_back(':');
  row += std::to_string(piece.min_x);
  row.push_back(',');
  row += std::to_string(piece.min_y);
  row.push_back(',');
  row += std::to_string(piece.min_z);
  row.push_back(',');
  row += std::to_string(piece.max_x);
  row.push_back(',');
  row += std::to_string(piece.max_y);
  row.push_back(',');
  row += std::to_string(piece.max_z);
  row.push_back(':');
  row += std::to_string(piece.room_index);
  row.push_back(':');
  uint32_t opening = 0;
  for (const WgMonumentRoom& room : start.rooms)
    if (room.index == piece.room_index)
      opening = room.has_opening;
  row += std::to_string(opening);
  return row;
}

std::string monument_room_row(const WgMonumentRoom& room) {
  std::string row = std::to_string(room.index);
  row.push_back(':');
  row += std::to_string(room.claimed);
  row.push_back(':');
  row += std::to_string(room.is_source);
  row.push_back(':');
  row += std::to_string(room.has_opening);
  row.push_back(':');
  for (int i = 0; i < 6; ++i) {
    if (i != 0)
      row.push_back(',');
    row += std::to_string(room.connections[i]);
  }
  return row;
}

uint64_t monument_start_digest(const WgOceanMonumentStart& start) {
  uint64_t hash = 0xcbf29ce484222325ull;
  hash = fnv1a_row(hash, monument_piece_row(start, start.building));
  for (const WgOceanMonumentPiece& piece : start.pieces)
    hash = fnv1a_row(hash, monument_piece_row(start, piece));
  std::vector<WgMonumentRoom> rooms = start.rooms;
  std::sort(rooms.begin(), rooms.end(),
            [](const WgMonumentRoom& a, const WgMonumentRoom& b) { return a.index < b.index; });
  for (const WgMonumentRoom& room : rooms)
    hash = fnv1a_row(hash, monument_room_row(room));
  return hash;
}

std::string mansion_piece_row(const WgMansionPiece& piece) {
  const WgPieceBox box =
      wg_piece_box(*piece.templ, piece.rotation, piece.x, piece.y, piece.z, piece.mirror);
  std::string row = piece.templ != nullptr ? piece.templ->name : "null";
  constexpr std::string_view kMansionPrefix = "minecraft:woodland_mansion/";
  if (row.rfind(kMansionPrefix.data(), 0) == 0)
    row.erase(0, kMansionPrefix.size());
  row.push_back(':');
  row += std::to_string(piece.rotation);
  row.push_back(':');
  row += std::to_string(piece.mirror);
  row.push_back(':');
  row += std::to_string(piece.x);
  row.push_back(',');
  row += std::to_string(piece.y);
  row.push_back(',');
  row += std::to_string(piece.z);
  row.push_back(':');
  row += std::to_string(box.min_x);
  row.push_back(',');
  row += std::to_string(box.min_y);
  row.push_back(',');
  row += std::to_string(box.min_z);
  row.push_back(',');
  row += std::to_string(box.max_x);
  row.push_back(',');
  row += std::to_string(box.max_y);
  row.push_back(',');
  row += std::to_string(box.max_z);
  return row;
}

uint64_t mansion_start_digest(const WgMansionStart& start) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (const WgMansionPiece& piece : start.pieces)
    hash = fnv1a_row(hash, mansion_piece_row(piece));
  return hash;
}

void test_mansion() {
  // `WoodlandMansionProbe 42 find 20` found the random-spread source chunk
  // (-49, -1559). The 514-piece start pins the whole grid/placer order, every
  // template name and the mirror/rotation pair each piece carries.
  WgOverworldGenerator generator(42);
  WgGeneratedHeights heights(generator);
  WgClimateSampler sampler(generator.state());
  const WgMansionStart start = wg_mansion_start(42, -49, -1559, sampler, heights);
  CHECK(start.valid);
  CHECK_EQ(start.rotation, 2u);
  CHECK_EQ(start.x, -777);
  CHECK_EQ(start.y, 69);
  CHECK_EQ(start.z, -24937);
  CHECK_EQ(start.min_x, -788);
  CHECK_EQ(start.min_y, 69);
  CHECK_EQ(start.min_z, -24978);
  CHECK_EQ(start.max_x, -710);
  CHECK_EQ(start.max_y, 99);
  CHECK_EQ(start.max_z, -24918);
  CHECK_EQ(start.pieces.size(), 514u);
  const uint64_t digest = mansion_start_digest(start);
  CHECK_EQ(digest, 12291287715087390869ull);
  if (digest != 12291287715087390869ull) {
    for (const WgMansionPiece& piece : start.pieces)
      std::fprintf(stderr, "MANSION %s\n", mansion_piece_row(piece).c_str());
  }

  // Every chunk the start box touches, against the server's own template
  // `postProcess` dumps plus `WoodlandMansionStructure.afterPlace`. The probe
  // clears mob markers before running, matching the port's no-entity rule.
  struct ExpectedChunk {
    int32_t x;
    int32_t z;
    uint32_t changed;
    uint64_t digest;
  };
  const ExpectedChunk expected[30] = {
      {-50, -1562, 24u, 10988388482095727922ull},   {-49, -1562, 197u, 267566516809075894ull},
      {-48, -1562, 189u, 2824388392061739819ull},   {-47, -1562, 201u, 3448776523108748858ull},
      {-46, -1562, 258u, 16655301591554688165ull},  {-45, -1562, 195u, 13224628578089202961ull},
      {-50, -1561, 206u, 8503160609597944807ull},   {-49, -1561, 2588u, 13724789952619972983ull},
      {-48, -1561, 2762u, 11008074609586406924ull}, {-47, -1561, 2674u, 6035516053227904479ull},
      {-46, -1561, 3474u, 9052035535997805484ull},  {-45, -1561, 2702u, 17344723007281184361ull},
      {-50, -1560, 253u, 14109284279363831639ull},  {-49, -1560, 2583u, 7653833098188908589ull},
      {-48, -1560, 2718u, 11630710872909024083ull}, {-47, -1560, 3418u, 9959352824170651384ull},
      {-46, -1560, 3092u, 9088127150718441928ull},  {-45, -1560, 1746u, 5477253162738300869ull},
      {-50, -1559, 265u, 6176117016182168105ull},   {-49, -1559, 2391u, 2712308453363457979ull},
      {-48, -1559, 2607u, 8755995235046262036ull},  {-47, -1559, 3286u, 207367422500250506ull},
      {-46, -1559, 2257u, 10167608476030082562ull}, {-45, -1559, 0u, 14695981039346656037ull},
      {-50, -1558, 145u, 15589683224414148262ull},  {-49, -1558, 1784u, 10604232081126309653ull},
      {-48, -1558, 1768u, 9662757712739078330ull},  {-47, -1558, 2590u, 17734904807296664697ull},
      {-46, -1558, 1582u, 10187610740404064039ull}, {-45, -1558, 0u, 14695981039346656037ull},
  };
  uint64_t combined = 0xcbf29ce484222325ull;
  size_t total_rows = 0;
  for (const ExpectedChunk& expected_chunk : expected) {
    Chunk chunk;
    reset_chunk(chunk, expected_chunk.x, expected_chunk.z);
    fill_synthetic_terrain(chunk);
    const uint32_t placed = wg_place_mansion(chunk, start);
    const Dump dump = dump_terrain_changes(chunk);
    CHECK(placed >= dump.rows.size());
    CHECK_EQ(dump.rows.size(), expected_chunk.changed);
    CHECK_EQ(dump.digest, expected_chunk.digest);
    if (std::getenv("MC_MANSION_DUMP") != nullptr &&
        (dump.rows.size() != expected_chunk.changed || dump.digest != expected_chunk.digest)) {
      std::FILE* file = std::fopen("/tmp/mansion_cpp_chunk.txt", "w");
      if (file != nullptr) {
        for (const std::string& dumped : dump.rows)
          std::fprintf(file, "%s\n", dumped.c_str());
        std::fclose(file);
      }
      std::fprintf(stderr, "MANSION_DUMP %d,%d\n", expected_chunk.x, expected_chunk.z);
    }
    total_rows += dump.rows.size();
    std::string row = std::to_string(expected_chunk.x);
    row.push_back(',');
    row += std::to_string(expected_chunk.z);
    row.push_back(':');
    row += std::to_string(dump.rows.size());
    row.push_back(':');
    row += std::to_string(static_cast<unsigned long long>(dump.digest));
    combined = fnv1a_row(combined, row);
  }
  CHECK_EQ(total_rows, 47955u);
  CHECK_EQ(combined, 11842314630865678230ull);

  // The other three start rotations, each from a different random-spread
  // source that `WoodlandMansionProbe 42 findall 20` found. Their full start
  // digests pin the grid/placer stream under every rotation, and one chunk
  // each pins the mirror-then-rotate state transform through a real template
  // set (the rotation-2 start alone leaves several mirror/rotation pairs
  // unexercised).
  struct RotationPin {
    int32_t x;
    int32_t z;
    uint32_t rotation;
    size_t pieces;
    uint64_t start_digest;
    uint32_t changed;
    uint64_t digest;
  };
  const RotationPin rotations[3] = {
      {-1100, -1504, 1u, 600u, 15726188401772376339ull, 2454u, 9047187541241735622ull},
      {-1420, -537, 0u, 559u, 2407726527686312004ull, 12343u, 12097738783544816192ull},
      {670, -387, 3u, 606u, 5549044616558309115ull, 3260u, 12092282046044333549ull},
  };
  for (const RotationPin& pin : rotations) {
    const WgMansionStart other = wg_mansion_start(42, pin.x, pin.z, sampler, heights);
    CHECK(other.valid);
    CHECK_EQ(other.rotation, pin.rotation);
    CHECK_EQ(other.pieces.size(), pin.pieces);
    CHECK_EQ(mansion_start_digest(other), pin.start_digest);
    Chunk chunk;
    reset_chunk(chunk, pin.x, pin.z);
    fill_synthetic_terrain(chunk);
    wg_place_mansion(chunk, other);
    const Dump dump = dump_terrain_changes(chunk);
    CHECK_EQ(dump.rows.size(), pin.changed);
    CHECK_EQ(dump.digest, pin.digest);
    if (dump.rows.size() != pin.changed || dump.digest != pin.digest) {
      std::fprintf(stderr, "MANSION_ROT %d,%d rows=%zu digest=%llu\n", pin.x, pin.z,
                   dump.rows.size(), static_cast<unsigned long long>(dump.digest));
      if (std::getenv("MC_MANSION_DUMP") != nullptr) {
        if (std::FILE* file = std::fopen("/tmp/mansion_rot_cpp.txt", "w")) {
          for (const std::string& dumped : dump.rows)
            std::fprintf(file, "%s\n", dumped.c_str());
          std::fclose(file);
        }
      }
    }
  }

  // The bridge finds this mansion (not just a direct call) and produces the
  // same cells, foundation included, on a real generated chunk. Comparing only
  // the cells the direct writer changed keeps unrelated structures out of the
  // check without weakening it.
  Chunk pristine;
  fill_generated_surface_chunk(generator, -48, -1559, pristine);
  Chunk applied = pristine;
  const WgStructureApplyStats stats = apply_generated_structures(generator, applied);
  CHECK(stats.starts >= 1u);
  Chunk direct = pristine;
  CHECK(wg_place_mansion(direct, start) > 0u);
  size_t mansion_cells = 0;
  size_t mismatches = 0;
  for (int32_t x = 0; x < 16; ++x)
    for (int32_t z = 0; z < 16; ++z)
      for (int32_t y = kMinY; y < kMinY + kWgHeight; ++y) {
        if (direct.block_state_at(x, y, z) == pristine.block_state_at(x, y, z))
          continue;
        ++mansion_cells;
        if (!(applied.block_state_at(x, y, z) == direct.block_state_at(x, y, z)))
          ++mismatches;
      }
  CHECK(mansion_cells > 0u);
  CHECK_EQ(mismatches, 0u);
}

void test_ocean_monument() {
  // `OceanMonumentProbe 42 find 24` found the random-spread source chunk
  // (-202, -754). The start is one `MonumentBuilding` with 32 children and a
  // 49-row room graph; the room rows pin the graph construction, shuffle and
  // door-closing pass, and the child rows pin the fitter order and boxes.
  WgOverworldGenerator generator(42);
  WgGeneratedHeights heights(generator);
  WgClimateSampler sampler(generator.state());
  const WgOceanMonumentStart start = wg_ocean_monument_start(42, -202, -754, sampler, heights);
  CHECK(start.valid);
  CHECK_EQ(start.min_x, -3261);
  CHECK_EQ(start.min_y, 39);
  CHECK_EQ(start.min_z, -12093);
  CHECK_EQ(start.max_x, -3204);
  CHECK_EQ(start.max_y, 61);
  CHECK_EQ(start.max_z, -12036);
  CHECK_EQ(start.pieces.size(), 32u);
  CHECK_EQ(start.rooms.size(), 49u);
  CHECK_EQ(monument_start_digest(start), 4033139150091024041ull);
  CHECK_EQ(static_cast<int>(start.building.kind), 0);
  CHECK_EQ(start.building.direction, 0);
  CHECK_EQ(start.pieces[0].kind, WgOceanMonumentPieceKind::kEntry);
  CHECK_EQ(start.pieces[0].min_x, -3236);
  CHECK_EQ(start.pieces[0].min_y, 39);
  CHECK_EQ(start.pieces[0].min_z, -12065);
  CHECK_EQ(start.pieces[0].max_x, -3229);
  CHECK_EQ(start.pieces[0].max_y, 42);
  CHECK_EQ(start.pieces[0].max_z, -12058);
  CHECK_EQ(start.pieces[1].kind, WgOceanMonumentPieceKind::kCore);
  CHECK_EQ(start.pieces[1].room_index, 13);
  CHECK_EQ(start.pieces[29].kind, WgOceanMonumentPieceKind::kWing);
  CHECK_EQ(start.pieces[29].main_design, 1);
  CHECK_EQ(start.pieces[30].main_design, 0);
  CHECK_EQ(start.pieces[31].kind, WgOceanMonumentPieceKind::kPenthouse);

  // Every chunk the building box touches, against the server's own
  // `MonumentBuilding.postProcess` dump (`OceanMonumentProbe 42 bulk -202
  // -754`). The building's positional random is shared by the children, so a
  // wrong fitter order or a child that should have been skipped shows up in
  // the later rooms' random cells as well as its own.
  struct ExpectedChunk {
    int32_t x;
    int32_t z;
    uint32_t changed;
    uint64_t digest;
  };
  const ExpectedChunk expected[16] = {
      {-204, -756, 6326, 9249460236872337158ull},  {-203, -756, 6496, 17763998214278001382ull},
      {-202, -756, 6496, 2439516001787424334ull},  {-201, -756, 6354, 15434194195032272642ull},
      {-204, -755, 6464, 12612855427606206231ull}, {-203, -755, 6656, 17181670289221204273ull},
      {-202, -755, 6656, 7209146653533463561ull},  {-201, -755, 6496, 7373450961188415377ull},
      {-204, -754, 6464, 13894249107711794718ull}, {-203, -754, 6656, 12558926939595233796ull},
      {-202, -754, 6656, 7491285043678048268ull},  {-201, -754, 6496, 1691603482260134872ull},
      {-204, -753, 6300, 4998346925677462289ull},  {-203, -753, 6464, 13420664329344249253ull},
      {-202, -753, 6464, 8654766327384259339ull},  {-201, -753, 6326, 8139151406970546506ull},
  };
  uint64_t combined = 0xcbf29ce484222325ull;
  size_t total_rows = 0;
  for (const ExpectedChunk& expected_chunk : expected) {
    Chunk chunk;
    reset_chunk(chunk, expected_chunk.x, expected_chunk.z);
    fill_synthetic_terrain(chunk);
    const uint32_t placed = wg_place_ocean_monument(chunk, start);
    const Dump dump = dump_terrain_changes(chunk);
    CHECK(placed >= dump.rows.size());
    CHECK_EQ(dump.rows.size(), expected_chunk.changed);
    CHECK_EQ(dump.digest, expected_chunk.digest);
    total_rows += dump.rows.size();
    std::string row = std::to_string(expected_chunk.x);
    row.push_back(',');
    row += std::to_string(expected_chunk.z);
    row.push_back(':');
    row += std::to_string(dump.rows.size());
    row.push_back(':');
    row += std::to_string(static_cast<unsigned long long>(dump.digest));
    combined = fnv1a_row(combined, row);
  }
  CHECK_EQ(total_rows, 103770u);
  CHECK_EQ(combined, 13007413590596353550ull);

  // The scan hook: the source chunk is a placement candidate, and the applied
  // chunk agrees with the direct writer on every cell the monument changed.
  const WgStructureSet* set = wg_structure_set_by_name("minecraft:ocean_monuments");
  CHECK(set != nullptr);
  if (set != nullptr)
    CHECK(wg_structure_is_chunk(42, *set, -202, -754));
  Chunk pristine;
  fill_generated_surface_chunk(generator, -202, -754, pristine);
  Chunk applied = pristine;
  const WgStructureApplyStats stats = apply_generated_structures(generator, applied);
  CHECK(stats.starts >= 1u);
  Chunk direct = pristine;
  const uint32_t direct_blocks = wg_place_ocean_monument(direct, start);
  CHECK(direct_blocks > 0u);
  size_t monument_cells = 0;
  size_t mismatches = 0;
  for (int32_t x = 0; x < 16; ++x)
    for (int32_t z = 0; z < 16; ++z)
      for (int32_t y = kMinY; y < kMinY + kWgHeight; ++y) {
        if (direct.block_state_at(x, y, z) == pristine.block_state_at(x, y, z))
          continue;
        ++monument_cells;
        if (!(applied.block_state_at(x, y, z) == direct.block_state_at(x, y, z)))
          ++mismatches;
      }
  CHECK(monument_cells > 0u);
  CHECK_EQ(mismatches, 0u);

  // The other three orientations: seed 44's south-facing start, seed 45's
  // west-facing one and seed 57's east-facing one (`OceanMonumentProbe <seed>
  // start <cx> <cz>`). The start digest pins the room boxes and wing/penthouse
  // corners after `getWorldPos`, and the source chunk pins the orientation's
  // writer and water reads.
  WgOverworldGenerator generator_south(44);
  WgGeneratedHeights heights_south(generator_south);
  WgClimateSampler sampler_south(generator_south.state());
  const WgOceanMonumentStart south =
      wg_ocean_monument_start(44, 241, -338, sampler_south, heights_south);
  CHECK(south.valid);
  CHECK_EQ(south.building.direction, 2);
  CHECK_EQ(monument_start_digest(south), 10863205483714595010ull);
  Chunk south_chunk;
  reset_chunk(south_chunk, 241, -338);
  fill_synthetic_terrain(south_chunk);
  wg_place_ocean_monument(south_chunk, south);
  const Dump south_dump = dump_terrain_changes(south_chunk);
  CHECK_EQ(south_dump.rows.size(), 6656u);
  CHECK_EQ(south_dump.digest, 14196309955695423550ull);

  WgOverworldGenerator generator_west(45);
  WgGeneratedHeights heights_west(generator_west);
  WgClimateSampler sampler_west(generator_west.state());
  const WgOceanMonumentStart west =
      wg_ocean_monument_start(45, 9, -347, sampler_west, heights_west);
  CHECK(west.valid);
  CHECK_EQ(west.building.direction, 3);
  CHECK_EQ(monument_start_digest(west), 996517210931880261ull);
  Chunk west_chunk;
  reset_chunk(west_chunk, 9, -347);
  fill_synthetic_terrain(west_chunk);
  wg_place_ocean_monument(west_chunk, west);
  const Dump west_dump = dump_terrain_changes(west_chunk);
  CHECK_EQ(west_dump.rows.size(), 6656u);
  CHECK_EQ(west_dump.digest, 8800627468822043911ull);

  WgOverworldGenerator generator_east(57);
  WgGeneratedHeights heights_east(generator_east);
  WgClimateSampler sampler_east(generator_east.state());
  const WgOceanMonumentStart east =
      wg_ocean_monument_start(57, -144, -179, sampler_east, heights_east);
  CHECK(east.valid);
  CHECK_EQ(east.building.direction, 1);
  CHECK_EQ(monument_start_digest(east), 862171760881333074ull);
  Chunk east_chunk;
  reset_chunk(east_chunk, -144, -179);
  fill_synthetic_terrain(east_chunk);
  wg_place_ocean_monument(east_chunk, east);
  const Dump east_dump = dump_terrain_changes(east_chunk);
  CHECK_EQ(east_dump.rows.size(), 6656u);
  CHECK_EQ(east_dump.digest, 16032392377937409442ull);
}

void test_mineshaft() {
  // `MineshaftProbe.java` on seed 42, chunk (-3, -17): the set's normal
  // mineshaft is picked, vanilla's room and recursive children produce 176
  // pieces, and `moveBelowSeaLevel` lands the room at y 2. The probe's piece
  // digest includes every field the port carries: kind, type, depth,
  // direction, the crossing's two-floor flag, a corridor's rails/spider/
  // sections, the box and the room's entrance boxes.
  WgOverworldGenerator generator(42);
  WgGeneratedHeights heights(generator);
  WgClimateSampler sampler(generator.state());
  CaveHeights cave_heights;
  const WgMineshaftStart start = wg_mineshaft_start(42, -3, -17, sampler, heights);
  CHECK(start.valid);
  CHECK_EQ(start.type, 0);
  CHECK_EQ(start.pieces.size(), 176u);
  CHECK_EQ(start.min_x, -131);
  CHECK_EQ(start.min_y, -12);
  CHECK_EQ(start.min_z, -344);
  CHECK_EQ(start.max_x, 31);
  CHECK_EQ(start.max_y, 19);
  CHECK_EQ(start.max_z, -180);
  CHECK_EQ(start.start_x, -40);
  CHECK_EQ(start.start_y, 2);
  CHECK_EQ(start.start_z, -272);
  CHECK_EQ(start.pieces[0].kind, WgMineshaftPieceKind::kRoom);
  CHECK_EQ(start.pieces[0].min_x, -46);
  CHECK_EQ(start.pieces[0].min_y, 2);
  CHECK_EQ(start.pieces[0].min_z, -270);
  CHECK_EQ(start.pieces[0].max_x, -37);
  CHECK_EQ(start.pieces[0].max_y, 10);
  CHECK_EQ(start.pieces[0].max_z, -263);
  CHECK_EQ(mineshaft_start_digest(start), 5434815479370800690ull);

  // A second seed-42 start in eroded badlands: the gate's first weighted pick
  // is the normal mineshaft, whose biome set rejects the start, so the retry
  // removes it and picks `mineshaft_mesa` from the same continuing stream.
  // The probe's digest includes every piece field; the box and count are
  // vanilla's own.
  const WgMineshaftStart mesa = wg_mineshaft_start(42, -228, -266, sampler, heights);
  CHECK(mesa.valid);
  CHECK_EQ(mesa.type, 1);
  CHECK_EQ(mesa.pieces.size(), 123u);
  CHECK_EQ(mesa.min_x, -3702);
  CHECK_EQ(mesa.min_y, 64);
  CHECK_EQ(mesa.min_z, -4335);
  CHECK_EQ(mesa.max_x, -3583);
  CHECK_EQ(mesa.max_y, 92);
  CHECK_EQ(mesa.max_z, -4182);
  CHECK_EQ(mesa.start_x, -3640);
  CHECK_EQ(mesa.start_z, -4256);
  CHECK_EQ(mineshaft_start_digest(mesa), 11310403918791139981ull);

  // `getBaseHeight` walks the noise column, not the surface column. These are
  // the exact `HEIGHT` rows from `MineshaftProbe 42 height ...`: the first two
  // columns are eroded-badlands pillars whose surface rule raises them above
  // the noise answer, and the third is the mesa start's center. Reading the
  // surface stage here moved two mesa shafts 3-4 blocks up and failed the scan
  // digest even though both hand-picked starts matched.
  CHECK_EQ(heights.height(-2374, -3353, WgStructureHeightmap::kWorldSurface), 64);
  CHECK_EQ(heights.height(-3642, -3190, WgStructureHeightmap::kWorldSurface), 72);
  CHECK_EQ(heights.height(-3642, -4258, WgStructureHeightmap::kWorldSurface), 103);

  // Gate: the set's spacing is one chunk, so a chunk that fails the 0.004
  // draw has no start even though its candidate is itself.
  const WgMineshaftStart none = wg_mineshaft_start(42, 0, 1, sampler, heights);
  CHECK(!none.valid);

  // Every chunk the start's bounding box touches, against the server's own
  // positional-seed postProcess dump. The combined FNV digest is over the
  // 121 `x,z:changed:digest` rows in the same z-then-x order the bulk probe
  // prints, so one wrong cell in any chunk fails the test.
  struct ChunkPin {
    int32_t chunk_x;
    int32_t chunk_z;
    uint32_t rows;
    uint64_t digest;
  };
  static const ChunkPin kPins[] = {
      {-3, -17, 967, 11985448751218601588ull}, {-2, -17, 196, 17600243409068691560ull},
      {-3, -18, 324, 13738181401978092375ull}, {-2, -18, 0, 14695981039346656037ull},
      {-4, -17, 734, 2992392375709289752ull},  {-5, -20, 575, 11307574805791159673ull},
      {-6, -19, 615, 1586850299716861109ull},  {-6, -16, 315, 14048880761141165513ull},
      {-3, -15, 540, 17214670767781683716ull},
  };
  CHECK_EQ(floor_div(start.min_x, 16), -9);
  CHECK_EQ(floor_div(start.max_x, 16), 1);
  CHECK_EQ(floor_div(start.min_z, 16), -22);
  CHECK_EQ(floor_div(start.max_z, 16), -12);
  uint64_t combined = 0xcbf29ce484222325ull;
  size_t total_rows = 0;
  for (int32_t chunk_z = floor_div(start.min_z, 16); chunk_z <= floor_div(start.max_z, 16);
       ++chunk_z) {
    for (int32_t chunk_x = floor_div(start.min_x, 16); chunk_x <= floor_div(start.max_x, 16);
         ++chunk_x) {
      Chunk chunk;
      reset_chunk(chunk, chunk_x, chunk_z);
      fill_cave_terrain(chunk);
      const uint32_t placed = wg_place_mineshaft(chunk, start, cave_heights, sampler);
      const Dump dump = dump_cave_changes(chunk);
      total_rows += dump.rows.size();
      std::string row = std::to_string(chunk_x);
      row.push_back(',');
      row += std::to_string(chunk_z);
      row.push_back(':');
      row += std::to_string(dump.rows.size());
      row.push_back(':');
      row += std::to_string(static_cast<unsigned long long>(dump.digest));
      combined = fnv1a_row(combined, row);
      for (const ChunkPin& pin : kPins) {
        if (pin.chunk_x != chunk_x || pin.chunk_z != chunk_z)
          continue;
        if (dump.digest != pin.digest)
          std::fprintf(stderr, "  mineshaft chunk %d,%d: %zu rows digest %llu\n", chunk_x, chunk_z,
                       dump.rows.size(), static_cast<unsigned long long>(dump.digest));
        CHECK_EQ(dump.rows.size(), pin.rows);
        CHECK_EQ(dump.digest, pin.digest);
      }
      CHECK(placed >= dump.rows.size());
    }
  }
  CHECK_EQ(total_rows, 20877u);
  CHECK_EQ(combined, 38358090319676836ull);

  // The mesa start's writer uses dark-oak planks, logs and fences. Its 90
  // intersecting chunks are the server's own too; the source chunk's row and
  // digest are pinned alongside the combined one so a failure names a cell.
  uint64_t mesa_combined = 0xcbf29ce484222325ull;
  size_t mesa_rows = 0;
  for (int32_t chunk_z = floor_div(mesa.min_z, 16); chunk_z <= floor_div(mesa.max_z, 16);
       ++chunk_z) {
    for (int32_t chunk_x = floor_div(mesa.min_x, 16); chunk_x <= floor_div(mesa.max_x, 16);
         ++chunk_x) {
      Chunk chunk;
      reset_chunk(chunk, chunk_x, chunk_z);
      fill_cave_terrain(chunk);
      wg_place_mineshaft(chunk, mesa, cave_heights, sampler);
      const Dump dump = dump_cave_changes(chunk);
      mesa_rows += dump.rows.size();
      std::string row = std::to_string(chunk_x);
      row.push_back(',');
      row += std::to_string(chunk_z);
      row.push_back(':');
      row += std::to_string(dump.rows.size());
      row.push_back(':');
      row += std::to_string(static_cast<unsigned long long>(dump.digest));
      mesa_combined = fnv1a_row(mesa_combined, row);
      if (chunk_x == -228 && chunk_z == -266) {
        CHECK_EQ(dump.rows.size(), 840u);
        CHECK_EQ(dump.digest, 1421135720150577093ull);
      }
      if (chunk_x == -228 && chunk_z == -267) {
        CHECK_EQ(dump.rows.size(), 837u);
        CHECK_EQ(dump.digest, 2449460900573308920ull);
      }
    }
  }
  CHECK_EQ(mesa_rows, 13493u);
  CHECK_EQ(mesa_combined, 8692376189570027913ull);

  // The gate and weighted/retry selection over a 201 x 201-chunk window: the
  // server's 187 candidates and their generated starts, including the invalid
  // first pick that a retry replaces with the mesa member. The combined digest
  // covers a row per candidate (`x,z:valid[:type:pieces:box:digest]`).
  const WgStructureSet* set = wg_structure_set_by_name("minecraft:mineshafts");
  CHECK(set != nullptr);
  uint64_t scan_combined = 0xcbf29ce484222325ull;
  size_t scan_candidates = 0;
  size_t scan_valid = 0;
  if (set != nullptr) {
    for (int32_t dz = -100; dz <= 100; ++dz) {
      for (int32_t dx = -100; dx <= 100; ++dx) {
        const int32_t chunk_x = -220 + dx;
        const int32_t chunk_z = -256 + dz;
        if (!wg_structure_is_chunk(42, *set, chunk_x, chunk_z))
          continue;
        ++scan_candidates;
        const WgMineshaftStart candidate =
            wg_mineshaft_start(42, chunk_x, chunk_z, sampler, heights);
        std::string row = std::to_string(chunk_x);
        row.push_back(',');
        row += std::to_string(chunk_z);
        if (candidate.valid) {
          ++scan_valid;
          row += ":1:";
          row += std::to_string(candidate.type);
          row.push_back(':');
          row += std::to_string(candidate.pieces.size());
          row.push_back(':');
          row += std::to_string(candidate.min_x);
          row.push_back(',');
          row += std::to_string(candidate.min_y);
          row.push_back(',');
          row += std::to_string(candidate.min_z);
          row += "..";
          row += std::to_string(candidate.max_x);
          row.push_back(',');
          row += std::to_string(candidate.max_y);
          row.push_back(',');
          row += std::to_string(candidate.max_z);
          row.push_back(':');
          row += std::to_string(static_cast<unsigned long long>(mineshaft_start_digest(candidate)));
        } else {
          row += ":0";
        }
        scan_combined = fnv1a_row(scan_combined, row);
      }
    }
  }
  CHECK_EQ(scan_candidates, 187u);
  CHECK_EQ(scan_valid, 187u);
  CHECK_EQ(scan_combined, 12230471391028475921ull);

  // The five cave-spider spawners the server's pieces create, with their
  // `SpawnData.entity.id`; the port writes a `minecraft:spawner` block plus
  // that block entity. Each is checked on a fresh chunk.
  struct SpawnerPin {
    int32_t chunk_x;
    int32_t chunk_z;
    int32_t x;
    int32_t y;
    int32_t z;
  };
  static const SpawnerPin kSpawners[] = {
      {-2, -17, -23, 4, -260}, {-4, -17, -59, 3, -267}, {-6, -18, -88, 3, -275},
      {-3, -16, -34, 9, -256}, {0, -18, 11, 9, -275},
  };
  for (const SpawnerPin& pin : kSpawners) {
    Chunk chunk;
    reset_chunk(chunk, pin.chunk_x, pin.chunk_z);
    fill_cave_terrain(chunk);
    wg_place_mineshaft(chunk, start, cave_heights, sampler);
    const int32_t local_x = pin.x - pin.chunk_x * 16;
    const int32_t local_z = pin.z - pin.chunk_z * 16;
    CHECK(chunk.block_state_at(local_x, pin.y, local_z).name == "minecraft:spawner");
    const nbt::Value* spawner = nullptr;
    for (const nbt::Value& be : chunk.block_entities)
      if (block_entity_matches(be, pin.x, pin.y, pin.z))
        spawner = &be;
    CHECK(spawner != nullptr);
    if (spawner != nullptr) {
      const nbt::Value* data = nbt::child(*spawner, "SpawnData");
      const nbt::Value* entity = data == nullptr ? nullptr : nbt::child(*data, "entity");
      const nbt::Value* id = entity == nullptr ? nullptr : nbt::child(*entity, "id");
      const auto name = id == nullptr ? std::nullopt : nbt::as_string(*id);
      CHECK(name && *name == "minecraft:cave_spider");
    }
  }

  // The four abandoned-mineshaft chests (the port's stand-in for vanilla's
  // chest minecarts). The cells and their non-empty `Items` are checked on
  // fresh chunks; the minecart seeds are the server's own draws.
  struct ChestPin {
    int32_t chunk_x;
    int32_t chunk_z;
    int32_t x;
    int32_t y;
    int32_t z;
    int64_t seed;
  };
  static const ChestPin kChests[] = {
      {-5, -20, -74, 6, -307, 6862518239263520418ll},
      {-6, -19, -92, 2, -301, 8073613727382735302ll},
      {-6, -16, -82, 8, -251, -8011288146450306704ll},
      {-3, -15, -42, -2, -230, 6638141729432287331ll},
  };
  for (const ChestPin& pin : kChests) {
    Chunk chunk;
    reset_chunk(chunk, pin.chunk_x, pin.chunk_z);
    fill_cave_terrain(chunk);
    wg_place_mineshaft(chunk, start, cave_heights, sampler);
    const int32_t local_x = pin.x - pin.chunk_x * 16;
    const int32_t local_z = pin.z - pin.chunk_z * 16;
    CHECK(chunk.block_state_at(local_x, pin.y, local_z).name == "minecraft:chest");
    CHECK_EQ(chunk.block_entities.size(), 1u);
    CHECK(chunk.block_entities.size() == 1u &&
          block_entity_matches(chunk.block_entities[0], pin.x, pin.y, pin.z));
    if (chunk.block_entities.size() == 1u) {
      const nbt::Value* items = nbt::child(chunk.block_entities[0], "Items");
      CHECK(items != nullptr && items->type == nbt::Tag::List && !items->items.empty());
      // The seed belongs to the piece's own stream, not the position mix the
      // template chests use. Re-rolling the table with the pinned draw has to
      // reproduce the whole `Items` list, slots and components included; the
      // block entity is otherwise identical to a freshly built one.
      const std::vector<ItemStack> expected =
          wg_structure_loot_items("minecraft:chests/abandoned_mineshaft", pin.seed);
      CHECK(!expected.empty());
      const nbt::Value expected_be = make_container_block_entity(
          "minecraft:chest", BlockPosKey{pin.x, pin.y, pin.z}, expected);
      CHECK(nbt::serialize(chunk.block_entities[0]) == nbt::serialize(expected_be));
    }
  }

  // The bridge's scan finds every start whose pieces touch the target from
  // the shared window, in the same z-then-x order, and applies them in that
  // order. Two mineshafts overlap chunk (-3, -17); comparing against only the
  // source start was the first version's bug.
  Chunk applied;
  fill_generated_surface_chunk(generator, -3, -17, applied);
  const WgStructureApplyStats stats = apply_generated_structures(generator, applied);
  std::vector<WgMineshaftStart> touching;
  const int32_t base_x = -3 * 16;
  const int32_t base_z = -17 * 16;
  for (int32_t dz = -8; dz <= 8; ++dz) {
    for (int32_t dx = -8; dx <= 8; ++dx) {
      const WgMineshaftStart candidate =
          wg_mineshaft_start(42, -3 + dx, -17 + dz, sampler, heights);
      if (!candidate.valid || candidate.max_x < base_x || candidate.min_x >= base_x + 16 ||
          candidate.max_z < base_z || candidate.min_z >= base_z + 16 || candidate.max_y < kMinY ||
          candidate.min_y >= kMinY + kWgHeight)
        continue;
      touching.push_back(candidate);
    }
  }
  CHECK_EQ(stats.starts, touching.size());
  Chunk direct;
  fill_generated_surface_chunk(generator, -3, -17, direct);
  uint32_t direct_blocks = 0;
  uint32_t direct_pieces = 0;
  for (const WgMineshaftStart& candidate : touching) {
    const uint32_t placed = wg_place_mineshaft(direct, candidate, heights, sampler);
    direct_blocks += placed;
    if (placed > 0)
      ++direct_pieces;
  }
  CHECK_EQ(stats.pieces, direct_pieces);
  CHECK_EQ(stats.blocks, direct_blocks);
  size_t mismatches = 0;
  for (int32_t y = kMinY; y < kMinY + kWgHeight; ++y)
    for (int32_t x = 0; x < 16; ++x)
      for (int32_t z = 0; z < 16; ++z)
        if (!(applied.block_state_at(x, y, z) == direct.block_state_at(x, y, z)))
          ++mismatches;
  CHECK_EQ(mismatches, 0u);
}

std::string outpost_element_name(const WgPoolElementRow& element) {
  if (element.kind == 4)
    return "<FeaturePoolElement>";
  if (element.kind == 3) {
    std::string row = "[";
    for (uint16_t i = 0; i < element.part_count; ++i) {
      const WgPoolElementPart& part = kWgPoolElementParts[element.part_offset + i];
      if (i != 0)
        row.push_back('+');
      row += kWgPieceTemplates[part.template_index].name;
    }
    row.push_back(']');
    return row;
  }
  if (element.kind == 0 || element.template_index == 0xFFFF)
    return "<empty>";
  return kWgPieceTemplates[element.template_index].name;
}

// The probe's `pieceRow`: element, rotation, position and the placed box (the
// latter includes the expansion-hack growth).
std::string outpost_piece_row(const WgJigsawPiece& piece) {
  std::string row =
      piece.element != nullptr ? outpost_element_name(*piece.element) : piece.templ->name;
  row.push_back(':');
  row += std::to_string(piece.rotation);
  row.push_back(':');
  row += std::to_string(piece.x);
  row.push_back(',');
  row += std::to_string(piece.y);
  row.push_back(',');
  row += std::to_string(piece.z);
  row.push_back(':');
  row += std::to_string(piece.box.min_x);
  row.push_back(',');
  row += std::to_string(piece.box.min_y);
  row.push_back(',');
  row += std::to_string(piece.box.min_z);
  row.push_back(',');
  row += std::to_string(piece.box.max_x);
  row.push_back(',');
  row += std::to_string(piece.box.max_y);
  row.push_back(',');
  row += std::to_string(piece.box.max_z);
  return row;
}

uint64_t outpost_start_digest(const WgJigsawStart& start) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (uint32_t i = 0; i < start.piece_count; ++i)
    hash = fnv1a_row(hash, outpost_piece_row(start.pieces[i]));
  return hash;
}

void test_outpost() {
  WgOverworldGenerator generator(42);
  WgClimateSampler sampler(generator.state());
  WgGeneratedHeights heights(generator);

  // `OutpostProbe 42 start <cx> <cz>` over the 26.2 server, one start per
  // rotation. The digest is over the probe's `element:rotation:pos:box` rows in
  // placement order; the boxes carry the expansion-hack growth.
  struct StartPin {
    int32_t chunk_x;
    int32_t chunk_z;
    uint32_t rotation;
    uint32_t pieces;
    uint64_t digest;
  };
  static const StartPin kStarts[] = {
      {870, -2226, 2, 11, 10729771851272496705ull},
      {-1747, -3198, 0, 11, 14022774170490161508ull},
      {-1204, -3185, 1, 13, 8228106130198342466ull},
      {1283, -3118, 3, 15, 13904892987267823231ull},
  };
  for (const StartPin& pin : kStarts) {
    const WgJigsawStart start = wg_outpost_start(42, pin.chunk_x, pin.chunk_z, sampler, heights);
    CHECK(start.valid);
    if (!start.valid)
      continue;
    CHECK_EQ(start.rotation, pin.rotation);
    CHECK_EQ(start.piece_count, pin.pieces);
    CHECK_EQ(outpost_start_digest(start), pin.digest);
    CHECK_EQ(start.pieces[0].templ != nullptr && std::string_view(start.pieces[0].templ->name) ==
                                                     "minecraft:pillager_outpost/base_plate",
             true);
    // The first two rows separately, so a digest mismatch has a place to point:
    // the base plate and its west feature plate.
    if (pin.chunk_x == 870 && pin.chunk_z == -2226) {
      CHECK(outpost_piece_row(start.pieces[0]) ==
            "minecraft:pillager_outpost/"
            "base_plate:2:13920,72,-35616:13905,72,-35631,13920,101,-35616");
      CHECK(outpost_piece_row(start.pieces[1]) ==
            "minecraft:pillager_outpost/"
            "feature_plate:0:13889,74,-35631:13889,74,-35631,13904,79,-35600");
    }
  }

  // The start-biome gate runs at `JigsawPlacement`'s stub position: the centre
  // piece box's centre, one block above the piece's own ground cell. Seed 42's
  // source `(-110, -171)` is a start the old chunk-centre/`height - 1` probe
  // rejected (the two block columns land in different quart cells), so this pin
  // keeps the gate from drifting back to the wrong cell.
  const WgStructure* outpost_structure = wg_structure_by_name("minecraft:pillager_outpost");
  CHECK(outpost_structure != nullptr);
  const WgJigsawStart gate_start = wg_outpost_start(42, -110, -171, sampler, heights);
  CHECK(gate_start.valid);
  CHECK_EQ(gate_start.rotation, 3u);
  CHECK_EQ(gate_start.piece_count, 12u);
  CHECK_EQ(outpost_start_digest(gate_start), 4981215695792486528ull);
  if (outpost_structure != nullptr && gate_start.valid) {
    const WgPieceBox& plate_box = gate_start.pieces[0].box;
    const int32_t plate_x = (plate_box.min_x + plate_box.max_x) / 2;
    const int32_t plate_z = (plate_box.min_z + plate_box.max_z) / 2;
    const int32_t plate_y = heights.height(plate_x, plate_z, WgStructureHeightmap::kWorldSurface);
    CHECK(
        wg_structure_start_biome_is_valid(sampler, *outpost_structure, plate_x, plate_y, plate_z));
    CHECK(!wg_structure_start_biome_is_valid(sampler, *outpost_structure, -110 * 16 + 8,
                                             plate_y - 1, -171 * 16 + 8));
  }

  // The gate runs before the biome probe and rejects a chunk the set does not
  // name; the outpost set's candidate at (0, 0) is not a start.
  const WgStructureSet* set = wg_structure_set_by_name("minecraft:pillager_outposts");
  CHECK(set != nullptr);
  if (set != nullptr)
    CHECK(!wg_structure_is_chunk(42, *set, 0, 0));
  const WgJigsawStart none = wg_outpost_start(42, 0, 0, sampler, heights);
  CHECK(!none.valid);

  // The writer against the probe's synthetic terrain, every chunk of
  // `StructureStart.getBoundingBox()`'s 12-block BEARD_THIN inflation:
  // `OutpostProbe 42 bulk 870 -2226`.
  const WgJigsawStart start = wg_outpost_start(42, 870, -2226, sampler, heights);
  CHECK(start.valid);
  struct ChunkPin {
    int32_t chunk_x;
    int32_t chunk_z;
    uint32_t rows;
    uint64_t digest;
  };
  static const ChunkPin kChunks[] = {
      {867, -2229, 0, 14695981039346656037ull},    {868, -2229, 0, 14695981039346656037ull},
      {869, -2229, 0, 14695981039346656037ull},    {870, -2229, 0, 14695981039346656037ull},
      {871, -2229, 0, 14695981039346656037ull},    {867, -2228, 0, 14695981039346656037ull},
      {868, -2228, 0, 14695981039346656037ull},    {869, -2228, 0, 14695981039346656037ull},
      {870, -2228, 72, 2025769077668297927ull},    {871, -2228, 0, 14695981039346656037ull},
      {867, -2227, 0, 14695981039346656037ull},    {868, -2227, 0, 14695981039346656037ull},
      {869, -2227, 1160, 14634304318562922863ull}, {870, -2227, 33, 8401056880190022266ull},
      {871, -2227, 0, 14695981039346656037ull},    {867, -2226, 0, 14695981039346656037ull},
      {868, -2226, 62, 9602269637847817995ull},    {869, -2226, 2, 14221401644034472027ull},
      {870, -2226, 0, 14695981039346656037ull},    {871, -2226, 0, 14695981039346656037ull},
      {867, -2225, 0, 14695981039346656037ull},    {868, -2225, 0, 14695981039346656037ull},
      {869, -2225, 0, 14695981039346656037ull},    {870, -2225, 0, 14695981039346656037ull},
      {871, -2225, 0, 14695981039346656037ull},
  };
  SyntheticHeights synthetic;
  uint64_t combined = 0xcbf29ce484222325ull;
  size_t total_rows = 0;
  for (const ChunkPin& pin : kChunks) {
    Chunk chunk;
    reset_chunk(chunk, pin.chunk_x, pin.chunk_z);
    fill_synthetic_terrain(chunk);
    wg_place_outpost(chunk, start, synthetic);
    const Dump dump = dump_terrain_changes(chunk);
    if (dump.rows.size() != pin.rows || dump.digest != pin.digest)
      std::fprintf(stderr, "  outpost chunk %d,%d: %zu rows, digest %llu\n", pin.chunk_x,
                   pin.chunk_z, dump.rows.size(), static_cast<unsigned long long>(dump.digest));
    CHECK_EQ(dump.rows.size(), pin.rows);
    CHECK_EQ(dump.digest, pin.digest);
    total_rows += dump.rows.size();
    std::string row = std::to_string(pin.chunk_x);
    row.push_back(',');
    row += std::to_string(pin.chunk_z);
    row.push_back(':');
    row += std::to_string(dump.rows.size());
    row.push_back(':');
    row += std::to_string(static_cast<unsigned long long>(dump.digest));
    combined = fnv1a_row(combined, row);
  }
  CHECK_EQ(total_rows, 1329u);
  CHECK_EQ(combined, 8405266560838885098ull);

  // The bridge's entry point places the outpost in a real generated chunk,
  // and every cell the direct writer changes is the cell apply wrote. The
  // outpost step runs before the igloo, shipwreck, mansion and hut steps, so a
  // chunk where one of those also lands would show their writes as extras;
  // this chunk has none.
  Chunk pristine;
  fill_generated_surface_chunk(generator, 869, -2227, pristine);
  Chunk applied = pristine;
  const WgStructureApplyStats stats = apply_generated_structures(generator, applied);
  CHECK(stats.starts >= 1u);
  CHECK(stats.blocks > 0u);
  Chunk direct = pristine;
  const uint32_t direct_blocks = wg_place_outpost(direct, start, heights);
  CHECK(direct_blocks > 0u);
  size_t outpost_cells = 0;
  size_t mismatches = 0;
  for (int32_t x = 0; x < 16; ++x)
    for (int32_t z = 0; z < 16; ++z)
      for (int32_t y = kMinY; y < kMinY + kWgHeight; ++y) {
        if (direct.block_state_at(x, y, z) == pristine.block_state_at(x, y, z))
          continue;
        ++outpost_cells;
        if (!(applied.block_state_at(x, y, z) == direct.block_state_at(x, y, z)))
          ++mismatches;
      }
  CHECK(outpost_cells > 0u);
  CHECK_EQ(mismatches, 0u);
}

// `WorldgenRandom.setLargeFeatureSeed` for the village set's weighted member
// choice. The set's own RNG (`ChunkGenerator.createStructures`) is separate from
// the assembly stream `JigsawPlacement` draws from, and it is what the retry
// below advances when a member's start is invalid.
int64_t village_wrap_mul(int64_t a, int64_t b) {
  return static_cast<int64_t>(static_cast<uint64_t>(a) * static_cast<uint64_t>(b));
}

void village_seed_large_feature(WgLegacyRandom& random, int64_t seed, int32_t x, int32_t z) {
  random.set_seed(seed);
  const int64_t first = random.next_long();
  const int64_t second = random.next_long();
  random.set_seed(village_wrap_mul(x, first) ^ village_wrap_mul(z, second) ^ seed);
}

// The member vanilla would try *first* for a chunk. Comparing this with the
// member the runtime reports is how the retry is pinned: a start whose member
// is not the first pick can only exist because the first pick's start was
// invalid and the loop removed it.
const WgStructure* first_village_pick(const WgStructureSet& set, int64_t seed, int32_t cx,
                                      int32_t cz) {
  WgLegacyRandom random(0);
  village_seed_large_feature(random, seed, cx, cz);
  int32_t total = 0;
  for (uint32_t i = 0; i < set.entry_count; ++i)
    total += static_cast<int32_t>(kWgStructureSetEntries[set.entry_offset + i].weight);
  if (total <= 0)
    return nullptr;
  int32_t choice = random.next_int(total);
  for (uint32_t i = 0; i < set.entry_count; ++i) {
    const WgStructureSetEntry& entry = kWgStructureSetEntries[set.entry_offset + i];
    choice -= static_cast<int32_t>(entry.weight);
    if (choice < 0)
      return wg_structure_by_name(entry.structure);
  }
  return nullptr;
}

// The Java probe's `pieceRow`: the element's name as `StructurePoolElement`
// renders it (`FeaturePoolElement` keeps its class name and a list element is
// `[part+part]`), then rotation, position and the placed box.
std::string village_piece_row(const WgJigsawPiece& piece) {
  std::string row =
      piece.element != nullptr ? outpost_element_name(*piece.element) : piece.templ->name;
  row += ':';
  row += std::to_string(piece.rotation);
  row += ':';
  row += std::to_string(piece.x);
  row += ',';
  row += std::to_string(piece.y);
  row += ',';
  row += std::to_string(piece.z);
  row += ':';
  row += std::to_string(piece.box.min_x);
  row += ',';
  row += std::to_string(piece.box.min_y);
  row += ',';
  row += std::to_string(piece.box.min_z);
  row += ',';
  row += std::to_string(piece.box.max_x);
  row += ',';
  row += std::to_string(piece.box.max_y);
  row += ',';
  row += std::to_string(piece.box.max_z);
  return row;
}

uint64_t village_start_digest(const WgJigsawStart& start) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (uint32_t i = 0; i < start.piece_count; ++i)
    hash = fnv1a_row(hash, village_piece_row(start.pieces[i]));
  return hash;
}

// `AncientCityProbe.elementName` renders every element that is neither a
// `SinglePoolElement` nor a `ListPoolElement` as `<empty>`: its final `else`
// catches both the empty element and the feature element. The probe's digest is
// over that rendering, so the ancient-city rows mirror it exactly (the village
// helper above keeps `<FeaturePoolElement>`, which is what its own golden rows
// were recorded with).
std::string ancient_city_element_name(const WgPoolElementRow& element) {
  if (element.kind == 3) {
    std::string row = "[";
    for (uint16_t i = 0; i < element.part_count; ++i) {
      const WgPoolElementPart& part = kWgPoolElementParts[element.part_offset + i];
      if (i != 0)
        row.push_back('+');
      row += kWgPieceTemplates[part.template_index].name;
    }
    row.push_back(']');
    return row;
  }
  if (element.kind == 0 || element.kind == 4)
    return "<empty>";
  return kWgPieceTemplates[element.template_index].name;
}

std::string ancient_city_piece_row(const WgJigsawPiece& piece) {
  std::string row =
      piece.element != nullptr ? ancient_city_element_name(*piece.element) : piece.templ->name;
  row += ':';
  row += std::to_string(piece.rotation);
  row += ':';
  row += std::to_string(piece.x);
  row += ',';
  row += std::to_string(piece.y);
  row += ',';
  row += std::to_string(piece.z);
  row += ':';
  row += std::to_string(piece.box.min_x);
  row += ',';
  row += std::to_string(piece.box.min_y);
  row += ',';
  row += std::to_string(piece.box.min_z);
  row += ',';
  row += std::to_string(piece.box.max_x);
  row += ',';
  row += std::to_string(piece.box.max_y);
  row += ',';
  row += std::to_string(piece.box.max_z);
  return row;
}

uint64_t ancient_city_start_digest(const WgJigsawStart& start) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (uint32_t i = 0; i < start.piece_count; ++i)
    hash = fnv1a_row(hash, ancient_city_piece_row(start.pieces[i]));
  return hash;
}

// `minecraft:ancient_city` (deep dark): the size-7 graph over the city_center,
// city, walls and sculk pools. Unlike a village it starts at absolute y -27, has
// no heightmap projection, anchors its centre on the `minecraft:city_anchor`
// connector and carries a 116-block bound. The start, one chunk and the apply
// path are pinned against `AncientCityProbe`, which runs for this family.
void test_ancient_city() {
  WgOverworldGenerator generator(42);
  WgClimateSampler sampler(generator.state());
  WgGeneratedHeights heights(generator);

  // One start per rotation, all from `AncientCityProbe 42 findall 400`. Each
  // digest is over the probe's piece rows in placement order. The probe's
  // `START box=` is the union of these piece boxes inflated by 12 on every
  // side, because `terrain_adaptation: beard_box` adds that padding to
  // `StructureStart.getBoundingBox()`; the port stores the pieces' own boxes,
  // which is what the `min_*`/`max_*` columns below hold (vanilla minus 12).
  struct StartPin {
    int32_t chunk_x;
    int32_t chunk_z;
    uint32_t rotation;
    uint32_t pieces;
    uint64_t digest;
    int32_t min_x, min_y, min_z, max_x, max_y, max_z;
  };
  static const StartPin kStarts[] = {
      {148, -383, 0, 88, 7633151191828944074ull, 2262, -52, -6243, 2472, -22, -6022},
      {51, -47, 1, 91, 17076434367480540452ull, 722, -52, -858, 928, -22, -644},
      {361, -264, 2, 94, 4521671917719848206ull, 5714, -52, -4336, 5883, -22, -4116},
      {-165, 80, 3, 82, 8203093223928220651ull, -2746, -52, 1221, -2530, -22, 1387},
  };
  for (const StartPin& pin : kStarts) {
    const WgJigsawStart pinned =
        wg_ancient_city_start(42, pin.chunk_x, pin.chunk_z, sampler, heights);
    std::printf("ANCIENT %d,%d valid=%d pieces=%u rot=%u digest=%llu\n", pin.chunk_x, pin.chunk_z,
                pinned.valid ? 1 : 0, pinned.piece_count, pinned.rotation,
                static_cast<unsigned long long>(ancient_city_start_digest(pinned)));
    CHECK(pinned.valid);
    if (!pinned.valid)
      continue;
    CHECK(pinned.structure != nullptr);
    if (pinned.structure != nullptr)
      CHECK(std::string_view(pinned.structure->name) == "minecraft:ancient_city");
    CHECK_EQ(pinned.rotation, pin.rotation);
    CHECK_EQ(pinned.piece_count, pin.pieces);
    CHECK_EQ(ancient_city_start_digest(pinned), pin.digest);
    int32_t min_x = pinned.pieces[0].box.min_x;
    int32_t min_y = pinned.pieces[0].box.min_y;
    int32_t min_z = pinned.pieces[0].box.min_z;
    int32_t max_x = pinned.pieces[0].box.max_x;
    int32_t max_y = pinned.pieces[0].box.max_y;
    int32_t max_z = pinned.pieces[0].box.max_z;
    for (uint32_t i = 1; i < pinned.piece_count; ++i) {
      min_x = std::min(min_x, pinned.pieces[i].box.min_x);
      min_y = std::min(min_y, pinned.pieces[i].box.min_y);
      min_z = std::min(min_z, pinned.pieces[i].box.min_z);
      max_x = std::max(max_x, pinned.pieces[i].box.max_x);
      max_y = std::max(max_y, pinned.pieces[i].box.max_y);
      max_z = std::max(max_z, pinned.pieces[i].box.max_z);
    }
    CHECK_EQ(min_x, pin.min_x);
    CHECK_EQ(min_y, pin.min_y);
    CHECK_EQ(min_z, pin.min_z);
    CHECK_EQ(max_x, pin.max_x);
    CHECK_EQ(max_y, pin.max_y);
    CHECK_EQ(max_z, pin.max_z);
  }

  const WgJigsawStart start = wg_ancient_city_start(42, 51, -47, sampler, heights);
  CHECK(start.valid);
  if (!start.valid)
    return;
  CHECK(ancient_city_piece_row(start.pieces[0]) ==
        "minecraft:ancient_city/city_center/city_center_3:1:836,-52,-765:"
        "796,-52,-765,836,-22,-748");
  // The probe's synthetic terrain: stone through the surface, so every city
  // cell is a change and the row set is exactly what the server's
  // `postProcess` wrote. The city is entirely below ground, which is why the
  // terrain-matching branch does not run here.
  struct ChunkPin {
    int32_t chunk_x, chunk_z;
    uint32_t rows;
    uint64_t digest;
  };
  static const ChunkPin kChunks[] = {
      {148, -383, 1846, 16072184790590772418ull},
      {51, -47, 1868, 13296572573022898111ull},
      {361, -264, 2567, 4718214164733434316ull},
      {-165, 80, 2543, 8303484642719323454ull},
  };
  for (const ChunkPin& pin : kChunks) {
    const WgJigsawStart chunk_start =
        wg_ancient_city_start(42, pin.chunk_x, pin.chunk_z, sampler, heights);
    if (!chunk_start.valid)
      continue;
    Chunk chunk;
    reset_chunk(chunk, pin.chunk_x, pin.chunk_z);
    fill_synthetic_terrain(chunk);
    const uint32_t placed = wg_place_ancient_city(chunk, chunk_start, heights);
    const Dump dump = dump_terrain_changes(chunk);
    std::printf("ANCIENT chunk %d,%d placed=%u changed=%zu digest=%llu\n", pin.chunk_x, pin.chunk_z,
                placed, dump.rows.size(), static_cast<unsigned long long>(dump.digest));
    CHECK(placed > 0u);
    CHECK_EQ(dump.rows.size(), pin.rows);
    CHECK_EQ(dump.digest, pin.digest);
  }

  // The bridge's scan has to reach the source chunk from an eight-chunk
  // window; this chunk is the start's own source, so a missing city here means
  // the scan or the gate is wrong, not the writer.
  Chunk pristine;
  fill_generated_surface_chunk(generator, 51, -47, pristine);
  Chunk applied = pristine;
  const WgStructureApplyStats stats = apply_generated_structures(generator, applied);
  Chunk direct = pristine;
  const uint32_t direct_blocks = wg_place_ancient_city(direct, start, heights);
  CHECK(direct_blocks > 0u);
  CHECK(stats.blocks >= direct_blocks);
  size_t city_cells = 0;
  size_t mismatches = 0;
  for (int32_t x = 0; x < 16; ++x)
    for (int32_t z = 0; z < 16; ++z)
      for (int32_t y = kMinY; y < kMinY + kWgHeight; ++y) {
        if (direct.block_state_at(x, y, z) == pristine.block_state_at(x, y, z))
          continue;
        ++city_cells;
        if (!(applied.block_state_at(x, y, z) == direct.block_state_at(x, y, z)))
          ++mismatches;
      }
  CHECK(city_cells > 0u);
  CHECK_EQ(mismatches, 0u);

  const WgJigsawStart again = wg_ancient_city_start(42, 51, -47, sampler, heights);
  CHECK(again.valid);
  CHECK_EQ(ancient_city_start_digest(again), ancient_city_start_digest(start));
  CHECK(!wg_ancient_city_start(42, 0, 0, sampler, heights).valid);
}

// `minecraft:trail_ruins`: the first applied jigsaw family whose processor
// lists carry `capped` passes. The start is absolute y -15 projected onto
// `WORLD_SURFACE_WG`; all pieces are rigid. `TrailRuinsProbe` runs the server's
// own `postProcess`, including the whole-piece capped shuffle.
void test_trail_ruins() {
  WgOverworldGenerator generator(42);
  WgClimateSampler sampler(generator.state());
  WgGeneratedHeights heights(generator);
  const WgJigsawStart start = wg_trail_ruins_start(42, 2, -34, sampler, heights);
  std::printf("TRAIL %d,%d valid=%d pieces=%u rot=%u digest=%llu\n", 2, -34, start.valid ? 1 : 0,
              start.piece_count, start.rotation,
              static_cast<unsigned long long>(ancient_city_start_digest(start)));
  CHECK(start.valid);
  if (!start.valid)
    return;
  CHECK(start.structure != nullptr);
  if (start.structure != nullptr)
    CHECK(std::string_view(start.structure->name) == "minecraft:trail_ruins");
  CHECK_EQ(start.rotation, 2u);
  CHECK_EQ(start.piece_count, 17u);
  CHECK_EQ(ancient_city_start_digest(start), 7106010961696734058ull);
  CHECK(ancient_city_piece_row(start.pieces[0]) ==
        "minecraft:trail_ruins/tower/tower_2:2:32,86,-544:"
        "28,86,-548,32,98,-544");
  if (std::getenv("PLUSMC_WG_TRAIL_DUMP") != nullptr)
    for (uint32_t i = 0; i < start.piece_count; ++i)
      std::printf("TPIECE %u %s\n", i, ancient_city_piece_row(start.pieces[i]).c_str());

  Chunk chunk;
  reset_chunk(chunk, 2, -34);
  fill_synthetic_terrain(chunk);
  const uint32_t placed = wg_place_trail_ruins(chunk, start, heights);
  const Dump dump = dump_terrain_changes(chunk);
  std::printf("TRAIL chunk 2,-34 placed=%u changed=%zu digest=%llu\n", placed, dump.rows.size(),
              static_cast<unsigned long long>(dump.digest));
  CHECK(placed > 0u);
  CHECK_EQ(dump.rows.size(), 334u);
  CHECK_EQ(dump.digest, 6755282691902986146ull);
  if (std::getenv("PLUSMC_WG_TRAIL_DUMP") != nullptr)
    for (const std::string& row : dump.rows)
      std::printf("TCELL %s\n", row.c_str());

  // Determinism and the set gate.
  const WgJigsawStart again = wg_trail_ruins_start(42, 2, -34, sampler, heights);
  CHECK(again.valid);
  CHECK_EQ(ancient_city_start_digest(again), ancient_city_start_digest(start));
  CHECK(!wg_trail_ruins_start(42, 0, 0, sampler, heights).valid);
}

void test_village() {
  WgOverworldGenerator generator(42);
  WgClimateSampler sampler(generator.state());
  WgGeneratedHeights heights(generator);
  SyntheticHeights synthetic;
  const WgStructureSet* set = wg_structure_set_by_name("minecraft:villages");
  CHECK(set != nullptr);
  if (set == nullptr)
    return;

  struct VillageStart {
    int32_t chunk_x;
    int32_t chunk_z;
    const WgStructure* structure;
    uint32_t rotation;
    uint32_t pieces;
    uint64_t digest;
    bool has_feature;
  };
  std::vector<VillageStart> starts;
  for (int32_t cz = -60; cz <= 60; ++cz) {
    for (int32_t cx = -60; cx <= 60; ++cx) {
      if (!wg_structure_is_chunk(42, *set, cx, cz))
        continue;
      const WgJigsawStart start = wg_village_start(42, cx, cz, sampler, heights);
      if (!start.valid)
        continue;
      bool has_feature = false;
      for (uint32_t i = 0; i < start.piece_count; ++i)
        if (start.pieces[i].element != nullptr && start.pieces[i].element->kind == 4)
          has_feature = true;
      starts.push_back(VillageStart{cx, cz, start.structure, start.rotation, start.piece_count,
                                    village_start_digest(start), has_feature});
    }
  }
  CHECK(starts.size() >= 2);
  for (size_t i = 0; i < starts.size() && i < 3; ++i)
    std::printf("VILLAGE %d %d %s rot=%u pieces=%u digest=%llu feature=%d\n", starts[i].chunk_x,
                starts[i].chunk_z,
                starts[i].structure != nullptr ? starts[i].structure->name : "<none>",
                starts[i].rotation, starts[i].pieces,
                static_cast<unsigned long long>(starts[i].digest), starts[i].has_feature ? 1 : 0);

  CHECK(starts.size() >= 2);
  // The five-member set exposes the retry: at least one start in this sweep is
  // built from a member that is *not* the chunk's first weighted pick, which can
  // only happen when the first pick's start failed its biome gate and
  // `ChunkGenerator.createStructures` removed it and drew again.
  size_t retried = 0;
  size_t feature_starts = 0;
  for (const VillageStart& start : starts) {
    if (start.has_feature)
      ++feature_starts;
    if (start.structure != nullptr &&
        start.structure != first_village_pick(*set, 42, start.chunk_x, start.chunk_z))
      ++retried;
  }
  CHECK(retried >= 1);
  // `FeaturePoolElement` candidates are real pieces now: a start that drew one
  // keeps it, with its placed feature applied by the writer.
  CHECK(feature_starts >= 1);

  // Two starts pinned row for row against the server. These are what
  // `VillageProbe 42 start <cx> <cz>` prints from the 26.2 jar: the same
  // weighted retry, the same shuffled pools and jigsaws, and the same box the
  // expansion hack leaves. The first was unchanged by the retry/feature work;
  // the second needs the front-cell y check in the expansion hack, without
  // which the well's bottom piece is grown into the meeting point and dropped.
  struct VillagePin {
    int32_t chunk_x;
    int32_t chunk_z;
    const char* structure;
    uint32_t rotation;
    uint32_t pieces;
    uint64_t digest;
  };
  static const VillagePin kPins[] = {
      {53, -55, "minecraft:village_taiga", 1, 148, 14780570220100668918ull},
      {41, -19, "minecraft:village_plains", 2, 186, 8760117829497996082ull},
  };
  for (const VillagePin& pin : kPins) {
    bool found = false;
    for (const VillageStart& start : starts) {
      if (start.chunk_x != pin.chunk_x || start.chunk_z != pin.chunk_z)
        continue;
      found = true;
      CHECK(start.structure != nullptr);
      if (start.structure != nullptr)
        CHECK(std::string_view(start.structure->name) == pin.structure);
      CHECK_EQ(start.rotation, pin.rotation);
      CHECK_EQ(start.pieces, pin.pieces);
      CHECK_EQ(start.digest, pin.digest);
    }
    CHECK(found);
  }

  // Every chunk the (11-chunk-wide, 9-chunk-deep) start box touches, against
  // `VillageProbe 42 bulk 53 -55`: 99 chunks and 3,634 changed cells. The
  // combined digest is over the `x,z:changed:digest` rows in the same z-then-x
  // order the probe prints, so one wrong cell in any street, house, decor
  // feature or jigsaw final state fails. Feature placement and the writer's
  // per-cell draws are the same synthetic-terrain path the probe uses.
  {
    const WgJigsawStart start = wg_village_start(42, 53, -55, sampler, heights);
    CHECK(start.valid);
    if (start.valid) {
      int32_t min_x = start.pieces[0].box.min_x;
      int32_t min_z = start.pieces[0].box.min_z;
      int32_t max_x = start.pieces[0].box.max_x;
      int32_t max_z = start.pieces[0].box.max_z;
      for (uint32_t i = 1; i < start.piece_count; ++i) {
        min_x = std::min(min_x, start.pieces[i].box.min_x);
        min_z = std::min(min_z, start.pieces[i].box.min_z);
        max_x = std::max(max_x, start.pieces[i].box.max_x);
        max_z = std::max(max_z, start.pieces[i].box.max_z);
      }
      // `StructureStart.getBoundingBox()` inflates by 12 for BEARD_THIN.
      CHECK_EQ(floor_div(min_x - 12, 16), 50);
      CHECK_EQ(floor_div(max_x + 12, 16), 58);
      CHECK_EQ(floor_div(min_z - 12, 16), -60);
      CHECK_EQ(floor_div(max_z + 12, 16), -50);
      uint64_t bulk_combined = 0xcbf29ce484222325ull;
      size_t bulk_rows = 0;
      for (int32_t chunk_z = floor_div(min_z - 12, 16); chunk_z <= floor_div(max_z + 12, 16);
           ++chunk_z)
        for (int32_t chunk_x = floor_div(min_x - 12, 16); chunk_x <= floor_div(max_x + 12, 16);
             ++chunk_x) {
          Chunk chunk;
          reset_chunk(chunk, chunk_x, chunk_z);
          fill_synthetic_terrain(chunk);
          const uint32_t placed = wg_place_village(chunk, start, synthetic);
          const Dump dump = dump_terrain_changes(chunk);
          CHECK(placed >= dump.rows.size());
          bulk_rows += dump.rows.size();
          std::string row = std::to_string(chunk_x);
          row.push_back(',');
          row += std::to_string(chunk_z);
          row.push_back(':');
          row += std::to_string(dump.rows.size());
          row.push_back(':');
          row += std::to_string(static_cast<unsigned long long>(dump.digest));
          bulk_combined = fnv1a_row(bulk_combined, row);
        }
      CHECK_EQ(bulk_rows, 3634u);
      CHECK_EQ(bulk_combined, 656728468050274566ull);
    }
  }

  // Determinism: rebuilding the first start reproduces its digest exactly.
  if (!starts.empty()) {
    const VillageStart& first = starts.front();
    const WgJigsawStart again =
        wg_village_start(42, first.chunk_x, first.chunk_z, sampler, heights);
    CHECK(again.valid);
    CHECK_EQ(village_start_digest(again), first.digest);
  }

  // A chunk the set does not name has no start, exactly as for outposts.
  CHECK(!wg_village_start(42, 0, 0, sampler, heights).valid);
}

// `NetherFortressPieces`: the hand-coded start, pinned against
// `StructurePiecesProbe 42 minecraft:fortress 0 0 anybiome`. The server reports
// 90 pieces and the union below; the digest is over the assembly's
// `type:depth:direction:box` rows in placement order.
std::string fortress_piece_row(const WgFortressPiece& piece) {
  static const char* const kNames[] = {
      "BridgeStraight",
      "BridgeCrossing",
      "RoomCrossing",
      "StairsRoom",
      "MonsterThrone",
      "CastleEntrance",
      "CastleSmallCorridor",
      "CastleSmallCorridorCrossing",
      "CastleSmallCorridorRightTurn",
      "CastleSmallCorridorLeftTurn",
      "CastleCorridorStairs",
      "CastleCorridorTBalcony",
      "CastleStalkRoom",
      "BridgeEndFiller",
  };
  std::string row = kNames[static_cast<size_t>(piece.type)];
  row.push_back(':');
  row += std::to_string(piece.depth);
  row.push_back(':');
  row += std::to_string(piece.direction);
  row.push_back(':');
  row += std::to_string(piece.box.min_x);
  row.push_back(',');
  row += std::to_string(piece.box.min_y);
  row.push_back(',');
  row += std::to_string(piece.box.min_z);
  row.push_back(',');
  row += std::to_string(piece.box.max_x);
  row.push_back(',');
  row += std::to_string(piece.box.max_y);
  row.push_back(',');
  row += std::to_string(piece.box.max_z);
  return row;
}

uint64_t fortress_start_digest(const WgFortressStart& start) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (uint32_t i = 0; i < start.piece_count; ++i)
    hash = fnv1a_row(hash, fortress_piece_row(start.pieces[i]));
  return hash;
}

void test_fortress() {
  const WgFortressStart start = wg_fortress_start(42, 0, 0);
  CHECK(start.valid);
  CHECK_EQ(start.piece_count, 90u);
  CHECK_EQ(fortress_start_digest(start), 18013487240119309735ull);
  int32_t min_x = start.pieces[0].box.min_x;
  int32_t min_y = start.pieces[0].box.min_y;
  int32_t min_z = start.pieces[0].box.min_z;
  int32_t max_x = start.pieces[0].box.max_x;
  int32_t max_y = start.pieces[0].box.max_y;
  int32_t max_z = start.pieces[0].box.max_z;
  for (uint32_t i = 1; i < start.piece_count; ++i) {
    min_x = std::min(min_x, start.pieces[i].box.min_x);
    min_y = std::min(min_y, start.pieces[i].box.min_y);
    min_z = std::min(min_z, start.pieces[i].box.min_z);
    max_x = std::max(max_x, start.pieces[i].box.max_x);
    max_y = std::max(max_y, start.pieces[i].box.max_y);
    max_z = std::max(max_z, start.pieces[i].box.max_z);
  }
  CHECK_EQ(min_x, -48);
  CHECK_EQ(min_y, 48);
  CHECK_EQ(min_z, -38);
  CHECK_EQ(max_x, 66);
  CHECK_EQ(max_y, 84);
  CHECK_EQ(max_z, 114);
  CHECK(start.pieces[0].type == WgFortressPieceType::kBridgeCrossing);
  CHECK_EQ(start.pieces[0].direction, 2);

  // Every chunk the union touches, written by the hand-coded postProcess. The
  // bridge chests and the throne's blaze spawner are block entities the writer
  // must create, not just block states.
  size_t chests = 0;
  size_t spawners = 0;
  uint32_t blocks = 0;
  for (int32_t cz = -3; cz <= 7; ++cz)
    for (int32_t cx = -3; cx <= 4; ++cx) {
      Chunk chunk;
      reset_chunk(chunk, cx, cz);
      fill_synthetic_terrain(chunk);
      blocks += wg_place_fortress(chunk, start);
      for (const nbt::Value& be : chunk.block_entities) {
        const nbt::Value* id = nbt::child(be, "id");
        const auto name = id == nullptr ? std::nullopt : nbt::as_string(*id);
        if (!name)
          continue;
        if (*name == "minecraft:chest") {
          ++chests;
          const nbt::Value* items = nbt::child(be, "Items");
          CHECK(items != nullptr && items->type == nbt::Tag::List && !items->items.empty());
        } else if (*name == "minecraft:spawner") {
          ++spawners;
        }
      }
    }
  CHECK(blocks > 0u);
  CHECK(chests > 0u);
  CHECK(spawners > 0u);
}

// `minecraft:bastion_remnant`: the size-6 jigsaw, pinned against
// `StructurePiecesProbe 42 minecraft:bastion_remnant 0 0 anybiome`. The probe
// opens the server's biome predicate (`anybiome`), which is what `skip_biome`
// does here; the production call still checks the nether biome tag.
std::string bastion_piece_row(const WgJigsawPiece& piece) {
  std::string row = piece.templ != nullptr ? piece.templ->name : "<feature>";
  row.push_back(':');
  row += std::to_string(piece.rotation);
  row.push_back(':');
  row += std::to_string(piece.x);
  row.push_back(',');
  row += std::to_string(piece.y);
  row.push_back(',');
  row += std::to_string(piece.z);
  row.push_back(':');
  row += std::to_string(piece.box.min_x);
  row.push_back(',');
  row += std::to_string(piece.box.min_y);
  row.push_back(',');
  row += std::to_string(piece.box.min_z);
  row.push_back(',');
  row += std::to_string(piece.box.max_x);
  row.push_back(',');
  row += std::to_string(piece.box.max_y);
  row.push_back(',');
  row += std::to_string(piece.box.max_z);
  return row;
}

uint64_t bastion_start_digest(const WgJigsawStart& start) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (uint32_t i = 0; i < start.piece_count; ++i)
    hash = fnv1a_row(hash, bastion_piece_row(start.pieces[i]));
  return hash;
}

void test_bastion() {
  WgOverworldGenerator generator(42);
  WgClimateSampler sampler(generator.state());
  SyntheticHeights heights;

  const WgJigsawStart start = wg_bastion_start(42, 0, 0, sampler, heights, true);
  CHECK(start.valid);
  CHECK_EQ(start.piece_count, 83u);
  CHECK_EQ(start.rotation, 2u);
  CHECK_EQ(bastion_start_digest(start), 504431930021510499ull);
  CHECK(start.pieces[0].templ != nullptr &&
        std::string_view(start.pieces[0].templ->name) == "minecraft:bastion/units/air_base");

  int32_t min_x = start.pieces[0].box.min_x;
  int32_t min_y = start.pieces[0].box.min_y;
  int32_t min_z = start.pieces[0].box.min_z;
  int32_t max_x = start.pieces[0].box.max_x;
  int32_t max_y = start.pieces[0].box.max_y;
  int32_t max_z = start.pieces[0].box.max_z;
  for (uint32_t i = 1; i < start.piece_count; ++i) {
    min_x = std::min(min_x, start.pieces[i].box.min_x);
    min_y = std::min(min_y, start.pieces[i].box.min_y);
    min_z = std::min(min_z, start.pieces[i].box.min_z);
    max_x = std::max(max_x, start.pieces[i].box.max_x);
    max_y = std::max(max_y, start.pieces[i].box.max_y);
    max_z = std::max(max_z, start.pieces[i].box.max_z);
  }
  CHECK_EQ(min_x, -45);
  CHECK_EQ(min_y, 32);
  CHECK_EQ(min_z, -49);
  CHECK_EQ(max_x, 21);
  CHECK_EQ(max_y, 87);
  CHECK_EQ(max_z, 0);

  // The overworld sampler has no nether biome, so the same call with the real
  // gate must reject this chunk. That is the gate, not an assembly failure.
  CHECK(!wg_bastion_start(42, 0, 0, sampler, heights, false).valid);
  // The nether complex set's frequency gate still applies before the build.
  const WgStructureSet* set = wg_structure_set_by_name("minecraft:nether_complexes");
  CHECK(set != nullptr);
  if (set != nullptr)
    CHECK(wg_structure_is_chunk(42, *set, 0, 0));

  // The bastion's treasure/bridge/stable/other chests are template block
  // entities. Write every chunk of the union and require the four bastion
  // tables to survive the jigsaw writer.
  size_t chests = 0;
  uint32_t blocks = 0;
  for (int32_t cz = -4; cz <= 0; ++cz)
    for (int32_t cx = -3; cx <= 1; ++cx) {
      Chunk chunk;
      reset_chunk(chunk, cx, cz);
      fill_synthetic_terrain(chunk);
      blocks += wg_place_jigsaw(chunk, start, heights);
      for (const nbt::Value& be : chunk.block_entities) {
        const nbt::Value* id = nbt::child(be, "id");
        const auto name = id == nullptr ? std::nullopt : nbt::as_string(*id);
        if (!name || *name != "minecraft:chest")
          continue;
        ++chests;
        const nbt::Value* items = nbt::child(be, "Items");
        CHECK(items != nullptr && items->type == nbt::Tag::List && !items->items.empty());
      }
    }
  CHECK(blocks > 0u);
  CHECK(chests >= 4u);
}

// `EndCityPieces`: the recursive house/tower/bridge graph, pinned against
// `StructurePiecesProbe 42 minecraft:end_city 7 3 anybiome`. The probe opens
// the biome predicate; `skip_biome` is the same opening here. The end city is
// the last Phase 4 structure family.
uint64_t end_city_start_digest(const WgJigsawStart& start) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (uint32_t i = 0; i < start.piece_count; ++i)
    hash = fnv1a_row(hash, bastion_piece_row(start.pieces[i]));
  return hash;
}

void test_end_city() {
  WgOverworldGenerator generator(42);
  WgClimateSampler sampler(generator.state());
  WgGeneratedHeights heights(generator);

  const WgJigsawStart start = wg_end_city_start(42, 7, 3, sampler, heights, true);
  CHECK(start.valid);
  CHECK_EQ(start.piece_count, 120u);
  CHECK_EQ(start.rotation, 3u);
  CHECK_EQ(end_city_start_digest(start), 2410144656309969380ull);
  CHECK(start.pieces[0].templ != nullptr &&
        std::string_view(start.pieces[0].templ->name) == "minecraft:end_city/base_floor");
  CHECK_EQ(start.pieces[0].box.min_x, 119);
  CHECK_EQ(start.pieces[0].box.min_y, 73);
  CHECK_EQ(start.pieces[0].box.min_z, 46);

  int32_t min_x = start.pieces[0].box.min_x;
  int32_t min_y = start.pieces[0].box.min_y;
  int32_t min_z = start.pieces[0].box.min_z;
  int32_t max_x = start.pieces[0].box.max_x;
  int32_t max_y = start.pieces[0].box.max_y;
  int32_t max_z = start.pieces[0].box.max_z;
  for (uint32_t i = 1; i < start.piece_count; ++i) {
    min_x = std::min(min_x, start.pieces[i].box.min_x);
    min_y = std::min(min_y, start.pieces[i].box.min_y);
    min_z = std::min(min_z, start.pieces[i].box.min_z);
    max_x = std::max(max_x, start.pieces[i].box.max_x);
    max_y = std::max(max_y, start.pieces[i].box.max_y);
    max_z = std::max(max_z, start.pieces[i].box.max_z);
  }
  CHECK_EQ(min_x, 30);
  CHECK_EQ(min_y, 73);
  CHECK_EQ(min_z, 6);
  CHECK_EQ(max_x, 175);
  CHECK_EQ(max_y, 173);
  CHECK_EQ(max_z, 94);

  const WgStructureSet* set = wg_structure_set_by_name("minecraft:end_cities");
  CHECK(set != nullptr);
  if (set != nullptr)
    CHECK(wg_structure_is_chunk(42, *set, 7, 3));
  // The overworld sampler cannot satisfy the end biome tag.
  CHECK(!wg_end_city_start(42, 7, 3, sampler, heights, false).valid);

  // Every chunk the start touches, through the same `wg_place_jigsaw` path the
  // other template pieces use. The ship/tower chests must carry contents; the
  // overwrite-air pieces write their air cells (the start's first row).
  uint32_t blocks = 0;
  for (int32_t cz = 0; cz <= 5; ++cz)
    for (int32_t cx = 1; cx <= 10; ++cx) {
      Chunk chunk;
      reset_chunk(chunk, cx, cz);
      fill_synthetic_terrain(chunk);
      blocks += wg_place_jigsaw(chunk, start, heights);
    }
  CHECK(blocks > 0u);
}

void test_apply_path() {
  // The bridge's entry point: a candidate chunk of a real generated world sees
  // the swamp hut start, and every cell that hut writes matches the direct
  // writer. Other structure sets (mineshafts in particular) may also write
  // this chunk; the hut is applied last, so its cells must still be exact.
  WgOverworldGenerator generator(42);
  Chunk chunk;
  fill_generated_surface_chunk(generator, -51, -23, chunk);
  const WgStructureApplyStats stats = apply_generated_structures(generator, chunk);
  CHECK(stats.starts >= 1u);
  CHECK(stats.pieces >= 1u);
  CHECK(stats.blocks > 0u);

  WgGeneratedHeights heights(generator);
  WgClimateSampler sampler(generator.state());
  const WgSwampHutStart start = wg_swamp_hut_start(42, -51, -23, sampler, heights);
  CHECK(start.valid);
  Chunk pristine;
  fill_generated_surface_chunk(generator, -51, -23, pristine);
  Chunk direct = pristine;
  const uint32_t hut_blocks = wg_place_swamp_hut(direct, start);
  CHECK(hut_blocks > 0u);
  CHECK(stats.blocks >= hut_blocks);

  size_t hut_cells = 0;
  size_t mismatches = 0;
  for (int32_t x = 0; x < 16; ++x)
    for (int32_t z = 0; z < 16; ++z)
      for (int32_t y = kMinY; y < kMinY + kWgHeight; ++y) {
        if (direct.block_state_at(x, y, z) == pristine.block_state_at(x, y, z))
          continue;
        ++hut_cells;
        if (!(chunk.block_state_at(x, y, z) == direct.block_state_at(x, y, z)))
          ++mismatches;
      }
  CHECK(hut_cells > 0u);
  CHECK_EQ(mismatches, 0u);

  // A chunk that is not a swamp-hut candidate has no swamp-hut start; the
  // direct start gate is independent of whatever other sets write there.
  const WgSwampHutStart none = wg_swamp_hut_start(42, 0, 0, sampler, heights);
  CHECK(!none.valid);
}

} // namespace

int main() {
  test_writer_directions();
  test_starts();
  test_igloo_and_buried_treasure();
  test_jungle_temple();
  test_desert_pyramid();
  test_mineshaft();
  test_stronghold();
  test_mansion();
  test_ocean_monument();
  test_outpost();
  test_ancient_city();
  test_trail_ruins();
  test_village();
  test_fortress();
  test_bastion();
  test_end_city();
  test_apply_path();
  std::printf("worldgen-structure: %d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
