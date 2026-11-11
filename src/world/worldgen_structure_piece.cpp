// SPDX-License-Identifier: GPL-3.0-or-later
//
// `ShipwreckStructure.findGenerationPoint` and the `TemplateStructurePiece`
// writer over `worldgen_structure_piece_table.h`. See the header for the
// slice's boundary and docs/PHASE4.md for what is pinned against the 26.2
// server.
#include "world/worldgen_structure_piece.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "world/chunk.h"
#include "world/container.h"
#include "world/generated_chunk.h"
#include "world/registry.h"
#include "world/worldgen_chunk.h"
#include "world/worldgen_feature.h"
#include "world/worldgen_loot.h"
#include "world/worldgen_random.h"
#include "world/worldgen_structure.h"

namespace mc::world {

namespace {

constexpr int32_t kInitialY = 90; // `ShipwreckStructure.generatePieces`' offset y

// `IglooPieces.OFFSETS` and `PIVOTS`, keyed by template name. The pivots also
// live in the generated table; these are the offsets the piece constructor
// applies before the template box is built.
struct IglooOffset {
  int32_t x;
  int32_t y;
  int32_t z;
};

IglooOffset igloo_offset(const WgPieceTemplate& templ) {
  if (std::strstr(templ.name, "/middle") != nullptr)
    return IglooOffset{2, -3, 4};
  if (std::strstr(templ.name, "/bottom") != nullptr)
    return IglooOffset{0, -3, -2};
  return IglooOffset{0, 0, 0};
}

constexpr int32_t kMinY = kWgMinY;
constexpr int32_t kHeight = kWgHeight;

// `SwampHutPiece`: `super(..., west, 64, north, 7, 7, 9, direction)`.
constexpr int32_t kSwampHutWidth = 7;
constexpr int32_t kSwampHutDepth = 9;
constexpr int32_t kSwampHutHeight = 7;
constexpr int32_t kSwampHutY = 64;

// `ShipwreckPieces.MARKERS_TO_LOOT`. The generated marker strings are the NBT
// `metadata` values; the chest is the block below the marker, exactly as
// `handleDataMarker` writes it.
const char* marker_loot_table(std::string_view template_name, std::string_view marker) {
  if (std::strncmp(template_name.data(), "minecraft:shipwreck/", 20) == 0) {
    if (marker == "map_chest")
      return "minecraft:chests/shipwreck_map";
    if (marker == "treasure_chest")
      return "minecraft:chests/shipwreck_treasure";
    if (marker == "supply_chest")
      return "minecraft:chests/shipwreck_supply";
    return nullptr;
  }
  // `WoodlandMansionPieces.WoodlandMansionPiece.handleDataMarker`: every
  // marker starting with `Chest` creates the chest at the marker cell with the
  // rotated facing; the table is always the mansion's.
  if (std::strncmp(template_name.data(), "minecraft:woodland_mansion/", 27) == 0 &&
      marker.size() >= 5 && std::strncmp(marker.data(), "Chest", 5) == 0)
    return "minecraft:chests/woodland_mansion";
  // `IglooPieces.IglooPiece.handleDataMarker`: the bottom template's marker is
  // named `chest` and the chest cell itself is already part of the template.
  if (std::strncmp(template_name.data(), "minecraft:igloo/", 16) == 0 && marker == "chest")
    return "minecraft:chests/igloo_chest";
  // `EndCityPiece.handleDataMarker`: a marker starting with `Chest` creates
  // the chest one cell below with the end-city treasure table.
  if (std::strncmp(template_name.data(), "minecraft:end_city/", 20) == 0 && marker.size() >= 5 &&
      std::strncmp(marker.data(), "Chest", 5) == 0)
    return "minecraft:chests/end_city_treasure";
  return nullptr;
}

// Java's `long` arithmetic wraps; the products below are formed in `uint64_t`
// and narrowed once, like worldgen_structure.cpp's helpers.
int64_t wrap_mul(int64_t a, int64_t b) {
  return static_cast<int64_t>(static_cast<uint64_t>(a) * static_cast<uint64_t>(b));
}

// `WorldgenRandom.setLargeFeatureSeed(seed, chunkX, chunkZ)`.
void seed_large_feature(WgLegacyRandom& random, int64_t seed, int32_t x, int32_t z) {
  random.set_seed(seed);
  const int64_t first = random.next_long();
  const int64_t second = random.next_long();
  random.set_seed(wrap_mul(x, first) ^ wrap_mul(z, second) ^ seed);
}

int32_t floor_div16(int32_t value) {
  return value >= 0 ? value / 16 : -((15 - value) / 16);
}

struct XZ {
  int32_t x;
  int32_t z;
};

// `StructureTemplate.transform(pos, mirror, rotation, pivot)`: mirror first
// (`LEFT_RIGHT` reflects Z, `FRONT_BACK` reflects X), then the decompiled
// quarter-turn formulas about the pivot. `mirror = 0` is the shipwreck path.
XZ rotate_pivot(int32_t x, int32_t z, uint32_t rotation, int32_t pivot_x, int32_t pivot_z,
                uint8_t mirror = 0) {
  if (mirror == 1)
    z = -z;
  else if (mirror == 2)
    x = -x;
  switch (rotation & 3u) {
  case 1: // CLOCKWISE_90
    return XZ{pivot_x + pivot_z - z, pivot_z - pivot_x + x};
  case 2: // CLOCKWISE_180
    return XZ{pivot_x + pivot_x - x, pivot_z + pivot_z - z};
  case 3: // COUNTERCLOCKWISE_90
    return XZ{pivot_x - pivot_z + z, pivot_x + pivot_z - x};
  default:
    return XZ{x, z};
  }
}

const char* rotate_direction(const char* direction, uint32_t rotation) {
  const bool north = std::strcmp(direction, "north") == 0;
  const bool east = std::strcmp(direction, "east") == 0;
  const bool south = std::strcmp(direction, "south") == 0;
  const bool west = std::strcmp(direction, "west") == 0;
  if (!north && !east && !south && !west)
    return direction; // up/down are rotation-invariant
  switch (rotation & 3u) {
  case 1: // clockwise: north -> east, east -> south, ...
    if (north)
      return "east";
    if (east)
      return "south";
    if (south)
      return "west";
    return "north";
  case 2:
    if (north)
      return "south";
    if (east)
      return "west";
    if (south)
      return "north";
    return "east";
  case 3: // counterclockwise
    if (north)
      return "west";
    if (east)
      return "north";
    if (south)
      return "east";
    return "south";
  default:
    return direction;
  }
}

bool is_horizontal_connection(const std::string& key) {
  return key == "north" || key == "east" || key == "south" || key == "west";
}

std::map<std::string, std::string> parse_properties(std::string_view properties) {
  std::map<std::string, std::string> out;
  size_t start = 0;
  while (start < properties.size()) {
    const size_t comma = properties.find(',', start);
    const std::string_view entry = properties.substr(
        start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
    const size_t equals = entry.find('=');
    if (equals != std::string_view::npos)
      out.emplace(std::string(entry.substr(0, equals)), std::string(entry.substr(equals + 1)));
    if (comma == std::string_view::npos)
      break;
    start = comma + 1;
  }
  return out;
}

std::string join_properties(const std::map<std::string, std::string>& properties) {
  std::string out;
  for (const auto& [key, value] : properties) {
    if (!out.empty())
      out.push_back(',');
    out += key;
    out.push_back('=');
    out += value;
  }
  return out;
}

bool has_property(const std::string& properties, std::string_view key) {
  if (properties.empty())
    return false;
  size_t start = 0;
  while (start <= properties.size()) {
    const size_t comma = properties.find(',', start);
    const std::string_view entry =
        std::string_view(properties)
            .substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (entry.size() > key.size() && entry.compare(0, key.size(), key) == 0 &&
        entry[key.size()] == '=')
      return true;
    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }
  return false;
}

std::string set_property(const std::string& properties, std::string_view key,
                         std::string_view value) {
  std::map<std::string, std::string> parsed = parse_properties(properties);
  parsed[std::string(key)] = std::string(value);
  return join_properties(parsed);
}

// `BlockStateParser`: a state string is a block plus the properties it
// overrides; every omitted property is the block's default. The jigsaw
// `final_state` values are the reachable case — a village connector writes
// `minecraft:grass_block` or `cobblestone_stairs[facing=east]` and the server
// resolves the rest of the state before placing it.
std::string canonical_state_properties(std::string_view name, std::string_view properties) {
  const auto def = default_block_state(name);
  if (!def)
    return std::string(properties);
  std::map<std::string, std::string> merged = parse_properties(def->key.properties);
  for (const auto& [key, value] : parse_properties(properties))
    merged[key] = value;
  return join_properties(merged);
}

bool box_hits_chunk(const WgShipwreckStart& start, int32_t chunk_x, int32_t chunk_z) {
  const int32_t min_x = chunk_x * 16;
  const int32_t min_z = chunk_z * 16;
  return start.max_x >= min_x && start.min_x < min_x + 16 && start.max_z >= min_z &&
         start.min_z < min_z + 16 && start.max_y >= kMinY && start.min_y < kMinY + kHeight;
}

bool is_beached(const WgShipwreckStart& start) {
  return std::strcmp(start.structure->name, "minecraft:shipwreck_beached") == 0;
}

const char* mirror_direction(const std::string& direction) {
  // `Mirror.LEFT_RIGHT` reflects the Z axis: north <-> south; east and west are
  // unchanged.
  if (direction == "north")
    return "south";
  if (direction == "south")
    return "north";
  return direction.c_str();
}

// `RailBlock.mirror(LEFT_RIGHT)` and `.rotate(...)` for the eight rail shapes.
// The `shape` property is a world direction (unlike a stair's `shape`, which is
// relative to its `facing`), so it is the one property the generic transforms
// must move. A value the rail block does not carry is returned unchanged, which
// keeps stair and other shapes out of this table.
const char* mirror_rail_shape(const std::string& shape) {
  if (shape == "ascending_north")
    return "ascending_south";
  if (shape == "ascending_south")
    return "ascending_north";
  if (shape == "north_east")
    return "south_east";
  if (shape == "north_west")
    return "south_west";
  if (shape == "south_east")
    return "north_east";
  if (shape == "south_west")
    return "north_west";
  return shape.c_str();
}

const char* rotate_rail_shape(const std::string& shape, uint32_t rotation) {
  static constexpr const char* kStraight[4] = {"north_south", "east_west", "north_south",
                                               "east_west"};
  static constexpr const char* kAscending[4] = {"ascending_north", "ascending_east",
                                                "ascending_south", "ascending_west"};
  static constexpr const char* kNorthEast[4] = {"north_east", "south_east", "south_west",
                                                "north_west"};
  static constexpr const char* kNorthWest[4] = {"north_west", "north_east", "south_east",
                                                "south_west"};
  const uint32_t turns = rotation & 3u;
  if (shape == "north_south")
    return kStraight[turns];
  if (shape == "east_west")
    // The two straights alternate, so the east/west track starts one phase
    // later: an odd quarter turn swaps it and an even one leaves it.
    return kStraight[(turns + 1u) & 3u];
  for (uint32_t i = 0; i < 4; ++i) {
    if (shape == kAscending[i])
      return kAscending[(i + turns) & 3u];
    if (shape == kNorthEast[i])
      return kNorthEast[(i + turns) & 3u];
    if (shape == kNorthWest[i])
      return kNorthWest[(i + turns) & 3u];
  }
  return shape.c_str();
}

// `BlockState.mirror(Mirror.LEFT_RIGHT)` for the states these pieces carry.
// Facing and a cross-shape's four connections reflect across Z; a stair whose
// facing is on the Z axis also swaps its left/right shape (`StairBlock.mirror`).
// `axis` is invariant under a reflection; a rail's shape is not.
std::string mirror_left_right(std::string_view properties) {
  if (properties.empty())
    return std::string(properties);
  const std::map<std::string, std::string> parsed = parse_properties(properties);
  const auto facing = parsed.find("facing");
  const bool stair_on_z =
      facing != parsed.end() && (facing->second == "north" || facing->second == "south");

  std::map<std::string, std::string> out;
  for (const auto& [key, value] : parsed) {
    if (key == "facing")
      out[key] = mirror_direction(value);
    else if (key == "hinge")
      // `DoorBlock.mirror(LEFT_RIGHT)` ends with `.cycle(HINGE)`, whatever the
      // rotation part was. `rotate` preserves the hinge, so this is the one
      // property the mirror must move itself.
      out[key] = value == "left" ? "right" : "left";
    else if (key == "north")
      out["south"] = value;
    else if (key == "south")
      out["north"] = value;
    else if (key == "shape" && value == "north_south")
      out[key] = value;
    else if (key == "shape" && value == "east_west")
      out[key] = value;
    else if (key == "shape" &&
             (value == "ascending_north" || value == "ascending_south" ||
              value == "ascending_east" || value == "ascending_west" || value == "north_east" ||
              value == "north_west" || value == "south_east" || value == "south_west"))
      out[key] = mirror_rail_shape(value);
    else
      out[key] = value;
  }
  if (stair_on_z) {
    const auto shape = out.find("shape");
    if (shape != out.end()) {
      if (shape->second == "inner_left")
        shape->second = "inner_right";
      else if (shape->second == "inner_right")
        shape->second = "inner_left";
      else if (shape->second == "outer_left")
        shape->second = "outer_right";
      else if (shape->second == "outer_right")
        shape->second = "outer_left";
    }
  }
  return join_properties(out);
}

// `RailShape` under `Mirror.FRONT_BACK` (reflect X): east <-> west; the two
// north/south ascending shapes and the two straight shapes are unchanged.
const char* mirror_rail_shape_front_back(const std::string& shape) {
  if (shape == "ascending_east")
    return "ascending_west";
  if (shape == "ascending_west")
    return "ascending_east";
  if (shape == "north_east")
    return "north_west";
  if (shape == "north_west")
    return "north_east";
  if (shape == "south_east")
    return "south_west";
  if (shape == "south_west")
    return "south_east";
  return shape.c_str();
}

// `BlockState.mirror(Mirror.FRONT_BACK)` for the mansion templates. This is
// deliberately separate from `mirror_front_back` below: the ruined-portal
// writer's tighter transform is pinned by its own tests, while the mansion
// carries `CrossCollisionBlock`/door/stair state that needs the full rule.
// `StairBlock.mirror(FRONT_BACK)` keeps a stair's relative inner shape and
// swaps only the outers; a fence/wall/pane swaps its east/west connections;
// a door rotates 180 only when its facing lies on the mirrored axis and always
// cycles its hinge.
std::string mirror_front_back_template(std::string_view properties) {
  if (properties.empty())
    return std::string(properties);
  const std::map<std::string, std::string> parsed = parse_properties(properties);
  const auto facing = parsed.find("facing");
  const bool stair_on_x =
      facing != parsed.end() && (facing->second == "east" || facing->second == "west");

  std::map<std::string, std::string> out;
  for (const auto& [key, value] : parsed) {
    if (key == "facing") {
      if (value == "east")
        out[key] = "west";
      else if (value == "west")
        out[key] = "east";
      else
        out[key] = value;
    } else if (key == "hinge") {
      out[key] = value == "left" ? "right" : "left";
    } else if (key == "east") {
      out["west"] = value;
    } else if (key == "west") {
      out["east"] = value;
    } else if (key == "shape") {
      if (value == "inner_left" || value == "inner_right" || value == "outer_left" ||
          value == "outer_right" || value == "straight") {
        if (!stair_on_x)
          out[key] = value;
        else if (value == "outer_left")
          out[key] = "outer_right";
        else if (value == "outer_right")
          out[key] = "outer_left";
        else
          out[key] = value;
      } else {
        out[key] = mirror_rail_shape_front_back(value);
      }
    } else if (key == "rotation") {
      // `Mirror.FRONT_BACK.mirror(rotation, 16)`.
      const int rotation = std::atoi(value.c_str());
      const int half = 8;
      const int corrected = rotation > half ? rotation - 16 : rotation;
      out[key] = std::to_string((16 - corrected) % 16);
    } else {
      out[key] = value;
    }
  }
  return join_properties(out);
}

std::string wg_piece_mirror_properties(std::string_view properties, uint8_t mirror) {
  if (mirror == 1)
    return mirror_left_right(properties);
  if (mirror == 2)
    return mirror_front_back_template(properties);
  return std::string(properties);
}

// `StructurePiece.setOrientation`: SOUTH and WEST get `Mirror.LEFT_RIGHT`;
// WEST and EAST get `Rotation.CLOCKWISE_90`; NORTH gets neither.
uint32_t orientation_rotation(uint8_t direction) {
  return direction == 1 || direction == 3 ? 1u : 0u;
}

bool orientation_mirrors(uint8_t direction) {
  return direction == 2 || direction == 3;
}

// `StructurePiece.getWorldX`/`getWorldZ` for a `ScatteredFeaturePiece` with
// the swamp hut's 7 x 9 footprint (the box is 9 x 7 for east/west).
struct SwampHutWorldXZ {
  int32_t x;
  int32_t z;
};

SwampHutWorldXZ swamp_hut_world_xz(const WgSwampHutStart& start, int32_t local_x, int32_t local_z) {
  switch (start.direction) {
  case 0: // north
    return SwampHutWorldXZ{start.min_x + local_x, start.max_z - local_z};
  case 1: // east
    return SwampHutWorldXZ{start.min_x + local_z, start.min_z + local_x};
  case 2: // south
    return SwampHutWorldXZ{start.min_x + local_x, start.min_z + local_z};
  default: // west
    return SwampHutWorldXZ{start.max_x - local_z, start.min_z + local_x};
  }
}

constexpr const char* kSwampHutFenceProperties =
    "east=false,north=false,south=false,waterlogged=false,west=false";

std::string stair_properties(const char* facing, const char* shape) {
  return std::string("facing=") + facing + ",half=bottom,shape=" + shape + ",waterlogged=false";
}

bool is_replaceable_by_structures(const BlockStateKey& state) {
  // `StructurePiece.isReplaceableByStructures`: air, liquid, glow lichen, and
  // the two seagrasses. The two extra air blocks are `isAir()` too.
  return state.name == "minecraft:air" || state.name == "minecraft:cave_air" ||
         state.name == "minecraft:void_air" || state.name == "minecraft:water" ||
         state.name == "minecraft:lava" || state.name == "minecraft:glow_lichen" ||
         state.name == "minecraft:seagrass" || state.name == "minecraft:tall_seagrass";
}

// `SwampHutPiece.postProcess`'s block writes. The writer owns the orientation
// transform (world coordinates) and the mirror-then-rotation transform (block
// states), because those are the two halves that are easy to get backwards.
class SwampHutWriter {
public:
  SwampHutWriter(Chunk& chunk_in, const WgSwampHutStart& start_in)
      : chunk(chunk_in), start(start_in) {}

  uint32_t run() {
    generate_box(1, 1, 1, 5, 1, 7, kPlanks, kPlanks);
    generate_box(1, 4, 2, 5, 4, 7, kPlanks, kPlanks);
    generate_box(2, 1, 0, 4, 1, 0, kPlanks, kPlanks);
    generate_box(2, 2, 2, 3, 3, 2, kPlanks, kPlanks);
    generate_box(1, 2, 3, 1, 3, 6, kPlanks, kPlanks);
    generate_box(5, 2, 3, 5, 3, 6, kPlanks, kPlanks);
    generate_box(2, 2, 7, 4, 3, 7, kPlanks, kPlanks);
    generate_box(1, 0, 2, 1, 3, 2, kLog, kLog);
    generate_box(5, 0, 2, 5, 3, 2, kLog, kLog);
    generate_box(1, 0, 7, 1, 3, 7, kLog, kLog);
    generate_box(5, 0, 7, 5, 3, 7, kLog, kLog);

    place(kFence, 2, 3, 2);
    place(kFence, 3, 3, 7);
    place(kAir, 1, 3, 4);
    place(kAir, 5, 3, 4);
    place(kAir, 5, 3, 5);
    place(kPottedRedMushroom, 1, 3, 5);
    place(kCraftingTable, 3, 2, 6);
    place(kCauldron, 4, 2, 6);
    place(kFence, 1, 2, 1);
    place(kFence, 5, 2, 1);

    const BlockStateKey north_stairs{"minecraft:spruce_stairs",
                                     stair_properties("north", "straight")};
    const BlockStateKey east_stairs{"minecraft:spruce_stairs",
                                    stair_properties("east", "straight")};
    const BlockStateKey west_stairs{"minecraft:spruce_stairs",
                                    stair_properties("west", "straight")};
    const BlockStateKey south_stairs{"minecraft:spruce_stairs",
                                     stair_properties("south", "straight")};
    generate_box(0, 4, 1, 6, 4, 1, north_stairs, north_stairs);
    generate_box(0, 4, 2, 0, 4, 7, east_stairs, east_stairs);
    generate_box(6, 4, 2, 6, 4, 7, west_stairs, west_stairs);
    generate_box(0, 4, 8, 6, 4, 8, south_stairs, south_stairs);
    place(BlockStateKey{"minecraft:spruce_stairs", stair_properties("north", "outer_right")}, 0, 4,
          1);
    place(BlockStateKey{"minecraft:spruce_stairs", stair_properties("north", "outer_left")}, 6, 4,
          1);
    place(BlockStateKey{"minecraft:spruce_stairs", stair_properties("south", "outer_left")}, 0, 4,
          8);
    place(BlockStateKey{"minecraft:spruce_stairs", stair_properties("south", "outer_right")}, 6, 4,
          8);

    for (int32_t z = 2; z <= 7; z += 5)
      for (int32_t x = 1; x <= 5; x += 4)
        fill_column_down(kLog, x, -1, z);

    // The caller's count is "cells whose state changed", not "writes": a
    // corner is written by the stair run and again by its outer-shape override,
    // and a column can be filled after a floor block was placed. Compare each
    // touched cell's final state with what it held before the piece ran.
    uint32_t changed = 0;
    const int32_t base_x = chunk.x * 16;
    const int32_t base_z = chunk.z * 16;
    for (const auto& [position, before] : original) {
      const auto [world_x, world_y, world_z] = position;
      if (!(chunk.block_state_at(world_x - base_x, world_y, world_z - base_z) == before))
        ++changed;
    }
    return changed;
  }

private:
  inline static const BlockStateKey kAir{"minecraft:air", ""};
  inline static const BlockStateKey kPlanks{"minecraft:spruce_planks", ""};
  inline static const BlockStateKey kLog{"minecraft:oak_log", "axis=y"};
  inline static const BlockStateKey kFence{"minecraft:oak_fence", kSwampHutFenceProperties};
  inline static const BlockStateKey kPottedRedMushroom{"minecraft:potted_red_mushroom", ""};
  inline static const BlockStateKey kCraftingTable{"minecraft:crafting_table", ""};
  inline static const BlockStateKey kCauldron{"minecraft:cauldron", ""};

  BlockStateKey transform(const BlockStateKey& state) const {
    std::string properties = state.properties;
    if (orientation_mirrors(start.direction))
      properties = mirror_left_right(properties);
    properties = wg_piece_rotate_properties(properties, orientation_rotation(start.direction));
    return BlockStateKey{state.name, std::move(properties)};
  }

  bool inside_chunk(int32_t world_x, int32_t world_z) const {
    const int32_t base_x = chunk.x * 16;
    const int32_t base_z = chunk.z * 16;
    return world_x >= base_x && world_x < base_x + 16 && world_z >= base_z && world_z < base_z + 16;
  }

  void place(const BlockStateKey& state, int32_t local_x, int32_t local_y, int32_t local_z) {
    const SwampHutWorldXZ world = swamp_hut_world_xz(start, local_x, local_z);
    const int32_t world_y = start.ground_y + local_y;
    if (!inside_chunk(world.x, world.z) || world_y < kMinY || world_y >= kMinY + kHeight)
      return;
    const int32_t chunk_local_x = world.x - chunk.x * 16;
    const int32_t chunk_local_z = world.z - chunk.z * 16;
    const BlockStateKey transformed = transform(state);
    const BlockStateKey before = chunk.block_state_at(chunk_local_x, world_y, chunk_local_z);
    if (before == transformed)
      return;
    if (chunk.set_block_state(chunk_local_x, world_y, chunk_local_z, transformed))
      original.try_emplace({world.x, world_y, world.z}, before);
  }

  void generate_box(int32_t x0, int32_t y0, int32_t z0, int32_t x1, int32_t y1, int32_t z1,
                    const BlockStateKey& edge, const BlockStateKey& fill) {
    for (int32_t y = y0; y <= y1; ++y)
      for (int32_t x = x0; x <= x1; ++x)
        for (int32_t z = z0; z <= z1; ++z) {
          const bool is_edge = y == y0 || y == y1 || x == x0 || x == x1 || z == z0 || z == z1;
          place(is_edge ? edge : fill, x, y, z);
        }
  }

  void fill_column_down(const BlockStateKey& state, int32_t local_x, int32_t local_y,
                        int32_t local_z) {
    const SwampHutWorldXZ world = swamp_hut_world_xz(start, local_x, local_z);
    if (!inside_chunk(world.x, world.z))
      return;
    const int32_t base_x = chunk.x * 16;
    const int32_t base_z = chunk.z * 16;
    int32_t world_y = start.ground_y + local_y;
    BlockStateKey transformed = transform(state);
    while (world_y > kMinY + 1 && is_replaceable_by_structures(chunk.block_state_at(
                                      world.x - base_x, world_y, world.z - base_z))) {
      const BlockStateKey before =
          chunk.block_state_at(world.x - base_x, world_y, world.z - base_z);
      if (!(before == transformed) &&
          chunk.set_block_state(world.x - base_x, world_y, world.z - base_z, transformed))
        original.try_emplace({world.x, world_y, world.z}, before);
      --world_y;
    }
  }

  Chunk& chunk;
  const WgSwampHutStart& start;
  std::map<std::tuple<int32_t, int32_t, int32_t>, BlockStateKey> original;
};

// The two height adjustments `ShipwreckPiece.postProcess` makes. The ocean arm
// is the mean of the unrotated footprint's `OCEAN_FLOOR_WG` heights (no random);
// the beached arm is the footprint's lowest `WORLD_SURFACE_WG` height minus half
// the template height and a 0..2 draw. Vanilla's draw comes from the decorating
// chunk's random; this port derives it from the start's own position so the
// piece lands at the same height from every target chunk (the header's
// ordering note).
int32_t adjusted_y(const WgShipwreckStart& start, WgStructureHeights& heights) {
  const WgPieceTemplate& templ = *start.templ;
  if (is_beached(start)) {
    int32_t lowest = kMinY + kHeight;
    for (int32_t x = 0; x < templ.size_x; ++x)
      for (int32_t z = 0; z < templ.size_z; ++z)
        lowest = std::min(
            lowest, heights.height(start.x + x, start.z + z, WgStructureHeightmap::kWorldSurface));
    WgLegacyRandom random(wg_mth_get_seed(start.x, start.y, start.z));
    return lowest - templ.size_y / 2 - random.next_int(3);
  }
  if (templ.size_x <= 0 || templ.size_z <= 0)
    return heights.height(start.x, start.z, WgStructureHeightmap::kOceanFloor);
  int64_t sum = 0;
  for (int32_t x = 0; x < templ.size_x; ++x)
    for (int32_t z = 0; z < templ.size_z; ++z)
      sum += heights.height(start.x + x, start.z + z, WgStructureHeightmap::kOceanFloor);
  return static_cast<int32_t>(sum / (static_cast<int64_t>(templ.size_x) * templ.size_z));
}

bool is_air_state(const BlockStateKey& state) {
  return state.name == "minecraft:air" || state.name == "minecraft:cave_air" ||
         state.name == "minecraft:void_air";
}

bool is_liquid_state(const BlockStateKey& state) {
  return state.name == "minecraft:water" || state.name == "minecraft:lava";
}

// `BlockState.isSolidRender` without block models. This is
// `isShapeFullBlock(occlusionShape)`, not "the player collides with it": a
// slab, stair, fence, pane, door or chest has a collision shape but its
// occlusion shape is not a full cube, so `StructurePiece.reorient` must not
// count it as the one solid neighbour that picks a chest's facing. Glass and
// ice are full cubes that do not occlude, which the generated no-occlusion
// table already answers. The shapes the structure writers can produce are
// classified by name and the one property that changes a slab's cube.
bool has_suffix(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_solid_render(const Chunk& chunk, int32_t local_x, int32_t y, int32_t local_z) {
  const BlockStateKey& state = chunk.block_state_at(local_x, y, local_z);
  const std::string& name = state.name;
  if (block_no_occlusion(name) || !block_blocks_motion(name))
    return false;
  if (has_suffix(name, "_slab"))
    return state.properties.find("type=double") != std::string::npos;
  if (has_suffix(name, "_stairs") || has_suffix(name, "_fence") ||
      has_suffix(name, "_fence_gate") || has_suffix(name, "_wall") || has_suffix(name, "_pane") ||
      has_suffix(name, "_door") || has_suffix(name, "_trapdoor") || has_suffix(name, "_carpet") ||
      has_suffix(name, "_bed") || has_suffix(name, "_banner") || has_suffix(name, "_sign") ||
      has_suffix(name, "_button") || has_suffix(name, "_pressure_plate") ||
      name == "minecraft:iron_bars" || name == "minecraft:chest" ||
      name == "minecraft:trapped_chest" || name == "minecraft:ender_chest")
    return false;
  return true;
}

void add_container_loot_with_seed(Chunk& chunk, const char* id, int32_t world_x, int32_t world_y,
                                  int32_t world_z, const char* table, int64_t seed) {
  for (const nbt::Value& be : chunk.block_entities)
    if (block_entity_matches(be, world_x, world_y, world_z))
      return;
  chunk.block_entities.push_back(make_container_block_entity(
      id, BlockPosKey{world_x, world_y, world_z}, wg_structure_loot_items(table, seed)));
}

void add_container_loot(Chunk& chunk, const char* id, int32_t world_x, int32_t world_y,
                        int32_t world_z, const char* table, int64_t world_seed) {
  const int64_t seed = wg_structure_loot_seed(world_seed, world_x, world_y, world_z, table);
  add_container_loot_with_seed(chunk, id, world_x, world_y, world_z, table, seed);
}

void add_chest_loot(Chunk& chunk, int32_t world_x, int32_t world_y, int32_t world_z,
                    const char* table, int64_t world_seed) {
  add_container_loot(chunk, "minecraft:chest", world_x, world_y, world_z, table, world_seed);
}

// The block-entity id a template's own `LootTable` belongs to. Only the
// container blocks a structure template ships with are mapped; a table on any
// other block is ignored rather than attached to the wrong entity.
const char* container_id_for_block(const std::string& name) {
  if (name == "minecraft:chest" || name == "minecraft:trapped_chest")
    return name.c_str();
  if (name == "minecraft:barrel")
    return "minecraft:barrel";
  if (name == "minecraft:dispenser")
    return "minecraft:dispenser";
  if (name == "minecraft:dropper")
    return "minecraft:dropper";
  if (name == "minecraft:decorated_pot")
    return "minecraft:decorated_pot";
  return nullptr;
}

void add_dispenser_loot(Chunk& chunk, int32_t world_x, int32_t world_y, int32_t world_z,
                        const char* table, int64_t world_seed) {
  add_container_loot(chunk, "minecraft:dispenser", world_x, world_y, world_z, table, world_seed);
}

// `StructurePiece.reorient`: the chest faces away from its only solid
// horizontal neighbour; when every horizontal cell is solid the fallback
// probes north, south, east and west in turn.
const char* reoriented_chest_facing(Chunk& chunk, int32_t local_x, int32_t y, int32_t local_z) {
  constexpr const char* kDirections[4] = {"north", "east", "south", "west"};
  constexpr int32_t kDx[4] = {0, 1, 0, -1};
  constexpr int32_t kDz[4] = {-1, 0, 1, 0};
  const char* solid = nullptr;
  for (int32_t i = 0; i < 4; ++i) {
    const int32_t x = local_x + kDx[i];
    const int32_t z = local_z + kDz[i];
    if (x < 0 || x >= 16 || z < 0 || z >= 16)
      continue;
    const std::string& name = chunk.block_state_at(x, y, z).name;
    if (name == "minecraft:chest")
      return "north";
    if (!is_solid_render(chunk, x, y, z))
      continue;
    if (solid == nullptr) {
      solid = kDirections[i];
      continue;
    }
    solid = nullptr;
    break;
  }
  if (solid != nullptr) {
    if (std::strcmp(solid, "north") == 0)
      return "south";
    if (std::strcmp(solid, "east") == 0)
      return "west";
    if (std::strcmp(solid, "south") == 0)
      return "north";
    return "east";
  }
  // The default chest faces north. Each probe flips or turns once.
  auto solid_at = [&](const char* facing) {
    int32_t dx = 0;
    int32_t dz = 0;
    if (std::strcmp(facing, "north") == 0)
      dz = -1;
    else if (std::strcmp(facing, "south") == 0)
      dz = 1;
    else if (std::strcmp(facing, "east") == 0)
      dx = 1;
    else
      dx = -1;
    const int32_t x = local_x + dx;
    const int32_t z = local_z + dz;
    return x >= 0 && x < 16 && z >= 0 && z < 16 && is_solid_render(chunk, x, y, z);
  };
  const char* facing = "north";
  if (solid_at(facing))
    facing = "south";
  if (solid_at(facing))
    facing = "west";
  if (solid_at(facing))
    facing = "east";
  return facing;
}

// `StructureTemplate.placeInWorld` for one template, clipped to `chunk`. The
// palette choice is vanilla's own position-seeded draw
// (`StructurePlaceSettings.getRandomPalette` -> `RandomSource.create(
// Mth.getSeed(position))`). `ignore_air` selects `STRUCTURE_AND_AIR`
// (shipwrecks) versus `STRUCTURE_BLOCK` (igloos); `apply_waterlogging` selects
// the default `APPLY_WATERLOGGING` versus `IGNORE_WATERLOGGING`.
uint32_t place_template(Chunk& chunk, const WgPieceTemplate& templ, uint32_t rotation, int32_t x,
                        int32_t y, int32_t z, int64_t world_seed, bool ignore_air,
                        bool apply_waterlogging, uint8_t mirror = 0, bool markers_at_cell = false) {
  if (templ.palette_count == 0)
    return 0;
  WgLegacyRandom palette_random(wg_mth_get_seed(x, y, z));
  const uint32_t palette_index =
      static_cast<uint32_t>(palette_random.next_int(static_cast<int32_t>(templ.palette_count)));
  const WgPiecePalette& palette = kWgPiecePalettes[templ.palette_offset + palette_index];

  const int32_t base_x = chunk.x * 16;
  const int32_t base_z = chunk.z * 16;
  uint32_t placed = 0;
  // `handleDataMarker` runs after the template's blocks are placed, so a
  // marker that happens to precede its chest in the NBT still finds the block.
  struct ChestMarker {
    int32_t x;
    int32_t y;
    int32_t z;
    const char* table;
    const char* facing; // null for the template's own chest (below-marker path)
  };
  std::vector<ChestMarker> chest_markers;
  std::vector<std::tuple<int32_t, int32_t, int32_t>> air_markers;
  for (uint32_t i = 0; i < templ.block_count; ++i) {
    const WgPieceBlock& block = kWgPieceBlocks[templ.block_offset + i];
    const WgState& state =
        kWgPieceStates[kWgPiecePaletteStates[palette.state_offset + block.state]];
    const XZ offset =
        rotate_pivot(block.x, block.z, rotation, templ.pivot_x, templ.pivot_z, mirror);
    const int32_t world_x = x + offset.x;
    const int32_t world_y = y + block.y;
    const int32_t world_z = z + offset.z;
    if (world_x < base_x || world_x >= base_x + 16 || world_z < base_z || world_z >= base_z + 16 ||
        world_y < kMinY || world_y >= kMinY + kHeight)
      continue;
    const int32_t local_x = world_x - base_x;
    const int32_t local_z = world_z - base_z;
    if (block.marker != 0) {
      // `TemplateStructurePiece.handleDataMarker`: the marker block itself is
      // dropped by the associated processor. A shipwreck/igloo marker names a
      // chest one cell below; a mansion marker names the chest cell itself and
      // its facing, and entity markers are cleared without spawning a mob.
      const std::string_view marker = kWgPieceMarkers[block.marker - 1];
      const char* table = marker_loot_table(templ.name, marker);
      if (markers_at_cell) {
        const char* facing = "north";
        if (marker == "ChestWest")
          facing = "west";
        else if (marker == "ChestEast")
          facing = "east";
        else if (marker == "ChestSouth")
          facing = "south";
        if (table != nullptr)
          chest_markers.push_back(
              ChestMarker{world_x, world_y, world_z, table, rotate_direction(facing, rotation)});
        air_markers.emplace_back(world_x, world_y, world_z);
      } else {
        const int32_t chest_y = world_y - 1;
        if (table != nullptr && chest_y >= kMinY)
          chest_markers.push_back(ChestMarker{world_x, chest_y, world_z, table, nullptr});
        // `IglooPiece.handleDataMarker` clears the marker cell itself; a
        // shipwreck's marker is already air through `STRUCTURE_AND_AIR`.
        if (!ignore_air)
          air_markers.emplace_back(world_x, world_y, world_z);
      }
      continue;
    }
    if (std::strcmp(state.name, "minecraft:structure_block") == 0)
      continue;
    if (ignore_air && is_air_state(BlockStateKey{state.name, ""}))
      continue;
    // `AnvilBlock` inherits the identity mirror (only its `rotate` moves
    // `facing`), unlike the stair/fence/door states around it.
    const bool mirror_is_identity = std::strcmp(state.name, "minecraft:anvil") == 0 ||
                                    std::strcmp(state.name, "minecraft:chipped_anvil") == 0 ||
                                    std::strcmp(state.name, "minecraft:damaged_anvil") == 0;
    std::string properties = mirror_is_identity
                                 ? state.properties
                                 : wg_piece_mirror_properties(state.properties, mirror);
    properties = wg_piece_rotate_properties(properties, rotation);
    // `APPLY_WATERLOGGING`: a cell that held water fills the placed block's
    // waterlogged property. Generation holds source water only, so the
    // flowing-fluid flood after vanilla's loop has nothing to do.
    if (apply_waterlogging && !properties.empty() && has_property(properties, "waterlogged") &&
        chunk.block_state_at(local_x, world_y, local_z).name == "minecraft:water")
      properties = set_property(properties, "waterlogged", "true");
    if (chunk.set_block_state(local_x, world_y, local_z,
                              BlockStateKey{state.name, std::move(properties)}))
      ++placed;
  }
  for (const auto& [air_x, air_y, air_z] : air_markers) {
    const int32_t local_air_x = air_x - base_x;
    const int32_t local_air_z = air_z - base_z;
    if (chunk.block_state_at(local_air_x, air_y, local_air_z).name == "minecraft:air")
      continue;
    if (chunk.set_block_state(local_air_x, air_y, local_air_z, BlockStateKey{"minecraft:air", ""}))
      ++placed;
  }
  for (const ChestMarker& chest : chest_markers) {
    if (chest.facing != nullptr) {
      const std::string properties =
          std::string("facing=") + chest.facing + ",type=single,waterlogged=false";
      if (chunk.set_block_state(chest.x - base_x, chest.y, chest.z - base_z,
                                BlockStateKey{"minecraft:chest", properties}))
        ++placed;
    }
    if (chunk.block_state_at(chest.x - base_x, chest.y, chest.z - base_z).name != "minecraft:chest")
      continue;
    add_chest_loot(chunk, chest.x, chest.y, chest.z, chest.table, world_seed);
  }
  return placed;
}

// `ShipwreckPiece.postProcess`, after `adjusted_y`.
uint32_t place_piece(Chunk& chunk, const WgShipwreckStart& start, WgStructureHeights& heights) {
  return place_template(chunk, *start.templ, start.rotation, start.x, adjusted_y(start, heights),
                        start.z, start.world_seed, true, true);
}

// `ScatteredFeaturePiece`'s coordinate and block-state transforms, shared by
// the swamp hut and the desert pyramid. `place` maps a piece-local cell through
// `getWorldX`/`getWorldZ`, mirrors (SOUTH/WEST) then rotates (EAST/WEST) the
// state, and records each touched cell's original so the caller can count
// changed cells rather than writes.
class ScatteredWriter {
public:
  ScatteredWriter(Chunk& chunk_in, uint8_t direction_in, int32_t min_x_in, int32_t max_x_in,
                  int32_t min_z_in, int32_t max_z_in, int32_t base_y_in)
      : chunk(chunk_in), direction(direction_in), min_x(min_x_in), max_x(max_x_in), min_z(min_z_in),
        max_z(max_z_in), base_y(base_y_in) {}

  uint32_t changed() const {
    return changed_;
  }

  std::pair<int32_t, int32_t> world_xz(int32_t local_x, int32_t local_z) const {
    switch (direction) {
    case 0:
      return {min_x + local_x, max_z - local_z};
    case 1:
      return {min_x + local_z, min_z + local_x};
    case 2:
      return {min_x + local_x, min_z + local_z};
    default:
      return {max_x - local_z, min_z + local_x};
    }
  }

  void place_public(const BlockStateKey& state, int32_t local_x, int32_t local_y, int32_t local_z) {
    place(state, local_x, local_y, local_z);
  }

  // `StructurePiece.createChest` writes its reoriented state with
  // `level.setBlock`, not through `placeBlock`, so the chest's facing is a
  // world direction and must not be mirrored/rotated a second time. The
  // coordinate transform is still the piece's own.
  void place_raw(const BlockStateKey& state, int32_t local_x, int32_t local_y, int32_t local_z) {
    int32_t world_x = 0;
    int32_t world_z = 0;
    switch (direction) {
    case 0:
      world_x = min_x + local_x;
      world_z = max_z - local_z;
      break;
    case 1:
      world_x = min_x + local_z;
      world_z = min_z + local_x;
      break;
    case 2:
      world_x = min_x + local_x;
      world_z = min_z + local_z;
      break;
    default:
      world_x = max_x - local_z;
      world_z = min_z + local_x;
      break;
    }
    const int32_t world_y = base_y + local_y;
    const int32_t base_x = chunk.x * 16;
    const int32_t base_z = chunk.z * 16;
    if (world_x < base_x || world_x >= base_x + 16 || world_z < base_z || world_z >= base_z + 16 ||
        world_y < kMinY || world_y >= kMinY + kHeight)
      return;
    const int32_t local_column_x = world_x - base_x;
    const int32_t local_column_z = world_z - base_z;
    const BlockStateKey before = chunk.block_state_at(local_column_x, world_y, local_column_z);
    if (before == state)
      return;
    if (chunk.set_block_state(local_column_x, world_y, local_column_z, state)) {
      original_.try_emplace({world_x, world_y, world_z}, before);
      ++changed_;
    }
  }

  void place(const BlockStateKey& state, int32_t local_x, int32_t local_y, int32_t local_z) {
    int32_t world_x = 0;
    int32_t world_z = 0;
    switch (direction) {
    case 0: // north
      world_x = min_x + local_x;
      world_z = max_z - local_z;
      break;
    case 1: // east
      world_x = min_x + local_z;
      world_z = min_z + local_x;
      break;
    case 2: // south
      world_x = min_x + local_x;
      world_z = min_z + local_z;
      break;
    default: // west
      world_x = max_x - local_z;
      world_z = min_z + local_x;
      break;
    }
    const int32_t world_y = base_y + local_y;
    const int32_t base_x = chunk.x * 16;
    const int32_t base_z = chunk.z * 16;
    if (world_x < base_x || world_x >= base_x + 16 || world_z < base_z || world_z >= base_z + 16 ||
        world_y < kMinY || world_y >= kMinY + kHeight)
      return;
    BlockStateKey transformed = state;
    if (orientation_mirrors(direction))
      transformed.properties = mirror_left_right(transformed.properties);
    transformed.properties =
        wg_piece_rotate_properties(transformed.properties, orientation_rotation(direction));
    const int32_t local_column_x = world_x - base_x;
    const int32_t local_column_z = world_z - base_z;
    const BlockStateKey before = chunk.block_state_at(local_column_x, world_y, local_column_z);
    if (before == transformed)
      return;
    if (chunk.set_block_state(local_column_x, world_y, local_column_z, transformed)) {
      original_.try_emplace({world_x, world_y, world_z}, before);
      ++changed_;
    }
  }

  void generate_box(int32_t x0, int32_t y0, int32_t z0, int32_t x1, int32_t y1, int32_t z1,
                    const BlockStateKey& edge, const BlockStateKey& fill, bool skip_air = false) {
    for (int32_t y = y0; y <= y1; ++y)
      for (int32_t x = x0; x <= x1; ++x)
        for (int32_t z = z0; z <= z1; ++z) {
          if (skip_air && is_air_state(get_block(x, y, z)))
            continue;
          const bool is_edge = y == y0 || y == y1 || x == x0 || x == x1 || z == z0 || z == z1;
          place(is_edge ? edge : fill, x, y, z);
        }
  }

  // `StructurePiece.generateBox(..., skipAir, random, SmoothStoneSelector)`:
  // one float draw for every *edge* cell, in Java's y/x/z walk; an interior
  // cell is cave air and draws nothing. The selector's ternary only evaluates
  // `nextFloat` on the edge branch, which is why the interior cells cannot
  // shift the stream.
  void generate_smooth_box(int32_t x0, int32_t y0, int32_t z0, int32_t x1, int32_t y1, int32_t z1,
                           bool skip_air, WgLegacyRandom& random) {
    const BlockStateKey cave_air{"minecraft:cave_air", ""};
    for (int32_t y = y0; y <= y1; ++y)
      for (int32_t x = x0; x <= x1; ++x)
        for (int32_t z = z0; z <= z1; ++z) {
          if (skip_air && is_air_state(get_block(x, y, z)))
            continue;
          const bool is_edge = y == y0 || y == y1 || x == x0 || x == x1 || z == z0 || z == z1;
          if (!is_edge) {
            place(cave_air, x, y, z);
            continue;
          }
          const float selection = random.next_float();
          if (selection < 0.2f)
            place(BlockStateKey{"minecraft:cracked_stone_bricks", ""}, x, y, z);
          else if (selection < 0.5f)
            place(BlockStateKey{"minecraft:mossy_stone_bricks", ""}, x, y, z);
          else if (selection < 0.55f)
            place(BlockStateKey{"minecraft:infested_stone_bricks", ""}, x, y, z);
          else
            place(BlockStateKey{"minecraft:stone_bricks", ""}, x, y, z);
        }
  }

  // `StructurePiece.generateMaybeBox` with `hasToBeInside` false (the only
  // form the stronghold pieces use): the probability draw happens for every
  // cell in the box, before the skip-air check.
  void generate_maybe_box(WgLegacyRandom& random, float probability, int32_t x0, int32_t y0,
                          int32_t z0, int32_t x1, int32_t y1, int32_t z1, const BlockStateKey& edge,
                          const BlockStateKey& fill, bool skip_air) {
    for (int32_t y = y0; y <= y1; ++y)
      for (int32_t x = x0; x <= x1; ++x)
        for (int32_t z = z0; z <= z1; ++z) {
          const bool picked = !(random.next_float() > probability);
          if (!picked || (skip_air && is_air_state(get_block(x, y, z))))
            continue;
          const bool is_edge = y == y0 || y == y1 || x == x0 || x == x1 || z == z0 || z == z1;
          place(is_edge ? edge : fill, x, y, z);
        }
  }

  // `StructurePiece.maybeGenerateBlock`: one draw, then one `placeBlock`.
  void maybe_generate_block(WgLegacyRandom& random, float probability, int32_t x, int32_t y,
                            int32_t z, const BlockStateKey& state) {
    if (random.next_float() < probability)
      place(state, x, y, z);
  }

  BlockStateKey get_block(int32_t local_x, int32_t local_y, int32_t local_z) const {
    int32_t world_x = 0;
    int32_t world_z = 0;
    switch (direction) {
    case 0:
      world_x = min_x + local_x;
      world_z = max_z - local_z;
      break;
    case 1:
      world_x = min_x + local_z;
      world_z = min_z + local_x;
      break;
    case 2:
      world_x = min_x + local_x;
      world_z = min_z + local_z;
      break;
    default:
      world_x = max_x - local_z;
      world_z = min_z + local_x;
      break;
    }
    const int32_t base_x = chunk.x * 16;
    const int32_t base_z = chunk.z * 16;
    const int32_t world_y = base_y + local_y;
    if (world_x < base_x || world_x >= base_x + 16 || world_z < base_z || world_z >= base_z + 16 ||
        world_y < kMinY || world_y >= kMinY + kHeight)
      return BlockStateKey{"minecraft:air", ""};
    return chunk.block_state_at(world_x - base_x, world_y, world_z - base_z);
  }

  BlockStateKey get_world_block(int32_t world_x, int32_t world_y, int32_t world_z) const {
    const int32_t base_x = chunk.x * 16;
    const int32_t base_z = chunk.z * 16;
    if (world_x < base_x || world_x >= base_x + 16 || world_z < base_z || world_z >= base_z + 16 ||
        world_y < kMinY || world_y >= kMinY + kHeight)
      return BlockStateKey{"minecraft:air", ""};
    return chunk.block_state_at(world_x - base_x, world_y, world_z - base_z);
  }

  void fill_column_down(const BlockStateKey& state, int32_t local_x, int32_t start_y,
                        int32_t local_z) {
    int32_t world_x = 0;
    int32_t world_z = 0;
    switch (direction) {
    case 0:
      world_x = min_x + local_x;
      world_z = max_z - local_z;
      break;
    case 1:
      world_x = min_x + local_z;
      world_z = min_z + local_x;
      break;
    case 2:
      world_x = min_x + local_x;
      world_z = min_z + local_z;
      break;
    default:
      world_x = max_x - local_z;
      world_z = min_z + local_x;
      break;
    }
    const int32_t base_x = chunk.x * 16;
    const int32_t base_z = chunk.z * 16;
    if (world_x < base_x || world_x >= base_x + 16 || world_z < base_z || world_z >= base_z + 16)
      return;
    int32_t world_y = base_y + start_y;
    while (world_y > kMinY + 1 && is_replaceable_by_structures(chunk.block_state_at(
                                      world_x - base_x, world_y, world_z - base_z))) {
      BlockStateKey transformed = state;
      if (orientation_mirrors(direction))
        transformed.properties = mirror_left_right(transformed.properties);
      transformed.properties =
          wg_piece_rotate_properties(transformed.properties, orientation_rotation(direction));
      const BlockStateKey before =
          chunk.block_state_at(world_x - base_x, world_y, world_z - base_z);
      if (before == transformed) {
        --world_y;
        continue;
      }
      if (chunk.set_block_state(world_x - base_x, world_y, world_z - base_z, transformed)) {
        original_.try_emplace({world_x, world_y, world_z}, before);
        ++changed_;
      }
      --world_y;
    }
  }

  Chunk& chunk;
  uint8_t direction;
  int32_t min_x;
  int32_t max_x;
  int32_t min_z;
  int32_t max_z;
  int32_t base_y;
  uint32_t changed_ = 0;
  std::map<std::tuple<int32_t, int32_t, int32_t>, BlockStateKey> original_;
};

} // namespace

struct WgGeneratedHeights::Impl {
  explicit Impl(const WgOverworldGenerator& generator_in) : generator(generator_in) {}
  const WgOverworldGenerator& generator;
  std::unordered_map<uint64_t, Chunk> chunks;
};

WgGeneratedHeights::WgGeneratedHeights(const WgOverworldGenerator& generator)
    : impl_(std::make_unique<Impl>(generator)) {}

WgGeneratedHeights::~WgGeneratedHeights() = default;

int32_t WgGeneratedHeights::height(int32_t block_x, int32_t block_z, WgStructureHeightmap type) {
  const int32_t chunk_x = floor_div16(block_x);
  const int32_t chunk_z = floor_div16(block_z);
  const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(chunk_x)) << 32) |
                       static_cast<uint32_t>(chunk_z);
  auto it = impl_->chunks.find(key);
  if (it == impl_->chunks.end()) {
    it = impl_->chunks.emplace(key, Chunk{}).first;
    // `ChunkGenerator.getBaseHeight` walks `iterateNoiseColumn`: the height is
    // the top cell of the *noise* column, before `buildSurface`. Surface rules
    // can raise a column above the noise surface (the eroded-badlands pillars
    // write stone into air up to y 64 + the pillar height), and that raised
    // cell is not part of the answer vanilla gives structure placement.
    if (impl_->generator.table().dimension == WgDimensionKind::kOverworld)
      fill_generated_chunk(impl_->generator, chunk_x, chunk_z, it->second);
    else
      fill_generated_dimension_chunk(impl_->generator, chunk_x, chunk_z, it->second);
  }
  const int32_t local_x = block_x - chunk_x * 16;
  const int32_t local_z = block_z - chunk_z * 16;
  if (type == WgStructureHeightmap::kWorldSurface)
    return it->second.height_at(HeightmapType::WorldSurface, local_x, local_z);
  if (type == WgStructureHeightmap::kMotionBlockingNoLeaves)
    return it->second.height_at(HeightmapType::MotionBlockingNoLeaves, local_x, local_z);
  // `OCEAN_FLOOR_WG` is not one of the heightmaps the bridge materialises (the
  // client never receives it), so its `MATERIAL_MOTION_BLOCKING` answer is
  // walked here: `block_blocks_motion` alone, one past the highest cell it
  // accepts. This is the same predicate `HeightmapType::OceanFloor` carries.
  const int32_t chunk_min_y = it->second.min_section_y * 16;
  const int32_t chunk_height = static_cast<int32_t>(it->second.sections.size()) * 16;
  for (int32_t y = chunk_min_y + chunk_height - 1; y >= chunk_min_y; --y) {
    if (block_blocks_motion(it->second.block_state_at(local_x, y, local_z).name))
      return y + 1;
  }
  return chunk_min_y;
}

WgPieceBox wg_piece_box(const WgPieceTemplate& templ, uint32_t rotation, int32_t x, int32_t y,
                        int32_t z, uint8_t mirror) {
  if (templ.size_x <= 0 || templ.size_z <= 0 || templ.size_y <= 0) {
    // Vanilla's `StructureTemplate` for a missing resource is empty, and its
    // box is `position - 1 .. position`. 26.2's ancient-city walls pool names
    // one template the jar does not ship, so this is a reachable shape rather
    // than a defensive branch; the weighted pool draw must still see it.
    return WgPieceBox{x - 1, y - 1, z - 1, x, y, z};
  }
  const XZ a = rotate_pivot(0, 0, rotation, templ.pivot_x, templ.pivot_z, mirror);
  const XZ b = rotate_pivot(templ.size_x - 1, templ.size_z - 1, rotation, templ.pivot_x,
                            templ.pivot_z, mirror);
  WgPieceBox box;
  box.min_x = x + std::min(a.x, b.x);
  box.max_x = x + std::max(a.x, b.x);
  box.min_z = z + std::min(a.z, b.z);
  box.max_z = z + std::max(a.z, b.z);
  box.min_y = y;
  box.max_y = y + templ.size_y - 1;
  return box;
}

std::string wg_piece_rotate_properties(std::string_view properties, uint32_t rotation) {
  if (properties.empty() || (rotation & 3u) == 0)
    return std::string(properties);
  const std::map<std::string, std::string> parsed = parse_properties(properties);
  std::map<std::string, std::string> out;
  for (const auto& [key, value] : parsed) {
    if (key == "facing") {
      out[key] = rotate_direction(value.c_str(), rotation);
    } else if (key == "axis" && (rotation & 1u) != 0) {
      out[key] = value == "x" ? "z" : (value == "z" ? "x" : value);
    } else if (is_horizontal_connection(key)) {
      out[rotate_direction(key.c_str(), rotation)] = value;
    } else if (key == "shape" &&
               (value == "north_south" || value == "east_west" || value == "ascending_north" ||
                value == "ascending_south" || value == "ascending_east" ||
                value == "ascending_west" || value == "north_east" || value == "north_west" ||
                value == "south_east" || value == "south_west")) {
      out[key] = rotate_rail_shape(value, rotation);
    } else {
      out[key] = value;
    }
  }
  return join_properties(out);
}

const WgPieceTemplate* wg_piece_template_by_name(const char* name) {
  constexpr size_t kCount = sizeof(kWgPieceTemplates) / sizeof(kWgPieceTemplates[0]);
  for (size_t i = 0; i < kCount; ++i) {
    if (std::strcmp(kWgPieceTemplates[i].name, name) == 0)
      return &kWgPieceTemplates[i];
  }
  return nullptr;
}

namespace {

BlockStateKey sand_state() {
  return BlockStateKey{"minecraft:sand", ""};
}
BlockStateKey sandstone_state() {
  return BlockStateKey{"minecraft:sandstone", ""};
}
BlockStateKey cut_sandstone_state() {
  return BlockStateKey{"minecraft:cut_sandstone", ""};
}
BlockStateKey chiseled_sandstone_state() {
  return BlockStateKey{"minecraft:chiseled_sandstone", ""};
}
BlockStateKey orange_terracotta_state() {
  return BlockStateKey{"minecraft:orange_terracotta", ""};
}
BlockStateKey blue_terracotta_state() {
  return BlockStateKey{"minecraft:blue_terracotta", ""};
}
BlockStateKey sandstone_stairs_state(const char* facing) {
  return BlockStateKey{"minecraft:sandstone_stairs",
                       std::string("facing=") + facing +
                           ",half=bottom,shape=straight,waterlogged=false"};
}
BlockStateKey sandstone_slab_state() {
  return BlockStateKey{"minecraft:sandstone_slab", "type=bottom,waterlogged=false"};
}
BlockStateKey tnt_state() {
  return BlockStateKey{"minecraft:tnt", "unstable=false"};
}
BlockStateKey pressure_plate_state() {
  return BlockStateKey{"minecraft:stone_pressure_plate", "powered=false"};
}

int64_t packed_block_pos(int32_t x, int32_t y, int32_t z) {
  // `BlockPos.asLong`: x and z get 26 bits, y gets 12.
  return (static_cast<int64_t>(x) & 0x3FFFFFFLL) << 38 |
         (static_cast<int64_t>(z) & 0x3FFFFFFLL) << 12 | (static_cast<int64_t>(y) & 0xFFFLL);
}

void add_brushable_loot(Chunk& chunk, int32_t world_x, int32_t world_y, int32_t world_z,
                        const char* table, int64_t seed) {
  for (const nbt::Value& be : chunk.block_entities)
    if (block_entity_matches(be, world_x, world_y, world_z))
      return;
  std::vector<std::pair<std::string, nbt::Value>> fields;
  fields.emplace_back("id", nbt::make_string("minecraft:brushable_block"));
  fields.emplace_back("x", nbt::make_int(world_x));
  fields.emplace_back("y", nbt::make_int(world_y));
  fields.emplace_back("z", nbt::make_int(world_z));
  fields.emplace_back("LootTable", nbt::make_string(table));
  fields.emplace_back("LootTableSeed", nbt::make_long(seed));
  chunk.block_entities.push_back(nbt::make_compound(std::move(fields)));
}

} // namespace

namespace {

// `JungleTemplePiece.MossStoneSelector`: one draw per generated cell, in the
// same y/x/z order `StructurePiece.generateBox(..., selector)` walks.
BlockStateKey jungle_stone(WgLegacyRandom& random) {
  return random.next_float() < 0.4f ? BlockStateKey{"minecraft:cobblestone", ""}
                                    : BlockStateKey{"minecraft:mossy_cobblestone", ""};
}

BlockStateKey jungle_stairs(const char* facing) {
  return BlockStateKey{"minecraft:cobblestone_stairs",
                       std::string("facing=") + facing +
                           ",half=bottom,shape=straight,waterlogged=false"};
}

BlockStateKey jungle_tripwire(bool north, bool south, bool east, bool west) {
  const auto b = [](bool value) { return value ? "true" : "false"; };
  return BlockStateKey{"minecraft:tripwire",
                       std::string("attached=true,disarmed=false,east=") + b(east) + ",north=" +
                           b(north) + ",powered=false,south=" + b(south) + ",west=" + b(west)};
}

BlockStateKey jungle_redstone(const char* north, const char* south, const char* east,
                              const char* west) {
  return BlockStateKey{"minecraft:redstone_wire", std::string("east=") + east + ",north=" + north +
                                                      ",power=0,south=" + south + ",west=" + west};
}

BlockStateKey jungle_vine(bool east, bool north, bool south, bool west) {
  const auto b = [](bool value) { return value ? "true" : "false"; };
  return BlockStateKey{"minecraft:vine", std::string("east=") + b(east) + ",north=" + b(north) +
                                             ",south=" + b(south) + ",up=false,west=" + b(west)};
}

const char* opposite_direction(const char* direction) {
  if (std::strcmp(direction, "north") == 0)
    return "south";
  if (std::strcmp(direction, "south") == 0)
    return "north";
  if (std::strcmp(direction, "east") == 0)
    return "west";
  return "east";
}

// `StructurePiece.reorient`'s fallback walk: north, south, west, east. The
// piece-local coordinates are not compass coordinates for the north and east/
// west orientations, so the four neighbours are read through the writer's own
// `world_xz` mapping rather than by adding to the local cell.
const char* jungle_chest_facing(ScatteredWriter& writer, int32_t local_x, int32_t local_y,
                                int32_t local_z, int32_t ground_y) {
  const auto world = writer.world_xz(local_x, local_z);
  const int32_t world_x = world.first;
  const int32_t world_z = world.second;
  const int32_t world_y = ground_y + local_y;
  struct Step {
    const char* name;
    int32_t dx;
    int32_t dz;
  };
  static const Step kSteps[4] = {
      {"north", 0, -1}, {"east", 1, 0}, {"south", 0, 1}, {"west", -1, 0}};
  const auto block_at = [&](const char* direction) {
    int32_t dx = 0;
    int32_t dz = 0;
    if (std::strcmp(direction, "north") == 0)
      dz = -1;
    else if (std::strcmp(direction, "south") == 0)
      dz = 1;
    else if (std::strcmp(direction, "east") == 0)
      dx = 1;
    else
      dx = -1;
    return writer.get_world_block(world_x + dx, world_y, world_z + dz);
  };
  const char* solid = nullptr;
  for (const Step& step : kSteps) {
    const BlockStateKey state = block_at(step.name);
    if (state.name == "minecraft:chest")
      return "north";
    if (!block_blocks_motion(state.name))
      continue;
    if (solid == nullptr) {
      solid = step.name;
      continue;
    }
    solid = nullptr;
    break;
  }
  if (solid != nullptr)
    return opposite_direction(solid);
  const char* lock = "north";
  if (block_blocks_motion(block_at(lock).name))
    lock = "south";
  if (block_blocks_motion(block_at(lock).name))
    lock = "west";
  if (block_blocks_motion(block_at(lock).name))
    lock = "east";
  return lock;
}

class JungleTempleWriter {
public:
  JungleTempleWriter(Chunk& chunk_in, const WgJungleTempleStart& start_in)
      : chunk(chunk_in), start(start_in),
        writer(chunk_in, start_in.direction, start_in.min_x, start_in.max_x, start_in.min_z,
               start_in.max_z, start_in.ground_y),
        random(wg_mth_get_seed(start_in.x, 64, start_in.z)) {}

  uint32_t run() {
    // `JungleTemplePiece.postProcess`'s first act is its height update, which
    // the start already performed over the same footprint (the 12 x 15 box is
    // inside the source chunk, so there is no seam-dependent answer).
    selector_box(0, -4, 0, 11, 0, 14);
    selector_box(2, 1, 2, 9, 2, 2);
    selector_box(2, 1, 12, 9, 2, 12);
    selector_box(2, 1, 3, 2, 2, 11);
    selector_box(9, 1, 3, 9, 2, 11);
    selector_box(1, 3, 1, 10, 6, 1);
    selector_box(1, 3, 13, 10, 6, 13);
    selector_box(1, 3, 2, 1, 6, 12);
    selector_box(10, 3, 2, 10, 6, 12);
    selector_box(2, 3, 2, 9, 3, 12);
    selector_box(2, 6, 2, 9, 6, 12);
    selector_box(3, 7, 3, 8, 7, 11);
    selector_box(4, 8, 4, 7, 8, 10);

    air_box(3, 1, 3, 8, 2, 11);
    air_box(4, 3, 6, 7, 3, 9);
    air_box(2, 4, 2, 9, 5, 12);
    air_box(4, 6, 5, 7, 6, 9);
    air_box(5, 7, 6, 6, 7, 8);
    air_box(5, 1, 2, 6, 2, 2);
    air_box(5, 2, 12, 6, 2, 12);
    air_box(5, 5, 1, 6, 5, 1);
    air_box(5, 5, 13, 6, 5, 13);
    place(air, 1, 5, 5);
    place(air, 10, 5, 5);
    place(air, 1, 5, 9);
    place(air, 10, 5, 9);

    for (int32_t z = 0; z <= 14; z += 14) {
      selector_box(2, 4, z, 2, 5, z);
      selector_box(4, 4, z, 4, 5, z);
      selector_box(7, 4, z, 7, 5, z);
      selector_box(9, 4, z, 9, 5, z);
    }
    selector_box(5, 6, 0, 6, 6, 0);
    for (int32_t x = 0; x <= 11; x += 11) {
      for (int32_t z = 2; z <= 12; z += 2)
        selector_box(x, 4, z, x, 5, z);
      selector_box(x, 6, 5, x, 6, 5);
      selector_box(x, 6, 9, x, 6, 9);
    }

    selector_box(2, 7, 2, 2, 9, 2);
    selector_box(9, 7, 2, 9, 9, 2);
    selector_box(2, 7, 12, 2, 9, 12);
    selector_box(9, 7, 12, 9, 9, 12);
    selector_box(4, 9, 4, 4, 9, 4);
    selector_box(7, 9, 4, 7, 9, 4);
    selector_box(4, 9, 10, 4, 9, 10);
    selector_box(7, 9, 10, 7, 9, 10);
    selector_box(5, 9, 7, 6, 9, 7);

    const BlockStateKey north_stairs = jungle_stairs("north");
    const BlockStateKey south_stairs = jungle_stairs("south");
    const BlockStateKey east_stairs = jungle_stairs("east");
    const BlockStateKey west_stairs = jungle_stairs("west");
    place(north_stairs, 5, 9, 6);
    place(north_stairs, 6, 9, 6);
    place(south_stairs, 5, 9, 8);
    place(south_stairs, 6, 9, 8);
    for (int32_t x = 4; x <= 7; ++x)
      place(north_stairs, x, 0, 0);
    place(north_stairs, 4, 1, 8);
    place(north_stairs, 4, 2, 9);
    place(north_stairs, 4, 3, 10);
    place(north_stairs, 7, 1, 8);
    place(north_stairs, 7, 2, 9);
    place(north_stairs, 7, 3, 10);
    selector_box(4, 1, 9, 4, 1, 9);
    selector_box(7, 1, 9, 7, 1, 9);
    selector_box(4, 1, 10, 7, 2, 10);
    selector_box(5, 4, 5, 6, 4, 5);
    place(east_stairs, 4, 4, 5);
    place(west_stairs, 7, 4, 5);
    for (int32_t i = 0; i < 4; ++i) {
      place(south_stairs, 5, -i, 6 + i);
      place(south_stairs, 6, -i, 6 + i);
      air_box(5, -i, 7 + i, 6, -i, 9 + i);
    }

    air_box(1, -3, 12, 10, -1, 13);
    air_box(1, -3, 1, 3, -1, 13);
    air_box(1, -3, 1, 9, -1, 5);
    for (int32_t z = 1; z <= 13; z += 2)
      selector_box(1, -3, z, 1, -2, z);
    for (int32_t z = 2; z <= 12; z += 2)
      selector_box(1, -1, z, 3, -1, z);
    selector_box(2, -2, 1, 5, -2, 1);
    selector_box(7, -2, 1, 9, -2, 1);
    selector_box(6, -3, 1, 6, -3, 1);
    selector_box(6, -1, 1, 6, -1, 1);

    place(BlockStateKey{"minecraft:tripwire_hook", "attached=true,facing=east,powered=false"}, 1,
          -3, 8);
    place(BlockStateKey{"minecraft:tripwire_hook", "attached=true,facing=west,powered=false"}, 4,
          -3, 8);
    const BlockStateKey tripwire_east_west = jungle_tripwire(false, false, true, true);
    place(tripwire_east_west, 2, -3, 8);
    place(tripwire_east_west, 3, -3, 8);
    const BlockStateKey redstone_ns = jungle_redstone("side", "side", "none", "none");
    for (int32_t z = 7; z >= 2; --z)
      place(redstone_ns, 5, -3, z);
    place(jungle_redstone("side", "none", "none", "side"), 5, -3, 1);
    place(jungle_redstone("none", "none", "side", "side"), 4, -3, 1);
    place(mossy, 3, -3, 1);

    // `placedTrap1`; a start is written once, so the flag is always false here.
    create_dispenser(3, -2, 1, "north");
    place(jungle_vine(false, false, true, false), 3, -2, 2);
    place(BlockStateKey{"minecraft:tripwire_hook", "attached=true,facing=north,powered=false"}, 7,
          -3, 1);
    place(BlockStateKey{"minecraft:tripwire_hook", "attached=true,facing=south,powered=false"}, 7,
          -3, 5);
    const BlockStateKey tripwire_north_south = jungle_tripwire(true, true, false, false);
    place(tripwire_north_south, 7, -3, 2);
    place(tripwire_north_south, 7, -3, 3);
    place(tripwire_north_south, 7, -3, 4);
    place(jungle_redstone("none", "none", "side", "side"), 8, -3, 6);
    place(jungle_redstone("none", "side", "none", "side"), 9, -3, 6);
    place(jungle_redstone("side", "up", "none", "none"), 9, -3, 5);
    place(mossy, 9, -3, 4);
    place(redstone_ns, 9, -2, 4);

    // `placedTrap2`.
    create_dispenser(9, -2, 3, "west");
    place(jungle_vine(true, false, false, false), 8, -1, 3);
    place(jungle_vine(true, false, false, false), 8, -2, 3);

    // `placedMainChest`.
    create_chest(8, -3, 3, "minecraft:chests/jungle_temple");
    place(mossy, 9, -3, 2);
    place(mossy, 8, -3, 1);
    place(mossy, 4, -3, 5);
    place(mossy, 5, -2, 5);
    place(mossy, 5, -1, 5);
    place(mossy, 6, -3, 5);
    place(mossy, 7, -2, 5);
    place(mossy, 7, -1, 5);
    place(mossy, 8, -3, 5);
    selector_box(9, -1, 1, 9, -1, 5);
    air_box(8, -3, 8, 10, -1, 10);
    place(chiseled, 8, -2, 11);
    place(chiseled, 9, -2, 11);
    place(chiseled, 10, -2, 11);
    const BlockStateKey lever{"minecraft:lever", "face=wall,facing=north,powered=false"};
    place(lever, 8, -2, 12);
    place(lever, 9, -2, 12);
    place(lever, 10, -2, 12);
    selector_box(8, -3, 8, 8, -3, 10);
    selector_box(10, -3, 8, 10, -3, 10);
    place(mossy, 10, -2, 9);
    place(redstone_ns, 8, -2, 9);
    place(redstone_ns, 8, -2, 10);
    place(jungle_redstone("side", "side", "side", "side"), 10, -1, 9);
    place(BlockStateKey{"minecraft:sticky_piston", "extended=false,facing=up"}, 9, -2, 8);
    place(BlockStateKey{"minecraft:sticky_piston", "extended=false,facing=west"}, 10, -2, 8);
    place(BlockStateKey{"minecraft:sticky_piston", "extended=false,facing=west"}, 10, -1, 8);
    place(BlockStateKey{"minecraft:repeater", "delay=1,facing=north,locked=false,powered=false"},
          10, -2, 10);

    // `placedHiddenChest`.
    create_chest(9, -3, 10, "minecraft:chests/jungle_temple");
    return writer.changed();
  }

private:
  inline static const BlockStateKey air{"minecraft:air", ""};
  inline static const BlockStateKey mossy{"minecraft:mossy_cobblestone", ""};
  inline static const BlockStateKey chiseled{"minecraft:chiseled_stone_bricks", ""};

  void place(const BlockStateKey& state, int32_t local_x, int32_t local_y, int32_t local_z) {
    writer.place_public(state, local_x, local_y, local_z);
  }

  void selector_box(int32_t x0, int32_t y0, int32_t z0, int32_t x1, int32_t y1, int32_t z1) {
    for (int32_t y = y0; y <= y1; ++y)
      for (int32_t x = x0; x <= x1; ++x)
        for (int32_t z = z0; z <= z1; ++z)
          place(jungle_stone(random), x, y, z);
  }

  void air_box(int32_t x0, int32_t y0, int32_t z0, int32_t x1, int32_t y1, int32_t z1) {
    writer.generate_box(x0, y0, z0, x1, y1, z1, air, air);
  }

  bool inside_world(int32_t world_x, int32_t world_z) const {
    const int32_t base_x = chunk.x * 16;
    const int32_t base_z = chunk.z * 16;
    return world_x >= base_x && world_x < base_x + 16 && world_z >= base_z && world_z < base_z + 16;
  }

  bool create_dispenser(int32_t local_x, int32_t local_y, int32_t local_z, const char* facing) {
    const auto [world_x, world_z] = writer.world_xz(local_x, local_z);
    if (!inside_world(world_x, world_z))
      return false;
    // `createDispenser` sets the block entity's loot-table seed from this
    // draw; the port's loot seed is positional, but the draw stays in the
    // stream so the selector's cells do not move. It is consumed even when a
    // previous application already left a dispenser here, which keeps a
    // repeat of the writer idempotent (vanilla's own `placedTrap` flags do
    // the same job for it).
    (void)random.next_long();
    if (writer.get_block(local_x, local_y, local_z).name == "minecraft:dispenser")
      return false;
    place(
        BlockStateKey{"minecraft:dispenser", std::string("facing=") + facing + ",triggered=false"},
        local_x, local_y, local_z);
    add_dispenser_loot(chunk, world_x, start.ground_y + local_y, world_z,
                       "minecraft:chests/jungle_temple_dispenser", start.world_seed);
    return true;
  }

  bool create_chest(int32_t local_x, int32_t local_y, int32_t local_z, const char* table) {
    const auto [world_x, world_z] = writer.world_xz(local_x, local_z);
    if (!inside_world(world_x, world_z))
      return false;
    // The chest's loot seed draw stays in the stream even when the chest is
    // already there, so a repeat writes the same selector cells.
    (void)random.next_long();
    if (writer.get_block(local_x, local_y, local_z).name == "minecraft:chest")
      return false;
    const char* facing = jungle_chest_facing(writer, local_x, local_y, local_z, start.ground_y);
    writer.place_raw(BlockStateKey{"minecraft:chest", std::string("facing=") + facing +
                                                          ",type=single,waterlogged=false"},
                     local_x, local_y, local_z);
    add_chest_loot(chunk, world_x, start.ground_y + local_y, world_z, table, start.world_seed);
    return true;
  }

  Chunk& chunk;
  const WgJungleTempleStart& start;
  ScatteredWriter writer;
  WgLegacyRandom random;
};

} // namespace

WgJungleTempleStart wg_jungle_temple_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                           WgClimateSampler& sampler, WgStructureHeights& heights) {
  WgJungleTempleStart start;
  const WgStructureSet* set = wg_structure_set_by_name("minecraft:jungle_temples");
  if (set == nullptr || !wg_structure_is_chunk(seed, *set, chunk_x, chunk_z))
    return start;
  const WgStructure* structure = wg_structure_by_name("minecraft:jungle_pyramid");
  if (structure == nullptr)
    return start;

  const int32_t min_x = chunk_x * 16;
  const int32_t min_z = chunk_z * 16;
  const int32_t center_x = min_x + 8;
  const int32_t center_z = min_z + 8;

  // `SinglePieceStructure.findGenerationPoint`: the four corners one block
  // past the 12 x 15 footprint, `WORLD_SURFACE_WG` first-occupied, minimum;
  // below sea level there is no start.
  int32_t lowest = heights.height(min_x, min_z, WgStructureHeightmap::kWorldSurface);
  lowest = std::min(lowest, heights.height(min_x + 12, min_z, WgStructureHeightmap::kWorldSurface));
  lowest = std::min(lowest, heights.height(min_x, min_z + 15, WgStructureHeightmap::kWorldSurface));
  lowest =
      std::min(lowest, heights.height(min_x + 12, min_z + 15, WgStructureHeightmap::kWorldSurface));
  if (lowest < 63) // `chunkGenerator.getSeaLevel()`
    return start;

  const int32_t center_y =
      heights.height(center_x, center_z, WgStructureHeightmap::kWorldSurface) - 1;
  if (!wg_structure_start_biome_is_valid(sampler, *structure, center_x, center_y, center_z))
    return start;

  // `JungleTemplePiece`'s constructor draws `getRandomHorizontalDirection` from
  // the context random; `StructurePiece.makeBoundingBox` swaps the width and
  // depth for the east/west orientations.
  WgLegacyRandom context(0);
  seed_large_feature(context, seed, chunk_x, chunk_z);
  const uint8_t direction = static_cast<uint8_t>(context.next_int(4));
  start.structure = structure;
  start.world_seed = seed;
  start.chunk_x = chunk_x;
  start.chunk_z = chunk_z;
  start.direction = direction;
  start.x = min_x;
  start.y = 64;
  start.z = min_z;
  if (direction == 0 || direction == 2) {
    start.min_x = min_x;
    start.max_x = min_x + 11;
    start.min_z = min_z;
    start.max_z = min_z + 14;
  } else {
    start.min_x = min_x;
    start.max_x = min_x + 14;
    start.min_z = min_z;
    start.max_z = min_z + 11;
  }
  start.min_y = 64;
  start.max_y = 64 + 9;

  // `ScatteredFeaturePiece.updateAverageGroundHeight(level, chunkBB, 0)`: the
  // mean of the footprint columns' `MOTION_BLOCKING_NO_LEAVES` heights. The
  // whole footprint is in the source chunk, so every cell is counted.
  int64_t total = 0;
  int32_t count = 0;
  for (int32_t z = start.min_z; z <= start.max_z; ++z) {
    for (int32_t x = start.min_x; x <= start.max_x; ++x) {
      if (x < min_x || x >= min_x + 16 || z < min_z || z >= min_z + 16)
        continue;
      total += heights.height(x, z, WgStructureHeightmap::kMotionBlockingNoLeaves);
      ++count;
    }
  }
  if (count == 0)
    return start;
  start.ground_y = static_cast<int32_t>(total / count);
  start.valid = true;
  return start;
}

uint32_t wg_place_jungle_temple(Chunk& chunk, const WgJungleTempleStart& start) {
  if (!start.valid)
    return 0;
  JungleTempleWriter writer(chunk, start);
  return writer.run();
}

WgDesertPyramidStart wg_desert_pyramid_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                             WgClimateSampler& sampler,
                                             WgStructureHeights& heights) {
  WgDesertPyramidStart start;
  const WgStructureSet* set = wg_structure_set_by_name("minecraft:desert_pyramids");
  if (set == nullptr || !wg_structure_is_chunk(seed, *set, chunk_x, chunk_z))
    return start;
  const WgStructure* structure = wg_structure_by_name("minecraft:desert_pyramid");
  if (structure == nullptr)
    return start;

  const int32_t min_x = chunk_x * 16;
  const int32_t min_z = chunk_z * 16;
  const int32_t center_x = min_x + 8;
  const int32_t center_z = min_z + 8;
  const int32_t center_y =
      heights.height(center_x, center_z, WgStructureHeightmap::kWorldSurface) - 1;
  if (!wg_structure_start_biome_is_valid(sampler, *structure, center_x, center_y, center_z))
    return start;

  // `SinglePieceStructure.getLowestY`: the four corners one block past the
  // 21 x 21 footprint, `WORLD_SURFACE_WG` first-occupied. The sea-level gate is
  // before any piece is constructed.
  int32_t lowest = heights.height(min_x, min_z, WgStructureHeightmap::kWorldSurface) - 1;
  lowest =
      std::min(lowest, heights.height(min_x, min_z + 21, WgStructureHeightmap::kWorldSurface) - 1);
  lowest =
      std::min(lowest, heights.height(min_x + 21, min_z, WgStructureHeightmap::kWorldSurface) - 1);
  lowest = std::min(
      lowest, heights.height(min_x + 21, min_z + 21, WgStructureHeightmap::kWorldSurface) - 1);
  if (lowest < 63) // `chunkGenerator.getSeaLevel()`
    return start;

  // `DesertPyramidPiece`'s constructor draws the horizontal direction from the
  // structure's context random; the piece box is 21 x 15 x 21.
  WgLegacyRandom context(0);
  seed_large_feature(context, seed, chunk_x, chunk_z);
  const uint8_t direction = static_cast<uint8_t>(context.next_int(4));
  start.structure = structure;
  start.world_seed = seed;
  start.chunk_x = chunk_x;
  start.chunk_z = chunk_z;
  start.direction = direction;
  start.x = min_x;
  start.y = 64;
  start.z = min_z;
  start.min_x = min_x;
  start.max_x = min_x + 20;
  start.min_z = min_z;
  start.max_z = min_z + 20;

  // `postProcess` starts with `updateHeightPositionToLowestGroundHeight(level,
  // -random.nextInt(3))`: the minimum `MOTION_BLOCKING_NO_LEAVES` over the
  // whole 21 x 21 box, lowered by a 0..2 draw. The draw's stream is the piece's
  // positional source; the writer consumes the same first draw to keep its
  // selector/chest/cellar stream aligned.
  int32_t lowest_motion =
      heights.height(min_x, min_z, WgStructureHeightmap::kMotionBlockingNoLeaves) - 1;
  for (int32_t z = min_z; z <= min_z + 20; ++z)
    for (int32_t x = min_x; x <= min_x + 20; ++x)
      lowest_motion = std::min(
          lowest_motion, heights.height(x, z, WgStructureHeightmap::kMotionBlockingNoLeaves) - 1);
  WgLegacyRandom placement_random(wg_mth_get_seed(min_x, 64, min_z));
  const int32_t offset = placement_random.next_int(3);
  start.ground_y = lowest_motion - offset;
  start.min_y = start.ground_y;
  start.max_y = start.ground_y + 14;
  start.valid = true;
  return start;
}

namespace {

const char* const kUnderwaterWarm[] = {
    "minecraft:underwater_ruin/warm_1", "minecraft:underwater_ruin/warm_2",
    "minecraft:underwater_ruin/warm_3", "minecraft:underwater_ruin/warm_4",
    "minecraft:underwater_ruin/warm_5", "minecraft:underwater_ruin/warm_6",
    "minecraft:underwater_ruin/warm_7", "minecraft:underwater_ruin/warm_8"};
const char* const kUnderwaterBigWarm[] = {
    "minecraft:underwater_ruin/big_warm_4", "minecraft:underwater_ruin/big_warm_5",
    "minecraft:underwater_ruin/big_warm_6", "minecraft:underwater_ruin/big_warm_7"};
const char* const kUnderwaterBrick[] = {
    "minecraft:underwater_ruin/brick_1", "minecraft:underwater_ruin/brick_2",
    "minecraft:underwater_ruin/brick_3", "minecraft:underwater_ruin/brick_4",
    "minecraft:underwater_ruin/brick_5", "minecraft:underwater_ruin/brick_6",
    "minecraft:underwater_ruin/brick_7", "minecraft:underwater_ruin/brick_8"};
const char* const kUnderwaterBigBrick[] = {
    "minecraft:underwater_ruin/big_brick_1", "minecraft:underwater_ruin/big_brick_2",
    "minecraft:underwater_ruin/big_brick_3", "minecraft:underwater_ruin/big_brick_8"};
const char* const kUnderwaterCracked[] = {
    "minecraft:underwater_ruin/cracked_1", "minecraft:underwater_ruin/cracked_2",
    "minecraft:underwater_ruin/cracked_3", "minecraft:underwater_ruin/cracked_4",
    "minecraft:underwater_ruin/cracked_5", "minecraft:underwater_ruin/cracked_6",
    "minecraft:underwater_ruin/cracked_7", "minecraft:underwater_ruin/cracked_8"};
const char* const kUnderwaterBigCracked[] = {
    "minecraft:underwater_ruin/big_cracked_1", "minecraft:underwater_ruin/big_cracked_2",
    "minecraft:underwater_ruin/big_cracked_3", "minecraft:underwater_ruin/big_cracked_8"};
const char* const kUnderwaterMossy[] = {
    "minecraft:underwater_ruin/mossy_1", "minecraft:underwater_ruin/mossy_2",
    "minecraft:underwater_ruin/mossy_3", "minecraft:underwater_ruin/mossy_4",
    "minecraft:underwater_ruin/mossy_5", "minecraft:underwater_ruin/mossy_6",
    "minecraft:underwater_ruin/mossy_7", "minecraft:underwater_ruin/mossy_8"};
const char* const kUnderwaterBigMossy[] = {
    "minecraft:underwater_ruin/big_mossy_1", "minecraft:underwater_ruin/big_mossy_2",
    "minecraft:underwater_ruin/big_mossy_3", "minecraft:underwater_ruin/big_mossy_8"};

template <size_t N> const char* pick_name(const char* const (&names)[N], WgLegacyRandom& random) {
  return names[random.next_int(static_cast<int32_t>(N))];
}

void push_ocean_piece(WgOceanRuinStart& start, const char* name, uint32_t rotation, int32_t x,
                      int32_t y, int32_t z, float integrity, uint8_t cold, uint8_t large) {
  if (start.piece_count >= WgOceanRuinStart::kMaxPieces)
    return;
  const WgPieceTemplate* templ = wg_piece_template_by_name(name);
  if (templ == nullptr)
    return;
  WgOceanRuinPiece& piece = start.pieces[start.piece_count++];
  piece.templ = templ;
  piece.rotation = rotation;
  piece.x = x;
  piece.y = y;
  piece.z = z;
  piece.integrity = integrity;
  piece.cold = cold;
  piece.large = large;
}

void add_ocean_piece(WgOceanRuinStart& start, WgLegacyRandom& random, const WgOceanRuinConfig& cfg,
                     int32_t x, int32_t y, int32_t z, uint32_t rotation, bool large,
                     float integrity) {
  if (cfg.cold == 0) {
    const char* name =
        large ? pick_name(kUnderwaterBigWarm, random) : pick_name(kUnderwaterWarm, random);
    push_ocean_piece(start, name, rotation, x, y, z, integrity, cfg.cold, large ? 1 : 0);
    return;
  }
  const char* const* brick = large ? kUnderwaterBigBrick : kUnderwaterBrick;
  const char* const* cracked = large ? kUnderwaterBigCracked : kUnderwaterCracked;
  const char* const* mossy = large ? kUnderwaterBigMossy : kUnderwaterMossy;
  const int32_t count = static_cast<int32_t>(large ? 4 : 8);
  const int32_t index = random.next_int(count);
  push_ocean_piece(start, brick[index], rotation, x, y, z, integrity, cfg.cold, large ? 1 : 0);
  push_ocean_piece(start, cracked[index], rotation, x, y, z, 0.7f, cfg.cold, large ? 1 : 0);
  push_ocean_piece(start, mossy[index], rotation, x, y, z, 0.5f, cfg.cold, large ? 1 : 0);
}

int32_t ocean_next_int(WgLegacyRandom& random, int32_t min, int32_t max) {
  return min + random.next_int(max - min + 1);
}

struct OceanXZ {
  int32_t x;
  int32_t z;
};

OceanXZ ocean_transform(int32_t x, int32_t z, uint32_t rotation) {
  switch (rotation & 3u) {
  case 1:
    return OceanXZ{-z, x};
  case 2:
    return OceanXZ{-x, -z};
  case 3:
    return OceanXZ{z, -x};
  default:
    return OceanXZ{x, z};
  }
}

} // namespace

namespace {

const char* const kPortalTemplates[] = {
    "minecraft:ruined_portal/portal_1", "minecraft:ruined_portal/portal_2",
    "minecraft:ruined_portal/portal_3", "minecraft:ruined_portal/portal_4",
    "minecraft:ruined_portal/portal_5", "minecraft:ruined_portal/portal_6",
    "minecraft:ruined_portal/portal_7", "minecraft:ruined_portal/portal_8",
    "minecraft:ruined_portal/portal_9", "minecraft:ruined_portal/portal_10"};
const char* const kGiantPortalTemplates[] = {"minecraft:ruined_portal/giant_portal_1",
                                             "minecraft:ruined_portal/giant_portal_2",
                                             "minecraft:ruined_portal/giant_portal_3"};

int32_t portal_random_between(WgLegacyRandom& random, int32_t min, int32_t max) {
  return min + random.next_int(max - min + 1);
}

struct PortalXZ {
  int32_t x;
  int32_t z;
};

PortalXZ portal_transform(int32_t x, int32_t z, uint32_t rotation, uint8_t mirror, int32_t pivot_x,
                          int32_t pivot_z) {
  if (mirror != 0)
    x = -x;
  switch (rotation & 3u) {
  case 1:
    return PortalXZ{pivot_x + pivot_z - z, pivot_z - pivot_x + x};
  case 2:
    return PortalXZ{pivot_x + pivot_x - x, pivot_z + pivot_z - z};
  case 3:
    return PortalXZ{pivot_x - pivot_z + z, pivot_x + pivot_z - x};
  default:
    return PortalXZ{x, z};
  }
}

std::string mirror_front_back(std::string_view properties) {
  if (properties.empty())
    return std::string(properties);
  const std::map<std::string, std::string> parsed = parse_properties(properties);
  const auto facing = parsed.find("facing");
  const bool stair_on_x =
      facing != parsed.end() && (facing->second == "east" || facing->second == "west");
  std::map<std::string, std::string> out;
  for (const auto& [key, value] : parsed) {
    if (key == "facing") {
      if (value == "east")
        out[key] = "west";
      else if (value == "west")
        out[key] = "east";
      else
        out[key] = value;
    } else {
      out[key] = value;
    }
  }
  if (stair_on_x) {
    const auto shape = out.find("shape");
    if (shape != out.end()) {
      if (shape->second == "inner_left")
        shape->second = "inner_right";
      else if (shape->second == "inner_right")
        shape->second = "inner_left";
      else if (shape->second == "outer_left")
        shape->second = "outer_right";
      else if (shape->second == "outer_right")
        shape->second = "outer_left";
    }
  }
  return join_properties(out);
}

bool portal_protected_block(const std::string& name) {
  return name == "minecraft:bedrock" || name == "minecraft:spawner" || name == "minecraft:chest" ||
         name == "minecraft:end_portal_frame" || name == "minecraft:reinforced_deepslate" ||
         name == "minecraft:trial_spawner" || name == "minecraft:vault";
}

bool portal_is_stairs(const std::string& name) {
  return name.size() >= 7 && name.compare(name.size() - 7, 7, "_stairs") == 0;
}
bool portal_is_slab(const std::string& name) {
  return name.size() >= 5 && name.compare(name.size() - 5, 5, "_slab") == 0;
}
bool portal_is_wall(const std::string& name) {
  return name.size() >= 5 && name.compare(name.size() - 5, 5, "_wall") == 0;
}

std::string stone_bricks_for_blackstone(const std::string& name) {
  if (name == "minecraft:cobblestone" || name == "minecraft:mossy_cobblestone")
    return "minecraft:blackstone";
  if (name == "minecraft:stone")
    return "minecraft:polished_blackstone";
  if (name == "minecraft:stone_bricks" || name == "minecraft:mossy_stone_bricks")
    return "minecraft:polished_blackstone_bricks";
  if (name == "minecraft:stone_stairs")
    return "minecraft:polished_blackstone_stairs";
  if (name == "minecraft:stone_brick_stairs" || name == "minecraft:mossy_stone_brick_stairs")
    return "minecraft:polished_blackstone_brick_stairs";
  if (name == "minecraft:cobblestone_stairs" || name == "minecraft:mossy_cobblestone_stairs")
    return "minecraft:blackstone_stairs";
  if (name == "minecraft:stone_slab" || name == "minecraft:smooth_stone_slab")
    return "minecraft:polished_blackstone_slab";
  if (name == "minecraft:stone_brick_slab" || name == "minecraft:mossy_stone_brick_slab")
    return "minecraft:polished_blackstone_brick_slab";
  if (name == "minecraft:cobblestone_slab" || name == "minecraft:mossy_cobblestone_slab")
    return "minecraft:blackstone_slab";
  if (name == "minecraft:stone_brick_wall" || name == "minecraft:mossy_stone_brick_wall")
    return "minecraft:polished_blackstone_brick_wall";
  if (name == "minecraft:cobblestone_wall" || name == "minecraft:mossy_cobblestone_wall")
    return "minecraft:blackstone_wall";
  if (name == "minecraft:chiseled_stone_bricks")
    return "minecraft:chiseled_polished_blackstone";
  if (name == "minecraft:cracked_stone_bricks")
    return "minecraft:cracked_polished_blackstone_bricks";
  if (name == "minecraft:iron_bars")
    return "minecraft:iron_chain";
  return name;
}

BlockStateKey portal_age_block(const BlockStateKey& state, float mossiness,
                               WgLegacyRandom& random) {
  BlockStateKey out = state;
  if (state.name == "minecraft:stone_bricks" || state.name == "minecraft:stone" ||
      state.name == "minecraft:chiseled_stone_bricks") {
    if (random.next_float() >= 0.5f)
      return out;
    const bool mossy = random.next_float() < mossiness;
    if (random.next_int(2) == 0) {
      out.name = mossy ? "minecraft:mossy_stone_bricks" : "minecraft:cracked_stone_bricks";
      out.properties.clear();
    } else {
      out.name = mossy ? "minecraft:mossy_stone_brick_stairs" : "minecraft:stone_brick_stairs";
      const char* facing = (const char*[]){"north", "east", "south", "west"}[random.next_int(4)];
      const char* half = random.next_boolean() ? "top" : "bottom";
      out.properties =
          std::string("facing=") + facing + ",half=" + half + ",shape=straight,waterlogged=false";
    }
    return out;
  }
  if (portal_is_stairs(state.name)) {
    if (random.next_float() >= 0.5f)
      return out;
    if (random.next_int(2) == 0) {
      out.name = "minecraft:mossy_stone_brick_stairs";
    } else {
      out.name = "minecraft:mossy_stone_brick_slab";
      out.properties = "type=bottom,waterlogged=false";
    }
    return out;
  }
  if (portal_is_slab(state.name)) {
    if (random.next_float() < mossiness) {
      out.name = "minecraft:mossy_stone_brick_slab";
    }
    return out;
  }
  if (portal_is_wall(state.name)) {
    if (random.next_float() < mossiness)
      out.name = "minecraft:mossy_stone_brick_wall";
    return out;
  }
  if (state.name == "minecraft:obsidian" && random.next_float() < 0.15f)
    out.name = "minecraft:crying_obsidian";
  return out;
}

} // namespace

namespace {

uint8_t rotate_dir(uint8_t direction, uint32_t rotation) {
  // Direction ordinals: 0 down, 1 up, 2 north, 3 south, 4 west, 5 east.
  if (direction < 2)
    return direction;
  const uint8_t horizontal[4] = {2, 5, 3, 4}; // north, east, south, west
  int32_t index = 0;
  for (int32_t i = 0; i < 4; ++i)
    if (horizontal[i] == direction)
      index = i;
  index = (index + static_cast<int32_t>(rotation & 3u)) & 3;
  return horizontal[index];
}

struct JigsawInfo {
  int32_t x = 0, y = 0, z = 0;
  uint8_t front = 0, top = 0, joint = 0;
  uint16_t name = 0, target = 0, pool = 0, final_state = 0;
  int16_t placement_priority = 0;
  int16_t selection_priority = 0;
};

struct JPiece {
  const WgPieceTemplate* templ = nullptr; // the first part, for the jigsaw pass
  const WgPoolElementRow* element = nullptr;
  uint32_t rotation = 0;
  int32_t x = 0, y = 0, z = 0;
  int32_t ground_delta = 1;
  uint16_t processors = 0xFFFF;
  uint8_t terrain_matching = 0;
  WgPieceBox box;
};

// One `MutableObject<VoxelShape>` from `JigsawPlacement`. The two flavours are
// not the same predicate:
//   * the shared context shape is `bounds` with the centre and every child
//     placed through it *subtracted* — an obstacle list, so a target must not
//     touch any of them;
//   * a source's own shape is the source box itself, which is the *allowed*
//     region (an inside connector may put a child anywhere within the source),
//     minus the children already placed through it.
struct JFree {
  bool containment = false;
  WgPieceBox limit;
  std::vector<WgPieceBox> occupied;
};

// The generated boxes carry inclusive maxima (a 1 x 1 template has min == max),
// and `JigsawPlacement` tests the target box deflated by 0.25 against another
// `AABB.of(BoundingBox)` shape. Two inclusive boxes therefore collide when they
// share at least one cell; half-open comparisons would let a 1 x 1 piece sit on
// top of another and would miss face-adjacent overlaps.
bool jbox_intersects(const WgPieceBox& a, const WgPieceBox& b) {
  return a.min_x <= b.max_x && b.min_x <= a.max_x && a.min_y <= b.max_y && b.min_y <= a.max_y &&
         a.min_z <= b.max_z && b.min_z <= a.max_z;
}

bool jbox_inside(const WgPieceBox& outer, const WgPieceBox& inner) {
  return inner.min_x >= outer.min_x && inner.max_x <= outer.max_x && inner.min_y >= outer.min_y &&
         inner.max_y <= outer.max_y && inner.min_z >= outer.min_z && inner.max_z <= outer.max_z;
}

const char* jigsaw_string(uint16_t index) {
  return kWgJigsawStringTable[index];
}

int32_t pool_index_by_name(const char* name) {
  constexpr size_t kCount = sizeof(kWgTemplatePoolNames) / sizeof(kWgTemplatePoolNames[0]);
  for (size_t i = 0; i < kCount; ++i)
    if (std::strcmp(name, kWgTemplatePoolNames[i]) == 0)
      return static_cast<int32_t>(i);
  return -1;
}

bool pool_is_empty(uint16_t pool) {
  return pool < sizeof(kWgTemplatePools) / sizeof(kWgTemplatePools[0]) &&
         kWgTemplatePools[pool].element_count == 0;
}

const WgPoolElementRow& pool_element(uint16_t pool, uint32_t index) {
  return kWgPoolElements[kWgTemplatePools[pool].element_offset + index];
}

const WgPoolElementPart* element_part(const WgPoolElementRow& element, uint16_t index) {
  if (element.kind != 3 || index >= element.part_count ||
      element.part_offset + index >= sizeof(kWgPoolElementParts) / sizeof(kWgPoolElementParts[0]))
    return nullptr;
  return &kWgPoolElementParts[element.part_offset + index];
}

const WgPieceTemplate* part_template(const WgPoolElementPart& part) {
  if (part.kind == 0 ||
      part.template_index >= sizeof(kWgPieceTemplates) / sizeof(kWgPieceTemplates[0]))
    return nullptr;
  return &kWgPieceTemplates[part.template_index];
}

// The placed feature a `FeaturePoolElement` row names, or null for every other
// kind.
const char* pool_element_feature(const WgPoolElementRow& element) {
  if (element.kind != 4 ||
      element.feature_index >= sizeof(kWgPoolElementFeatures) / sizeof(kWgPoolElementFeatures[0]))
    return nullptr;
  return kWgPoolElementFeatures[element.feature_index];
}

const WgPieceTemplate* element_template(const WgPoolElementRow& element) {
  if (element.kind == 3) {
    const WgPoolElementPart* first = element_part(element, 0);
    return first != nullptr ? part_template(*first) : nullptr;
  }
  // `kind == 4` is a `FeaturePoolElement`: it has no template at all, only a
  // placed feature, so it must not be read as one (its `template_index` slot is
  // not a template index).
  if (element.kind == 0 || element.kind == 4 ||
      element.template_index >= sizeof(kWgPieceTemplates) / sizeof(kWgPieceTemplates[0]))
    return nullptr;
  return &kWgPieceTemplates[element.template_index];
}

// `StructurePoolElement.getBoundingBox`: a single template's box, or for a
// `ListPoolElement` the union of every non-empty part's box.
WgPieceBox element_box(const WgPoolElementRow& element, uint32_t rotation, int32_t x, int32_t y,
                       int32_t z) {
  // `FeaturePoolElement.getSize` is `Vec3i.ZERO`, so its box is the single cell
  // it is handed (position..position), not an empty box. An empty box would let
  // the free-shape test accept an overlap the server rejects.
  if (element.kind == 4)
    return WgPieceBox{x, y, z, x, y, z};
  WgPieceBox box{0, 0, 0, -1, -1, -1};
  bool first = true;
  const auto include = [&](const WgPieceTemplate& templ) {
    const WgPieceBox part = wg_piece_box(templ, rotation, x, y, z);
    if (first) {
      box = part;
      first = false;
      return;
    }
    box.min_x = std::min(box.min_x, part.min_x);
    box.min_y = std::min(box.min_y, part.min_y);
    box.min_z = std::min(box.min_z, part.min_z);
    box.max_x = std::max(box.max_x, part.max_x);
    box.max_y = std::max(box.max_y, part.max_y);
    box.max_z = std::max(box.max_z, part.max_z);
  };
  if (element.kind == 3) {
    for (uint16_t i = 0; i < element.part_count; ++i) {
      const WgPoolElementPart* part = element_part(element, i);
      const WgPieceTemplate* templ = part != nullptr ? part_template(*part) : nullptr;
      if (templ != nullptr)
        include(*templ);
    }
  } else if (const WgPieceTemplate* templ = element_template(element)) {
    include(*templ);
  }
  return box;
}

int32_t pool_max_size(uint16_t pool) {
  if (pool >= sizeof(kWgTemplatePools) / sizeof(kWgTemplatePools[0]))
    return 0;
  int32_t max_size = 0;
  const WgTemplatePoolRow& row = kWgTemplatePools[pool];
  for (uint32_t i = 0; i < row.element_count; ++i) {
    const WgPoolElementRow& element = pool_element(pool, i);
    if (element.kind == 3) {
      for (uint16_t part_index = 0; part_index < element.part_count; ++part_index) {
        const WgPoolElementPart* part = element_part(element, part_index);
        const WgPieceTemplate* templ = part != nullptr ? part_template(*part) : nullptr;
        if (templ != nullptr)
          max_size = std::max(max_size, static_cast<int32_t>(templ->size_y));
      }
    } else if (const WgPieceTemplate* templ = element_template(element)) {
      max_size = std::max(max_size, static_cast<int32_t>(templ->size_y));
    }
  }
  return max_size;
}

std::vector<uint16_t> shuffled_pool(uint16_t pool, WgLegacyRandom& random) {
  std::vector<uint16_t> out;
  if (pool >= sizeof(kWgTemplatePools) / sizeof(kWgTemplatePools[0]))
    return out;
  const WgTemplatePoolRow& row = kWgTemplatePools[pool];
  out.reserve(row.element_count);
  for (uint32_t i = 0; i < row.element_count; ++i)
    out.push_back(static_cast<uint16_t>(i));
  for (size_t i = out.size(); i > 1; --i) {
    const size_t swap = static_cast<size_t>(random.next_int(static_cast<int32_t>(i)));
    std::swap(out[i - 1], out[swap]);
  }
  return out;
}

std::vector<uint8_t> shuffled_rotations(WgLegacyRandom& random) {
  std::vector<uint8_t> out{0, 1, 2, 3};
  for (size_t i = out.size(); i > 1; --i) {
    const size_t swap = static_cast<size_t>(random.next_int(static_cast<int32_t>(i)));
    std::swap(out[i - 1], out[swap]);
  }
  return out;
}

// The `WgJigsawStringTable` index for where `minecraft:bottom` and
// `minecraft:empty` live; the generator interns both so a feature element's
// synthesised connector can name them. `-1` means the string is absent, which
// makes the connector unattachable rather than silently matching the wrong row.
int32_t jigsaw_string_index(const char* text) {
  constexpr size_t kCount = sizeof(kWgJigsawStringTable) / sizeof(kWgJigsawStringTable[0]);
  for (size_t i = 0; i < kCount; ++i)
    if (std::strcmp(text, kWgJigsawStringTable[i]) == 0)
      return static_cast<int32_t>(i);
  return -1;
}

std::vector<JigsawInfo> shuffled_jigsaws(const WgPieceTemplate& templ, int32_t position_x,
                                         int32_t position_y, int32_t position_z, uint32_t rotation,
                                         WgLegacyRandom& random);

// `StructurePoolElement.getShuffledJigsawBlocks` for one element. A template
// element reads its own NBT connectors; a `FeaturePoolElement` synthesises the
// one default connector `fillDefaultJigsawNBT` builds — at the element origin,
// facing down, `name = minecraft:bottom`, `target/pool = minecraft:empty`,
// ROLLABLE — which is what lets a village connector target a decoration.
std::vector<JigsawInfo> element_jigsaws(const WgPoolElementRow& element, int32_t position_x,
                                        int32_t position_y, int32_t position_z, uint32_t rotation,
                                        WgLegacyRandom& random) {
  if (element.kind != 4) {
    const WgPieceTemplate* templ = element_template(element);
    if (templ == nullptr)
      return {};
    return shuffled_jigsaws(*templ, position_x, position_y, position_z, rotation, random);
  }
  // The string table is large and a village can try a dozen feature candidates
  // per connector, so the three lookups are resolved once rather than per
  // candidate.
  static const int32_t name = jigsaw_string_index("minecraft:bottom");
  static const int32_t empty = jigsaw_string_index("minecraft:empty");
  static const int32_t air = jigsaw_string_index("minecraft:air");
  if (name < 0 || empty < 0 || air < 0)
    return {};
  // `FeaturePoolElement.getShuffledJigsawBlocks` returns a `List.of` with the
  // one default connector; unlike the template path it does not shuffle, so it
  // must not consume a draw here.
  JigsawInfo info{position_x,
                  position_y,
                  position_z,
                  /*front=*/0,
                  /*top=*/3,
                  /*joint=*/0,
                  static_cast<uint16_t>(name),
                  static_cast<uint16_t>(empty),
                  static_cast<uint16_t>(empty),
                  static_cast<uint16_t>(air),
                  /*placement_priority=*/0,
                  /*selection_priority=*/0};
  return {info};
}

std::vector<JigsawInfo> shuffled_jigsaws(const WgPieceTemplate& templ, int32_t position_x,
                                         int32_t position_y, int32_t position_z, uint32_t rotation,
                                         WgLegacyRandom& random) {
  std::vector<JigsawInfo> out;
  out.reserve(templ.jigsaw_count);
  for (uint32_t i = 0; i < templ.jigsaw_count; ++i) {
    const WgPieceJigsaw& jigsaw = kWgPieceJigsaws[templ.jigsaw_offset + i];
    const XZ offset = rotate_pivot(jigsaw.x, jigsaw.z, rotation, templ.pivot_x, templ.pivot_z);
    out.push_back(JigsawInfo{position_x + offset.x, position_y + jigsaw.y, position_z + offset.z,
                             rotate_dir(jigsaw.front, rotation), rotate_dir(jigsaw.top, rotation),
                             jigsaw.joint, jigsaw.name, jigsaw.target, jigsaw.pool,
                             jigsaw.final_state, jigsaw.placement_priority,
                             jigsaw.selection_priority});
  }
  for (size_t i = out.size(); i > 1; --i) {
    const size_t swap = static_cast<size_t>(random.next_int(static_cast<int32_t>(i)));
    std::swap(out[i - 1], out[swap]);
  }
  std::stable_sort(out.begin(), out.end(), [](const JigsawInfo& a, const JigsawInfo& b) {
    return a.selection_priority > b.selection_priority;
  });
  return out;
}

bool can_attach(const JigsawInfo& source, const JigsawInfo& target) {
  return source.front == (target.front ^ 1) && (source.joint == 0 || source.top == target.top) &&
         std::strcmp(jigsaw_string(source.target), jigsaw_string(target.name)) == 0;
}

int32_t direction_step_x(uint8_t direction) {
  return direction == 5 ? 1 : (direction == 4 ? -1 : 0);
}
int32_t direction_step_z(uint8_t direction) {
  return direction == 3 ? 1 : (direction == 2 ? -1 : 0);
}
int32_t direction_step_y(uint8_t direction) {
  return direction == 1 ? 1 : (direction == 0 ? -1 : 0);
}

} // namespace

// The shared `JigsawStructure.findGenerationPoint` + `JigsawPlacement` body.
// Villages and pillager outposts differ only in their structure set, start
// pool and `size`; both start at the chunk corner with `start_height` 0 and
// project the centre piece to `WORLD_SURFACE_WG`.
// One member of a jigsaw structure set. `ChunkGenerator.createStructures`
// owns *which* member a chunk tries and what happens when it fails; this is the
// `Structure.generate` -> `JigsawStructure.findGenerationPoint` ->
// `JigsawPlacement.addPieces` body that choice calls.
static WgJigsawStart build_jigsaw_start_for_structure(int64_t seed, int32_t chunk_x,
                                                      int32_t chunk_z, WgClimateSampler& sampler,
                                                      WgStructureHeights& heights,
                                                      const WgStructure& structure,
                                                      bool skip_biome = false) {
  WgJigsawStart start;
  const WgJigsawConfig* config = nullptr;
  for (const WgJigsawConfig& candidate : kWgJigsawConfigs) {
    if (std::strcmp(candidate.name, structure.name) == 0) {
      config = &candidate;
      break;
    }
  }
  if (config == nullptr)
    return start;
  const uint8_t max_depth = config->max_depth;
  const uint16_t start_pool = config->start_pool;
  if (start_pool >= sizeof(kWgTemplatePools) / sizeof(kWgTemplatePools[0]))
    return start;
  const uint32_t start_count = kWgTemplatePools[start_pool].element_count;
  if (start_count == 0)
    return start;

  WgLegacyRandom random(0);
  seed_large_feature(random, seed, chunk_x, chunk_z);
  // `JigsawStructure.findGenerationPoint` samples `startHeight` *before*
  // `JigsawPlacement` draws the rotation. An absolute provider consumes
  // nothing; a uniform one consumes exactly one draw.
  int32_t start_height = config->start_height_min;
  if (config->start_height_kind == 1) {
    const int32_t span = static_cast<int32_t>(config->start_height_max) -
                         static_cast<int32_t>(config->start_height_min) + 1;
    start_height += random.next_int(span);
  }
  const uint32_t rotation = static_cast<uint32_t>(random.next_int(4));
  const uint32_t center_choice =
      static_cast<uint32_t>(random.next_int(static_cast<int32_t>(start_count)));
  const WgPoolElementRow& center_element = pool_element(start_pool, center_choice);
  const WgPieceTemplate* center_template = element_template(center_element);
  if (center_template == nullptr)
    return start;

  JPiece center_piece;
  center_piece.templ = center_template;
  center_piece.element = &center_element;
  center_piece.rotation = rotation;
  center_piece.x = chunk_x * 16;
  center_piece.y = start_height;
  center_piece.z = chunk_z * 16;
  center_piece.ground_delta = 1;
  center_piece.processors = center_element.processor_list;
  center_piece.terrain_matching = center_element.projection;

  // `JigsawPlacement.getRandomNamedJigsaw`: the ancient city's `city_anchor`
  // names the connector that must land on the start position. The lookup walks
  // the *shuffled* connector list, so it consumes the same stream a plain
  // centre piece would spend on its later child pass. Without this, the city
  // centre is anchored at its template corner and the whole city sits a few
  // blocks off the server's grid.
  int32_t anchor_local_x = 0;
  int32_t anchor_local_y = 0;
  int32_t anchor_local_z = 0;
  if (config->start_jigsaw != 0xFFFF) {
    const char* wanted = jigsaw_string(config->start_jigsaw);
    const std::vector<JigsawInfo> anchors = shuffled_jigsaws(
        *center_template, center_piece.x, center_piece.y, center_piece.z, rotation, random);
    bool found = false;
    for (const JigsawInfo& anchor : anchors) {
      if (std::strcmp(jigsaw_string(anchor.name), wanted) != 0)
        continue;
      anchor_local_x = anchor.x - center_piece.x;
      anchor_local_y = anchor.y - center_piece.y;
      anchor_local_z = anchor.z - center_piece.z;
      found = true;
      break;
    }
    if (!found)
      return start;
    center_piece.x -= anchor_local_x;
    center_piece.y -= anchor_local_y;
    center_piece.z -= anchor_local_z;
  }
  center_piece.box =
      element_box(center_element, rotation, center_piece.x, center_piece.y, center_piece.z);
  const int32_t box_center_x = (center_piece.box.min_x + center_piece.box.max_x) / 2;
  const int32_t box_center_z = (center_piece.box.min_z + center_piece.box.max_z) / 2;
  // `JigsawPlacement`: `bottomY` is the sampled start height (projected onto
  // the surface when the structure asks for it); the stub's `centerY` adds the
  // named anchor's local Y. `Structure.isValidBiome` runs on the stub.
  const int32_t bottom_y = config->project_heightmap
                               ? start_height + heights.height(box_center_x, box_center_z,
                                                               WgStructureHeightmap::kWorldSurface)
                               : center_piece.y;
  const int32_t center_y = bottom_y + anchor_local_y;
  if (!skip_biome &&
      !wg_structure_start_biome_is_valid(sampler, structure, box_center_x, center_y, box_center_z))
    return start;
  const int32_t old_absolute_ground = center_piece.box.min_y + center_piece.ground_delta;
  const int32_t move_y = bottom_y - old_absolute_ground;
  center_piece.y += move_y;
  center_piece.box.min_y += move_y;
  center_piece.box.max_y += move_y;

  struct Queued {
    JPiece piece;
    std::shared_ptr<JFree> free;
    int32_t depth;
  };
  std::vector<JPiece> assembled;
  assembled.push_back(center_piece);

  const int32_t max_distance = static_cast<int32_t>(config->max_distance);
  WgPieceBox bounds;
  bounds.min_x = box_center_x - max_distance;
  bounds.max_x = box_center_x + max_distance;
  bounds.min_z = box_center_z - max_distance;
  bounds.max_z = box_center_z + max_distance;
  // `JigsawPlacement` centres the free shape on the stub's `centerY`.
  bounds.min_y = std::max(center_y - max_distance, kWgMinY);
  bounds.max_y = std::min(center_y + max_distance, kWgMinY + kWgHeight - 1);
  // `JigsawPlacement` builds its shared free shape as `join(bounds, centerBox,
  // ONLY_FIRST)` — the bounds *minus* the centre piece — then subtracts each
  // child placed through it. `JFree` stores that as obstacles. An inside
  // connector instead uses its source box as the allowed region (the outpost
  // watchtower must be *contained* in its base plate), which is the
  // `containment` flavour below.
  auto context_free = std::make_shared<JFree>();
  context_free->occupied.push_back(center_piece.box);
  // `JigsawPlacement.Placer.placing` is a `SequencedPriorityIterator`: the
  // highest `placement_priority` is processed first, FIFO inside one priority.
  // A plain BFS deque matches vanilla only while every connector is priority 0.
  std::map<int32_t, std::deque<Queued>, std::greater<int32_t>> queue;
  queue[0].push_back(Queued{center_piece, context_free, 0});

  const auto free_contains = [&](const std::shared_ptr<JFree>& free, const WgPieceBox& box) {
    if (!jbox_inside(bounds, box))
      return false;
    if (free->containment && !jbox_inside(free->limit, box))
      return false;
    for (const WgPieceBox& occupied : free->occupied)
      if (jbox_intersects(box, occupied))
        return false;
    return true;
  };

  while (!queue.empty() && assembled.size() < WgJigsawStart::kMaxPieces) {
    auto next = queue.begin();
    Queued source = std::move(next->second.front());
    next->second.pop_front();
    if (next->second.empty())
      queue.erase(next);
    const JPiece& source_piece = source.piece;
    if (source_piece.templ == nullptr)
      continue;
    auto source_free = std::make_shared<JFree>();
    source_free->containment = true;
    source_free->limit = source_piece.box;
    const int32_t source_box_y = source_piece.box.min_y;

    const std::vector<JigsawInfo> source_jigsaws =
        shuffled_jigsaws(*source_piece.templ, source_piece.x, source_piece.y, source_piece.z,
                         source_piece.rotation, random);
    for (const JigsawInfo& source_jigsaw : source_jigsaws) {
      // `JigsawPlacement` declares this per connector: a later connector on the
      // same source must sample its own column, not reuse the first one's.
      int32_t source_jigsaw_base_height = std::numeric_limits<int32_t>::min();
      const int32_t target_pool = pool_index_by_name(jigsaw_string(source_jigsaw.pool));
      if (target_pool < 0)
        continue;
      const uint16_t pool = static_cast<uint16_t>(target_pool);
      if (pool_is_empty(pool) &&
          std::strcmp(jigsaw_string(source_jigsaw.pool), "minecraft:empty") != 0)
        continue;
      const int32_t fallback_pool = kWgTemplatePools[pool].fallback;
      if (fallback_pool < 0 || fallback_pool >= static_cast<int32_t>(sizeof(kWgTemplatePools) /
                                                                     sizeof(kWgTemplatePools[0])))
        continue;
      const int32_t source_local_y = source_jigsaw.y - source_box_y;
      const int32_t target_x = source_jigsaw.x + direction_step_x(source_jigsaw.front);
      const int32_t target_y = source_jigsaw.y + direction_step_y(source_jigsaw.front);
      const int32_t target_z = source_jigsaw.z + direction_step_z(source_jigsaw.front);
      // `JigsawPlacement`: ``sourceBB.isInside(targetJigsawPos)`` — the cell
      // *in front* of the connector, not the connector cell itself (which is
      // always inside its own piece). Testing the connector cell made every
      // attachment "inside" and rejected the outpost watchtower against the
      // base plate it is supposed to sit on.
      const bool attach_inside = jbox_inside(
          source_piece.box, WgPieceBox{target_x, target_y, target_z, target_x, target_y, target_z});
      const std::shared_ptr<JFree> children_free = attach_inside ? source_free : source.free;
      if (std::getenv("PLUSMC_WG_DEBUG") != nullptr)
        std::printf("    anchor jigsaw=%d,%d,%d front=%u target=%d,%d,%d inside=%d "
                    "source=%d,%d,%d..%d,%d,%d\n",
                    source_jigsaw.x, source_jigsaw.y, source_jigsaw.z, source_jigsaw.front,
                    target_x, target_y, target_z, attach_inside ? 1 : 0, source_piece.box.min_x,
                    source_piece.box.min_y, source_piece.box.min_z, source_piece.box.max_x,
                    source_piece.box.max_y, source_piece.box.max_z);
      if (std::getenv("PLUSMC_WG_DEBUG") != nullptr)
        std::printf("      bounds=%d,%d,%d..%d,%d,%d occupied=%zu\n", bounds.min_x, bounds.min_y,
                    bounds.min_z, bounds.max_x, bounds.max_y, bounds.max_z,
                    children_free->occupied.size());
      if (std::getenv("PLUSMC_WG_DEBUG") != nullptr)
        for (const WgPieceBox& occupied : children_free->occupied)
          std::printf("        occ=%d,%d,%d..%d,%d,%d\n", occupied.min_x, occupied.min_y,
                      occupied.min_z, occupied.max_x, occupied.max_y, occupied.max_z);

      std::vector<const WgPoolElementRow*> target_elements;
      if (source.depth != max_depth) {
        const std::vector<uint16_t> pool_order = shuffled_pool(pool, random);
        target_elements.reserve(pool_order.size());
        for (uint16_t element_index : pool_order)
          target_elements.push_back(&pool_element(pool, element_index));
      }
      const std::vector<uint16_t> fallback_order =
          shuffled_pool(static_cast<uint16_t>(fallback_pool), random);
      for (uint16_t element_index : fallback_order)
        target_elements.push_back(
            &pool_element(static_cast<uint16_t>(fallback_pool), element_index));

      if (std::getenv("PLUSMC_WG_DEBUG") != nullptr)
        std::printf("  source jig pool=%s targetPool=%d count=%zu fallback=%zu\n",
                    jigsaw_string(source_jigsaw.pool), target_pool, target_elements.size(),
                    fallback_order.size());
      bool placed = false;
      for (const WgPoolElementRow* element : target_elements) {
        const WgPoolElementRow& row = *element;
        if (row.kind == 0)
          break;
        // A `FeaturePoolElement` is a real candidate with no template: it
        // carries one synthesised connector and places a feature at its cell.
        const WgPieceTemplate* target_template = element_template(row);
        for (uint8_t target_rotation : shuffled_rotations(random)) {
          const std::vector<JigsawInfo> target_jigsaws =
              element_jigsaws(row, 0, 0, 0, target_rotation, random);
          const WgPieceBox hack_box = element_box(row, target_rotation, 0, 0, 0);
          int32_t expand_to = 0;
          if (config->use_expansion_hack && hack_box.max_y - hack_box.min_y + 1 <= 16) {
            for (const JigsawInfo& jigsaw : target_jigsaws) {
              const int32_t front_x = jigsaw.x + direction_step_x(jigsaw.front);
              const int32_t front_y = jigsaw.y + direction_step_y(jigsaw.front);
              const int32_t front_z = jigsaw.z + direction_step_z(jigsaw.front);
              // `JigsawPlacement`: only a connector whose front cell is still
              // inside the template contributes its child pool's size; one
              // pointing out of the box returns 0 from the stream. The check is
              // on the front cell, all three axes: using the connector's own y
              // let an upward connector (the village well's bottom piece) count
              // a child and grow a box the server leaves at its template size.
              if (front_x < hack_box.min_x || front_x > hack_box.max_x ||
                  front_y < hack_box.min_y || front_y > hack_box.max_y ||
                  front_z < hack_box.min_z || front_z > hack_box.max_z)
                continue;
              const int32_t child_pool = pool_index_by_name(jigsaw_string(jigsaw.pool));
              if (child_pool < 0)
                continue;
              const int32_t child_fallback = kWgTemplatePools[child_pool].fallback;
              expand_to = std::max(
                  expand_to, std::max(pool_max_size(static_cast<uint16_t>(child_pool)),
                                      child_fallback >= 0
                                          ? pool_max_size(static_cast<uint16_t>(child_fallback))
                                          : 0));
            }
          }
          for (const JigsawInfo& target_jigsaw : target_jigsaws) {
            if (!can_attach(source_jigsaw, target_jigsaw))
              continue;
            const int32_t raw_x = target_x - target_jigsaw.x;
            const int32_t raw_y = target_y - target_jigsaw.y;
            const int32_t raw_z = target_z - target_jigsaw.z;
            WgPieceBox target_box = element_box(row, target_rotation, raw_x, raw_y, raw_z);
            const int32_t target_local_y = target_jigsaw.y;
            const int32_t delta_y =
                source_local_y - target_local_y + direction_step_y(source_jigsaw.front);
            if (std::getenv("PLUSMC_WG_DEBUG") != nullptr)
              std::printf("      try targetjig=%d,%d,%d localY=%d delta=%d rawboxY=%d srcBoxY=%d "
                          "rigid=%d/%d\n",
                          target_jigsaw.x, target_jigsaw.y, target_jigsaw.z, target_local_y,
                          delta_y, target_box.min_y, source_box_y,
                          source_piece.terrain_matching == 0 ? 1 : 0, row.projection == 0 ? 1 : 0);
            int32_t target_box_y = 0;
            const bool source_rigid = source_piece.terrain_matching == 0;
            const bool target_rigid = row.projection == 0;
            if (source_rigid && target_rigid) {
              target_box_y = source_box_y + delta_y;
            } else {
              if (source_jigsaw_base_height == std::numeric_limits<int32_t>::min())
                source_jigsaw_base_height = heights.height(source_jigsaw.x, source_jigsaw.z,
                                                           WgStructureHeightmap::kWorldSurface);
              target_box_y = source_jigsaw_base_height - target_local_y;
            }
            const int32_t y_offset = target_box_y - target_box.min_y;
            const int32_t target_pos_y = raw_y + y_offset;
            target_box.min_y += y_offset;
            target_box.max_y += y_offset;
            if (expand_to > 0) {
              // `newSize = max(expandTo + 1, targetBB.maxY() - targetBB.minY())`,
              // then the box encapsulates (minX, minY + newSize, minZ). Both
              // bounds are inclusive, so this grows the span to max(old,
              // expandTo + 2) without adding the extra cell an off-by-one
              // `span + 1` would.
              const int32_t new_size = std::max(expand_to + 1, target_box.max_y - target_box.min_y);
              target_box.max_y = std::max(target_box.max_y, target_box.min_y + new_size);
            }
            if (!free_contains(children_free, target_box)) {
              if (std::getenv("PLUSMC_WG_DEBUG") != nullptr) {
                std::printf("    reject box %d,%d,%d..%d,%d,%d expand=%d inbounds=%d\n",
                            target_box.min_x, target_box.min_y, target_box.min_z, target_box.max_x,
                            target_box.max_y, target_box.max_z, expand_to,
                            jbox_inside(bounds, target_box) ? 1 : 0);
                for (const WgPieceBox& occupied : children_free->occupied)
                  std::printf("      hit=%d with %d,%d,%d..%d,%d,%d\n",
                              jbox_intersects(target_box, occupied) ? 1 : 0, occupied.min_x,
                              occupied.min_y, occupied.min_z, occupied.max_x, occupied.max_y,
                              occupied.max_z);
              }
              continue;
            }
            children_free->occupied.push_back(target_box);
            JPiece target_piece;
            target_piece.templ = target_template;
            target_piece.element = &row;
            target_piece.rotation = target_rotation;
            target_piece.x = raw_x;
            target_piece.y = target_pos_y;
            target_piece.z = raw_z;
            target_piece.ground_delta = target_rigid ? source_piece.ground_delta - delta_y : 1;
            target_piece.processors = row.processor_list;
            target_piece.terrain_matching = row.projection;
            target_piece.box = target_box;
            if (std::getenv("PLUSMC_WG_DEBUG") != nullptr)
              std::printf("    accept %s rot=%u pos=%d,%d,%d box=%d,%d,%d..%d,%d,%d expand=%d\n",
                          target_template != nullptr ? target_template->name : "<feature>",
                          target_rotation, raw_x, target_pos_y, raw_z, target_box.min_x,
                          target_box.min_y, target_box.min_z, target_box.max_x, target_box.max_y,
                          target_box.max_z, expand_to);
            assembled.push_back(target_piece);
            if (source.depth + 1 <= max_depth)
              queue[source_jigsaw.placement_priority].push_back(
                  Queued{target_piece, children_free, source.depth + 1});
            placed = true;
            break;
          }
          if (placed)
            break;
        }
        if (placed)
          break;
      }
      // `continue block0`: one child per source connector, then the next
      // connector. Stopping the whole piece here was why a base plate grew a
      // single feature instead of the watchtower and its satellites.
    }
  }

  start.valid = true;
  start.structure = &structure;
  start.world_seed = seed;
  start.chunk_x = chunk_x;
  start.chunk_z = chunk_z;
  start.rotation = rotation;
  start.piece_count =
      static_cast<uint32_t>(std::min<size_t>(assembled.size(), WgJigsawStart::kMaxPieces));
  for (uint32_t i = 0; i < start.piece_count; ++i) {
    const JPiece& piece = assembled[i];
    start.pieces[i].templ = piece.templ;
    start.pieces[i].element = piece.element;
    start.pieces[i].rotation = piece.rotation;
    start.pieces[i].x = piece.x;
    start.pieces[i].y = piece.y;
    start.pieces[i].z = piece.z;
    start.pieces[i].box = piece.box;
    start.pieces[i].processor_list = piece.processors;
    start.pieces[i].terrain_matching = piece.terrain_matching;
  }
  return start;
}

// `ChunkGenerator.createStructures`' weighted member choice and retry. A set
// with more than one member (the five village types) picks one weighted at
// random, tries to build it, and on an invalid start (a failed biome gate)
// removes that member and draws again from the shrunken total — vanilla retries
// the *other* members, it does not give up on the chunk. The single-member case
// (outposts) draws nothing, exactly as the server's branch does.
static WgJigsawStart build_jigsaw_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                        WgClimateSampler& sampler, WgStructureHeights& heights,
                                        const char* set_name, bool skip_biome = false) {
  WgJigsawStart start;
  const WgStructureSet* set = wg_structure_set_by_name(set_name);
  if (set == nullptr || !wg_structure_is_chunk(seed, *set, chunk_x, chunk_z))
    return start;

  std::vector<const WgStructure*> options;
  std::vector<int32_t> weights;
  int32_t total = 0;
  for (uint32_t i = 0; i < set->entry_count; ++i) {
    const WgStructureSetEntry& entry = kWgStructureSetEntries[set->entry_offset + i];
    const WgStructure* structure = wg_structure_by_name(entry.structure);
    if (structure == nullptr)
      continue;
    options.push_back(structure);
    weights.push_back(static_cast<int32_t>(entry.weight));
    total += static_cast<int32_t>(entry.weight);
  }
  if (total <= 0)
    return start;
  if (options.size() == 1)
    return build_jigsaw_start_for_structure(seed, chunk_x, chunk_z, sampler, heights, *options[0],
                                            skip_biome);

  WgLegacyRandom pick_random(0);
  seed_large_feature(pick_random, seed, chunk_x, chunk_z);
  while (!options.empty() && total > 0) {
    int32_t choice = pick_random.next_int(total);
    size_t index = 0;
    while (index < options.size()) {
      choice -= weights[index];
      if (choice < 0)
        break;
      ++index;
    }
    if (index >= options.size())
      index = options.size() - 1;
    WgJigsawStart built = build_jigsaw_start_for_structure(seed, chunk_x, chunk_z, sampler, heights,
                                                           *options[index], skip_biome);
    if (built.valid)
      return built;
    total -= weights[index];
    options.erase(options.begin() + static_cast<std::ptrdiff_t>(index));
    weights.erase(weights.begin() + static_cast<std::ptrdiff_t>(index));
  }
  return start;
}

WgJigsawStart wg_village_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                               WgClimateSampler& sampler, WgStructureHeights& heights) {
  return build_jigsaw_start(seed, chunk_x, chunk_z, sampler, heights, "minecraft:villages");
}

WgJigsawStart wg_outpost_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                               WgClimateSampler& sampler, WgStructureHeights& heights) {
  return build_jigsaw_start(seed, chunk_x, chunk_z, sampler, heights,
                            "minecraft:pillager_outposts");
}

WgJigsawStart wg_ancient_city_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                    WgClimateSampler& sampler, WgStructureHeights& heights) {
  return build_jigsaw_start(seed, chunk_x, chunk_z, sampler, heights, "minecraft:ancient_cities");
}

WgJigsawStart wg_trail_ruins_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                   WgClimateSampler& sampler, WgStructureHeights& heights) {
  return build_jigsaw_start(seed, chunk_x, chunk_z, sampler, heights, "minecraft:trail_ruins");
}

namespace {

const WgPieceTemplate* end_city_template(const char* name) {
  for (size_t i = 0; i < sizeof(kWgPieceTemplates) / sizeof(kWgPieceTemplates[0]); ++i)
    if (std::strcmp(kWgPieceTemplates[i].name, name) == 0)
      return &kWgPieceTemplates[i];
  return nullptr;
}

struct EndCityPart {
  const WgPieceTemplate* templ = nullptr;
  int32_t x = 0, y = 0, z = 0;
  uint32_t rotation = 0;
  uint8_t overwrite_air = 0;
  int32_t gen_depth = 0;
  WgPieceBox box;
};

struct EndCityOffset {
  int32_t x = 0, y = 0, z = 0;
};

// `EndCityPieces`' four `SectionGenerator`s. The generators append into the
// *child* list they are handed; `recursive` is what decides whether that batch
// survives the collision rule and then merges it into the current list.
struct EndCityBuilder {
  WgLegacyRandom& random;
  bool ship_created = false;

  int32_t add(std::vector<EndCityPart>& out, const EndCityPart& parent, EndCityOffset offset,
              const char* name, uint32_t rotation, bool overwrite) {
    EndCityPart part;
    part.templ = end_city_template(name);
    if (part.templ == nullptr)
      return -1;
    const XZ rotated = rotate_pivot(offset.x, offset.z, parent.rotation, 0, 0);
    part.x = parent.x + rotated.x;
    part.y = parent.y + offset.y;
    part.z = parent.z + rotated.z;
    part.rotation = rotation;
    part.overwrite_air = overwrite ? 1 : 0;
    part.box = wg_piece_box(*part.templ, rotation, part.x, part.y, part.z);
    out.push_back(part);
    return static_cast<int32_t>(out.size() - 1);
  }

  bool house_tower(int depth, const EndCityPart& parent, EndCityOffset offset,
                   std::vector<EndCityPart>& out) {
    if (depth > 8)
      return false;
    int32_t last = add(out, parent, offset, "minecraft:end_city/base_floor", parent.rotation, true);
    if (last < 0)
      return false;
    const int32_t floors = random.next_int(3);
    if (floors == 0) {
      last =
          add(out, out[last], {-1, 4, -1}, "minecraft:end_city/base_roof", parent.rotation, true);
    } else if (floors == 1) {
      last = add(out, out[last], {-1, 0, -1}, "minecraft:end_city/second_floor_2", parent.rotation,
                 false);
      if (last >= 0)
        last = add(out, out[last], {-1, 8, -1}, "minecraft:end_city/second_roof", parent.rotation,
                   false);
      if (last >= 0)
        recursive(TOWER, depth + 1, out[last], out, {}, false);
    } else if (floors == 2) {
      last = add(out, out[last], {-1, 0, -1}, "minecraft:end_city/second_floor_2", parent.rotation,
                 false);
      if (last >= 0)
        last = add(out, out[last], {-1, 4, -1}, "minecraft:end_city/third_floor_2", parent.rotation,
                   false);
      if (last >= 0)
        last = add(out, out[last], {-1, 8, -1}, "minecraft:end_city/third_roof", parent.rotation,
                   true);
      if (last >= 0)
        recursive(TOWER, depth + 1, out[last], out, {}, false);
    }
    return last >= 0;
  }

  bool tower(int depth, const EndCityPart& parent, EndCityOffset offset,
             std::vector<EndCityPart>& out) {
    (void)offset;
    int32_t last = add(out, parent, {3 + random.next_int(2), -3, 3 + random.next_int(2)},
                       "minecraft:end_city/tower_base", parent.rotation, true);
    if (last < 0)
      return false;
    last = add(out, out[last], {0, 7, 0}, "minecraft:end_city/tower_piece", parent.rotation, true);
    if (last < 0)
      return false;
    int32_t bridge = random.next_int(3) == 0 ? last : -1;
    const int32_t height = 1 + random.next_int(3);
    for (int32_t i = 0; i < height; ++i) {
      last =
          add(out, out[last], {0, 4, 0}, "minecraft:end_city/tower_piece", parent.rotation, true);
      if (last < 0)
        return false;
      if (i >= height - 1 || !random.next_boolean())
        continue;
      bridge = last;
    }
    if (bridge >= 0) {
      static constexpr uint32_t kBridgeRot[4] = {0, 1, 3, 2};
      static constexpr EndCityOffset kBridgeOffset[4] = {
          {1, -1, 0}, {6, -1, 1}, {0, -1, 5}, {5, -1, 6}};
      for (int32_t i = 0; i < 4; ++i) {
        if (!random.next_boolean())
          continue;
        const int32_t start =
            add(out, out[bridge], kBridgeOffset[i], "minecraft:end_city/bridge_end",
                (parent.rotation + kBridgeRot[i]) & 3u, true);
        if (start >= 0)
          recursive(TOWER_BRIDGE, depth + 1, out[start], out, {}, false);
      }
      last =
          add(out, out[last], {-1, 4, -1}, "minecraft:end_city/tower_top", parent.rotation, true);
    } else if (depth == 7) {
      last =
          add(out, out[last], {-1, 4, -1}, "minecraft:end_city/tower_top", parent.rotation, true);
    } else {
      return recursive(FAT_TOWER, depth + 1, out[last], out, {}, false);
    }
    return last >= 0;
  }

  bool tower_bridge(int depth, const EndCityPart& parent, EndCityOffset,
                    std::vector<EndCityPart>& out) {
    const uint32_t rotation = parent.rotation;
    const int32_t length = random.next_int(4) + 1;
    if (std::getenv("PLUSMC_WG_DEBUG") != nullptr)
      std::fprintf(stderr, "TB depth=%d parent=%d,%d,%d rot=%u len=%d ship=%d\n", depth, parent.x,
                   parent.y, parent.z, rotation, length, ship_created ? 1 : 0);
    int32_t last = add(out, parent, {0, 0, -4}, "minecraft:end_city/bridge_piece", rotation, true);
    if (last < 0)
      return false;
    out[last].gen_depth = -1;
    int32_t next_y = 0;
    for (int32_t i = 0; i < length; ++i) {
      if (random.next_boolean()) {
        last =
            add(out, out[last], {0, next_y, -4}, "minecraft:end_city/bridge_piece", rotation, true);
        next_y = 0;
        continue;
      }
      if (random.next_boolean())
        last = add(out, out[last], {0, next_y, -4}, "minecraft:end_city/bridge_steep_stairs",
                   rotation, true);
      else
        last = add(out, out[last], {0, next_y, -8}, "minecraft:end_city/bridge_gentle_stairs",
                   rotation, true);
      next_y = 4;
      if (last < 0)
        return false;
    }
    if (ship_created || random.next_int(10 - depth) != 0) {
      if (!recursive(HOUSE_TOWER, depth + 1, out[last], out, {-3, next_y + 1, -11}, true))
        return false;
    } else {
      // The ship is appended but is not the new `lastPiece`: vanilla keeps
      // the bridge piece as the parent of the closing `bridge_end`.
      if (add(out, out[last], {-8 + random.next_int(8), next_y, -70 + random.next_int(10)},
              "minecraft:end_city/ship", rotation, true) < 0)
        return false;
      ship_created = true;
    }
    if (last < 0)
      return false;
    last = add(out, out[last], {4, next_y, 0}, "minecraft:end_city/bridge_end", (rotation + 2) & 3u,
               true);
    if (last >= 0)
      out[last].gen_depth = -1;
    return last >= 0;
  }

  bool fat_tower(int depth, const EndCityPart& parent, EndCityOffset,
                 std::vector<EndCityPart>& out) {
    const uint32_t rotation = parent.rotation;
    int32_t last =
        add(out, parent, {-3, 4, -3}, "minecraft:end_city/fat_tower_base", rotation, true);
    if (last < 0)
      return false;
    last = add(out, out[last], {0, 4, 0}, "minecraft:end_city/fat_tower_middle", rotation, true);
    if (last < 0)
      return false;
    for (int32_t i = 0; i < 2 && random.next_int(3) != 0; ++i) {
      last = add(out, out[last], {0, 8, 0}, "minecraft:end_city/fat_tower_middle", rotation, true);
      if (last < 0)
        return false;
      static constexpr uint32_t kBridgeRot[4] = {0, 1, 3, 2};
      static constexpr EndCityOffset kBridgeOffset[4] = {
          {4, -1, 0}, {12, -1, 4}, {0, -1, 8}, {8, -1, 12}};
      for (int32_t b = 0; b < 4; ++b) {
        if (!random.next_boolean())
          continue;
        const int32_t start = add(out, out[last], kBridgeOffset[b], "minecraft:end_city/bridge_end",
                                  (rotation + kBridgeRot[b]) & 3u, true);
        if (start >= 0)
          recursive(TOWER_BRIDGE, depth + 1, out[start], out, {}, false);
      }
    }
    last = add(out, out[last], {-2, 8, -2}, "minecraft:end_city/fat_tower_top", rotation, true);
    return last >= 0;
  }

  enum Kind { HOUSE_TOWER, TOWER, TOWER_BRIDGE, FAT_TOWER };

  bool recursive(Kind kind, int depth, const EndCityPart& parent, std::vector<EndCityPart>& current,
                 EndCityOffset offset, bool has_offset) {
    if (depth > 8)
      return false;
    std::vector<EndCityPart> child;
    bool generated = false;
    switch (kind) {
    case HOUSE_TOWER:
      generated = house_tower(depth, parent, has_offset ? offset : EndCityOffset{}, child);
      break;
    case TOWER:
      generated = tower(depth, parent, offset, child);
      break;
    case TOWER_BRIDGE:
      generated = tower_bridge(depth, parent, offset, child);
      break;
    case FAT_TOWER:
      generated = fat_tower(depth, parent, offset, child);
      break;
    }
    if (!generated)
      return false;
    const int32_t child_tag = random.next_int();
    bool collision = false;
    for (EndCityPart& part : child) {
      part.gen_depth = child_tag;
      for (const EndCityPart& existing : current) {
        const bool intersects =
            part.box.min_x <= existing.box.max_x && part.box.max_x >= existing.box.min_x &&
            part.box.min_z <= existing.box.max_z && part.box.max_z >= existing.box.min_z &&
            part.box.min_y <= existing.box.max_y && part.box.max_y >= existing.box.min_y;
        if (intersects && existing.gen_depth != parent.gen_depth) {
          collision = true;
          break;
        }
      }
      if (collision)
        break;
    }
    if (collision)
      return false;
    current.insert(current.end(), child.begin(), child.end());
    return true;
  }
};

} // namespace

WgJigsawStart wg_end_city_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                WgClimateSampler& sampler, WgStructureHeights& heights,
                                bool skip_biome) {
  WgJigsawStart start;
  const WgStructureSet* set = wg_structure_set_by_name("minecraft:end_cities");
  if (set == nullptr || !wg_structure_is_chunk(seed, *set, chunk_x, chunk_z))
    return start;
  const WgStructure* structure = wg_structure_by_name("minecraft:end_city");
  if (structure == nullptr)
    return start;
  WgLegacyRandom random(0);
  seed_large_feature(random, seed, chunk_x, chunk_z);
  const uint32_t rotation = static_cast<uint32_t>(random.next_int(4));
  int32_t offset_x = 5;
  int32_t offset_z = 5;
  if (rotation == 1)
    offset_x = -5;
  else if (rotation == 2)
    offset_x = -5, offset_z = -5;
  else if (rotation == 3)
    offset_z = -5;
  const int32_t block_x = chunk_x * 16 + 7;
  const int32_t block_z = chunk_z * 16 + 7;
  // `getFirstOccupiedHeight` is the first air cell; `WgStructureHeights`
  // answers the surface's last solid cell, so the start sits one lower.
  int32_t y = heights.height(block_x, block_z, WgStructureHeightmap::kWorldSurface) - 1;
  y = std::min(y, heights.height(block_x + offset_x, block_z, WgStructureHeightmap::kWorldSurface) -
                      1);
  y = std::min(y, heights.height(block_x, block_z + offset_z, WgStructureHeightmap::kWorldSurface) -
                      1);
  y = std::min(y, heights.height(block_x + offset_x, block_z + offset_z,
                                 WgStructureHeightmap::kWorldSurface) -
                      1);
  if (y < 60)
    return start;
  if (!skip_biome && !wg_structure_start_biome_is_valid(sampler, *structure, block_x, y, block_z))
    return start;

  std::vector<EndCityPart> parts;
  EndCityPart base;
  base.templ = end_city_template("minecraft:end_city/base_floor");
  if (base.templ == nullptr)
    return start;
  base.x = block_x;
  base.y = y;
  base.z = block_z;
  base.rotation = rotation;
  base.overwrite_air = 1;
  base.box = wg_piece_box(*base.templ, rotation, base.x, base.y, base.z);
  parts.push_back(base);
  EndCityBuilder builder{random};
  const int32_t second = builder.add(parts, parts.back(), {-1, 0, -1},
                                     "minecraft:end_city/second_floor_1", rotation, false);
  if (second < 0)
    return start;
  const int32_t third = builder.add(parts, parts[second], {-1, 4, -1},
                                    "minecraft:end_city/third_floor_1", rotation, false);
  if (third < 0)
    return start;
  const int32_t roof = builder.add(parts, parts[third], {-1, 8, -1},
                                   "minecraft:end_city/third_roof", rotation, true);
  if (roof < 0)
    return start;
  builder.recursive(EndCityBuilder::TOWER, 1, parts[roof], parts, {}, false);

  start.valid = true;
  start.structure = structure;
  start.world_seed = seed;
  start.chunk_x = chunk_x;
  start.chunk_z = chunk_z;
  start.rotation = rotation;
  start.piece_count =
      static_cast<uint32_t>(std::min<size_t>(parts.size(), WgJigsawStart::kMaxPieces));
  for (uint32_t i = 0; i < start.piece_count; ++i) {
    start.pieces[i].templ = parts[i].templ;
    start.pieces[i].rotation = parts[i].rotation;
    start.pieces[i].x = parts[i].x;
    start.pieces[i].y = parts[i].y;
    start.pieces[i].z = parts[i].z;
    start.pieces[i].box = parts[i].box;
    start.pieces[i].processor_list = 0xFFFF;
    start.pieces[i].terrain_matching = 0;
    start.pieces[i].overwrite_air = parts[i].overwrite_air;
  }
  return start;
}

WgJigsawStart wg_bastion_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                               WgClimateSampler& sampler, WgStructureHeights& heights,
                               bool skip_biome) {
  // The nether complex set owns the placement gate for both the fortress and
  // the bastion; the member choice is not part of `Structure.generate`, so a
  // direct bastion probe uses the set's frequency gate and the bastion itself.
  const WgStructureSet* set = wg_structure_set_by_name("minecraft:nether_complexes");
  if (set == nullptr || !wg_structure_is_chunk(seed, *set, chunk_x, chunk_z))
    return {};
  const WgStructure* structure = wg_structure_by_name("minecraft:bastion_remnant");
  if (structure == nullptr)
    return {};
  return build_jigsaw_start_for_structure(seed, chunk_x, chunk_z, sampler, heights, *structure,
                                          skip_biome);
}

uint32_t wg_place_jigsaw(Chunk& chunk, const WgJigsawStart& start, WgStructureHeights& heights) {
  if (!start.valid)
    return 0;
  const int32_t base_x = chunk.x * 16;
  const int32_t base_z = chunk.z * 16;
  uint32_t changed = 0;

  // One `StructureProcessor` rule group. Vanilla runs each processor in list
  // order and each `RuleProcessor` stops at its first match; the table's rules
  // are contiguous per processor, so the range is the processor. `loot_table`
  // is an `append_loot` modifier on the matching rule (trail ruins), and its
  // `LootTableSeed` is one `nextLong` from the same per-position random the
  // rule's predicates used.
  const auto apply_rule_range = [&](BlockStateKey state, uint32_t rule_offset, uint16_t rule_count,
                                    int32_t world_x, int32_t world_y, int32_t world_z,
                                    const BlockStateKey& world_state, uint16_t* loot_table,
                                    int64_t* loot_seed) -> BlockStateKey {
    WgLegacyRandom random(wg_mth_get_seed(world_x, world_y, world_z));
    for (uint16_t i = 0; i < rule_count; ++i) {
      const WgPieceProcessorRule& rule = kWgPieceProcessorRules[rule_offset + i];
      bool input_match = false;
      const char* input = jigsaw_string(rule.input);
      if (rule.input_kind == 0 || rule.input_kind == 1) {
        input_match = state.name == input;
        if (input_match && rule.input_kind == 1)
          input_match = random.next_float() < rule.probability;
      } else if (rule.input_kind == 2) {
        // `TagMatchTest`; the generator resolved the tag's members into
        // `kWgPieceProcessorBlocks`.
        for (uint16_t member = 0; member < rule.input_block_count; ++member) {
          if (state.name == kWgPieceProcessorBlocks[rule.input_block_offset + member]) {
            input_match = true;
            break;
          }
        }
      } else {
        // `BlockStateMatchTest` compares the whole state, not just the name.
        // The generated `input` is `name[props]`, the same form
        // `state_text` writes everywhere else.
        const std::string_view text(input);
        const size_t bracket = text.find('[');
        if (bracket == std::string_view::npos) {
          input_match = state.name == text;
        } else {
          input_match = state.name == text.substr(0, bracket) &&
                        state.properties == text.substr(bracket + 1, text.size() - bracket - 2);
        }
      }
      if (!input_match)
        continue;
      if (rule.location_kind == 1 && world_state.name != jigsaw_string(rule.location))
        continue;
      const WgState& output = kWgPieceStates[rule.output_state];
      state.name = output.name;
      state.properties = output.properties;
      if (rule.loot_table != 0xFFFF && loot_table != nullptr) {
        *loot_table = rule.loot_table;
        *loot_seed = random.next_long();
      }
      break;
    }
    return state;
  };

  const auto apply_rules = [&](const WgState& original, uint16_t processor_list, int32_t world_x,
                               int32_t world_y, int32_t world_z,
                               const BlockStateKey& world_state) -> BlockStateKey {
    BlockStateKey state{original.name, original.properties};
    if (processor_list == 0xFFFF ||
        processor_list >= sizeof(kWgPieceProcessorLists) / sizeof(kWgPieceProcessorLists[0]))
      return state;
    const WgPieceProcessorList& list = kWgPieceProcessorLists[processor_list];
    return apply_rule_range(std::move(state), list.rule_offset, list.rule_count, world_x, world_y,
                            world_z, world_state, nullptr, nullptr);
  };

  const auto final_state_for = [&](const WgPieceTemplate& templ, int32_t local_x, int32_t local_y,
                                   int32_t local_z) -> const char* {
    for (uint32_t i = 0; i < templ.jigsaw_count; ++i) {
      const WgPieceJigsaw& jigsaw = kWgPieceJigsaws[templ.jigsaw_offset + i];
      if (jigsaw.x == local_x && jigsaw.y == local_y && jigsaw.z == local_z)
        return kWgJigsawFinalStates[jigsaw.final_state];
    }
    return nullptr;
  };

  const auto place_part = [&](const WgJigsawPiece& piece, const WgPieceTemplate& templ,
                              uint16_t processor_list, uint8_t terrain_matching,
                              bool overwrite_air) {
    if (templ.palette_count == 0)
      return;
    WgLegacyRandom palette_random(wg_mth_get_seed(piece.x, piece.y, piece.z));
    const uint32_t palette_index =
        static_cast<uint32_t>(palette_random.next_int(static_cast<int32_t>(templ.palette_count)));
    const WgPiecePalette& palette = kWgPiecePalettes[templ.palette_offset + palette_index];
    for (uint32_t i = 0; i < templ.block_count; ++i) {
      const WgPieceBlock& block = kWgPieceBlocks[templ.block_offset + i];
      const WgState& palette_state =
          kWgPieceStates[kWgPiecePaletteStates[palette.state_offset + block.state]];
      // The generated block list is the template's full volume, with the cells
      // the NBT omits synthesised as air; vanilla's palette only holds the
      // listed (non-air) blocks, so an air row is not a write. Emitting it
      // replaced the terrain under a plate with air (the outpost's 225/240-row
      // chunk differences were all `minecraft:air`).
      if (!overwrite_air && std::strcmp(palette_state.name, "minecraft:air") == 0)
        continue;
      const XZ offset =
          rotate_pivot(block.x, block.z, piece.rotation, templ.pivot_x, templ.pivot_z);
      const int32_t world_x = piece.x + offset.x;
      const int32_t world_z = piece.z + offset.z;
      const int32_t world_y = piece.y + block.y;
      if (world_x < base_x || world_x >= base_x + 16 || world_z < base_z ||
          world_z >= base_z + 16 || world_y < kMinY || world_y >= kMinY + kHeight)
        continue;
      // `BlockRotProcessor` runs before the rule list and drops the whole
      // block when its per-position draw is above the integrity. It is a
      // per-cell draw (`StructurePlaceSettings.getRandom(pos)`), so a skipped
      // cell does not move a shared stream. `rottable_blocks`, when named, is
      // a gate on the palette state: the ancient-city lists only rot their
      // replaceable deepslate family, not chests or lanterns.
      const WgPieceProcessorList* processors =
          processor_list == 0xFFFF || processor_list >= sizeof(kWgPieceProcessorLists) /
                                                            sizeof(kWgPieceProcessorLists[0])
              ? nullptr
              : &kWgPieceProcessorLists[processor_list];
      const auto in_block_list = [](const char* name, uint32_t offset, uint16_t count) {
        for (uint16_t i = 0; i < count; ++i)
          if (std::strcmp(name, kWgPieceProcessorBlocks[offset + i]) == 0)
            return true;
        return false;
      };
      if (processors != nullptr && processors->block_rot_integrity < 1.0f) {
        const bool rottable = processors->rot_block_count == 0 ||
                              in_block_list(palette_state.name, processors->rot_block_offset,
                                            processors->rot_block_count);
        if (rottable) {
          WgLegacyRandom rot_random(wg_mth_get_seed(world_x, world_y, world_z));
          if (rot_random.next_float() > processors->block_rot_integrity)
            continue;
        }
      }
      // `ProtectedBlockProcessor` keeps whatever the terrain already held when
      // that cell is in the pack's protected set (bedrock and the other
      // `features_cannot_replace` blocks). It reads the world, not the
      // template, so it is checked after the position is clipped and before
      // the write.
      if (processors != nullptr && processors->protected_block_count != 0 &&
          in_block_list(
              chunk.block_state_at(world_x - base_x, world_y, world_z - base_z).name.c_str(),
              processors->protected_block_offset, processors->protected_block_count))
        continue;
      if (std::strcmp(palette_state.name, "minecraft:air") == 0 &&
          std::getenv("PLUSMC_WG_DEBUG") != nullptr)
        std::printf("AIRBLOCK templ=%s local=%d,%d,%d rot=%u pos=%d,%d,%d\n", templ.name, block.x,
                    block.y, block.z, piece.rotation, piece.x, piece.y, piece.z);
      if (std::strcmp(palette_state.name, "minecraft:structure_block") == 0)
        continue;
      // `GravityProcessor` snaps terrain-matching blocks to the world surface
      // with offset -1 (the heightmap answer is one past the top solid cell).
      int32_t final_y = world_y;
      if (terrain_matching != 0)
        final_y =
            heights.height(world_x, world_z, WgStructureHeightmap::kWorldSurface) - 1 + block.y;
      if (std::strcmp(palette_state.name, "minecraft:jigsaw") == 0) {
        const char* final_state = final_state_for(templ, block.x, block.y, block.z);
        if (final_state == nullptr || std::strcmp(final_state, "minecraft:structure_void") == 0)
          continue;
        // `final_state` is `name[props]`; parse the bracket form. Vanilla's
        // jigsaw replacement happens after the processor pass, so the rules
        // do not touch it.
        std::string text(final_state);
        BlockStateKey key{text, ""};
        const size_t bracket = text.find('[');
        if (bracket != std::string::npos && text.back() == ']') {
          key.name = text.substr(0, bracket);
          key.properties = text.substr(bracket + 1, text.size() - bracket - 2);
        }
        key.properties = canonical_state_properties(key.name, key.properties);
        // `StructureTemplate.placeInWorld` rotates the state a processor
        // returns, so a jigsaw's `final_state` is oriented like the template
        // block it replaces.
        key.properties = wg_piece_rotate_properties(key.properties, piece.rotation);
        if (final_y < kMinY || final_y >= kMinY + kHeight)
          continue;
        // A connector's `final_state: minecraft:air` is a no-op placeholder in
        // the template; with a terrain-matching piece the gravity processor has
        // already moved the cell to the surface, and writing air there would
        // punch a hole in the ground the server never produces.
        if (std::strcmp(key.name.c_str(), "minecraft:air") == 0)
          continue;
        if (chunk.set_block_state(world_x - base_x, final_y, world_z - base_z, key))
          ++changed;
        continue;
      }
      // Vanilla reads the world state at the block's unadjusted `pos` for every
      // processor, whatever `GravityProcessor` later does to the y, so the
      // terrain-matching and ordinary pieces read the same cell here; only
      // `final_y` above depends on `terrain_matching`.
      BlockStateKey state =
          apply_rules(palette_state, processor_list, world_x, world_y, world_z,
                      chunk.block_state_at(world_x - base_x, world_y, world_z - base_z));
      state.properties = wg_piece_rotate_properties(state.properties, piece.rotation);
      if (final_y < kMinY || final_y >= kMinY + kHeight)
        continue;
      if (chunk.set_block_state(world_x - base_x, final_y, world_z - base_z, state))
        ++changed;
      // A template chest/barrel carries its own `LootTable` in the NBT. The
      // block entity is written here, where the concrete world position is
      // known, and rolled from the port's positional seed (see
      // `worldgen_loot.h`). Villages had 62 such chests before this path.
      if (block.loot_table != 0xFFFF) {
        if (const char* container = container_id_for_block(state.name))
          add_container_loot(chunk, container, world_x, final_y, world_z,
                             kWgPieceLootTables[block.loot_table], start.world_seed);
      }
    }
  };

  // `CappedProcessor` is a finalize pass: it shuffles the whole piece's
  // surviving block list (not just the cells in this chunk), applies its
  // delegate to at most `limit` of them and only then does `placeInWorld`
  // write the in-chunk cells. Trail ruins are the first applied family with
  // one. The extra pass is kept separate from `place_part` so a family
  // without a capped list keeps the exact per-block path its pins cover.
  const auto parse_state_key = [](const char* text) -> BlockStateKey {
    std::string value(text);
    BlockStateKey key{value, ""};
    const size_t bracket = value.find('[');
    if (bracket != std::string::npos && value.back() == ']') {
      key.name = value.substr(0, bracket);
      key.properties = value.substr(bracket + 1, value.size() - bracket - 2);
    }
    return key;
  };

  const auto place_part_capped = [&](const WgJigsawPiece& piece, const WgPieceTemplate& templ,
                                     uint16_t processor_list, uint8_t terrain_matching,
                                     bool overwrite_air) -> uint32_t {
    if (templ.palette_count == 0)
      return 0;
    const WgPieceProcessorList* processors =
        processor_list == 0xFFFF ||
                processor_list >= sizeof(kWgPieceProcessorLists) / sizeof(kWgPieceProcessorLists[0])
            ? nullptr
            : &kWgPieceProcessorLists[processor_list];
    if (processors == nullptr || processors->capped_count == 0)
      return 0;
    WgLegacyRandom palette_random(wg_mth_get_seed(piece.x, piece.y, piece.z));
    const uint32_t palette_index =
        static_cast<uint32_t>(palette_random.next_int(static_cast<int32_t>(templ.palette_count)));
    const WgPiecePalette& palette = kWgPiecePalettes[templ.palette_offset + palette_index];

    struct Entry {
      int32_t x, y, z;
      int16_t local_y;
      BlockStateKey state;
      uint16_t loot_table;
      int64_t loot_seed;
    };
    std::vector<Entry> entries;
    entries.reserve(templ.block_count);
    const auto in_block_list = [](const char* name, uint32_t offset, uint16_t count) {
      for (uint16_t i = 0; i < count; ++i)
        if (std::strcmp(name, kWgPieceProcessorBlocks[offset + i]) == 0)
          return true;
      return false;
    };
    for (uint32_t i = 0; i < templ.block_count; ++i) {
      const WgPieceBlock& block = kWgPieceBlocks[templ.block_offset + i];
      BlockStateKey state{
          kWgPieceStates[kWgPiecePaletteStates[palette.state_offset + block.state]].name,
          kWgPieceStates[kWgPiecePaletteStates[palette.state_offset + block.state]].properties};
      if (!overwrite_air && std::strcmp(state.name.c_str(), "minecraft:air") == 0)
        continue;
      // `BlockIgnoreProcessor.STRUCTURE_BLOCK` runs first.
      if (std::strcmp(state.name.c_str(), "minecraft:structure_block") == 0)
        continue;
      // `JigsawReplacementProcessor` runs before the element's own processors.
      if (std::strcmp(state.name.c_str(), "minecraft:jigsaw") == 0) {
        const char* final_state = final_state_for(templ, block.x, block.y, block.z);
        if (final_state == nullptr)
          continue;
        state = parse_state_key(final_state);
        if (state.name == "minecraft:structure_void")
          continue;
      }
      const XZ offset =
          rotate_pivot(block.x, block.z, piece.rotation, templ.pivot_x, templ.pivot_z);
      const int32_t world_x = piece.x + offset.x;
      const int32_t world_z = piece.z + offset.z;
      const int32_t world_y = piece.y + block.y;
      if (world_y < kMinY || world_y >= kMinY + kHeight)
        continue;
      const bool in_chunk =
          world_x >= base_x && world_x < base_x + 16 && world_z >= base_z && world_z < base_z + 16;
      const BlockStateKey world_state =
          in_chunk ? chunk.block_state_at(world_x - base_x, world_y, world_z - base_z)
                   : BlockStateKey{"minecraft:air", ""};
      if (processors->block_rot_integrity < 1.0f &&
          (processors->rot_block_count == 0 ||
           in_block_list(state.name.c_str(), processors->rot_block_offset,
                         processors->rot_block_count))) {
        WgLegacyRandom rot_random(wg_mth_get_seed(world_x, world_y, world_z));
        if (rot_random.next_float() > processors->block_rot_integrity)
          continue;
      }
      if (processors->protected_block_count != 0 && in_chunk &&
          in_block_list(world_state.name.c_str(), processors->protected_block_offset,
                        processors->protected_block_count))
        continue;
      uint16_t loot_table = 0xFFFF;
      int64_t loot_seed = 0;
      state = apply_rule_range(std::move(state), processors->rule_offset, processors->rule_count,
                               world_x, world_y, world_z, world_state, &loot_table, &loot_seed);
      entries.push_back(
          Entry{world_x, world_y, world_z, block.y, std::move(state), loot_table, loot_seed});
    }
    if (entries.empty())
      return 0;

    // `CappedProcessor.finalizeProcessing`: a fresh positional stream per pass,
    // the same shuffle `Util.toShuffledList(IntStream.range(...))` performs, and
    // the delegate's per-position rule random for each tried cell.
    WgLegacyRandom factory_source(start.world_seed);
    const int64_t positional_factory_seed = factory_source.next_long();
    std::vector<uint32_t> order(entries.size());
    for (uint32_t pass = 0; pass < processors->capped_count; ++pass) {
      const WgPieceCappedPass& capped = kWgPieceCappedPasses[processors->capped_offset + pass];
      WgLegacyRandom capped_random(wg_mth_get_seed(piece.x, piece.y, piece.z) ^
                                   positional_factory_seed);
      const int32_t limit =
          capped.limit_max == capped.limit_min
              ? static_cast<int32_t>(capped.limit_min)
              : capped.limit_min +
                    capped_random.next_int(static_cast<int32_t>(capped.limit_max) -
                                           static_cast<int32_t>(capped.limit_min) + 1);
      if (limit <= 0)
        continue;
      for (uint32_t i = 0; i < order.size(); ++i)
        order[i] = i;
      for (size_t i = order.size(); i > 1; --i) {
        const size_t swap = static_cast<size_t>(capped_random.next_int(static_cast<int32_t>(i)));
        std::swap(order[i - 1], order[swap]);
      }
      int32_t replaced = 0;
      for (uint32_t index : order) {
        if (replaced >= limit)
          break;
        Entry& entry = entries[index];
        const bool in_chunk = entry.x >= base_x && entry.x < base_x + 16 && entry.z >= base_z &&
                              entry.z < base_z + 16;
        const BlockStateKey world_state =
            in_chunk ? chunk.block_state_at(entry.x - base_x, entry.y, entry.z - base_z)
                     : BlockStateKey{"minecraft:air", ""};
        uint16_t loot_table = 0xFFFF;
        int64_t loot_seed = 0;
        BlockStateKey updated =
            apply_rule_range(entry.state, capped.rule_offset, capped.rule_count, entry.x, entry.y,
                             entry.z, world_state, &loot_table, &loot_seed);
        if (updated.name == entry.state.name && updated.properties == entry.state.properties)
          continue;
        entry.state = std::move(updated);
        if (loot_table != 0xFFFF) {
          entry.loot_table = loot_table;
          entry.loot_seed = loot_seed;
        }
        ++replaced;
      }
    }

    uint32_t written = 0;
    for (const Entry& entry : entries) {
      if (entry.x < base_x || entry.x >= base_x + 16 || entry.z < base_z || entry.z >= base_z + 16)
        continue;
      int32_t final_y = entry.y;
      if (terrain_matching != 0)
        final_y = heights.height(entry.x, entry.z, WgStructureHeightmap::kWorldSurface) - 1 +
                  entry.local_y;
      if (final_y < kMinY || final_y >= kMinY + kHeight)
        continue;
      BlockStateKey state = entry.state;
      state.properties = wg_piece_rotate_properties(state.properties, piece.rotation);
      if (chunk.set_block_state(entry.x - base_x, final_y, entry.z - base_z, state))
        ++written;
      if (entry.loot_table != 0xFFFF) {
        const char* table = kWgPieceLootTables[entry.loot_table];
        if (state.name == "minecraft:suspicious_gravel" ||
            state.name == "minecraft:suspicious_sand")
          add_brushable_loot(chunk, entry.x, final_y, entry.z, table, entry.loot_seed);
        else if (const char* container = container_id_for_block(state.name))
          add_container_loot_with_seed(chunk, container, entry.x, final_y, entry.z, table,
                                       entry.loot_seed);
      }
    }
    return written;
  };

  const auto part_has_capped = [](uint16_t processor_list) {
    return processor_list != 0xFFFF &&
           processor_list < sizeof(kWgPieceProcessorLists) / sizeof(kWgPieceProcessorLists[0]) &&
           kWgPieceProcessorLists[processor_list].capped_count != 0;
  };

  for (uint32_t piece_index = 0; piece_index < start.piece_count; ++piece_index) {
    const WgJigsawPiece& piece = start.pieces[piece_index];
    if (piece.element != nullptr && piece.element->kind == 4) {
      // `FeaturePoolElement.place`: run the element's placed feature at the
      // piece cell. The stream is this port's positional convention, like every
      // other piece's palette/processor draw here; writes outside the chunk are
      // clipped by the placed-feature path, which is what the server's
      // per-chunk `WorldGenRegion` does.
      const char* feature = pool_element_feature(*piece.element);
      if (feature != nullptr) {
        WgRandom feature_random(wg_mth_get_seed(piece.x, piece.y, piece.z));
        changed += wg_place_placed_feature_at(chunk, feature, feature_random, start.world_seed,
                                              piece.x, piece.y, piece.z);
      }
    } else if (piece.element != nullptr && piece.element->kind == 3) {
      for (uint16_t part_index = 0; part_index < piece.element->part_count; ++part_index) {
        const WgPoolElementPart* part = element_part(*piece.element, part_index);
        const WgPieceTemplate* templ = part != nullptr ? part_template(*part) : nullptr;
        if (templ == nullptr)
          continue;
        if (part_has_capped(part->processor_list))
          changed += place_part_capped(piece, *templ, part->processor_list, piece.terrain_matching,
                                       piece.overwrite_air != 0);
        else
          place_part(piece, *templ, part->processor_list, piece.terrain_matching,
                     piece.overwrite_air != 0);
      }
    } else if (piece.templ != nullptr) {
      if (part_has_capped(piece.processor_list))
        changed += place_part_capped(piece, *piece.templ, piece.processor_list,
                                     piece.terrain_matching, piece.overwrite_air != 0);
      else
        place_part(piece, *piece.templ, piece.processor_list, piece.terrain_matching,
                   piece.overwrite_air != 0);
    }
  }
  return changed;
}

// ---------------------------------------------------------------------------
// `minecraft:fortress` (`NetherFortressPieces`). The pieces are hand-coded, so
// this is a direct port of the assembly and of each `postProcess`, not a table.
// ---------------------------------------------------------------------------
namespace {

struct FortressWeight {
  WgFortressPieceType type;
  int32_t weight;
  int32_t max_place;
  bool allow_in_row;
  int32_t place_count;
};

WgPieceBox fortress_orient_box(int32_t foot_x, int32_t foot_y, int32_t foot_z, int32_t off_x,
                               int32_t off_y, int32_t off_z, int32_t width, int32_t height,
                               int32_t depth, uint8_t direction) {
  switch (direction) {
  case 0: // north
    return WgPieceBox{foot_x + off_x,
                      foot_y + off_y,
                      foot_z - depth + 1 + off_z,
                      foot_x + width - 1 + off_x,
                      foot_y + height - 1 + off_y,
                      foot_z + off_z};
  case 3: // west
    return WgPieceBox{foot_x - depth + 1 + off_z,
                      foot_y + off_y,
                      foot_z + off_x,
                      foot_x + off_z,
                      foot_y + height - 1 + off_y,
                      foot_z + width - 1 + off_x};
  case 1: // east
    return WgPieceBox{foot_x + off_z,
                      foot_y + off_y,
                      foot_z + off_x,
                      foot_x + depth - 1 + off_z,
                      foot_y + height - 1 + off_y,
                      foot_z + width - 1 + off_x};
  default: // south
    return WgPieceBox{foot_x + off_x,
                      foot_y + off_y,
                      foot_z + off_z,
                      foot_x + width - 1 + off_x,
                      foot_y + height - 1 + off_y,
                      foot_z + depth - 1 + off_z};
  }
}

bool fortress_intersects(const WgPieceBox& a, const WgPieceBox& b) {
  return a.max_x >= b.min_x && a.min_x <= b.max_x && a.max_z >= b.min_z && a.min_z <= b.max_z &&
         a.max_y >= b.min_y && a.min_y <= b.max_y;
}

const WgFortressPiece* fortress_collision(const std::vector<WgFortressPiece>& pieces,
                                          const WgPieceBox& box) {
  for (const WgFortressPiece& piece : pieces)
    if (fortress_intersects(piece.box, box))
      return &piece;
  return nullptr;
}

bool fortress_create(WgFortressPieceType type, WgLegacyRandom* random, int32_t foot_x,
                     int32_t foot_y, int32_t foot_z, uint8_t direction, uint8_t depth,
                     const std::vector<WgFortressPiece>& pieces, WgFortressPiece* out) {
  int32_t off_x = 0, off_y = 0, off_z = 0, width = 0, height = 0, depth_box = 0;
  switch (type) {
  case WgFortressPieceType::kBridgeStraight:
    off_x = -1;
    off_y = -3;
    off_z = 0;
    width = 5;
    height = 10;
    depth_box = 19;
    break;
  case WgFortressPieceType::kBridgeCrossing:
    off_x = -8;
    off_y = -3;
    off_z = 0;
    width = 19;
    height = 10;
    depth_box = 19;
    break;
  case WgFortressPieceType::kRoomCrossing:
    off_x = -2;
    off_y = 0;
    off_z = 0;
    width = 7;
    height = 9;
    depth_box = 7;
    break;
  case WgFortressPieceType::kStairsRoom:
    off_x = -2;
    off_y = 0;
    off_z = 0;
    width = 7;
    height = 11;
    depth_box = 7;
    break;
  case WgFortressPieceType::kMonsterThrone:
    off_x = -2;
    off_y = 0;
    off_z = 0;
    width = 7;
    height = 8;
    depth_box = 9;
    break;
  case WgFortressPieceType::kCastleEntrance:
    off_x = -5;
    off_y = -3;
    off_z = 0;
    width = 13;
    height = 14;
    depth_box = 13;
    break;
  case WgFortressPieceType::kCastleSmallCorridor:
  case WgFortressPieceType::kCastleSmallCorridorCrossing:
  case WgFortressPieceType::kCastleSmallCorridorRightTurn:
  case WgFortressPieceType::kCastleSmallCorridorLeftTurn:
    off_x = -1;
    off_y = 0;
    off_z = 0;
    width = 5;
    height = 7;
    depth_box = 5;
    break;
  case WgFortressPieceType::kCastleCorridorStairs:
    off_x = -1;
    off_y = -7;
    off_z = 0;
    width = 5;
    height = 14;
    depth_box = 10;
    break;
  case WgFortressPieceType::kCastleCorridorTBalcony:
    off_x = -3;
    off_y = 0;
    off_z = 0;
    width = 9;
    height = 7;
    depth_box = 9;
    break;
  case WgFortressPieceType::kCastleStalkRoom:
    off_x = -5;
    off_y = -3;
    off_z = 0;
    width = 13;
    height = 14;
    depth_box = 13;
    break;
  case WgFortressPieceType::kBridgeEndFiller:
    off_x = -1;
    off_y = -3;
    off_z = 0;
    width = 5;
    height = 10;
    depth_box = 8;
    break;
  }
  const WgPieceBox box = fortress_orient_box(foot_x, foot_y, foot_z, off_x, off_y, off_z, width,
                                             height, depth_box, direction);
  if (box.min_y <= 10 || fortress_collision(pieces, box) != nullptr)
    return false;
  out->type = type;
  out->direction = direction;
  out->depth = depth;
  out->box = box;
  out->needing_chest = 0;
  out->self_seed = 0;
  if (type == WgFortressPieceType::kCastleSmallCorridorRightTurn ||
      type == WgFortressPieceType::kCastleSmallCorridorLeftTurn)
    out->needing_chest = random->next_int(3) == 0 ? 1 : 0;
  else if (type == WgFortressPieceType::kBridgeEndFiller)
    out->self_seed = random->next_int();
  return true;
}

int32_t fortress_update_weight(const std::vector<FortressWeight>& weights) {
  bool has_any = false;
  int32_t total = 0;
  for (const FortressWeight& weight : weights) {
    if (weight.max_place > 0 && weight.place_count < weight.max_place)
      has_any = true;
    total += weight.weight;
  }
  return has_any ? total : -1;
}

bool fortress_do_place(const FortressWeight& weight, uint8_t depth) {
  (void)depth;
  return weight.max_place == 0 || weight.place_count < weight.max_place;
}

bool fortress_is_valid(const FortressWeight& weight) {
  return weight.max_place == 0 || weight.place_count < weight.max_place;
}

WgFortressPieceType fortress_factory_type(const FortressWeight& weight) {
  return weight.type;
}

bool fortress_generate_piece(WgFortressPieceType* previous_type,
                             std::vector<FortressWeight>& weights, WgLegacyRandom& random,
                             int32_t foot_x, int32_t foot_y, int32_t foot_z, uint8_t direction,
                             uint8_t depth, std::vector<WgFortressPiece>& pieces,
                             WgFortressPiece* out) {
  const int32_t total = fortress_update_weight(weights);
  if (total > 0 && depth <= 30) {
    for (int32_t attempt = 0; attempt < 5; ++attempt) {
      int32_t choice = random.next_int(total);
      for (size_t i = 0; i < weights.size(); ++i) {
        FortressWeight& weight = weights[i];
        choice -= weight.weight;
        if (choice >= 0)
          continue;
        if (!fortress_do_place(weight, depth) ||
            (weight.type == *previous_type && !weight.allow_in_row))
          break; // `continue block0`: the next attempt.
        if (!fortress_create(fortress_factory_type(weight), &random, foot_x, foot_y, foot_z,
                             direction, depth, pieces, out))
          continue;
        ++weight.place_count;
        *previous_type = weight.type;
        if (!fortress_is_valid(weight))
          weights.erase(weights.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
      }
    }
  }
  // Exhausted weights (or past depth 30): vanilla still creates a filler and
  // the caller queues it when the box fits.
  return fortress_create(WgFortressPieceType::kBridgeEndFiller, &random, foot_x, foot_y, foot_z,
                         direction, depth, pieces, out);
}

bool fortress_generate_and_add(WgFortressPieceType* previous_type,
                               std::vector<FortressWeight>& weights, WgLegacyRandom& random,
                               int32_t foot_x, int32_t foot_y, int32_t foot_z, uint8_t direction,
                               uint8_t depth, const WgPieceBox& start_box,
                               std::vector<WgFortressPiece>& pieces,
                               std::vector<uint32_t>& pending) {
  if (std::abs(foot_x - start_box.min_x) > 112 || std::abs(foot_z - start_box.min_z) > 112) {
    WgFortressPiece filler;
    return fortress_create(WgFortressPieceType::kBridgeEndFiller, &random, foot_x, foot_y, foot_z,
                           direction, depth, pieces, &filler);
  }
  WgFortressPiece piece;
  if (!fortress_generate_piece(previous_type, weights, random, foot_x, foot_y, foot_z, direction,
                               static_cast<uint8_t>(depth + 1), pieces, &piece))
    return false;
  pieces.push_back(piece);
  pending.push_back(static_cast<uint32_t>(pieces.size() - 1));
  return true;
}

} // namespace

WgFortressStart wg_fortress_start(int64_t seed, int32_t chunk_x, int32_t chunk_z) {
  WgFortressStart start;
  const WgStructure* fortress = wg_structure_by_name("minecraft:fortress");
  if (fortress == nullptr)
    return start;
  start.structure = fortress;
  start.world_seed = seed;
  start.chunk_x = chunk_x;
  start.chunk_z = chunk_z;

  WgLegacyRandom random(0);
  seed_large_feature(random, seed, chunk_x, chunk_z);
  static constexpr uint8_t kHorizontal[4] = {0, 1, 2, 3}; // N, E, S, W
  const uint8_t direction = kHorizontal[random.next_int(4)];
  const int32_t west = chunk_x * 16 + 2;
  const int32_t north = chunk_z * 16 + 2;
  WgFortressPiece first;
  first.type = WgFortressPieceType::kBridgeCrossing;
  first.direction = direction;
  first.depth = 0;
  first.box = WgPieceBox{west, 64, north, west + 18, 73, north + 18};
  start.pieces[0] = first;
  start.piece_count = 1;

  std::vector<FortressWeight> bridge = {
      {WgFortressPieceType::kBridgeStraight, 30, 0, true, 0},
      {WgFortressPieceType::kBridgeCrossing, 10, 4, false, 0},
      {WgFortressPieceType::kRoomCrossing, 10, 4, false, 0},
      {WgFortressPieceType::kStairsRoom, 10, 3, false, 0},
      {WgFortressPieceType::kMonsterThrone, 5, 2, false, 0},
      {WgFortressPieceType::kCastleEntrance, 5, 1, false, 0},
  };
  std::vector<FortressWeight> castle = {
      {WgFortressPieceType::kCastleSmallCorridor, 25, 0, true, 0},
      {WgFortressPieceType::kCastleSmallCorridorCrossing, 15, 5, false, 0},
      {WgFortressPieceType::kCastleSmallCorridorRightTurn, 5, 10, false, 0},
      {WgFortressPieceType::kCastleSmallCorridorLeftTurn, 5, 10, false, 0},
      {WgFortressPieceType::kCastleCorridorStairs, 10, 3, true, 0},
      {WgFortressPieceType::kCastleCorridorTBalcony, 7, 2, false, 0},
      {WgFortressPieceType::kCastleStalkRoom, 5, 2, false, 0},
  };
  // `StartPiece.previousPiece` starts null: a bridge crossing may be the first
  // child even though the start piece is one. No weighted piece uses the
  // filler type, so it is a safe "no previous piece" sentinel.
  WgFortressPieceType previous = WgFortressPieceType::kBridgeEndFiller;
  std::vector<uint32_t> pending;
  std::vector<WgFortressPiece> pieces(start.pieces, start.pieces + 1);
  const WgPieceBox start_box = first.box;
  const auto child_forward = [&](const WgFortressPiece& p, std::vector<FortressWeight>& weights,
                                 int32_t x_off, int32_t y_off) {
    const int32_t x = p.box.min_x, y = p.box.min_y + y_off, z = p.box.min_z;
    switch (p.direction) {
    case 0:
      return fortress_generate_and_add(&previous, weights, random, x + x_off, y, z - 1, 0, p.depth,
                                       start_box, pieces, pending);
    case 2:
      return fortress_generate_and_add(&previous, weights, random, x + x_off, y, p.box.max_z + 1, 2,
                                       p.depth, start_box, pieces, pending);
    case 3:
      return fortress_generate_and_add(&previous, weights, random, x - 1, y, z + x_off, 3, p.depth,
                                       start_box, pieces, pending);
    default:
      return fortress_generate_and_add(&previous, weights, random, p.box.max_x + 1, y, z + x_off, 1,
                                       p.depth, start_box, pieces, pending);
    }
  };
  const auto child_left = [&](const WgFortressPiece& p, std::vector<FortressWeight>& weights,
                              int32_t y_off, int32_t z_off) {
    switch (p.direction) {
    case 0:
      return fortress_generate_and_add(&previous, weights, random, p.box.min_x - 1,
                                       p.box.min_y + y_off, p.box.min_z + z_off, 3, p.depth,
                                       start_box, pieces, pending);
    case 2:
      return fortress_generate_and_add(&previous, weights, random, p.box.min_x - 1,
                                       p.box.min_y + y_off, p.box.min_z + z_off, 3, p.depth,
                                       start_box, pieces, pending);
    case 3:
      return fortress_generate_and_add(&previous, weights, random, p.box.min_x + z_off,
                                       p.box.min_y + y_off, p.box.min_z - 1, 0, p.depth, start_box,
                                       pieces, pending);
    default:
      return fortress_generate_and_add(&previous, weights, random, p.box.min_x + z_off,
                                       p.box.min_y + y_off, p.box.min_z - 1, 0, p.depth, start_box,
                                       pieces, pending);
    }
  };
  const auto child_right = [&](const WgFortressPiece& p, std::vector<FortressWeight>& weights,
                               int32_t y_off, int32_t z_off) {
    switch (p.direction) {
    case 0:
      return fortress_generate_and_add(&previous, weights, random, p.box.max_x + 1,
                                       p.box.min_y + y_off, p.box.min_z + z_off, 1, p.depth,
                                       start_box, pieces, pending);
    case 2:
      return fortress_generate_and_add(&previous, weights, random, p.box.max_x + 1,
                                       p.box.min_y + y_off, p.box.min_z + z_off, 1, p.depth,
                                       start_box, pieces, pending);
    case 3:
      return fortress_generate_and_add(&previous, weights, random, p.box.min_x + z_off,
                                       p.box.min_y + y_off, p.box.max_z + 1, 2, p.depth, start_box,
                                       pieces, pending);
    default:
      return fortress_generate_and_add(&previous, weights, random, p.box.min_x + z_off,
                                       p.box.min_y + y_off, p.box.max_z + 1, 2, p.depth, start_box,
                                       pieces, pending);
    }
  };
  const auto add_children = [&](uint32_t index) {
    const WgFortressPiece piece = pieces[index];
    switch (piece.type) {
    case WgFortressPieceType::kBridgeStraight:
      child_forward(piece, bridge, 1, 3);
      break;
    case WgFortressPieceType::kBridgeCrossing:
      child_forward(piece, bridge, 8, 3);
      child_left(piece, bridge, 3, 8);
      child_right(piece, bridge, 3, 8);
      break;
    case WgFortressPieceType::kRoomCrossing:
      child_forward(piece, bridge, 2, 0);
      child_left(piece, bridge, 0, 2);
      child_right(piece, bridge, 0, 2);
      break;
    case WgFortressPieceType::kStairsRoom:
      child_right(piece, bridge, 6, 2);
      break;
    case WgFortressPieceType::kMonsterThrone:
      break;
    case WgFortressPieceType::kCastleEntrance:
      child_forward(piece, castle, 5, 3);
      break;
    case WgFortressPieceType::kCastleSmallCorridor:
      child_forward(piece, castle, 1, 0);
      break;
    case WgFortressPieceType::kCastleSmallCorridorCrossing:
      child_forward(piece, castle, 1, 0);
      child_left(piece, castle, 0, 1);
      child_right(piece, castle, 0, 1);
      break;
    case WgFortressPieceType::kCastleSmallCorridorRightTurn:
      child_right(piece, castle, 0, 1);
      break;
    case WgFortressPieceType::kCastleSmallCorridorLeftTurn:
      child_left(piece, castle, 0, 1);
      break;
    case WgFortressPieceType::kCastleCorridorStairs:
      child_forward(piece, castle, 1, 0);
      break;
    case WgFortressPieceType::kCastleCorridorTBalcony: {
      const int32_t z_off = piece.direction == 3 || piece.direction == 0 ? 5 : 1;
      // Each side independently chooses a castle or a bridge branch
      // (`random.nextInt(8) > 0`); the left draw is consumed before the left
      // child generates, the right draw after it.
      child_left(piece, random.next_int(8) > 0 ? castle : bridge, 0, z_off);
      child_right(piece, random.next_int(8) > 0 ? castle : bridge, 0, z_off);
      break;
    }
    case WgFortressPieceType::kCastleStalkRoom:
      child_forward(piece, castle, 5, 3);
      child_forward(piece, castle, 5, 11);
      break;
    case WgFortressPieceType::kBridgeEndFiller:
      break;
    }
  };
  add_children(0);
  while (!pending.empty()) {
    const size_t pick = static_cast<size_t>(random.next_int(static_cast<int32_t>(pending.size())));
    const uint32_t index = pending[pick];
    pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(pick));
    add_children(index);
  }

  WgPieceBox union_box = pieces[0].box;
  for (const WgFortressPiece& piece : pieces) {
    union_box.min_x = std::min(union_box.min_x, piece.box.min_x);
    union_box.min_y = std::min(union_box.min_y, piece.box.min_y);
    union_box.min_z = std::min(union_box.min_z, piece.box.min_z);
    union_box.max_x = std::max(union_box.max_x, piece.box.max_x);
    union_box.max_y = std::max(union_box.max_y, piece.box.max_y);
    union_box.max_z = std::max(union_box.max_z, piece.box.max_z);
  }
  const int32_t span = 70 - 48 + 1 - (union_box.max_y - union_box.min_y + 1);
  const int32_t target_y = span > 1 ? 48 + random.next_int(span) : 48;
  const int32_t dy = target_y - union_box.min_y;
  for (WgFortressPiece& piece : pieces) {
    piece.box.min_y += dy;
    piece.box.max_y += dy;
  }
  start.piece_count = 0;
  for (const WgFortressPiece& piece : pieces) {
    if (start.piece_count >= WgFortressStart::kMaxPieces)
      break;
    start.pieces[start.piece_count++] = piece;
  }
  start.valid = start.piece_count > 0;
  return start;
}

WgNetherComplexKind wg_nether_complex_kind(int64_t seed, int32_t chunk_x, int32_t chunk_z) {
  const WgStructureSet* set = wg_structure_set_by_name("minecraft:nether_complexes");
  WgLegacyRandom random(0);
  seed_large_feature(random, seed, chunk_x, chunk_z);
  int32_t total = 0;
  if (set != nullptr) {
    for (uint32_t i = 0; i < set->entry_count; ++i)
      total += static_cast<int32_t>(kWgStructureSetEntries[set->entry_offset + i].weight);
  }
  if (total <= 0)
    return WgNetherComplexKind::kFortress;
  int32_t choice = random.next_int(total);
  if (set != nullptr) {
    for (uint32_t i = 0; i < set->entry_count; ++i) {
      const WgStructureSetEntry& entry = kWgStructureSetEntries[set->entry_offset + i];
      choice -= static_cast<int32_t>(entry.weight);
      if (choice < 0)
        return std::strcmp(entry.structure, "minecraft:fortress") == 0
                   ? WgNetherComplexKind::kFortress
                   : WgNetherComplexKind::kBastion;
    }
  }
  return WgNetherComplexKind::kFortress;
}

namespace {

void fortress_add_spawner(Chunk& chunk, int32_t world_x, int32_t world_y, int32_t world_z) {
  for (const nbt::Value& be : chunk.block_entities)
    if (block_entity_matches(be, world_x, world_y, world_z))
      return;
  using namespace nbt;
  nbt::Value entity = make_compound(
      std::vector<std::pair<std::string, Value>>{{"id", make_string("minecraft:blaze")}});
  nbt::Value spawn_data =
      make_compound(std::vector<std::pair<std::string, Value>>{{"entity", std::move(entity)}});
  chunk.block_entities.push_back(make_compound(
      std::vector<std::pair<std::string, Value>>{{"id", make_string("minecraft:spawner")},
                                                 {"x", make_int(world_x)},
                                                 {"y", make_int(world_y)},
                                                 {"z", make_int(world_z)},
                                                 {"SpawnData", std::move(spawn_data)}}));
}

class FortressWriter {
public:
  FortressWriter(Chunk& chunk_in, const WgFortressPiece& piece_in, int64_t world_seed_in)
      : chunk(chunk_in), piece(piece_in), world_seed(world_seed_in), base_x(chunk_in.x * 16),
        base_z(chunk_in.z * 16) {}

  uint32_t run() {
    switch (piece.type) {
    case WgFortressPieceType::kBridgeStraight:
      bridge_straight();
      break;
    case WgFortressPieceType::kBridgeCrossing:
      bridge_crossing();
      break;
    case WgFortressPieceType::kRoomCrossing:
      room_crossing();
      break;
    case WgFortressPieceType::kStairsRoom:
      stairs_room();
      break;
    case WgFortressPieceType::kMonsterThrone:
      monster_throne();
      break;
    case WgFortressPieceType::kCastleEntrance:
      castle_entrance();
      break;
    case WgFortressPieceType::kCastleSmallCorridor:
      castle_small_corridor();
      break;
    case WgFortressPieceType::kCastleSmallCorridorCrossing:
      castle_small_crossing();
      break;
    case WgFortressPieceType::kCastleSmallCorridorRightTurn:
      castle_turn(true);
      break;
    case WgFortressPieceType::kCastleSmallCorridorLeftTurn:
      castle_turn(false);
      break;
    case WgFortressPieceType::kCastleCorridorStairs:
      castle_corridor_stairs();
      break;
    case WgFortressPieceType::kCastleCorridorTBalcony:
      castle_t_balcony();
      break;
    case WgFortressPieceType::kCastleStalkRoom:
      castle_stalk_room();
      break;
    case WgFortressPieceType::kBridgeEndFiller:
      bridge_end_filler();
      break;
    }
    return changed;
  }

private:
  static BlockStateKey air() {
    return BlockStateKey{"minecraft:air", ""};
  }
  static BlockStateKey bricks() {
    return BlockStateKey{"minecraft:nether_bricks", ""};
  }
  static BlockStateKey soul_sand() {
    return BlockStateKey{"minecraft:soul_sand", ""};
  }
  static BlockStateKey lava() {
    return BlockStateKey{"minecraft:lava", "level=0"};
  }
  static BlockStateKey wart() {
    return BlockStateKey{"minecraft:nether_wart", "age=0"};
  }
  static BlockStateKey fence(bool north, bool south, bool east, bool west) {
    std::string p = "east=";
    p += east ? "true" : "false";
    p += ",north=";
    p += north ? "true" : "false";
    p += ",south=";
    p += south ? "true" : "false";
    p += ",waterlogged=false,west=";
    p += west ? "true" : "false";
    return BlockStateKey{"minecraft:nether_brick_fence", std::move(p)};
  }
  static BlockStateKey stairs(const char* facing) {
    return BlockStateKey{"minecraft:nether_brick_stairs",
                         std::string("facing=") + facing +
                             ",half=bottom,shape=straight,waterlogged=false"};
  }

  int32_t world_x(int32_t x, int32_t z) const {
    switch (piece.direction) {
    case 0:
    case 2:
      return piece.box.min_x + x;
    case 3:
      return piece.box.max_x - z;
    default:
      return piece.box.min_x + z;
    }
  }
  int32_t world_z(int32_t x, int32_t z) const {
    switch (piece.direction) {
    case 0:
      return piece.box.max_z - z;
    case 2:
      return piece.box.min_z + z;
    case 3:
    case 1:
      return piece.box.min_z + x;
    default:
      return z;
    }
  }
  int32_t world_y(int32_t y) const {
    return piece.box.min_y + y;
  }

  bool inside(int32_t x, int32_t z) const {
    return x >= base_x && x < base_x + 16 && z >= base_z && z < base_z + 16;
  }

  BlockStateKey transformed(BlockStateKey state) const {
    if (orientation_mirrors(piece.direction))
      state.properties = wg_piece_mirror_properties(state.properties, 1);
    state.properties =
        wg_piece_rotate_properties(state.properties, orientation_rotation(piece.direction));
    return state;
  }

  void place(const BlockStateKey& state, int32_t x, int32_t y, int32_t z) {
    const int32_t wx = world_x(x, z), wy = world_y(y), wz = world_z(x, z);
    if (!inside(wx, wz) || wy < kMinY || wy >= kMinY + kHeight)
      return;
    if (chunk.set_block_state(wx - base_x, wy, wz - base_z, transformed(state)))
      ++changed;
  }

  BlockStateKey get_block(int32_t x, int32_t y, int32_t z) const {
    const int32_t wx = world_x(x, z), wy = world_y(y), wz = world_z(x, z);
    if (!inside(wx, wz) || wy < kMinY || wy >= kMinY + kHeight)
      return air();
    return chunk.block_state_at(wx - base_x, wy, wz - base_z);
  }

  void generate_box(int32_t x0, int32_t y0, int32_t z0, int32_t x1, int32_t y1, int32_t z1,
                    const BlockStateKey& edge, const BlockStateKey& fill, bool skip_air = false) {
    for (int32_t y = y0; y <= y1; ++y)
      for (int32_t x = x0; x <= x1; ++x)
        for (int32_t z = z0; z <= z1; ++z) {
          if (skip_air && is_air_state(get_block(x, y, z)))
            continue;
          const bool on_edge = y == y0 || y == y1 || x == x0 || x == x1 || z == z0 || z == z1;
          place(on_edge ? edge : fill, x, y, z);
        }
  }

  void fill_column_down(int32_t x, int32_t start_y, int32_t z) {
    const int32_t wx = world_x(x, z), wz = world_z(x, z);
    if (!inside(wx, wz))
      return;
    int32_t wy = world_y(start_y);
    while (wy > kMinY + 1 &&
           is_replaceable_by_structures(chunk.block_state_at(wx - base_x, wy, wz - base_z))) {
      if (chunk.set_block_state(wx - base_x, wy, wz - base_z, bricks()))
        ++changed;
      --wy;
    }
  }

  void chest(int32_t x, int32_t y, int32_t z) {
    const int32_t wx = world_x(x, z), wy = world_y(y), wz = world_z(x, z);
    if (!inside(wx, wz) || wy < kMinY || wy >= kMinY + kHeight)
      return;
    if (chunk.block_state_at(wx - base_x, wy, wz - base_z).name == "minecraft:chest")
      return;
    const char* facing = reoriented_chest_facing(chunk, wx - base_x, wy, wz - base_z);
    const BlockStateKey state{"minecraft:chest",
                              std::string("facing=") + facing + ",type=single,waterlogged=false"};
    if (chunk.set_block_state(wx - base_x, wy, wz - base_z, state))
      ++changed;
    add_chest_loot(chunk, wx, wy, wz, "minecraft:chests/nether_bridge", world_seed);
  }

  void spawner(int32_t x, int32_t y, int32_t z) {
    const int32_t wx = world_x(x, z), wy = world_y(y), wz = world_z(x, z);
    if (!inside(wx, wz) || wy < kMinY || wy >= kMinY + kHeight)
      return;
    if (chunk.set_block_state(wx - base_x, wy, wz - base_z, BlockStateKey{"minecraft:spawner", ""}))
      ++changed;
    fortress_add_spawner(chunk, wx, wy, wz);
  }

  void bridge_straight() {
    generate_box(0, 3, 0, 4, 4, 18, bricks(), bricks());
    generate_box(1, 5, 0, 3, 7, 18, air(), air());
    generate_box(0, 5, 0, 0, 5, 18, bricks(), bricks());
    generate_box(4, 5, 0, 4, 5, 18, bricks(), bricks());
    generate_box(0, 2, 0, 4, 2, 5, bricks(), bricks());
    generate_box(0, 2, 13, 4, 2, 18, bricks(), bricks());
    generate_box(0, 0, 0, 4, 1, 3, bricks(), bricks());
    generate_box(0, 0, 15, 4, 1, 18, bricks(), bricks());
    for (int32_t x = 0; x <= 4; ++x)
      for (int32_t z = 0; z <= 2; ++z) {
        fill_column_down(x, -1, z);
        fill_column_down(x, -1, 18 - z);
      }
    const BlockStateKey nse = fence(true, true, true, false);
    const BlockStateKey nsw = fence(true, true, false, true);
    generate_box(0, 1, 1, 0, 4, 1, nse, nse);
    generate_box(0, 3, 4, 0, 4, 4, nse, nse);
    generate_box(0, 3, 14, 0, 4, 14, nse, nse);
    generate_box(0, 1, 17, 0, 4, 17, nse, nse);
    generate_box(4, 1, 1, 4, 4, 1, nsw, nsw);
    generate_box(4, 3, 4, 4, 4, 4, nsw, nsw);
    generate_box(4, 3, 14, 4, 4, 14, nsw, nsw);
    generate_box(4, 1, 17, 4, 4, 17, nsw, nsw);
  }

  void bridge_crossing() {
    generate_box(7, 3, 0, 11, 4, 18, bricks(), bricks());
    generate_box(0, 3, 7, 18, 4, 11, bricks(), bricks());
    generate_box(8, 5, 0, 10, 7, 18, air(), air());
    generate_box(0, 5, 8, 18, 7, 10, air(), air());
    generate_box(7, 5, 0, 7, 5, 7, bricks(), bricks());
    generate_box(7, 5, 11, 7, 5, 18, bricks(), bricks());
    generate_box(11, 5, 0, 11, 5, 7, bricks(), bricks());
    generate_box(11, 5, 11, 11, 5, 18, bricks(), bricks());
    generate_box(0, 5, 7, 7, 5, 7, bricks(), bricks());
    generate_box(11, 5, 7, 18, 5, 7, bricks(), bricks());
    generate_box(0, 5, 11, 7, 5, 11, bricks(), bricks());
    generate_box(11, 5, 11, 18, 5, 11, bricks(), bricks());
    generate_box(7, 2, 0, 11, 2, 5, bricks(), bricks());
    generate_box(7, 2, 13, 11, 2, 18, bricks(), bricks());
    generate_box(7, 0, 0, 11, 1, 3, bricks(), bricks());
    generate_box(7, 0, 15, 11, 1, 18, bricks(), bricks());
    for (int32_t x = 7; x <= 11; ++x)
      for (int32_t z = 0; z <= 2; ++z) {
        fill_column_down(x, -1, z);
        fill_column_down(x, -1, 18 - z);
      }
    generate_box(0, 2, 7, 5, 2, 11, bricks(), bricks());
    generate_box(13, 2, 7, 18, 2, 11, bricks(), bricks());
    generate_box(0, 0, 7, 3, 1, 11, bricks(), bricks());
    generate_box(15, 0, 7, 18, 1, 11, bricks(), bricks());
    for (int32_t x = 0; x <= 2; ++x)
      for (int32_t z = 7; z <= 11; ++z) {
        fill_column_down(x, -1, z);
        fill_column_down(18 - x, -1, z);
      }
  }

  void room_crossing() {
    generate_box(0, 0, 0, 6, 1, 6, bricks(), bricks());
    generate_box(0, 2, 0, 6, 7, 6, air(), air());
    generate_box(0, 2, 0, 1, 6, 0, bricks(), bricks());
    generate_box(0, 2, 6, 1, 6, 6, bricks(), bricks());
    generate_box(5, 2, 0, 6, 6, 0, bricks(), bricks());
    generate_box(5, 2, 6, 6, 6, 6, bricks(), bricks());
    generate_box(0, 2, 0, 0, 6, 1, bricks(), bricks());
    generate_box(0, 2, 5, 0, 6, 6, bricks(), bricks());
    generate_box(6, 2, 0, 6, 6, 1, bricks(), bricks());
    generate_box(6, 2, 5, 6, 6, 6, bricks(), bricks());
    const BlockStateKey we = fence(false, false, true, true);
    const BlockStateKey ns = fence(true, true, false, false);
    generate_box(2, 6, 0, 4, 6, 0, bricks(), bricks());
    generate_box(2, 5, 0, 4, 5, 0, we, we);
    generate_box(2, 6, 6, 4, 6, 6, bricks(), bricks());
    generate_box(2, 5, 6, 4, 5, 6, we, we);
    generate_box(0, 6, 2, 0, 6, 4, bricks(), bricks());
    generate_box(0, 5, 2, 0, 5, 4, ns, ns);
    generate_box(6, 6, 2, 6, 6, 4, bricks(), bricks());
    generate_box(6, 5, 2, 6, 5, 4, ns, ns);
    for (int32_t x = 0; x <= 6; ++x)
      for (int32_t z = 0; z <= 6; ++z)
        fill_column_down(x, -1, z);
  }

  void stairs_room() {
    generate_box(0, 0, 0, 6, 1, 6, bricks(), bricks());
    generate_box(0, 2, 0, 6, 10, 6, air(), air());
    generate_box(0, 2, 0, 1, 8, 0, bricks(), bricks());
    generate_box(5, 2, 0, 6, 8, 0, bricks(), bricks());
    generate_box(0, 2, 1, 0, 8, 6, bricks(), bricks());
    generate_box(6, 2, 1, 6, 8, 6, bricks(), bricks());
    generate_box(1, 2, 6, 5, 8, 6, bricks(), bricks());
    const BlockStateKey we = fence(false, false, true, true);
    const BlockStateKey ns = fence(true, true, false, false);
    generate_box(0, 3, 2, 0, 5, 4, ns, ns);
    generate_box(6, 3, 2, 6, 5, 2, ns, ns);
    generate_box(6, 3, 4, 6, 5, 4, ns, ns);
    place(bricks(), 5, 2, 5);
    generate_box(4, 2, 5, 4, 3, 5, bricks(), bricks());
    generate_box(3, 2, 5, 3, 4, 5, bricks(), bricks());
    generate_box(2, 2, 5, 2, 5, 5, bricks(), bricks());
    generate_box(1, 2, 5, 1, 6, 5, bricks(), bricks());
    generate_box(1, 7, 1, 5, 7, 4, bricks(), bricks());
    generate_box(6, 8, 2, 6, 8, 4, air(), air());
    generate_box(2, 6, 0, 4, 8, 0, bricks(), bricks());
    generate_box(2, 5, 0, 4, 5, 0, we, we);
    for (int32_t x = 0; x <= 6; ++x)
      for (int32_t z = 0; z <= 6; ++z)
        fill_column_down(x, -1, z);
  }

  void monster_throne() {
    generate_box(0, 2, 0, 6, 7, 7, air(), air());
    generate_box(1, 0, 0, 5, 1, 7, bricks(), bricks());
    generate_box(1, 2, 1, 5, 2, 7, bricks(), bricks());
    generate_box(1, 3, 2, 5, 3, 7, bricks(), bricks());
    generate_box(1, 4, 3, 5, 4, 7, bricks(), bricks());
    generate_box(1, 2, 0, 1, 4, 2, bricks(), bricks());
    generate_box(5, 2, 0, 5, 4, 2, bricks(), bricks());
    generate_box(1, 5, 2, 1, 5, 3, bricks(), bricks());
    generate_box(5, 5, 2, 5, 5, 3, bricks(), bricks());
    generate_box(0, 5, 3, 0, 5, 8, bricks(), bricks());
    generate_box(6, 5, 3, 6, 5, 8, bricks(), bricks());
    generate_box(1, 5, 8, 5, 5, 8, bricks(), bricks());
    const BlockStateKey we = fence(false, false, true, true);
    const BlockStateKey ns = fence(true, true, false, false);
    place(fence(false, false, false, true), 1, 6, 3);
    place(fence(false, false, true, false), 5, 6, 3);
    place(fence(true, false, true, false), 0, 6, 3);
    place(fence(true, false, false, true), 6, 6, 3);
    generate_box(0, 6, 4, 0, 6, 7, ns, ns);
    generate_box(6, 6, 4, 6, 6, 7, ns, ns);
    place(fence(false, true, true, false), 0, 6, 8);
    place(fence(false, true, false, true), 6, 6, 8);
    generate_box(1, 6, 8, 5, 6, 8, we, we);
    place(fence(false, false, true, false), 1, 7, 8);
    generate_box(2, 7, 8, 4, 7, 8, we, we);
    place(fence(false, false, false, true), 5, 7, 8);
    place(fence(false, false, true, false), 2, 8, 8);
    place(we, 3, 8, 8);
    place(fence(false, false, false, true), 4, 8, 8);
    spawner(3, 5, 5);
    for (int32_t x = 0; x <= 6; ++x)
      for (int32_t z = 0; z <= 6; ++z)
        fill_column_down(x, -1, z);
  }

  void castle_entrance() {
    generate_box(0, 3, 0, 12, 4, 12, bricks(), bricks());
    generate_box(0, 5, 0, 12, 13, 12, air(), air());
    generate_box(0, 5, 0, 1, 12, 12, bricks(), bricks());
    generate_box(11, 5, 0, 12, 12, 12, bricks(), bricks());
    generate_box(2, 5, 11, 4, 12, 12, bricks(), bricks());
    generate_box(8, 5, 11, 10, 12, 12, bricks(), bricks());
    generate_box(5, 9, 11, 7, 12, 12, bricks(), bricks());
    generate_box(2, 5, 0, 4, 12, 1, bricks(), bricks());
    generate_box(8, 5, 0, 10, 12, 1, bricks(), bricks());
    generate_box(5, 9, 0, 7, 12, 1, bricks(), bricks());
    generate_box(2, 11, 2, 10, 12, 10, bricks(), bricks());
    generate_box(5, 8, 0, 7, 8, 0, BlockStateKey{"minecraft:nether_brick_fence", ""},
                 BlockStateKey{"minecraft:nether_brick_fence", ""});
    const BlockStateKey we = fence(false, false, true, true);
    const BlockStateKey ns = fence(true, true, false, false);
    for (int32_t i = 1; i <= 11; i += 2) {
      generate_box(i, 10, 0, i, 11, 0, we, we);
      generate_box(i, 10, 12, i, 11, 12, we, we);
      generate_box(0, 10, i, 0, 11, i, ns, ns);
      generate_box(12, 10, i, 12, 11, i, ns, ns);
      place(bricks(), i, 13, 0);
      place(bricks(), i, 13, 12);
      place(bricks(), 0, 13, i);
      place(bricks(), 12, 13, i);
      if (i == 11)
        continue;
      place(we, i + 1, 13, 0);
      place(we, i + 1, 13, 12);
      place(ns, 0, 13, i + 1);
      place(ns, 12, 13, i + 1);
    }
    place(fence(true, false, true, false), 0, 13, 0);
    place(fence(false, true, true, false), 0, 13, 12);
    place(fence(false, true, false, true), 12, 13, 12);
    place(fence(true, false, false, true), 12, 13, 0);
    for (int32_t z = 3; z <= 9; z += 2) {
      generate_box(1, 7, z, 1, 8, z, fence(true, true, false, true),
                   fence(true, true, false, true));
      generate_box(11, 7, z, 11, 8, z, fence(true, true, true, false),
                   fence(true, true, true, false));
    }
    generate_box(4, 2, 0, 8, 2, 12, bricks(), bricks());
    generate_box(0, 2, 4, 12, 2, 8, bricks(), bricks());
    generate_box(4, 0, 0, 8, 1, 3, bricks(), bricks());
    generate_box(4, 0, 9, 8, 1, 12, bricks(), bricks());
    generate_box(0, 0, 4, 3, 1, 8, bricks(), bricks());
    generate_box(9, 0, 4, 12, 1, 8, bricks(), bricks());
    for (int32_t x = 4; x <= 8; ++x)
      for (int32_t z = 0; z <= 2; ++z) {
        fill_column_down(x, -1, z);
        fill_column_down(x, -1, 12 - z);
      }
    for (int32_t x = 0; x <= 2; ++x)
      for (int32_t z = 4; z <= 8; ++z) {
        fill_column_down(x, -1, z);
        fill_column_down(12 - x, -1, z);
      }
    generate_box(5, 5, 5, 7, 5, 7, bricks(), bricks());
    generate_box(6, 1, 6, 6, 4, 6, air(), air());
    place(bricks(), 6, 0, 6);
    place(lava(), 6, 5, 6);
  }

  void castle_small_corridor() {
    generate_box(0, 0, 0, 4, 1, 4, bricks(), bricks());
    generate_box(0, 2, 0, 4, 5, 4, air(), air());
    const BlockStateKey ns = fence(true, true, false, false);
    generate_box(0, 2, 0, 0, 5, 4, bricks(), bricks());
    generate_box(4, 2, 0, 4, 5, 4, bricks(), bricks());
    generate_box(0, 3, 1, 0, 4, 1, ns, ns);
    generate_box(0, 3, 3, 0, 4, 3, ns, ns);
    generate_box(4, 3, 1, 4, 4, 1, ns, ns);
    generate_box(4, 3, 3, 4, 4, 3, ns, ns);
    generate_box(0, 6, 0, 4, 6, 4, bricks(), bricks());
    for (int32_t x = 0; x <= 4; ++x)
      for (int32_t z = 0; z <= 4; ++z)
        fill_column_down(x, -1, z);
  }

  void castle_small_crossing() {
    generate_box(0, 0, 0, 4, 1, 4, bricks(), bricks());
    generate_box(0, 2, 0, 4, 5, 4, air(), air());
    generate_box(0, 2, 0, 0, 5, 0, bricks(), bricks());
    generate_box(4, 2, 0, 4, 5, 0, bricks(), bricks());
    generate_box(0, 2, 4, 0, 5, 4, bricks(), bricks());
    generate_box(4, 2, 4, 4, 5, 4, bricks(), bricks());
    generate_box(0, 6, 0, 4, 6, 4, bricks(), bricks());
    for (int32_t x = 0; x <= 4; ++x)
      for (int32_t z = 0; z <= 4; ++z)
        fill_column_down(x, -1, z);
  }

  void castle_turn(bool right) {
    generate_box(0, 0, 0, 4, 1, 4, bricks(), bricks());
    generate_box(0, 2, 0, 4, 5, 4, air(), air());
    const BlockStateKey we = fence(false, false, true, true);
    const BlockStateKey ns = fence(true, true, false, false);
    generate_box(0, 2, 0, 0, 5, 4, bricks(), bricks());
    generate_box(0, 3, 1, 0, 4, 1, ns, ns);
    generate_box(0, 3, 3, 0, 4, 3, ns, ns);
    generate_box(4, 2, 0, 4, 5, 0, bricks(), bricks());
    generate_box(1, 2, 4, 4, 5, 4, bricks(), bricks());
    generate_box(1, 3, 4, 1, 4, 4, we, we);
    generate_box(3, 3, 4, 3, 4, 4, we, we);
    if (piece.needing_chest != 0)
      chest(right ? 1 : 3, 2, 3);
    generate_box(0, 6, 0, 4, 6, 4, bricks(), bricks());
    for (int32_t x = 0; x <= 4; ++x)
      for (int32_t z = 0; z <= 4; ++z)
        fill_column_down(x, -1, z);
  }

  void castle_corridor_stairs() {
    const BlockStateKey south = stairs("south");
    const BlockStateKey ns = fence(true, true, false, false);
    for (int32_t step = 0; step <= 9; ++step) {
      const int32_t floor = std::max(1, 7 - step);
      const int32_t roof = std::min(std::max(floor + 5, 14 - step), 13);
      generate_box(0, 0, step, 4, floor, step, bricks(), bricks());
      generate_box(1, floor + 1, step, 3, roof - 1, step, air(), air());
      if (step <= 6) {
        place(south, 1, floor + 1, step);
        place(south, 2, floor + 1, step);
        place(south, 3, floor + 1, step);
      }
      generate_box(0, roof, step, 4, roof, step, bricks(), bricks());
      generate_box(0, floor + 1, step, 0, roof - 1, step, bricks(), bricks());
      generate_box(4, floor + 1, step, 4, roof - 1, step, bricks(), bricks());
      if ((step & 1) == 0) {
        generate_box(0, floor + 2, step, 0, floor + 3, step, ns, ns);
        generate_box(4, floor + 2, step, 4, floor + 3, step, ns, ns);
      }
      for (int32_t x = 0; x <= 4; ++x)
        fill_column_down(x, -1, step);
    }
  }

  void castle_t_balcony() {
    const BlockStateKey ns = fence(true, true, false, false);
    const BlockStateKey we = fence(false, false, true, true);
    generate_box(0, 0, 0, 8, 1, 8, bricks(), bricks());
    generate_box(0, 2, 0, 8, 5, 8, air(), air());
    generate_box(0, 6, 0, 8, 6, 5, bricks(), bricks());
    generate_box(0, 2, 0, 2, 5, 0, bricks(), bricks());
    generate_box(6, 2, 0, 8, 5, 0, bricks(), bricks());
    generate_box(1, 3, 0, 1, 4, 0, we, we);
    generate_box(7, 3, 0, 7, 4, 0, we, we);
    generate_box(0, 2, 4, 8, 2, 8, bricks(), bricks());
    generate_box(1, 1, 4, 2, 2, 4, air(), air());
    generate_box(6, 1, 4, 7, 2, 4, air(), air());
    generate_box(1, 3, 8, 7, 3, 8, we, we);
    place(fence(false, true, true, false), 0, 3, 8);
    place(fence(false, true, false, true), 8, 3, 8);
    generate_box(0, 3, 6, 0, 3, 7, ns, ns);
    generate_box(8, 3, 6, 8, 3, 7, ns, ns);
    generate_box(0, 3, 4, 0, 5, 5, bricks(), bricks());
    generate_box(8, 3, 4, 8, 5, 5, bricks(), bricks());
    generate_box(1, 3, 5, 2, 5, 5, bricks(), bricks());
    generate_box(6, 3, 5, 7, 5, 5, bricks(), bricks());
    generate_box(1, 4, 5, 1, 5, 5, we, we);
    generate_box(7, 4, 5, 7, 5, 5, we, we);
    for (int32_t z = 0; z <= 5; ++z)
      for (int32_t x = 0; x <= 8; ++x)
        fill_column_down(x, -1, z);
  }

  void castle_stalk_room() {
    generate_box(0, 3, 0, 12, 4, 12, bricks(), bricks());
    generate_box(0, 5, 0, 12, 13, 12, air(), air());
    generate_box(0, 5, 0, 1, 12, 12, bricks(), bricks());
    generate_box(11, 5, 0, 12, 12, 12, bricks(), bricks());
    generate_box(2, 5, 11, 4, 12, 12, bricks(), bricks());
    generate_box(8, 5, 11, 10, 12, 12, bricks(), bricks());
    generate_box(5, 9, 11, 7, 12, 12, bricks(), bricks());
    generate_box(2, 5, 0, 4, 12, 1, bricks(), bricks());
    generate_box(8, 5, 0, 10, 12, 1, bricks(), bricks());
    generate_box(5, 9, 0, 7, 12, 1, bricks(), bricks());
    generate_box(2, 11, 2, 10, 12, 10, bricks(), bricks());
    const BlockStateKey we = fence(false, false, true, true);
    const BlockStateKey ns = fence(true, true, false, false);
    const BlockStateKey nsw = fence(true, true, false, true);
    const BlockStateKey nse = fence(true, true, true, false);
    for (int32_t i = 1; i <= 11; i += 2) {
      generate_box(i, 10, 0, i, 11, 0, we, we);
      generate_box(i, 10, 12, i, 11, 12, we, we);
      generate_box(0, 10, i, 0, 11, i, ns, ns);
      generate_box(12, 10, i, 12, 11, i, ns, ns);
      place(bricks(), i, 13, 0);
      place(bricks(), i, 13, 12);
      place(bricks(), 0, 13, i);
      place(bricks(), 12, 13, i);
      if (i == 11)
        continue;
      place(we, i + 1, 13, 0);
      place(we, i + 1, 13, 12);
      place(ns, 0, 13, i + 1);
      place(ns, 12, 13, i + 1);
    }
    place(fence(true, false, true, false), 0, 13, 0);
    place(fence(false, true, true, false), 0, 13, 12);
    place(fence(false, true, false, true), 12, 13, 12);
    place(fence(true, false, false, true), 12, 13, 0);
    for (int32_t z = 3; z <= 9; z += 2) {
      generate_box(1, 7, z, 1, 8, z, nsw, nsw);
      generate_box(11, 7, z, 11, 8, z, nse, nse);
    }
    const BlockStateKey north = stairs("north");
    for (int32_t i = 0; i <= 6; ++i) {
      const int32_t z = i + 4;
      for (int32_t x = 5; x <= 7; ++x)
        place(north, x, 5 + i, z);
      if (z >= 5 && z <= 8)
        generate_box(5, 5, z, 7, i + 4, z, bricks(), bricks());
      else if (z >= 9 && z <= 10)
        generate_box(5, 8, z, 7, i + 4, z, bricks(), bricks());
      if (i < 1)
        continue;
      generate_box(5, 6 + i, z, 7, 9 + i, z, air(), air());
    }
    for (int32_t x = 5; x <= 7; ++x)
      place(north, x, 12, 11);
    generate_box(5, 6, 7, 5, 7, 7, nse, nse);
    generate_box(7, 6, 7, 7, 7, 7, nsw, nsw);
    generate_box(5, 13, 12, 7, 13, 12, air(), air());
    generate_box(2, 5, 2, 3, 5, 3, bricks(), bricks());
    generate_box(2, 5, 9, 3, 5, 10, bricks(), bricks());
    generate_box(2, 5, 4, 2, 5, 8, bricks(), bricks());
    generate_box(9, 5, 2, 10, 5, 3, bricks(), bricks());
    generate_box(9, 5, 9, 10, 5, 10, bricks(), bricks());
    generate_box(10, 5, 4, 10, 5, 8, bricks(), bricks());
    const BlockStateKey east = stairs("east");
    const BlockStateKey west = stairs("west");
    place(west, 4, 5, 2);
    place(west, 4, 5, 3);
    place(west, 4, 5, 9);
    place(west, 4, 5, 10);
    place(east, 8, 5, 2);
    place(east, 8, 5, 3);
    place(east, 8, 5, 9);
    place(east, 8, 5, 10);
    generate_box(3, 4, 4, 4, 4, 8, soul_sand(), soul_sand());
    generate_box(8, 4, 4, 9, 4, 8, soul_sand(), soul_sand());
    generate_box(3, 5, 4, 4, 5, 8, wart(), wart());
    generate_box(8, 5, 4, 9, 5, 8, wart(), wart());
    generate_box(4, 2, 0, 8, 2, 12, bricks(), bricks());
    generate_box(0, 2, 4, 12, 2, 8, bricks(), bricks());
    generate_box(4, 0, 0, 8, 1, 3, bricks(), bricks());
    generate_box(4, 0, 9, 8, 1, 12, bricks(), bricks());
    generate_box(0, 0, 4, 3, 1, 8, bricks(), bricks());
    generate_box(9, 0, 4, 12, 1, 8, bricks(), bricks());
    for (int32_t x = 4; x <= 8; ++x)
      for (int32_t z = 0; z <= 2; ++z) {
        fill_column_down(x, -1, z);
        fill_column_down(x, -1, 12 - z);
      }
    for (int32_t x = 0; x <= 2; ++x)
      for (int32_t z = 4; z <= 8; ++z) {
        fill_column_down(x, -1, z);
        fill_column_down(12 - x, -1, z);
      }
  }

  void bridge_end_filler() {
    WgLegacyRandom self(piece.self_seed);
    for (int32_t x = 0; x <= 4; ++x)
      for (int32_t y = 3; y <= 4; ++y) {
        const int32_t z = self.next_int(8);
        generate_box(x, y, 0, x, y, z, bricks(), bricks());
      }
    int32_t z = self.next_int(8);
    generate_box(0, 5, 0, 0, 5, z, bricks(), bricks());
    z = self.next_int(8);
    generate_box(4, 5, 0, 4, 5, z, bricks(), bricks());
    for (int32_t x = 0; x <= 4; ++x)
      generate_box(x, 2, 0, x, 2, self.next_int(5), bricks(), bricks());
    for (int32_t x = 0; x <= 4; ++x)
      for (int32_t y = 0; y <= 1; ++y)
        generate_box(x, y, 0, x, y, self.next_int(3), bricks(), bricks());
  }

  Chunk& chunk;
  const WgFortressPiece& piece;
  int64_t world_seed;
  int32_t base_x;
  int32_t base_z;
  uint32_t changed = 0;
};

} // namespace

uint32_t wg_place_fortress(Chunk& chunk, const WgFortressStart& start) {
  uint32_t changed = 0;
  for (uint32_t i = 0; i < start.piece_count; ++i)
    changed += FortressWriter(chunk, start.pieces[i], start.world_seed).run();
  return changed;
}

uint32_t wg_place_village(Chunk& chunk, const WgJigsawStart& start, WgStructureHeights& heights) {
  return wg_place_jigsaw(chunk, start, heights);
}

uint32_t wg_place_outpost(Chunk& chunk, const WgJigsawStart& start, WgStructureHeights& heights) {
  return wg_place_jigsaw(chunk, start, heights);
}

uint32_t wg_place_ancient_city(Chunk& chunk, const WgJigsawStart& start,
                               WgStructureHeights& heights) {
  return wg_place_jigsaw(chunk, start, heights);
}

uint32_t wg_place_trail_ruins(Chunk& chunk, const WgJigsawStart& start,
                              WgStructureHeights& heights) {
  return wg_place_jigsaw(chunk, start, heights);
}

WgRuinedPortalStart wg_ruined_portal_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                           WgStructureHeights& heights) {
  WgRuinedPortalStart start;
  const WgStructureSet* set = wg_structure_set_by_name("minecraft:ruined_portals");
  if (set == nullptr || !wg_structure_is_chunk(seed, *set, chunk_x, chunk_z))
    return start;

  WgLegacyRandom context(0);
  seed_large_feature(context, seed, chunk_x, chunk_z);

  // `Structure.generate`'s weighted member pick.
  const WgStructure* structure = nullptr;
  int32_t total = 0;
  for (uint32_t i = 0; i < set->entry_count; ++i)
    total += static_cast<int32_t>(kWgStructureSetEntries[set->entry_offset + i].weight);
  if (total <= 0)
    return start;
  int32_t choice = context.next_int(total);
  for (uint32_t i = 0; i < set->entry_count; ++i) {
    const WgStructureSetEntry& entry = kWgStructureSetEntries[set->entry_offset + i];
    choice -= static_cast<int32_t>(entry.weight);
    if (choice >= 0)
      continue;
    structure = wg_structure_by_name(entry.structure);
    break;
  }
  if (structure == nullptr)
    return start;

  // The structure's own context random starts here; the set pick above used the
  // same `setLargeFeatureSeed` stream in vanilla's `Structure.generate`.
  WgLegacyRandom random(0);
  seed_large_feature(random, seed, chunk_x, chunk_z);

  const WgRuinedPortalConfig* config = nullptr;
  for (const WgRuinedPortalConfig& candidate : kWgRuinedPortalConfigs) {
    if (std::strcmp(candidate.name, structure->name) == 0) {
      config = &candidate;
      break;
    }
  }
  if (config == nullptr || config->setup_count == 0)
    return start;
  const WgRuinedPortalSetup* setup = nullptr;
  if (config->setup_count == 1) {
    setup = &kWgRuinedPortalSetups[config->setup_offset];
  } else {
    float weight = 0.0f;
    for (uint16_t i = 0; i < config->setup_count; ++i)
      weight += kWgRuinedPortalSetups[config->setup_offset + i].weight;
    float pick = random.next_float();
    for (uint16_t i = 0; i < config->setup_count; ++i) {
      const WgRuinedPortalSetup& candidate = kWgRuinedPortalSetups[config->setup_offset + i];
      pick -= candidate.weight / weight;
      if (pick < 0.0f) {
        setup = &candidate;
        break;
      }
    }
    if (setup == nullptr)
      setup = &kWgRuinedPortalSetups[config->setup_offset + config->setup_count - 1];
  }

  bool air_pocket = false;
  if (setup->air_pocket == 1.0f)
    air_pocket = true;
  else if (setup->air_pocket > 0.0f)
    air_pocket = random.next_float() < setup->air_pocket;

  const bool giant = random.next_float() < 0.05f;
  const char* template_name =
      giant ? kGiantPortalTemplates[random.next_int(3)] : kPortalTemplates[random.next_int(10)];
  const WgPieceTemplate* templ = wg_piece_template_by_name(template_name);
  if (templ == nullptr)
    return start;
  const uint32_t rotation = static_cast<uint32_t>(random.next_int(4));
  const uint8_t mirror = random.next_float() < 0.5f ? 0 : 1;
  const int32_t pivot_x = templ->size_x / 2;
  const int32_t pivot_z = templ->size_z / 2;

  const int32_t base_x = chunk_x * 16;
  const int32_t base_z = chunk_z * 16;
  const int32_t y_span = templ->size_y;
  PortalXZ corner0 = portal_transform(0, 0, rotation, mirror, pivot_x, pivot_z);
  PortalXZ corner1 =
      portal_transform(templ->size_x - 1, templ->size_z - 1, rotation, mirror, pivot_x, pivot_z);
  const int32_t min_x = base_x + std::min(corner0.x, corner1.x);
  const int32_t max_x = base_x + std::max(corner0.x, corner1.x);
  const int32_t min_z = base_z + std::min(corner0.z, corner1.z);
  const int32_t max_z = base_z + std::max(corner0.z, corner1.z);
  const int32_t center_x = (min_x + max_x) / 2;
  const int32_t center_z = (min_z + max_z) / 2;
  const auto height_type = setup->placement == 2 ? WgStructureHeightmap::kOceanFloor
                                                 : WgStructureHeightmap::kWorldSurface;
  const int32_t surface_y = heights.height(center_x, center_z, height_type) - 1;

  int32_t projected = surface_y;
  if (setup->placement == 5) { // in_nether
    if (air_pocket)
      projected = portal_random_between(random, 32, 100);
    else if (random.next_float() < 0.5f)
      projected = portal_random_between(random, 27, 29);
    else
      projected = portal_random_between(random, 29, 100);
  } else if (setup->placement == 3) { // in_mountain
    const int32_t max_y = surface_y - y_span;
    projected = 70 < max_y ? portal_random_between(random, 70, max_y) : max_y;
  } else if (setup->placement == 4) { // underground
    const int32_t max_y = surface_y - y_span;
    projected = -49 < max_y ? portal_random_between(random, -49, max_y) : max_y;
  } else if (setup->placement == 1) { // partly_buried
    projected = surface_y - y_span + portal_random_between(random, 2, 8);
  }
  const int32_t min_y = -49;
  for (; projected > min_y; --projected) {
    int32_t solid = 0;
    const int32_t xs[4] = {min_x, max_x, min_x, max_x};
    const int32_t zs[4] = {min_z, min_z, max_z, max_z};
    for (int32_t i = 0; i < 4; ++i) {
      const int32_t first_free = heights.height(xs[i], zs[i], height_type);
      if (projected >= first_free)
        continue;
      if (++solid == 3)
        break;
    }
    if (solid >= 3)
      break;
  }

  start.valid = true;
  start.structure = structure;
  start.world_seed = seed;
  start.chunk_x = chunk_x;
  start.chunk_z = chunk_z;
  start.templ = templ;
  start.rotation = rotation;
  start.mirror = mirror;
  start.placement = setup->placement;
  start.cold = setup->cold;
  start.air_pocket = air_pocket ? 1 : 0;
  start.overgrown = setup->overgrown;
  start.vines = setup->vines;
  start.blackstone = setup->blackstone;
  start.mossiness = setup->mossiness;
  start.x = base_x;
  start.y = projected;
  start.z = base_z;
  start.pivot_x = pivot_x;
  start.pivot_z = pivot_z;
  start.min_x = min_x;
  start.max_x = max_x;
  start.min_z = min_z;
  start.max_z = max_z;
  start.min_y = projected;
  start.max_y = projected + templ->size_y - 1;
  return start;
}

uint32_t wg_place_ruined_portal(Chunk& chunk, const WgRuinedPortalStart& start,
                                WgStructureHeights& heights) {
  if (!start.valid || start.templ == nullptr)
    return 0;
  const WgPieceTemplate& templ = *start.templ;
  if (templ.palette_count == 0)
    return 0;
  const int32_t base_x = chunk.x * 16;
  const int32_t base_z = chunk.z * 16;
  WgLegacyRandom random(wg_mth_get_seed(start.x, start.y, start.z));
  WgLegacyRandom palette_random(wg_mth_get_seed(start.x, start.y, start.z));
  const uint32_t palette_index =
      static_cast<uint32_t>(palette_random.next_int(static_cast<int32_t>(templ.palette_count)));
  const WgPiecePalette& palette = kWgPiecePalettes[templ.palette_offset + palette_index];

  struct Pending {
    int32_t x, y, z;
    BlockStateKey state;
  };
  std::vector<Pending> pending;
  for (uint32_t i = 0; i < templ.block_count; ++i) {
    const WgPieceBlock& block = kWgPieceBlocks[templ.block_offset + i];
    const WgState& state =
        kWgPieceStates[kWgPiecePaletteStates[palette.state_offset + block.state]];
    const PortalXZ offset = portal_transform(block.x, block.z, start.rotation, start.mirror,
                                             start.pivot_x, start.pivot_z);
    const int32_t world_x = start.x + offset.x;
    const int32_t world_y = start.y + block.y;
    const int32_t world_z = start.z + offset.z;
    if (world_x < base_x || world_x >= base_x + 16 || world_z < base_z || world_z >= base_z + 16 ||
        world_y < kMinY || world_y >= kMinY + kHeight)
      continue;
    if (block.marker != 0)
      continue;
    if (std::strcmp(state.name, "minecraft:structure_block") == 0)
      continue;
    if (is_air_state(BlockStateKey{state.name, ""}) && (start.air_pocket == 0))
      continue;

    BlockStateKey key{state.name, state.properties};
    if (start.mirror != 0)
      key.properties = mirror_front_back(key.properties);
    key.properties = wg_piece_rotate_properties(key.properties, start.rotation);

    // `RuleProcessor`: gold 30%, lava/magma per placement, netherrack 7%.
    WgLegacyRandom rule_random(wg_mth_get_seed(world_x, world_y, world_z));
    if (key.name == "minecraft:gold_block" && rule_random.next_float() < 0.3f) {
      key = BlockStateKey{"minecraft:air", ""};
    } else if (key.name == "minecraft:lava") {
      if (start.placement == 2)
        key = BlockStateKey{"minecraft:magma_block", ""};
      else if (start.cold != 0)
        key = BlockStateKey{"minecraft:netherrack", ""};
      else if (rule_random.next_float() < 0.2f)
        key = BlockStateKey{"minecraft:magma_block", ""};
    } else if (start.cold == 0 && key.name == "minecraft:netherrack" &&
               rule_random.next_float() < 0.07f) {
      key = BlockStateKey{"minecraft:magma_block", ""};
    }

    key = portal_age_block(key, start.mossiness, rule_random);
    if (start.blackstone != 0) {
      const std::string replaced = stone_bricks_for_blackstone(key.name);
      if (replaced != key.name) {
        const std::string old_props = key.properties;
        key.name = replaced;
        if (key.name.find("_stairs") != std::string::npos)
          key.properties = old_props;
        else if (key.name.find("_slab") != std::string::npos) {
          key.properties = "type=bottom,waterlogged=false";
          for (const auto& [k, v] : parse_properties(old_props))
            if (k == "type")
              key.properties = "type=" + v + ",waterlogged=false";
        } else {
          key.properties.clear();
        }
      }
    }
    // `ProtectedBlockProcessor`: a cell the feature stage may not replace is
    // dropped entirely.
    if (portal_protected_block(
            chunk.block_state_at(world_x - base_x, world_y, world_z - base_z).name))
      continue;
    // `LavaSubmergedBlockProcessor`: a non-full block placed into lava becomes
    // lava again. The pieces' full cubes are the stone-family blocks; stairs,
    // slabs, walls, fences and bars are not.
    const BlockStateKey current = chunk.block_state_at(world_x - base_x, world_y, world_z - base_z);
    if (current.name == "minecraft:lava" &&
        (portal_is_stairs(key.name) || portal_is_slab(key.name) || portal_is_wall(key.name) ||
         key.name == "minecraft:iron_bars" || key.name == "minecraft:iron_chain"))
      key = BlockStateKey{"minecraft:lava", ""};
    pending.push_back(Pending{world_x, world_y, world_z, std::move(key)});
  }

  uint32_t changed = 0;
  for (const Pending& block : pending)
    if (chunk.set_block_state(block.x - base_x, block.y, block.z - base_z, block.state))
      ++changed;

  // `spreadNetherrack`, the drip columns, vines and leaves. The pass runs over
  // the piece's box only, like `postProcess`'s `getBoundingBox` loop.
  const bool follow_ground = start.placement == 0 || start.placement == 2;
  const int32_t center_x = (start.min_x + start.max_x) / 2;
  const int32_t center_z = (start.min_z + start.max_z) / 2;
  constexpr float kProbability[] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                    0.9f, 0.9f, 0.8f, 0.7f, 0.6f, 0.4f, 0.2f};
  const int32_t max_distance = 14;
  const int32_t average_width = (start.max_x - start.min_x + start.max_z - start.min_z) / 2;
  const int32_t distance_adjustment = random.next_int(std::max(1, 8 - average_width / 2));
  for (int32_t x = center_x - max_distance; x <= center_x + max_distance; ++x) {
    for (int32_t z = center_z - max_distance; z <= center_z + max_distance; ++z) {
      const int32_t distance = std::abs(x - center_x) + std::abs(z - center_z);
      const int32_t adjusted = std::max(0, distance + distance_adjustment);
      if (adjusted >= max_distance)
        continue;
      if (!(random.next_double() < kProbability[adjusted]))
        continue;
      const int32_t surface = heights.height(x, z, WgStructureHeightmap::kWorldSurface) - 1;
      const int32_t y = follow_ground ? surface : std::min(start.min_y, surface);
      if (std::abs(y - start.min_y) > 3)
        continue;
      if (x < base_x || x >= base_x + 16 || z < base_z || z >= base_z + 16 || y < kMinY ||
          y >= kMinY + kHeight)
        continue;
      const BlockStateKey current = chunk.block_state_at(x - base_x, y, z - base_z);
      if (current.name == "minecraft:air" || current.name == "minecraft:obsidian" ||
          current.name == "minecraft:lava" || portal_protected_block(current.name))
        continue;
      const BlockStateKey netherrack{"minecraft:netherrack", ""};
      if (chunk.set_block_state(x - base_x, y, z - base_z, netherrack))
        ++changed;
      if (start.overgrown != 0 && random.next_float() < 0.5f) {
        if (y + 1 < kMinY + kHeight &&
            chunk.block_state_at(x - base_x, y + 1, z - base_z).name == "minecraft:air") {
          if (chunk.set_block_state(x - base_x, y + 1, z - base_z,
                                    BlockStateKey{"minecraft:jungle_leaves", "persistent=true"}))
            ++changed;
        }
      }
      int32_t drip_y = y - 1;
      for (int32_t cap = 8; cap > 0 && random.next_float() < 0.5f; --cap) {
        if (drip_y < kMinY ||
            chunk.block_state_at(x - base_x, drip_y, z - base_z).name != "minecraft:air")
          break;
        const bool magma = start.cold == 0 && random.next_float() < 0.07f;
        if (chunk.set_block_state(
                x - base_x, drip_y, z - base_z,
                BlockStateKey{magma ? "minecraft:magma_block" : "minecraft:netherrack", ""}))
          ++changed;
        --drip_y;
      }
    }
  }
  return changed;
}

WgOceanRuinStart wg_ocean_ruin_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                     WgClimateSampler& sampler, WgStructureHeights& heights) {
  WgOceanRuinStart start;
  const WgStructureSet* set = wg_structure_set_by_name("minecraft:ocean_ruins");
  if (set == nullptr || !wg_structure_is_chunk(seed, *set, chunk_x, chunk_z))
    return start;

  const int32_t center_x = chunk_x * 16 + 8;
  const int32_t center_z = chunk_z * 16 + 8;
  const int32_t center_y =
      heights.height(center_x, center_z, WgStructureHeightmap::kOceanFloor) - 1;
  WgLegacyRandom context(0);
  seed_large_feature(context, seed, chunk_x, chunk_z);
  const uint32_t rotation = static_cast<uint32_t>(context.next_int(4));
  // The set carries both members; `Structure.generate` picks the first one
  // whose biome set accepts the centre column, then `StructureStart` keeps
  // that structure. The generated tables preserve the registry order.
  const WgStructure* structure = nullptr;
  const WgOceanRuinConfig* cfg = nullptr;
  for (uint32_t i = 0; i < set->entry_count; ++i) {
    const WgStructureSetEntry& entry = kWgStructureSetEntries[set->entry_offset + i];
    const WgStructure* candidate = wg_structure_by_name(entry.structure);
    if (candidate == nullptr)
      continue;
    if (!wg_structure_start_biome_is_valid(sampler, *candidate, center_x, center_y, center_z))
      continue;
    structure = candidate;
    cfg = (std::strcmp(candidate->name, "minecraft:ocean_ruin_cold") == 0)
              ? &kWgOceanRuinConfigs[0]
              : &kWgOceanRuinConfigs[1];
    break;
  }
  if (structure == nullptr || cfg == nullptr)
    return start;

  const bool large = context.next_float() <= cfg->large_probability;
  start.valid = true;
  start.structure = structure;
  start.world_seed = seed;
  start.chunk_x = chunk_x;
  start.chunk_z = chunk_z;
  start.cold = cfg->cold;
  start.large = large ? 1 : 0;
  const int32_t x = chunk_x * 16;
  const int32_t z = chunk_z * 16;
  add_ocean_piece(start, context, *cfg, x, 90, z, rotation, large, large ? 0.9f : 0.8f);

  if (!large || context.next_float() > cfg->cluster_probability)
    return start;

  // `OceanRuinPieces.addClusterRuins`.
  const OceanXZ parent_corner = ocean_transform(15, 15, rotation);
  const int32_t parent_min_x = std::min(x, x + parent_corner.x);
  const int32_t parent_max_x = std::max(x, x + parent_corner.x);
  const int32_t parent_min_z = std::min(z, z + parent_corner.z);
  const int32_t parent_max_z = std::max(z, z + parent_corner.z);
  const int32_t bottom_min_x = parent_min_x;
  const int32_t bottom_min_z = parent_min_z;

  std::array<std::pair<int32_t, int32_t>, 8> positions{};
  positions[0] = {-16 + ocean_next_int(context, 1, 8), 16 + ocean_next_int(context, 1, 7)};
  positions[1] = {-16 + ocean_next_int(context, 1, 8), ocean_next_int(context, 1, 7)};
  positions[2] = {-16 + ocean_next_int(context, 1, 8), -16 + ocean_next_int(context, 4, 8)};
  positions[3] = {ocean_next_int(context, 1, 7), 16 + ocean_next_int(context, 1, 7)};
  positions[4] = {ocean_next_int(context, 1, 7), -16 + ocean_next_int(context, 4, 6)};
  positions[5] = {16 + ocean_next_int(context, 1, 7), 16 + ocean_next_int(context, 3, 8)};
  positions[6] = {16 + ocean_next_int(context, 1, 7), ocean_next_int(context, 1, 7)};
  positions[7] = {16 + ocean_next_int(context, 1, 7), -16 + ocean_next_int(context, 4, 8)};
  for (auto& position : positions) {
    position.first += bottom_min_x;
    position.second += bottom_min_z;
  }
  std::vector<std::pair<int32_t, int32_t>> available(positions.begin(), positions.end());
  const int32_t ruins = ocean_next_int(context, 4, 8);
  for (int32_t i = 0; i < ruins && !available.empty(); ++i) {
    const int32_t index = context.next_int(static_cast<int32_t>(available.size()));
    const auto [pos_x, pos_z] = available[static_cast<size_t>(index)];
    available.erase(available.begin() + index);
    const uint32_t next_rotation = static_cast<uint32_t>(context.next_int(4));
    const OceanXZ corner = ocean_transform(5, 6, next_rotation);
    const int32_t next_x = pos_x + corner.x;
    const int32_t next_z = pos_z + corner.z;
    const int32_t next_min_x = std::min(pos_x, next_x);
    const int32_t next_max_x = std::max(pos_x, next_x);
    const int32_t next_min_z = std::min(pos_z, next_z);
    const int32_t next_max_z = std::max(pos_z, next_z);
    if (next_max_x >= parent_min_x && next_min_x <= parent_max_x && next_max_z >= parent_min_z &&
        next_min_z <= parent_max_z)
      continue;
    add_ocean_piece(start, context, *cfg, pos_x, 90, pos_z, next_rotation, false, 0.8f);
  }
  return start;
}

uint32_t wg_place_ocean_ruin(Chunk& chunk, const WgOceanRuinStart& start,
                             WgStructureHeights& heights) {
  if (!start.valid)
    return 0;
  const int32_t base_x = chunk.x * 16;
  const int32_t base_z = chunk.z * 16;
  uint32_t changed = 0;
  struct Pending {
    int32_t x, y, z;
    BlockStateKey state;
  };
  struct Marker {
    int32_t x, y, z;
    std::string metadata;
  };

  for (uint32_t piece_index = 0; piece_index < start.piece_count; ++piece_index) {
    const WgOceanRuinPiece& piece = start.pieces[piece_index];
    if (piece.templ == nullptr || piece.templ->palette_count == 0)
      continue;
    const int32_t y = heights.height(piece.x, piece.z, WgStructureHeightmap::kOceanFloor);
    WgLegacyRandom palette_random(wg_mth_get_seed(piece.x, y, piece.z));
    const uint32_t palette_index = static_cast<uint32_t>(
        palette_random.next_int(static_cast<int32_t>(piece.templ->palette_count)));
    const WgPiecePalette& palette = kWgPiecePalettes[piece.templ->palette_offset + palette_index];

    std::vector<Pending> pending;
    std::vector<Marker> markers;
    for (uint32_t i = 0; i < piece.templ->block_count; ++i) {
      const WgPieceBlock& block = kWgPieceBlocks[piece.templ->block_offset + i];
      const WgState& state =
          kWgPieceStates[kWgPiecePaletteStates[palette.state_offset + block.state]];
      const XZ offset = rotate_pivot(block.x, block.z, piece.rotation, piece.templ->pivot_x,
                                     piece.templ->pivot_z);
      const int32_t world_x = piece.x + offset.x;
      const int32_t world_y = y + block.y;
      const int32_t world_z = piece.z + offset.z;
      if (block.marker != 0) {
        if (world_x >= base_x && world_x < base_x + 16 && world_z >= base_z &&
            world_z < base_z + 16)
          markers.push_back(Marker{world_x, world_y, world_z, kWgPieceMarkers[block.marker - 1]});
        continue;
      }
      if (std::strcmp(state.name, "minecraft:structure_block") == 0)
        continue;
      if (is_air_state(BlockStateKey{state.name, ""}))
        continue;
      if (world_x < base_x || world_x >= base_x + 16 || world_z < base_z ||
          world_z >= base_z + 16 || world_y < kMinY || world_y >= kMinY + kHeight)
        continue;
      // `BlockRotProcessor`: a fresh `RandomSource.create(Mth.getSeed(pos))` per
      // cell, skipped when the draw is above the integrity.
      WgLegacyRandom rot_random(wg_mth_get_seed(world_x, world_y, world_z));
      if (rot_random.next_float() > piece.integrity)
        continue;
      std::string properties = wg_piece_rotate_properties(state.properties, piece.rotation);
      pending.push_back(
          Pending{world_x, world_y, world_z, BlockStateKey{state.name, std::move(properties)}});
    }

    // `CappedProcessor` around the suspicious-block rule: shuffle every
    // processed cell, then replace at most five sand/gravel cells.
    WgLegacyRandom capped_random(wg_mth_get_seed(piece.x, y, piece.z));
    std::vector<uint32_t> order(pending.size());
    for (uint32_t i = 0; i < order.size(); ++i)
      order[i] = i;
    for (size_t i = order.size(); i > 1; --i) {
      const size_t swap = static_cast<size_t>(capped_random.next_int(static_cast<int32_t>(i)));
      std::swap(order[i - 1], order[swap]);
    }
    int32_t replaced = 0;
    const char* suspicious_name =
        piece.cold ? "minecraft:suspicious_gravel" : "minecraft:suspicious_sand";
    const char* candidate_name = piece.cold ? "minecraft:gravel" : "minecraft:sand";
    const char* archaeology = piece.cold ? "minecraft:archaeology/ocean_ruin_cold"
                                         : "minecraft:archaeology/ocean_ruin_warm";
    for (uint32_t index : order) {
      if (replaced >= 5)
        break;
      Pending& block = pending[index];
      if (block.state.name != candidate_name)
        continue;
      block.state.name = suspicious_name;
      block.state.properties = "dusted=0";
      add_brushable_loot(chunk, block.x, block.y, block.z, archaeology, start.world_seed);
      ++replaced;
    }

    for (const Pending& block : pending) {
      if (chunk.set_block_state(block.x - base_x, block.y, block.z - base_z, block.state))
        ++changed;
    }

    for (const Marker& marker : markers) {
      const int32_t local_x = marker.x - base_x;
      const int32_t local_z = marker.z - base_z;
      if (marker.metadata == "chest") {
        const bool waterlogged =
            chunk.block_state_at(local_x, marker.y, local_z).name == "minecraft:water";
        const BlockStateKey chest{"minecraft:chest",
                                  std::string("facing=north,type=single,waterlogged=") +
                                      (waterlogged ? "true" : "false")};
        if (chunk.set_block_state(local_x, marker.y, local_z, chest)) {
          ++changed;
          add_chest_loot(chunk, marker.x, marker.y, marker.z,
                         piece.large ? "minecraft:chests/underwater_ruin_big"
                                     : "minecraft:chests/underwater_ruin_small",
                         start.world_seed);
        }
      } else if (marker.metadata == "drowned") {
        const BlockStateKey state{marker.y > 63 ? "minecraft:air" : "minecraft:water", ""};
        if (chunk.set_block_state(local_x, marker.y, local_z, state))
          ++changed;
      }
    }
  }
  return changed;
}

uint32_t wg_place_desert_pyramid(Chunk& chunk, const WgDesertPyramidStart& start,
                                 WgStructureHeights& heights) {
  (void)heights;
  if (!start.valid)
    return 0;
  const int32_t base_min_x = start.min_x;
  const int32_t base_max_x = start.max_x;
  const int32_t base_min_z = start.min_z;
  const int32_t base_max_z = start.max_z;
  // `updateHeightPositionToLowestGroundHeight(level, -random.nextInt(3))` is
  // resolved by the start, so every chunk that intersects the 21 x 21 piece
  // uses the same final base height.
  WgLegacyRandom placement_random(wg_mth_get_seed(start.x, 64, start.z));
  // `postProcess` consumes this draw before any chest or cellar work; the
  // start has already applied it to the final height, but the stream still
  // has to advance by one.
  (void)placement_random.next_int(3);
  const int32_t base_y = start.ground_y;
  ScatteredWriter writer(chunk, start.direction, base_min_x, base_max_x, base_min_z, base_max_z,
                         base_y);

  const BlockStateKey air{"minecraft:air", ""};
  const BlockStateKey sand = sand_state();
  const BlockStateKey sandstone = sandstone_state();
  const BlockStateKey cut = cut_sandstone_state();
  const BlockStateKey chiseled = chiseled_sandstone_state();
  const BlockStateKey orange = orange_terracotta_state();
  const BlockStateKey blue = blue_terracotta_state();

  writer.generate_box(0, -4, 0, 20, 0, 20, sandstone, sandstone);
  for (int32_t pos = 1; pos <= 9; ++pos) {
    writer.generate_box(pos, pos, pos, 20 - pos, pos, 20 - pos, sandstone, sandstone);
    writer.generate_box(pos + 1, pos, pos + 1, 19 - pos, pos, 19 - pos, air, air);
  }
  for (int32_t x = 0; x < 21; ++x)
    for (int32_t z = 0; z < 21; ++z)
      writer.fill_column_down(sandstone, x, -5, z);

  const BlockStateKey north = sandstone_stairs_state("north");
  const BlockStateKey south = sandstone_stairs_state("south");
  const BlockStateKey east = sandstone_stairs_state("east");
  const BlockStateKey west = sandstone_stairs_state("west");
  writer.generate_box(0, 0, 0, 4, 9, 4, sandstone, air);
  writer.generate_box(1, 10, 1, 3, 10, 3, sandstone, sandstone);
  writer.place_public(north, 2, 10, 0);
  writer.place_public(south, 2, 10, 4);
  writer.place_public(east, 0, 10, 2);
  writer.place_public(west, 4, 10, 2);
  writer.generate_box(16, 0, 0, 20, 9, 4, sandstone, air);
  writer.generate_box(17, 10, 1, 19, 10, 3, sandstone, sandstone);
  writer.place_public(north, 18, 10, 0);
  writer.place_public(south, 18, 10, 4);
  writer.place_public(east, 16, 10, 2);
  writer.place_public(west, 20, 10, 2);
  writer.generate_box(8, 0, 0, 12, 4, 4, sandstone, air);
  writer.generate_box(9, 1, 0, 11, 3, 4, air, air);
  writer.place_public(cut, 9, 1, 1);
  writer.place_public(cut, 9, 2, 1);
  writer.place_public(cut, 9, 3, 1);
  writer.place_public(cut, 10, 3, 1);
  writer.place_public(cut, 11, 3, 1);
  writer.place_public(cut, 11, 2, 1);
  writer.place_public(cut, 11, 1, 1);
  writer.generate_box(4, 1, 1, 8, 3, 3, sandstone, air);
  writer.generate_box(4, 1, 2, 8, 2, 2, air, air);
  writer.generate_box(12, 1, 1, 16, 3, 3, sandstone, air);
  writer.generate_box(12, 1, 2, 16, 2, 2, air, air);
  writer.generate_box(5, 4, 5, 15, 4, 15, sandstone, sandstone);
  writer.generate_box(9, 4, 9, 11, 4, 11, air, air);
  writer.generate_box(8, 1, 8, 8, 3, 8, cut, cut);
  writer.generate_box(12, 1, 8, 12, 3, 8, cut, cut);
  writer.generate_box(8, 1, 12, 8, 3, 12, cut, cut);
  writer.generate_box(12, 1, 12, 12, 3, 12, cut, cut);
  writer.generate_box(1, 1, 5, 4, 4, 11, sandstone, sandstone);
  writer.generate_box(16, 1, 5, 19, 4, 11, sandstone, sandstone);
  writer.generate_box(6, 7, 9, 6, 7, 11, sandstone, sandstone);
  writer.generate_box(14, 7, 9, 14, 7, 11, sandstone, sandstone);
  writer.generate_box(5, 5, 9, 5, 7, 11, cut, cut);
  writer.generate_box(15, 5, 9, 15, 7, 11, cut, cut);
  writer.place_public(air, 5, 5, 10);
  writer.place_public(air, 5, 6, 10);
  writer.place_public(air, 6, 6, 10);
  writer.place_public(air, 15, 5, 10);
  writer.place_public(air, 15, 6, 10);
  writer.place_public(air, 14, 6, 10);
  writer.generate_box(2, 4, 4, 2, 6, 4, air, air);
  writer.generate_box(18, 4, 4, 18, 6, 4, air, air);
  writer.place_public(north, 2, 4, 5);
  writer.place_public(north, 2, 3, 4);
  writer.place_public(north, 18, 4, 5);
  writer.place_public(north, 18, 3, 4);
  writer.generate_box(1, 1, 3, 2, 2, 3, sandstone, sandstone);
  writer.generate_box(18, 1, 3, 19, 2, 3, sandstone, sandstone);
  writer.place_public(sandstone, 1, 1, 2);
  writer.place_public(sandstone, 19, 1, 2);
  writer.place_public(sandstone_slab_state(), 1, 2, 2);
  writer.place_public(sandstone_slab_state(), 19, 2, 2);
  writer.place_public(west, 2, 1, 2);
  writer.place_public(east, 18, 1, 2);
  writer.generate_box(4, 3, 5, 4, 3, 17, sandstone, sandstone);
  writer.generate_box(16, 3, 5, 16, 3, 17, sandstone, sandstone);
  writer.generate_box(3, 1, 5, 4, 2, 16, air, air);
  writer.generate_box(15, 1, 5, 16, 2, 16, air, air);
  for (int32_t z = 5; z <= 17; z += 2) {
    writer.place_public(cut, 4, 1, z);
    writer.place_public(chiseled, 4, 2, z);
    writer.place_public(cut, 16, 1, z);
    writer.place_public(chiseled, 16, 2, z);
  }
  writer.place_public(orange, 10, 0, 7);
  writer.place_public(orange, 10, 0, 8);
  writer.place_public(orange, 9, 0, 9);
  writer.place_public(orange, 11, 0, 9);
  writer.place_public(orange, 8, 0, 10);
  writer.place_public(orange, 12, 0, 10);
  writer.place_public(orange, 7, 0, 10);
  writer.place_public(orange, 13, 0, 10);
  writer.place_public(orange, 9, 0, 11);
  writer.place_public(orange, 11, 0, 11);
  writer.place_public(orange, 10, 0, 12);
  writer.place_public(orange, 10, 0, 13);
  writer.place_public(blue, 10, 0, 10);
  for (int32_t x = 0; x <= 20; x += 20) {
    writer.place_public(cut, x, 2, 1);
    writer.place_public(orange, x, 2, 2);
    writer.place_public(cut, x, 2, 3);
    writer.place_public(cut, x, 3, 1);
    writer.place_public(orange, x, 3, 2);
    writer.place_public(cut, x, 3, 3);
    writer.place_public(orange, x, 4, 1);
    writer.place_public(chiseled, x, 4, 2);
    writer.place_public(orange, x, 4, 3);
    writer.place_public(cut, x, 5, 1);
    writer.place_public(orange, x, 5, 2);
    writer.place_public(cut, x, 5, 3);
    writer.place_public(orange, x, 6, 1);
    writer.place_public(chiseled, x, 6, 2);
    writer.place_public(orange, x, 6, 3);
    writer.place_public(orange, x, 7, 1);
    writer.place_public(orange, x, 7, 2);
    writer.place_public(orange, x, 7, 3);
    writer.place_public(cut, x, 8, 1);
    writer.place_public(cut, x, 8, 2);
    writer.place_public(cut, x, 8, 3);
  }
  for (int32_t x = 2; x <= 18; x += 16) {
    writer.place_public(cut, x - 1, 2, 0);
    writer.place_public(orange, x, 2, 0);
    writer.place_public(cut, x + 1, 2, 0);
    writer.place_public(cut, x - 1, 3, 0);
    writer.place_public(orange, x, 3, 0);
    writer.place_public(cut, x + 1, 3, 0);
    writer.place_public(orange, x - 1, 4, 0);
    writer.place_public(chiseled, x, 4, 0);
    writer.place_public(orange, x + 1, 4, 0);
    writer.place_public(cut, x - 1, 5, 0);
    writer.place_public(orange, x, 5, 0);
    writer.place_public(cut, x + 1, 5, 0);
    writer.place_public(orange, x - 1, 6, 0);
    writer.place_public(chiseled, x, 6, 0);
    writer.place_public(orange, x + 1, 6, 0);
    writer.place_public(orange, x - 1, 7, 0);
    writer.place_public(orange, x, 7, 0);
    writer.place_public(orange, x + 1, 7, 0);
    writer.place_public(cut, x - 1, 8, 0);
    writer.place_public(cut, x, 8, 0);
    writer.place_public(cut, x + 1, 8, 0);
  }
  writer.generate_box(8, 4, 0, 12, 6, 0, cut, cut);
  writer.place_public(air, 8, 6, 0);
  writer.place_public(air, 12, 6, 0);
  writer.place_public(orange, 9, 5, 0);
  writer.place_public(chiseled, 10, 5, 0);
  writer.place_public(orange, 11, 5, 0);

  // The TNT cellar.
  writer.generate_box(8, -14, 8, 12, -11, 12, cut, cut);
  writer.generate_box(8, -10, 8, 12, -10, 12, chiseled, chiseled);
  writer.generate_box(8, -9, 8, 12, -9, 12, cut, cut);
  writer.generate_box(8, -8, 8, 12, -1, 12, sandstone, sandstone);
  writer.generate_box(9, -11, 9, 11, -1, 11, air, air);
  writer.place_public(pressure_plate_state(), 10, -11, 10);
  writer.generate_box(9, -13, 9, 11, -13, 11, tnt_state(), air);
  writer.place_public(air, 8, -11, 10);
  writer.place_public(air, 8, -10, 10);
  writer.place_public(chiseled, 7, -10, 10);
  writer.place_public(cut, 7, -11, 10);
  writer.place_public(air, 12, -11, 10);
  writer.place_public(air, 12, -10, 10);
  writer.place_public(chiseled, 13, -10, 10);
  writer.place_public(cut, 13, -11, 10);
  writer.place_public(air, 10, -11, 8);
  writer.place_public(air, 10, -10, 8);
  writer.place_public(chiseled, 10, -10, 7);
  writer.place_public(cut, 10, -11, 7);
  writer.place_public(air, 10, -11, 12);
  writer.place_public(air, 10, -10, 12);
  writer.place_public(chiseled, 10, -10, 13);
  writer.place_public(cut, 10, -11, 13);

  for (int32_t i = 0; i < 4; ++i) {
    const int32_t xo = (i == 1 ? 1 : (i == 3 ? -1 : 0)) * 2;
    const int32_t zo = (i == 0 ? -1 : (i == 2 ? 1 : 0)) * 2;
    // Every chest consumes its loot-table draw, whether it lies in this chunk
    // or was already written by an earlier application. That keeps the
    // selector and collapsed-roof streams independent of the chunk split.
    (void)placement_random.next_long();
    const auto [world_x, world_z] = writer.world_xz(10 + xo, 10 + zo);
    const int32_t world_y = base_y - 11;
    const int32_t local_x = world_x - chunk.x * 16;
    const int32_t local_z = world_z - chunk.z * 16;
    if (local_x < 0 || local_x >= 16 || local_z < 0 || local_z >= 16 || world_y < kMinY)
      continue;
    if (chunk.block_state_at(local_x, world_y, local_z).name == "minecraft:chest")
      continue;
    const char* facing = reoriented_chest_facing(chunk, local_x, world_y, local_z);
    // `createChest` writes the reoriented state with `Level.setBlock`, after
    // the piece's palette lookup, so it must not be mirrored/rotated again.
    writer.place_raw(BlockStateKey{"minecraft:chest", std::string("facing=") + facing +
                                                          ",type=single,waterlogged=false"},
                     10 + xo, -11, 10 + zo);
    if (chunk.block_state_at(local_x, world_y, local_z).name == "minecraft:chest")
      add_chest_loot(chunk, world_x, world_y, world_z, "minecraft:chests/desert_pyramid",
                     start.world_seed);
  }

  // `addCellar` with roomCenter (16, -4, 13).
  const int32_t cx = 16;
  const int32_t cy = -4;
  const int32_t cz = 13;
  // `sandstoneStairs.rotate(Rotation.COUNTERCLOCKWISE_90)`: north -> west.
  const BlockStateKey ccw = sandstone_stairs_state("west");
  writer.place_public(ccw, 13, -1, 17);
  writer.place_public(ccw, 14, -2, 17);
  writer.place_public(ccw, 15, -3, 17);
  const bool variant = placement_random.next_boolean();
  writer.place_public(sand, cx - 4, cy + 4, cz + 4);
  writer.place_public(sand, cx - 3, cy + 4, cz + 4);
  writer.place_public(sand, cx - 2, cy + 4, cz + 4);
  writer.place_public(sand, cx - 1, cy + 4, cz + 4);
  writer.place_public(sand, cx, cy + 4, cz + 4);
  writer.place_public(sand, cx - 2, cy + 3, cz + 4);
  writer.place_public(variant ? sand : sandstone, cx - 1, cy + 3, cz + 4);
  writer.place_public(!variant ? sand : sandstone, cx, cy + 3, cz + 4);
  writer.place_public(sand, cx - 1, cy + 2, cz + 4);
  writer.place_public(sandstone, cx, cy + 2, cz + 4);
  writer.place_public(sand, cx, cy + 1, cz + 4);
  writer.place_public(cut, cx, cy, cz);
  writer.generate_box(cx - 3, cy + 1, cz - 3, cx - 3, cy + 1, cz + 2, cut, cut, true);
  writer.generate_box(cx + 3, cy + 1, cz - 3, cx + 3, cy + 1, cz + 2, cut, cut, true);
  writer.generate_box(cx - 3, cy + 1, cz - 3, cx + 3, cy + 1, cz - 2, cut, cut, true);
  writer.generate_box(cx - 3, cy + 1, cz + 3, cx + 3, cy + 1, cz + 3, cut, cut, true);
  writer.generate_box(cx - 3, cy + 2, cz - 3, cx - 3, cy + 2, cz + 2, chiseled, chiseled, true);
  writer.generate_box(cx + 3, cy + 2, cz - 3, cx + 3, cy + 2, cz + 2, chiseled, chiseled, true);
  writer.generate_box(cx - 3, cy + 2, cz - 3, cx + 3, cy + 2, cz - 2, chiseled, chiseled, true);
  writer.generate_box(cx - 3, cy + 2, cz + 3, cx + 3, cy + 2, cz + 3, chiseled, chiseled, true);
  writer.generate_box(cx - 3, -1, cz - 3, cx - 3, -1, cz + 2, cut, cut, true);
  writer.generate_box(cx + 3, -1, cz - 3, cx + 3, -1, cz + 2, cut, cut, true);
  writer.generate_box(cx - 3, -1, cz - 3, cx + 3, -1, cz - 2, cut, cut, true);
  writer.generate_box(cx - 3, -1, cz + 3, cx + 3, -1, cz + 3, cut, cut, true);

  // `placeSandBox` and the room's decorative shell. The potential sand cells
  // are the same set `placeSand` records.
  std::vector<std::tuple<int32_t, int32_t, int32_t>> potential;
  for (int32_t y = cy + 1; y <= cy + 3; ++y)
    for (int32_t x = cx - 2; x <= cx + 2; ++x)
      for (int32_t z = cz - 2; z <= cz + 2; ++z)
        potential.emplace_back(x, y, z);
  for (const auto& [x, y, z] :
       std::initializer_list<std::tuple<int32_t, int32_t, int32_t>>{{cx + 3, cy + 1, cz},
                                                                    {cx + 3, cy + 2, cz},
                                                                    {cx - 3, cy + 1, cz},
                                                                    {cx - 3, cy + 2, cz},
                                                                    {cx, cy + 1, cz + 3},
                                                                    {cx, cy + 2, cz + 3},
                                                                    {cx, cy + 1, cz - 3},
                                                                    {cx, cy + 2, cz - 3}})
    potential.emplace_back(x, y, z);

  // `placeCollapsedRoof` over x 14..18, z 11..15 at local y 0.
  for (int32_t x = cx - 2; x <= cx + 2; ++x)
    for (int32_t z = cz - 2; z <= cz + 2; ++z)
      writer.place_public(placement_random.next_float() < 0.33f ? sandstone : sand, x, cy + 4, z);

  writer.place_public(blue, cx, cy, cz);
  writer.place_public(orange, cx + 1, cy, cz - 1);
  writer.place_public(orange, cx + 1, cy, cz + 1);
  writer.place_public(orange, cx - 1, cy, cz - 1);
  writer.place_public(orange, cx - 1, cy, cz + 1);
  writer.place_public(orange, cx + 2, cy, cz);
  writer.place_public(orange, cx - 2, cy, cz);
  writer.place_public(orange, cx, cy, cz + 2);
  writer.place_public(orange, cx, cy, cz - 2);
  writer.place_public(orange, cx + 3, cy, cz);
  writer.place_public(cut, cx + 4, cy + 1, cz);
  writer.place_public(chiseled, cx + 4, cy + 2, cz);
  writer.place_public(orange, cx - 3, cy, cz);
  writer.place_public(cut, cx - 4, cy + 1, cz);
  writer.place_public(chiseled, cx - 4, cy + 2, cz);
  writer.place_public(orange, cx, cy, cz + 3);
  writer.place_public(orange, cx, cy, cz - 3);
  writer.place_public(cut, cx, cy + 1, cz - 4);
  writer.place_public(chiseled, cx, -2, cz - 4);

  // `afterPlace`: one suspicious cell at the collapsed-roof pick, then the
  // shuffled potential set, `nextInt(5, 8)` of it suspicious and the rest sand.
  const int32_t base_x = chunk.x * 16;
  const int32_t base_z = chunk.z * 16;
  const auto inside_chunk = [&](int32_t world_x, int32_t world_z) {
    return world_x >= base_x && world_x < base_x + 16 && world_z >= base_z && world_z < base_z + 16;
  };
  // `placeCollapsedRoof` seeds its own positional random at the first cell of
  // the 5 x 5 roof (`getWorldPos(x0, y0, z0)`), then picks one cell in it.
  // `RandomSource.createThreadLocalInstance(level.getSeed())` seeds a
  // single-threaded source whose `forkPositional()` takes one `nextLong`
  // before producing the positional factory seed.
  WgLegacyRandom factory_source(start.world_seed);
  const int64_t positional_factory_seed = factory_source.next_long();
  const auto [roof_anchor_x, roof_anchor_z] = writer.world_xz(cx - 2, cz - 2);
  WgLegacyRandom roof_random(wg_mth_get_seed(roof_anchor_x, base_y + cy + 4, roof_anchor_z) ^
                             positional_factory_seed);
  const int32_t roof_x = cx - 2 + roof_random.next_int(5);
  const int32_t roof_z = cz - 2 + roof_random.next_int(5);
  const auto [roof_world_x, roof_world_z] = writer.world_xz(roof_x, roof_z);
  if (inside_chunk(roof_world_x, roof_world_z)) {
    const int32_t roof_y = base_y + cy + 4;
    if (chunk.set_block_state(roof_world_x - base_x, roof_y, roof_world_z - base_z,
                              BlockStateKey{"minecraft:suspicious_sand", "dusted=0"}))
      add_brushable_loot(chunk, roof_world_x, roof_y, roof_world_z,
                         "minecraft:archaeology/desert_pyramid",
                         packed_block_pos(roof_world_x, roof_y, roof_world_z));
  }

  // `SortedArraySet` deduplicates all potential cells across the whole piece,
  // not just this chunk; `afterPlace` then shuffles that global list once.
  std::vector<std::tuple<int32_t, int32_t, int32_t>> unique;
  unique.reserve(potential.size());
  for (const auto& local : potential) {
    const auto [world_x, world_z] = writer.world_xz(std::get<0>(local), std::get<2>(local));
    unique.emplace_back(world_x, base_y + std::get<1>(local), world_z);
  }
  // `Vec3i.compareTo` orders potential cells by y, then z, then x; the
  // SortedArraySet's iteration order is what `Util.shuffle` starts from.
  std::sort(unique.begin(), unique.end(),
            [](const std::tuple<int32_t, int32_t, int32_t>& a,
               const std::tuple<int32_t, int32_t, int32_t>& b) {
              if (std::get<1>(a) != std::get<1>(b))
                return std::get<1>(a) < std::get<1>(b);
              if (std::get<2>(a) != std::get<2>(b))
                return std::get<2>(a) < std::get<2>(b);
              return std::get<0>(a) < std::get<0>(b);
            });
  unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
  // `pieces.calculateBoundingBox().getCenter()`: the 21 x 15 x 21 piece box.
  WgLegacyRandom pick_random(wg_mth_get_seed((start.min_x + start.max_x) / 2,
                                             (base_y + base_y + 14) / 2,
                                             (start.min_z + start.max_z) / 2) ^
                             positional_factory_seed);
  for (size_t i = unique.size(); i > 1; --i) {
    const size_t swap = static_cast<size_t>(pick_random.next_int(static_cast<int32_t>(i)));
    std::swap(unique[i - 1], unique[swap]);
  }
  int32_t suspicious_count =
      std::min<int32_t>(static_cast<int32_t>(unique.size()), 5 + pick_random.next_int(3));
  for (const auto& [world_x, world_y, world_z] : unique) {
    if (suspicious_count > 0) {
      --suspicious_count;
      if (!inside_chunk(world_x, world_z))
        continue;
      if (chunk.set_block_state(world_x - base_x, world_y, world_z - base_z,
                                BlockStateKey{"minecraft:suspicious_sand", "dusted=0"}))
        add_brushable_loot(chunk, world_x, world_y, world_z, "minecraft:archaeology/desert_pyramid",
                           packed_block_pos(world_x, world_y, world_z));
      continue;
    }
    if (inside_chunk(world_x, world_z))
      chunk.set_block_state(world_x - base_x, world_y, world_z - base_z, sand);
  }
  return writer.changed();
}

WgShipwreckStart wg_shipwreck_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                    WgClimateSampler& sampler, WgStructureHeights& heights) {
  WgShipwreckStart start;
  const WgStructureSet* set = wg_structure_set_by_name("minecraft:shipwrecks");
  if (set == nullptr || !wg_structure_is_chunk(seed, *set, chunk_x, chunk_z))
    return start;

  // `ChunkGenerator.createStructures`' weighted pick, with a fresh
  // `WorldgenRandom` (`LegacyRandomSource(0)`) seeded `setLargeFeatureSeed`.
  struct Option {
    const WgStructure* structure;
    uint32_t weight;
  };
  Option options[8];
  int32_t count = 0;
  int32_t total = 0;
  for (uint32_t i = 0; i < set->entry_count && count < 8; ++i) {
    const WgStructureSetEntry& entry = kWgStructureSetEntries[set->entry_offset + i];
    const WgStructure* structure = wg_structure_by_name(entry.structure);
    if (structure == nullptr)
      continue;
    options[count++] = Option{structure, entry.weight};
    total += static_cast<int32_t>(entry.weight);
  }
  if (count == 0 || total <= 0)
    return start;

  WgLegacyRandom pick(0);
  seed_large_feature(pick, seed, chunk_x, chunk_z);

  const int32_t center_x = chunk_x * 16 + 8;
  const int32_t center_z = chunk_z * 16 + 8;

  while (count > 0) {
    int32_t choice = pick.next_int(total);
    int32_t index = 0;
    while (index < count) {
      choice -= static_cast<int32_t>(options[index].weight);
      if (choice < 0)
        break;
      ++index;
    }
    const WgStructure* structure = options[index].structure;
    const bool beached = std::strcmp(structure->name, "minecraft:shipwreck_beached") == 0;

    // `Structure.generate` makes its own context random, reseeded for this
    // attempt; `Rotation.getRandom` then `Util.getRandom` are the two draws.
    WgLegacyRandom context(0);
    seed_large_feature(context, seed, chunk_x, chunk_z);
    const uint32_t rotation = static_cast<uint32_t>(context.next_int(4));
    const int32_t template_count = beached ? static_cast<int32_t>(sizeof(kWgShipwreckBeached))
                                           : static_cast<int32_t>(sizeof(kWgShipwreckOcean));
    const uint8_t* template_list = beached ? kWgShipwreckBeached : kWgShipwreckOcean;
    const WgPieceTemplate& templ =
        kWgPieceTemplates[template_list[context.next_int(template_count)]];

    // `onTopOfChunkCenter` reads the centre column; `isValidBiome` looks the
    // biome up at that point, one below the heightmap's first-free answer.
    const int32_t surface = heights.height(center_x, center_z,
                                           beached ? WgStructureHeightmap::kWorldSurface
                                                   : WgStructureHeightmap::kOceanFloor) -
                            1;
    if (wg_structure_start_biome_is_valid(sampler, *structure, center_x, surface, center_z)) {
      start.valid = true;
      start.structure = structure;
      start.templ = &templ;
      start.rotation = rotation;
      start.world_seed = seed;
      start.chunk_x = chunk_x;
      start.chunk_z = chunk_z;
      start.x = chunk_x * 16;
      start.y = kInitialY;
      start.z = chunk_z * 16;
      const WgPieceBox box = wg_piece_box(templ, rotation, start.x, start.y, start.z);
      start.min_x = box.min_x;
      start.min_y = box.min_y;
      start.min_z = box.min_z;
      start.max_x = box.max_x;
      start.max_y = box.max_y;
      start.max_z = box.max_z;
      return start;
    }

    // `options.remove(index); total -= weight;` and try the next member.
    total -= static_cast<int32_t>(options[index].weight);
    for (int32_t i = index; i + 1 < count; ++i)
      options[i] = options[i + 1];
    --count;
  }
  return start;
}

WgSwampHutStart wg_swamp_hut_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                   WgClimateSampler& sampler, WgStructureHeights& heights) {
  WgSwampHutStart start;
  const WgStructureSet* set = wg_structure_set_by_name("minecraft:swamp_huts");
  if (set == nullptr || !wg_structure_is_chunk(seed, *set, chunk_x, chunk_z))
    return start;
  const WgStructure* structure = wg_structure_by_name("minecraft:swamp_hut");
  if (structure == nullptr)
    return start;

  // `Structure.GenerationContext`'s own random, then the piece constructor's
  // `getRandomHorizontalDirection`: HORIZONTAL is north, east, south, west.
  WgLegacyRandom context(0);
  seed_large_feature(context, seed, chunk_x, chunk_z);
  const uint8_t direction = static_cast<uint8_t>(context.next_int(4));

  // `onTopOfChunkCenter(WORLD_SURFACE_WG)` supplies the biome probe's position;
  // the piece itself is placed at the chunk's minimum corner, y=64.
  const int32_t center_x = chunk_x * 16 + 8;
  const int32_t center_z = chunk_z * 16 + 8;
  // `getFirstOccupiedHeight` is the heightmap's first-free answer minus one.
  const int32_t center_y =
      heights.height(center_x, center_z, WgStructureHeightmap::kWorldSurface) - 1;
  if (!wg_structure_start_biome_is_valid(sampler, *structure, center_x, center_y, center_z))
    return start;

  start.structure = structure;
  start.direction = direction;
  start.chunk_x = chunk_x;
  start.chunk_z = chunk_z;
  start.x = chunk_x * 16;
  start.y = kSwampHutY;
  start.z = chunk_z * 16;
  // `StructurePiece.makeBoundingBox`: a north/south orientation puts the width
  // on x and the depth on z; east/west swaps them.
  if (direction == 0 || direction == 2) {
    start.min_x = start.x;
    start.max_x = start.x + kSwampHutWidth - 1;
    start.min_z = start.z;
    start.max_z = start.z + kSwampHutDepth - 1;
  } else {
    start.min_x = start.x;
    start.max_x = start.x + kSwampHutDepth - 1;
    start.min_z = start.z;
    start.max_z = start.z + kSwampHutWidth - 1;
  }
  start.min_y = kSwampHutY;
  start.max_y = kSwampHutY + kSwampHutHeight - 1;

  // `ScatteredFeaturePiece.updateAverageGroundHeight(level, chunkBB, 0)`: the
  // mean of the footprint columns' `MOTION_BLOCKING_NO_LEAVES` heights, over
  // the intersection with the decorating chunk. The whole footprint is in the
  // source chunk, so every cell is counted.
  int64_t total = 0;
  int32_t count = 0;
  const int32_t chunk_min_x = chunk_x * 16;
  const int32_t chunk_min_z = chunk_z * 16;
  for (int32_t z = start.min_z; z <= start.max_z; ++z) {
    for (int32_t x = start.min_x; x <= start.max_x; ++x) {
      if (x < chunk_min_x || x >= chunk_min_x + 16 || z < chunk_min_z || z >= chunk_min_z + 16)
        continue;
      total += heights.height(x, z, WgStructureHeightmap::kMotionBlockingNoLeaves);
      ++count;
    }
  }
  if (count == 0)
    return start;
  start.ground_y = static_cast<int32_t>(total / count);
  start.valid = true;
  return start;
}

uint32_t wg_place_swamp_hut(Chunk& chunk, const WgSwampHutStart& start) {
  if (!start.valid)
    return 0;
  SwampHutWriter writer(chunk, start);
  return writer.run();
}

WgIglooStart wg_igloo_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                            WgClimateSampler& sampler, WgStructureHeights& heights) {
  WgIglooStart start;
  const WgStructureSet* set = wg_structure_set_by_name("minecraft:igloos");
  if (set == nullptr || !wg_structure_is_chunk(seed, *set, chunk_x, chunk_z))
    return start;
  const WgStructure* structure = wg_structure_by_name("minecraft:igloo");
  if (structure == nullptr)
    return start;

  const int32_t center_x = chunk_x * 16 + 8;
  const int32_t center_z = chunk_z * 16 + 8;
  const int32_t center_y =
      heights.height(center_x, center_z, WgStructureHeightmap::kWorldSurface) - 1;
  if (!wg_structure_start_biome_is_valid(sampler, *structure, center_x, center_y, center_z))
    return start;

  // `IglooStructure.generatePieces`: one context random, `Rotation.getRandom`
  // then `addPieces`' own draws.
  WgLegacyRandom random(0);
  seed_large_feature(random, seed, chunk_x, chunk_z);
  start.rotation = static_cast<uint32_t>(random.next_int(4));
  start.x = chunk_x * 16;
  start.y = kInitialY;
  start.z = chunk_z * 16;

  const auto add = [&](const char* name, int32_t dx, int32_t dy, int32_t dz) {
    if (start.piece_count >= WgIglooStart::kMaxPieces)
      return;
    const WgPieceTemplate* templ = wg_piece_template_by_name(name);
    if (templ == nullptr)
      return;
    WgIglooPiece& piece = start.pieces[start.piece_count++];
    piece.templ = templ;
    piece.rotation = start.rotation;
    piece.x = start.x + dx;
    piece.y = start.y + dy;
    piece.z = start.z + dz;
  };

  if (random.next_double() < 0.5) {
    const int32_t depth = random.next_int(8) + 4;
    add("minecraft:igloo/bottom", 0, -3 - depth * 3, -2);
    for (int32_t i = 0; i < depth - 1; ++i)
      add("minecraft:igloo/middle", 2, -3 - i * 3, 4);
  }
  add("minecraft:igloo/top", 0, 0, 0);
  if (start.piece_count == 0)
    return start;

  start.valid = true;
  start.structure = structure;
  start.world_seed = seed;
  start.chunk_x = chunk_x;
  start.chunk_z = chunk_z;
  return start;
}

uint32_t wg_place_igloo(Chunk& chunk, const WgIglooStart& start, WgStructureHeights& heights) {
  if (!start.valid)
    return 0;
  const int32_t base_x = chunk.x * 16;
  const int32_t base_z = chunk.z * 16;
  uint32_t changed = 0;
  for (uint32_t i = 0; i < start.piece_count; ++i) {
    const WgIglooPiece& piece = start.pieces[i];
    const WgPieceTemplate& templ = *piece.templ;
    const IglooOffset offset = igloo_offset(templ);
    // `IglooPiece.postProcess`: the entrance is `(3 - offsetX, 0, -offsetZ)`
    // relative to the piece position; the surface height at that world column
    // moves the whole template.
    const XZ entrance =
        rotate_pivot(3 - offset.x, -offset.z, piece.rotation, templ.pivot_x, templ.pivot_z);
    const int32_t entrance_height = heights.height(piece.x + entrance.x, piece.z + entrance.z,
                                                   WgStructureHeightmap::kWorldSurface);
    const int32_t y = piece.y + entrance_height - kInitialY - 1;
    changed += place_template(chunk, templ, piece.rotation, piece.x, y, piece.z, start.world_seed,
                              false, false);

    if (std::strstr(templ.name, "/top") == nullptr)
      continue;
    const XZ trapdoor = rotate_pivot(3, 5, piece.rotation, templ.pivot_x, templ.pivot_z);
    const int32_t trap_x = piece.x + trapdoor.x;
    const int32_t trap_z = piece.z + trapdoor.z;
    if (trap_x < base_x || trap_x >= base_x + 16 || trap_z < base_z || trap_z >= base_z + 16 ||
        y - 1 < kMinY)
      continue;
    const BlockStateKey below = chunk.block_state_at(trap_x - base_x, y - 1, trap_z - base_z);
    if (is_air_state(below) || below.name == "minecraft:ladder")
      continue;
    const BlockStateKey snow{"minecraft:snow_block", ""};
    if (chunk.block_state_at(trap_x - base_x, y, trap_z - base_z) == snow)
      continue;
    if (chunk.set_block_state(trap_x - base_x, y, trap_z - base_z, snow))
      ++changed;
  }
  return changed;
}

WgBuriedTreasureStart wg_buried_treasure_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                               WgClimateSampler& sampler,
                                               WgStructureHeights& heights) {
  WgBuriedTreasureStart start;
  const WgStructureSet* set = wg_structure_set_by_name("minecraft:buried_treasures");
  if (set == nullptr || !wg_structure_is_chunk(seed, *set, chunk_x, chunk_z))
    return start;
  const WgStructure* structure = wg_structure_by_name("minecraft:buried_treasure");
  if (structure == nullptr)
    return start;
  const int32_t center_x = chunk_x * 16 + 8;
  const int32_t center_z = chunk_z * 16 + 8;
  const int32_t center_y =
      heights.height(center_x, center_z, WgStructureHeightmap::kOceanFloor) - 1;
  if (!wg_structure_start_biome_is_valid(sampler, *structure, center_x, center_y, center_z))
    return start;
  start.valid = true;
  start.structure = structure;
  start.world_seed = seed;
  start.chunk_x = chunk_x;
  start.chunk_z = chunk_z;
  start.x = chunk_x * 16 + 9; // `chunkPos.getBlockX(9)`
  start.z = chunk_z * 16 + 9;
  return start;
}

uint32_t wg_place_buried_treasure(Chunk& chunk, const WgBuriedTreasureStart& start,
                                  WgStructureHeights& heights) {
  if (!start.valid)
    return 0;
  const int32_t base_x = chunk.x * 16;
  const int32_t base_z = chunk.z * 16;
  const int32_t local_x = start.x - base_x;
  const int32_t local_z = start.z - base_z;
  if (local_x < 0 || local_x >= 16 || local_z < 0 || local_z >= 16)
    return 0;

  const auto is_hard = [](const std::string& name) {
    return name == "minecraft:sandstone" || name == "minecraft:stone" ||
           name == "minecraft:andesite" || name == "minecraft:granite" ||
           name == "minecraft:diorite";
  };
  uint32_t changed = 0;
  const auto write = [&](int32_t x, int32_t y, int32_t z, const BlockStateKey& state) {
    if (x < 0 || x >= 16 || z < 0 || z >= 16 || y < kMinY || y >= kMinY + kHeight)
      return;
    const BlockStateKey before = chunk.block_state_at(x, y, z);
    if (before == state)
      return;
    if (chunk.set_block_state(x, y, z, state))
      ++changed;
  };

  int32_t y = heights.height(start.x, start.z, WgStructureHeightmap::kOceanFloor);
  while (y > kMinY) {
    const BlockStateKey current = chunk.block_state_at(local_x, y, local_z);
    const BlockStateKey below = chunk.block_state_at(local_x, y - 1, local_z);
    if (!is_hard(below.name)) {
      --y;
      continue;
    }
    const BlockStateKey soft = (is_air_state(current) || is_liquid_state(current))
                                   ? BlockStateKey{"minecraft:sand", ""}
                                   : current;
    // `Direction.values()`: down, up, north, south, west, east. `UP` is the
    // one direction whose relative cell may not be filled from below.
    struct Step {
      int32_t dx, dy, dz;
      bool up;
    };
    static const Step kSteps[6] = {{0, -1, 0, false}, {0, 1, 0, true},   {0, 0, -1, false},
                                   {0, 0, 1, false},  {-1, 0, 0, false}, {1, 0, 0, false}};
    for (const Step& step : kSteps) {
      const int32_t x = local_x + step.dx;
      const int32_t ry = y + step.dy;
      const int32_t z = local_z + step.dz;
      if (x < 0 || x >= 16 || z < 0 || z >= 16 || ry < kMinY || ry >= kMinY + kHeight)
        continue;
      const BlockStateKey relative = chunk.block_state_at(x, ry, z);
      if (!is_air_state(relative) && !is_liquid_state(relative))
        continue;
      const BlockStateKey below_relative = ry - 1 < kMinY ? BlockStateKey{"minecraft:bedrock", ""}
                                                          : chunk.block_state_at(x, ry - 1, z);
      if ((is_air_state(below_relative) || is_liquid_state(below_relative)) && !step.up)
        write(x, ry, z, below);
      else
        write(x, ry, z, soft);
    }
    if (chunk.block_state_at(local_x, y, local_z).name != "minecraft:chest") {
      const char* facing = reoriented_chest_facing(chunk, local_x, y, local_z);
      BlockStateKey chest{"minecraft:chest",
                          std::string("facing=") + facing + ",type=single,waterlogged=false"};
      write(local_x, y, local_z, chest);
      if (chunk.block_state_at(local_x, y, local_z).name == "minecraft:chest")
        add_chest_loot(chunk, start.x, y, start.z, "minecraft:chests/buried_treasure",
                       start.world_seed);
    }
    return changed;
  }
  return changed;
}

namespace {

// ---------------------------------------------------------------------------
// Mineshafts
// ---------------------------------------------------------------------------

constexpr uint8_t kMineshaftNorth = 0;
constexpr uint8_t kMineshaftEast = 1;
constexpr uint8_t kMineshaftSouth = 2;
constexpr uint8_t kMineshaftWest = 3;

WgPieceBox mineshaft_box(int32_t x0, int32_t y0, int32_t z0, int32_t x1, int32_t y1, int32_t z1) {
  return WgPieceBox{x0, y0, z0, x1, y1, z1};
}

bool mineshaft_box_intersects(const WgPieceBox& a, const WgPieceBox& b) {
  return a.min_x <= b.max_x && a.max_x >= b.min_x && a.min_y <= b.max_y && a.max_y >= b.min_y &&
         a.min_z <= b.max_z && a.max_z >= b.min_z;
}

bool mineshaft_has_piece_collision(const WgMineshaftStart& start, const WgPieceBox& box) {
  for (const WgMineshaftPiece& piece : start.pieces) {
    const WgPieceBox other{piece.min_x, piece.min_y, piece.min_z,
                           piece.max_x, piece.max_y, piece.max_z};
    if (mineshaft_box_intersects(box, other))
      return true;
  }
  return false;
}

class MineshaftAssembler {
public:
  MineshaftAssembler(WgMineshaftStart& start_in, WgLegacyRandom& random_in, uint8_t type_in)
      : start(start_in), random(random_in), type(type_in) {}

  int32_t add_piece(const WgMineshaftPiece& piece) {
    start.pieces.push_back(piece);
    return static_cast<int32_t>(start.pieces.size()) - 1;
  }

  int32_t generate_and_add(const WgPieceBox& start_box, int32_t foot_x, int32_t foot_y,
                           int32_t foot_z, uint8_t direction, int32_t depth) {
    if (depth > 8)
      return -1;
    if (std::abs(foot_x - start_box.min_x) > 80 || std::abs(foot_z - start_box.min_z) > 80)
      return -1;
    WgMineshaftPiece piece;
    if (!create_random_piece(piece, foot_x, foot_y, foot_z, direction, depth))
      return -1;
    const int32_t index = add_piece(piece);
    add_children(index, start_box);
    return index;
  }

  void add_children(int32_t index, const WgPieceBox& start_box) {
    if (index < 0 || index >= static_cast<int32_t>(start.pieces.size()))
      return;
    const WgMineshaftPieceKind kind = start.pieces[static_cast<size_t>(index)].kind;
    if (kind == WgMineshaftPieceKind::kRoom) {
      add_room_children(index, start_box);
      return;
    }
    if (kind == WgMineshaftPieceKind::kCorridor) {
      add_corridor_children(index, start_box);
      return;
    }
    if (kind == WgMineshaftPieceKind::kCrossing) {
      add_crossing_children(index, start_box);
      return;
    }
    add_stairs_children(index, start_box);
  }

private:
  bool create_random_piece(WgMineshaftPiece& piece, int32_t foot_x, int32_t foot_y, int32_t foot_z,
                           uint8_t direction, int32_t depth) {
    const int32_t selection = random.next_int(100);
    if (selection >= 80) {
      const int32_t y1 = random.next_int(4) == 0 ? 6 : 2;
      WgPieceBox box;
      switch (direction) {
      case kMineshaftNorth:
        box = mineshaft_box(-1, 0, -4, 3, y1, 0);
        break;
      case kMineshaftSouth:
        box = mineshaft_box(-1, 0, 0, 3, y1, 4);
        break;
      case kMineshaftWest:
        box = mineshaft_box(-4, 0, -1, 0, y1, 3);
        break;
      default:
        box = mineshaft_box(0, 0, -1, 4, y1, 3);
        break;
      }
      box.min_x += foot_x;
      box.max_x += foot_x;
      box.min_y += foot_y;
      box.max_y += foot_y;
      box.min_z += foot_z;
      box.max_z += foot_z;
      if (mineshaft_has_piece_collision(start, box))
        return false;
      piece = WgMineshaftPiece{};
      piece.kind = WgMineshaftPieceKind::kCrossing;
      piece.type = type;
      piece.direction = direction;
      piece.two_floored = static_cast<uint8_t>(box.max_y - box.min_y + 1 > 3);
      piece.gen_depth = static_cast<uint16_t>(depth + 1);
      piece.min_x = box.min_x;
      piece.min_y = box.min_y;
      piece.min_z = box.min_z;
      piece.max_x = box.max_x;
      piece.max_y = box.max_y;
      piece.max_z = box.max_z;
      return true;
    }
    if (selection >= 70) {
      WgPieceBox box;
      switch (direction) {
      case kMineshaftNorth:
        box = mineshaft_box(0, -5, -8, 2, 2, 0);
        break;
      case kMineshaftSouth:
        box = mineshaft_box(0, -5, 0, 2, 2, 8);
        break;
      case kMineshaftWest:
        box = mineshaft_box(-8, -5, 0, 0, 2, 2);
        break;
      default:
        box = mineshaft_box(0, -5, 0, 8, 2, 2);
        break;
      }
      box.min_x += foot_x;
      box.max_x += foot_x;
      box.min_y += foot_y;
      box.max_y += foot_y;
      box.min_z += foot_z;
      box.max_z += foot_z;
      if (mineshaft_has_piece_collision(start, box))
        return false;
      piece = WgMineshaftPiece{};
      piece.kind = WgMineshaftPieceKind::kStairs;
      piece.type = type;
      piece.direction = direction;
      piece.gen_depth = static_cast<uint16_t>(depth + 1);
      piece.min_x = box.min_x;
      piece.min_y = box.min_y;
      piece.min_z = box.min_z;
      piece.max_x = box.max_x;
      piece.max_y = box.max_y;
      piece.max_z = box.max_z;
      return true;
    }

    int32_t corridor_length = random.next_int(3) + 2;
    WgPieceBox box;
    bool found = false;
    while (corridor_length > 0) {
      const int32_t block_length = corridor_length * 5;
      switch (direction) {
      case kMineshaftNorth:
        box = mineshaft_box(0, 0, -(block_length - 1), 2, 2, 0);
        break;
      case kMineshaftSouth:
        box = mineshaft_box(0, 0, 0, 2, 2, block_length - 1);
        break;
      case kMineshaftWest:
        box = mineshaft_box(-(block_length - 1), 0, 0, 0, 2, 2);
        break;
      default:
        box = mineshaft_box(0, 0, 0, block_length - 1, 2, 2);
        break;
      }
      box.min_x += foot_x;
      box.max_x += foot_x;
      box.min_y += foot_y;
      box.max_y += foot_y;
      box.min_z += foot_z;
      box.max_z += foot_z;
      if (!mineshaft_has_piece_collision(start, box)) {
        found = true;
        break;
      }
      --corridor_length;
    }
    if (!found)
      return false;

    piece = WgMineshaftPiece{};
    piece.kind = WgMineshaftPieceKind::kCorridor;
    piece.type = type;
    piece.direction = direction;
    piece.gen_depth = static_cast<uint16_t>(depth + 1);
    piece.has_rails = static_cast<uint8_t>(random.next_int(3) == 0);
    if (!piece.has_rails)
      piece.spider_corridor = static_cast<uint8_t>(random.next_int(23) == 0);
    const bool axis_z = direction == kMineshaftNorth || direction == kMineshaftSouth;
    piece.num_sections = static_cast<uint16_t>(axis_z ? (box.max_z - box.min_z + 1) / 5
                                                      : (box.max_x - box.min_x + 1) / 5);
    piece.min_x = box.min_x;
    piece.min_y = box.min_y;
    piece.min_z = box.min_z;
    piece.max_x = box.max_x;
    piece.max_y = box.max_y;
    piece.max_z = box.max_z;
    return true;
  }

  void add_room_children(int32_t index, const WgPieceBox& start_box) {
    const int32_t room_min_x = start.pieces[static_cast<size_t>(index)].min_x;
    const int32_t room_min_y = start.pieces[static_cast<size_t>(index)].min_y;
    const int32_t room_min_z = start.pieces[static_cast<size_t>(index)].min_z;
    const int32_t room_max_x = start.pieces[static_cast<size_t>(index)].max_x;
    const int32_t room_max_z = start.pieces[static_cast<size_t>(index)].max_z;
    const int32_t span_x = room_max_x - room_min_x + 1;
    const int32_t span_z = room_max_z - room_min_z + 1;
    const int32_t room_height = start.pieces[static_cast<size_t>(index)].max_y - room_min_y + 1;
    int32_t height_space = room_height - 3 - 1;
    if (height_space <= 0)
      height_space = 1;
    std::vector<WgPieceBox> entrances;

    auto add_entrance = [&](int32_t child, bool north, bool south, bool west) {
      if (child < 0)
        return;
      const WgMineshaftPiece& piece = start.pieces[static_cast<size_t>(child)];
      WgPieceBox entrance;
      if (north) {
        entrance = mineshaft_box(piece.min_x, piece.min_y, room_min_z, piece.max_x, piece.max_y,
                                 room_min_z + 1);
      } else if (south) {
        entrance = mineshaft_box(piece.min_x, piece.min_y, room_max_z - 1, piece.max_x, piece.max_y,
                                 room_max_z);
      } else if (west) {
        entrance = mineshaft_box(room_min_x, piece.min_y, piece.min_z, room_min_x + 1, piece.max_y,
                                 piece.max_z);
      } else {
        entrance = mineshaft_box(room_max_x - 1, piece.min_y, piece.min_z, room_max_x, piece.max_y,
                                 piece.max_z);
      }
      entrances.push_back(entrance);
    };

    for (int32_t pos = 0; pos < span_x;) {
      pos += random.next_int(span_x);
      if (pos + 3 > span_x)
        break;
      const int32_t child = generate_and_add(
          start_box, room_min_x + pos, room_min_y + random.next_int(height_space) + 1,
          room_min_z - 1, kMineshaftNorth, start.pieces[static_cast<size_t>(index)].gen_depth);
      add_entrance(child, true, false, false);
      pos += 4;
    }
    for (int32_t pos = 0; pos < span_x;) {
      pos += random.next_int(span_x);
      if (pos + 3 > span_x)
        break;
      const int32_t child = generate_and_add(
          start_box, room_min_x + pos, room_min_y + random.next_int(height_space) + 1,
          room_max_z + 1, kMineshaftSouth, start.pieces[static_cast<size_t>(index)].gen_depth);
      add_entrance(child, false, true, false);
      pos += 4;
    }
    for (int32_t pos = 0; pos < span_z;) {
      pos += random.next_int(span_z);
      if (pos + 3 > span_z)
        break;
      const int32_t child = generate_and_add(
          start_box, room_min_x - 1, room_min_y + random.next_int(height_space) + 1,
          room_min_z + pos, kMineshaftWest, start.pieces[static_cast<size_t>(index)].gen_depth);
      add_entrance(child, false, false, true);
      pos += 4;
    }
    for (int32_t pos = 0; pos < span_z;) {
      pos += random.next_int(span_z);
      if (pos + 3 > span_z)
        break;
      const int32_t child = generate_and_add(
          start_box, room_max_x + 1, room_min_y + random.next_int(height_space) + 1,
          room_min_z + pos, kMineshaftEast, start.pieces[static_cast<size_t>(index)].gen_depth);
      add_entrance(child, false, false, false);
      pos += 4;
    }
    auto& room = start.pieces[static_cast<size_t>(index)];
    room.entrances.insert(room.entrances.end(), entrances.begin(), entrances.end());
  }

  void add_corridor_children(int32_t index, const WgPieceBox& start_box) {
    const WgMineshaftPiece piece = start.pieces[static_cast<size_t>(index)];
    const int32_t depth = piece.gen_depth;
    const int32_t end_selection = random.next_int(4);
    const uint8_t orientation = piece.direction;
    const int32_t min_x = piece.min_x;
    const int32_t min_y = piece.min_y;
    const int32_t min_z = piece.min_z;
    const int32_t max_x = piece.max_x;
    const int32_t max_z = piece.max_z;
    switch (orientation) {
    case kMineshaftNorth:
      if (end_selection <= 1) {
        generate_and_add(start_box, min_x, min_y - 1 + random.next_int(3), min_z - 1, orientation,
                         depth);
      } else if (end_selection == 2) {
        generate_and_add(start_box, min_x - 1, min_y - 1 + random.next_int(3), min_z,
                         kMineshaftWest, depth);
      } else {
        generate_and_add(start_box, max_x + 1, min_y - 1 + random.next_int(3), min_z,
                         kMineshaftEast, depth);
      }
      break;
    case kMineshaftSouth:
      if (end_selection <= 1) {
        generate_and_add(start_box, min_x, min_y - 1 + random.next_int(3), max_z + 1, orientation,
                         depth);
      } else if (end_selection == 2) {
        generate_and_add(start_box, min_x - 1, min_y - 1 + random.next_int(3), max_z - 3,
                         kMineshaftWest, depth);
      } else {
        generate_and_add(start_box, max_x + 1, min_y - 1 + random.next_int(3), max_z - 3,
                         kMineshaftEast, depth);
      }
      break;
    case kMineshaftWest:
      if (end_selection <= 1) {
        generate_and_add(start_box, min_x - 1, min_y - 1 + random.next_int(3), min_z, orientation,
                         depth);
      } else if (end_selection == 2) {
        generate_and_add(start_box, min_x, min_y - 1 + random.next_int(3), min_z - 1,
                         kMineshaftNorth, depth);
      } else {
        generate_and_add(start_box, min_x, min_y - 1 + random.next_int(3), max_z + 1,
                         kMineshaftSouth, depth);
      }
      break;
    default:
      if (end_selection <= 1) {
        generate_and_add(start_box, max_x + 1, min_y - 1 + random.next_int(3), min_z, orientation,
                         depth);
      } else if (end_selection == 2) {
        generate_and_add(start_box, max_x - 3, min_y - 1 + random.next_int(3), min_z - 1,
                         kMineshaftNorth, depth);
      } else {
        generate_and_add(start_box, max_x - 3, min_y - 1 + random.next_int(3), max_z + 1,
                         kMineshaftSouth, depth);
      }
      break;
    }
    if (depth >= 8)
      return;
    if (orientation == kMineshaftNorth || orientation == kMineshaftSouth) {
      for (int32_t z = min_z + 3; z + 3 <= max_z; z += 5) {
        const int32_t selection = random.next_int(5);
        if (selection == 0)
          generate_and_add(start_box, min_x - 1, min_y, z, kMineshaftWest, depth + 1);
        else if (selection == 1)
          generate_and_add(start_box, max_x + 1, min_y, z, kMineshaftEast, depth + 1);
      }
    } else {
      for (int32_t x = min_x + 3; x + 3 <= max_x; x += 5) {
        const int32_t selection = random.next_int(5);
        if (selection == 0)
          generate_and_add(start_box, x, min_y, min_z - 1, kMineshaftNorth, depth + 1);
        else if (selection == 1)
          generate_and_add(start_box, x, min_y, max_z + 1, kMineshaftSouth, depth + 1);
      }
    }
  }

  void add_crossing_children(int32_t index, const WgPieceBox& start_box) {
    const WgMineshaftPiece piece = start.pieces[static_cast<size_t>(index)];
    const int32_t depth = piece.gen_depth;
    const int32_t min_x = piece.min_x;
    const int32_t min_y = piece.min_y;
    const int32_t min_z = piece.min_z;
    const int32_t max_x = piece.max_x;
    const int32_t max_z = piece.max_z;
    switch (piece.direction) {
    case kMineshaftNorth:
      generate_and_add(start_box, min_x + 1, min_y, min_z - 1, kMineshaftNorth, depth);
      generate_and_add(start_box, min_x - 1, min_y, min_z + 1, kMineshaftWest, depth);
      generate_and_add(start_box, max_x + 1, min_y, min_z + 1, kMineshaftEast, depth);
      break;
    case kMineshaftSouth:
      generate_and_add(start_box, min_x + 1, min_y, max_z + 1, kMineshaftSouth, depth);
      generate_and_add(start_box, min_x - 1, min_y, min_z + 1, kMineshaftWest, depth);
      generate_and_add(start_box, max_x + 1, min_y, min_z + 1, kMineshaftEast, depth);
      break;
    case kMineshaftWest:
      generate_and_add(start_box, min_x + 1, min_y, min_z - 1, kMineshaftNorth, depth);
      generate_and_add(start_box, min_x + 1, min_y, max_z + 1, kMineshaftSouth, depth);
      generate_and_add(start_box, min_x - 1, min_y, min_z + 1, kMineshaftWest, depth);
      break;
    default:
      generate_and_add(start_box, min_x + 1, min_y, min_z - 1, kMineshaftNorth, depth);
      generate_and_add(start_box, min_x + 1, min_y, max_z + 1, kMineshaftSouth, depth);
      generate_and_add(start_box, max_x + 1, min_y, min_z + 1, kMineshaftEast, depth);
      break;
    }
    if (!piece.two_floored)
      return;
    if (random.next_boolean())
      generate_and_add(start_box, min_x + 1, min_y + 4, min_z - 1, kMineshaftNorth, depth);
    if (random.next_boolean())
      generate_and_add(start_box, min_x - 1, min_y + 4, min_z + 1, kMineshaftWest, depth);
    if (random.next_boolean())
      generate_and_add(start_box, max_x + 1, min_y + 4, min_z + 1, kMineshaftEast, depth);
    if (random.next_boolean())
      generate_and_add(start_box, min_x + 1, min_y + 4, max_z + 1, kMineshaftSouth, depth);
  }

  void add_stairs_children(int32_t index, const WgPieceBox& start_box) {
    const WgMineshaftPiece piece = start.pieces[static_cast<size_t>(index)];
    const int32_t depth = piece.gen_depth;
    switch (piece.direction) {
    case kMineshaftNorth:
      generate_and_add(start_box, piece.min_x, piece.min_y, piece.min_z - 1, kMineshaftNorth,
                       depth);
      break;
    case kMineshaftSouth:
      generate_and_add(start_box, piece.min_x, piece.min_y, piece.max_z + 1, kMineshaftSouth,
                       depth);
      break;
    case kMineshaftWest:
      generate_and_add(start_box, piece.min_x - 1, piece.min_y, piece.min_z, kMineshaftWest, depth);
      break;
    default:
      generate_and_add(start_box, piece.max_x + 1, piece.min_y, piece.min_z, kMineshaftEast, depth);
      break;
    }
  }

  WgMineshaftStart& start;
  WgLegacyRandom& random;
  uint8_t type;
};

void mineshaft_offset_pieces(WgMineshaftStart& start, int32_t dy) {
  for (WgMineshaftPiece& piece : start.pieces) {
    piece.min_y += dy;
    piece.max_y += dy;
    for (WgPieceBox& entrance : piece.entrances) {
      entrance.min_y += dy;
      entrance.max_y += dy;
    }
  }
}

int32_t mineshaft_move_below_sea_level(WgMineshaftStart& start, WgLegacyRandom& random,
                                       int32_t sea_level, int32_t min_y, int32_t max_distance) {
  if (start.pieces.empty())
    return 0;
  int32_t box_min_y = start.pieces[0].min_y;
  int32_t box_max_y = start.pieces[0].max_y;
  for (const WgMineshaftPiece& piece : start.pieces) {
    box_min_y = std::min(box_min_y, piece.min_y);
    box_max_y = std::max(box_max_y, piece.max_y);
  }
  const int32_t target = sea_level - max_distance;
  int32_t top = (box_max_y - box_min_y + 1) + min_y + 1;
  if (top < target)
    top += random.next_int(target - top);
  const int32_t dy = top - box_max_y;
  mineshaft_offset_pieces(start, dy);
  return dy;
}

// The writer's identity/oriented split is vanilla's own: `MineShaftRoom` and
// `MineShaftCrossing` never call `setOrientation`, so their local coordinates
// are world coordinates. `MineShaftCorridor` and `MineShaftStairs` do, and the
// writer adds the box origin and applies the mirror/rotation pair.
struct WgMineshaftChest {
  int32_t x = 0;
  int32_t y = 0;
  int32_t z = 0;
  int64_t loot_seed = 0;
};

class MineshaftWriter {
public:
  MineshaftWriter(Chunk& chunk_in, const WgMineshaftPiece& piece_in, WgStructureHeights& heights_in,
                  WgClimateSampler& sampler_in, std::vector<WgMineshaftChest>& chests_in)
      : chunk(chunk_in), piece(piece_in), heights(heights_in), sampler(sampler_in),
        chests(chests_in), random(wg_mth_get_seed(piece_in.min_x, piece_in.min_y, piece_in.min_z)) {
    // `StructureStart.placeInChunk` passes the chunk's writable area, not the
    // piece's own box. `placeBlock`/`getBlock` are therefore free to touch any
    // cell of the target chunk — the corridor's floor is one cell below its
    // box, and a support's chain can climb above it.
    const int32_t base_x = chunk.x * 16;
    const int32_t base_z = chunk.z * 16;
    clip.min_x = base_x;
    clip.max_x = base_x + 15;
    clip.min_y = kMinY;
    clip.max_y = kMinY + kHeight - 1;
    clip.min_z = base_z;
    clip.max_z = base_z + 15;
  }

  uint32_t run() {
    switch (piece.kind) {
    case WgMineshaftPieceKind::kRoom:
      return run_room();
    case WgMineshaftPieceKind::kCorridor:
      return run_corridor();
    case WgMineshaftPieceKind::kCrossing:
      return run_crossing();
    default:
      return run_stairs();
    }
  }

  uint32_t changed() const {
    return changed_;
  }

private:
  bool oriented() const {
    return piece.kind == WgMineshaftPieceKind::kCorridor ||
           piece.kind == WgMineshaftPieceKind::kStairs;
  }

  bool identity_piece() const {
    return !oriented();
  }

  void world_xz(int32_t local_x, int32_t local_z, int32_t& world_x, int32_t& world_z) const {
    if (identity_piece()) {
      world_x = local_x;
      world_z = local_z;
      return;
    }
    switch (piece.direction) {
    case kMineshaftNorth:
      world_x = piece.min_x + local_x;
      world_z = piece.max_z - local_z;
      break;
    case kMineshaftSouth:
      world_x = piece.min_x + local_x;
      world_z = piece.min_z + local_z;
      break;
    case kMineshaftWest:
      world_x = piece.max_x - local_z;
      world_z = piece.min_z + local_x;
      break;
    default:
      world_x = piece.min_x + local_z;
      world_z = piece.min_z + local_x;
      break;
    }
  }

  int32_t world_y(int32_t local_y) const {
    return identity_piece() ? local_y : piece.min_y + local_y;
  }

  bool inside_clip(int32_t world_x, int32_t world_y, int32_t world_z) const {
    return world_x >= clip.min_x && world_x <= clip.max_x && world_y >= clip.min_y &&
           world_y <= clip.max_y && world_z >= clip.min_z && world_z <= clip.max_z;
  }

  bool inside_chunk(int32_t world_x, int32_t world_y, int32_t world_z) const {
    const int32_t base_x = chunk.x * 16;
    const int32_t base_z = chunk.z * 16;
    return world_x >= base_x && world_x < base_x + 16 && world_z >= base_z &&
           world_z < base_z + 16 && world_y >= kMinY && world_y < kMinY + kHeight;
  }

  BlockStateKey get_block(int32_t local_x, int32_t local_y, int32_t local_z) const {
    int32_t world_x = 0;
    int32_t world_z = 0;
    world_xz(local_x, local_z, world_x, world_z);
    const int32_t wy = world_y(local_y);
    if (!inside_clip(world_x, wy, world_z))
      return BlockStateKey{"minecraft:air", ""};
    return chunk.block_state_at(world_x - chunk.x * 16, wy, world_z - chunk.z * 16);
  }

  BlockStateKey get_world_block(int32_t world_x, int32_t world_y, int32_t world_z) const {
    if (!inside_chunk(world_x, world_y, world_z))
      return BlockStateKey{"minecraft:air", ""};
    return chunk.block_state_at(world_x - chunk.x * 16, world_y, world_z - chunk.z * 16);
  }

  BlockStateKey transform(BlockStateKey state) const {
    if (identity_piece())
      return state;
    if (orientation_mirrors(piece.direction))
      state.properties = mirror_left_right(state.properties);
    state.properties =
        wg_piece_rotate_properties(state.properties, orientation_rotation(piece.direction));
    return state;
  }

  bool can_replace(const BlockStateKey& state) const {
    return state.name != planks().name && state.name != log().name && state.name != fence().name &&
           state.name != "minecraft:iron_chain";
  }

  uint32_t set_world_block(int32_t world_x, int32_t world_y, int32_t world_z,
                           const BlockStateKey& state) {
    if (!inside_chunk(world_x, world_y, world_z))
      return 0;
    const int32_t local_x = world_x - chunk.x * 16;
    const int32_t local_z = world_z - chunk.z * 16;
    if (chunk.block_state_at(local_x, world_y, local_z) == state)
      return 0;
    return chunk.set_block_state(local_x, world_y, local_z, state) ? 1u : 0u;
  }

  void place_block(const BlockStateKey& state, int32_t local_x, int32_t local_y, int32_t local_z) {
    int32_t world_x = 0;
    int32_t world_z = 0;
    world_xz(local_x, local_z, world_x, world_z);
    const int32_t wy = world_y(local_y);
    if (!inside_clip(world_x, wy, world_z) || !can_replace(get_block(local_x, local_y, local_z)))
      return;
    changed_ += set_world_block(world_x, wy, world_z, transform(state));
  }

  void place_raw(const BlockStateKey& state, int32_t local_x, int32_t local_y, int32_t local_z) {
    int32_t world_x = 0;
    int32_t world_z = 0;
    world_xz(local_x, local_z, world_x, world_z);
    changed_ += set_world_block(world_x, world_y(local_y), world_z, state);
  }

  void generate_box(int32_t x0, int32_t y0, int32_t z0, int32_t x1, int32_t y1, int32_t z1,
                    const BlockStateKey& edge, const BlockStateKey& fill, bool skip_air = false) {
    for (int32_t y = y0; y <= y1; ++y)
      for (int32_t x = x0; x <= x1; ++x)
        for (int32_t z = z0; z <= z1; ++z) {
          if (skip_air && is_air_state(get_block(x, y, z)))
            continue;
          const bool is_edge = y == y0 || y == y1 || x == x0 || x == x1 || z == z0 || z == z1;
          place_block(is_edge ? edge : fill, x, y, z);
        }
  }

  void generate_maybe_box(float probability, int32_t x0, int32_t y0, int32_t z0, int32_t x1,
                          int32_t y1, int32_t z1, const BlockStateKey& edge,
                          const BlockStateKey& fill, bool skip_air, bool has_to_be_inside) {
    for (int32_t y = y0; y <= y1; ++y)
      for (int32_t x = x0; x <= x1; ++x)
        for (int32_t z = z0; z <= z1; ++z) {
          if (random.next_float() > probability)
            continue;
          if (skip_air && is_air_state(get_block(x, y, z)))
            continue;
          if (has_to_be_inside && !is_interior(x, y, z))
            continue;
          const bool is_edge = y == y0 || y == y1 || x == x0 || x == x1 || z == z0 || z == z1;
          place_block(is_edge ? edge : fill, x, y, z);
        }
  }

  void maybe_generate_block(float probability, int32_t x, int32_t y, int32_t z,
                            const BlockStateKey& state) {
    if (random.next_float() < probability)
      place_block(state, x, y, z);
  }

  void generate_upper_half_sphere(int32_t x0, int32_t y0, int32_t z0, int32_t x1, int32_t y1,
                                  int32_t z1, const BlockStateKey& fill, bool skip_air) {
    const float diag_x = static_cast<float>(x1 - x0 + 1);
    const float diag_y = static_cast<float>(y1 - y0 + 1);
    const float diag_z = static_cast<float>(z1 - z0 + 1);
    const float center_x = static_cast<float>(x0) + diag_x / 2.0f;
    const float center_z = static_cast<float>(z0) + diag_z / 2.0f;
    for (int32_t y = y0; y <= y1; ++y) {
      const float normalized_y = static_cast<float>(y - y0) / diag_y;
      for (int32_t x = x0; x <= x1; ++x) {
        const float normalized_x = (static_cast<float>(x) - center_x) / (diag_x * 0.5f);
        for (int32_t z = z0; z <= z1; ++z) {
          const float normalized_z = (static_cast<float>(z) - center_z) / (diag_z * 0.5f);
          if (skip_air && is_air_state(get_block(x, y, z)))
            continue;
          const float dist = normalized_x * normalized_x + normalized_y * normalized_y +
                             normalized_z * normalized_z;
          if (dist <= 1.05f)
            place_block(fill, x, y, z);
        }
      }
    }
  }

  bool is_interior(int32_t local_x, int32_t local_y, int32_t local_z) const {
    int32_t world_x = 0;
    int32_t world_z = 0;
    world_xz(local_x, local_z, world_x, world_z);
    const int32_t wy = world_y(local_y) + 1;
    if (!inside_clip(world_x, wy, world_z))
      return false;
    return wy < heights.height(world_x, world_z, WgStructureHeightmap::kOceanFloor);
  }

  bool is_invalid_location() const {
    const int32_t x0 = std::max(piece.min_x - 1, clip.min_x);
    const int32_t y0 = std::max(piece.min_y - 1, clip.min_y);
    const int32_t z0 = std::max(piece.min_z - 1, clip.min_z);
    const int32_t x1 = std::min(piece.max_x + 1, clip.max_x);
    const int32_t y1 = std::min(piece.max_y + 1, clip.max_y);
    const int32_t z1 = std::min(piece.max_z + 1, clip.max_z);
    if (x0 > x1 || y0 > y1 || z0 > z1)
      return false;
    const int32_t mid_x = (x0 + x1) / 2;
    const int32_t mid_y = (y0 + y1) / 2;
    const int32_t mid_z = (z0 + z1) / 2;
    if (std::strcmp(sampler.biome_at_quart(mid_x >> 2, mid_y >> 2, mid_z >> 2),
                    "minecraft:deep_dark") == 0)
      return true;
    for (int32_t x = x0; x <= x1; ++x) {
      for (int32_t z = z0; z <= z1; ++z) {
        if (is_liquid_state(get_world_block(x, y0, z)))
          return true;
        if (is_liquid_state(get_world_block(x, y1, z)))
          return true;
      }
    }
    for (int32_t x = x0; x <= x1; ++x) {
      for (int32_t y = y0; y <= y1; ++y) {
        if (is_liquid_state(get_world_block(x, y, z0)))
          return true;
        if (is_liquid_state(get_world_block(x, y, z1)))
          return true;
      }
    }
    for (int32_t z = z0; z <= z1; ++z) {
      for (int32_t y = y0; y <= y1; ++y) {
        if (is_liquid_state(get_world_block(x0, y, z)))
          return true;
        if (is_liquid_state(get_world_block(x1, y, z)))
          return true;
      }
    }
    return false;
  }

  uint32_t run_room() {
    if (is_invalid_location())
      return changed_;
    generate_box(piece.min_x, piece.min_y + 1, piece.min_z, piece.max_x,
                 std::min(piece.min_y + 3, piece.max_y), piece.max_z, cave_air(), cave_air());
    for (const WgPieceBox& entrance : piece.entrances) {
      generate_box(entrance.min_x, entrance.max_y - 2, entrance.min_z, entrance.max_x,
                   entrance.max_y, entrance.max_z, cave_air(), cave_air());
    }
    generate_upper_half_sphere(piece.min_x, piece.min_y + 4, piece.min_z, piece.max_x, piece.max_y,
                               piece.max_z, cave_air(), false);
    return changed_;
  }

  uint32_t run_crossing() {
    if (is_invalid_location())
      return changed_;
    if (piece.two_floored) {
      generate_box(piece.min_x + 1, piece.min_y, piece.min_z, piece.max_x - 1, piece.min_y + 3 - 1,
                   piece.max_z, cave_air(), cave_air());
      generate_box(piece.min_x, piece.min_y, piece.min_z + 1, piece.max_x, piece.min_y + 3 - 1,
                   piece.max_z - 1, cave_air(), cave_air());
      generate_box(piece.min_x + 1, piece.max_y - 2, piece.min_z, piece.max_x - 1, piece.max_y,
                   piece.max_z, cave_air(), cave_air());
      generate_box(piece.min_x, piece.max_y - 2, piece.min_z + 1, piece.max_x, piece.max_y,
                   piece.max_z - 1, cave_air(), cave_air());
      generate_box(piece.min_x + 1, piece.min_y + 3, piece.min_z + 1, piece.max_x - 1,
                   piece.min_y + 3, piece.max_z - 1, cave_air(), cave_air());
    } else {
      generate_box(piece.min_x + 1, piece.min_y, piece.min_z, piece.max_x - 1, piece.max_y,
                   piece.max_z, cave_air(), cave_air());
      generate_box(piece.min_x, piece.min_y, piece.min_z + 1, piece.max_x, piece.max_y,
                   piece.max_z - 1, cave_air(), cave_air());
    }
    place_support_pillar(piece.min_x + 1, piece.min_y, piece.min_z + 1, piece.max_y);
    place_support_pillar(piece.min_x + 1, piece.min_y, piece.max_z - 1, piece.max_y);
    place_support_pillar(piece.max_x - 1, piece.min_y, piece.min_z + 1, piece.max_y);
    place_support_pillar(piece.max_x - 1, piece.min_y, piece.max_z - 1, piece.max_y);
    const int32_t floor_y = piece.min_y - 1;
    for (int32_t x = piece.min_x; x <= piece.max_x; ++x) {
      for (int32_t z = piece.min_z; z <= piece.max_z; ++z)
        set_planks_block(x, floor_y, z);
    }
    return changed_;
  }

  void place_support_pillar(int32_t world_x, int32_t y0, int32_t world_z, int32_t y1) {
    if (!is_air_state(get_world_block(world_x, y1 + 1, world_z))) {
      generate_box(world_x, y0, world_z, world_x, y1, world_z, planks(), cave_air());
    }
  }

  uint32_t run_stairs() {
    if (is_invalid_location())
      return changed_;
    generate_box(0, 5, 0, 2, 7, 1, cave_air(), cave_air());
    generate_box(0, 0, 7, 2, 2, 8, cave_air(), cave_air());
    for (int32_t i = 0; i < 5; ++i) {
      generate_box(0, 5 - i - (i < 4 ? 1 : 0), 2 + i, 2, 7 - i, 2 + i, cave_air(), cave_air());
    }
    return changed_;
  }

  uint32_t run_corridor() {
    if (is_invalid_location())
      return changed_;
    const int32_t length = static_cast<int32_t>(piece.num_sections) * 5 - 1;
    generate_box(0, 0, 0, 2, 1, length, cave_air(), cave_air());
    generate_maybe_box(0.8f, 0, 2, 0, 2, 2, length, cave_air(), cave_air(), false, false);
    if (piece.spider_corridor)
      generate_maybe_box(0.6f, 0, 0, 0, 2, 1, length, cobweb(), cave_air(), false, true);

    bool has_placed_spider = false;
    for (int32_t section = 0; section < piece.num_sections; ++section) {
      const int32_t z = 2 + section * 5;
      place_support(0, 0, z, 2, 2);
      maybe_place_cobweb(0.1f, 0, 2, z - 1);
      maybe_place_cobweb(0.1f, 2, 2, z - 1);
      maybe_place_cobweb(0.1f, 0, 2, z + 1);
      maybe_place_cobweb(0.1f, 2, 2, z + 1);
      maybe_place_cobweb(0.05f, 0, 2, z - 2);
      maybe_place_cobweb(0.05f, 2, 2, z - 2);
      maybe_place_cobweb(0.05f, 0, 2, z + 2);
      maybe_place_cobweb(0.05f, 2, 2, z + 2);
      if (random.next_int(100) == 0)
        create_chest(2, 0, z - 1);
      if (random.next_int(100) == 0)
        create_chest(0, 0, z + 1);
      if (!piece.spider_corridor || has_placed_spider)
        continue;
      const int32_t new_z = z - 1 + random.next_int(3);
      int32_t world_x = 0;
      int32_t world_z = 0;
      world_xz(1, new_z, world_x, world_z);
      const int32_t wy = world_y(0);
      if (!inside_clip(world_x, wy, world_z) || !is_interior(1, 0, new_z))
        continue;
      has_placed_spider = true;
      place_raw(spawner(), 1, 0, new_z);
      add_spawner_block_entity(world_x, wy, world_z);
    }
    for (int32_t x = 0; x <= 2; ++x)
      for (int32_t z = 0; z <= length; ++z)
        set_planks_block(x, -1, z);
    place_double_lower_or_upper(0, -1, 2);
    if (piece.num_sections > 1)
      place_double_lower_or_upper(0, -1, length - 2);
    if (piece.has_rails) {
      for (int32_t z = 0; z <= length; ++z) {
        const BlockStateKey floor = get_block(1, -1, z);
        if (is_air_state(floor) || !block_blocks_motion(floor.name))
          continue;
        const float probability = is_interior(1, 0, z) ? 0.7f : 0.9f;
        maybe_generate_block(probability, 1, 0, z, rail_north_south());
      }
    }
    return changed_;
  }

  void place_support(int32_t x0, int32_t y0, int32_t z, int32_t y1, int32_t x1) {
    if (!is_supporting_box(x0, x1, y1, z))
      return;
    generate_box(x0, y0, z, x0, y1 - 1, z, fence_west(), cave_air());
    generate_box(x1, y0, z, x1, y1 - 1, z, fence_east(), cave_air());
    if (random.next_int(4) == 0) {
      generate_box(x0, y1, z, x0, y1, z, planks(), cave_air());
      generate_box(x1, y1, z, x1, y1, z, planks(), cave_air());
    } else {
      generate_box(x0, y1, z, x1, y1, z, planks(), cave_air());
      maybe_generate_block(0.05f, x0 + 1, y1, z - 1, wall_torch_south());
      maybe_generate_block(0.05f, x0 + 1, y1, z + 1, wall_torch_north());
    }
  }

  bool is_supporting_box(int32_t x0, int32_t x1, int32_t y1, int32_t z0) const {
    for (int32_t x = x0; x <= x1; ++x) {
      if (!is_air_state(get_block(x, y1 + 1, z0)))
        continue;
      return false;
    }
    return true;
  }

  void maybe_place_cobweb(float probability, int32_t x, int32_t y, int32_t z) {
    if (is_interior(x, y, z) && random.next_float() < probability &&
        has_sturdy_neighbours(x, y, z, 2))
      place_block(cobweb(), x, y, z);
  }

  bool has_sturdy_neighbours(int32_t local_x, int32_t local_y, int32_t local_z,
                             int32_t count) const {
    int32_t world_x = 0;
    int32_t world_z = 0;
    world_xz(local_x, local_z, world_x, world_z);
    const int32_t wy = world_y(local_y);
    static constexpr int32_t kDx[6] = {0, 0, 0, 0, -1, 1};
    static constexpr int32_t kDy[6] = {-1, 1, 0, 0, 0, 0};
    static constexpr int32_t kDz[6] = {0, 0, -1, 1, 0, 0};
    int32_t sturdy = 0;
    for (int32_t i = 0; i < 6; ++i) {
      const int32_t x = world_x + kDx[i];
      const int32_t y = wy + kDy[i];
      const int32_t z = world_z + kDz[i];
      if (inside_clip(x, y, z) && block_blocks_motion(get_world_block(x, y, z).name) &&
          ++sturdy >= count)
        return true;
    }
    return false;
  }

  void set_planks_block(int32_t local_x, int32_t local_y, int32_t local_z) {
    if (!is_interior(local_x, local_y, local_z))
      return;
    int32_t world_x = 0;
    int32_t world_z = 0;
    world_xz(local_x, local_z, world_x, world_z);
    const int32_t wy = world_y(local_y);
    if (!block_blocks_motion(get_world_block(world_x, wy, world_z).name))
      changed_ += set_world_block(world_x, wy, world_z, planks());
  }

  void place_double_lower_or_upper(int32_t x, int32_t y, int32_t z) {
    if (get_block(x, y, z).name == planks().name)
      fill_pillar_down_or_chain_up(x, y, z);
    if (get_block(x + 2, y, z).name == planks().name)
      fill_pillar_down_or_chain_up(x + 2, y, z);
  }

  void fill_pillar_down_or_chain_up(int32_t local_x, int32_t local_y, int32_t local_z) {
    int32_t world_x = 0;
    int32_t world_z = 0;
    world_xz(local_x, local_z, world_x, world_z);
    const int32_t world_y0 = world_y(local_y);
    if (!inside_clip(world_x, world_y0, world_z))
      return;
    int32_t distance = 1;
    bool check_below = true;
    bool check_above = true;
    while (check_below || check_above) {
      if (check_below) {
        const int32_t y = world_y0 - distance;
        const BlockStateKey below = get_world_block(world_x, y, world_z);
        const bool empty_below =
            is_replaceable_by_structures(below) && below.name != "minecraft:lava";
        if (!empty_below && block_blocks_motion(below.name)) {
          fill_column_between(log(), world_x, y + 1, world_y0, world_z);
          return;
        }
        check_below = distance <= 20 && empty_below && y > kMinY + 1;
      }
      if (check_above) {
        const int32_t y = world_y0 + distance;
        const BlockStateKey above = get_world_block(world_x, y, world_z);
        const bool empty_above = is_replaceable_by_structures(above);
        if (!empty_above && block_blocks_motion(above.name) && !is_falling_block(above.name)) {
          changed_ += set_world_block(world_x, world_y0 + 1, world_z, fence());
          fill_column_between(iron_chain(), world_x, world_y0 + 2, world_y0 + distance, world_z);
          return;
        }
        check_above = distance <= 50 && empty_above && y < kMinY + kHeight - 1;
      }
      ++distance;
    }
  }

  void fill_column_between(const BlockStateKey& state, int32_t world_x, int32_t bottom, int32_t top,
                           int32_t world_z) {
    for (int32_t y = bottom; y < top; ++y)
      changed_ += set_world_block(world_x, y, world_z, state);
  }

  void create_chest(int32_t local_x, int32_t local_y, int32_t local_z) {
    int32_t world_x = 0;
    int32_t world_z = 0;
    world_xz(local_x, local_z, world_x, world_z);
    const int32_t wy = world_y(local_y);
    if (!inside_clip(world_x, wy, world_z))
      return;
    const BlockStateKey current = get_world_block(world_x, wy, world_z);
    // `createChest` draws only once the cell and the one below it have passed:
    // a repeat pass over a finished chest therefore consumes no randomness.
    if (!is_air_state(current))
      return;
    if (is_air_state(get_world_block(world_x, wy - 1, world_z)))
      return;
    const bool north_south = random.next_boolean();
    const int64_t loot_seed = random.next_long();
    // Vanilla writes the rail and adds a `chest_minecart`; the port writes only
    // the rail here, then stands a chest at the same cell after every piece has
    // run (see `wg_place_mineshaft`). Keeping the cell a rail during the pass
    // matters: later pieces read it through `getBlock`/`canBeReplaced`.
    place_block(north_south ? rail_north_south() : rail_east_west(), local_x, local_y, local_z);
    chests.push_back(WgMineshaftChest{world_x, wy, world_z, loot_seed});
  }

  void add_spawner_block_entity(int32_t world_x, int32_t world_y, int32_t world_z) {
    for (const nbt::Value& be : chunk.block_entities)
      if (block_entity_matches(be, world_x, world_y, world_z))
        return;
    using namespace nbt;
    nbt::Value entity = make_compound(
        std::vector<std::pair<std::string, Value>>{{"id", make_string("minecraft:cave_spider")}});
    nbt::Value spawn_data =
        make_compound(std::vector<std::pair<std::string, Value>>{{"entity", std::move(entity)}});
    chunk.block_entities.push_back(make_compound(
        std::vector<std::pair<std::string, Value>>{{"id", make_string("minecraft:spawner")},
                                                   {"x", make_int(world_x)},
                                                   {"y", make_int(world_y)},
                                                   {"z", make_int(world_z)},
                                                   {"SpawnData", std::move(spawn_data)}}));
  }

  BlockStateKey cave_air() const {
    return BlockStateKey{"minecraft:cave_air", ""};
  }
  BlockStateKey planks() const {
    return BlockStateKey{piece.type == 1 ? "minecraft:dark_oak_planks" : "minecraft:oak_planks",
                         ""};
  }
  BlockStateKey log() const {
    return BlockStateKey{piece.type == 1 ? "minecraft:dark_oak_log" : "minecraft:oak_log",
                         "axis=y"};
  }
  BlockStateKey fence() const {
    return BlockStateKey{piece.type == 1 ? "minecraft:dark_oak_fence" : "minecraft:oak_fence",
                         "east=false,north=false,south=false,waterlogged=false,west=false"};
  }
  BlockStateKey fence_west() const {
    return BlockStateKey{fence().name,
                         "east=false,north=false,south=false,waterlogged=false,west=true"};
  }
  BlockStateKey fence_east() const {
    return BlockStateKey{fence().name,
                         "east=true,north=false,south=false,waterlogged=false,west=false"};
  }
  static BlockStateKey cobweb() {
    return BlockStateKey{"minecraft:cobweb", ""};
  }
  static BlockStateKey spawner() {
    return BlockStateKey{"minecraft:spawner", ""};
  }
  static BlockStateKey iron_chain() {
    return BlockStateKey{"minecraft:iron_chain", "axis=y,waterlogged=false"};
  }
  static BlockStateKey rail_north_south() {
    return BlockStateKey{"minecraft:rail", "shape=north_south,waterlogged=false"};
  }
  static BlockStateKey rail_east_west() {
    return BlockStateKey{"minecraft:rail", "shape=east_west,waterlogged=false"};
  }
  static BlockStateKey wall_torch_north() {
    return BlockStateKey{"minecraft:wall_torch", "facing=north"};
  }
  static BlockStateKey wall_torch_south() {
    return BlockStateKey{"minecraft:wall_torch", "facing=south"};
  }

  static bool is_falling_block(const std::string& name) {
    static constexpr const char* kFalling[] = {"minecraft:sand",
                                               "minecraft:red_sand",
                                               "minecraft:gravel",
                                               "minecraft:suspicious_sand",
                                               "minecraft:suspicious_gravel",
                                               "minecraft:anvil",
                                               "minecraft:chipped_anvil",
                                               "minecraft:damaged_anvil",
                                               "minecraft:pointed_dripstone",
                                               "minecraft:scaffolding"};
    for (const char* falling : kFalling)
      if (name == falling)
        return true;
    return false;
  }

  Chunk& chunk;
  const WgMineshaftPiece& piece;
  WgStructureHeights& heights;
  WgClimateSampler& sampler;
  std::vector<WgMineshaftChest>& chests;
  WgLegacyRandom random;
  WgPieceBox clip;
  uint32_t changed_ = 0;
};

} // namespace

WgMineshaftStart wg_mineshaft_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                    WgClimateSampler& sampler, WgStructureHeights& heights) {
  WgMineshaftStart start;
  const WgStructureSet* set = wg_structure_set_by_name("minecraft:mineshafts");
  if (set == nullptr || !wg_structure_is_chunk(seed, *set, chunk_x, chunk_z))
    return start;

  struct Option {
    const WgStructure* structure;
    uint32_t weight;
  };
  Option options[2];
  int32_t count = 0;
  int32_t total = 0;
  for (uint32_t i = 0; i < set->entry_count && count < 2; ++i) {
    const WgStructureSetEntry& entry = kWgStructureSetEntries[set->entry_offset + i];
    const WgStructure* structure = wg_structure_by_name(entry.structure);
    if (structure == nullptr)
      continue;
    options[count++] = Option{structure, entry.weight};
    total += static_cast<int32_t>(entry.weight);
  }
  if (count == 0 || total <= 0)
    return start;

  WgLegacyRandom pick(0);
  seed_large_feature(pick, seed, chunk_x, chunk_z);

  while (count > 0) {
    int32_t choice = pick.next_int(total);
    int32_t index = 0;
    while (index < count) {
      choice -= static_cast<int32_t>(options[index].weight);
      if (choice < 0)
        break;
      ++index;
    }
    const WgStructure* structure = options[index].structure;
    WgMineshaftStart candidate;
    candidate.structure = structure;
    candidate.world_seed = seed;
    candidate.chunk_x = chunk_x;
    candidate.chunk_z = chunk_z;
    candidate.type = std::strcmp(structure->name, "minecraft:mineshaft_mesa") == 0 ? 1 : 0;

    WgLegacyRandom context(0);
    seed_large_feature(context, seed, chunk_x, chunk_z);
    (void)context.next_double();

    const int32_t west = chunk_x * 16 + 2;
    const int32_t north = chunk_z * 16 + 2;
    WgMineshaftPiece room;
    room.kind = WgMineshaftPieceKind::kRoom;
    room.type = candidate.type;
    room.gen_depth = 0;
    room.min_x = west;
    room.min_y = 50;
    room.min_z = north;
    room.max_x = west + 7 + context.next_int(6);
    room.max_y = 54 + context.next_int(6);
    room.max_z = north + 7 + context.next_int(6);
    const WgPieceBox room_box{room.min_x, room.min_y, room.min_z,
                              room.max_x, room.max_y, room.max_z};
    MineshaftAssembler assembler(candidate, context, candidate.type);
    const int32_t room_index = assembler.add_piece(room);
    assembler.add_children(room_index, room_box);

    int32_t dy = 0;
    if (candidate.type == 1) {
      int32_t min_x = candidate.pieces[0].min_x;
      int32_t min_y = candidate.pieces[0].min_y;
      int32_t min_z = candidate.pieces[0].min_z;
      int32_t max_x = candidate.pieces[0].max_x;
      int32_t max_y = candidate.pieces[0].max_y;
      int32_t max_z = candidate.pieces[0].max_z;
      for (const WgMineshaftPiece& piece : candidate.pieces) {
        min_x = std::min(min_x, piece.min_x);
        min_y = std::min(min_y, piece.min_y);
        min_z = std::min(min_z, piece.min_z);
        max_x = std::max(max_x, piece.max_x);
        max_y = std::max(max_y, piece.max_y);
        max_z = std::max(max_z, piece.max_z);
      }
      const int32_t center_x = min_x + (max_x - min_x + 1) / 2;
      const int32_t center_y = min_y + (max_y - min_y + 1) / 2;
      const int32_t center_z = min_z + (max_z - min_z + 1) / 2;
      const int32_t surface =
          heights.height(center_x, center_z, WgStructureHeightmap::kWorldSurface);
      const int32_t target = surface <= 63 ? 63 : 63 + context.next_int(surface - 63 + 1);
      dy = target - center_y;
      mineshaft_offset_pieces(candidate, dy);
    } else {
      dy = mineshaft_move_below_sea_level(candidate, context, 63, kMinY, 10);
    }

    candidate.start_x = chunk_x * 16 + 8;
    candidate.start_y = 50 + dy;
    candidate.start_z = chunk_z * 16;
    int32_t min_x = candidate.pieces[0].min_x;
    int32_t min_y = candidate.pieces[0].min_y;
    int32_t min_z = candidate.pieces[0].min_z;
    int32_t max_x = candidate.pieces[0].max_x;
    int32_t max_y = candidate.pieces[0].max_y;
    int32_t max_z = candidate.pieces[0].max_z;
    for (const WgMineshaftPiece& piece : candidate.pieces) {
      min_x = std::min(min_x, piece.min_x);
      min_y = std::min(min_y, piece.min_y);
      min_z = std::min(min_z, piece.min_z);
      max_x = std::max(max_x, piece.max_x);
      max_y = std::max(max_y, piece.max_y);
      max_z = std::max(max_z, piece.max_z);
    }
    candidate.min_x = min_x;
    candidate.min_y = min_y;
    candidate.min_z = min_z;
    candidate.max_x = max_x;
    candidate.max_y = max_y;
    candidate.max_z = max_z;

    if (wg_structure_start_biome_is_valid(sampler, *structure, candidate.start_x, candidate.start_y,
                                          candidate.start_z)) {
      candidate.valid = true;
      return candidate;
    }

    total -= static_cast<int32_t>(options[index].weight);
    for (int32_t i = index; i + 1 < count; ++i)
      options[i] = options[i + 1];
    --count;
  }
  return start;
}

uint32_t wg_place_mineshaft(Chunk& chunk, const WgMineshaftStart& start,
                            WgStructureHeights& heights, WgClimateSampler& sampler) {
  if (!start.valid)
    return 0;
  uint32_t changed = 0;
  std::vector<WgMineshaftChest> chests;
  const int32_t base_x = chunk.x * 16;
  const int32_t base_z = chunk.z * 16;
  for (const WgMineshaftPiece& piece : start.pieces) {
    if (piece.max_x < base_x || piece.min_x >= base_x + 16 || piece.max_z < base_z ||
        piece.min_z >= base_z + 16 || piece.max_y < kMinY || piece.min_y >= kMinY + kHeight)
      continue;
    MineshaftWriter writer(chunk, piece, heights, sampler, chests);
    writer.run();
    changed += writer.changed();
  }
  // The minecart chest substitution happens after the piece pass, exactly as
  // the Java probe's PROXY dump does. `reorient` therefore sees the finished
  // neighbourhood, and the rail stays readable to every later piece.
  for (const WgMineshaftChest& chest : chests) {
    const int32_t local_x = chest.x - base_x;
    const int32_t local_z = chest.z - base_z;
    if (local_x < 0 || local_x >= 16 || local_z < 0 || local_z >= 16)
      continue;
    const char* facing = reoriented_chest_facing(chunk, local_x, chest.y, local_z);
    const BlockStateKey state{"minecraft:chest",
                              std::string("facing=") + facing + ",type=single,waterlogged=false"};
    if (chunk.block_state_at(local_x, chest.y, local_z) != state &&
        chunk.set_block_state(local_x, chest.y, local_z, state))
      ++changed;
    add_container_loot_with_seed(chunk, "minecraft:chest", chest.x, chest.y, chest.z,
                                 "minecraft:chests/abandoned_mineshaft", chest.loot_seed);
  }
  return changed;
}

namespace {

// ---------------------------------------------------------------------------
// Strongholds
// ---------------------------------------------------------------------------

constexpr int32_t kStrongholdMagicStartY = 64;
constexpr int32_t kStrongholdMaxDepth = 50;
constexpr int32_t kStrongholdLowestY = 10;
constexpr int32_t kStrongholdMaxDistance = 112;
constexpr uint32_t kStrongholdMaxAttempts = 512;
constexpr size_t kStrongholdMaxPieces = 4096;

// `StrongholdPieces.STRONGHOLD_PIECE_WEIGHTS`, in the order the game checks
// them. The max is 0 for the unbounded kinds; `Library` and `PortalRoom` also
// carry the `doPlace` depth gates the anonymous subclasses add.
struct StrongholdWeight {
  WgStrongholdPieceKind kind;
  int32_t weight;
  int32_t max_place;
};

constexpr StrongholdWeight kStrongholdWeights[] = {
    {WgStrongholdPieceKind::kStraight, 40, 0},
    {WgStrongholdPieceKind::kPrisonHall, 5, 5},
    {WgStrongholdPieceKind::kLeftTurn, 20, 0},
    {WgStrongholdPieceKind::kRightTurn, 20, 0},
    {WgStrongholdPieceKind::kRoomCrossing, 10, 6},
    {WgStrongholdPieceKind::kStraightStairsDown, 5, 5},
    {WgStrongholdPieceKind::kStairsDown, 5, 5},
    {WgStrongholdPieceKind::kFiveCrossing, 5, 4},
    {WgStrongholdPieceKind::kChestCorridor, 5, 4},
    {WgStrongholdPieceKind::kLibrary, 10, 2},
    {WgStrongholdPieceKind::kPortalRoom, 20, 1},
};
constexpr int32_t kStrongholdWeightCount =
    static_cast<int32_t>(sizeof(kStrongholdWeights) / sizeof(kStrongholdWeights[0]));

int32_t stronghold_weight_index(WgStrongholdPieceKind kind) {
  for (int32_t i = 0; i < kStrongholdWeightCount; ++i)
    if (kStrongholdWeights[i].kind == kind)
      return i;
  return -1;
}

// `BoundingBox.orientBox`: the four decompiled corner formulas, then the
// constructor's inverted-bounds normalisation.
WgPieceBox stronghold_orient_box(int32_t foot_x, int32_t foot_y, int32_t foot_z, int32_t off_x,
                                 int32_t off_y, int32_t off_z, int32_t width, int32_t height,
                                 int32_t depth, uint8_t direction) {
  WgPieceBox box;
  switch (direction) {
  case 0: // NORTH
    box.min_x = foot_x + off_x;
    box.max_x = foot_x + width - 1 + off_x;
    box.min_z = foot_z - depth + 1 + off_z;
    box.max_z = foot_z + off_z;
    break;
  case 3: // WEST
    box.min_x = foot_x - depth + 1 + off_z;
    box.max_x = foot_x + off_z;
    box.min_z = foot_z + off_x;
    box.max_z = foot_z + width - 1 + off_x;
    break;
  case 1: // EAST
    box.min_x = foot_x + off_z;
    box.max_x = foot_x + depth - 1 + off_z;
    box.min_z = foot_z + off_x;
    box.max_z = foot_z + width - 1 + off_x;
    break;
  default: // SOUTH
    box.min_x = foot_x + off_x;
    box.max_x = foot_x + width - 1 + off_x;
    box.min_z = foot_z + off_z;
    box.max_z = foot_z + depth - 1 + off_z;
    break;
  }
  box.min_y = foot_y + off_y;
  box.max_y = foot_y + height - 1 + off_y;
  if (box.max_x < box.min_x)
    std::swap(box.min_x, box.max_x);
  if (box.max_y < box.min_y)
    std::swap(box.min_y, box.max_y);
  if (box.max_z < box.min_z)
    std::swap(box.min_z, box.max_z);
  return box;
}

// `StructurePiece.makeBoundingBox` for the start's 5 x 11 x 5 `StairsDown`.
WgPieceBox stronghold_start_box(int32_t west, int32_t north, uint8_t direction) {
  WgPieceBox box;
  box.min_x = west;
  box.max_x = west + 4;
  box.min_y = kStrongholdMagicStartY;
  box.max_y = kStrongholdMagicStartY + 10;
  box.min_z = north;
  box.max_z = north + 4;
  if (direction == 1 || direction == 3) {
    // The width/depth swap is a no-op for 5 x 5; kept for the formula's shape.
    box.max_x = west + 4;
    box.max_z = north + 4;
  }
  return box;
}

bool stronghold_intersects(const WgPieceBox& a, const WgPieceBox& b) {
  return a.max_x >= b.min_x && a.min_x <= b.max_x && a.max_z >= b.min_z && a.min_z <= b.max_z &&
         a.max_y >= b.min_y && a.min_y <= b.max_y;
}

bool stronghold_box_ok(const WgPieceBox& box) {
  return box.min_y > kStrongholdLowestY;
}

WgPieceBox stronghold_piece_box(const WgStrongholdPiece& piece) {
  return WgPieceBox{piece.min_x, piece.min_y, piece.min_z, piece.max_x, piece.max_y, piece.max_z};
}

void stronghold_set_box(WgStrongholdPiece& piece, const WgPieceBox& box) {
  piece.min_x = box.min_x;
  piece.min_y = box.min_y;
  piece.min_z = box.min_z;
  piece.max_x = box.max_x;
  piece.max_y = box.max_y;
  piece.max_z = box.max_z;
}

uint8_t stronghold_random_small_door(WgLegacyRandom& random) {
  switch (random.next_int(5)) {
  case 2:
    return 1; // WOOD_DOOR
  case 3:
    return 2; // GRATES
  case 4:
    return 3; // IRON_DOOR
  default:
    return 0; // OPENING
  }
}

struct StrongholdAssembler {
  WgStrongholdStart& start;
  WgLegacyRandom& random;
  std::vector<int32_t> current_weights;
  int32_t place_count[kStrongholdWeightCount] = {};
  int32_t total_weight = 0;
  int32_t previous_piece = -1;
  int32_t imposed_piece = -1;
  std::vector<int32_t> pending;

  StrongholdAssembler(WgStrongholdStart& start_in, WgLegacyRandom& random_in)
      : start(start_in), random(random_in) {
    for (int32_t i = 0; i < kStrongholdWeightCount; ++i)
      current_weights.push_back(i);
  }

  // `StrongholdPieces.updatePieceWeight`: every remaining weight contributes
  // to the total, including the maxed-out ones; `hasAny` only decides whether
  // the selection loop runs at all.
  bool update_piece_weight() {
    bool has_any = false;
    total_weight = 0;
    for (const int32_t index : current_weights) {
      // Only a bounded row can keep the walk alive; once every remaining row
      // is unbounded vanilla returns false and the start stops growing, even
      // though those rows still contribute to the total below.
      if (kStrongholdWeights[index].max_place != 0 &&
          place_count[index] < kStrongholdWeights[index].max_place)
        has_any = true;
      total_weight += kStrongholdWeights[index].weight;
    }
    return has_any;
  }

  bool do_place(int32_t weight_index, int32_t depth) const {
    const StrongholdWeight& weight = kStrongholdWeights[weight_index];
    if (weight.max_place != 0 && place_count[weight_index] >= weight.max_place)
      return false;
    if (weight.kind == WgStrongholdPieceKind::kLibrary && depth <= 4)
      return false;
    if (weight.kind == WgStrongholdPieceKind::kPortalRoom && depth <= 5)
      return false;
    return true;
  }

  bool weight_valid(int32_t weight_index) const {
    const StrongholdWeight& weight = kStrongholdWeights[weight_index];
    return weight.max_place == 0 || place_count[weight_index] < weight.max_place;
  }

  int32_t find_collision(const WgPieceBox& box) const {
    for (size_t i = 0; i < start.pieces.size(); ++i)
      if (stronghold_intersects(stronghold_piece_box(start.pieces[i]), box))
        return static_cast<int32_t>(i);
    return -1;
  }

  int32_t add_piece(const WgStrongholdPiece& piece) {
    start.pieces.push_back(piece);
    return static_cast<int32_t>(start.pieces.size()) - 1;
  }

  // `StrongholdPieces.createPiece` for one kind: build the box, apply the
  // `isOkBox`/collision gate, then the constructor's own draws. A failed gate
  // returns before consuming any random value.
  bool create_piece(WgStrongholdPieceKind kind, int32_t foot_x, int32_t foot_y, int32_t foot_z,
                    uint8_t direction, int32_t depth, WgStrongholdPiece& out) const {
    WgPieceBox box;
    switch (kind) {
    case WgStrongholdPieceKind::kStraight:
      box = stronghold_orient_box(foot_x, foot_y, foot_z, -1, -1, 0, 5, 5, 7, direction);
      break;
    case WgStrongholdPieceKind::kPrisonHall:
      box = stronghold_orient_box(foot_x, foot_y, foot_z, -1, -1, 0, 9, 5, 11, direction);
      break;
    case WgStrongholdPieceKind::kLeftTurn:
    case WgStrongholdPieceKind::kRightTurn:
      box = stronghold_orient_box(foot_x, foot_y, foot_z, -1, -1, 0, 5, 5, 5, direction);
      break;
    case WgStrongholdPieceKind::kRoomCrossing:
      box = stronghold_orient_box(foot_x, foot_y, foot_z, -4, -1, 0, 11, 7, 11, direction);
      break;
    case WgStrongholdPieceKind::kStraightStairsDown:
      box = stronghold_orient_box(foot_x, foot_y, foot_z, -1, -7, 0, 5, 11, 8, direction);
      break;
    case WgStrongholdPieceKind::kStairsDown:
      box = stronghold_orient_box(foot_x, foot_y, foot_z, -1, -7, 0, 5, 11, 5, direction);
      break;
    case WgStrongholdPieceKind::kFiveCrossing:
      box = stronghold_orient_box(foot_x, foot_y, foot_z, -4, -3, 0, 10, 9, 11, direction);
      break;
    case WgStrongholdPieceKind::kChestCorridor:
      box = stronghold_orient_box(foot_x, foot_y, foot_z, -1, -1, 0, 5, 5, 7, direction);
      break;
    case WgStrongholdPieceKind::kLibrary: {
      box = stronghold_orient_box(foot_x, foot_y, foot_z, -4, -1, 0, 14, 11, 15, direction);
      if (!stronghold_box_ok(box) || find_collision(box) >= 0) {
        box = stronghold_orient_box(foot_x, foot_y, foot_z, -4, -1, 0, 14, 6, 15, direction);
        if (!stronghold_box_ok(box) || find_collision(box) >= 0)
          return false;
      }
      break;
    }
    case WgStrongholdPieceKind::kPortalRoom:
      box = stronghold_orient_box(foot_x, foot_y, foot_z, -4, -1, 0, 11, 8, 16, direction);
      break;
    default:
      return false;
    }
    if (kind != WgStrongholdPieceKind::kLibrary &&
        (!stronghold_box_ok(box) || find_collision(box) >= 0))
      return false;

    out = WgStrongholdPiece{};
    out.kind = kind;
    out.direction = direction;
    out.gen_depth = static_cast<uint8_t>(depth);
    stronghold_set_box(out, box);

    switch (kind) {
    case WgStrongholdPieceKind::kStraight:
      out.entry_door = stronghold_random_small_door(random);
      out.left = random.next_int(2) == 0 ? 1 : 0;
      out.right = random.next_int(2) == 0 ? 1 : 0;
      break;
    case WgStrongholdPieceKind::kPrisonHall:
    case WgStrongholdPieceKind::kLeftTurn:
    case WgStrongholdPieceKind::kRightTurn:
    case WgStrongholdPieceKind::kStraightStairsDown:
    case WgStrongholdPieceKind::kStairsDown:
    case WgStrongholdPieceKind::kChestCorridor:
      out.entry_door = stronghold_random_small_door(random);
      break;
    case WgStrongholdPieceKind::kRoomCrossing:
      out.entry_door = stronghold_random_small_door(random);
      out.room_type = static_cast<uint8_t>(random.next_int(5));
      break;
    case WgStrongholdPieceKind::kFiveCrossing:
      out.entry_door = stronghold_random_small_door(random);
      out.left = random.next_boolean() ? 1 : 0;
      out.left_high = random.next_boolean() ? 1 : 0;
      out.right = random.next_boolean() ? 1 : 0;
      out.right_high = random.next_int(3) > 0 ? 1 : 0;
      break;
    case WgStrongholdPieceKind::kLibrary:
      out.entry_door = stronghold_random_small_door(random);
      out.is_tall = (box.max_y - box.min_y + 1) > 6 ? 1 : 0;
      break;
    case WgStrongholdPieceKind::kPortalRoom:
      break;
    default:
      break;
    }
    return true;
  }

  // `FillerCorridor.findPieceBox`.
  bool find_filler_box(int32_t foot_x, int32_t foot_y, int32_t foot_z, uint8_t direction,
                       WgPieceBox& out) const {
    const WgPieceBox probe =
        stronghold_orient_box(foot_x, foot_y, foot_z, -1, -1, 0, 5, 5, 4, direction);
    const int32_t collision = find_collision(probe);
    if (collision < 0)
      return false;
    const WgPieceBox& collision_box = stronghold_piece_box(start.pieces[collision]);
    if (collision_box.min_y != probe.min_y)
      return false;
    for (int32_t depth = 2; depth >= 1; --depth) {
      const WgPieceBox short_box =
          stronghold_orient_box(foot_x, foot_y, foot_z, -1, -1, 0, 5, 5, depth, direction);
      if (stronghold_intersects(collision_box, short_box))
        continue;
      out = stronghold_orient_box(foot_x, foot_y, foot_z, -1, -1, 0, 5, 5, depth + 1, direction);
      return true;
    }
    return false;
  }

  // `StrongholdPieces.generatePieceFromSmallDoor`.
  bool generate_piece_from_small_door(int32_t foot_x, int32_t foot_y, int32_t foot_z,
                                      uint8_t direction, int32_t depth, WgStrongholdPiece& out) {
    if (!update_piece_weight())
      return false;
    if (imposed_piece >= 0) {
      const int32_t imposed = imposed_piece;
      imposed_piece = -1;
      if (create_piece(kStrongholdWeights[imposed].kind, foot_x, foot_y, foot_z, direction, depth,
                       out))
        return true;
    }
    for (int32_t attempt = 0; attempt < 5; ++attempt) {
      int32_t weight_selection = random.next_int(total_weight);
      bool next_attempt = false;
      for (size_t ci = 0; ci < current_weights.size(); ++ci) {
        const int32_t weight_index = current_weights[ci];
        weight_selection -= kStrongholdWeights[weight_index].weight;
        if (weight_selection >= 0)
          continue;
        if (!do_place(weight_index, depth) || weight_index == previous_piece) {
          next_attempt = true;
          break;
        }
        if (!create_piece(kStrongholdWeights[weight_index].kind, foot_x, foot_y, foot_z, direction,
                          depth, out))
          continue;
        ++place_count[weight_index];
        previous_piece = weight_index;
        if (!weight_valid(weight_index))
          current_weights.erase(current_weights.begin() + static_cast<std::ptrdiff_t>(ci));
        return true;
      }
      if (next_attempt)
        continue;
    }
    WgPieceBox filler_box;
    if (!find_filler_box(foot_x, foot_y, foot_z, direction, filler_box) || filler_box.min_y <= 1)
      return false;
    out = WgStrongholdPiece{};
    out.kind = WgStrongholdPieceKind::kFillerCorridor;
    out.direction = direction;
    out.gen_depth = static_cast<uint8_t>(depth);
    out.steps = direction == 0 || direction == 2 ? filler_box.max_z - filler_box.min_z + 1
                                                 : filler_box.max_x - filler_box.min_x + 1;
    stronghold_set_box(out, filler_box);
    return true;
  }

  // `StrongholdPieces.generateAndAddPiece`.
  void generate_and_add(int32_t foot_x, int32_t foot_y, int32_t foot_z, uint8_t direction,
                        int32_t depth) {
    if (depth > kStrongholdMaxDepth)
      return;
    if (std::abs(foot_x - start.pieces[0].min_x) > kStrongholdMaxDistance ||
        std::abs(foot_z - start.pieces[0].min_z) > kStrongholdMaxDistance)
      return;
    if (start.pieces.size() >= kStrongholdMaxPieces)
      return;
    WgStrongholdPiece piece;
    if (!generate_piece_from_small_door(foot_x, foot_y, foot_z, direction, depth + 1, piece))
      return;
    const int32_t index = add_piece(piece);
    pending.push_back(index);
  }

  void add_forward_child(const WgStrongholdPiece& piece, int32_t x_off, int32_t y_off) {
    switch (piece.direction) {
    case 0:
      generate_and_add(piece.min_x + x_off, piece.min_y + y_off, piece.min_z - 1, 0,
                       piece.gen_depth);
      break;
    case 1:
      generate_and_add(piece.max_x + 1, piece.min_y + y_off, piece.min_z + x_off, 1,
                       piece.gen_depth);
      break;
    case 2:
      generate_and_add(piece.min_x + x_off, piece.min_y + y_off, piece.max_z + 1, 2,
                       piece.gen_depth);
      break;
    default:
      generate_and_add(piece.min_x - 1, piece.min_y + y_off, piece.min_z + x_off, 3,
                       piece.gen_depth);
      break;
    }
  }

  void add_left_child(const WgStrongholdPiece& piece, int32_t y_off, int32_t z_off) {
    switch (piece.direction) {
    case 0:
    case 2:
      generate_and_add(piece.min_x - 1, piece.min_y + y_off, piece.min_z + z_off, 3,
                       piece.gen_depth);
      break;
    default:
      generate_and_add(piece.min_x + z_off, piece.min_y + y_off, piece.min_z - 1, 0,
                       piece.gen_depth);
      break;
    }
  }

  void add_right_child(const WgStrongholdPiece& piece, int32_t y_off, int32_t z_off) {
    switch (piece.direction) {
    case 0:
    case 2:
      generate_and_add(piece.max_x + 1, piece.min_y + y_off, piece.min_z + z_off, 1,
                       piece.gen_depth);
      break;
    default:
      generate_and_add(piece.min_x + z_off, piece.min_y + y_off, piece.max_z + 1, 2,
                       piece.gen_depth);
      break;
    }
  }

  // `StrongholdPiece.addChildren` for one piece.
  void add_children(int32_t index) {
    const WgStrongholdPiece piece = start.pieces[index];
    switch (piece.kind) {
    case WgStrongholdPieceKind::kStart:
      imposed_piece = stronghold_weight_index(WgStrongholdPieceKind::kFiveCrossing);
      add_forward_child(piece, 1, 1);
      break;
    case WgStrongholdPieceKind::kStairsDown:
      if (piece.is_source)
        imposed_piece = stronghold_weight_index(WgStrongholdPieceKind::kFiveCrossing);
      add_forward_child(piece, 1, 1);
      break;
    case WgStrongholdPieceKind::kStraight:
      add_forward_child(piece, 1, 1);
      if (piece.left)
        add_left_child(piece, 1, 2);
      if (piece.right)
        add_right_child(piece, 1, 2);
      break;
    case WgStrongholdPieceKind::kPrisonHall:
    case WgStrongholdPieceKind::kStraightStairsDown:
    case WgStrongholdPieceKind::kChestCorridor:
      add_forward_child(piece, 1, 1);
      break;
    case WgStrongholdPieceKind::kLeftTurn:
      if (piece.direction == 0 || piece.direction == 1)
        add_left_child(piece, 1, 1);
      else
        add_right_child(piece, 1, 1);
      break;
    case WgStrongholdPieceKind::kRightTurn:
      if (piece.direction == 0 || piece.direction == 1)
        add_right_child(piece, 1, 1);
      else
        add_left_child(piece, 1, 1);
      break;
    case WgStrongholdPieceKind::kRoomCrossing:
      add_forward_child(piece, 4, 1);
      add_left_child(piece, 1, 4);
      add_right_child(piece, 1, 4);
      break;
    case WgStrongholdPieceKind::kFiveCrossing: {
      int32_t z_off_a = 3;
      int32_t z_off_b = 5;
      if (piece.direction == 3 || piece.direction == 0) {
        z_off_a = 8 - z_off_a;
        z_off_b = 8 - z_off_b;
      }
      add_forward_child(piece, 5, 1);
      // `FiveCrossing.addChildren` passes the `zOffA`/`zOffB` variables as the
      // **y** offset and the literal 1/7 as the z offset, despite the names.
      if (piece.left)
        add_left_child(piece, z_off_a, 1);
      if (piece.left_high)
        add_left_child(piece, z_off_b, 7);
      if (piece.right)
        add_right_child(piece, z_off_a, 1);
      if (piece.right_high)
        add_right_child(piece, z_off_b, 7);
      break;
    }
    case WgStrongholdPieceKind::kPortalRoom:
      start.portal_room = index;
      break;
    case WgStrongholdPieceKind::kLibrary:
    case WgStrongholdPieceKind::kFillerCorridor:
      break;
    }
  }
};

} // namespace

WgStrongholdStart wg_stronghold_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                      WgClimateSampler& sampler) {
  WgStrongholdStart start;
  const WgStructure* structure = wg_structure_by_name("minecraft:stronghold");
  if (structure == nullptr)
    return start;

  // `StrongholdStructure.findGenerationPoint` returns the chunk's minimum
  // corner at y 0 as its stub position; `Structure.isValidBiome` tests that
  // quart before any piece is built.
  if (!wg_structure_start_biome_is_valid(sampler, *structure, chunk_x * 16, 0, chunk_z * 16))
    return start;

  start.structure = structure;
  start.world_seed = seed;
  start.chunk_x = chunk_x;
  start.chunk_z = chunk_z;

  for (uint32_t attempt = 0; attempt < kStrongholdMaxAttempts; ++attempt) {
    WgStrongholdStart candidate;
    candidate.structure = structure;
    candidate.world_seed = seed;
    candidate.chunk_x = chunk_x;
    candidate.chunk_z = chunk_z;
    candidate.attempts = attempt + 1;

    WgLegacyRandom random(0);
    seed_large_feature(random, seed + static_cast<int64_t>(attempt), chunk_x, chunk_z);
    StrongholdAssembler assembler(candidate, random);

    // `new StartPiece(random, chunkPos.getBlockX(2), chunkPos.getBlockZ(2))`:
    // `getRandomHorizontalDirection` is evaluated before the constructor body.
    const uint8_t direction = static_cast<uint8_t>(random.next_int(4));
    WgStrongholdPiece start_piece;
    start_piece.kind = WgStrongholdPieceKind::kStart;
    start_piece.direction = direction;
    start_piece.entry_door = 0;
    start_piece.gen_depth = 0;
    start_piece.is_source = 1;
    stronghold_set_box(start_piece,
                       stronghold_start_box(chunk_x * 16 + 2, chunk_z * 16 + 2, direction));
    const int32_t start_index = assembler.add_piece(start_piece);
    assembler.add_children(start_index);

    while (!assembler.pending.empty()) {
      const int32_t pick = random.next_int(static_cast<int32_t>(assembler.pending.size()));
      const int32_t index = assembler.pending[static_cast<size_t>(pick)];
      assembler.pending.erase(assembler.pending.begin() + pick);
      assembler.add_children(index);
    }

    if (candidate.pieces.empty())
      continue;

    WgPieceBox bounds = stronghold_piece_box(candidate.pieces[0]);
    for (const WgStrongholdPiece& piece : candidate.pieces) {
      const WgPieceBox box = stronghold_piece_box(piece);
      bounds.min_x = std::min(bounds.min_x, box.min_x);
      bounds.min_y = std::min(bounds.min_y, box.min_y);
      bounds.min_z = std::min(bounds.min_z, box.min_z);
      bounds.max_x = std::max(bounds.max_x, box.max_x);
      bounds.max_y = std::max(bounds.max_y, box.max_y);
      bounds.max_z = std::max(bounds.max_z, box.max_z);
    }

    // `StructurePiecesBuilder.moveBelowSeaLevel(63, -64, random, 10)`: the
    // top of the union is pushed up to at most y 53 (or left alone when it is
    // already above it), and every box moves together.
    const int32_t max_y_limit = 63 - 10;
    int32_t y1 = (bounds.max_y - bounds.min_y + 1) + kMinY + 1;
    if (y1 < max_y_limit)
      y1 += random.next_int(max_y_limit - y1);
    const int32_t dy = y1 - bounds.max_y;
    for (WgStrongholdPiece& piece : candidate.pieces) {
      piece.min_y += dy;
      piece.max_y += dy;
    }
    bounds.min_y += dy;
    bounds.max_y += dy;

    // `StructureStart.getBoundingBox` is `structure.adjustBoundingBox` of the
    // pieces' union; the stronghold's terrain adaptation is not `none`, so
    // vanilla inflates that box by 12 on every side. The pieces themselves
    // stay where `moveBelowSeaLevel` put them.
    constexpr int32_t kStrongholdBoxInflation = 12;
    candidate.min_x = bounds.min_x - kStrongholdBoxInflation;
    candidate.min_y = bounds.min_y - kStrongholdBoxInflation;
    candidate.min_z = bounds.min_z - kStrongholdBoxInflation;
    candidate.max_x = bounds.max_x + kStrongholdBoxInflation;
    candidate.max_y = bounds.max_y + kStrongholdBoxInflation;
    candidate.max_z = bounds.max_z + kStrongholdBoxInflation;

    if (candidate.portal_room < 0)
      continue;
    candidate.valid = true;
    return candidate;
  }
  return start;
}

namespace {

// `Direction.get3DDataValue` order: down, up, north, south, west, east.
constexpr int32_t kMonumentDown = 0;
constexpr int32_t kMonumentUp = 1;
constexpr int32_t kMonumentNorth = 2;
constexpr int32_t kMonumentSouth = 3;
constexpr int32_t kMonumentWest = 4;
constexpr int32_t kMonumentEast = 5;
constexpr int32_t kMonumentOpposite[6] = {1, 0, 3, 2, 5, 4};
constexpr int32_t kMonumentStepX[6] = {0, 0, 0, 0, -1, 1};
constexpr int32_t kMonumentStepZ[6] = {0, 0, -1, 1, 0, 0};
constexpr int32_t kMonumentSeaLevel = 63;
constexpr int32_t kMonumentGridSize = 75;
constexpr int32_t kMonumentRoofIndex = 1003;
constexpr int32_t kMonumentLeftWingIndex = 1001;
constexpr int32_t kMonumentRightWingIndex = 1002;

// `OceanMonumentPieces.OceanMonumentPiece.getRoomIndex`.
int32_t monument_room_index(int32_t room_x, int32_t room_y, int32_t room_z) {
  return room_y * 25 + room_z * 5 + room_x;
}

// One `RoomDefinition` during assembly. `connections` hold room `index`
// values, not vector slots, because the pieces carry that index too.
struct MonumentRoomSlot {
  bool present = false;
  int32_t index = 0;
  uint8_t has_opening = 0;
  int16_t connections[6] = {-1, -1, -1, -1, -1, -1};
  uint8_t claimed = 0;
  uint8_t is_source = 0;
  int32_t scan_index = 0;
};

constexpr int32_t kMonumentRoomSlots = kMonumentGridSize + 3;

int32_t monument_slot_for_index(const std::array<MonumentRoomSlot, kMonumentRoomSlots>& slots,
                                int32_t index) {
  if (index >= 0 && index < kMonumentGridSize)
    return slots[static_cast<size_t>(index)].present ? index : -1;
  for (int32_t i = kMonumentGridSize; i < kMonumentRoomSlots; ++i)
    if (slots[static_cast<size_t>(i)].present && slots[static_cast<size_t>(i)].index == index)
      return i;
  return -1;
}

bool monument_open(const MonumentRoomSlot& room, int32_t direction) {
  return (room.has_opening & (1u << direction)) != 0;
}

void monument_set_connection(std::array<MonumentRoomSlot, kMonumentRoomSlots>& slots,
                             MonumentRoomSlot& room, int32_t direction, MonumentRoomSlot& other) {
  (void)slots;
  room.connections[direction] = static_cast<int16_t>(other.index);
  other.connections[kMonumentOpposite[direction]] = static_cast<int16_t>(room.index);
}

void monument_update_openings(MonumentRoomSlot& room) {
  uint8_t openings = 0;
  for (int32_t i = 0; i < 6; ++i)
    if (room.connections[i] >= 0)
      openings = static_cast<uint8_t>(openings | (1u << i));
  room.has_opening = openings;
}

// `RoomDefinition.findSource`: a depth-first walk over still-open doors,
// marking each room with the scan number it was reached under. `scanIndex` is
// the caller's post-incremented integer; vanilla passes it to both sides of a
// closed door, and a failed side still consumes its value.
bool monument_find_source(std::array<MonumentRoomSlot, kMonumentRoomSlots>& slots, int32_t slot,
                          int32_t scan_index) {
  MonumentRoomSlot& room = slots[static_cast<size_t>(slot)];
  if (room.is_source != 0)
    return true;
  room.scan_index = scan_index;
  for (int32_t i = 0; i < 6; ++i) {
    if (room.connections[i] < 0 || !monument_open(room, i))
      continue;
    const int32_t next = monument_slot_for_index(slots, room.connections[i]);
    if (next < 0)
      continue;
    if (slots[static_cast<size_t>(next)].scan_index == scan_index)
      continue;
    if (!monument_find_source(slots, next, scan_index))
      continue;
    return true;
  }
  return false;
}

void monument_set_piece_box(WgOceanMonumentPiece& piece, const WgPieceBox& box) {
  piece.min_x = box.min_x;
  piece.min_y = box.min_y;
  piece.min_z = box.min_z;
  piece.max_x = box.max_x;
  piece.max_y = box.max_y;
  piece.max_z = box.max_z;
}

WgPieceBox monument_building_box(int32_t west, int32_t north, uint8_t direction) {
  // `StructurePiece.makeBoundingBox(west, 39, north, direction, 58, 23, 58)`.
  // Width and depth are equal, so the axis-dependent swap is a no-op here.
  (void)direction;
  return WgPieceBox{west, 39, north, west + 57, 39 + 22, north + 57};
}

XZ monument_world_xz(const WgPieceBox& box, uint8_t direction, int32_t local_x, int32_t local_z) {
  switch (direction) {
  case 0:
    return XZ{box.min_x + local_x, box.max_z - local_z};
  case 1:
    return XZ{box.min_x + local_z, box.min_z + local_x};
  case 2:
    return XZ{box.min_x + local_x, box.min_z + local_z};
  default:
    return XZ{box.max_x - local_z, box.min_z + local_x};
  }
}

WgPieceBox monument_from_corners(XZ a, int32_t ay, XZ b, int32_t by) {
  return WgPieceBox{std::min(a.x, b.x), std::min(ay, by), std::min(a.z, b.z),
                    std::max(a.x, b.x), std::max(ay, by), std::max(a.z, b.z)};
}

XZ monument_world_pos(const WgPieceBox& building, uint8_t direction, int32_t local_x,
                      int32_t local_z) {
  return monument_world_xz(building, direction, local_x, local_z);
}

// `OceanMonumentPiece.makeBoundingBox(orientation, roomDefinition, width,
// height, depth)`: the base box is `roomWidth * 8` by `roomHeight * 4` by
// `roomDepth * 8` (with the orientation's width/depth swap), moved by the
// room's grid position. The 0..N-1 extents are `StructurePiece.makeBoundingBox`
// with the same arguments.
WgPieceBox monument_room_box(uint8_t direction, const MonumentRoomSlot& room, int32_t room_width,
                             int32_t room_height, int32_t room_depth) {
  const int32_t room_x = room.index % 5;
  const int32_t room_z = room.index / 5 % 5;
  const int32_t room_y = room.index / 25;
  const bool z_axis = direction == 0 || direction == 2;
  const int32_t size_x = (z_axis ? room_width : room_depth) * 8;
  const int32_t size_z = (z_axis ? room_depth : room_width) * 8;
  WgPieceBox box{0, 0, 0, size_x - 1, room_height * 4 - 1, size_z - 1};
  switch (direction) {
  case 0: // NORTH
    box.min_x += room_x * 8;
    box.max_x += room_x * 8;
    box.min_z += -(room_z + room_depth) * 8 + 1;
    box.max_z += -(room_z + room_depth) * 8 + 1;
    break;
  case 2: // SOUTH
    box.min_x += room_x * 8;
    box.max_x += room_x * 8;
    box.min_z += room_z * 8;
    box.max_z += room_z * 8;
    break;
  case 3: // WEST
    box.min_x += -(room_z + room_depth) * 8 + 1;
    box.max_x += -(room_z + room_depth) * 8 + 1;
    box.min_z += room_x * 8;
    box.max_z += room_x * 8;
    break;
  default: // EAST
    box.min_x += room_z * 8;
    box.max_x += room_z * 8;
    box.min_z += room_x * 8;
    box.max_z += room_x * 8;
    break;
  }
  box.min_y += room_y * 4;
  box.max_y += room_y * 4;
  return box;
}

void monument_move_box(WgPieceBox& box, int32_t dx, int32_t dy, int32_t dz) {
  box.min_x += dx;
  box.max_x += dx;
  box.min_y += dy;
  box.max_y += dy;
  box.min_z += dz;
  box.max_z += dz;
}

// The `required_ocean_monument_surrounding` tag resolved: `#is_ocean` plus
// `#is_river`. The tag is deliberately a small fixed list here, like the other
// tag tables this stage carries; a biome outside it (or an unknown one) makes
// `getBiomesWithin` fail the start.
bool monument_surrounding_biome(const char* biome) {
  static constexpr const char* kSurrounding[] = {
      "minecraft:deep_frozen_ocean", "minecraft:deep_cold_ocean",
      "minecraft:deep_ocean",        "minecraft:deep_lukewarm_ocean",
      "minecraft:frozen_ocean",      "minecraft:ocean",
      "minecraft:cold_ocean",        "minecraft:lukewarm_ocean",
      "minecraft:warm_ocean",        "minecraft:river",
      "minecraft:frozen_river",
  };
  for (const char* name : kSurrounding)
    if (std::strcmp(biome, name) == 0)
      return true;
  return false;
}

// `BiomeSource.getBiomesWithin(x, seaLevel, z, 29, sampler)`: every quart cell
// in the 15 x 16 x 15 box has to be in the surrounding tag. The loops are
// vanilla's z, x, y order; the answer is just the predicate, so the order only
// matters for which failing sample exits first.
bool monument_surrounding_ok(WgClimateSampler& sampler, int32_t center_x, int32_t center_z) {
  const int32_t x0 = (center_x - 29) >> 2;
  const int32_t x1 = (center_x + 29) >> 2;
  const int32_t z0 = (center_z - 29) >> 2;
  const int32_t z1 = (center_z + 29) >> 2;
  const int32_t y0 = (kMonumentSeaLevel - 29) >> 2;
  const int32_t y1 = (kMonumentSeaLevel + 29) >> 2;
  for (int32_t z = z0; z <= z1; ++z)
    for (int32_t x = x0; x <= x1; ++x)
      for (int32_t y = y0; y <= y1; ++y)
        if (!monument_surrounding_biome(sampler.biome_at_quart(x, y, z)))
          return false;
  return true;
}

} // namespace

WgOceanMonumentStart wg_ocean_monument_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                             WgClimateSampler& sampler,
                                             WgStructureHeights& heights) {
  WgOceanMonumentStart start;
  const WgStructure* structure = wg_structure_by_name("minecraft:monument");
  if (structure == nullptr)
    return start;

  const int32_t center_x = chunk_x * 16 + 9;
  const int32_t center_z = chunk_z * 16 + 9;
  if (!monument_surrounding_ok(sampler, center_x, center_z))
    return start;

  // `onTopOfChunkCenter(context, OCEAN_FLOOR_WG, ...)`: the chunk middle
  // column (`getBlockX(8)` / `getBlockZ(8)`), not the (9, 9) column the biome
  // ring is sampled from.
  const int32_t middle_x = chunk_x * 16 + 8;
  const int32_t middle_z = chunk_z * 16 + 8;
  const int32_t middle_y = heights.height(middle_x, middle_z, WgStructureHeightmap::kOceanFloor);
  if (!wg_structure_start_biome_is_valid(sampler, *structure, middle_x, middle_y, middle_z))
    return start;

  start.structure = structure;
  start.world_seed = seed;
  start.chunk_x = chunk_x;
  start.chunk_z = chunk_z;
  start.x = middle_x;
  start.y = middle_y;
  start.z = middle_z;

  WgLegacyRandom random(0);
  seed_large_feature(random, seed, chunk_x, chunk_z);

  // `createTopPiece`: `getRandomHorizontalDirection` is the constructor
  // argument, so it is the stream's first draw; `west`/`north` are the source
  // chunk's minimum corner minus 29.
  const uint8_t direction = static_cast<uint8_t>(random.next_int(4));
  const int32_t west = chunk_x * 16 - 29;
  const int32_t north = chunk_z * 16 - 29;
  const WgPieceBox building_box = monument_building_box(west, north, direction);
  start.building.kind = WgOceanMonumentPieceKind::kBuilding;
  start.building.direction = direction;
  start.building.room_index = -1;
  monument_set_piece_box(start.building, building_box);
  start.min_x = building_box.min_x;
  start.min_y = building_box.min_y;
  start.min_z = building_box.min_z;
  start.max_x = building_box.max_x;
  start.max_y = building_box.max_y;
  start.max_z = building_box.max_z;

  std::array<MonumentRoomSlot, kMonumentRoomSlots> slots{};
  for (int32_t x = 0; x < 5; ++x)
    for (int32_t z = 0; z < 4; ++z)
      for (int32_t y = 0; y < 2; ++y) {
        const int32_t index = monument_room_index(x, y, z);
        slots[static_cast<size_t>(index)].present = true;
        slots[static_cast<size_t>(index)].index = index;
      }
  for (int32_t x = 1; x < 4; ++x)
    for (int32_t z = 0; z < 2; ++z) {
      const int32_t index = monument_room_index(x, 2, z);
      slots[static_cast<size_t>(index)].present = true;
      slots[static_cast<size_t>(index)].index = index;
    }

  // `roomGrid[GRIDROOM_SOURCE_INDEX]` is (2, 0, 0).
  const int32_t source_index = monument_room_index(2, 0, 0);
  // The graph walk is vanilla's x, z, y order and Direction.values() order.
  for (int32_t x = 0; x < 5; ++x) {
    for (int32_t z = 0; z < 5; ++z) {
      for (int32_t y = 0; y < 3; ++y) {
        const int32_t index = monument_room_index(x, y, z);
        MonumentRoomSlot& room = slots[static_cast<size_t>(index)];
        if (!room.present)
          continue;
        for (int32_t dir = 0; dir < 6; ++dir) {
          const int32_t nx = x + kMonumentStepX[dir];
          const int32_t nz = z + kMonumentStepZ[dir];
          const int32_t ny = y + (dir == kMonumentUp ? 1 : (dir == kMonumentDown ? -1 : 0));
          if (nx < 0 || nx >= 5 || nz < 0 || nz >= 5 || ny < 0 || ny >= 3)
            continue;
          const int32_t neighbour = monument_room_index(nx, ny, nz);
          MonumentRoomSlot& other = slots[static_cast<size_t>(neighbour)];
          if (!other.present)
            continue;
          if (nz == z)
            monument_set_connection(slots, room, dir, other);
          else
            monument_set_connection(slots, room, kMonumentOpposite[dir], other);
        }
      }
    }
  }

  // The three special definitions the building appends to the graph. Their
  // indexes are 1003/1001/1002, in that construction order.
  MonumentRoomSlot& roof = slots[kMonumentGridSize];
  MonumentRoomSlot& left = slots[kMonumentGridSize + 1];
  MonumentRoomSlot& right = slots[kMonumentGridSize + 2];
  roof.present = true;
  roof.index = kMonumentRoofIndex;
  left.present = true;
  left.index = kMonumentLeftWingIndex;
  right.present = true;
  right.index = kMonumentRightWingIndex;
  monument_set_connection(slots, slots[static_cast<size_t>(monument_room_index(2, 2, 0))],
                          kMonumentUp, roof);
  monument_set_connection(slots, slots[static_cast<size_t>(monument_room_index(0, 1, 0))],
                          kMonumentSouth, left);
  monument_set_connection(slots, slots[static_cast<size_t>(monument_room_index(4, 1, 0))],
                          kMonumentSouth, right);
  roof.claimed = 1;
  left.claimed = 1;
  right.claimed = 1;

  MonumentRoomSlot& source = slots[static_cast<size_t>(source_index)];
  source.is_source = 1;
  const int32_t core_index = monument_room_index(random.next_int(4), 0, 2);
  MonumentRoomSlot& core = slots[static_cast<size_t>(core_index)];
  core.claimed = 1;
  auto claim_from = [&slots](MonumentRoomSlot& room, int32_t direction) -> MonumentRoomSlot& {
    return slots[static_cast<size_t>(monument_slot_for_index(slots, room.connections[direction]))];
  };
  MonumentRoomSlot& core_east = claim_from(core, kMonumentEast);
  core_east.claimed = 1;
  MonumentRoomSlot& core_north = claim_from(core, kMonumentNorth);
  core_north.claimed = 1;
  MonumentRoomSlot& core_east_north = claim_from(core_east, kMonumentNorth);
  core_east_north.claimed = 1;
  MonumentRoomSlot& core_up = claim_from(core, kMonumentUp);
  core_up.claimed = 1;
  MonumentRoomSlot& core_east_up = claim_from(core_east, kMonumentUp);
  core_east_up.claimed = 1;
  MonumentRoomSlot& core_north_up = claim_from(core_north, kMonumentUp);
  core_north_up.claimed = 1;
  MonumentRoomSlot& core_east_north_up = claim_from(core_east_north, kMonumentUp);
  core_east_north_up.claimed = 1;

  std::vector<int32_t> room_order;
  room_order.reserve(49);
  for (int32_t i = 0; i < kMonumentGridSize; ++i) {
    MonumentRoomSlot& room = slots[static_cast<size_t>(i)];
    if (!room.present)
      continue;
    monument_update_openings(room);
    room_order.push_back(i);
  }
  monument_update_openings(roof);
  // `Util.shuffle`: descending Fisher-Yates, `nextInt(i)` for i = size..2.
  for (int32_t i = static_cast<int32_t>(room_order.size()); i > 1; --i) {
    const int32_t swap_to = random.next_int(i);
    std::swap(room_order[static_cast<size_t>(i - 1)], room_order[static_cast<size_t>(swap_to)]);
  }

  // Close two doors on every room (at most five attempts), each close only
  // accepted when both sides can still reach the source. `scanIndex` is the
  // caller's post-incremented value; the second side is evaluated only when
  // the first succeeded.
  int32_t scan_index = 1;
  for (const int32_t slot_index : room_order) {
    MonumentRoomSlot& room = slots[static_cast<size_t>(slot_index)];
    int32_t close_count = 0;
    for (int32_t attempt = 0; close_count < 2 && attempt < 5; ++attempt) {
      const int32_t f = random.next_int(6);
      if (!monument_open(room, f))
        continue;
      const int32_t other_index = room.connections[f];
      if (other_index < 0)
        continue;
      const int32_t other_slot = monument_slot_for_index(slots, other_index);
      if (other_slot < 0)
        continue;
      MonumentRoomSlot& other = slots[static_cast<size_t>(other_slot)];
      const int32_t opposite = kMonumentOpposite[f];
      room.has_opening = static_cast<uint8_t>(room.has_opening & ~(1u << f));
      other.has_opening = static_cast<uint8_t>(other.has_opening & ~(1u << opposite));
      const int32_t first_scan = scan_index++;
      const bool first = monument_find_source(slots, slot_index, first_scan);
      bool second = false;
      if (first) {
        const int32_t second_scan = scan_index++;
        second = monument_find_source(slots, other_slot, second_scan);
      }
      if (first && second) {
        ++close_count;
        continue;
      }
      room.has_opening = static_cast<uint8_t>(room.has_opening | (1u << f));
      other.has_opening = static_cast<uint8_t>(other.has_opening | (1u << opposite));
    }
  }
  // The constructor iterates the returned list, including the three specials;
  // they are claimed, so the fitter loop skips them.
  room_order.push_back(kMonumentGridSize);
  room_order.push_back(kMonumentGridSize + 1);
  room_order.push_back(kMonumentGridSize + 2);

  source.claimed = 1;

  // `new OceanMonumentEntryRoom` and `new OceanMonumentCoreRoom`.
  const auto add_room_piece = [&start, direction](WgOceanMonumentPieceKind kind,
                                                  const MonumentRoomSlot& room, int32_t width,
                                                  int32_t height, int32_t depth) {
    WgOceanMonumentPiece piece;
    piece.kind = kind;
    piece.direction = direction;
    piece.room_index = room.index;
    monument_set_piece_box(piece, monument_room_box(direction, room, width, height, depth));
    start.pieces.push_back(piece);
    return start.pieces.size() - 1;
  };
  add_room_piece(WgOceanMonumentPieceKind::kEntry, source, 1, 1, 1);
  add_room_piece(WgOceanMonumentPieceKind::kCore, core, 2, 2, 2);

  // The seven fitters in `MonumentBuilding`'s order. Each fit is evaluated
  // before the next, and a fitter's `create` claims the rooms it consumes and
  // (for a simple room) draws `mainDesign`.
  for (const int32_t slot_index : room_order) {
    const MonumentRoomSlot& room = slots[static_cast<size_t>(slot_index)];
    if (room.claimed || room.index >= kMonumentGridSize)
      continue;
    const auto conn = [&slots, &room](int32_t dir) -> const MonumentRoomSlot& {
      const int32_t index = room.connections[dir];
      return slots[static_cast<size_t>(monument_slot_for_index(slots, index))];
    };
    bool made = false;
    // FitDoubleXYRoom: east and up, plus the east room's up.
    if (!made && monument_open(room, kMonumentEast) && !conn(kMonumentEast).claimed &&
        monument_open(room, kMonumentUp) && !conn(kMonumentUp).claimed) {
      const MonumentRoomSlot& east = conn(kMonumentEast);
      if (monument_open(east, kMonumentUp) &&
          !slots[static_cast<size_t>(monument_slot_for_index(slots, east.connections[kMonumentUp]))]
               .claimed) {
        slots[static_cast<size_t>(slot_index)].claimed = 1;
        slots[static_cast<size_t>(monument_slot_for_index(slots, room.connections[kMonumentEast]))]
            .claimed = 1;
        slots[static_cast<size_t>(monument_slot_for_index(slots, room.connections[kMonumentUp]))]
            .claimed = 1;
        slots[static_cast<size_t>(
                  monument_slot_for_index(slots, conn(kMonumentEast).connections[kMonumentUp]))]
            .claimed = 1;
        add_room_piece(WgOceanMonumentPieceKind::kDoubleXY, room, 2, 2, 1);
        made = true;
      }
    }
    // FitDoubleYZRoom: north and up, plus the north room's up.
    if (!made && monument_open(room, kMonumentNorth) && !conn(kMonumentNorth).claimed &&
        monument_open(room, kMonumentUp) && !conn(kMonumentUp).claimed) {
      const MonumentRoomSlot& north = conn(kMonumentNorth);
      if (monument_open(north, kMonumentUp) && !slots[static_cast<size_t>(monument_slot_for_index(
                                                          slots, north.connections[kMonumentUp]))]
                                                    .claimed) {
        slots[static_cast<size_t>(slot_index)].claimed = 1;
        slots[static_cast<size_t>(monument_slot_for_index(slots, room.connections[kMonumentNorth]))]
            .claimed = 1;
        slots[static_cast<size_t>(monument_slot_for_index(slots, room.connections[kMonumentUp]))]
            .claimed = 1;
        slots[static_cast<size_t>(
                  monument_slot_for_index(slots, conn(kMonumentNorth).connections[kMonumentUp]))]
            .claimed = 1;
        add_room_piece(WgOceanMonumentPieceKind::kDoubleYZ, room, 1, 2, 2);
        made = true;
      }
    }
    // FitDoubleZRoom: a north opening only. Its `create` picks the source
    // definition again when the fit was on the south room, which cannot happen
    // for a fit that required `hasOpening[NORTH]`, but the branch is kept.
    if (!made && monument_open(room, kMonumentNorth) && !conn(kMonumentNorth).claimed) {
      const MonumentRoomSlot& chosen = room;
      slots[static_cast<size_t>(slot_index)].claimed = 1;
      slots[static_cast<size_t>(monument_slot_for_index(slots, chosen.connections[kMonumentNorth]))]
          .claimed = 1;
      add_room_piece(WgOceanMonumentPieceKind::kDoubleZ, chosen, 1, 1, 2);
      made = true;
    }
    // FitDoubleXRoom: an east opening only.
    if (!made && monument_open(room, kMonumentEast) && !conn(kMonumentEast).claimed) {
      slots[static_cast<size_t>(slot_index)].claimed = 1;
      slots[static_cast<size_t>(monument_slot_for_index(slots, room.connections[kMonumentEast]))]
          .claimed = 1;
      add_room_piece(WgOceanMonumentPieceKind::kDoubleX, room, 2, 1, 1);
      made = true;
    }
    // FitDoubleYRoom: an up opening only.
    if (!made && monument_open(room, kMonumentUp) && !conn(kMonumentUp).claimed) {
      slots[static_cast<size_t>(slot_index)].claimed = 1;
      slots[static_cast<size_t>(monument_slot_for_index(slots, room.connections[kMonumentUp]))]
          .claimed = 1;
      add_room_piece(WgOceanMonumentPieceKind::kDoubleY, room, 1, 2, 1);
      made = true;
    }
    // FitSimpleTopRoom: no west/east/north/south/up openings (down is free).
    if (!made && !monument_open(room, kMonumentWest) && !monument_open(room, kMonumentEast) &&
        !monument_open(room, kMonumentNorth) && !monument_open(room, kMonumentSouth) &&
        !monument_open(room, kMonumentUp)) {
      slots[static_cast<size_t>(slot_index)].claimed = 1;
      add_room_piece(WgOceanMonumentPieceKind::kSimpleTop, room, 1, 1, 1);
      made = true;
    }
    // FitSimpleRoom: the catch-all, with its `nextInt(3)`.
    if (!made) {
      slots[static_cast<size_t>(slot_index)].claimed = 1;
      const size_t piece_index = add_room_piece(WgOceanMonumentPieceKind::kSimple, room, 1, 1, 1);
      start.pieces[piece_index].main_design = static_cast<uint8_t>(random.next_int(3));
    }
  }

  // `BlockPos.MutableBlockPos offset = this.getWorldPos(9, 0, 22)`; every child
  // box is moved by it. The offset's y is the building box's minY (39).
  const XZ offset_xz = monument_world_pos(building_box, direction, 9, 22);
  for (WgOceanMonumentPiece& piece : start.pieces) {
    WgPieceBox box{piece.min_x, piece.min_y, piece.min_z, piece.max_x, piece.max_y, piece.max_z};
    monument_move_box(box, offset_xz.x, building_box.min_y, offset_xz.z);
    monument_set_piece_box(piece, box);
  }

  // The two 24 x 21 wings and the penthouse are built from the building box's
  // own world corners, after the child move loop.
  const XZ wing_a0 = monument_world_pos(building_box, direction, 1, 1);
  const XZ wing_a1 = monument_world_pos(building_box, direction, 23, 21);
  const XZ wing_b0 = monument_world_pos(building_box, direction, 34, 1);
  const XZ wing_b1 = monument_world_pos(building_box, direction, 56, 21);
  const XZ penthouse0 = monument_world_pos(building_box, direction, 22, 22);
  const XZ penthouse1 = monument_world_pos(building_box, direction, 35, 35);
  const WgPieceBox wing_a =
      monument_from_corners(wing_a0, building_box.min_y + 1, wing_a1, building_box.min_y + 8);
  const WgPieceBox wing_b =
      monument_from_corners(wing_b0, building_box.min_y + 1, wing_b1, building_box.min_y + 8);
  const WgPieceBox penthouse = monument_from_corners(penthouse0, building_box.min_y + 13,
                                                     penthouse1, building_box.min_y + 17);
  const int32_t wing_random = random.next_int();
  WgOceanMonumentPiece first_wing;
  first_wing.kind = WgOceanMonumentPieceKind::kWing;
  first_wing.direction = direction;
  first_wing.main_design = static_cast<uint8_t>(wing_random & 1);
  monument_set_piece_box(first_wing, wing_a);
  start.pieces.push_back(first_wing);
  WgOceanMonumentPiece second_wing;
  second_wing.kind = WgOceanMonumentPieceKind::kWing;
  second_wing.direction = direction;
  second_wing.main_design = static_cast<uint8_t>((wing_random + 1) & 1);
  monument_set_piece_box(second_wing, wing_b);
  start.pieces.push_back(second_wing);
  WgOceanMonumentPiece penthouse_piece;
  penthouse_piece.kind = WgOceanMonumentPieceKind::kPenthouse;
  penthouse_piece.direction = direction;
  monument_set_piece_box(penthouse_piece, penthouse);
  start.pieces.push_back(penthouse_piece);

  // Materialise the room graph in slot order (grid first, then roof/left/right);
  // the digest sorts by `index`, but the writer's lookups are order-free.
  start.rooms.reserve(49);
  for (int32_t i = 0; i < kMonumentRoomSlots; ++i) {
    const MonumentRoomSlot& slot = slots[static_cast<size_t>(i)];
    if (!slot.present)
      continue;
    WgMonumentRoom room;
    room.index = slot.index;
    room.has_opening = slot.has_opening;
    for (int32_t d = 0; d < 6; ++d)
      room.connections[d] = slot.connections[d];
    room.claimed = slot.claimed;
    room.is_source = slot.is_source;
    start.rooms.push_back(room);
  }

  start.valid = true;
  return start;
}

namespace {

// `StructurePiece.generateSmallDoor` for the four `SmallDoorType` values.
void stronghold_generate_small_door(ScatteredWriter& writer, WgLegacyRandom& random,
                                    uint8_t door_type, int32_t foot_x, int32_t foot_y,
                                    int32_t foot_z) {
  const BlockStateKey cave_air{"minecraft:cave_air", ""};
  const BlockStateKey stone_bricks{"minecraft:stone_bricks", ""};
  const BlockStateKey oak_door_lower{"minecraft:oak_door",
                                     "facing=north,half=lower,hinge=left,open=false,powered=false"};
  const BlockStateKey oak_door_upper{"minecraft:oak_door",
                                     "facing=north,half=upper,hinge=left,open=false,powered=false"};
  const BlockStateKey iron_door_lower{
      "minecraft:iron_door", "facing=north,half=lower,hinge=left,open=false,powered=false"};
  const BlockStateKey iron_door_upper{
      "minecraft:iron_door", "facing=north,half=upper,hinge=left,open=false,powered=false"};
  (void)random; // the door type's own draw happened when the piece was created
  switch (door_type) {
  case 0: // OPENING
    writer.generate_box(foot_x, foot_y, foot_z, foot_x + 2, foot_y + 2, foot_z, cave_air, cave_air);
    break;
  case 1: // WOOD_DOOR
    writer.place_public(stone_bricks, foot_x, foot_y, foot_z);
    writer.place_public(stone_bricks, foot_x, foot_y + 1, foot_z);
    writer.place_public(stone_bricks, foot_x, foot_y + 2, foot_z);
    writer.place_public(stone_bricks, foot_x + 1, foot_y + 2, foot_z);
    writer.place_public(stone_bricks, foot_x + 2, foot_y + 2, foot_z);
    writer.place_public(stone_bricks, foot_x + 2, foot_y + 1, foot_z);
    writer.place_public(stone_bricks, foot_x + 2, foot_y, foot_z);
    writer.place_public(oak_door_lower, foot_x + 1, foot_y, foot_z);
    writer.place_public(oak_door_upper, foot_x + 1, foot_y + 1, foot_z);
    break;
  case 2: { // GRATES
    writer.place_public(cave_air, foot_x + 1, foot_y, foot_z);
    writer.place_public(cave_air, foot_x + 1, foot_y + 1, foot_z);
    const BlockStateKey west_bar{"minecraft:iron_bars",
                                 "east=false,north=false,south=false,waterlogged=false,west=true"};
    const BlockStateKey east_bar{"minecraft:iron_bars",
                                 "east=true,north=false,south=false,waterlogged=false,west=false"};
    const BlockStateKey east_west_bar{
        "minecraft:iron_bars", "east=true,north=false,south=false,waterlogged=false,west=true"};
    writer.place_public(west_bar, foot_x, foot_y, foot_z);
    writer.place_public(west_bar, foot_x, foot_y + 1, foot_z);
    writer.place_public(east_west_bar, foot_x, foot_y + 2, foot_z);
    writer.place_public(east_west_bar, foot_x + 1, foot_y + 2, foot_z);
    writer.place_public(east_west_bar, foot_x + 2, foot_y + 2, foot_z);
    writer.place_public(east_bar, foot_x + 2, foot_y + 1, foot_z);
    writer.place_public(east_bar, foot_x + 2, foot_y, foot_z);
    break;
  }
  default: { // IRON_DOOR
    writer.place_public(stone_bricks, foot_x, foot_y, foot_z);
    writer.place_public(stone_bricks, foot_x, foot_y + 1, foot_z);
    writer.place_public(stone_bricks, foot_x, foot_y + 2, foot_z);
    writer.place_public(stone_bricks, foot_x + 1, foot_y + 2, foot_z);
    writer.place_public(stone_bricks, foot_x + 2, foot_y + 2, foot_z);
    writer.place_public(stone_bricks, foot_x + 2, foot_y + 1, foot_z);
    writer.place_public(stone_bricks, foot_x + 2, foot_y, foot_z);
    writer.place_public(iron_door_lower, foot_x + 1, foot_y, foot_z);
    writer.place_public(iron_door_upper, foot_x + 1, foot_y + 1, foot_z);
    writer.place_public(
        BlockStateKey{"minecraft:stone_button", "face=wall,facing=north,powered=false"}, foot_x + 2,
        foot_y + 1, foot_z + 1);
    writer.place_public(
        BlockStateKey{"minecraft:stone_button", "face=wall,facing=south,powered=false"}, foot_x + 2,
        foot_y + 1, foot_z - 1);
    break;
  }
  }
}

BlockStateKey stronghold_wall_torch(const char* facing) {
  return BlockStateKey{"minecraft:wall_torch", std::string("facing=") + facing};
}

BlockStateKey stronghold_slab(bool double_slab) {
  return BlockStateKey{"minecraft:smooth_stone_slab", double_slab
                                                          ? "type=double,waterlogged=false"
                                                          : "type=bottom,waterlogged=false"};
}

BlockStateKey stronghold_stone_brick_slab() {
  return BlockStateKey{"minecraft:stone_brick_slab", "type=bottom,waterlogged=false"};
}

BlockStateKey stronghold_bars(bool north, bool south, bool east, bool west) {
  std::string properties = "east=";
  properties += east ? "true" : "false";
  properties += ",north=";
  properties += north ? "true" : "false";
  properties += ",south=";
  properties += south ? "true" : "false";
  properties += ",waterlogged=false,west=";
  properties += west ? "true" : "false";
  return BlockStateKey{"minecraft:iron_bars", std::move(properties)};
}

BlockStateKey stronghold_fence(bool north, bool south, bool east, bool west) {
  std::string properties = "east=";
  properties += east ? "true" : "false";
  properties += ",north=";
  properties += north ? "true" : "false";
  properties += ",south=";
  properties += south ? "true" : "false";
  properties += ",waterlogged=false,west=";
  properties += west ? "true" : "false";
  return BlockStateKey{"minecraft:oak_fence", std::move(properties)};
}

BlockStateKey stronghold_stairs(const char* name, const char* facing) {
  return BlockStateKey{name, std::string("facing=") + facing +
                                 ",half=bottom,shape=straight,waterlogged=false"};
}

BlockStateKey stronghold_ladder(const char* facing) {
  return BlockStateKey{"minecraft:ladder", std::string("facing=") + facing + ",waterlogged=false"};
}

// `StrongholdPieces`' writers over one piece. The local x/z/y order is vanilla's
// `generateBox` order; `ScatteredWriter` supplies the orientation transform and
// clips every placement to the target chunk.
struct StrongholdWriter {
  Chunk& chunk;
  const WgStrongholdPiece& piece;
  int64_t world_seed;
  ScatteredWriter writer;
  WgLegacyRandom random;
  uint32_t placed = 0;

  StrongholdWriter(Chunk& chunk_in, const WgStrongholdPiece& piece_in, int64_t world_seed_in)
      : chunk(chunk_in), piece(piece_in), world_seed(world_seed_in),
        writer(chunk_in, piece_in.direction, piece_in.min_x, piece_in.max_x, piece_in.min_z,
               piece_in.max_z, piece_in.min_y),
        random(wg_mth_get_seed(piece_in.min_x, piece_in.min_y, piece_in.min_z)) {}

  BlockStateKey air() const {
    return BlockStateKey{"minecraft:cave_air", ""};
  }
  BlockStateKey stone_bricks() const {
    return BlockStateKey{"minecraft:stone_bricks", ""};
  }
  BlockStateKey stone_brick_slab() const {
    return stronghold_stone_brick_slab();
  }

  void door(uint8_t type, int32_t x, int32_t y, int32_t z) {
    stronghold_generate_small_door(writer, random, type, x, y, z);
  }

  void chest(int32_t local_x, int32_t local_y, int32_t local_z, const char* table) {
    const auto [world_x, world_z] = writer.world_xz(local_x, local_z);
    const int32_t base_x = chunk.x * 16;
    const int32_t base_z = chunk.z * 16;
    if (world_x < base_x || world_x >= base_x + 16 || world_z < base_z || world_z >= base_z + 16)
      return;
    if (chunk.block_state_at(world_x - base_x, piece.min_y + local_y, world_z - base_z).name ==
        "minecraft:chest")
      return;
    // `createChest`'s loot-table draw is part of the piece's stream; the port
    // uses a positional seed for the table itself, but keeps the draw so a
    // later cell in the same piece cannot move.
    (void)random.next_long();
    const char* facing =
        reoriented_chest_facing(chunk, world_x - base_x, piece.min_y + local_y, world_z - base_z);
    writer.place_raw(BlockStateKey{"minecraft:chest", std::string("facing=") + facing +
                                                          ",type=single,waterlogged=false"},
                     local_x, local_y, local_z);
    if (chunk.block_state_at(world_x - base_x, piece.min_y + local_y, world_z - base_z).name ==
        "minecraft:chest")
      add_chest_loot(chunk, world_x, piece.min_y + local_y, world_z, table, world_seed);
  }

  void spawner(int32_t local_x, int32_t local_y, int32_t local_z) {
    const auto [world_x, world_z] = writer.world_xz(local_x, local_z);
    const int32_t base_x = chunk.x * 16;
    const int32_t base_z = chunk.z * 16;
    if (world_x < base_x || world_x >= base_x + 16 || world_z < base_z || world_z >= base_z + 16)
      return;
    writer.place_raw(BlockStateKey{"minecraft:spawner", ""}, local_x, local_y, local_z);
    if (chunk.block_state_at(world_x - base_x, piece.min_y + local_y, world_z - base_z).name !=
        "minecraft:spawner")
      return;
    for (const nbt::Value& be : chunk.block_entities)
      if (block_entity_matches(be, world_x, piece.min_y + local_y, world_z))
        return;
    using namespace nbt;
    nbt::Value entity = make_compound(
        std::vector<std::pair<std::string, Value>>{{"id", make_string("minecraft:silverfish")}});
    nbt::Value spawn_data =
        make_compound(std::vector<std::pair<std::string, Value>>{{"entity", std::move(entity)}});
    chunk.block_entities.push_back(make_compound(
        std::vector<std::pair<std::string, Value>>{{"id", make_string("minecraft:spawner")},
                                                   {"x", make_int(world_x)},
                                                   {"y", make_int(piece.min_y + local_y)},
                                                   {"z", make_int(world_z)},
                                                   {"SpawnData", std::move(spawn_data)}}));
  }

  void straight() {
    writer.generate_smooth_box(0, 0, 0, 4, 4, 6, true, random);
    door(piece.entry_door, 1, 1, 0);
    door(0, 1, 1, 6);
    const BlockStateKey east = stronghold_wall_torch("east");
    const BlockStateKey west = stronghold_wall_torch("west");
    writer.maybe_generate_block(random, 0.1f, 1, 2, 1, east);
    writer.maybe_generate_block(random, 0.1f, 3, 2, 1, west);
    writer.maybe_generate_block(random, 0.1f, 1, 2, 5, east);
    writer.maybe_generate_block(random, 0.1f, 3, 2, 5, west);
    if (piece.left)
      writer.generate_box(0, 1, 2, 0, 3, 4, air(), air());
    if (piece.right)
      writer.generate_box(4, 1, 2, 4, 3, 4, air(), air());
  }

  void prison_hall() {
    writer.generate_smooth_box(0, 0, 0, 8, 4, 10, true, random);
    door(piece.entry_door, 1, 1, 0);
    writer.generate_box(1, 1, 10, 3, 3, 10, air(), air());
    writer.generate_smooth_box(4, 1, 1, 4, 3, 1, false, random);
    writer.generate_smooth_box(4, 1, 3, 4, 3, 3, false, random);
    writer.generate_smooth_box(4, 1, 7, 4, 3, 7, false, random);
    writer.generate_smooth_box(4, 1, 9, 4, 3, 9, false, random);
    for (int32_t y = 1; y <= 3; ++y) {
      writer.place_public(stronghold_bars(true, true, false, false), 4, y, 4);
      writer.place_public(stronghold_bars(true, true, true, false), 4, y, 5);
      writer.place_public(stronghold_bars(true, true, false, false), 4, y, 6);
      writer.place_public(stronghold_bars(false, false, true, true), 5, y, 5);
      writer.place_public(stronghold_bars(false, false, true, true), 6, y, 5);
      writer.place_public(stronghold_bars(false, false, true, true), 7, y, 5);
    }
    writer.place_public(stronghold_bars(true, true, false, false), 4, 3, 2);
    writer.place_public(stronghold_bars(true, true, false, false), 4, 3, 8);
    const BlockStateKey door_bottom{"minecraft:iron_door",
                                    "facing=west,half=lower,hinge=left,open=false,powered=false"};
    const BlockStateKey door_top{"minecraft:iron_door",
                                 "facing=west,half=upper,hinge=left,open=false,powered=false"};
    writer.place_public(door_bottom, 4, 1, 2);
    writer.place_public(door_top, 4, 2, 2);
    writer.place_public(door_bottom, 4, 1, 8);
    writer.place_public(door_top, 4, 2, 8);
  }

  void turn(bool left_turn) {
    writer.generate_smooth_box(0, 0, 0, 4, 4, 4, true, random);
    door(piece.entry_door, 1, 1, 0);
    const bool north_or_east = piece.direction == 0 || piece.direction == 1;
    const bool air_on_left = left_turn ? north_or_east : !north_or_east;
    if (air_on_left)
      writer.generate_box(0, 1, 1, 0, 3, 3, air(), air());
    else
      writer.generate_box(4, 1, 1, 4, 3, 3, air(), air());
  }

  void room_crossing() {
    writer.generate_smooth_box(0, 0, 0, 10, 6, 10, true, random);
    door(piece.entry_door, 4, 1, 0);
    writer.generate_box(4, 1, 10, 6, 3, 10, air(), air());
    writer.generate_box(0, 1, 4, 0, 3, 6, air(), air());
    writer.generate_box(10, 1, 4, 10, 3, 6, air(), air());
    switch (piece.room_type) {
    case 0:
      writer.place_public(stone_bricks(), 5, 1, 5);
      writer.place_public(stone_bricks(), 5, 2, 5);
      writer.place_public(stone_bricks(), 5, 3, 5);
      writer.place_public(stronghold_wall_torch("west"), 4, 3, 5);
      writer.place_public(stronghold_wall_torch("east"), 6, 3, 5);
      writer.place_public(stronghold_wall_torch("south"), 5, 3, 4);
      writer.place_public(stronghold_wall_torch("north"), 5, 3, 6);
      writer.place_public(stronghold_slab(false), 4, 1, 4);
      writer.place_public(stronghold_slab(false), 4, 1, 5);
      writer.place_public(stronghold_slab(false), 4, 1, 6);
      writer.place_public(stronghold_slab(false), 6, 1, 4);
      writer.place_public(stronghold_slab(false), 6, 1, 5);
      writer.place_public(stronghold_slab(false), 6, 1, 6);
      writer.place_public(stronghold_slab(false), 5, 1, 4);
      writer.place_public(stronghold_slab(false), 5, 1, 6);
      break;
    case 1:
      for (int32_t i = 0; i < 5; ++i) {
        writer.place_public(stone_bricks(), 3, 1, 3 + i);
        writer.place_public(stone_bricks(), 7, 1, 3 + i);
        writer.place_public(stone_bricks(), 3 + i, 1, 3);
        writer.place_public(stone_bricks(), 3 + i, 1, 7);
      }
      writer.place_public(stone_bricks(), 5, 1, 5);
      writer.place_public(stone_bricks(), 5, 2, 5);
      writer.place_public(stone_bricks(), 5, 3, 5);
      writer.place_public(BlockStateKey{"minecraft:water", "level=0"}, 5, 4, 5);
      break;
    case 2: {
      const BlockStateKey cobble{"minecraft:cobblestone", ""};
      for (int32_t z = 1; z <= 9; ++z) {
        writer.place_public(cobble, 1, 3, z);
        writer.place_public(cobble, 9, 3, z);
      }
      for (int32_t x = 1; x <= 9; ++x) {
        writer.place_public(cobble, x, 3, 1);
        writer.place_public(cobble, x, 3, 9);
      }
      writer.place_public(cobble, 5, 1, 4);
      writer.place_public(cobble, 5, 1, 6);
      writer.place_public(cobble, 5, 3, 4);
      writer.place_public(cobble, 5, 3, 6);
      writer.place_public(cobble, 4, 1, 5);
      writer.place_public(cobble, 6, 1, 5);
      writer.place_public(cobble, 4, 3, 5);
      writer.place_public(cobble, 6, 3, 5);
      for (int32_t y = 1; y <= 3; ++y) {
        writer.place_public(cobble, 4, y, 4);
        writer.place_public(cobble, 6, y, 4);
        writer.place_public(cobble, 4, y, 6);
        writer.place_public(cobble, 6, y, 6);
      }
      writer.place_public(BlockStateKey{"minecraft:wall_torch", "facing=north"}, 5, 3, 5);
      const BlockStateKey planks{"minecraft:oak_planks", ""};
      for (int32_t z = 2; z <= 8; ++z) {
        writer.place_public(planks, 2, 3, z);
        writer.place_public(planks, 3, 3, z);
        if (z <= 3 || z >= 7) {
          writer.place_public(planks, 4, 3, z);
          writer.place_public(planks, 5, 3, z);
          writer.place_public(planks, 6, 3, z);
        }
        writer.place_public(planks, 7, 3, z);
        writer.place_public(planks, 8, 3, z);
      }
      const BlockStateKey ladder = stronghold_ladder("west");
      writer.place_public(ladder, 9, 1, 3);
      writer.place_public(ladder, 9, 2, 3);
      writer.place_public(ladder, 9, 3, 3);
      chest(3, 4, 8, "minecraft:chests/stronghold_crossing");
      break;
    }
    default:
      break;
    }
  }

  void straight_stairs_down() {
    writer.generate_smooth_box(0, 0, 0, 4, 10, 7, true, random);
    door(piece.entry_door, 1, 7, 0);
    door(0, 1, 1, 7);
    const BlockStateKey stairs = stronghold_stairs("minecraft:cobblestone_stairs", "south");
    for (int32_t i = 0; i < 6; ++i) {
      writer.place_public(stairs, 1, 6 - i, 1 + i);
      writer.place_public(stairs, 2, 6 - i, 1 + i);
      writer.place_public(stairs, 3, 6 - i, 1 + i);
      if (i >= 5)
        continue;
      writer.place_public(stone_bricks(), 1, 5 - i, 1 + i);
      writer.place_public(stone_bricks(), 2, 5 - i, 1 + i);
      writer.place_public(stone_bricks(), 3, 5 - i, 1 + i);
    }
  }

  void stairs_down() {
    writer.generate_smooth_box(0, 0, 0, 4, 10, 4, true, random);
    door(piece.entry_door, 1, 7, 0);
    door(0, 1, 1, 4);
    writer.place_public(stone_bricks(), 2, 6, 1);
    writer.place_public(stone_bricks(), 1, 5, 1);
    writer.place_public(stronghold_slab(false), 1, 6, 1);
    writer.place_public(stone_bricks(), 1, 5, 2);
    writer.place_public(stone_bricks(), 1, 4, 3);
    writer.place_public(stronghold_slab(false), 1, 5, 3);
    writer.place_public(stone_bricks(), 2, 4, 3);
    writer.place_public(stone_bricks(), 3, 3, 3);
    writer.place_public(stronghold_slab(false), 3, 4, 3);
    writer.place_public(stone_bricks(), 3, 3, 2);
    writer.place_public(stone_bricks(), 3, 2, 1);
    writer.place_public(stronghold_slab(false), 3, 3, 1);
    writer.place_public(stone_bricks(), 2, 2, 1);
    writer.place_public(stone_bricks(), 1, 1, 1);
    writer.place_public(stronghold_slab(false), 1, 2, 1);
    writer.place_public(stone_bricks(), 1, 1, 2);
    writer.place_public(stronghold_slab(false), 1, 1, 3);
  }

  void five_crossing() {
    writer.generate_smooth_box(0, 0, 0, 9, 8, 10, true, random);
    door(piece.entry_door, 4, 3, 0);
    if (piece.left)
      writer.generate_box(0, 3, 1, 0, 5, 3, air(), air());
    if (piece.right)
      writer.generate_box(9, 3, 1, 9, 5, 3, air(), air());
    if (piece.left_high)
      writer.generate_box(0, 5, 7, 0, 7, 9, air(), air());
    if (piece.right_high)
      writer.generate_box(9, 5, 7, 9, 7, 9, air(), air());
    writer.generate_box(5, 1, 10, 7, 3, 10, air(), air());
    writer.generate_smooth_box(1, 2, 1, 8, 2, 6, false, random);
    writer.generate_smooth_box(4, 1, 5, 4, 4, 9, false, random);
    writer.generate_smooth_box(8, 1, 5, 8, 4, 9, false, random);
    writer.generate_smooth_box(1, 4, 7, 3, 4, 9, false, random);
    writer.generate_smooth_box(1, 3, 5, 3, 3, 6, false, random);
    writer.generate_box(1, 3, 4, 3, 3, 4, stronghold_slab(false), stronghold_slab(false));
    writer.generate_box(1, 4, 6, 3, 4, 6, stronghold_slab(false), stronghold_slab(false));
    writer.generate_smooth_box(5, 1, 7, 7, 1, 8, false, random);
    writer.generate_box(5, 1, 9, 7, 1, 9, stronghold_slab(false), stronghold_slab(false));
    writer.generate_box(5, 2, 7, 7, 2, 7, stronghold_slab(false), stronghold_slab(false));
    writer.generate_box(4, 5, 7, 4, 5, 9, stronghold_slab(false), stronghold_slab(false));
    writer.generate_box(8, 5, 7, 8, 5, 9, stronghold_slab(false), stronghold_slab(false));
    writer.generate_box(5, 5, 7, 7, 5, 9, stronghold_slab(true), stronghold_slab(true));
    writer.place_public(stronghold_wall_torch("south"), 6, 5, 6);
  }

  void chest_corridor() {
    writer.generate_smooth_box(0, 0, 0, 4, 4, 6, true, random);
    door(piece.entry_door, 1, 1, 0);
    door(0, 1, 1, 6);
    writer.generate_box(3, 1, 2, 3, 1, 4, stone_bricks(), stone_bricks());
    writer.place_public(stone_brick_slab(), 3, 1, 1);
    writer.place_public(stone_brick_slab(), 3, 1, 5);
    writer.place_public(stone_brick_slab(), 3, 2, 2);
    writer.place_public(stone_brick_slab(), 3, 2, 4);
    for (int32_t z = 2; z <= 4; ++z)
      writer.place_public(stone_brick_slab(), 2, 1, z);
    chest(3, 2, 3, "minecraft:chests/stronghold_corridor");
  }

  void library() {
    const int32_t current_height = piece.is_tall ? 11 : 6;
    writer.generate_smooth_box(0, 0, 0, 13, current_height - 1, 14, true, random);
    door(piece.entry_door, 4, 1, 0);
    writer.generate_maybe_box(random, 0.07f, 2, 1, 1, 11, 4, 13,
                              BlockStateKey{"minecraft:cobweb", ""},
                              BlockStateKey{"minecraft:cobweb", ""}, false);
    const BlockStateKey planks{"minecraft:oak_planks", ""};
    const BlockStateKey bookshelf{"minecraft:bookshelf", ""};
    for (int32_t d = 1; d <= 13; ++d) {
      if ((d - 1) % 4 == 0) {
        writer.generate_box(1, 1, d, 1, 4, d, planks, planks);
        writer.generate_box(12, 1, d, 12, 4, d, planks, planks);
        writer.place_public(stronghold_wall_torch("east"), 2, 3, d);
        writer.place_public(stronghold_wall_torch("west"), 11, 3, d);
        if (!piece.is_tall)
          continue;
        writer.generate_box(1, 6, d, 1, 9, d, planks, planks);
        writer.generate_box(12, 6, d, 12, 9, d, planks, planks);
        continue;
      }
      writer.generate_box(1, 1, d, 1, 4, d, bookshelf, bookshelf);
      writer.generate_box(12, 1, d, 12, 4, d, bookshelf, bookshelf);
      if (!piece.is_tall)
        continue;
      writer.generate_box(1, 6, d, 1, 9, d, bookshelf, bookshelf);
      writer.generate_box(12, 6, d, 12, 9, d, bookshelf, bookshelf);
    }
    for (int32_t d = 3; d < 12; d += 2) {
      writer.generate_box(3, 1, d, 4, 3, d, bookshelf, bookshelf);
      writer.generate_box(6, 1, d, 7, 3, d, bookshelf, bookshelf);
      writer.generate_box(9, 1, d, 10, 3, d, bookshelf, bookshelf);
    }
    if (piece.is_tall) {
      writer.generate_box(1, 5, 1, 3, 5, 13, planks, planks);
      writer.generate_box(10, 5, 1, 12, 5, 13, planks, planks);
      writer.generate_box(4, 5, 1, 9, 5, 2, planks, planks);
      writer.generate_box(4, 5, 12, 9, 5, 13, planks, planks);
      writer.place_public(planks, 9, 5, 11);
      writer.place_public(planks, 8, 5, 11);
      writer.place_public(planks, 9, 5, 10);
      const BlockStateKey we_fence = stronghold_fence(false, false, true, true);
      const BlockStateKey ns_fence = stronghold_fence(true, true, false, false);
      writer.generate_box(3, 6, 3, 3, 6, 11, ns_fence, ns_fence);
      writer.generate_box(10, 6, 3, 10, 6, 9, ns_fence, ns_fence);
      writer.generate_box(4, 6, 2, 9, 6, 2, we_fence, we_fence);
      writer.generate_box(4, 6, 12, 7, 6, 12, we_fence, we_fence);
      writer.place_public(stronghold_fence(true, false, true, false), 3, 6, 2);
      writer.place_public(stronghold_fence(false, true, true, false), 3, 6, 12);
      writer.place_public(stronghold_fence(true, false, false, true), 10, 6, 2);
      for (int32_t i = 0; i <= 2; ++i) {
        writer.place_public(stronghold_fence(false, true, false, true), 8 + i, 6, 12 - i);
        if (i == 2)
          continue;
        writer.place_public(stronghold_fence(true, false, true, false), 8 + i, 6, 11 - i);
      }
      const BlockStateKey ladder = stronghold_ladder("south");
      for (int32_t y = 1; y <= 7; ++y)
        writer.place_public(ladder, 10, y, 13);
      const BlockStateKey e_fence = stronghold_fence(false, false, true, false);
      const BlockStateKey w_fence = stronghold_fence(false, false, false, true);
      writer.place_public(e_fence, 6, 9, 7);
      writer.place_public(w_fence, 7, 9, 7);
      writer.place_public(e_fence, 6, 8, 7);
      writer.place_public(w_fence, 7, 8, 7);
      const BlockStateKey nswe_fence = stronghold_fence(true, true, true, true);
      writer.place_public(nswe_fence, 6, 7, 7);
      writer.place_public(nswe_fence, 7, 7, 7);
      writer.place_public(e_fence, 5, 7, 7);
      writer.place_public(w_fence, 8, 7, 7);
      writer.place_public(stronghold_fence(true, false, true, false), 6, 7, 6);
      writer.place_public(stronghold_fence(false, true, true, false), 6, 7, 8);
      writer.place_public(stronghold_fence(true, false, false, true), 7, 7, 6);
      writer.place_public(stronghold_fence(false, true, false, true), 7, 7, 8);
      const BlockStateKey torch{"minecraft:torch", ""};
      writer.place_public(torch, 5, 8, 7);
      writer.place_public(torch, 8, 8, 7);
      writer.place_public(torch, 6, 8, 6);
      writer.place_public(torch, 6, 8, 8);
      writer.place_public(torch, 7, 8, 6);
      writer.place_public(torch, 7, 8, 8);
    }
    chest(3, 3, 5, "minecraft:chests/stronghold_library");
    if (piece.is_tall) {
      writer.place_public(air(), 12, 9, 1);
      chest(12, 8, 1, "minecraft:chests/stronghold_library");
    }
  }

  void portal_room() {
    writer.generate_smooth_box(0, 0, 0, 10, 7, 15, false, random);
    door(2, 4, 1, 0);
    writer.generate_smooth_box(1, 6, 1, 1, 6, 14, false, random);
    writer.generate_smooth_box(9, 6, 1, 9, 6, 14, false, random);
    writer.generate_smooth_box(2, 6, 1, 8, 6, 2, false, random);
    writer.generate_smooth_box(2, 6, 14, 8, 6, 14, false, random);
    writer.generate_smooth_box(1, 1, 1, 2, 1, 4, false, random);
    writer.generate_smooth_box(8, 1, 1, 9, 1, 4, false, random);
    const BlockStateKey lava{"minecraft:lava", "level=0"};
    writer.generate_box(1, 1, 1, 1, 1, 3, lava, lava);
    writer.generate_box(9, 1, 1, 9, 1, 3, lava, lava);
    writer.generate_smooth_box(3, 1, 8, 7, 1, 12, false, random);
    writer.generate_box(4, 1, 9, 6, 1, 11, lava, lava);
    const BlockStateKey ns_bars = stronghold_bars(true, true, false, false);
    const BlockStateKey we_bars = stronghold_bars(false, false, true, true);
    for (int32_t z = 3; z < 14; z += 2) {
      writer.generate_box(0, 3, z, 0, 4, z, ns_bars, ns_bars);
      writer.generate_box(10, 3, z, 10, 4, z, ns_bars, ns_bars);
    }
    for (int32_t x = 2; x < 9; x += 2)
      writer.generate_box(x, 3, 15, x, 4, 15, we_bars, we_bars);
    const BlockStateKey stairs = stronghold_stairs("minecraft:stone_brick_stairs", "north");
    writer.generate_smooth_box(4, 1, 5, 6, 1, 7, false, random);
    writer.generate_smooth_box(4, 2, 6, 6, 2, 7, false, random);
    writer.generate_smooth_box(4, 3, 7, 6, 3, 7, false, random);
    for (int32_t x = 4; x <= 6; ++x) {
      writer.place_public(stairs, x, 1, 4);
      writer.place_public(stairs, x, 2, 5);
      writer.place_public(stairs, x, 3, 6);
    }
    const BlockStateKey north_frame{"minecraft:end_portal_frame", "eye=true,facing=north"};
    const BlockStateKey north_frame_empty{"minecraft:end_portal_frame", "eye=false,facing=north"};
    const BlockStateKey south_frame{"minecraft:end_portal_frame", "eye=true,facing=south"};
    const BlockStateKey south_frame_empty{"minecraft:end_portal_frame", "eye=false,facing=south"};
    const BlockStateKey east_frame{"minecraft:end_portal_frame", "eye=true,facing=east"};
    const BlockStateKey east_frame_empty{"minecraft:end_portal_frame", "eye=false,facing=east"};
    const BlockStateKey west_frame{"minecraft:end_portal_frame", "eye=true,facing=west"};
    const BlockStateKey west_frame_empty{"minecraft:end_portal_frame", "eye=false,facing=west"};
    bool all_eyes = true;
    bool eyes[12];
    for (bool& eye : eyes) {
      eye = random.next_float() > 0.9f;
      all_eyes = all_eyes && eye;
    }
    writer.place_public(!eyes[0] ? north_frame_empty : north_frame, 4, 3, 8);
    writer.place_public(!eyes[1] ? north_frame_empty : north_frame, 5, 3, 8);
    writer.place_public(!eyes[2] ? north_frame_empty : north_frame, 6, 3, 8);
    writer.place_public(!eyes[3] ? south_frame_empty : south_frame, 4, 3, 12);
    writer.place_public(!eyes[4] ? south_frame_empty : south_frame, 5, 3, 12);
    writer.place_public(!eyes[5] ? south_frame_empty : south_frame, 6, 3, 12);
    writer.place_public(!eyes[6] ? east_frame_empty : east_frame, 3, 3, 9);
    writer.place_public(!eyes[7] ? east_frame_empty : east_frame, 3, 3, 10);
    writer.place_public(!eyes[8] ? east_frame_empty : east_frame, 3, 3, 11);
    writer.place_public(!eyes[9] ? west_frame_empty : west_frame, 7, 3, 9);
    writer.place_public(!eyes[10] ? west_frame_empty : west_frame, 7, 3, 10);
    writer.place_public(!eyes[11] ? west_frame_empty : west_frame, 7, 3, 11);
    if (all_eyes) {
      const BlockStateKey portal{"minecraft:end_portal", ""};
      for (int32_t y = 3; y <= 3; ++y)
        for (int32_t z = 9; z <= 11; ++z)
          for (int32_t x = 4; x <= 6; ++x)
            writer.place_public(portal, x, y, z);
    }
    spawner(5, 3, 6);
  }

  void filler_corridor() {
    const BlockStateKey cave_air{"minecraft:cave_air", ""};
    for (int32_t i = 0; i < piece.steps; ++i) {
      for (int32_t x = 0; x <= 4; ++x)
        writer.place_public(stone_bricks(), x, 0, i);
      for (int32_t y = 1; y <= 3; ++y) {
        writer.place_public(stone_bricks(), 0, y, i);
        writer.place_public(cave_air, 1, y, i);
        writer.place_public(cave_air, 2, y, i);
        writer.place_public(cave_air, 3, y, i);
        writer.place_public(stone_bricks(), 4, y, i);
      }
      for (int32_t x = 0; x <= 4; ++x)
        writer.place_public(stone_bricks(), x, 4, i);
    }
  }

  uint32_t run() {
    switch (piece.kind) {
    case WgStrongholdPieceKind::kStart:
    case WgStrongholdPieceKind::kStairsDown:
      stairs_down();
      break;
    case WgStrongholdPieceKind::kStraight:
      straight();
      break;
    case WgStrongholdPieceKind::kPrisonHall:
      prison_hall();
      break;
    case WgStrongholdPieceKind::kLeftTurn:
      turn(true);
      break;
    case WgStrongholdPieceKind::kRightTurn:
      turn(false);
      break;
    case WgStrongholdPieceKind::kRoomCrossing:
      room_crossing();
      break;
    case WgStrongholdPieceKind::kStraightStairsDown:
      straight_stairs_down();
      break;
    case WgStrongholdPieceKind::kFiveCrossing:
      five_crossing();
      break;
    case WgStrongholdPieceKind::kChestCorridor:
      chest_corridor();
      break;
    case WgStrongholdPieceKind::kLibrary:
      library();
      break;
    case WgStrongholdPieceKind::kPortalRoom:
      portal_room();
      break;
    case WgStrongholdPieceKind::kFillerCorridor:
      filler_corridor();
      break;
    }
    return writer.changed();
  }
};

// `OceanMonumentPieces`' writers. `ScatteredWriter` supplies the orientation
// transform, the block-state mirror/rotation pair and the block reads; this
// layer adds the three read-then-write helpers the monument's water shell uses.
struct MonumentWriter {
  Chunk& chunk;
  const WgOceanMonumentStart& start;
  const WgOceanMonumentPiece& piece;
  ScatteredWriter writer;
  WgLegacyRandom& random;

  MonumentWriter(Chunk& chunk_in, const WgOceanMonumentStart& start_in,
                 const WgOceanMonumentPiece& piece_in, WgLegacyRandom& random_in)
      : chunk(chunk_in), start(start_in), piece(piece_in),
        writer(chunk_in, piece_in.direction, piece_in.min_x, piece_in.max_x, piece_in.min_z,
               piece_in.max_z, piece_in.min_y),
        random(random_in) {}

  static BlockStateKey gray() {
    return BlockStateKey{"minecraft:prismarine", ""};
  }
  static BlockStateKey light() {
    return BlockStateKey{"minecraft:prismarine_bricks", ""};
  }
  static BlockStateKey black() {
    return BlockStateKey{"minecraft:dark_prismarine", ""};
  }
  static BlockStateKey lamp() {
    return BlockStateKey{"minecraft:sea_lantern", ""};
  }
  static BlockStateKey air() {
    return BlockStateKey{"minecraft:air", ""};
  }
  static BlockStateKey water() {
    return BlockStateKey{"minecraft:water", "level=0"};
  }
  static BlockStateKey wet_sponge() {
    return BlockStateKey{"minecraft:wet_sponge", ""};
  }
  static BlockStateKey gold() {
    return BlockStateKey{"minecraft:gold_block", ""};
  }

  uint32_t changed() const {
    return writer.changed();
  }

  const WgMonumentRoom* room(int32_t index) const {
    if (index < 0)
      return nullptr;
    for (const WgMonumentRoom& candidate : start.rooms)
      if (candidate.index == index)
        return &candidate;
    return nullptr;
  }

  const WgMonumentRoom& room_ref(int32_t index) const {
    return *room(index);
  }

  static bool open(const WgMonumentRoom& room, int32_t direction) {
    return (room.has_opening & (1u << direction)) != 0;
  }

  static int32_t conn(const WgMonumentRoom& room, int32_t direction) {
    return room.connections[direction];
  }

  static int32_t count_openings(const WgMonumentRoom& room) {
    int32_t count = 0;
    for (int32_t i = 0; i < 6; ++i)
      if (open(room, i))
        ++count;
    return count;
  }

  BlockStateKey get_block(int32_t x, int32_t y, int32_t z) const {
    return writer.get_block(x, y, z);
  }

  void place(const BlockStateKey& state, int32_t x, int32_t y, int32_t z) {
    writer.place_public(state, x, y, z);
  }

  void gen_box(int32_t x0, int32_t y0, int32_t z0, int32_t x1, int32_t y1, int32_t z1,
               const BlockStateKey& edge, const BlockStateKey& fill) {
    writer.generate_box(x0, y0, z0, x1, y1, z1, edge, fill, false);
  }

  bool chunk_intersects(int32_t x0, int32_t z0, int32_t x1, int32_t z1) const {
    const auto [wx0, wz0] = writer.world_xz(x0, z0);
    const auto [wx1, wz1] = writer.world_xz(x1, z1);
    const int32_t min_x = std::min(wx0, wx1);
    const int32_t max_x = std::max(wx0, wx1);
    const int32_t min_z = std::min(wz0, wz1);
    const int32_t max_z = std::max(wz0, wz1);
    const int32_t base_x = chunk.x * 16;
    const int32_t base_z = chunk.z * 16;
    return max_x >= base_x && min_x < base_x + 16 && max_z >= base_z && min_z < base_z + 16;
  }

  // `OceanMonumentPiece.generateWaterBox`: keep water and the three ice
  // blocks; above sea level replace anything else with air, below it with
  // water. The read is the target chunk's current cell (air outside it).
  void generate_water_box(int32_t x0, int32_t y0, int32_t z0, int32_t x1, int32_t y1, int32_t z1) {
    for (int32_t y = y0; y <= y1; ++y) {
      for (int32_t x = x0; x <= x1; ++x) {
        for (int32_t z = z0; z <= z1; ++z) {
          const BlockStateKey block = get_block(x, y, z);
          if (block.name == "minecraft:water" || block.name == "minecraft:ice" ||
              block.name == "minecraft:packed_ice" || block.name == "minecraft:blue_ice")
            continue;
          if (piece.min_y + y >= kMonumentSeaLevel)
            place(air(), x, y, z);
          else
            place(water(), x, y, z);
        }
      }
    }
  }

  // `OceanMonumentPiece.generateBoxOnFillOnly`: only cells already holding
  // water are replaced.
  void generate_box_on_fill_only(int32_t x0, int32_t y0, int32_t z0, int32_t x1, int32_t y1,
                                 int32_t z1, const BlockStateKey& target) {
    for (int32_t y = y0; y <= y1; ++y)
      for (int32_t x = x0; x <= x1; ++x)
        for (int32_t z = z0; z <= z1; ++z)
          if (get_block(x, y, z).name == "minecraft:water")
            place(target, x, y, z);
  }

  void generate_default_floor(int32_t x_off, int32_t z_off, bool down_opening) {
    if (down_opening) {
      gen_box(x_off + 0, 0, z_off + 0, x_off + 2, 0, z_off + 7, gray(), gray());
      gen_box(x_off + 5, 0, z_off + 0, x_off + 7, 0, z_off + 7, gray(), gray());
      gen_box(x_off + 3, 0, z_off + 0, x_off + 4, 0, z_off + 2, gray(), gray());
      gen_box(x_off + 3, 0, z_off + 5, x_off + 4, 0, z_off + 7, gray(), gray());
      gen_box(x_off + 3, 0, z_off + 2, x_off + 4, 0, z_off + 2, light(), light());
      gen_box(x_off + 3, 0, z_off + 5, x_off + 4, 0, z_off + 5, light(), light());
      gen_box(x_off + 2, 0, z_off + 3, x_off + 2, 0, z_off + 4, light(), light());
      gen_box(x_off + 5, 0, z_off + 3, x_off + 5, 0, z_off + 4, light(), light());
    } else {
      gen_box(x_off + 0, 0, z_off + 0, x_off + 7, 0, z_off + 7, gray(), gray());
    }
  }

  void fill_column_down(int32_t x, int32_t start_y, int32_t z) {
    writer.fill_column_down(light(), x, start_y, z);
  }

  // `MonumentBuilding.postProcess`'s own body.
  void building() {
    const int32_t water_height = std::max(kMonumentSeaLevel, 64) - piece.min_y;
    generate_water_box(0, 0, 0, 58, water_height, 58);
    generate_wing(false, 0);
    generate_wing(true, 33);
    generate_entrance_archs();
    generate_entrance_wall();
    generate_roof_piece();
    generate_lower_wall();
    generate_middle_wall();
    generate_upper_wall();
    for (int32_t pillar_x = 0; pillar_x < 7; ++pillar_x) {
      int32_t pillar_z = 0;
      while (pillar_z < 7) {
        if (pillar_z == 0 && pillar_x == 3)
          pillar_z = 6;
        const int32_t bx = pillar_x * 9;
        const int32_t bz = pillar_z * 9;
        for (int32_t w = 0; w < 4; ++w) {
          for (int32_t d = 0; d < 4; ++d) {
            place(light(), bx + w, 0, bz + d);
            fill_column_down(bx + w, -1, bz + d);
          }
        }
        if (pillar_x == 0 || pillar_x == 6) {
          ++pillar_z;
          continue;
        }
        pillar_z += 6;
      }
    }
    for (int32_t i = 0; i < 5; ++i) {
      generate_water_box(-1 - i, 0 + i * 2, -1 - i, -1 - i, 23, 58 + i);
      generate_water_box(58 + i, 0 + i * 2, -1 - i, 58 + i, 23, 58 + i);
      generate_water_box(0 - i, 0 + i * 2, -1 - i, 57 + i, 23, -1 - i);
      generate_water_box(0 - i, 0 + i * 2, 58 + i, 57 + i, 23, 58 + i);
    }
  }

  void generate_wing(bool is_flipped, int32_t xoff) {
    if (!chunk_intersects(xoff, 0, xoff + 23, 20))
      return;
    gen_box(xoff + 0, 0, 0, xoff + 24, 0, 20, gray(), gray());
    generate_water_box(xoff + 0, 1, 0, xoff + 24, 10, 20);
    for (int32_t i = 0; i < 4; ++i) {
      gen_box(xoff + i, i + 1, i, xoff + i, i + 1, 20, light(), light());
      gen_box(xoff + i + 7, i + 5, i + 7, xoff + i + 7, i + 5, 20, light(), light());
      gen_box(xoff + 17 - i, i + 5, i + 7, xoff + 17 - i, i + 5, 20, light(), light());
      gen_box(xoff + 24 - i, i + 1, i, xoff + 24 - i, i + 1, 20, light(), light());
      gen_box(xoff + i + 1, i + 1, i, xoff + 23 - i, i + 1, i, light(), light());
      gen_box(xoff + i + 8, i + 5, i + 7, xoff + 16 - i, i + 5, i + 7, light(), light());
    }
    gen_box(xoff + 4, 4, 4, xoff + 6, 4, 20, gray(), gray());
    gen_box(xoff + 7, 4, 4, xoff + 17, 4, 6, gray(), gray());
    gen_box(xoff + 18, 4, 4, xoff + 20, 4, 20, gray(), gray());
    gen_box(xoff + 11, 8, 11, xoff + 13, 8, 20, gray(), gray());
    place(light(), xoff + 12, 9, 12);
    place(light(), xoff + 12, 9, 15);
    place(light(), xoff + 12, 9, 18);
    const int32_t left_pos = xoff + (is_flipped ? 19 : 5);
    const int32_t right_pos = xoff + (is_flipped ? 5 : 19);
    for (int32_t z = 20; z >= 5; z -= 3)
      place(light(), left_pos, 5, z);
    for (int32_t z = 19; z >= 7; z -= 3)
      place(light(), right_pos, 5, z);
    for (int32_t i = 0; i < 4; ++i) {
      const int32_t pos = is_flipped ? xoff + 24 - (17 - i * 3) : xoff + 17 - i * 3;
      place(light(), pos, 5, 5);
    }
    place(light(), right_pos, 5, 5);
    gen_box(xoff + 11, 1, 12, xoff + 13, 7, 12, gray(), gray());
    gen_box(xoff + 12, 1, 11, xoff + 12, 7, 13, gray(), gray());
  }

  void generate_entrance_archs() {
    if (!chunk_intersects(22, 5, 35, 17))
      return;
    generate_water_box(25, 0, 0, 32, 8, 20);
    for (int32_t i = 0; i < 4; ++i) {
      gen_box(24, 2, 5 + i * 4, 24, 4, 5 + i * 4, light(), light());
      gen_box(22, 4, 5 + i * 4, 23, 4, 5 + i * 4, light(), light());
      place(light(), 25, 5, 5 + i * 4);
      place(light(), 26, 6, 5 + i * 4);
      place(lamp(), 26, 5, 5 + i * 4);
      gen_box(33, 2, 5 + i * 4, 33, 4, 5 + i * 4, light(), light());
      gen_box(34, 4, 5 + i * 4, 35, 4, 5 + i * 4, light(), light());
      place(light(), 32, 5, 5 + i * 4);
      place(light(), 31, 6, 5 + i * 4);
      place(lamp(), 31, 5, 5 + i * 4);
      gen_box(27, 6, 5 + i * 4, 30, 6, 5 + i * 4, gray(), gray());
    }
  }

  void generate_entrance_wall() {
    if (!chunk_intersects(15, 20, 42, 21))
      return;
    gen_box(15, 0, 21, 42, 0, 21, gray(), gray());
    generate_water_box(26, 1, 21, 31, 3, 21);
    gen_box(21, 12, 21, 36, 12, 21, gray(), gray());
    gen_box(17, 11, 21, 40, 11, 21, gray(), gray());
    gen_box(16, 10, 21, 41, 10, 21, gray(), gray());
    gen_box(15, 7, 21, 42, 9, 21, gray(), gray());
    gen_box(16, 6, 21, 41, 6, 21, gray(), gray());
    gen_box(17, 5, 21, 40, 5, 21, gray(), gray());
    gen_box(21, 4, 21, 36, 4, 21, gray(), gray());
    gen_box(22, 3, 21, 26, 3, 21, gray(), gray());
    gen_box(31, 3, 21, 35, 3, 21, gray(), gray());
    gen_box(23, 2, 21, 25, 2, 21, gray(), gray());
    gen_box(32, 2, 21, 34, 2, 21, gray(), gray());
    gen_box(28, 4, 20, 29, 4, 21, light(), light());
    place(light(), 27, 3, 21);
    place(light(), 30, 3, 21);
    place(light(), 26, 2, 21);
    place(light(), 31, 2, 21);
    place(light(), 25, 1, 21);
    place(light(), 32, 1, 21);
    for (int32_t i = 0; i < 7; ++i) {
      place(black(), 28 - i, 6 + i, 21);
      place(black(), 29 + i, 6 + i, 21);
    }
    for (int32_t i = 0; i < 4; ++i) {
      place(black(), 28 - i, 9 + i, 21);
      place(black(), 29 + i, 9 + i, 21);
    }
    place(black(), 28, 12, 21);
    place(black(), 29, 12, 21);
    for (int32_t i = 0; i < 3; ++i) {
      place(black(), 22 - i * 2, 8, 21);
      place(black(), 22 - i * 2, 9, 21);
      place(black(), 35 + i * 2, 8, 21);
      place(black(), 35 + i * 2, 9, 21);
    }
    generate_water_box(15, 13, 21, 42, 15, 21);
    generate_water_box(15, 1, 21, 15, 6, 21);
    generate_water_box(16, 1, 21, 16, 5, 21);
    generate_water_box(17, 1, 21, 20, 4, 21);
    generate_water_box(21, 1, 21, 21, 3, 21);
    generate_water_box(22, 1, 21, 22, 2, 21);
    generate_water_box(23, 1, 21, 24, 1, 21);
    generate_water_box(42, 1, 21, 42, 6, 21);
    generate_water_box(41, 1, 21, 41, 5, 21);
    generate_water_box(37, 1, 21, 40, 4, 21);
    generate_water_box(36, 1, 21, 36, 3, 21);
    generate_water_box(33, 1, 21, 34, 1, 21);
    generate_water_box(35, 1, 21, 35, 2, 21);
  }

  void generate_roof_piece() {
    if (!chunk_intersects(21, 21, 36, 36))
      return;
    gen_box(21, 0, 22, 36, 0, 36, gray(), gray());
    generate_water_box(21, 1, 22, 36, 23, 36);
    for (int32_t i = 0; i < 4; ++i) {
      gen_box(21 + i, 13 + i, 21 + i, 36 - i, 13 + i, 21 + i, light(), light());
      gen_box(21 + i, 13 + i, 36 - i, 36 - i, 13 + i, 36 - i, light(), light());
      gen_box(21 + i, 13 + i, 22 + i, 21 + i, 13 + i, 35 - i, light(), light());
      gen_box(36 - i, 13 + i, 22 + i, 36 - i, 13 + i, 35 - i, light(), light());
    }
    gen_box(25, 16, 25, 32, 16, 32, gray(), gray());
    gen_box(25, 17, 25, 25, 19, 25, light(), light());
    gen_box(32, 17, 25, 32, 19, 25, light(), light());
    gen_box(25, 17, 32, 25, 19, 32, light(), light());
    gen_box(32, 17, 32, 32, 19, 32, light(), light());
    place(light(), 26, 20, 26);
    place(light(), 27, 21, 27);
    place(lamp(), 27, 20, 27);
    place(light(), 26, 20, 31);
    place(light(), 27, 21, 30);
    place(lamp(), 27, 20, 30);
    place(light(), 31, 20, 31);
    place(light(), 30, 21, 30);
    place(lamp(), 30, 20, 30);
    place(light(), 31, 20, 26);
    place(light(), 30, 21, 27);
    place(lamp(), 30, 20, 27);
    gen_box(28, 21, 27, 29, 21, 27, gray(), gray());
    gen_box(27, 21, 28, 27, 21, 29, gray(), gray());
    gen_box(28, 21, 30, 29, 21, 30, gray(), gray());
    gen_box(30, 21, 28, 30, 21, 29, gray(), gray());
  }

  void generate_lower_wall() {
    if (chunk_intersects(0, 21, 6, 58)) {
      gen_box(0, 0, 21, 6, 0, 57, gray(), gray());
      generate_water_box(0, 1, 21, 6, 7, 57);
      gen_box(4, 4, 21, 6, 4, 53, gray(), gray());
      for (int32_t i = 0; i < 4; ++i)
        gen_box(i, i + 1, 21, i, i + 1, 57 - i, light(), light());
      for (int32_t z = 23; z < 53; z += 3)
        place(light(), 5, 5, z);
      place(light(), 5, 5, 52);
      for (int32_t i = 0; i < 4; ++i)
        gen_box(i, i + 1, 21, i, i + 1, 57 - i, light(), light());
      gen_box(4, 1, 52, 6, 3, 52, gray(), gray());
      gen_box(5, 1, 51, 5, 3, 53, gray(), gray());
    }
    if (chunk_intersects(51, 21, 58, 58)) {
      gen_box(51, 0, 21, 57, 0, 57, gray(), gray());
      generate_water_box(51, 1, 21, 57, 7, 57);
      gen_box(51, 4, 21, 53, 4, 53, gray(), gray());
      for (int32_t i = 0; i < 4; ++i)
        gen_box(57 - i, i + 1, 21, 57 - i, i + 1, 57 - i, light(), light());
      for (int32_t z = 23; z < 53; z += 3)
        place(light(), 52, 5, z);
      place(light(), 52, 5, 52);
      gen_box(51, 1, 52, 53, 3, 52, gray(), gray());
      gen_box(52, 1, 51, 52, 3, 53, gray(), gray());
    }
    if (chunk_intersects(0, 51, 57, 57)) {
      gen_box(7, 0, 51, 50, 0, 57, gray(), gray());
      generate_water_box(7, 1, 51, 50, 10, 57);
      for (int32_t i = 0; i < 4; ++i)
        gen_box(i + 1, i + 1, 57 - i, 56 - i, i + 1, 57 - i, light(), light());
    }
  }

  void generate_middle_wall() {
    if (chunk_intersects(7, 21, 13, 50)) {
      gen_box(7, 0, 21, 13, 0, 50, gray(), gray());
      generate_water_box(7, 1, 21, 13, 10, 50);
      gen_box(11, 8, 21, 13, 8, 53, gray(), gray());
      for (int32_t i = 0; i < 4; ++i)
        gen_box(i + 7, i + 5, 21, i + 7, i + 5, 54, light(), light());
      for (int32_t z = 21; z <= 45; z += 3)
        place(light(), 12, 9, z);
    }
    if (chunk_intersects(44, 21, 50, 54)) {
      gen_box(44, 0, 21, 50, 0, 50, gray(), gray());
      generate_water_box(44, 1, 21, 50, 10, 50);
      gen_box(44, 8, 21, 46, 8, 53, gray(), gray());
      for (int32_t i = 0; i < 4; ++i)
        gen_box(50 - i, i + 5, 21, 50 - i, i + 5, 54, light(), light());
      for (int32_t z = 21; z <= 45; z += 3)
        place(light(), 45, 9, z);
    }
    if (chunk_intersects(8, 44, 49, 54)) {
      gen_box(14, 0, 44, 43, 0, 50, gray(), gray());
      generate_water_box(14, 1, 44, 43, 10, 50);
      for (int32_t x = 12; x <= 45; x += 3) {
        place(light(), x, 9, 45);
        place(light(), x, 9, 52);
        if (x != 12 && x != 18 && x != 24 && x != 33 && x != 39 && x != 45)
          continue;
        place(light(), x, 9, 47);
        place(light(), x, 9, 50);
        place(light(), x, 10, 45);
        place(light(), x, 10, 46);
        place(light(), x, 10, 51);
        place(light(), x, 10, 52);
        place(light(), x, 11, 47);
        place(light(), x, 11, 50);
        place(light(), x, 12, 48);
        place(light(), x, 12, 49);
      }
      for (int32_t i = 0; i < 3; ++i)
        gen_box(8 + i, 5 + i, 54, 49 - i, 5 + i, 54, gray(), gray());
      gen_box(11, 8, 54, 46, 8, 54, light(), light());
      gen_box(14, 8, 44, 43, 8, 53, gray(), gray());
    }
  }

  void generate_upper_wall() {
    if (chunk_intersects(14, 21, 20, 43)) {
      gen_box(14, 0, 21, 20, 0, 43, gray(), gray());
      generate_water_box(14, 1, 22, 20, 14, 43);
      gen_box(18, 12, 22, 20, 12, 39, gray(), gray());
      gen_box(18, 12, 21, 20, 12, 21, light(), light());
      for (int32_t i = 0; i < 4; ++i)
        gen_box(i + 14, i + 9, 21, i + 14, i + 9, 43 - i, light(), light());
      for (int32_t z = 23; z <= 39; z += 3)
        place(light(), 19, 13, z);
    }
    if (chunk_intersects(37, 21, 43, 43)) {
      gen_box(37, 0, 21, 43, 0, 43, gray(), gray());
      generate_water_box(37, 1, 22, 43, 14, 43);
      gen_box(37, 12, 22, 39, 12, 39, gray(), gray());
      gen_box(37, 12, 21, 39, 12, 21, light(), light());
      for (int32_t i = 0; i < 4; ++i)
        gen_box(43 - i, i + 9, 21, 43 - i, i + 9, 43 - i, light(), light());
      for (int32_t z = 23; z <= 39; z += 3)
        place(light(), 38, 13, z);
    }
    if (chunk_intersects(15, 37, 42, 43)) {
      gen_box(21, 0, 37, 36, 0, 43, gray(), gray());
      generate_water_box(21, 1, 37, 36, 14, 43);
      gen_box(21, 12, 37, 36, 12, 39, gray(), gray());
      for (int32_t i = 0; i < 4; ++i)
        gen_box(15 + i, i + 9, 43 - i, 42 - i, i + 9, 43 - i, light(), light());
      for (int32_t x = 21; x <= 36; x += 3)
        place(light(), x, 13, 38);
    }
  }

  uint32_t run();

  void entry() {
    const WgMonumentRoom& def = room_ref(piece.room_index);
    gen_box(0, 3, 0, 2, 3, 7, light(), light());
    gen_box(5, 3, 0, 7, 3, 7, light(), light());
    gen_box(0, 2, 0, 1, 2, 7, light(), light());
    gen_box(6, 2, 0, 7, 2, 7, light(), light());
    gen_box(0, 1, 0, 0, 1, 7, light(), light());
    gen_box(7, 1, 0, 7, 1, 7, light(), light());
    gen_box(0, 1, 7, 7, 3, 7, light(), light());
    gen_box(1, 1, 0, 2, 3, 0, light(), light());
    gen_box(5, 1, 0, 6, 3, 0, light(), light());
    if (open(def, kMonumentNorth))
      generate_water_box(3, 1, 7, 4, 2, 7);
    if (open(def, kMonumentWest))
      generate_water_box(0, 1, 3, 1, 2, 4);
    if (open(def, kMonumentEast))
      generate_water_box(6, 1, 3, 7, 2, 4);
  }

  void core() {
    generate_box_on_fill_only(1, 8, 0, 14, 8, 14, gray());
    BlockStateKey block = light();
    gen_box(0, 7, 0, 0, 7, 15, block, block);
    gen_box(15, 7, 0, 15, 7, 15, block, block);
    gen_box(1, 7, 0, 15, 7, 0, block, block);
    gen_box(1, 7, 15, 14, 7, 15, block, block);
    for (int32_t y = 1; y <= 6; ++y) {
      block = light();
      if (y == 2 || y == 6)
        block = gray();
      for (int32_t x = 0; x <= 15; x += 15) {
        gen_box(x, y, 0, x, y, 1, block, block);
        gen_box(x, y, 6, x, y, 9, block, block);
        gen_box(x, y, 14, x, y, 15, block, block);
      }
      gen_box(1, y, 0, 1, y, 0, block, block);
      gen_box(6, y, 0, 9, y, 0, block, block);
      gen_box(14, y, 0, 14, y, 0, block, block);
      gen_box(1, y, 15, 14, y, 15, block, block);
    }
    gen_box(6, 3, 6, 9, 6, 9, black(), black());
    gen_box(7, 4, 7, 8, 5, 8, gold(), gold());
    for (int32_t y = 3; y <= 6; y += 3) {
      for (int32_t x = 6; x <= 9; x += 3) {
        place(lamp(), x, y, 6);
        place(lamp(), x, y, 9);
      }
    }
    gen_box(5, 1, 6, 5, 2, 6, light(), light());
    gen_box(5, 1, 9, 5, 2, 9, light(), light());
    gen_box(10, 1, 6, 10, 2, 6, light(), light());
    gen_box(10, 1, 9, 10, 2, 9, light(), light());
    gen_box(6, 1, 5, 6, 2, 5, light(), light());
    gen_box(9, 1, 5, 9, 2, 5, light(), light());
    gen_box(6, 1, 10, 6, 2, 10, light(), light());
    gen_box(9, 1, 10, 9, 2, 10, light(), light());
    gen_box(5, 2, 5, 5, 6, 5, light(), light());
    gen_box(5, 2, 10, 5, 6, 10, light(), light());
    gen_box(10, 2, 5, 10, 6, 5, light(), light());
    gen_box(10, 2, 10, 10, 6, 10, light(), light());
    gen_box(5, 7, 1, 5, 7, 6, light(), light());
    gen_box(10, 7, 1, 10, 7, 6, light(), light());
    gen_box(5, 7, 9, 5, 7, 14, light(), light());
    gen_box(10, 7, 9, 10, 7, 14, light(), light());
    gen_box(1, 7, 5, 6, 7, 5, light(), light());
    gen_box(1, 7, 10, 6, 7, 10, light(), light());
    gen_box(9, 7, 5, 14, 7, 5, light(), light());
    gen_box(9, 7, 10, 14, 7, 10, light(), light());
    gen_box(2, 1, 2, 2, 1, 3, light(), light());
    gen_box(3, 1, 2, 3, 1, 2, light(), light());
    gen_box(13, 1, 2, 13, 1, 3, light(), light());
    gen_box(12, 1, 2, 12, 1, 2, light(), light());
    gen_box(2, 1, 12, 2, 1, 13, light(), light());
    gen_box(3, 1, 13, 3, 1, 13, light(), light());
    gen_box(13, 1, 12, 13, 1, 13, light(), light());
    gen_box(12, 1, 13, 12, 1, 13, light(), light());
  }

  void double_yz() {
    const WgMonumentRoom& south = room_ref(piece.room_index);
    const WgMonumentRoom& north = room_ref(conn(south, kMonumentNorth));
    const WgMonumentRoom& north_up = room_ref(conn(north, kMonumentUp));
    const WgMonumentRoom& south_up = room_ref(conn(south, kMonumentUp));
    if (south.index / 25 > 0) {
      generate_default_floor(0, 8, open(north, kMonumentDown));
      generate_default_floor(0, 0, open(south, kMonumentDown));
    }
    if (conn(south_up, kMonumentUp) < 0)
      generate_box_on_fill_only(1, 8, 1, 6, 8, 7, gray());
    if (conn(north_up, kMonumentUp) < 0)
      generate_box_on_fill_only(1, 8, 8, 6, 8, 14, gray());
    for (int32_t y = 1; y <= 7; ++y) {
      BlockStateKey block = light();
      if (y == 2 || y == 6)
        block = gray();
      gen_box(0, y, 0, 0, y, 15, block, block);
      gen_box(7, y, 0, 7, y, 15, block, block);
      gen_box(1, y, 0, 6, y, 0, block, block);
      gen_box(1, y, 15, 6, y, 15, block, block);
    }
    for (int32_t y = 1; y <= 7; ++y) {
      BlockStateKey block = black();
      if (y == 2 || y == 6)
        block = lamp();
      gen_box(3, y, 7, 4, y, 8, block, block);
    }
    if (open(south, kMonumentSouth))
      generate_water_box(3, 1, 0, 4, 2, 0);
    if (open(south, kMonumentEast))
      generate_water_box(7, 1, 3, 7, 2, 4);
    if (open(south, kMonumentWest))
      generate_water_box(0, 1, 3, 0, 2, 4);
    if (open(north, kMonumentNorth))
      generate_water_box(3, 1, 15, 4, 2, 15);
    if (open(north, kMonumentWest))
      generate_water_box(0, 1, 11, 0, 2, 12);
    if (open(north, kMonumentEast))
      generate_water_box(7, 1, 11, 7, 2, 12);
    if (open(south_up, kMonumentSouth))
      generate_water_box(3, 5, 0, 4, 6, 0);
    if (open(south_up, kMonumentEast)) {
      generate_water_box(7, 5, 3, 7, 6, 4);
      gen_box(5, 4, 2, 6, 4, 5, light(), light());
      gen_box(6, 1, 2, 6, 3, 2, light(), light());
      gen_box(6, 1, 5, 6, 3, 5, light(), light());
    }
    if (open(south_up, kMonumentWest)) {
      generate_water_box(0, 5, 3, 0, 6, 4);
      gen_box(1, 4, 2, 2, 4, 5, light(), light());
      gen_box(1, 1, 2, 1, 3, 2, light(), light());
      gen_box(1, 1, 5, 1, 3, 5, light(), light());
    }
    if (open(north_up, kMonumentNorth))
      generate_water_box(3, 5, 15, 4, 6, 15);
    if (open(north_up, kMonumentWest)) {
      generate_water_box(0, 5, 11, 0, 6, 12);
      gen_box(1, 4, 10, 2, 4, 13, light(), light());
      gen_box(1, 1, 10, 1, 3, 10, light(), light());
      gen_box(1, 1, 13, 1, 3, 13, light(), light());
    }
    if (open(north_up, kMonumentEast)) {
      generate_water_box(7, 5, 11, 7, 6, 12);
      gen_box(5, 4, 10, 6, 4, 13, light(), light());
      gen_box(6, 1, 10, 6, 3, 10, light(), light());
      gen_box(6, 1, 13, 6, 3, 13, light(), light());
    }
  }

  void double_xy() {
    const WgMonumentRoom& west = room_ref(piece.room_index);
    const WgMonumentRoom& east = room_ref(conn(west, kMonumentEast));
    const WgMonumentRoom& west_up = room_ref(conn(west, kMonumentUp));
    const WgMonumentRoom& east_up = room_ref(conn(east, kMonumentUp));
    if (west.index / 25 > 0) {
      generate_default_floor(8, 0, open(east, kMonumentDown));
      generate_default_floor(0, 0, open(west, kMonumentDown));
    }
    if (conn(west_up, kMonumentUp) < 0)
      generate_box_on_fill_only(1, 8, 1, 7, 8, 6, gray());
    if (conn(east_up, kMonumentUp) < 0)
      generate_box_on_fill_only(8, 8, 1, 14, 8, 6, gray());
    for (int32_t y = 1; y <= 7; ++y) {
      BlockStateKey block = light();
      if (y == 2 || y == 6)
        block = gray();
      gen_box(0, y, 0, 0, y, 7, block, block);
      gen_box(15, y, 0, 15, y, 7, block, block);
      gen_box(1, y, 0, 15, y, 0, block, block);
      gen_box(1, y, 7, 14, y, 7, block, block);
    }
    gen_box(2, 1, 3, 2, 7, 4, light(), light());
    gen_box(3, 1, 2, 4, 7, 2, light(), light());
    gen_box(3, 1, 5, 4, 7, 5, light(), light());
    gen_box(13, 1, 3, 13, 7, 4, light(), light());
    gen_box(11, 1, 2, 12, 7, 2, light(), light());
    gen_box(11, 1, 5, 12, 7, 5, light(), light());
    gen_box(5, 1, 3, 5, 3, 4, light(), light());
    gen_box(10, 1, 3, 10, 3, 4, light(), light());
    gen_box(5, 7, 2, 10, 7, 5, light(), light());
    gen_box(5, 5, 2, 5, 7, 2, light(), light());
    gen_box(10, 5, 2, 10, 7, 2, light(), light());
    gen_box(5, 5, 5, 5, 7, 5, light(), light());
    gen_box(10, 5, 5, 10, 7, 5, light(), light());
    place(light(), 6, 6, 2);
    place(light(), 9, 6, 2);
    place(light(), 6, 6, 5);
    place(light(), 9, 6, 5);
    gen_box(5, 4, 3, 6, 4, 4, light(), light());
    gen_box(9, 4, 3, 10, 4, 4, light(), light());
    place(lamp(), 5, 4, 2);
    place(lamp(), 5, 4, 5);
    place(lamp(), 10, 4, 2);
    place(lamp(), 10, 4, 5);
    if (open(west, kMonumentSouth))
      generate_water_box(3, 1, 0, 4, 2, 0);
    if (open(west, kMonumentNorth))
      generate_water_box(3, 1, 7, 4, 2, 7);
    if (open(west, kMonumentWest))
      generate_water_box(0, 1, 3, 0, 2, 4);
    if (open(east, kMonumentSouth))
      generate_water_box(11, 1, 0, 12, 2, 0);
    if (open(east, kMonumentNorth))
      generate_water_box(11, 1, 7, 12, 2, 7);
    if (open(east, kMonumentEast))
      generate_water_box(15, 1, 3, 15, 2, 4);
    if (open(west_up, kMonumentSouth))
      generate_water_box(3, 5, 0, 4, 6, 0);
    if (open(west_up, kMonumentNorth))
      generate_water_box(3, 5, 7, 4, 6, 7);
    if (open(west_up, kMonumentWest))
      generate_water_box(0, 5, 3, 0, 6, 4);
    if (open(east_up, kMonumentSouth))
      generate_water_box(11, 5, 0, 12, 6, 0);
    if (open(east_up, kMonumentNorth))
      generate_water_box(11, 5, 7, 12, 6, 7);
    if (open(east_up, kMonumentEast))
      generate_water_box(15, 5, 3, 15, 6, 4);
  }

  void double_z() {
    const WgMonumentRoom& south = room_ref(piece.room_index);
    const WgMonumentRoom& north = room_ref(conn(south, kMonumentNorth));
    if (south.index / 25 > 0) {
      generate_default_floor(0, 8, open(north, kMonumentDown));
      generate_default_floor(0, 0, open(south, kMonumentDown));
    }
    if (conn(south, kMonumentUp) < 0)
      generate_box_on_fill_only(1, 4, 1, 6, 4, 7, gray());
    if (conn(north, kMonumentUp) < 0)
      generate_box_on_fill_only(1, 4, 8, 6, 4, 14, gray());
    gen_box(0, 3, 0, 0, 3, 15, light(), light());
    gen_box(7, 3, 0, 7, 3, 15, light(), light());
    gen_box(1, 3, 0, 7, 3, 0, light(), light());
    gen_box(1, 3, 15, 6, 3, 15, light(), light());
    gen_box(0, 2, 0, 0, 2, 15, gray(), gray());
    gen_box(7, 2, 0, 7, 2, 15, gray(), gray());
    gen_box(1, 2, 0, 7, 2, 0, gray(), gray());
    gen_box(1, 2, 15, 6, 2, 15, gray(), gray());
    gen_box(0, 1, 0, 0, 1, 15, light(), light());
    gen_box(7, 1, 0, 7, 1, 15, light(), light());
    gen_box(1, 1, 0, 7, 1, 0, light(), light());
    gen_box(1, 1, 15, 6, 1, 15, light(), light());
    gen_box(1, 1, 1, 1, 1, 2, light(), light());
    gen_box(6, 1, 1, 6, 1, 2, light(), light());
    gen_box(1, 3, 1, 1, 3, 2, light(), light());
    gen_box(6, 3, 1, 6, 3, 2, light(), light());
    gen_box(1, 1, 13, 1, 1, 14, light(), light());
    gen_box(6, 1, 13, 6, 1, 14, light(), light());
    gen_box(1, 3, 13, 1, 3, 14, light(), light());
    gen_box(6, 3, 13, 6, 3, 14, light(), light());
    gen_box(2, 1, 6, 2, 3, 6, light(), light());
    gen_box(5, 1, 6, 5, 3, 6, light(), light());
    gen_box(2, 1, 9, 2, 3, 9, light(), light());
    gen_box(5, 1, 9, 5, 3, 9, light(), light());
    gen_box(3, 2, 6, 4, 2, 6, light(), light());
    gen_box(3, 2, 9, 4, 2, 9, light(), light());
    gen_box(2, 2, 7, 2, 2, 8, light(), light());
    gen_box(5, 2, 7, 5, 2, 8, light(), light());
    place(lamp(), 2, 2, 5);
    place(lamp(), 5, 2, 5);
    place(lamp(), 2, 2, 10);
    place(lamp(), 5, 2, 10);
    place(light(), 2, 3, 5);
    place(light(), 5, 3, 5);
    place(light(), 2, 3, 10);
    place(light(), 5, 3, 10);
    if (open(south, kMonumentSouth))
      generate_water_box(3, 1, 0, 4, 2, 0);
    if (open(south, kMonumentEast))
      generate_water_box(7, 1, 3, 7, 2, 4);
    if (open(south, kMonumentWest))
      generate_water_box(0, 1, 3, 0, 2, 4);
    if (open(north, kMonumentNorth))
      generate_water_box(3, 1, 15, 4, 2, 15);
    if (open(north, kMonumentWest))
      generate_water_box(0, 1, 11, 0, 2, 12);
    if (open(north, kMonumentEast))
      generate_water_box(7, 1, 11, 7, 2, 12);
  }

  void double_x() {
    const WgMonumentRoom& west = room_ref(piece.room_index);
    const WgMonumentRoom& east = room_ref(conn(west, kMonumentEast));
    if (west.index / 25 > 0) {
      generate_default_floor(8, 0, open(east, kMonumentDown));
      generate_default_floor(0, 0, open(west, kMonumentDown));
    }
    if (conn(west, kMonumentUp) < 0)
      generate_box_on_fill_only(1, 4, 1, 7, 4, 6, gray());
    if (conn(east, kMonumentUp) < 0)
      generate_box_on_fill_only(8, 4, 1, 14, 4, 6, gray());
    gen_box(0, 3, 0, 0, 3, 7, light(), light());
    gen_box(15, 3, 0, 15, 3, 7, light(), light());
    gen_box(1, 3, 0, 15, 3, 0, light(), light());
    gen_box(1, 3, 7, 14, 3, 7, light(), light());
    gen_box(0, 2, 0, 0, 2, 7, gray(), gray());
    gen_box(15, 2, 0, 15, 2, 7, gray(), gray());
    gen_box(1, 2, 0, 15, 2, 0, gray(), gray());
    gen_box(1, 2, 7, 14, 2, 7, gray(), gray());
    gen_box(0, 1, 0, 0, 1, 7, light(), light());
    gen_box(15, 1, 0, 15, 1, 7, light(), light());
    gen_box(1, 1, 0, 15, 1, 0, light(), light());
    gen_box(1, 1, 7, 14, 1, 7, light(), light());
    gen_box(5, 1, 0, 10, 1, 4, light(), light());
    gen_box(6, 2, 0, 9, 2, 3, gray(), gray());
    gen_box(5, 3, 0, 10, 3, 4, light(), light());
    place(lamp(), 6, 2, 3);
    place(lamp(), 9, 2, 3);
    if (open(west, kMonumentSouth))
      generate_water_box(3, 1, 0, 4, 2, 0);
    if (open(west, kMonumentNorth))
      generate_water_box(3, 1, 7, 4, 2, 7);
    if (open(west, kMonumentWest))
      generate_water_box(0, 1, 3, 0, 2, 4);
    if (open(east, kMonumentSouth))
      generate_water_box(11, 1, 0, 12, 2, 0);
    if (open(east, kMonumentNorth))
      generate_water_box(11, 1, 7, 12, 2, 7);
    if (open(east, kMonumentEast))
      generate_water_box(15, 1, 3, 15, 2, 4);
  }

  void double_y() {
    const WgMonumentRoom& lower = room_ref(piece.room_index);
    if (lower.index / 25 > 0)
      generate_default_floor(0, 0, open(lower, kMonumentDown));
    const WgMonumentRoom& above = room_ref(conn(lower, kMonumentUp));
    if (conn(above, kMonumentUp) < 0)
      generate_box_on_fill_only(1, 8, 1, 6, 8, 6, gray());
    gen_box(0, 4, 0, 0, 4, 7, light(), light());
    gen_box(7, 4, 0, 7, 4, 7, light(), light());
    gen_box(1, 4, 0, 6, 4, 0, light(), light());
    gen_box(1, 4, 7, 6, 4, 7, light(), light());
    gen_box(2, 4, 1, 2, 4, 2, light(), light());
    gen_box(1, 4, 2, 1, 4, 2, light(), light());
    gen_box(5, 4, 1, 5, 4, 2, light(), light());
    gen_box(6, 4, 2, 6, 4, 2, light(), light());
    gen_box(2, 4, 5, 2, 4, 6, light(), light());
    gen_box(1, 4, 5, 1, 4, 5, light(), light());
    gen_box(5, 4, 5, 5, 4, 6, light(), light());
    gen_box(6, 4, 5, 6, 4, 5, light(), light());
    const WgMonumentRoom* definition = &lower;
    for (int32_t y = 1; y <= 5; y += 4) {
      int32_t z = 0;
      if (open(*definition, kMonumentSouth)) {
        gen_box(2, y, z, 2, y + 2, z, light(), light());
        gen_box(5, y, z, 5, y + 2, z, light(), light());
        gen_box(3, y + 2, z, 4, y + 2, z, light(), light());
      } else {
        gen_box(0, y, z, 7, y + 2, z, light(), light());
        gen_box(0, y + 1, z, 7, y + 1, z, gray(), gray());
      }
      z = 7;
      if (open(*definition, kMonumentNorth)) {
        gen_box(2, y, z, 2, y + 2, z, light(), light());
        gen_box(5, y, z, 5, y + 2, z, light(), light());
        gen_box(3, y + 2, z, 4, y + 2, z, light(), light());
      } else {
        gen_box(0, y, z, 7, y + 2, z, light(), light());
        gen_box(0, y + 1, z, 7, y + 1, z, gray(), gray());
      }
      int32_t x = 0;
      if (open(*definition, kMonumentWest)) {
        gen_box(x, y, 2, x, y + 2, 2, light(), light());
        gen_box(x, y, 5, x, y + 2, 5, light(), light());
        gen_box(x, y + 2, 3, x, y + 2, 4, light(), light());
      } else {
        gen_box(x, y, 0, x, y + 2, 7, light(), light());
        gen_box(x, y + 1, 0, x, y + 1, 7, gray(), gray());
      }
      x = 7;
      if (open(*definition, kMonumentEast)) {
        gen_box(x, y, 2, x, y + 2, 2, light(), light());
        gen_box(x, y, 5, x, y + 2, 5, light(), light());
        gen_box(x, y + 2, 3, x, y + 2, 4, light(), light());
      } else {
        gen_box(x, y, 0, x, y + 2, 7, light(), light());
        gen_box(x, y + 1, 0, x, y + 1, 7, gray(), gray());
      }
      definition = &above;
    }
  }

  void simple_top() {
    const WgMonumentRoom& def = room_ref(piece.room_index);
    if (def.index / 25 > 0)
      generate_default_floor(0, 0, open(def, kMonumentDown));
    if (conn(def, kMonumentUp) < 0)
      generate_box_on_fill_only(1, 4, 1, 6, 4, 6, gray());
    for (int32_t x = 1; x <= 6; ++x) {
      for (int32_t z = 1; z <= 6; ++z) {
        if (random.next_int(3) == 0)
          continue;
        const int32_t y0 = 2 + (random.next_int(4) == 0 ? 0 : 1);
        gen_box(x, y0, z, x, 3, z, wet_sponge(), wet_sponge());
      }
    }
    gen_box(0, 1, 0, 0, 1, 7, light(), light());
    gen_box(7, 1, 0, 7, 1, 7, light(), light());
    gen_box(1, 1, 0, 6, 1, 0, light(), light());
    gen_box(1, 1, 7, 6, 1, 7, light(), light());
    gen_box(0, 2, 0, 0, 2, 7, black(), black());
    gen_box(7, 2, 0, 7, 2, 7, black(), black());
    gen_box(1, 2, 0, 6, 2, 0, black(), black());
    gen_box(1, 2, 7, 6, 2, 7, black(), black());
    gen_box(0, 3, 0, 0, 3, 7, light(), light());
    gen_box(7, 3, 0, 7, 3, 7, light(), light());
    gen_box(1, 3, 0, 6, 3, 0, light(), light());
    gen_box(1, 3, 7, 6, 3, 7, light(), light());
    gen_box(0, 1, 3, 0, 2, 4, black(), black());
    gen_box(7, 1, 3, 7, 2, 4, black(), black());
    gen_box(3, 1, 0, 4, 2, 0, black(), black());
    gen_box(3, 1, 7, 4, 2, 7, black(), black());
    if (open(def, kMonumentSouth))
      generate_water_box(3, 1, 0, 4, 2, 0);
  }

  void simple() {
    const WgMonumentRoom& def = room_ref(piece.room_index);
    if (def.index / 25 > 0)
      generate_default_floor(0, 0, open(def, kMonumentDown));
    if (conn(def, kMonumentUp) < 0)
      generate_box_on_fill_only(1, 4, 1, 6, 4, 6, gray());
    const bool center_pillar = piece.main_design != 0 && random.next_boolean() &&
                               !open(def, kMonumentDown) && !open(def, kMonumentUp) &&
                               count_openings(def) > 1;
    if (piece.main_design == 0) {
      gen_box(0, 1, 0, 2, 1, 2, light(), light());
      gen_box(0, 3, 0, 2, 3, 2, light(), light());
      gen_box(0, 2, 0, 0, 2, 2, gray(), gray());
      gen_box(1, 2, 0, 2, 2, 0, gray(), gray());
      place(lamp(), 1, 2, 1);
      gen_box(5, 1, 0, 7, 1, 2, light(), light());
      gen_box(5, 3, 0, 7, 3, 2, light(), light());
      gen_box(7, 2, 0, 7, 2, 2, gray(), gray());
      gen_box(5, 2, 0, 6, 2, 0, gray(), gray());
      place(lamp(), 6, 2, 1);
      gen_box(0, 1, 5, 2, 1, 7, light(), light());
      gen_box(0, 3, 5, 2, 3, 7, light(), light());
      gen_box(0, 2, 5, 0, 2, 7, gray(), gray());
      gen_box(1, 2, 7, 2, 2, 7, gray(), gray());
      place(lamp(), 1, 2, 6);
      gen_box(5, 1, 5, 7, 1, 7, light(), light());
      gen_box(5, 3, 5, 7, 3, 7, light(), light());
      gen_box(7, 2, 5, 7, 2, 7, gray(), gray());
      gen_box(5, 2, 7, 6, 2, 7, gray(), gray());
      place(lamp(), 6, 2, 6);
      if (open(def, kMonumentSouth)) {
        gen_box(3, 3, 0, 4, 3, 0, light(), light());
      } else {
        gen_box(3, 3, 0, 4, 3, 1, light(), light());
        gen_box(3, 2, 0, 4, 2, 0, gray(), gray());
        gen_box(3, 1, 0, 4, 1, 1, light(), light());
      }
      if (open(def, kMonumentNorth)) {
        gen_box(3, 3, 7, 4, 3, 7, light(), light());
      } else {
        gen_box(3, 3, 6, 4, 3, 7, light(), light());
        gen_box(3, 2, 7, 4, 2, 7, gray(), gray());
        gen_box(3, 1, 6, 4, 1, 7, light(), light());
      }
      if (open(def, kMonumentWest)) {
        gen_box(0, 3, 3, 0, 3, 4, light(), light());
      } else {
        gen_box(0, 3, 3, 1, 3, 4, light(), light());
        gen_box(0, 2, 3, 0, 2, 4, gray(), gray());
        gen_box(0, 1, 3, 1, 1, 4, light(), light());
      }
      if (open(def, kMonumentEast)) {
        gen_box(7, 3, 3, 7, 3, 4, light(), light());
      } else {
        gen_box(6, 3, 3, 7, 3, 4, light(), light());
        gen_box(7, 2, 3, 7, 2, 4, gray(), gray());
        gen_box(6, 1, 3, 7, 1, 4, light(), light());
      }
    } else if (piece.main_design == 1) {
      gen_box(2, 1, 2, 2, 3, 2, light(), light());
      gen_box(2, 1, 5, 2, 3, 5, light(), light());
      gen_box(5, 1, 5, 5, 3, 5, light(), light());
      gen_box(5, 1, 2, 5, 3, 2, light(), light());
      place(lamp(), 2, 2, 2);
      place(lamp(), 2, 2, 5);
      place(lamp(), 5, 2, 5);
      place(lamp(), 5, 2, 2);
      gen_box(0, 1, 0, 1, 3, 0, light(), light());
      gen_box(0, 1, 1, 0, 3, 1, light(), light());
      gen_box(0, 1, 7, 1, 3, 7, light(), light());
      gen_box(0, 1, 6, 0, 3, 6, light(), light());
      gen_box(6, 1, 7, 7, 3, 7, light(), light());
      gen_box(7, 1, 6, 7, 3, 6, light(), light());
      gen_box(6, 1, 0, 7, 3, 0, light(), light());
      gen_box(7, 1, 1, 7, 3, 1, light(), light());
      place(gray(), 1, 2, 0);
      place(gray(), 0, 2, 1);
      place(gray(), 1, 2, 7);
      place(gray(), 0, 2, 6);
      place(gray(), 6, 2, 7);
      place(gray(), 7, 2, 6);
      place(gray(), 6, 2, 0);
      place(gray(), 7, 2, 1);
      if (!open(def, kMonumentSouth)) {
        gen_box(1, 3, 0, 6, 3, 0, light(), light());
        gen_box(1, 2, 0, 6, 2, 0, gray(), gray());
        gen_box(1, 1, 0, 6, 1, 0, light(), light());
      }
      if (!open(def, kMonumentNorth)) {
        gen_box(1, 3, 7, 6, 3, 7, light(), light());
        gen_box(1, 2, 7, 6, 2, 7, gray(), gray());
        gen_box(1, 1, 7, 6, 1, 7, light(), light());
      }
      if (!open(def, kMonumentWest)) {
        gen_box(0, 3, 1, 0, 3, 6, light(), light());
        gen_box(0, 2, 1, 0, 2, 6, gray(), gray());
        gen_box(0, 1, 1, 0, 1, 6, light(), light());
      }
      if (!open(def, kMonumentEast)) {
        gen_box(7, 3, 1, 7, 3, 6, light(), light());
        gen_box(7, 2, 1, 7, 2, 6, gray(), gray());
        gen_box(7, 1, 1, 7, 1, 6, light(), light());
      }
    } else if (piece.main_design == 2) {
      gen_box(0, 1, 0, 0, 1, 7, light(), light());
      gen_box(7, 1, 0, 7, 1, 7, light(), light());
      gen_box(1, 1, 0, 6, 1, 0, light(), light());
      gen_box(1, 1, 7, 6, 1, 7, light(), light());
      gen_box(0, 2, 0, 0, 2, 7, black(), black());
      gen_box(7, 2, 0, 7, 2, 7, black(), black());
      gen_box(1, 2, 0, 6, 2, 0, black(), black());
      gen_box(1, 2, 7, 6, 2, 7, black(), black());
      gen_box(0, 3, 0, 0, 3, 7, light(), light());
      gen_box(7, 3, 0, 7, 3, 7, light(), light());
      gen_box(1, 3, 0, 6, 3, 0, light(), light());
      gen_box(1, 3, 7, 6, 3, 7, light(), light());
      gen_box(0, 1, 3, 0, 2, 4, black(), black());
      gen_box(7, 1, 3, 7, 2, 4, black(), black());
      gen_box(3, 1, 0, 4, 2, 0, black(), black());
      gen_box(3, 1, 7, 4, 2, 7, black(), black());
      if (open(def, kMonumentSouth))
        generate_water_box(3, 1, 0, 4, 2, 0);
      if (open(def, kMonumentNorth))
        generate_water_box(3, 1, 7, 4, 2, 7);
      if (open(def, kMonumentWest))
        generate_water_box(0, 1, 3, 0, 2, 4);
      if (open(def, kMonumentEast))
        generate_water_box(7, 1, 3, 7, 2, 4);
    }
    if (center_pillar) {
      gen_box(3, 1, 3, 4, 1, 4, light(), light());
      gen_box(3, 2, 3, 4, 2, 4, gray(), gray());
      gen_box(3, 3, 3, 4, 3, 4, light(), light());
    }
  }

  void wing() {
    if (piece.main_design == 0) {
      for (int32_t i = 0; i < 4; ++i)
        gen_box(10 - i, 3 - i, 20 - i, 12 + i, 3 - i, 20, light(), light());
      gen_box(7, 0, 6, 15, 0, 16, light(), light());
      gen_box(6, 0, 6, 6, 3, 20, light(), light());
      gen_box(16, 0, 6, 16, 3, 20, light(), light());
      gen_box(7, 1, 7, 7, 1, 20, light(), light());
      gen_box(15, 1, 7, 15, 1, 20, light(), light());
      gen_box(7, 1, 6, 9, 3, 6, light(), light());
      gen_box(13, 1, 6, 15, 3, 6, light(), light());
      gen_box(8, 1, 7, 9, 1, 7, light(), light());
      gen_box(13, 1, 7, 14, 1, 7, light(), light());
      gen_box(9, 0, 5, 13, 0, 5, light(), light());
      gen_box(10, 0, 7, 12, 0, 7, black(), black());
      gen_box(8, 0, 10, 8, 0, 12, black(), black());
      gen_box(14, 0, 10, 14, 0, 12, black(), black());
      for (int32_t z = 18; z >= 7; z -= 3) {
        place(lamp(), 6, 3, z);
        place(lamp(), 16, 3, z);
      }
      place(lamp(), 10, 0, 10);
      place(lamp(), 12, 0, 10);
      place(lamp(), 10, 0, 12);
      place(lamp(), 12, 0, 12);
      place(lamp(), 8, 3, 6);
      place(lamp(), 14, 3, 6);
      place(light(), 4, 2, 4);
      place(lamp(), 4, 1, 4);
      place(light(), 4, 0, 4);
      place(light(), 18, 2, 4);
      place(lamp(), 18, 1, 4);
      place(light(), 18, 0, 4);
      place(light(), 4, 2, 18);
      place(lamp(), 4, 1, 18);
      place(light(), 4, 0, 18);
      place(light(), 18, 2, 18);
      place(lamp(), 18, 1, 18);
      place(light(), 18, 0, 18);
      place(light(), 9, 7, 20);
      place(light(), 13, 7, 20);
      gen_box(6, 0, 21, 7, 4, 21, light(), light());
      gen_box(15, 0, 21, 16, 4, 21, light(), light());
    } else if (piece.main_design == 1) {
      gen_box(9, 3, 18, 13, 3, 20, light(), light());
      gen_box(9, 0, 18, 9, 2, 18, light(), light());
      gen_box(13, 0, 18, 13, 2, 18, light(), light());
      int32_t x = 9;
      for (int32_t i = 0; i < 2; ++i) {
        place(light(), x, 6, 20);
        place(lamp(), x, 5, 20);
        place(light(), x, 4, 20);
        x = 13;
      }
      gen_box(7, 3, 7, 15, 3, 14, light(), light());
      x = 10;
      for (int32_t i = 0; i < 2; ++i) {
        gen_box(x, 0, 10, x, 6, 10, light(), light());
        gen_box(x, 0, 12, x, 6, 12, light(), light());
        place(lamp(), x, 0, 10);
        place(lamp(), x, 0, 12);
        place(lamp(), x, 4, 10);
        place(lamp(), x, 4, 12);
        x = 12;
      }
      x = 8;
      for (int32_t i = 0; i < 2; ++i) {
        gen_box(x, 0, 7, x, 2, 7, light(), light());
        gen_box(x, 0, 14, x, 2, 14, light(), light());
        x = 14;
      }
      gen_box(8, 3, 8, 8, 3, 13, black(), black());
      gen_box(14, 3, 8, 14, 3, 13, black(), black());
    }
  }

  void penthouse() {
    gen_box(2, -1, 2, 11, -1, 11, light(), light());
    gen_box(0, -1, 0, 1, -1, 11, gray(), gray());
    gen_box(12, -1, 0, 13, -1, 11, gray(), gray());
    gen_box(2, -1, 0, 11, -1, 1, gray(), gray());
    gen_box(2, -1, 12, 11, -1, 13, gray(), gray());
    gen_box(0, 0, 0, 0, 0, 13, light(), light());
    gen_box(13, 0, 0, 13, 0, 13, light(), light());
    gen_box(1, 0, 0, 12, 0, 0, light(), light());
    gen_box(1, 0, 13, 12, 0, 13, light(), light());
    for (int32_t i = 2; i <= 11; i += 3) {
      place(lamp(), 0, 0, i);
      place(lamp(), 13, 0, i);
      place(lamp(), i, 0, 0);
    }
    gen_box(2, 0, 3, 4, 0, 9, light(), light());
    gen_box(9, 0, 3, 11, 0, 9, light(), light());
    gen_box(4, 0, 9, 9, 0, 11, light(), light());
    place(light(), 5, 0, 8);
    place(light(), 8, 0, 8);
    place(light(), 10, 0, 10);
    place(light(), 3, 0, 10);
    gen_box(3, 0, 3, 3, 0, 7, black(), black());
    gen_box(10, 0, 3, 10, 0, 7, black(), black());
    gen_box(6, 0, 10, 7, 0, 10, black(), black());
    int32_t x = 3;
    for (int32_t i = 0; i < 2; ++i) {
      for (int32_t z = 2; z <= 8; z += 3)
        gen_box(x, 0, z, x, 2, z, light(), light());
      x = 10;
    }
    gen_box(5, 0, 10, 5, 2, 10, light(), light());
    gen_box(8, 0, 10, 8, 2, 10, light(), light());
    gen_box(6, -1, 7, 7, -1, 8, black(), black());
    generate_water_box(6, -1, 3, 7, -1, 4);
  }
};

uint32_t MonumentWriter::run() {
  switch (piece.kind) {
  case WgOceanMonumentPieceKind::kEntry:
    entry();
    break;
  case WgOceanMonumentPieceKind::kCore:
    core();
    break;
  case WgOceanMonumentPieceKind::kDoubleYZ:
    double_yz();
    break;
  case WgOceanMonumentPieceKind::kDoubleXY:
    double_xy();
    break;
  case WgOceanMonumentPieceKind::kDoubleZ:
    double_z();
    break;
  case WgOceanMonumentPieceKind::kDoubleX:
    double_x();
    break;
  case WgOceanMonumentPieceKind::kDoubleY:
    double_y();
    break;
  case WgOceanMonumentPieceKind::kSimpleTop:
    simple_top();
    break;
  case WgOceanMonumentPieceKind::kSimple:
    simple();
    break;
  case WgOceanMonumentPieceKind::kWing:
    wing();
    break;
  case WgOceanMonumentPieceKind::kPenthouse:
    penthouse();
    break;
  default:
    break;
  }
  return writer.changed();
}

} // namespace

uint32_t wg_place_stronghold(Chunk& chunk, const WgStrongholdStart& start) {
  if (!start.valid)
    return 0;
  const int32_t base_x = chunk.x * 16;
  const int32_t base_z = chunk.z * 16;
  uint32_t changed = 0;
  for (const WgStrongholdPiece& piece : start.pieces) {
    if (piece.max_x < base_x || piece.min_x >= base_x + 16 || piece.max_z < base_z ||
        piece.min_z >= base_z + 16 || piece.max_y < kMinY || piece.min_y >= kMinY + kHeight)
      continue;
    StrongholdWriter writer(chunk, piece, start.world_seed);
    changed += writer.run();
  }
  return changed;
}

// `WoodlandMansionPieces.MansionGrid` and its `SimpleGrid`. The grid is an
// 11 x 11 x-valued map whose cells are corridor/room bytes plus packed room
// metadata; `valueIfOutside` is 5 so an out-of-bounds probe reads as blocked.
// Direction order here is the grid's own (N, E, S, W); Java's
// `Direction.from2DDataValue` order is separately `2D` below.
enum MansionDirection : int32_t {
  kMansionNorth = 0,
  kMansionEast = 1,
  kMansionSouth = 2,
  kMansionWest = 3,
};

constexpr int32_t mansion_dx(int32_t direction) {
  return direction == kMansionEast ? 1 : (direction == kMansionWest ? -1 : 0);
}

constexpr int32_t mansion_dz(int32_t direction) {
  return direction == kMansionSouth ? 1 : (direction == kMansionNorth ? -1 : 0);
}

constexpr int32_t mansion_clockwise(int32_t direction) {
  return (direction + 1) & 3;
}
constexpr int32_t mansion_counterclockwise(int32_t direction) {
  return (direction + 3) & 3;
}
constexpr int32_t mansion_opposite(int32_t direction) {
  return (direction + 2) & 3;
}
constexpr int32_t mansion_rotated(int32_t direction, uint32_t rotation) {
  return (direction + static_cast<int32_t>(rotation)) & 3;
}

// `Direction.from2DDataValue`: SOUTH, WEST, NORTH, EAST.
constexpr int32_t kMansion2DOrder[4] = {kMansionSouth, kMansionWest, kMansionNorth, kMansionEast};

struct MansionGridPos {
  int32_t x;
  int32_t y;
};

struct MansionSimpleGrid {
  static constexpr int32_t kSize = 11;
  int32_t cells[kSize][kSize] = {};
  int32_t outside = 5;

  int32_t get(int32_t x, int32_t y) const {
    if (x < 0 || x >= kSize || y < 0 || y >= kSize)
      return outside;
    return cells[x][y];
  }

  void set(int32_t x, int32_t y, int32_t value) {
    if (x >= 0 && x < kSize && y >= 0 && y < kSize)
      cells[x][y] = value;
  }

  void set_box(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t value) {
    for (int32_t y = y0; y <= y1; ++y)
      for (int32_t x = x0; x <= x1; ++x)
        set(x, y, value);
  }

  void set_if(int32_t x, int32_t y, int32_t expected, int32_t value) {
    if (get(x, y) == expected)
      set(x, y, value);
  }

  bool edges_to(int32_t x, int32_t y, int32_t value) const {
    return get(x - 1, y) == value || get(x + 1, y) == value || get(x, y + 1) == value ||
           get(x, y - 1) == value;
  }

  bool is_house(int32_t x, int32_t y) const {
    const int32_t value = get(x, y);
    return value == 1 || value == 2 || value == 3 || value == 4;
  }
};

// `Util.shuffle`: descending Fisher-Yates, exactly vanilla's swap order.
template <typename T> void mansion_shuffle(std::vector<T>& values, WgLegacyRandom& random) {
  for (int32_t i = static_cast<int32_t>(values.size()); i > 1; --i) {
    const int32_t swap_to = random.next_int(i);
    std::swap(values[static_cast<size_t>(i - 1)], values[static_cast<size_t>(swap_to)]);
  }
}

struct MansionGrid {
  explicit MansionGrid(WgLegacyRandom& random_in) : random(random_in) {
    constexpr int32_t kEntranceX = 7;
    constexpr int32_t kEntranceY = 4;
    entrance_x = kEntranceX;
    entrance_y = kEntranceY;
    base.set_box(entrance_x, entrance_y, entrance_x + 1, entrance_y + 1, 3);
    base.set_box(entrance_x - 1, entrance_y, entrance_x - 1, entrance_y + 1, 2);
    base.set_box(entrance_x + 2, entrance_y - 2, entrance_x + 3, entrance_y + 3, 5);
    base.set_box(entrance_x + 1, entrance_y - 2, entrance_x + 1, entrance_y - 1, 1);
    base.set_box(entrance_x + 1, entrance_y + 2, entrance_x + 1, entrance_y + 3, 1);
    base.set(entrance_x - 1, entrance_y - 1, 1);
    base.set(entrance_x - 1, entrance_y + 2, 1);
    base.set_box(0, 0, 11, 1, 5);
    base.set_box(0, 9, 11, 11, 5);
    recursive_corridor(base, entrance_x, entrance_y - 2, kMansionWest, 6);
    recursive_corridor(base, entrance_x, entrance_y + 3, kMansionWest, 6);
    recursive_corridor(base, entrance_x - 2, entrance_y - 1, kMansionWest, 3);
    recursive_corridor(base, entrance_x - 2, entrance_y + 2, kMansionWest, 3);
    while (clean_edges(base)) {
    }
    identify_rooms(base, floor_rooms[0]);
    identify_rooms(base, floor_rooms[1]);
    floor_rooms[0].set_box(entrance_x + 1, entrance_y, entrance_x + 1, entrance_y + 1, 0x800000);
    floor_rooms[1].set_box(entrance_x + 1, entrance_y, entrance_x + 1, entrance_y + 1, 0x800000);
    setup_third_floor();
    identify_rooms(third_floor, floor_rooms[2]);
  }

  bool is_room_id(const MansionSimpleGrid& /*grid*/, int32_t x, int32_t y, int32_t floor,
                  int32_t room_id) const {
    return (floor_rooms[floor].get(x, y) & 0xFFFF) == room_id;
  }

  // `get1x2RoomDirection`: `Direction.Plane.HORIZONTAL` is NORTH, EAST, SOUTH,
  // WEST, and the room id lives in the floor's own grid.
  int32_t get_1x2_room_direction(const MansionSimpleGrid& grid, int32_t x, int32_t y, int32_t floor,
                                 int32_t room_id) const {
    constexpr int32_t kHorizontal[4] = {kMansionNorth, kMansionEast, kMansionSouth, kMansionWest};
    for (int32_t direction : kHorizontal) {
      if (is_room_id(grid, x + mansion_dx(direction), y + mansion_dz(direction), floor, room_id))
        return direction;
    }
    return -1;
  }

  void recursive_corridor(MansionSimpleGrid& grid, int32_t x, int32_t y, int32_t heading,
                          int32_t depth) {
    if (depth <= 0)
      return;
    grid.set(x, y, 1);
    grid.set_if(x + mansion_dx(heading), y + mansion_dz(heading), 0, 1);
    for (int32_t attempts = 0; attempts < 8; ++attempts) {
      const int32_t next = kMansion2DOrder[random.next_int(4)];
      if (next == mansion_opposite(heading) || (next == kMansionEast && random.next_boolean()))
        continue;
      const int32_t nx = x + mansion_dx(heading);
      const int32_t ny = y + mansion_dz(heading);
      if (grid.get(nx + mansion_dx(next), ny + mansion_dz(next)) != 0 ||
          grid.get(nx + mansion_dx(next) * 2, ny + mansion_dz(next) * 2) != 0)
        continue;
      recursive_corridor(grid, x + mansion_dx(heading) + mansion_dx(next),
                         y + mansion_dz(heading) + mansion_dz(next), next, depth - 1);
      break;
    }
    const int32_t cw = mansion_clockwise(heading);
    const int32_t ccw = mansion_counterclockwise(heading);
    grid.set_if(x + mansion_dx(cw), y + mansion_dz(cw), 0, 2);
    grid.set_if(x + mansion_dx(ccw), y + mansion_dz(ccw), 0, 2);
    grid.set_if(x + mansion_dx(heading) + mansion_dx(cw), y + mansion_dz(heading) + mansion_dz(cw),
                0, 2);
    grid.set_if(x + mansion_dx(heading) + mansion_dx(ccw),
                y + mansion_dz(heading) + mansion_dz(ccw), 0, 2);
    grid.set_if(x + mansion_dx(heading) * 2, y + mansion_dz(heading) * 2, 0, 2);
    grid.set_if(x + mansion_dx(cw) * 2, y + mansion_dz(cw) * 2, 0, 2);
    grid.set_if(x + mansion_dx(ccw) * 2, y + mansion_dz(ccw) * 2, 0, 2);
  }

  bool clean_edges(MansionSimpleGrid& grid) {
    bool touched = false;
    for (int32_t y = 0; y < MansionSimpleGrid::kSize; ++y) {
      for (int32_t x = 0; x < MansionSimpleGrid::kSize; ++x) {
        if (grid.get(x, y) != 0)
          continue;
        int32_t direct = 0;
        direct += grid.is_house(x + 1, y) ? 1 : 0;
        direct += grid.is_house(x - 1, y) ? 1 : 0;
        direct += grid.is_house(x, y + 1) ? 1 : 0;
        direct += grid.is_house(x, y - 1) ? 1 : 0;
        if (direct >= 3) {
          grid.set(x, y, 2);
          touched = true;
          continue;
        }
        if (direct != 2)
          continue;
        int32_t diagonal = 0;
        diagonal += grid.is_house(x + 1, y + 1) ? 1 : 0;
        diagonal += grid.is_house(x - 1, y + 1) ? 1 : 0;
        diagonal += grid.is_house(x + 1, y - 1) ? 1 : 0;
        diagonal += grid.is_house(x - 1, y - 1) ? 1 : 0;
        if (diagonal <= 1) {
          grid.set(x, y, 2);
          touched = true;
        }
      }
    }
    return touched;
  }

  void setup_third_floor() {
    std::vector<MansionGridPos> potential_rooms;
    MansionSimpleGrid& floor = floor_rooms[1];
    for (int32_t y = 0; y < MansionSimpleGrid::kSize; ++y) {
      for (int32_t x = 0; x < MansionSimpleGrid::kSize; ++x) {
        const int32_t room_data = floor.get(x, y);
        if ((room_data & 0xF0000) == 0x20000 && (room_data & 0x200000) == 0x200000)
          potential_rooms.push_back(MansionGridPos{x, y});
      }
    }
    if (potential_rooms.empty()) {
      third_floor.set_box(0, 0, MansionSimpleGrid::kSize, MansionSimpleGrid::kSize, 5);
      return;
    }
    const MansionGridPos room_pos = potential_rooms[static_cast<size_t>(
        random.next_int(static_cast<int32_t>(potential_rooms.size())))];
    const int32_t room_data = floor.get(room_pos.x, room_pos.y);
    floor.set(room_pos.x, room_pos.y, room_data | 0x400000);
    const int32_t room_dir =
        get_1x2_room_direction(base, room_pos.x, room_pos.y, 1, room_data & 0xFFFF);
    if (room_dir < 0) {
      // Vanilla dereferences the direction here; a grid with no neighbour is
      // not a shape its own constructor can produce, so fail closed rather
      // than write a third floor around a guessed direction.
      third_floor.set_box(0, 0, MansionSimpleGrid::kSize, MansionSimpleGrid::kSize, 5);
      floor.set(room_pos.x, room_pos.y, room_data);
      return;
    }
    const int32_t room_end_x = room_pos.x + mansion_dx(room_dir);
    const int32_t room_end_y = room_pos.y + mansion_dz(room_dir);
    for (int32_t y = 0; y < MansionSimpleGrid::kSize; ++y) {
      for (int32_t x = 0; x < MansionSimpleGrid::kSize; ++x) {
        if (!base.is_house(x, y)) {
          third_floor.set(x, y, 5);
          continue;
        }
        if (x == room_pos.x && y == room_pos.y) {
          third_floor.set(x, y, 3);
          continue;
        }
        if (x == room_end_x && y == room_end_y) {
          third_floor.set(x, y, 3);
          floor_rooms[2].set(x, y, 0x800000);
        }
      }
    }
    std::vector<int32_t> potential_corridors;
    constexpr int32_t kHorizontal[4] = {kMansionNorth, kMansionEast, kMansionSouth, kMansionWest};
    for (int32_t direction : kHorizontal) {
      if (third_floor.get(room_end_x + mansion_dx(direction), room_end_y + mansion_dz(direction)) ==
          0)
        potential_corridors.push_back(direction);
    }
    if (potential_corridors.empty()) {
      third_floor.set_box(0, 0, MansionSimpleGrid::kSize, MansionSimpleGrid::kSize, 5);
      floor.set(room_pos.x, room_pos.y, room_data);
      return;
    }
    const int32_t corridor_dir = potential_corridors[static_cast<size_t>(
        random.next_int(static_cast<int32_t>(potential_corridors.size())))];
    recursive_corridor(third_floor, room_end_x + mansion_dx(corridor_dir),
                       room_end_y + mansion_dz(corridor_dir), corridor_dir, 4);
    while (clean_edges(third_floor)) {
    }
  }

  void identify_rooms(const MansionSimpleGrid& from, MansionSimpleGrid& rooms) {
    std::vector<MansionGridPos> positions;
    for (int32_t y = 0; y < MansionSimpleGrid::kSize; ++y)
      for (int32_t x = 0; x < MansionSimpleGrid::kSize; ++x)
        if (from.get(x, y) == 2)
          positions.push_back(MansionGridPos{x, y});
    mansion_shuffle(positions, random);
    int32_t room_id = 10;
    for (const MansionGridPos& pos : positions) {
      const int32_t x = pos.x;
      const int32_t y = pos.y;
      if (rooms.get(x, y) != 0)
        continue;
      int32_t x0 = x;
      int32_t x1 = x;
      int32_t y0 = y;
      int32_t y1 = y;
      int32_t type = 65536;
      if (rooms.get(x + 1, y) == 0 && rooms.get(x, y + 1) == 0 && rooms.get(x + 1, y + 1) == 0 &&
          from.get(x + 1, y) == 2 && from.get(x, y + 1) == 2 && from.get(x + 1, y + 1) == 2) {
        ++x1;
        ++y1;
        type = 262144;
      } else if (rooms.get(x - 1, y) == 0 && rooms.get(x, y + 1) == 0 &&
                 rooms.get(x - 1, y + 1) == 0 && from.get(x - 1, y) == 2 &&
                 from.get(x, y + 1) == 2 && from.get(x - 1, y + 1) == 2) {
        --x0;
        ++y1;
        type = 262144;
      } else if (rooms.get(x - 1, y) == 0 && rooms.get(x, y - 1) == 0 &&
                 rooms.get(x - 1, y - 1) == 0 && from.get(x - 1, y) == 2 &&
                 from.get(x, y - 1) == 2 && from.get(x - 1, y - 1) == 2) {
        --x0;
        --y0;
        type = 262144;
      } else if (rooms.get(x + 1, y) == 0 && from.get(x + 1, y) == 2) {
        ++x1;
        type = 131072;
      } else if (rooms.get(x, y + 1) == 0 && from.get(x, y + 1) == 2) {
        ++y1;
        type = 131072;
      } else if (rooms.get(x - 1, y) == 0 && from.get(x - 1, y) == 2) {
        --x0;
        type = 131072;
      } else if (rooms.get(x, y - 1) == 0 && from.get(x, y - 1) == 2) {
        --y0;
        type = 131072;
      }
      int32_t door_x = random.next_boolean() ? x0 : x1;
      int32_t door_y = random.next_boolean() ? y0 : y1;
      int32_t door_flag = 0x200000;
      if (!from.edges_to(door_x, door_y, 1)) {
        door_x = door_x == x0 ? x1 : x0;
        door_y = door_y == y0 ? y1 : y0;
        if (!from.edges_to(door_x, door_y, 1)) {
          door_y = door_y == y0 ? y1 : y0;
          if (!from.edges_to(door_x, door_y, 1)) {
            door_x = door_x == x0 ? x1 : x0;
            door_y = door_y == y0 ? y1 : y0;
            if (!from.edges_to(door_x, door_y, 1)) {
              door_flag = 0;
              door_x = x0;
              door_y = y0;
            }
          }
        }
      }
      for (int32_t ry = y0; ry <= y1; ++ry) {
        for (int32_t rx = x0; rx <= x1; ++rx) {
          const int32_t door = (rx == door_x && ry == door_y) ? (0x100000 | door_flag) : 0;
          rooms.set(rx, ry, type | room_id | door);
        }
      }
      ++room_id;
    }
  }

  WgLegacyRandom& random;
  MansionSimpleGrid base;
  MansionSimpleGrid third_floor;
  MansionSimpleGrid floor_rooms[3];
  int32_t entrance_x = 7;
  int32_t entrance_y = 4;
};

struct MansionBlockPos {
  int32_t x;
  int32_t y;
  int32_t z;
};

MansionBlockPos mansion_relative(const MansionBlockPos& pos, uint32_t rotation, int32_t direction,
                                 int32_t amount) {
  const int32_t rotated = mansion_rotated(direction, rotation);
  return MansionBlockPos{pos.x + mansion_dx(rotated) * amount, pos.y,
                         pos.z + mansion_dz(rotated) * amount};
}

MansionBlockPos mansion_above(const MansionBlockPos& pos, int32_t amount) {
  return MansionBlockPos{pos.x, pos.y + amount, pos.z};
}

MansionBlockPos mansion_add(const MansionBlockPos& pos, int32_t dx, int32_t dy, int32_t dz) {
  return MansionBlockPos{pos.x + dx, pos.y + dy, pos.z + dz};
}

// `BlockPos.rotate(Rotation)` around the origin.
MansionBlockPos mansion_rotate_pos(const MansionBlockPos& pos, uint32_t rotation) {
  switch (rotation & 3u) {
  case 1:
    return MansionBlockPos{-pos.z, pos.y, pos.x};
  case 2:
    return MansionBlockPos{-pos.x, pos.y, -pos.z};
  case 3:
    return MansionBlockPos{pos.z, pos.y, -pos.x};
  default:
    return pos;
  }
}

// `StructureTemplate.getZeroPositionWithTransform(zeroPos, NONE, rotation,
// sizeX, sizeZ)` for the 1x1 room's (1, 0, 0) pivot and 7 x 7 template. The
// 26.2 server's offsets are the template size minus one; `MansionZero` prints
// the four answers from the live class (7,0), (7,6), (1,6).
MansionBlockPos mansion_zero_position(int32_t x, int32_t z, uint32_t rotation) {
  switch (rotation & 3u) {
  case 1:
    return MansionBlockPos{x + 6, 0, z};
  case 2:
    return MansionBlockPos{x + 6, 0, z + 6};
  case 3:
    return MansionBlockPos{x, 0, z + 6};
  default:
    return MansionBlockPos{x, 0, z};
  }
}

struct MansionRoomCollection {
  explicit MansionRoomCollection(int32_t floor_in) : floor(floor_in) {}

  std::string get_1x1(WgLegacyRandom& random) const {
    return (floor == 0 ? "1x1_a" : "1x1_b") + std::to_string(random.next_int(5) + 1);
  }

  std::string get_1x1_secret(WgLegacyRandom& random) const {
    return "1x1_as" + std::to_string(random.next_int(4) + 1);
  }

  std::string get_1x2_side(WgLegacyRandom& random, bool stairs) const {
    if (floor == 0)
      return "1x2_a" + std::to_string(random.next_int(9) + 1);
    if (stairs)
      return "1x2_c_stairs";
    return "1x2_c" + std::to_string(random.next_int(4) + 1);
  }

  std::string get_1x2_front(WgLegacyRandom& random, bool stairs) const {
    if (floor == 0)
      return "1x2_b" + std::to_string(random.next_int(5) + 1);
    if (stairs)
      return "1x2_d_stairs";
    return "1x2_d" + std::to_string(random.next_int(5) + 1);
  }

  std::string get_1x2_secret(WgLegacyRandom& random) const {
    if (floor == 0)
      return "1x2_s" + std::to_string(random.next_int(2) + 1);
    return "1x2_se" + std::to_string(random.next_int(1) + 1);
  }

  std::string get_2x2(WgLegacyRandom& random) const {
    return (floor == 0 ? "2x2_a" : "2x2_b") +
           std::to_string(random.next_int(floor == 0 ? 4 : 5) + 1);
  }

  std::string get_2x2_secret() const {
    return "2x2_s1";
  }

  int32_t floor = 0;
};

// `WoodlandMansionPieces.MansionPiecePlacer`, including the `PlacementData`
// cursor that walks the outer wall and the room collection's exact draw order.
struct MansionPiecePlacer {
  MansionPiecePlacer(MansionGrid& grid_in, WgLegacyRandom& random_in,
                     std::vector<WgMansionPiece>& pieces_in)
      : grid(grid_in), random(random_in), pieces(pieces_in) {}

  void add(const std::string& name, const MansionBlockPos& pos, uint32_t rotation,
           uint8_t mirror = 0) {
    const std::string full = "minecraft:woodland_mansion/" + name;
    const WgPieceTemplate* templ = wg_piece_template_by_name(full.c_str());
    if (templ == nullptr)
      return;
    pieces.push_back(
        WgMansionPiece{templ, static_cast<uint8_t>(rotation & 3u), mirror, pos.x, pos.y, pos.z});
  }

  struct PlacementData {
    MansionBlockPos pos;
    uint32_t rotation = 0;
    const char* wall_type = "wall_flat";
  };

  void create_mansion(const MansionBlockPos& origin, uint32_t rotation) {
    PlacementData data{origin, rotation, "wall_flat"};
    entrance(data);
    PlacementData second;
    second.pos = mansion_above(data.pos, 8);
    second.rotation = data.rotation;
    second.wall_type = "wall_window";

    start_x = grid.entrance_x + 1;
    start_y = grid.entrance_y + 1;
    const int32_t end_x = grid.entrance_x + 1;
    const int32_t end_y = grid.entrance_y;
    traverse_outer_walls(data, grid.base, kMansionSouth, start_x, start_y, end_x, end_y);
    traverse_outer_walls(second, grid.base, kMansionSouth, start_x, start_y, end_x, end_y);

    PlacementData third;
    third.pos = mansion_above(data.pos, 19);
    third.rotation = data.rotation;
    third.wall_type = "wall_window";
    bool done = false;
    for (int32_t y = 0; y < MansionSimpleGrid::kSize && !done; ++y) {
      for (int32_t x = MansionSimpleGrid::kSize - 1; x >= 0 && !done; --x) {
        if (!grid.third_floor.is_house(x, y))
          continue;
        third.pos = mansion_relative(third.pos, rotation, kMansionSouth, 8 + (y - start_y) * 8);
        third.pos = mansion_relative(third.pos, rotation, kMansionEast, (x - start_x) * 8);
        traverse_wall_piece(third);
        traverse_outer_walls(third, grid.third_floor, kMansionSouth, x, y, x, y);
        done = true;
      }
    }

    create_roof(mansion_above(origin, 16), rotation, grid.base, &grid.third_floor);
    create_roof(mansion_above(origin, 27), rotation, grid.third_floor, nullptr);

    MansionRoomCollection collections[3] = {MansionRoomCollection(0), MansionRoomCollection(1),
                                            MansionRoomCollection(2)};
    for (int32_t floor = 0; floor < 3; ++floor) {
      MansionBlockPos floor_origin = mansion_above(origin, 8 * floor + (floor == 2 ? 3 : 0));
      MansionSimpleGrid& rooms = grid.floor_rooms[floor];
      MansionSimpleGrid& floor_grid = floor == 2 ? grid.third_floor : grid.base;
      const std::string south_piece = floor == 0 ? "carpet_south_1" : "carpet_south_2";
      const std::string west_piece = floor == 0 ? "carpet_west_1" : "carpet_west_2";
      for (int32_t y = 0; y < MansionSimpleGrid::kSize; ++y) {
        for (int32_t x = 0; x < MansionSimpleGrid::kSize; ++x) {
          if (floor_grid.get(x, y) != 1)
            continue;
          MansionBlockPos pos =
              mansion_relative(floor_origin, rotation, kMansionSouth, 8 + (y - start_y) * 8);
          pos = mansion_relative(pos, rotation, kMansionEast, (x - start_x) * 8);
          add("corridor_floor", pos, rotation);
          if (floor_grid.get(x, y - 1) == 1 || (rooms.get(x, y - 1) & 0x800000) == 0x800000) {
            add("carpet_north", mansion_above(mansion_relative(pos, rotation, kMansionEast, 1), 1),
                rotation);
          }
          if (floor_grid.get(x + 1, y) == 1 || (rooms.get(x + 1, y) & 0x800000) == 0x800000) {
            MansionBlockPos carpet = mansion_relative(pos, rotation, kMansionSouth, 1);
            carpet = mansion_relative(carpet, rotation, kMansionEast, 5);
            add("carpet_east", mansion_above(carpet, 1), rotation);
          }
          if (floor_grid.get(x, y + 1) == 1 || (rooms.get(x, y + 1) & 0x800000) == 0x800000) {
            MansionBlockPos carpet = mansion_relative(pos, rotation, kMansionSouth, 5);
            carpet = mansion_relative(carpet, rotation, kMansionWest, 1);
            add(south_piece, carpet, rotation);
          }
          if (floor_grid.get(x - 1, y) == 1 || (rooms.get(x - 1, y) & 0x800000) == 0x800000) {
            MansionBlockPos carpet = mansion_relative(pos, rotation, kMansionWest, 1);
            carpet = mansion_relative(carpet, rotation, kMansionNorth, 1);
            add(west_piece, carpet, rotation);
          }
        }
      }

      const std::string wall_piece = floor == 0 ? "indoors_wall_1" : "indoors_wall_2";
      const std::string door_piece = floor == 0 ? "indoors_door_1" : "indoors_door_2";
      std::vector<int32_t> door_dirs;
      constexpr int32_t kHorizontal[4] = {kMansionNorth, kMansionEast, kMansionSouth, kMansionWest};
      for (int32_t y = 0; y < MansionSimpleGrid::kSize; ++y) {
        for (int32_t x = 0; x < MansionSimpleGrid::kSize; ++x) {
          bool third_floor_start = floor == 2 && floor_grid.get(x, y) == 3;
          if (floor_grid.get(x, y) != 2 && !third_floor_start)
            continue;
          const int32_t room_data = rooms.get(x, y);
          const int32_t room_type = room_data & 0xF0000;
          const int32_t room_id = room_data & 0xFFFF;
          third_floor_start = third_floor_start && (room_data & 0x800000) == 0x800000;
          door_dirs.clear();
          if ((room_data & 0x200000) == 0x200000) {
            for (int32_t direction : kHorizontal) {
              if (floor_grid.get(x + mansion_dx(direction), y + mansion_dz(direction)) == 1)
                door_dirs.push_back(direction);
            }
          }
          int32_t door_dir = -1;
          if (!door_dirs.empty())
            door_dir = door_dirs[static_cast<size_t>(
                random.next_int(static_cast<int32_t>(door_dirs.size())))];
          else if ((room_data & 0x100000) == 0x100000)
            door_dir = 4; // `Direction.UP`

          MansionBlockPos room_pos =
              mansion_relative(floor_origin, rotation, kMansionSouth, 8 + (y - start_y) * 8);
          room_pos = mansion_relative(room_pos, rotation, kMansionEast, -1 + (x - start_x) * 8);
          if (floor_grid.is_house(x - 1, y) &&
              !grid.is_room_id(floor_grid, x - 1, y, floor, room_id)) {
            add(door_dir == kMansionWest ? door_piece : wall_piece, room_pos, rotation);
          }
          if (floor_grid.get(x + 1, y) == 1 && !third_floor_start) {
            const MansionBlockPos pos = mansion_relative(room_pos, rotation, kMansionEast, 8);
            add(door_dir == kMansionEast ? door_piece : wall_piece, pos, rotation);
          }
          if (floor_grid.is_house(x, y + 1) &&
              !grid.is_room_id(floor_grid, x, y + 1, floor, room_id)) {
            MansionBlockPos pos = mansion_relative(room_pos, rotation, kMansionSouth, 7);
            pos = mansion_relative(pos, rotation, kMansionEast, 7);
            add(door_dir == kMansionSouth ? door_piece : wall_piece, pos, (rotation + 1) & 3u);
          }
          if (floor_grid.get(x, y - 1) == 1 && !third_floor_start) {
            MansionBlockPos pos = mansion_relative(room_pos, rotation, kMansionNorth, 1);
            pos = mansion_relative(pos, rotation, kMansionEast, 7);
            add(door_dir == kMansionNorth ? door_piece : wall_piece, pos, (rotation + 1) & 3u);
          }

          if (room_type == 65536) {
            add_room_1x1(room_pos, rotation, door_dir, collections[floor]);
          } else if (room_type == 131072 && door_dir >= 0) {
            const int32_t room_dir = grid.get_1x2_room_direction(floor_grid, x, y, floor, room_id);
            if (room_dir >= 0)
              add_room_1x2(room_pos, rotation, room_dir, door_dir, collections[floor],
                           (room_data & 0x400000) == 0x400000);
          } else if (room_type == 262144 && door_dir >= 0 && door_dir != 4) {
            int32_t room_dir = mansion_clockwise(door_dir);
            if (!grid.is_room_id(floor_grid, x + mansion_dx(room_dir), y + mansion_dz(room_dir),
                                 floor, room_id))
              room_dir = mansion_opposite(room_dir);
            add_room_2x2(room_pos, rotation, room_dir, door_dir, collections[floor]);
          } else if (room_type == 262144 && door_dir == 4) {
            add_room_2x2_secret(room_pos, rotation, collections[floor]);
          }
        }
      }
    }
  }

  void entrance(PlacementData& data) {
    add("entrance", mansion_relative(data.pos, data.rotation, kMansionWest, 9), data.rotation);
    data.pos = mansion_relative(data.pos, data.rotation, kMansionSouth, 16);
  }

  void traverse_wall_piece(PlacementData& data) {
    add(data.wall_type, mansion_relative(data.pos, data.rotation, kMansionEast, 7), data.rotation);
    data.pos = mansion_relative(data.pos, data.rotation, kMansionSouth, 8);
  }

  void traverse_turn(PlacementData& data) {
    data.pos = mansion_relative(data.pos, data.rotation, kMansionSouth, -1);
    add("wall_corner", data.pos, data.rotation);
    data.pos = mansion_relative(data.pos, data.rotation, kMansionSouth, -7);
    data.pos = mansion_relative(data.pos, data.rotation, kMansionWest, -6);
    data.rotation = (data.rotation + 1) & 3u;
  }

  void traverse_inner_turn(PlacementData& data) {
    data.pos = mansion_relative(data.pos, data.rotation, kMansionSouth, 6);
    data.pos = mansion_relative(data.pos, data.rotation, kMansionEast, 8);
    data.rotation = (data.rotation + 3) & 3u;
  }

  void traverse_outer_walls(PlacementData& data, MansionSimpleGrid& wall_grid, int32_t direction,
                            int32_t start_wall_x, int32_t start_wall_y, int32_t end_x,
                            int32_t end_y) {
    int32_t grid_x = start_wall_x;
    int32_t grid_y = start_wall_y;
    const int32_t start_direction = direction;
    do {
      if (!wall_grid.is_house(grid_x + mansion_dx(direction), grid_y + mansion_dz(direction))) {
        traverse_turn(data);
        direction = mansion_clockwise(direction);
        if (grid_x == end_x && grid_y == end_y && start_direction == direction)
          continue;
        traverse_wall_piece(data);
        continue;
      }
      if (wall_grid.is_house(grid_x + mansion_dx(direction), grid_y + mansion_dz(direction)) &&
          wall_grid.is_house(
              grid_x + mansion_dx(direction) + mansion_dx(mansion_counterclockwise(direction)),
              grid_y + mansion_dz(direction) + mansion_dz(mansion_counterclockwise(direction)))) {
        traverse_inner_turn(data);
        grid_x += mansion_dx(direction);
        grid_y += mansion_dz(direction);
        direction = mansion_counterclockwise(direction);
        continue;
      }
      grid_x += mansion_dx(direction);
      grid_y += mansion_dz(direction);
      if (grid_x == end_x && grid_y == end_y && start_direction == direction)
        continue;
      traverse_wall_piece(data);
    } while (grid_x != end_x || grid_y != end_y || start_direction != direction);
  }

  void create_roof(const MansionBlockPos& roof_origin, uint32_t rotation,
                   const MansionSimpleGrid& roof_grid, const MansionSimpleGrid* above_grid) {
    for (int32_t y = 0; y < MansionSimpleGrid::kSize; ++y) {
      for (int32_t x = 0; x < MansionSimpleGrid::kSize; ++x) {
        MansionBlockPos pos =
            mansion_relative(roof_origin, rotation, kMansionSouth, 8 + (y - start_y) * 8);
        pos = mansion_relative(pos, rotation, kMansionEast, (x - start_x) * 8);
        const bool is_above = above_grid != nullptr && above_grid->is_house(x, y);
        if (!roof_grid.is_house(x, y) || is_above)
          continue;
        add("roof", mansion_above(pos, 3), rotation);
        if (!roof_grid.is_house(x + 1, y)) {
          add("roof_front", mansion_relative(pos, rotation, kMansionEast, 6), rotation);
        }
        if (!roof_grid.is_house(x - 1, y)) {
          MansionBlockPos p2 = mansion_relative(pos, rotation, kMansionEast, 0);
          p2 = mansion_relative(p2, rotation, kMansionSouth, 7);
          add("roof_front", p2, (rotation + 2) & 3u);
        }
        if (!roof_grid.is_house(x, y - 1)) {
          add("roof_front", mansion_relative(pos, rotation, kMansionWest, 1), (rotation + 3) & 3u);
        }
        if (!roof_grid.is_house(x, y + 1)) {
          MansionBlockPos p2 = mansion_relative(pos, rotation, kMansionEast, 6);
          p2 = mansion_relative(p2, rotation, kMansionSouth, 6);
          add("roof_front", p2, (rotation + 1) & 3u);
        }
      }
    }
    if (above_grid != nullptr) {
      for (int32_t y = 0; y < MansionSimpleGrid::kSize; ++y) {
        for (int32_t x = 0; x < MansionSimpleGrid::kSize; ++x) {
          MansionBlockPos pos =
              mansion_relative(roof_origin, rotation, kMansionSouth, 8 + (y - start_y) * 8);
          pos = mansion_relative(pos, rotation, kMansionEast, (x - start_x) * 8);
          const bool is_above = above_grid->is_house(x, y);
          if (!roof_grid.is_house(x, y) || !is_above)
            continue;
          if (!roof_grid.is_house(x + 1, y)) {
            add("small_wall", mansion_relative(pos, rotation, kMansionEast, 7), rotation);
          }
          if (!roof_grid.is_house(x - 1, y)) {
            MansionBlockPos p2 = mansion_relative(pos, rotation, kMansionWest, 1);
            p2 = mansion_relative(p2, rotation, kMansionSouth, 6);
            add("small_wall", p2, (rotation + 2) & 3u);
          }
          if (!roof_grid.is_house(x, y - 1)) {
            MansionBlockPos p2 = mansion_relative(pos, rotation, kMansionWest, 0);
            p2 = mansion_relative(p2, rotation, kMansionNorth, 1);
            add("small_wall", p2, (rotation + 3) & 3u);
          }
          if (!roof_grid.is_house(x, y + 1)) {
            MansionBlockPos p2 = mansion_relative(pos, rotation, kMansionEast, 6);
            p2 = mansion_relative(p2, rotation, kMansionSouth, 7);
            add("small_wall", p2, (rotation + 1) & 3u);
          }
          if (!roof_grid.is_house(x + 1, y)) {
            if (!roof_grid.is_house(x, y - 1)) {
              MansionBlockPos p2 = mansion_relative(pos, rotation, kMansionEast, 7);
              p2 = mansion_relative(p2, rotation, kMansionNorth, 2);
              add("small_wall_corner", p2, rotation);
            }
            if (!roof_grid.is_house(x, y + 1)) {
              MansionBlockPos p2 = mansion_relative(pos, rotation, kMansionEast, 8);
              p2 = mansion_relative(p2, rotation, kMansionSouth, 7);
              add("small_wall_corner", p2, (rotation + 1) & 3u);
            }
          }
          if (roof_grid.is_house(x - 1, y))
            continue;
          if (!roof_grid.is_house(x, y - 1)) {
            MansionBlockPos p2 = mansion_relative(pos, rotation, kMansionWest, 2);
            p2 = mansion_relative(p2, rotation, kMansionNorth, 1);
            add("small_wall_corner", p2, (rotation + 3) & 3u);
          }
          if (!roof_grid.is_house(x, y + 1)) {
            MansionBlockPos p2 = mansion_relative(pos, rotation, kMansionWest, 1);
            p2 = mansion_relative(p2, rotation, kMansionSouth, 8);
            add("small_wall_corner", p2, (rotation + 2) & 3u);
          }
        }
      }
    }
    for (int32_t y = 0; y < MansionSimpleGrid::kSize; ++y) {
      for (int32_t x = 0; x < MansionSimpleGrid::kSize; ++x) {
        MansionBlockPos pos =
            mansion_relative(roof_origin, rotation, kMansionSouth, 8 + (y - start_y) * 8);
        pos = mansion_relative(pos, rotation, kMansionEast, (x - start_x) * 8);
        const bool is_above = above_grid != nullptr && above_grid->is_house(x, y);
        if (!roof_grid.is_house(x, y) || is_above)
          continue;
        if (!roof_grid.is_house(x + 1, y)) {
          MansionBlockPos p2 = mansion_relative(pos, rotation, kMansionEast, 6);
          if (!roof_grid.is_house(x, y + 1)) {
            MansionBlockPos p3 = mansion_relative(p2, rotation, kMansionSouth, 6);
            add("roof_corner", p3, rotation);
          } else if (roof_grid.is_house(x + 1, y + 1)) {
            MansionBlockPos p3 = mansion_relative(p2, rotation, kMansionSouth, 5);
            add("roof_inner_corner", p3, rotation);
          }
          if (!roof_grid.is_house(x, y - 1)) {
            add("roof_corner", p2, (rotation + 3) & 3u);
          } else if (roof_grid.is_house(x + 1, y - 1)) {
            MansionBlockPos p3 = mansion_relative(pos, rotation, kMansionEast, 9);
            p3 = mansion_relative(p3, rotation, kMansionNorth, 2);
            add("roof_inner_corner", p3, (rotation + 1) & 3u);
          }
        }
        if (roof_grid.is_house(x - 1, y))
          continue;
        MansionBlockPos p2 = mansion_relative(pos, rotation, kMansionEast, 0);
        p2 = mansion_relative(p2, rotation, kMansionSouth, 0);
        if (!roof_grid.is_house(x, y + 1)) {
          MansionBlockPos p3 = mansion_relative(p2, rotation, kMansionSouth, 6);
          add("roof_corner", p3, (rotation + 1) & 3u);
        } else if (roof_grid.is_house(x - 1, y + 1)) {
          MansionBlockPos p3 = mansion_relative(p2, rotation, kMansionSouth, 8);
          p3 = mansion_relative(p3, rotation, kMansionWest, 3);
          add("roof_inner_corner", p3, (rotation + 3) & 3u);
        }
        if (!roof_grid.is_house(x, y - 1)) {
          add("roof_corner", p2, (rotation + 2) & 3u);
          continue;
        }
        if (roof_grid.is_house(x - 1, y - 1)) {
          MansionBlockPos p3 = mansion_relative(p2, rotation, kMansionSouth, 1);
          add("roof_inner_corner", p3, (rotation + 2) & 3u);
        }
      }
    }
  }

  void add_room_1x1(const MansionBlockPos& room_pos, uint32_t rotation, int32_t door_dir,
                    const MansionRoomCollection& rooms) {
    uint32_t piece_rotation = 0;
    std::string room_type = rooms.get_1x1(random);
    if (door_dir != kMansionEast) {
      if (door_dir == kMansionNorth)
        piece_rotation = 3;
      else if (door_dir == kMansionWest)
        piece_rotation = 2;
      else if (door_dir == kMansionSouth)
        piece_rotation = 1;
      else
        room_type = rooms.get_1x1_secret(random);
    }
    MansionBlockPos orientation = mansion_zero_position(1, 0, piece_rotation);
    piece_rotation = (piece_rotation + rotation) & 3u;
    orientation = mansion_rotate_pos(orientation, rotation);
    add(room_type, mansion_add(room_pos, orientation.x, 0, orientation.z), piece_rotation);
  }

  void add_room_1x2(const MansionBlockPos& room_pos, uint32_t rotation, int32_t room_dir,
                    int32_t door_dir, const MansionRoomCollection& rooms, bool stairs) {
    if (door_dir == kMansionEast && room_dir == kMansionSouth) {
      add(rooms.get_1x2_side(random, stairs), mansion_relative(room_pos, rotation, kMansionEast, 1),
          rotation);
    } else if (door_dir == kMansionEast && room_dir == kMansionNorth) {
      MansionBlockPos pos = mansion_relative(room_pos, rotation, kMansionEast, 1);
      pos = mansion_relative(pos, rotation, kMansionSouth, 6);
      add(rooms.get_1x2_side(random, stairs), pos, rotation, 1);
    } else if (door_dir == kMansionWest && room_dir == kMansionNorth) {
      MansionBlockPos pos = mansion_relative(room_pos, rotation, kMansionEast, 7);
      pos = mansion_relative(pos, rotation, kMansionSouth, 6);
      add(rooms.get_1x2_side(random, stairs), pos, (rotation + 2) & 3u);
    } else if (door_dir == kMansionWest && room_dir == kMansionSouth) {
      add(rooms.get_1x2_side(random, stairs), mansion_relative(room_pos, rotation, kMansionEast, 7),
          rotation, 2);
    } else if (door_dir == kMansionSouth && room_dir == kMansionEast) {
      add(rooms.get_1x2_side(random, stairs), mansion_relative(room_pos, rotation, kMansionEast, 1),
          (rotation + 1) & 3u, 1);
    } else if (door_dir == kMansionSouth && room_dir == kMansionWest) {
      add(rooms.get_1x2_side(random, stairs), mansion_relative(room_pos, rotation, kMansionEast, 7),
          (rotation + 1) & 3u);
    } else if (door_dir == kMansionNorth && room_dir == kMansionWest) {
      MansionBlockPos pos = mansion_relative(room_pos, rotation, kMansionEast, 7);
      pos = mansion_relative(pos, rotation, kMansionSouth, 6);
      add(rooms.get_1x2_side(random, stairs), pos, (rotation + 1) & 3u, 2);
    } else if (door_dir == kMansionNorth && room_dir == kMansionEast) {
      MansionBlockPos pos = mansion_relative(room_pos, rotation, kMansionEast, 1);
      pos = mansion_relative(pos, rotation, kMansionSouth, 6);
      add(rooms.get_1x2_side(random, stairs), pos, (rotation + 3) & 3u);
    } else if (door_dir == kMansionSouth && room_dir == kMansionNorth) {
      MansionBlockPos pos = mansion_relative(room_pos, rotation, kMansionEast, 1);
      pos = mansion_relative(pos, rotation, kMansionNorth, 8);
      add(rooms.get_1x2_front(random, stairs), pos, rotation);
    } else if (door_dir == kMansionNorth && room_dir == kMansionSouth) {
      MansionBlockPos pos = mansion_relative(room_pos, rotation, kMansionEast, 7);
      pos = mansion_relative(pos, rotation, kMansionSouth, 14);
      add(rooms.get_1x2_front(random, stairs), pos, (rotation + 2) & 3u);
    } else if (door_dir == kMansionWest && room_dir == kMansionEast) {
      add(rooms.get_1x2_front(random, stairs),
          mansion_relative(room_pos, rotation, kMansionEast, 15), (rotation + 1) & 3u);
    } else if (door_dir == kMansionEast && room_dir == kMansionWest) {
      MansionBlockPos pos = mansion_relative(room_pos, rotation, kMansionWest, 7);
      pos = mansion_relative(pos, rotation, kMansionSouth, 6);
      add(rooms.get_1x2_front(random, stairs), pos, (rotation + 3) & 3u);
    } else if (door_dir == 4 && room_dir == kMansionEast) {
      add(rooms.get_1x2_secret(random), mansion_relative(room_pos, rotation, kMansionEast, 15),
          (rotation + 1) & 3u);
    } else if (door_dir == 4 && room_dir == kMansionSouth) {
      MansionBlockPos pos = mansion_relative(room_pos, rotation, kMansionEast, 1);
      pos = mansion_relative(pos, rotation, kMansionNorth, 0);
      add(rooms.get_1x2_secret(random), pos, rotation);
    }
  }

  void add_room_2x2(const MansionBlockPos& room_pos, uint32_t rotation, int32_t room_dir,
                    int32_t door_dir, const MansionRoomCollection& rooms) {
    int32_t east = 0;
    int32_t south = 0;
    uint32_t piece_rotation = rotation;
    uint8_t mirror = 0;
    if (door_dir == kMansionEast && room_dir == kMansionSouth) {
      east = -7;
    } else if (door_dir == kMansionEast && room_dir == kMansionNorth) {
      east = -7;
      south = 6;
      mirror = 1;
    } else if (door_dir == kMansionNorth && room_dir == kMansionEast) {
      east = 1;
      south = 14;
      piece_rotation = (rotation + 3) & 3u;
    } else if (door_dir == kMansionNorth && room_dir == kMansionWest) {
      east = 7;
      south = 14;
      piece_rotation = (rotation + 3) & 3u;
      mirror = 1;
    } else if (door_dir == kMansionSouth && room_dir == kMansionWest) {
      east = 7;
      south = -8;
      piece_rotation = (rotation + 1) & 3u;
    } else if (door_dir == kMansionSouth && room_dir == kMansionEast) {
      east = 1;
      south = -8;
      piece_rotation = (rotation + 1) & 3u;
      mirror = 1;
    } else if (door_dir == kMansionWest && room_dir == kMansionNorth) {
      east = 15;
      south = 6;
      piece_rotation = (rotation + 2) & 3u;
    } else if (door_dir == kMansionWest && room_dir == kMansionSouth) {
      east = 15;
      mirror = 2;
    }
    MansionBlockPos pos = mansion_relative(room_pos, rotation, kMansionEast, east);
    pos = mansion_relative(pos, rotation, kMansionSouth, south);
    add(rooms.get_2x2(random), pos, piece_rotation, mirror);
  }

  void add_room_2x2_secret(const MansionBlockPos& room_pos, uint32_t rotation,
                           const MansionRoomCollection& rooms) {
    add(rooms.get_2x2_secret(), mansion_relative(room_pos, rotation, kMansionEast, 1), rotation);
  }

  MansionGrid& grid;
  WgLegacyRandom& random;
  std::vector<WgMansionPiece>& pieces;
  int32_t start_x = 0;
  int32_t start_y = 0;
};

WgMansionStart wg_mansion_start(int64_t seed, int32_t chunk_x, int32_t chunk_z,
                                WgClimateSampler& sampler, WgStructureHeights& heights) {
  WgMansionStart start;
  const WgStructure* structure = wg_structure_by_name("minecraft:mansion");
  if (structure == nullptr)
    return start;

  WgLegacyRandom random(0);
  seed_large_feature(random, seed, chunk_x, chunk_z);
  const uint32_t rotation = static_cast<uint32_t>(random.next_int(4));

  const int32_t block_x = chunk_x * 16 + 7;
  const int32_t block_z = chunk_z * 16 + 7;
  int32_t offset_x = 5;
  int32_t offset_z = 5;
  if (rotation == 1) {
    offset_x = -5;
  } else if (rotation == 2) {
    offset_x = -5;
    offset_z = -5;
  } else if (rotation == 3) {
    offset_z = -5;
  }
  const int32_t corner0 = heights.height(block_x, block_z, WgStructureHeightmap::kWorldSurface) - 1;
  const int32_t corner1 =
      heights.height(block_x, block_z + offset_z, WgStructureHeightmap::kWorldSurface) - 1;
  const int32_t corner2 =
      heights.height(block_x + offset_x, block_z, WgStructureHeightmap::kWorldSurface) - 1;
  const int32_t corner3 =
      heights.height(block_x + offset_x, block_z + offset_z, WgStructureHeightmap::kWorldSurface) -
      1;
  const int32_t start_y = std::min(std::min(corner0, corner1), std::min(corner2, corner3));
  // `findGenerationPoint` gates on y >= 60, then `findValidGenerationPoint`
  // applies the start-biome test at the stub position.
  if (start_y < 60)
    return start;
  if (!wg_structure_start_biome_is_valid(sampler, *structure, block_x, start_y, block_z))
    return start;

  start.structure = structure;
  start.world_seed = seed;
  start.chunk_x = chunk_x;
  start.chunk_z = chunk_z;
  start.rotation = rotation;
  start.x = block_x;
  start.y = start_y;
  start.z = block_z;

  MansionGrid grid(random);
  MansionPiecePlacer placer(grid, random, start.pieces);
  placer.create_mansion(MansionBlockPos{block_x, start_y, block_z}, rotation);
  if (start.pieces.empty())
    return start;

  bool first = true;
  for (const WgMansionPiece& piece : start.pieces) {
    if (piece.templ == nullptr)
      continue;
    const WgPieceBox box =
        wg_piece_box(*piece.templ, piece.rotation, piece.x, piece.y, piece.z, piece.mirror);
    if (first) {
      start.min_x = box.min_x;
      start.min_y = box.min_y;
      start.min_z = box.min_z;
      start.max_x = box.max_x;
      start.max_y = box.max_y;
      start.max_z = box.max_z;
      first = false;
      continue;
    }
    start.min_x = std::min(start.min_x, box.min_x);
    start.min_y = std::min(start.min_y, box.min_y);
    start.min_z = std::min(start.min_z, box.min_z);
    start.max_x = std::max(start.max_x, box.max_x);
    start.max_y = std::max(start.max_y, box.max_y);
    start.max_z = std::max(start.max_z, box.max_z);
  }
  start.valid = !first;
  return start;
}

// `WoodlandMansionStructure.afterPlace`: the cobblestone footing under every
// occupied footprint column, stopping at the first non-air, non-liquid cell.
// It reads the target chunk's own post-piece state, so it runs after the piece
// loop and before the next structure family in `apply_generated_structures`.
uint32_t place_mansion_foundation(Chunk& chunk, const WgMansionStart& start) {
  if (!start.valid)
    return 0;
  const int32_t base_x = chunk.x * 16;
  const int32_t base_z = chunk.z * 16;
  uint32_t changed = 0;
  for (int32_t x = 0; x < 16; ++x) {
    for (int32_t z = 0; z < 16; ++z) {
      const int32_t world_x = base_x + x;
      const int32_t world_z = base_z + z;
      if (world_x < start.min_x || world_x > start.max_x || world_z < start.min_z ||
          world_z > start.max_z)
        continue;
      bool inside_piece = false;
      for (const WgMansionPiece& piece : start.pieces) {
        if (piece.templ == nullptr)
          continue;
        const WgPieceBox box =
            wg_piece_box(*piece.templ, piece.rotation, piece.x, piece.y, piece.z, piece.mirror);
        if (world_x >= box.min_x && world_x <= box.max_x && world_z >= box.min_z &&
            world_z <= box.max_z && start.min_y >= box.min_y && start.min_y <= box.max_y) {
          inside_piece = true;
          break;
        }
      }
      if (!inside_piece)
        continue;
      // Vanilla only foots columns whose top cell (the start's own minY) is
      // already occupied: the wall above must exist for the footing to count.
      if (is_air_state(chunk.block_state_at(x, start.min_y, z)))
        continue;
      for (int32_t y = start.min_y - 1; y > kMinY; --y) {
        const BlockStateKey current = chunk.block_state_at(x, y, z);
        if (!is_air_state(current) && !is_liquid_state(current))
          break;
        if (chunk.set_block_state(x, y, z, BlockStateKey{"minecraft:cobblestone", ""}))
          ++changed;
      }
    }
  }
  return changed;
}

uint32_t wg_place_mansion(Chunk& chunk, const WgMansionStart& start) {
  if (!start.valid)
    return 0;
  const int32_t base_x = chunk.x * 16;
  const int32_t base_z = chunk.z * 16;
  uint32_t changed = 0;
  for (const WgMansionPiece& piece : start.pieces) {
    if (piece.templ == nullptr)
      continue;
    const WgPieceBox box =
        wg_piece_box(*piece.templ, piece.rotation, piece.x, piece.y, piece.z, piece.mirror);
    if (box.max_x < base_x || box.min_x >= base_x + 16 || box.max_z < base_z ||
        box.min_z >= base_z + 16 || box.max_y < kMinY || box.min_y >= kMinY + kHeight)
      continue;
    changed += place_template(chunk, *piece.templ, piece.rotation, piece.x, piece.y, piece.z,
                              start.world_seed, false, true, piece.mirror, true);
  }
  changed += place_mansion_foundation(chunk, start);
  return changed;
}

uint32_t wg_place_ocean_monument(Chunk& chunk, const WgOceanMonumentStart& start) {
  if (!start.valid)
    return 0;
  const int32_t base_x = chunk.x * 16;
  const int32_t base_z = chunk.z * 16;
  const auto intersects = [&](const WgOceanMonumentPiece& piece) {
    return piece.max_x >= base_x && piece.min_x < base_x + 16 && piece.max_z >= base_z &&
           piece.min_z < base_z + 16 && piece.max_y >= kMinY && piece.min_y < kMinY + kHeight;
  };
  if (!intersects(start.building))
    return 0;

  // The building's positional random is shared by every child, and a child
  // whose box misses this chunk is not run at all (vanilla's skip happens
  // before `postProcess`, so it also skips that child's draws).
  WgLegacyRandom random(
      wg_mth_get_seed(start.building.min_x, start.building.min_y, start.building.min_z));
  MonumentWriter building_writer(chunk, start, start.building, random);
  building_writer.building();
  uint32_t changed = building_writer.changed();
  for (const WgOceanMonumentPiece& piece : start.pieces) {
    if (!intersects(piece))
      continue;
    MonumentWriter writer(chunk, start, piece, random);
    changed += writer.run();
  }
  return changed;
}

WgStructureApplyStats apply_generated_structures(const WgOverworldGenerator& generator,
                                                 Chunk& chunk) {
  WgStructureApplyStats stats;
  std::unique_ptr<WgClimateSampler> sampler;
  std::unique_ptr<WgGeneratedHeights> heights;
  const auto ensure_terrain = [&]() {
    if (sampler == nullptr) {
      sampler = std::make_unique<WgClimateSampler>(generator.state());
      heights = std::make_unique<WgGeneratedHeights>(generator);
    }
  };

  // A buried treasure chest sits at the source chunk's (9, 9) column.
  if (const WgStructureSet* set = wg_structure_set_by_name("minecraft:buried_treasures")) {
    if (wg_structure_is_chunk(generator.seed(), *set, chunk.x, chunk.z)) {
      ensure_terrain();
      ++stats.candidates;
      const WgBuriedTreasureStart start =
          wg_buried_treasure_start(generator.seed(), chunk.x, chunk.z, *sampler, *heights);
      if (start.valid) {
        ++stats.starts;
        const uint32_t placed = wg_place_buried_treasure(chunk, start, *heights);
        if (placed > 0)
          ++stats.pieces;
        stats.blocks += placed;
      }
    }
  }

  // The jungle temple is a single 12 x 15 piece anchored at the source
  // chunk's minimum corner; the footprint fits inside that chunk, so only the
  // source chunk can own it.
  if (const WgStructureSet* set = wg_structure_set_by_name("minecraft:jungle_temples")) {
    if (wg_structure_is_chunk(generator.seed(), *set, chunk.x, chunk.z)) {
      ensure_terrain();
      ++stats.candidates;
      const WgJungleTempleStart start =
          wg_jungle_temple_start(generator.seed(), chunk.x, chunk.z, *sampler, *heights);
      if (start.valid) {
        ++stats.starts;
        const uint32_t placed = wg_place_jungle_temple(chunk, start);
        if (placed > 0)
          ++stats.pieces;
        stats.blocks += placed;
      }
    }
  }

  // The desert pyramid is a single 21 x 21 piece anchored at the source
  // chunk's minimum corner. Its footprint reaches one chunk east and south, so
  // a source in the shared scan window owns part of this chunk too.
  if (const WgStructureSet* set = wg_structure_set_by_name("minecraft:desert_pyramids")) {
    for (int32_t dz = -kWgPieceChunkReach; dz <= kWgPieceChunkReach; ++dz) {
      for (int32_t dx = -kWgPieceChunkReach; dx <= kWgPieceChunkReach; ++dx) {
        const int32_t source_x = chunk.x + dx;
        const int32_t source_z = chunk.z + dz;
        if (!wg_structure_is_chunk(generator.seed(), *set, source_x, source_z))
          continue;
        ensure_terrain();
        ++stats.candidates;
        const WgDesertPyramidStart start =
            wg_desert_pyramid_start(generator.seed(), source_x, source_z, *sampler, *heights);
        if (!start.valid || start.max_x < chunk.x * 16 || start.min_x >= chunk.x * 16 + 16 ||
            start.max_z < chunk.z * 16 || start.min_z >= chunk.z * 16 + 16)
          continue;
        ++stats.starts;
        const uint32_t placed = wg_place_desert_pyramid(chunk, start, *heights);
        if (placed > 0)
          ++stats.pieces;
        stats.blocks += placed;
      }
    }
  }

  // Mineshafts: the set's spacing is one chunk, so its gate is the 0.004
  // probability draw and the source window is the assembly's 80-block reach
  // plus a corridor or room's own extent. `kWgMineshaftChunkReach` is the
  // conservative 8 chunks (128 blocks) that bound allows.
  if (const WgStructureSet* set = wg_structure_set_by_name("minecraft:mineshafts")) {
    constexpr int32_t kMineshaftChunkReach = 8;
    for (int32_t dz = -kMineshaftChunkReach; dz <= kMineshaftChunkReach; ++dz) {
      for (int32_t dx = -kMineshaftChunkReach; dx <= kMineshaftChunkReach; ++dx) {
        const int32_t source_x = chunk.x + dx;
        const int32_t source_z = chunk.z + dz;
        if (!wg_structure_is_chunk(generator.seed(), *set, source_x, source_z))
          continue;
        ensure_terrain();
        ++stats.candidates;
        const WgMineshaftStart start =
            wg_mineshaft_start(generator.seed(), source_x, source_z, *sampler, *heights);
        if (!start.valid || start.max_x < chunk.x * 16 || start.min_x >= chunk.x * 16 + 16 ||
            start.max_z < chunk.z * 16 || start.min_z >= chunk.z * 16 + 16 || start.max_y < kMinY ||
            start.min_y >= kMinY + kHeight)
          continue;
        ++stats.starts;
        const uint32_t placed = wg_place_mineshaft(chunk, start, *heights, *sampler);
        if (placed > 0)
          ++stats.pieces;
        stats.blocks += placed;
      }
    }
  }

  // Strongholds are the concentric-ring set. The ring list is the placement
  // state's cached answer, not `wg_structure_is_chunk` (which refuses a
  // zero-spacing row). A ring position may repeat, so a source chunk is applied
  // once; the assembly's 112-block reach plus a piece's own extent fits in the
  // eight-chunk window the other families use.
  if (const WgStructureSet* set = wg_structure_set_by_name("minecraft:strongholds")) {
    constexpr int32_t kStrongholdChunkReach = 8;
    const auto& rings = generator.structure_placement_state().ring_positions(set->placement);
    std::vector<WgStructureChunk> seen;
    for (const WgStructureChunk& source : rings) {
      if (source.x < chunk.x - kStrongholdChunkReach ||
          source.x > chunk.x + kStrongholdChunkReach ||
          source.z < chunk.z - kStrongholdChunkReach || source.z > chunk.z + kStrongholdChunkReach)
        continue;
      bool duplicate = false;
      for (const WgStructureChunk& earlier : seen)
        if (earlier.x == source.x && earlier.z == source.z)
          duplicate = true;
      if (duplicate)
        continue;
      seen.push_back(source);
      ensure_terrain();
      ++stats.candidates;
      const WgStrongholdStart start =
          wg_stronghold_start(generator.seed(), source.x, source.z, *sampler);
      if (!start.valid || start.max_x < chunk.x * 16 || start.min_x >= chunk.x * 16 + 16 ||
          start.max_z < chunk.z * 16 || start.min_z >= chunk.z * 16 + 16 || start.max_y < kMinY ||
          start.min_y >= kMinY + kHeight)
        continue;
      ++stats.starts;
      const uint32_t placed = wg_place_stronghold(chunk, start);
      if (placed > 0)
        ++stats.pieces;
      stats.blocks += placed;
    }
  }

  // Ocean monuments are one 58 x 58 building centred on the source chunk; its
  // box reaches two chunks west/north and one east/south, so the shared two-
  // chunk scan window is exact. The building reads the target chunk's own
  // cells for its water shell.
  if (const WgStructureSet* set = wg_structure_set_by_name("minecraft:ocean_monuments")) {
    for (int32_t dz = -kWgPieceChunkReach; dz <= kWgPieceChunkReach; ++dz) {
      for (int32_t dx = -kWgPieceChunkReach; dx <= kWgPieceChunkReach; ++dx) {
        const int32_t source_x = chunk.x + dx;
        const int32_t source_z = chunk.z + dz;
        if (!wg_structure_is_chunk(generator.seed(), *set, source_x, source_z))
          continue;
        ensure_terrain();
        ++stats.candidates;
        const WgOceanMonumentStart start =
            wg_ocean_monument_start(generator.seed(), source_x, source_z, *sampler, *heights);
        if (!start.valid || start.max_x < chunk.x * 16 || start.min_x >= chunk.x * 16 + 16 ||
            start.max_z < chunk.z * 16 || start.min_z >= chunk.z * 16 + 16 || start.max_y < kMinY ||
            start.min_y >= kMinY + kHeight)
          continue;
        ++stats.starts;
        const uint32_t placed = wg_place_ocean_monument(chunk, start);
        if (placed > 0)
          ++stats.pieces;
        stats.blocks += placed;
      }
    }
  }

  // Ocean ruins and their clusters reach two chunks from the source; each
  // piece is snapped to the ocean floor independently.
  if (const WgStructureSet* set = wg_structure_set_by_name("minecraft:ocean_ruins")) {
    for (int32_t dz = -kWgPieceChunkReach; dz <= kWgPieceChunkReach; ++dz) {
      for (int32_t dx = -kWgPieceChunkReach; dx <= kWgPieceChunkReach; ++dx) {
        const int32_t source_x = chunk.x + dx;
        const int32_t source_z = chunk.z + dz;
        if (!wg_structure_is_chunk(generator.seed(), *set, source_x, source_z))
          continue;
        ensure_terrain();
        ++stats.candidates;
        const WgOceanRuinStart start =
            wg_ocean_ruin_start(generator.seed(), source_x, source_z, *sampler, *heights);
        if (!start.valid)
          continue;
        bool touches = false;
        for (uint32_t i = 0; i < start.piece_count && !touches; ++i) {
          const WgOceanRuinPiece& piece = start.pieces[i];
          if (piece.templ == nullptr)
            continue;
          const WgPieceBox box =
              wg_piece_box(*piece.templ, piece.rotation, piece.x, piece.y, piece.z);
          touches = box.max_x >= chunk.x * 16 && box.min_x < chunk.x * 16 + 16 &&
                    box.max_z >= chunk.z * 16 && box.min_z < chunk.z * 16 + 16;
        }
        if (!touches)
          continue;
        ++stats.starts;
        const uint32_t placed = wg_place_ocean_ruin(chunk, start, *heights);
        if (placed > 0)
          ++stats.pieces;
        stats.blocks += placed;
      }
    }
  }

  // Ruined portals are one template piece, up to 21 wide.
  if (const WgStructureSet* set = wg_structure_set_by_name("minecraft:ruined_portals")) {
    for (int32_t dz = -kWgPieceChunkReach; dz <= kWgPieceChunkReach; ++dz) {
      for (int32_t dx = -kWgPieceChunkReach; dx <= kWgPieceChunkReach; ++dx) {
        const int32_t source_x = chunk.x + dx;
        const int32_t source_z = chunk.z + dz;
        if (!wg_structure_is_chunk(generator.seed(), *set, source_x, source_z))
          continue;
        ensure_terrain();
        ++stats.candidates;
        const WgRuinedPortalStart start =
            wg_ruined_portal_start(generator.seed(), source_x, source_z, *heights);
        if (!start.valid || start.max_x < chunk.x * 16 || start.min_x >= chunk.x * 16 + 16 ||
            start.max_z < chunk.z * 16 || start.min_z >= chunk.z * 16 + 16)
          continue;
        ++stats.starts;
        const uint32_t placed = wg_place_ruined_portal(chunk, start, *heights);
        if (placed > 0)
          ++stats.pieces;
        stats.blocks += placed;
      }
    }
  }

  // Jigsaw villages reach two chunks from the source; assembly is deterministic
  // from the source chunk's seed and the surface heights.
  if (const WgStructureSet* set = wg_structure_set_by_name("minecraft:villages")) {
    for (int32_t dz = -kWgPieceChunkReach; dz <= kWgPieceChunkReach; ++dz) {
      for (int32_t dx = -kWgPieceChunkReach; dx <= kWgPieceChunkReach; ++dx) {
        const int32_t source_x = chunk.x + dx;
        const int32_t source_z = chunk.z + dz;
        if (!wg_structure_is_chunk(generator.seed(), *set, source_x, source_z))
          continue;
        ensure_terrain();
        ++stats.candidates;
        const WgJigsawStart start =
            wg_village_start(generator.seed(), source_x, source_z, *sampler, *heights);
        if (!start.valid)
          continue;
        bool touches = false;
        for (uint32_t i = 0; i < start.piece_count && !touches; ++i) {
          const WgJigsawPiece& piece = start.pieces[i];
          if (piece.element == nullptr)
            continue;
          const WgPieceBox& box = piece.box;
          touches = box.max_x >= chunk.x * 16 && box.min_x < chunk.x * 16 + 16 &&
                    box.max_z >= chunk.z * 16 && box.min_z < chunk.z * 16 + 16;
        }
        if (!touches)
          continue;
        ++stats.starts;
        const uint32_t placed = wg_place_village(chunk, start, *heights);
        if (placed > 0)
          ++stats.pieces;
        stats.blocks += placed;
      }
    }
  }

  // Pillager outposts are the same jigsaw assembly with a size-7 graph. Their
  // `max_distance_from_center` is 80, so the assembled box can touch a chunk up
  // to five chunks from the source; the piece boxes filter the window.
  if (const WgStructureSet* set = wg_structure_set_by_name("minecraft:pillager_outposts")) {
    constexpr int32_t kOutpostChunkReach = 5;
    for (int32_t dz = -kOutpostChunkReach; dz <= kOutpostChunkReach; ++dz) {
      for (int32_t dx = -kOutpostChunkReach; dx <= kOutpostChunkReach; ++dx) {
        const int32_t source_x = chunk.x + dx;
        const int32_t source_z = chunk.z + dz;
        if (!wg_structure_is_chunk(generator.seed(), *set, source_x, source_z))
          continue;
        ensure_terrain();
        ++stats.candidates;
        const WgJigsawStart start =
            wg_outpost_start(generator.seed(), source_x, source_z, *sampler, *heights);
        if (!start.valid)
          continue;
        bool touches = false;
        for (uint32_t i = 0; i < start.piece_count && !touches; ++i) {
          const WgJigsawPiece& piece = start.pieces[i];
          if (piece.element == nullptr)
            continue;
          const WgPieceBox& box = piece.box;
          touches = box.max_x >= chunk.x * 16 && box.min_x < chunk.x * 16 + 16 &&
                    box.max_z >= chunk.z * 16 && box.min_z < chunk.z * 16 + 16;
        }
        if (!touches)
          continue;
        ++stats.starts;
        const uint32_t placed = wg_place_outpost(chunk, start, *heights);
        if (placed > 0)
          ++stats.pieces;
        stats.blocks += placed;
      }
    }
  }

  // Ancient cities are deep-dark jigsaw graphs with a 116-block centre bound,
  // so an assembled city can touch a chunk eight chunks from its source. The
  // biome gate inside the start keeps the scan cheap in ordinary terrain: only
  // a candidate whose stub column is deep dark builds pieces.
  if (const WgStructureSet* set = wg_structure_set_by_name("minecraft:ancient_cities")) {
    constexpr int32_t kAncientCityChunkReach = 8;
    for (int32_t dz = -kAncientCityChunkReach; dz <= kAncientCityChunkReach; ++dz) {
      for (int32_t dx = -kAncientCityChunkReach; dx <= kAncientCityChunkReach; ++dx) {
        const int32_t source_x = chunk.x + dx;
        const int32_t source_z = chunk.z + dz;
        if (!wg_structure_is_chunk(generator.seed(), *set, source_x, source_z))
          continue;
        ensure_terrain();
        ++stats.candidates;
        const WgJigsawStart start =
            wg_ancient_city_start(generator.seed(), source_x, source_z, *sampler, *heights);
        if (!start.valid)
          continue;
        bool touches = false;
        for (uint32_t i = 0; i < start.piece_count && !touches; ++i) {
          const WgJigsawPiece& piece = start.pieces[i];
          if (piece.element == nullptr)
            continue;
          const WgPieceBox& box = piece.box;
          touches = box.max_x >= chunk.x * 16 && box.min_x < chunk.x * 16 + 16 &&
                    box.max_z >= chunk.z * 16 && box.min_z < chunk.z * 16 + 16;
        }
        if (!touches)
          continue;
        ++stats.starts;
        const uint32_t placed = wg_place_ancient_city(chunk, start, *heights);
        if (placed > 0)
          ++stats.pieces;
        stats.blocks += placed;
      }
    }
  }

  // Trail ruins are a size-7 graph with the same 80-block bound as an outpost,
  // so a source five chunks away can still touch this chunk. The pieces are
  // rigid and the archaeology lists' capped passes are applied by
  // `wg_place_jigsaw`'s whole-piece path.
  if (const WgStructureSet* set = wg_structure_set_by_name("minecraft:trail_ruins")) {
    constexpr int32_t kTrailRuinsChunkReach = 5;
    for (int32_t dz = -kTrailRuinsChunkReach; dz <= kTrailRuinsChunkReach; ++dz) {
      for (int32_t dx = -kTrailRuinsChunkReach; dx <= kTrailRuinsChunkReach; ++dx) {
        const int32_t source_x = chunk.x + dx;
        const int32_t source_z = chunk.z + dz;
        if (!wg_structure_is_chunk(generator.seed(), *set, source_x, source_z))
          continue;
        ensure_terrain();
        ++stats.candidates;
        const WgJigsawStart start =
            wg_trail_ruins_start(generator.seed(), source_x, source_z, *sampler, *heights);
        if (!start.valid)
          continue;
        bool touches = false;
        for (uint32_t i = 0; i < start.piece_count && !touches; ++i) {
          const WgJigsawPiece& piece = start.pieces[i];
          if (piece.element == nullptr)
            continue;
          const WgPieceBox& box = piece.box;
          touches = box.max_x >= chunk.x * 16 && box.min_x < chunk.x * 16 + 16 &&
                    box.max_z >= chunk.z * 16 && box.min_z < chunk.z * 16 + 16;
        }
        if (!touches)
          continue;
        ++stats.starts;
        const uint32_t placed = wg_place_trail_ruins(chunk, start, *heights);
        if (placed > 0)
          ++stats.pieces;
        stats.blocks += placed;
      }
    }
  }

  // An igloo's pieces reach at most one chunk from their source; the scan
  // covers that window and the box test filters the pieces.
  if (const WgStructureSet* set = wg_structure_set_by_name("minecraft:igloos")) {
    for (int32_t dz = -kWgPieceChunkReach; dz <= kWgPieceChunkReach; ++dz) {
      for (int32_t dx = -kWgPieceChunkReach; dx <= kWgPieceChunkReach; ++dx) {
        const int32_t source_x = chunk.x + dx;
        const int32_t source_z = chunk.z + dz;
        if (!wg_structure_is_chunk(generator.seed(), *set, source_x, source_z))
          continue;
        ensure_terrain();
        ++stats.candidates;
        const WgIglooStart start =
            wg_igloo_start(generator.seed(), source_x, source_z, *sampler, *heights);
        if (!start.valid)
          continue;
        bool touches = false;
        for (uint32_t i = 0; i < start.piece_count && !touches; ++i) {
          const WgIglooPiece& piece = start.pieces[i];
          const WgPieceBox box =
              wg_piece_box(*piece.templ, piece.rotation, piece.x, piece.y, piece.z);
          touches = box.max_x >= chunk.x * 16 && box.min_x < chunk.x * 16 + 16 &&
                    box.max_z >= chunk.z * 16 && box.min_z < chunk.z * 16 + 16;
        }
        if (!touches)
          continue;
        ++stats.starts;
        const uint32_t placed = wg_place_igloo(chunk, start, *heights);
        if (placed > 0)
          ++stats.pieces;
        stats.blocks += placed;
      }
    }
  }

  // A shipwreck template's box reaches at most one chunk west and one east of
  // its source chunk (`kWgPieceChunkReach`), so a source outside that window
  // cannot write here.
  if (const WgStructureSet* set = wg_structure_set_by_name("minecraft:shipwrecks")) {
    for (int32_t dz = -kWgPieceChunkReach; dz <= kWgPieceChunkReach; ++dz) {
      for (int32_t dx = -kWgPieceChunkReach; dx <= kWgPieceChunkReach; ++dx) {
        const int32_t source_x = chunk.x + dx;
        const int32_t source_z = chunk.z + dz;
        if (!wg_structure_is_chunk(generator.seed(), *set, source_x, source_z))
          continue;
        ensure_terrain();
        ++stats.candidates;
        const WgShipwreckStart start =
            wg_shipwreck_start(generator.seed(), source_x, source_z, *sampler, *heights);
        if (!start.valid || !box_hits_chunk(start, chunk.x, chunk.z))
          continue;
        ++stats.starts;
        const uint32_t placed = place_piece(chunk, start, *heights);
        if (placed > 0)
          ++stats.pieces;
        stats.blocks += placed;
      }
    }
  }

  // Woodland mansions start at the source chunk's (7, 7) column and their
  // outer-wall/roof pieces reach about seven chunks from it. The source scan is
  // wider than the shared two-chunk window; the assembled union box filters
  // the pieces that actually touch this chunk.
  if (const WgStructureSet* set = wg_structure_set_by_name("minecraft:woodland_mansions")) {
    constexpr int32_t kMansionChunkReach = 7;
    for (int32_t dz = -kMansionChunkReach; dz <= kMansionChunkReach; ++dz) {
      for (int32_t dx = -kMansionChunkReach; dx <= kMansionChunkReach; ++dx) {
        const int32_t source_x = chunk.x + dx;
        const int32_t source_z = chunk.z + dz;
        if (!wg_structure_is_chunk(generator.seed(), *set, source_x, source_z))
          continue;
        ensure_terrain();
        ++stats.candidates;
        const WgMansionStart start =
            wg_mansion_start(generator.seed(), source_x, source_z, *sampler, *heights);
        if (!start.valid || start.max_x < chunk.x * 16 || start.min_x >= chunk.x * 16 + 16 ||
            start.max_z < chunk.z * 16 || start.min_z >= chunk.z * 16 + 16 || start.max_y < kMinY ||
            start.min_y >= kMinY + kHeight)
          continue;
        ++stats.starts;
        const uint32_t placed = wg_place_mansion(chunk, start);
        if (placed > 0)
          ++stats.pieces;
        stats.blocks += placed;
      }
    }
  }

  // A swamp hut's whole 7 x 9 box starts at the source chunk's minimum corner,
  // so only the chunk itself can be its source. Vanilla's set order puts
  // shipwrecks before swamp huts; the two steps above follow it.
  if (const WgStructureSet* set = wg_structure_set_by_name("minecraft:swamp_huts")) {
    if (wg_structure_is_chunk(generator.seed(), *set, chunk.x, chunk.z)) {
      ensure_terrain();
      ++stats.candidates;
      const WgSwampHutStart start =
          wg_swamp_hut_start(generator.seed(), chunk.x, chunk.z, *sampler, *heights);
      if (start.valid) {
        ++stats.starts;
        const uint32_t placed = wg_place_swamp_hut(chunk, start);
        if (placed > 0)
          ++stats.pieces;
        stats.blocks += placed;
      }
    }
  }
  return stats;
}

} // namespace mc::world
