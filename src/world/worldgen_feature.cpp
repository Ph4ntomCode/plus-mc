// SPDX-License-Identifier: GPL-3.0-or-later
//
// The feature stage behind `world/worldgen_feature.h`. The placement pipeline
// follows `PlacementModifier.getPositions`, the feature applications follow
// their vanilla `Feature.place` (OreFeature, ScatteredOreFeature,
// SimpleBlockFeature, BlockPileFeature, DiskFeature, SpringFeature,
// BlockColumnFeature with its `IntProvider` heights and `BlockStateProvider`
// layers, TreeFeature with its trunk and foliage placers, MonsterRoomFeature,
// SnowAndFreezeFeature, SpikeFeature, GeodeFeature, and the three selector
// features), and the only registry entries this file does not apply — the two
// template-based fossils — are counted in `WgDecorationStats`.
//
// Two approximations are deliberate and named in the header: the per-feature
// random stream is this port's own (not vanilla's `WorldgenRandom` sequence),
// and a feature is confined to the chunk it decorates. The tree foliage shapes
// follow the decoded placers' own algorithms; where a placer derives its crown
// height or radius from the trunk (spruce, pine) the generator's stored
// parameter is used directly, which the table's own comment names.
#include "world/worldgen_feature.h"
#include "world/worldgen_fossil.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "world/chunk.h"
#include "world/container.h" // with_property
#include "world/leaves.h"    // settle_generated_leaf_distances
#include "world/registry.h"  // default_block_state
#include "world/weather.h"   // biome_precipitation_at, kOverworldSeaLevel
#include "world/worldgen_chunk.h"
#include "world/worldgen_feature_table.h"
#include "world/worldgen_loot.h"
#include "world/worldgen_noise.h"
#include "world/worldgen_random.h"
#include "world/worldgen_state.h"
#include "world/worldgen_surface.h"
#include "world/worldgen_table.h"

namespace mc::world {

namespace {

constexpr int kMinY = kWgMinY;
constexpr int kHeight = kWgHeight;
constexpr int kSteps = 11; // GenerationStep.Decoration values 0..10
static_assert(kWgFeatureKindCount == kFeatureKindSlots,
              "WgDecorationStats::applied_by_kind is sized for WgFeatureKind");
constexpr size_t kMaxPositions = 4096;
constexpr int kMaxDepth = 8; // selector nesting guard
constexpr float kPiF = 3.1415927f;

// The 3x3 chunk region a feature stage is currently writing to. Vanilla runs
// `Feature.place` against a `WorldGenRegion`; without this, a vein or canopy
// that crosses a chunk border was clipped at the border and the missing half
// never appeared in either column. The array is row-major around its centre
// (index 4), the same layout `fill_generated_dimension_decorated_chunk` builds.
//
// It is thread-local because the generation pool decorates more than one column
// at once: each worker passes its own stack-owned region, and a process-global
// pointer let the Nether workers overwrite each other's array (the End path has
// no region and was unaffected).
thread_local Chunk* const* g_feature_region = nullptr;

int floor_div16(int value) {
  const int quotient = value / 16;
  return value % 16 < 0 ? quotient - 1 : quotient;
}

int floor_mod16(int value) {
  const int remainder = value % 16;
  return remainder < 0 ? remainder + 16 : remainder;
}

Chunk* region_chunk_at(int wx, int wz) {
  if (g_feature_region == nullptr)
    return nullptr;
  const int centre_x = g_feature_region[4]->x;
  const int centre_z = g_feature_region[4]->z;
  const int dx = floor_div16(wx) - centre_x + 1;
  const int dz = floor_div16(wz) - centre_z + 1;
  if (dx < 0 || dx > 2 || dz < 0 || dz > 2)
    return nullptr;
  return g_feature_region[dz * 3 + dx];
}

struct Pos {
  int x = 0;
  int y = 0;
  int z = 0;
};

bool str_less(const char* a, const char* b) {
  return std::strcmp(a, b) < 0;
}

template <typename Row> const Row* find_row(const Row* rows, size_t count, const char* name) {
  const Row* end = rows + count;
  const Row* it = std::lower_bound(
      rows, end, name, [](const Row& row, const char* key) { return str_less(row.name, key); });
  if (it == end || std::strcmp(it->name, name) != 0)
    return nullptr;
  return it;
}

// The same lookup for a `std::string_view` name, which the extra-kind
// dispatchers below hold registry ids as. The table's names are C strings, so
// the view is materialised once rather than truncating at its first NUL (a
// `string_view` is not promised to be terminated).
template <typename Row> const Row* find_row(const Row* rows, size_t count, std::string_view name) {
  const std::string owned(name);
  return find_row(rows, count, owned.c_str());
}

const WgBiomeFeatures* find_biome(const char* name) {
  const WgBiomeFeatures* end = kWgBiomeFeatures + std::size(kWgBiomeFeatures);
  const WgBiomeFeatures* it = std::lower_bound(
      kWgBiomeFeatures, end, name,
      [](const WgBiomeFeatures& row, const char* key) { return str_less(row.biome, key); });
  if (it == end || std::strcmp(it->biome, name) != 0)
    return nullptr;
  return it;
}

// A `|`-joined name list membership test (the generator's canonical form for a
// resolved tag or a multi-block reference).
bool name_in_list(std::string_view list, std::string_view name) {
  size_t start = 0;
  while (start <= list.size()) {
    const size_t bar = list.find('|', start);
    const std::string_view piece =
        list.substr(start, bar == std::string_view::npos ? std::string_view::npos : bar - start);
    if (piece == name)
      return true;
    if (bar == std::string_view::npos)
      break;
    start = bar + 1;
  }
  return false;
}

// `Mth.randomBetweenInclusive(random, min, max)`.
int random_between_inclusive(WgRandom& rng, int min, int max) {
  if (min >= max)
    return min;
  return min + rng.next_int(max - min + 1);
}

// `VerticalAnchor.resolveY` in the column's own generation context. The
// overworld is -64/384, the Nether 0/128 and the End 0/256; a constant kMinY
// here sends every Nether `above_bottom` height to negative Y.
int resolve_anchor(int kind, int value, const Chunk& chunk) {
  const int min_y = chunk.min_section_y * 16;
  const int max_y = min_y + static_cast<int>(chunk.sections.size()) * 16;
  switch (kind) {
  case 1:
    return min_y + value; // above_bottom
  case 2:
    return max_y - 1 - value; // below_top
  default:
    return value; // absolute
  }
}

// A count encoded as a plain JSON number is vanilla's `ConstantInt`, whose
// `sample` returns its value without touching the random source. The generated
// table flattens both constant and ranged counts to `[i0, i1]`, so an equal
// pair is the constant case. Calling `nextInt(1)` here would consume a draw
// that vanilla never consumes and shift every following placement in the
// feature (fixed counts are the common case: ore veins, vegetation patches).
int sample_count(WgRandom& rng, int min, int max) {
  return min == max ? min : random_between_inclusive(rng, min, max);
}

int sample_height(WgRandom& rng, const WgPlacement& m, const Chunk& chunk) {
  const int code = m.i0;
  if (code == 0) // constant
    return resolve_anchor(m.i1, m.i2, chunk);
  const int min = resolve_anchor(m.i1, m.i2, chunk);
  const int max = resolve_anchor(m.i3, m.i4, chunk);
  if (code == 2) { // trapezoid
    if (min > max)
      return min;
    const int range = max - min;
    const int plateau = m.i5;
    if (plateau >= range)
      return random_between_inclusive(rng, min, max);
    const int plateau_start = (range - plateau) / 2;
    const int plateau_end = range - plateau_start;
    return min + random_between_inclusive(rng, 0, plateau_end) +
           random_between_inclusive(rng, 0, plateau_start);
  }
  return random_between_inclusive(rng, min, max); // uniform
}

bool is_air_name(std::string_view name) {
  return name == "minecraft:air" || name == "minecraft:cave_air" || name == "minecraft:void_air";
}

bool is_fluid_name(std::string_view name) {
  return name == "minecraft:water" || name == "minecraft:flowing_water" ||
         name == "minecraft:lava" || name == "minecraft:flowing_lava";
}

// Air or one of the `#minecraft:replaceable_by_trees` plants: the set
// `TreeFeature.validTreePos` tests, which is `is_replaceable_name` without the
// fluids.
bool is_replaceable_by_trees_name(std::string_view name) {
  if (is_air_name(name))
    return true;
  // Every vanilla leaf id ends in `_leaves` (oak through pale_oak), which is the
  // whole `#minecraft:leaves` entry the tag pulls in.
  if (name.size() > 7 && name.substr(name.size() - 7) == "_leaves")
    return true;
  static constexpr const char* kPlants[] = {
      "minecraft:short_grass", "minecraft:tall_grass",
      "minecraft:fern",        "minecraft:large_fern",
      "minecraft:dead_bush",   "minecraft:seagrass",
      "minecraft:snow",        "minecraft:vine",
      "minecraft:glow_lichen", "minecraft:sweet_berry_bush",
      "minecraft:fire",        "minecraft:soul_fire",
  };
  for (const char* plant : kPlants)
    if (name == plant)
      return true;
  return false;
}

// A cell a feature may write over (vanilla's `isReplaceable`/air check, kept to
// the names the decoration stage actually meets).
bool is_replaceable_name(std::string_view name) {
  return is_fluid_name(name) || is_replaceable_by_trees_name(name);
}

bool in_chunk(const Chunk& chunk, int wx, int wz) {
  if (g_feature_region != nullptr)
    return region_chunk_at(wx, wz) != nullptr;
  const int lx = wx - chunk.x * 16;
  const int lz = wz - chunk.z * 16;
  return lx >= 0 && lx < 16 && lz >= 0 && lz < 16;
}

// The column's own vertical bounds. The feature stage is shared by all three
// dimensions, whose min_y/height are 0/128 (Nether), 0/256 (End) and -64/384
// (overworld), so a constant `kMinY` here would read the wrong section in the
// two dimension columns.
int chunk_min_y(const Chunk& chunk) {
  return chunk.min_section_y * 16;
}
int chunk_max_y(const Chunk& chunk) {
  return chunk_min_y(chunk) + static_cast<int>(chunk.sections.size()) * 16;
}

// The state at a world position; synthetic air outside the column.
BlockStateKey block_at(const Chunk& chunk, int wx, int wy, int wz) {
  if (g_feature_region != nullptr) {
    const Chunk* const source = region_chunk_at(wx, wz);
    if (source == nullptr || wy < chunk_min_y(*source) || wy >= chunk_max_y(*source))
      return BlockStateKey{"minecraft:air", ""};
    return source->block_state_at(floor_mod16(wx), wy, floor_mod16(wz));
  }
  const int lx = wx - chunk.x * 16;
  const int lz = wz - chunk.z * 16;
  if (lx < 0 || lx >= 16 || lz < 0 || lz >= 16 || wy < chunk_min_y(chunk) ||
      wy >= chunk_max_y(chunk))
    return BlockStateKey{"minecraft:air", ""};
  return chunk.block_state_at(lx, wy, lz);
}

// The complete, canonical property string a feature-written state resolves to:
// the block's vanilla default state with the feature's own properties applied.
// The chunk stores whole states — the block-state table looks up exact strings
// — so a feature that names one face of a vine or a lichen has to spell out the
// rest, or the encoder cannot resolve the palette entry and the whole column is
// sent as the void fallback (see `chunk_body_for`). `with_property` keeps the
// canonical key-sorted form.
std::string canonical_properties(std::string_view name, std::string_view properties) {
  const auto def = default_block_state(name);
  if (!def)
    return std::string(properties);
  std::string props = def->key.properties;
  size_t start = 0;
  while (start < properties.size()) {
    const size_t comma = properties.find(',', start);
    const std::string_view pair = properties.substr(
        start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
    const size_t eq = pair.find('=');
    if (eq != std::string_view::npos) {
      const std::string_view key = pair.substr(0, eq);
      // A property the block does not carry cannot exist on it: the pack's
      // springs name `falling=true` on water and lava, which is a pre-1.13
      // block property the fluid state now holds instead. Ignore it rather than
      // write a state the registry cannot resolve.
      if (!block_property(props, key).empty())
        props = with_property(props, key, pair.substr(eq + 1));
    }
    if (comma == std::string_view::npos)
      break;
    start = comma + 1;
  }
  return props;
}

// Whole-state equality against a config's state (vanilla's `state ==
// config.state()`); the properties are canonicalised the way `try_set` writes
// them, so a config that names only a name still compares its default state.
bool is_state(const BlockStateKey& key, const WgState& state) {
  return key.name == state.name &&
         key.properties == canonical_properties(state.name, state.properties);
}

// Writes a state if the cell is inside the column and replaceable; returns
// whether anything changed.
bool try_set(Chunk& chunk, int wx, int wy, int wz, std::string_view name,
             std::string_view properties, bool only_replaceable) {
  Chunk* destination = g_feature_region != nullptr ? region_chunk_at(wx, wz) : &chunk;
  if (destination == nullptr || !in_chunk(chunk, wx, wz) || wy < chunk_min_y(*destination) ||
      wy >= chunk_max_y(*destination))
    return false;
  const int lx = floor_mod16(wx);
  const int lz = floor_mod16(wz);
  if (only_replaceable && !is_replaceable_name(destination->block_state_at(lx, wy, lz).name))
    return false;
  return destination->set_block_state(
      lx, wy, lz, BlockStateKey{std::string(name), canonical_properties(name, properties)});
}

// The biome name at a world position, from the chunk's own 4x4x4 containers.
const char* biome_at(const Chunk& chunk, int wx, int wy, int wz) {
  const Chunk* source = g_feature_region != nullptr ? region_chunk_at(wx, wz) : &chunk;
  if (source == nullptr)
    return nullptr;
  const int lx = floor_mod16(wx);
  const int lz = floor_mod16(wz);
  if (wx - source->x * 16 < 0 || wx - source->x * 16 >= 16 || wz - source->z * 16 < 0 ||
      wz - source->z * 16 >= 16 || wy < chunk_min_y(*source) || wy >= chunk_max_y(*source))
    return nullptr;
  const int sec = (wy - chunk_min_y(*source)) / 16;
  if (sec < 0 || sec >= static_cast<int>(source->sections.size()))
    return nullptr;
  const Section& s = source->sections[static_cast<size_t>(sec)];
  const int ly = (wy - (source->min_section_y + sec) * 16) / 4;
  const int cell = ((ly * 4 + (lz / 4)) * 4) + (lx / 4);
  if (!s.has_biomes || cell < 0 || cell >= kBiomeCellsPerSection ||
      s.biome_cells.size() != static_cast<size_t>(kBiomeCellsPerSection))
    return nullptr;
  const int32_t pal = s.biome_cells[static_cast<size_t>(cell)];
  if (pal < 0 || static_cast<size_t>(pal) >= s.biome_palette.size())
    return nullptr;
  return s.biome_palette[static_cast<size_t>(pal)].c_str();
}

// `BiomeFilter`'s question: does the biome at this position list `placed`?
bool biome_lists(const Chunk& chunk, int wx, int wy, int wz, const char* placed) {
  const char* biome = biome_at(chunk, wx, wy, wz);
  if (biome == nullptr)
    return false;
  const WgBiomeFeatures* row = find_biome(biome);
  if (row == nullptr)
    return false;
  for (uint32_t i = 0; i < row->count; ++i) {
    const WgBiomeFeatureEntry& entry = kWgBiomeFeatureEntries[row->offset + i];
    if (std::strcmp(entry.placed_feature, placed) == 0)
      return true;
  }
  return false;
}

// ---- block predicates ------------------------------------------------------

// Whether `state_name` would survive at (x, y, z). The pack's `would_survive`
// predicates carry only plants (saplings, cactus, sugar cane, firefly bush,
// mangrove propagule), so the question the engine has to answer is whether the
// block below is one they root in — the generator's own `kWgPlantSoil` list.
bool would_survive(const Chunk& chunk, std::string_view state_name, int x, int y, int z) {
  const BlockStateKey below = block_at(chunk, x, y - 1, z);
  // `kWgPlantSoil` is the overworld `#dirt`-like set the pack's sapling/flower
  // predicates resolve to. The Nether plants use nylium/soul soil instead; the
  // vegetation feature was rejecting every crimson root because the shared
  // list has no nylium and `would_survive` used to ignore the plant name.
  if (state_name == "minecraft:crimson_roots" || state_name == "minecraft:warped_roots" ||
      state_name == "minecraft:nether_sprouts" || state_name == "minecraft:crimson_fungus" ||
      state_name == "minecraft:warped_fungus") {
    return below.name == "minecraft:crimson_nylium" || below.name == "minecraft:warped_nylium" ||
           below.name == "minecraft:soul_soil";
  }
  // `BaseFireBlock.canSurvive`: a sturdy face below (any full block), which is
  // the case the Nether fire patches meet — their predicate already put
  // netherrack under the cell. `SoulFireBlock` narrows that to soul soil/sand.
  if (state_name == "minecraft:soul_fire")
    return below.name == "minecraft:soul_soil" || below.name == "minecraft:soul_sand";
  if (state_name == "minecraft:fire")
    return !is_replaceable_name(below.name);
  return name_in_list(kWgPlantSoil, below.name);
}

// `BlockPredicate.test` at one position. Every predicate carries its own
// `offset` (vanilla's `TestPredicate.test` shifts the position first, and a
// container shifts it for the children it recurses into), which is what makes a
// `matching_fluids` predicate ask about the cell beside the plant rather than
// the plant's own cell.
bool eval_predicate(const Chunk& chunk, uint32_t index, int wx, int wy, int wz) {
  if (index >= std::size(kWgPredicates))
    return true;
  const WgPredicate& p = kWgPredicates[index];
  const int x = wx + p.ox;
  const int y = wy + p.oy;
  const int z = wz + p.oz;
  switch (p.kind) {
  case 0: // always
    return true;
  case 1: // matching_blocks / matching_block_tag
  case 9: // matching_fluids
    return name_in_list(p.ref, block_at(chunk, x, y, z).name);
  case 2: // all_of
    for (uint32_t i = 0; i < p.b; ++i)
      if (!eval_predicate(chunk, kWgPredicateChildren[p.a + i], x, y, z))
        return false;
    return true;
  case 3: // any_of
    for (uint32_t i = 0; i < p.b; ++i)
      if (eval_predicate(chunk, kWgPredicateChildren[p.a + i], x, y, z))
        return true;
    return false;
  case 4: // not
    return !eval_predicate(chunk, p.a, x, y, z);
  case 5: { // would_survive
    const WgState& state = kWgPredicateStates[p.a];
    return would_survive(chunk, state.name, x, y, z);
  }
  case 6: // solid
    return !is_replaceable_name(block_at(chunk, x, y, z).name);
  case 7: // has_sturdy_face
    return !is_replaceable_name(block_at(chunk, x, y, z).name);
  case 8: // replaceable
    return is_replaceable_name(block_at(chunk, x, y, z).name);
  default:
    return true;
  }
}

bool predicate_matches(const WgOreTarget& target, std::string_view block_name) {
  if (target.predicate == WgPredicateKind::kAlways)
    return true;
  // `kBlocks` carries a `|`-joined name list (the generator resolves tags, so
  // this is a name membership test).
  return name_in_list(target.ref, block_name);
}

bool is_adjacent_to_air(const Chunk& chunk, int wx, int wy, int wz) {
  const int dx[6] = {1, -1, 0, 0, 0, 0};
  const int dy[6] = {0, 0, 1, -1, 0, 0};
  const int dz[6] = {0, 0, 0, 0, 1, -1};
  for (int i = 0; i < 6; ++i) {
    const int nx = wx + dx[i];
    const int ny = wy + dy[i];
    const int nz = wz + dz[i];
    // A neighbour outside the chunk is unknown; treat it as not-air so a
    // discard chance never fires on an unreadable cell.
    if (!in_chunk(chunk, nx, nz))
      continue;
    if (is_air_name(block_at(chunk, nx, ny, nz).name))
      return true;
  }
  return false;
}

// Defined below with the other heightmap helpers; `place_ore` needs the same
// `OCEAN_FLOOR_WG` question before it can choose its vein grid.
int ocean_floor_height(const Chunk& chunk, int lx, int lz);

bool should_skip_air_check(WgRandom& rng, float discard) {
  if (discard <= 0.0f)
    return true;
  if (discard >= 1.0f)
    return false;
  return rng.next_float() >= discard;
}

// `OreFeature.canPlaceOre` + the target pick, in vanilla's order.
const WgOreTarget* choose_ore_target(const Chunk& chunk, const WgOreConfig& cfg,
                                     const WgOreTarget* targets, WgRandom& rng, int x, int y,
                                     int z) {
  const BlockStateKey here = block_at(chunk, x, y, z);
  for (uint32_t i = 0; i < cfg.target_count; ++i) {
    const WgOreTarget& target = targets[cfg.target_offset + i];
    if (!predicate_matches(target, here.name))
      continue;
    if (!should_skip_air_check(rng, cfg.discard_on_air) && is_adjacent_to_air(chunk, x, y, z))
      continue;
    return &target;
  }
  return nullptr;
}

// `OreFeature.doPlace`, with the level replaced by the chunk and the worldgen
// context by the overworld's -64/384 bounds. Returns the number of blocks set.
uint32_t place_ore(Chunk& chunk, const WgOreConfig& cfg, const WgOreTarget* targets, WgRandom& rng,
                   int ox, int oy, int oz) {
  const int size = static_cast<int>(cfg.size);
  if (size <= 0)
    return 0;
  const float dir = rng.next_float() * kPiF;
  const float spread = static_cast<float>(size) / 8.0f;
  const int max_radius =
      static_cast<int>(std::ceil((static_cast<float>(size) / 16.0f * 2.0f + 1.0f) / 2.0f));
  const double sin_dir = std::sin(static_cast<double>(dir));
  const double cos_dir = std::cos(static_cast<double>(dir));
  const double x0 = ox + sin_dir * static_cast<double>(spread);
  const double x1 = ox - sin_dir * static_cast<double>(spread);
  const double z0 = oz + cos_dir * static_cast<double>(spread);
  const double z1 = oz - cos_dir * static_cast<double>(spread);
  const double y0 = oy + rng.next_int(3) - 2;
  const double y1 = oy + rng.next_int(3) - 2;
  const int y_start = oy - 2 - max_radius;
  int x_start = ox - static_cast<int>(std::ceil(spread)) - max_radius;
  int z_start = oz - static_cast<int>(std::ceil(spread)) - max_radius;
  const int size_xz = 2 * (static_cast<int>(std::ceil(spread)) + max_radius);
  const int size_y = 2 * (2 + max_radius);
  if (size_xz <= 0 || size_y <= 0)
    return 0;

  // `OreFeature.place` first looks for a column in the vein's square whose
  // `OCEAN_FLOOR_WG` height reaches the vein grid's bottom. That test only
  // gates the vein: `doPlace` is called with the *nominal* origin
  // (`ox - ceil(spread) - max_radius`), never with the column that was found.
  // The loops are inclusive in vanilla (`<= start + size`), so the gate covers
  // `size + 1` columns per axis. No draw is consumed by the scan, and a square
  // with no qualifying column returns false before the vein's data is built.
  // The scan is a region-path refinement: it reads the 3x3 chunks for a column
  // whose ocean floor reaches the vein grid. The single-chunk entry point has
  // no neighbours to look at, so keep it ungated; treating the absent region as
  // "no column found" made every ore return before its vein data was built.
  bool gate_passed = g_feature_region == nullptr;
  for (int gx = x_start; gx <= x_start + size_xz && !gate_passed; ++gx) {
    for (int gz = z_start; gz <= z_start + size_xz && !gate_passed; ++gz) {
      const Chunk* const column = region_chunk_at(gx, gz);
      if (column == nullptr)
        continue;
      if (y_start <= ocean_floor_height(*column, floor_mod16(gx), floor_mod16(gz)))
        gate_passed = true;
    }
  }
  if (!gate_passed)
    return 0;

  std::vector<double> data(static_cast<size_t>(size) * 4);
  for (int i = 0; i < size; ++i) {
    // `OreFeature.doPlace` keeps `step` as a float (`(float)i / (float)size`)
    // and evaluates `Mth.sin((double)(3.1415927f * step))`, which is a float
    // result. Widening the division to double changes the radius enough to move
    // a vein's outer cells, so the intermediate stays float here too.
    const float step = static_cast<float>(i) / static_cast<float>(size);
    const double xx = x0 + step * (x1 - x0);
    const double yy = y0 + step * (y1 - y0);
    const double zz = z0 + step * (z1 - z0);
    const double ss = rng.next_double() * static_cast<double>(size) / 16.0;
    const float sine = std::sin(static_cast<double>(kPiF * step));
    const double r = ((static_cast<double>(sine) + 1.0) * ss + 1.0) / 2.0;
    data[static_cast<size_t>(i) * 4 + 0] = xx;
    data[static_cast<size_t>(i) * 4 + 1] = yy;
    data[static_cast<size_t>(i) * 4 + 2] = zz;
    data[static_cast<size_t>(i) * 4 + 3] = r;
  }
  for (int i1 = 0; i1 < size - 1; ++i1) {
    if (data[static_cast<size_t>(i1) * 4 + 3] <= 0.0)
      continue;
    for (int i2 = i1 + 1; i2 < size; ++i2) {
      const double dr =
          data[static_cast<size_t>(i1) * 4 + 3] - data[static_cast<size_t>(i2) * 4 + 3];
      const double dx =
          data[static_cast<size_t>(i1) * 4 + 0] - data[static_cast<size_t>(i2) * 4 + 0];
      const double dy =
          data[static_cast<size_t>(i1) * 4 + 1] - data[static_cast<size_t>(i2) * 4 + 1];
      const double dz =
          data[static_cast<size_t>(i1) * 4 + 2] - data[static_cast<size_t>(i2) * 4 + 2];
      if (data[static_cast<size_t>(i2) * 4 + 3] <= 0.0 || !(dr * dr > dx * dx + dy * dy + dz * dz))
        continue;
      if (dr > 0.0) {
        data[static_cast<size_t>(i2) * 4 + 3] = -1.0;
      } else {
        data[static_cast<size_t>(i1) * 4 + 3] = -1.0;
      }
    }
  }

  const bool ore_trace =
      std::getenv("PLUSMC_ORE_TRACE") != nullptr &&
      std::strcmp(targets[cfg.target_offset].state.name, "minecraft:magma_block") == 0;
  const bool ore_trace_cells = ore_trace && std::getenv("PLUSMC_ORE_CELLS") != nullptr;
  int trace_cand = 0, trace_out = 0, trace_notarget = 0, trace_set = 0, trace_fail = 0;
  const size_t tested_size =
      static_cast<size_t>(size_xz) * static_cast<size_t>(size_y) * static_cast<size_t>(size_xz);
  std::vector<uint8_t> tested(tested_size, 0);
  uint32_t placed = 0;
  for (int i = 0; i < size; ++i) {
    const double r = data[static_cast<size_t>(i) * 4 + 3];
    if (r < 0.0)
      continue;
    const double xx = data[static_cast<size_t>(i) * 4 + 0];
    const double yy = data[static_cast<size_t>(i) * 4 + 1];
    const double zz = data[static_cast<size_t>(i) * 4 + 2];
    const int x_min = std::max(static_cast<int>(std::floor(xx - r)), x_start);
    const int y_min = std::max(static_cast<int>(std::floor(yy - r)), y_start);
    const int z_min = std::max(static_cast<int>(std::floor(zz - r)), z_start);
    const int x_max = std::max(static_cast<int>(std::floor(xx + r)), x_min);
    const int y_max = std::max(static_cast<int>(std::floor(yy + r)), y_min);
    const int z_max = std::max(static_cast<int>(std::floor(zz + r)), z_min);
    for (int x = x_min; x <= x_max; ++x) {
      const double xd = (static_cast<double>(x) + 0.5 - xx) / r;
      if (!(xd * xd < 1.0))
        continue;
      for (int y = y_min; y <= y_max; ++y) {
        const double yd = (static_cast<double>(y) + 0.5 - yy) / r;
        if (!(xd * xd + yd * yd < 1.0))
          continue;
        for (int z = z_min; z <= z_max; ++z) {
          const double zd = (static_cast<double>(z) + 0.5 - zz) / r;
          if (!(xd * xd + yd * yd + zd * zd < 1.0))
            continue;
          if (y < chunk_min_y(chunk) || y >= chunk_max_y(chunk))
            continue;
          const int idx =
              (x - x_start) + (y - y_start) * size_xz + (z - z_start) * size_xz * size_y;
          if (idx < 0 || static_cast<size_t>(idx) >= tested_size ||
              tested[static_cast<size_t>(idx)])
            continue;
          tested[static_cast<size_t>(idx)] = 1;
          if (ore_trace)
            ++trace_cand;
          if (!in_chunk(chunk, x, z)) {
            if (ore_trace)
              ++trace_out;
            continue;
          }
          const WgOreTarget* chosen = choose_ore_target(chunk, cfg, targets, rng, x, y, z);
          if (chosen == nullptr) {
            if (ore_trace)
              ++trace_notarget;
            continue;
          }
          if (try_set(chunk, x, y, z, chosen->state.name, chosen->state.properties,
                      /*only_replaceable=*/false)) {
            ++placed;
            if (ore_trace) {
              ++trace_set;
              if (ore_trace_cells)
                std::fprintf(stderr, "CELL %d %d %d %d %d %d\n", ox, oy, oz, x, y, z);
            }
          } else if (ore_trace) {
            ++trace_fail;
          }
          // Vanilla finishes the target loop here and continues the z-loop; the
          // old `break` left the z-loop entirely, so a vein placed at most one
          // block per (x, y) column.
        }
      }
    }
  }
  if (ore_trace)
    std::fprintf(stderr, "PLACE %d %d %d cand %d out %d notarget %d set %d fail %d placed %u\n", ox,
                 oy, oz, trace_cand, trace_out, trace_notarget, trace_set, trace_fail, placed);
  return placed;
}

// `ScatteredOreFeature.place` over the same `OreConfiguration` the ore kind
// reads: `size + 1` draws' worth of tries, each at a bounded triangular offset
// from the origin, and the first target that matches takes its state. `Math.round`
// is Java's `floor(x + 0.5f)`, not a half-away-from-zero round.
int scattered_offset(WgRandom& rng, int max_distance) {
  // Two draws, pulled into their own statements: they advance one shared
  // generator, so writing them inline leaves their order unspecified (`a - b`
  // would let the compiler give the first draw to `b` and flip the sign).
  // Vanilla evaluates the argument list left to right, so the first draw is the
  // minuend here too.
  const float first = rng.next_float();
  const float second = rng.next_float();
  const float delta = (first - second) * static_cast<float>(max_distance);
  return static_cast<int>(std::floor(delta + 0.5f));
}

uint32_t place_scattered_ore(Chunk& chunk, const WgOreConfig& cfg, const WgOreTarget* targets,
                             WgRandom& rng, int ox, int oy, int oz) {
  const int tries = rng.next_int(static_cast<int32_t>(cfg.size) + 1);
  uint32_t placed = 0;
  for (int i = 0; i < tries; ++i) {
    const int max_distance = std::min(i, 7); // MAX_DIST_FROM_ORIGIN
    const int x = ox + scattered_offset(rng, max_distance);
    const int y = oy + scattered_offset(rng, max_distance);
    const int z = oz + scattered_offset(rng, max_distance);
    if (!in_chunk(chunk, x, z) || y < kMinY || y >= kMinY + kHeight)
      continue;
    const WgOreTarget* chosen = choose_ore_target(chunk, cfg, targets, rng, x, y, z);
    if (chosen == nullptr)
      continue;
    // Vanilla's `continue block0`: the next try, not the next target.
    if (try_set(chunk, x, y, z, chosen->state.name, chosen->state.properties,
                /*only_replaceable=*/false))
      ++placed;
  }
  return placed;
}

// ---- the simpler feature kinds ---------------------------------------------

// `SimpleBlockFeature.place`: the state goes down when it can survive there.
// Vanilla's per-block `canSurvive` is reduced to the plant-soil question the
// pack's own `would_survive` predicates already pose (see `would_survive`).
uint32_t place_simple_block(Chunk& chunk, const WgState& state, int x, int y, int z) {
  if (!would_survive(chunk, state.name, x, y, z))
    return 0;
  return try_set(chunk, x, y, z, state.name, state.properties, /*only_replaceable=*/true) ? 1 : 0;
}

// `VinesFeature.place`: if the origin is air, try vanilla's own direction
// order (DOWN is skipped) and write the vine on the first neighbour with a
// full face toward the origin. The face test is `isAcceptableNeighbour` ->
// `MultifaceBlock.canAttachTo`, reduced to the same motion-blocking
// approximation the snow and seagrass support rules use.
uint32_t place_vines(Chunk& chunk, int x, int y, int z) {
  if (!is_air_name(block_at(chunk, x, y, z).name))
    return 0;
  struct Face {
    const char* property;
    int dx, dy, dz;
  };
  // Direction.values()'s own order: DOWN, UP, NORTH, SOUTH, WEST, EAST.
  static constexpr Face kFaces[] = {
      {"up", 0, 1, 0},    {"north", 0, 0, -1}, {"south", 0, 0, 1},
      {"west", -1, 0, 0}, {"east", 1, 0, 0},
  };
  for (const Face& face : kFaces) {
    if (!block_blocks_motion(block_at(chunk, x + face.dx, y + face.dy, z + face.dz).name))
      continue;
    std::string properties = face.property;
    properties += "=true";
    return try_set(chunk, x, y, z, "minecraft:vine", properties, /*only_replaceable=*/false) ? 1
                                                                                             : 0;
  }
  return 0;
}

// `BlockPileFeature.place`: the small isotropic pile, cell for cell.
uint32_t place_block_pile(Chunk& chunk, const WgState& state, WgRandom& rng, int ox, int oy,
                          int oz) {
  if (oy < kMinY + 5)
    return 0;
  const int xr = 2 + rng.next_int(2);
  const int zr = 2 + rng.next_int(2);
  uint32_t placed = 0;
  for (int x = ox - xr; x <= ox + xr; ++x) {
    for (int z = oz - zr; z <= oz + zr; ++z) {
      for (int y = oy; y <= oy + 1; ++y) {
        const int xd = ox - x;
        const int zd = oz - z;
        // Vanilla's `random.nextFloat() * 10.0F - random.nextFloat() * 6.0F`,
        // in that order; the two draws are sequenced on purpose.
        const float roll_scaled_10 = rng.next_float();
        const float roll_scaled_6 = rng.next_float();
        const float roll = roll_scaled_10 * 10.0f - roll_scaled_6 * 6.0f;
        const bool inside = static_cast<float>(xd * xd + zd * zd) <= roll;
        if (!inside && !(rng.next_float() < 0.031f))
          continue;
        const BlockStateKey below = block_at(chunk, x, y - 1, z);
        const bool may_place = below.name == "minecraft:dirt_path"
                                   ? rng.next_boolean()
                                   : !is_replaceable_name(below.name);
        if (!is_air_name(block_at(chunk, x, y, z).name) || !may_place)
          continue;
        if (try_set(chunk, x, y, z, state.name, state.properties, /*only_replaceable=*/true))
          ++placed;
      }
    }
  }
  return placed;
}

// `DiskFeature.place`: a horizontal disc whose per-column top/bottom run is
// bounded by `half_height`. `column_z` is the innermost loop so the same random
// draw order as vanilla is kept.
uint32_t place_disk(Chunk& chunk, const WgDiskConfig& cfg, WgRandom& rng, int ox, int oy, int oz) {
  const int radius = random_between_inclusive(rng, cfg.radius_min, cfg.radius_max);
  const int top = oy + cfg.half_height;
  const int bottom = oy - cfg.half_height - 1;
  uint32_t placed = 0;
  for (int x = ox - radius; x <= ox + radius; ++x) {
    const int xd = x - ox;
    for (int z = oz - radius; z <= oz + radius; ++z) {
      const int zd = z - oz;
      if (xd * xd + zd * zd > radius * radius)
        continue;
      // Vanilla's `placedColumn` keeps a `placedAbove` flag in this scan, but its
      // only reader is `markAboveForPostProcessing`, which this stage has no
      // counterpart for (`place_fallen_tree`'s note below says why); the scan
      // writes and nothing here reads back.
      for (int y = top; y > bottom; --y) {
        const BlockStateKey here = block_at(chunk, x, y, z);
        if (!name_in_list(cfg.target, here.name))
          continue;
        if (try_set(chunk, x, y, z, cfg.state.name, cfg.state.properties,
                    /*only_replaceable=*/false))
          ++placed;
      }
    }
  }
  return placed;
}

// `SpringFeature.place`: the rock/hole neighbour census, then the fluid.
uint32_t place_spring(Chunk& chunk, const WgSpringConfig& cfg, int x, int y, int z) {
  if (!name_in_list(cfg.valid_blocks, block_at(chunk, x, y + 1, z).name))
    return 0;
  if (cfg.requires_block_below &&
      !name_in_list(cfg.valid_blocks, block_at(chunk, x, y - 1, z).name))
    return 0;
  const BlockStateKey current = block_at(chunk, x, y, z);
  if (!is_air_name(current.name) && !name_in_list(cfg.valid_blocks, current.name))
    return 0;
  const int nx[5] = {x - 1, x + 1, x, x, x};
  const int nz[5] = {z, z, z - 1, z + 1, z};
  const int ny[5] = {y, y, y, y, y - 1};
  int rocks = 0;
  int holes = 0;
  for (int i = 0; i < 5; ++i) {
    const BlockStateKey n = block_at(chunk, nx[i], ny[i], nz[i]);
    if (name_in_list(cfg.valid_blocks, n.name))
      ++rocks;
    if (is_air_name(n.name))
      ++holes;
  }
  if (rocks != cfg.rock_count || holes != cfg.hole_count)
    return 0;
  return try_set(chunk, x, y, z, cfg.state.name, cfg.state.properties,
                 /*only_replaceable=*/false)
             ? 1
             : 0;
}

// ---- monster rooms ---------------------------------------------------------

// `Feature.isReplaceable(BlockTags.FEATURES_CANNOT_REPLACE)`: the blocks a
// feature may not write over, taken from the pack's own tag file.
bool is_cannot_replace_name(std::string_view name) {
  static constexpr const char* kBlocks[] = {
      "minecraft:bedrock",
      "minecraft:spawner",
      "minecraft:chest",
      "minecraft:end_portal_frame",
      "minecraft:reinforced_deepslate",
      "minecraft:trial_spawner",
      "minecraft:vault",
  };
  for (const char* block : kBlocks)
    if (name == block)
      return true;
  return false;
}

// `MonsterRoomFeature.place`: the sanity scan (a solid floor and ceiling, and
// one to five holes — cave-air cells on a wall with air above them), then the
// room's own carve. The shell becomes cobblestone (mossy for three of the four
// floor rows), the interior cave air, up to two chests are put against a single
// solid wall, and a spawner goes at the origin. Writes go through
// `safeSetBlock`'s guard, `Feature.isReplaceable(#features_cannot_replace)`.
//
// One approximation, named rather than hidden: the room is confined to the
// chunk it decorates — a cell outside reads as air, so a room whose floor or
// ceiling straddles the boundary fails its own sanity scan, exactly as the
// header says of the stage. The chest carries `simple_dungeon` loot and the
// spawner a `SpawnData.entity.id`; its mob is position-seeded rather than
// vanilla's last `Util.getRandom(MOBS)` draw, so the shared feature stream (and
// the decorated-cell golden) does not move.
uint32_t place_monster_room(Chunk& chunk, WgRandom& rng, int64_t world_seed, int ox, int oy,
                            int oz) {
  const int xr = rng.next_int(2) + 2;
  const int zr = rng.next_int(2) + 2;
  const int min_x = -xr - 1;
  const int max_x = xr + 1;
  const int min_z = -zr - 1;
  const int max_z = zr + 1;

  int holes = 0;
  for (int dx = min_x; dx <= max_x; ++dx) {
    for (int dy = -1; dy <= 4; ++dy) {
      for (int dz = min_z; dz <= max_z; ++dz) {
        const bool solid = !is_replaceable_name(block_at(chunk, ox + dx, oy + dy, oz + dz).name);
        if (dy == -1 && !solid)
          return 0;
        if (dy == 4 && !solid)
          return 0;
        const bool on_wall = dx == min_x || dx == max_x || dz == min_z || dz == max_z;
        if (on_wall && dy == 0 && is_air_name(block_at(chunk, ox + dx, oy, oz + dz).name) &&
            is_air_name(block_at(chunk, ox + dx, oy + 1, oz + dz).name))
          ++holes;
      }
    }
  }
  if (holes < 1 || holes > 5)
    return 0;

  // `safeSetBlock`: write only where the cell is not one the tag protects.
  auto safe_set = [&](int x, int y, int z, std::string_view name) -> uint32_t {
    if (is_cannot_replace_name(block_at(chunk, x, y, z).name))
      return 0;
    return try_set(chunk, x, y, z, name, "", /*only_replaceable=*/false) ? 1 : 0;
  };

  uint32_t placed = 0;
  for (int dx = min_x; dx <= max_x; ++dx) {
    for (int dy = 3; dy >= -1; --dy) {
      for (int dz = min_z; dz <= max_z; ++dz) {
        const int x = ox + dx;
        const int y = oy + dy;
        const int z = oz + dz;
        const BlockStateKey here = block_at(chunk, x, y, z);
        const bool shell =
            dx == min_x || dy == -1 || dz == min_z || dx == max_x || dy == 4 || dz == max_z;
        if (shell) {
          // The room's own floor is dropped back to air where nothing holds it.
          if (y >= kMinY && is_replaceable_name(block_at(chunk, x, y - 1, z).name)) {
            placed += safe_set(x, y, z, "minecraft:cave_air");
            continue;
          }
          if (is_replaceable_name(here.name) || here.name == "minecraft:chest")
            continue;
          if (dy == -1 && rng.next_int(4) != 0) {
            placed += safe_set(x, y, z, "minecraft:mossy_cobblestone");
            continue;
          }
          placed += safe_set(x, y, z, "minecraft:cobblestone");
          continue;
        }
        if (here.name == "minecraft:chest" || here.name == "minecraft:spawner")
          continue;
        placed += safe_set(x, y, z, "minecraft:cave_air");
      }
    }
  }

  // Up to two chests, one per outer pass, each against exactly one solid wall
  // (vanilla's labelled `continue block6` skips the rest of the inner pass).
  for (int cc = 0; cc < 2; ++cc) {
    for (int i = 0; i < 3; ++i) {
      const int x = ox + rng.next_int(xr * 2 + 1) - xr;
      const int z = oz + rng.next_int(zr * 2 + 1) - zr;
      if (!is_air_name(block_at(chunk, x, oy, z).name))
        continue;
      int walls = 0;
      walls += !is_replaceable_name(block_at(chunk, x - 1, oy, z).name) ? 1 : 0;
      walls += !is_replaceable_name(block_at(chunk, x + 1, oy, z).name) ? 1 : 0;
      walls += !is_replaceable_name(block_at(chunk, x, oy, z - 1).name) ? 1 : 0;
      walls += !is_replaceable_name(block_at(chunk, x, oy, z + 1).name) ? 1 : 0;
      if (walls != 1)
        continue;
      const uint32_t wrote = safe_set(x, oy, z, "minecraft:chest");
      placed += wrote;
      if (wrote != 0) {
        const int32_t world_x = chunk.x * 16 + x;
        const int32_t world_z = chunk.z * 16 + z;
        const int64_t seed = wg_structure_loot_seed(world_seed, world_x, oy, world_z,
                                                    "minecraft:chests/simple_dungeon");
        chunk.block_entities.push_back(make_container_block_entity(
            "minecraft:chest", BlockPosKey{world_x, oy, world_z},
            wg_structure_loot_items("minecraft:chests/simple_dungeon", seed)));
      }
      break;
    }
  }

  placed += safe_set(ox, oy, oz, "minecraft:spawner");
  if (block_at(chunk, ox, oy, oz).name == "minecraft:spawner") {
    // `MonsterRoomFeature.randomEntityId`: MOBS is skeleton, zombie, zombie,
    // spider. The real draw is a `Util.getRandom` at the end of the feature;
    // using a position seed keeps that draw out of the shared feature stream,
    // which is what lets this addition leave every golden cell unchanged.
    static constexpr const char* kMobs[4] = {"minecraft:skeleton", "minecraft:zombie",
                                             "minecraft:zombie", "minecraft:spider"};
    const int32_t world_x = chunk.x * 16 + ox;
    const int32_t world_z = chunk.z * 16 + oz;
    const int64_t seed =
        wg_structure_loot_seed(world_seed, world_x, oy, world_z, "minecraft:spawner/monster_room");
    const uint32_t pick = static_cast<uint32_t>(seed) & 3u;
    using namespace nbt;
    nbt::Value entity =
        make_compound(std::vector<std::pair<std::string, Value>>{{"id", make_string(kMobs[pick])}});
    nbt::Value spawn_data =
        make_compound(std::vector<std::pair<std::string, Value>>{{"entity", std::move(entity)}});
    nbt::Value be = make_compound(
        std::vector<std::pair<std::string, Value>>{{"id", make_string("minecraft:spawner")},
                                                   {"x", make_int(world_x)},
                                                   {"y", make_int(oy)},
                                                   {"z", make_int(world_z)},
                                                   {"SpawnData", std::move(spawn_data)}});
    chunk.block_entities.push_back(std::move(be));
  }
  return placed;
}

// `SnowLayerBlock.canSurvive` at the cell a snow layer would sit on. The pack's
// `#minecraft:cannot_support_snow_layer` refuses it (ice, packed ice, barrier)
// and `#minecraft:support_override_snow_layer` accepts it (honey, soul sand,
// mud); otherwise the block must present a full collision face upward. That
// last term is code, not data, so it is approximated by the same
// motion-blocking predicate the heightmap already uses — the surface the snow
// lands on is a motion-blocking block by construction.
bool can_support_snow_layer(const BlockStateKey& below) {
  static constexpr const char* kCannotSupport[] = {"minecraft:ice", "minecraft:packed_ice",
                                                   "minecraft:barrier"};
  for (const char* name : kCannotSupport)
    if (below.name == name)
      return false;
  static constexpr const char* kOverride[] = {"minecraft:honey_block", "minecraft:soul_sand",
                                              "minecraft:mud"};
  for (const char* name : kOverride)
    if (below.name == name)
      return true;
  return block_blocks_motion(below.name);
}

// `SnowAndFreezeFeature.place`: the whole 16x16 chunk, column by column. The
// origin is the chunk corner (the placement is the bare `biome` modifier) and
// its y is irrelevant — each column's own first free cell decides where the
// snow goes. Vanilla fetches the biome at the column's top and asks that same
// biome about both cells.
//
// `Biome.shouldSnow`/`shouldFreeze` also require `getBrightness(BLOCK, pos) <
// 10`. At generation time no block-light source exists yet, so that reading is
// 0 at every candidate and the gate is satisfied by construction; the runtime
// records the omission in docs/PHASE4.md. Temperature comes from the biome's
// base value via `biome_precipitation_at` (the weather path's own function), so
// `shouldSnow`'s SNOW answer is exact for the precipitation-bearing overworld
// biomes and `shouldFreeze` reuses it: vanilla's `shouldFreeze` tests
// temperature alone, but for a biome that precipitates the two questions have
// the same answer, and the no-precipitation biomes (nether, void) never carry
// water here.
uint32_t place_freeze_top_layer(Chunk& chunk, int ox, int oz) {
  uint32_t placed = 0;
  for (int dx = 0; dx < 16; ++dx) {
    for (int dz = 0; dz < 16; ++dz) {
      const int top = chunk_motion_blocking_height(chunk, dx, dz);
      const int below = top - 1;
      if (top <= kMinY || top >= kMinY + kHeight)
        continue;
      const char* biome = biome_at(chunk, ox + dx, top, oz + dz);
      if (biome == nullptr)
        continue;
      // `biome.shouldFreeze(level, belowPos, false)`: cold enough, the cell is
      // a water source, and no neighbour check.
      if (biome_precipitation_at(biome, below, kOverworldSeaLevel) == Precipitation::kSnow &&
          block_at(chunk, ox + dx, below, oz + dz).name == "minecraft:water") {
        if (try_set(chunk, ox + dx, below, oz + dz, "minecraft:ice", "",
                    /*only_replaceable=*/false))
          ++placed;
      }
      if (biome_precipitation_at(biome, top, kOverworldSeaLevel) != Precipitation::kSnow)
        continue;
      // `biome.shouldSnow(level, topPos)`: an air or snow cell that snow can
      // survive on.
      const BlockStateKey here = block_at(chunk, ox + dx, top, oz + dz);
      if (!is_air_name(here.name) && here.name != "minecraft:snow")
        continue;
      const BlockStateKey support = block_at(chunk, ox + dx, below, oz + dz);
      if (!can_support_snow_layer(support))
        continue;
      if (try_set(chunk, ox + dx, top, oz + dz, "minecraft:snow", "layers=1",
                  /*only_replaceable=*/false))
        ++placed;
      // `SnowyBlock.SNOWY` on the block under the layer.
      std::string_view props = support.properties;
      if (props.find("snowy=") != std::string_view::npos) {
        if (try_set(chunk, ox + dx, below, oz + dz, support.name,
                    with_property(props, "snowy", "true"), /*only_replaceable=*/false))
          ++placed;
      }
    }
  }
  return placed;
}

// ---- seagrass --------------------------------------------------------------

// `SeagrassBlock.mayPlaceOn` / `TallSeagrassBlock.mayPlaceOn`: the support's
// top face is sturdy and the block is not `#minecraft:cannot_support_seagrass`
// (whose only entry is magma block). The face term is the same motion-blocking
// approximation the snow layer's support uses (worldgen_feature.h names it).
bool can_support_seagrass(const BlockStateKey& below) {
  if (below.name == "minecraft:magma_block")
    return false;
  return block_blocks_motion(below.name);
}

// A full water cell: vanilla's `FluidState.isFull()`. The chunk stores a source
// as `minecraft:water[level=0]` and a flow with a nonzero `level`, so the
// property is the whole question here.
bool is_full_water(const BlockStateKey& k) {
  return k.name == "minecraft:water" && k.properties == "level=0";
}

// `level.getHeight(Heightmap.Types.OCEAN_FLOOR, x, z)` over a column of the
// chunk: the first free cell above the highest block that blocks motion
// (vanilla's `MATERIAL_MOTION_BLOCKING`, which ignores fluids — water is not
// motion-blocking, so a water column answers one cell above its bed). A
// generated chunk carries no `OCEAN_FLOOR` array (chunk.cpp leaves an absent
// map absent, and `Chunk::height_at` answers the dimension floor for one), so
// there is no heightmap to read here; the predicate is the collision table's,
// the same one the maps that do exist are computed from.
int ocean_floor_height(const Chunk& chunk, int lx, int lz) {
  const int min_y = chunk_min_y(chunk);
  for (int y = chunk_max_y(chunk) - 1; y >= min_y; --y)
    if (block_blocks_motion(chunk.block_state_at(lx, y, lz).name))
      return y + 1;
  return min_y;
}

// `SeagrassFeature.place`: one candidate cell per application, chosen from the
// origin's own 8x8 neighbourhood per horizontal axis (`nextInt(8) -
// nextInt(8)`, so the cell can fall outside the chunk; this stage drops that,
// as every kind here does, rather than placing into the neighbour), at the
// column's `OCEAN_FLOOR` height. If that cell is water, one draw picks
// `tall_seagrass` (both halves, the upper one only if the cell above is water
// too) over `seagrass`; either way the support below must be a sturdy block.
// The draws are vanilla's in vanilla's order, including the one the water check
// gates: the stream is this port's own, but it is consumed the same way.
uint32_t place_seagrass(Chunk& chunk, const WgSeagrassConfig& cfg, WgRandom& rng, int ox, int oy,
                        int oz) {
  (void)oy; // the placement's heightmap only moves the origin; the feature recomputes it
  // `random.nextInt(8) - random.nextInt(8)` per axis, in vanilla's
  // left-to-right order: the added draw first, the subtracted one second.
  const int offset_x_added = rng.next_int(8);
  const int offset_x_subtracted = rng.next_int(8);
  const int x = ox + offset_x_added - offset_x_subtracted;
  const int offset_z_added = rng.next_int(8);
  const int offset_z_subtracted = rng.next_int(8);
  const int z = oz + offset_z_added - offset_z_subtracted;
  if (!in_chunk(chunk, x, z))
    return 0;
  const int y = ocean_floor_height(chunk, x - chunk.x * 16, z - chunk.z * 16);
  const BlockStateKey here = block_at(chunk, x, y, z);
  if (here.name != "minecraft:water")
    return 0;
  const bool is_tall = rng.next_double() < static_cast<double>(cfg.probability);
  if (!can_support_seagrass(block_at(chunk, x, y - 1, z)))
    return 0;
  if (!is_tall)
    return try_set(chunk, x, y, z, "minecraft:seagrass", "", /*only_replaceable=*/false) ? 1 : 0;
  // `TallSeagrassBlock.canSurvive(LOWER)` adds `fluidState.isFull()` at the
  // cell itself, which a flowing cell fails. Its return is vanilla's boolean,
  // which is true even when the upper half's water check refuses; this stage
  // reports the cells it wrote instead, so a refused upper half is a 0 here
  // (docs/PHASE4.md says so).
  if (!is_full_water(here))
    return 0;
  uint32_t placed = 0;
  if (block_at(chunk, x, y + 1, z).name == "minecraft:water") {
    if (try_set(chunk, x, y, z, "minecraft:tall_seagrass", "half=lower",
                /*only_replaceable=*/false))
      ++placed;
    if (try_set(chunk, x, y + 1, z, "minecraft:tall_seagrass", "half=upper",
                /*only_replaceable=*/false))
      ++placed;
  }
  return placed;
}

// ---- multiface growth ------------------------------------------------------

// `Direction.values()`'s own order (down, up, north, south, west, east), which
// is the order every shuffle below draws from, with each face's axis so the
// spreader's `spreadDirection.getAxis() == startingFace.getAxis()` test can be
// made (`Direction.Axis`: X 0, Y 1, Z 2).
struct WgFace {
  const char* name;
  int dx, dy, dz;
  uint8_t axis;
};
constexpr WgFace kGrowthFaces[6] = {
    {"down", 0, -1, 0, 1}, {"up", 0, 1, 0, 1},    {"north", 0, 0, -1, 2},
    {"south", 0, 0, 1, 2}, {"west", -1, 0, 0, 0}, {"east", 1, 0, 0, 0},
};
constexpr int kGrowthFaceCount = 6;

// The opposite face: the ordinals pair down/up, north/south, west/east.
int opposite_face(int face) {
  return face ^ 1;
}

// `Util.shuffle`: the Fisher-Yates loop `for (i = size; i > 1; --i)` drawing
// `nextInt(i)`, i.e. the same draws vanilla's `Direction.allShuffled` and
// `getShuffledDirections` consume.
void shuffle_faces(WgRandom& rng, int* faces, int count) {
  for (int i = count; i > 1; --i) {
    const int swap_to = rng.next_int(i);
    const int held = faces[i - 1];
    faces[i - 1] = faces[swap_to];
    faces[swap_to] = held;
  }
}

// `MultifaceBlock.hasFace`: the state's own `direction=true` property.
bool has_growth_face(const BlockStateKey& state, int face) {
  return block_property(state.properties, kGrowthFaces[face].name) == "true";
}

// `MultifaceGrowthFeature.isAirOrWater`: `state.isAir() || state.is(Blocks.WATER)`.
bool is_air_or_water(const BlockStateKey& state) {
  return is_air_name(state.name) || state.name == "minecraft:water";
}

// `MultifaceBlock.canAttachTo`: is the face the growth would sit on a full one?
// Vanilla tests the neighbour's block-support shape and then its collision
// shape; this chunk model has no per-face shapes, so a motion-blocking
// neighbour is the same approximation `place_vines` and the snow support rule
// make (docs/PHASE4.md names it).
bool growth_can_attach(const Chunk& chunk, int x, int y, int z, int face) {
  const WgFace& f = kGrowthFaces[face];
  return block_blocks_motion(block_at(chunk, x + f.dx, y + f.dy, z + f.dz).name);
}

// `MultifaceBlock.isValidStateForPlacement`: the face is supported by this
// block (every face of a multiface block is) and the state does not already
// carry it, and the neighbour can attach it.
bool growth_is_valid_placement(const Chunk& chunk, const WgMultifaceGrowthConfig& cfg,
                               const BlockStateKey& old, int x, int y, int z, int face) {
  if (old.name == cfg.state.name && has_growth_face(old, face))
    return false;
  return growth_can_attach(chunk, x, y, z, face);
}

// `MultifaceBlock.getStateForPlacement(oldState, level, pos, direction)`: keep
// the old state when it is already the growth block, otherwise start from the
// block's vanilla default state (`glow_lichen` starts with every face false and
// `waterlogged=false`), waterlogged when the cell held source water, and set
// the one face true.
BlockStateKey growth_state(const WgMultifaceGrowthConfig& cfg, const BlockStateKey& old, int face) {
  const std::string name(cfg.state.name);
  std::string props;
  if (old.name == name) {
    props = old.properties;
  } else {
    const auto def = default_block_state(name);
    props = def ? def->key.properties : std::string(cfg.state.properties);
    if (is_full_water(old))
      props = with_property(props, "waterlogged", "true");
  }
  return BlockStateKey{name, with_property(props, kGrowthFaces[face].name, "true")};
}

// `MultifaceSpreader.spreadFromFaceTowardRandomDirection` over the default
// spreader: a shuffled direction walk, skipping the starting face's own axis,
// then the three `SpreadType`s in their declared order (SAME_POSITION,
// SAME_PLANE, WRAP_AROUND). The first candidate the spreader can place into is
// written, and the spread stops whether or not the write landed (the candidate
// is chosen before the block is set).
uint32_t spread_growth(Chunk& chunk, const WgMultifaceGrowthConfig& cfg, WgRandom& rng, int x,
                       int y, int z, const BlockStateKey& state, int from_face) {
  int order[kGrowthFaceCount] = {0, 1, 2, 3, 4, 5};
  shuffle_faces(rng, order, kGrowthFaceCount);
  for (int i = 0; i < kGrowthFaceCount; ++i) {
    const int spread = order[i];
    if (kGrowthFaces[spread].axis == kGrowthFaces[from_face].axis)
      continue;
    if (!has_growth_face(state, from_face) || has_growth_face(state, spread))
      continue;
    struct Candidate {
      int x, y, z, face;
    };
    const WgFace& sd = kGrowthFaces[spread];
    const WgFace& ff = kGrowthFaces[from_face];
    const Candidate candidates[3] = {
        {x, y, z, spread},
        {x + sd.dx, y + sd.dy, z + sd.dz, from_face},
        {x + sd.dx + ff.dx, y + sd.dy + ff.dy, z + sd.dz + ff.dz, opposite_face(spread)},
    };
    for (const Candidate& c : candidates) {
      // A candidate outside the column is not placeable; this stage confines a
      // feature to the chunk it decorates (the header names that).
      if (!in_chunk(chunk, c.x, c.z))
        continue;
      const BlockStateKey old = block_at(chunk, c.x, c.y, c.z);
      // `DefaultSpreaderConfig.stateCanBeReplaced`: air, the growth block, or a
      // *source* water cell.
      if (!is_air_name(old.name) && old.name != cfg.state.name && !is_full_water(old))
        continue;
      if (!growth_is_valid_placement(chunk, cfg, old, c.x, c.y, c.z, c.face))
        continue;
      const BlockStateKey next = growth_state(cfg, old, c.face);
      return try_set(chunk, c.x, c.y, c.z, next.name, next.properties,
                     /*only_replaceable=*/false)
                 ? 1u
                 : 0u;
    }
  }
  return 0;
}

// `MultifaceGrowthFeature.placeGrowthIfPossible`: the first direction whose
// neighbour is in `can_be_placed_on` is *the* attempt — a placement state that
// is not valid there ends the whole attempt rather than moving on, which is
// vanilla's `return false` inside the loop.
bool place_growth_if_possible(Chunk& chunk, const WgMultifaceGrowthConfig& cfg,
                              const int* directions, uint32_t count, int x, int y, int z,
                              WgRandom& rng, uint32_t& placed) {
  const BlockStateKey old = block_at(chunk, x, y, z);
  for (uint32_t i = 0; i < count; ++i) {
    const int face = directions[i];
    const WgFace& f = kGrowthFaces[face];
    if (!name_in_list(cfg.can_be_placed_on, block_at(chunk, x + f.dx, y + f.dy, z + f.dz).name))
      continue;
    if (!growth_is_valid_placement(chunk, cfg, old, x, y, z, face))
      return false;
    const BlockStateKey next = growth_state(cfg, old, face);
    if (try_set(chunk, x, y, z, next.name, next.properties, /*only_replaceable=*/false))
      ++placed;
    if (rng.next_float() < cfg.chance_of_spreading)
      placed += spread_growth(chunk, cfg, rng, x, y, z, next, face);
    return true;
  }
  return false;
}

// `MultifaceGrowthFeature.place`: the origin first, then one cell out along each
// valid direction, `search_range` times. Vanilla's loop re-sets the same
// neighbouring cell on every iteration (there is no `move` after the attempt),
// so the same cell is tried `search_range` times; that is reproduced as written
// because its draws are this port's stream.
uint32_t place_multiface_growth(Chunk& chunk, const WgMultifaceGrowthConfig& cfg, WgRandom& rng,
                                int ox, int oy, int oz) {
  if (!is_air_or_water(block_at(chunk, ox, oy, oz)))
    return 0;
  // `MultifaceGrowthConfiguration.validDirections`' build order: UP for the
  // ceiling, DOWN for the floor, then `Plane.HORIZONTAL` (NORTH, EAST, SOUTH,
  // WEST).
  int valid[kGrowthFaceCount];
  uint32_t valid_count = 0;
  if (cfg.can_place_on_ceiling)
    valid[valid_count++] = 1;
  if (cfg.can_place_on_floor)
    valid[valid_count++] = 0;
  if (cfg.can_place_on_wall) {
    valid[valid_count++] = 2;
    valid[valid_count++] = 5;
    valid[valid_count++] = 3;
    valid[valid_count++] = 4;
  }
  if (valid_count == 0)
    return 0;
  shuffle_faces(rng, valid, static_cast<int>(valid_count));
  uint32_t placed = 0;
  if (place_growth_if_possible(chunk, cfg, valid, valid_count, ox, oy, oz, rng, placed))
    return placed;
  for (uint32_t si = 0; si < valid_count; ++si) {
    const int search = valid[si];
    int placement[kGrowthFaceCount];
    uint32_t placement_count = 0;
    for (uint32_t k = 0; k < valid_count; ++k)
      if (valid[k] != opposite_face(search))
        placement[placement_count++] = valid[k];
    shuffle_faces(rng, placement, static_cast<int>(placement_count));
    const WgFace& s = kGrowthFaces[search];
    for (int i = 0; i < static_cast<int>(cfg.search_range); ++i) {
      const int px = ox + s.dx;
      const int py = oy + s.dy;
      const int pz = oz + s.dz;
      const BlockStateKey here = block_at(chunk, px, py, pz);
      if (!is_air_or_water(here) && here.name != cfg.state.name)
        break;
      if (place_growth_if_possible(chunk, cfg, placement, placement_count, px, py, pz, rng, placed))
        return placed;
    }
  }
  return placed;
}

// ---- block columns ---------------------------------------------------------

// The most layers one column can name; the pack's own widest column is two.
constexpr uint32_t kMaxColumnLayers = 8;

// `WeightedList.getRandomOrThrow` over the weighted *integer* entries: one
// bounded draw, then subtract each weight. Returns the index of the chosen
// entry in `kWgWeightedInts`.
uint32_t weighted_int_index(WgRandom& rng, uint32_t offset, uint32_t count) {
  if (count == 0)
    return offset;
  int32_t total = 0;
  for (uint32_t i = 0; i < count; ++i)
    total += kWgWeightedInts[offset + i].weight;
  if (total <= 0)
    return offset;
  int32_t roll = rng.next_int(total);
  for (uint32_t i = 0; i < count; ++i) {
    roll -= kWgWeightedInts[offset + i].weight;
    if (roll < 0)
      return offset + i;
  }
  return offset + count - 1;
}

// `IntProvider.sample`, the four kinds a block column's heights use. A
// `biased_to_bottom` is vanilla's own two-draw form,
// `min + nextInt(nextInt(max - min + 1) + 1)`, and a weighted list recurses
// into the provider it picks.
int sample_int_provider(WgRandom& rng, uint32_t index) {
  if (index >= std::size(kWgIntProviders))
    return 0;
  const WgIntProvider& p = kWgIntProviders[index];
  switch (p.kind) {
  case WgIntProviderKind::kConstant:
    return p.lo;
  case WgIntProviderKind::kUniform:
    return random_between_inclusive(rng, p.lo, p.hi);
  case WgIntProviderKind::kBiasedToBottom:
    if (p.hi <= p.lo)
      return p.lo;
    return p.lo + rng.next_int(rng.next_int(p.hi - p.lo + 1) + 1);
  case WgIntProviderKind::kWeightedList:
    return sample_int_provider(
        rng, kWgWeightedInts[weighted_int_index(rng, p.offset, p.count)].provider);
  }
  return p.lo;
}

// `WeightedList.getRandomOrThrow` over the weighted states.
const WgWeightedState& weighted_state(WgRandom& rng, uint32_t offset, uint32_t count) {
  if (count == 0)
    return kWgWeightedStates[offset];
  int32_t total = 0;
  for (uint32_t i = 0; i < count; ++i)
    total += kWgWeightedStates[offset + i].weight;
  if (total <= 0)
    return kWgWeightedStates[offset];
  int32_t roll = rng.next_int(total);
  for (uint32_t i = 0; i < count; ++i) {
    roll -= kWgWeightedStates[offset + i].weight;
    if (roll < 0)
      return kWgWeightedStates[offset + i];
  }
  return kWgWeightedStates[offset + count - 1];
}

// `StateHolder.setValue` for one integer property of a canonical "k=v,k=v"
// string: the key's own value is replaced, and a state without that property is
// left alone (vanilla's `findProperty == null` branch).
void set_int_property(std::string& properties, const char* property, int value) {
  const size_t len = std::strlen(property);
  size_t start = 0;
  while (start <= properties.size()) {
    const size_t comma = properties.find(',', start);
    const size_t end = comma == std::string::npos ? properties.size() : comma;
    if (end - start > len && properties.compare(start, len, property) == 0 &&
        properties[start + len] == '=') {
      properties.replace(start + len + 1, end - start - len - 1, std::to_string(value));
      return;
    }
    if (comma == std::string::npos)
      return;
    start = comma + 1;
  }
}

// `BlockStateProvider.getState` for a column layer: a simple state as it is, a
// weighted pick of its entries, and a `randomized_int_state_provider` as its
// source's state with one integer property drawn from the `values` provider.
BlockStateKey sample_state_provider(WgRandom& rng, uint32_t index) {
  if (index >= std::size(kWgStateProviders))
    return BlockStateKey{"minecraft:air", ""};
  const WgStateProvider& p = kWgStateProviders[index];
  if (p.kind == WgStateProviderKind::kSimple)
    return BlockStateKey{p.state.name, p.state.properties};
  if (p.kind == WgStateProviderKind::kWeighted) {
    const WgWeightedState& s = weighted_state(rng, p.offset, p.count);
    return BlockStateKey{s.state.name, s.state.properties};
  }
  BlockStateKey base = sample_state_provider(rng, p.source);
  set_int_property(base.properties, p.property, sample_int_provider(rng, p.values));
  return base;
}

// `BlockColumnFeature.place`: sample every layer's height first, walk the column
// to find how much of it the placement predicate allows (vanilla tests the cell
// *after* the origin on each pass, so a failure at index k leaves a column k
// blocks tall), truncate the layers from the tip — or from the base when
// `prioritize_tip` — and then write each layer's state in order along the
// direction. The `direction` ordinal is `Direction`'s own (down, up, north,
// south, west, east).
uint32_t place_block_column(Chunk& chunk, const WgColumnConfig& cfg, WgRandom& rng, int ox, int oy,
                            int oz) {
  if (cfg.layer_count == 0 || cfg.layer_count > kMaxColumnLayers)
    return 0;
  int heights[kMaxColumnLayers];
  int total = 0;
  for (uint32_t i = 0; i < cfg.layer_count; ++i) {
    const int height = sample_int_provider(rng, kWgColumnLayers[cfg.layer_offset + i].height);
    heights[i] = std::max(height, 0);
    total += heights[i];
  }
  if (total == 0)
    return 0;

  static constexpr int kDx[6] = {0, 0, 0, 0, -1, 1};
  static constexpr int kDy[6] = {-1, 1, 0, 0, 0, 0};
  static constexpr int kDz[6] = {0, 0, -1, 1, 0, 0};
  const uint32_t dir = cfg.direction < 6 ? cfg.direction : 1;
  const int step_x = kDx[dir];
  const int step_y = kDy[dir];
  const int step_z = kDz[dir];

  int allowed = total;
  for (int k = 0; k < total; ++k) {
    if (!eval_predicate(chunk, cfg.allowed_placement, ox + step_x * (k + 1), oy + step_y * (k + 1),
                        oz + step_z * (k + 1))) {
      allowed = k;
      break;
    }
  }
  if (allowed < total) {
    // `BlockColumnFeature.truncate`: take the excess out of the layers from the
    // tip backwards, or from the base forwards when the tip is what survives.
    int remove = total - allowed;
    if (cfg.prioritize_tip != 0) {
      for (uint32_t i = 0; i < cfg.layer_count && remove > 0; ++i) {
        const int take = std::min(heights[i], remove);
        heights[i] -= take;
        remove -= take;
      }
    } else {
      for (int i = static_cast<int>(cfg.layer_count) - 1; i >= 0 && remove > 0; --i) {
        const int take = std::min(heights[i], remove);
        heights[i] -= take;
        remove -= take;
      }
    }
  }

  uint32_t placed = 0;
  int x = ox;
  int y = oy;
  int z = oz;
  for (uint32_t i = 0; i < cfg.layer_count; ++i) {
    const uint32_t provider = kWgColumnLayers[cfg.layer_offset + i].provider;
    for (int k = 0; k < heights[i]; ++k) {
      const BlockStateKey next = sample_state_provider(rng, provider);
      if (try_set(chunk, x, y, z, next.name, next.properties, /*only_replaceable=*/false))
        ++placed;
      x += step_x;
      y += step_y;
      z += step_z;
    }
  }
  return placed;
}

// ---- trees -----------------------------------------------------------------

// `TrunkPlacer.getTreeHeight`.
int tree_height(const WgTreeConfig& cfg, WgRandom& rng) {
  // `TrunkPlacer.getTreeHeight`: the heightRandA draw, then the heightRandB
  // one. Sequenced so the order cannot follow the compiler.
  const int height_rand_a = rng.next_int(cfg.height_rand_a + 1);
  const int height_rand_b = rng.next_int(cfg.height_rand_b + 1);
  return cfg.base_height + height_rand_a + height_rand_b;
}

bool is_leaf_name(std::string_view name) {
  return name.size() > 7 && name.substr(name.size() - 7) == "_leaves";
}

void place_trunk_log(Chunk& chunk, const WgTreeConfig& cfg, int x, int y, int z, uint32_t& placed) {
  if (try_set(chunk, x, y, z, cfg.trunk.name, cfg.trunk.properties, /*only_replaceable=*/true))
    ++placed;
}

// `FoliagePlacer.shouldSkipLocationSigned` for the foliage kinds this port
// models. The double-trunk fold (`min(|dx|, |dx-1|)`) is vanilla's.
bool skip_leaf(WgRandom& rng, int foliage_kind, int dx, int y, int dz, int radius,
               bool double_trunk) {
  const int adx = double_trunk ? std::min(std::abs(dx), std::abs(dx - 1)) : std::abs(dx);
  const int adz = double_trunk ? std::min(std::abs(dz), std::abs(dz - 1)) : std::abs(dz);
  switch (foliage_kind) {
  case 2: // pine
  case 3: // spruce
  case 9: // mega pine
    return adx == radius && adz == radius && radius > 0;
  case 4: // acacia
    if (y == 0)
      return (adx > 1 || adz > 1) && adx != 0 && adz != 0;
    return adx == radius && adz == radius && radius > 0;
  case 5: // dark oak
    if (y == -1 && !double_trunk)
      return adx == radius && adz == radius;
    if (y == 1)
      return adx + adz > radius * 2 - 2;
    return false;
  case 6: // jungle / mega jungle
    return adx + adz >= 7 || adx * adx + adz * adz > radius * radius;
  case 0: // blob
  case 7: // cherry (modelled as a blob)
  case 8: // bush
  default:
    return adx == radius && adz == radius && (rng.next_int(2) == 0 || y == 0);
  }
}

// One `FoliagePlacer.placeLeavesRow`, centred at (cx, cz) and `y` above
// `base_y`. Leaves go down only where the cell is replaceable or already a
// leaf, which is what `FoliageSetter`'s own-set guard amounts to here.
void leaves_row(Chunk& chunk, const WgTreeConfig& cfg, WgRandom& rng, int cx, int base_y, int cz,
                int radius, int y, bool double_trunk, uint32_t& placed) {
  const int off = double_trunk ? 1 : 0;
  for (int dx = -radius; dx <= radius + off; ++dx) {
    for (int dz = -radius; dz <= radius + off; ++dz) {
      if (skip_leaf(rng, cfg.foliage_kind, dx, y, dz, radius, double_trunk))
        continue;
      const int wx = cx + dx;
      const int wz = cz + dz;
      const int wy = base_y + y;
      const BlockStateKey here = block_at(chunk, wx, wy, wz);
      if (!is_replaceable_name(here.name) && !is_leaf_name(here.name))
        continue;
      if (try_set(chunk, wx, wy, wz, cfg.foliage.name, cfg.foliage.properties,
                  /*only_replaceable=*/true))
        ++placed;
    }
  }
}

// `TreeFeature.place`, over the generator's stored trunk/foliage parameters.
uint32_t place_tree(Chunk& chunk, const WgTreeConfig& cfg, WgRandom& rng, int ox, int oy, int oz) {
  const int height = tree_height(cfg, rng);
  if (height <= 0)
    return 0;
  const bool double_trunk = cfg.trunk_kind >= 2; // dark oak / mega jungle / giant
  uint32_t placed = 0;

  // `TrunkPlacer.setDirtAt`: the block the trunk stands on becomes dirt.
  const BlockStateKey below = block_at(chunk, ox, oy - 1, oz);
  if (below.name == "minecraft:grass_block" || below.name == "minecraft:podzol" ||
      below.name == "minecraft:mycelium" || below.name == "minecraft:coarse_dirt" ||
      below.name == "minecraft:rooted_dirt" || below.name == "minecraft:moss_block")
    try_set(chunk, ox, oy - 1, oz, "minecraft:dirt", "", /*only_replaceable=*/false);

  // The trunk column (2x2 for the thick placers).
  const int width = double_trunk ? 2 : 1;
  for (int dx = 0; dx < width; ++dx)
    for (int dz = 0; dz < width; ++dz)
      for (int i = 0; i < height; ++i)
        place_trunk_log(chunk, cfg, ox + dx, oy + i, oz + dz, placed);

  // A branch for the fancy placers: one diagonal log near the crown.
  if (cfg.trunk_kind == 1 && height >= 4) {
    const int by = oy + height - 2;
    if (rng.next_boolean())
      place_trunk_log(chunk, cfg, ox + 1, by, oz, placed);
    if (rng.next_boolean())
      place_trunk_log(chunk, cfg, ox - 1, by, oz, placed);
    if (rng.next_boolean())
      place_trunk_log(chunk, cfg, ox, by, oz + 1, placed);
    if (rng.next_boolean())
      place_trunk_log(chunk, cfg, ox, by, oz - 1, placed);
  }

  const int crown_base = oy + height;
  const int radius = cfg.foliage_radius;
  const int offset = cfg.foliage_offset;
  // `SpruceFoliagePlacer.foliageHeight` is `max(4, treeHeight - trunkHeight)`,
  // and the generator stores the placer's `trunk_height` in this slot; every
  // other modelled placer's slot is already the crown height.
  int crown = cfg.foliage_height;
  if (cfg.foliage_kind == 3)
    crown = std::max(4, height - cfg.foliage_height);

  switch (cfg.foliage_kind) {
  case 0: // blob (oak, birch): shrinking rows with a corner hole
  case 7: // cherry (modelled as a blob)
  case 8: // bush
    for (int yo = offset; yo >= offset - crown; --yo) {
      const int r = std::max(radius - 1 - yo / 2, 0);
      leaves_row(chunk, cfg, rng, ox, crown_base, oz, r, yo, double_trunk, placed);
    }
    break;
  case 1: // fancy oak: a denser blob
    for (int yo = offset; yo >= offset - crown; --yo) {
      const int r = std::max(radius + 1 - std::abs(yo) / 2, 0);
      leaves_row(chunk, cfg, rng, ox, crown_base, oz, r, yo, double_trunk, placed);
    }
    break;
  case 2: // pine: a narrow cone, widening then narrowing
  case 3: // spruce
  case 9: // mega pine
  {
    int r = 0;
    const int max_r = radius;
    for (int yo = offset; yo >= offset - crown; --yo) {
      leaves_row(chunk, cfg, rng, ox, crown_base, oz, r, yo, double_trunk, placed);
      if (r >= max_r)
        r = 0;
      else
        ++r;
    }
    break;
  }
  case 4: // acacia: three flat layers, the widest at the top
    leaves_row(chunk, cfg, rng, ox, crown_base + offset, oz, radius, -1 - crown, double_trunk,
               placed);
    leaves_row(chunk, cfg, rng, ox, crown_base + offset, oz, std::max(radius - 1, 0), -crown,
               double_trunk, placed);
    leaves_row(chunk, cfg, rng, ox, crown_base + offset, oz, std::max(radius - 1, 0), 0,
               double_trunk, placed);
    break;
  case 5: // dark oak: a broad flat crown
    leaves_row(chunk, cfg, rng, ox, crown_base + offset, oz, radius + 2, -1, double_trunk, placed);
    leaves_row(chunk, cfg, rng, ox, crown_base + offset, oz, radius + 1, 0, double_trunk, placed);
    if (double_trunk) {
      leaves_row(chunk, cfg, rng, ox, crown_base + offset, oz, radius + 3, 0, double_trunk, placed);
      leaves_row(chunk, cfg, rng, ox, crown_base + offset, oz, radius + 2, 1, double_trunk, placed);
    }
    break;
  case 6: // jungle / mega jungle: rounded rows
    for (int yo = offset; yo >= offset - crown; --yo) {
      const int r = std::max(radius + 1 - yo, 0);
      leaves_row(chunk, cfg, rng, ox, crown_base, oz, r, yo, double_trunk, placed);
    }
    break;
  default:
    for (int yo = offset; yo >= offset - crown; --yo) {
      const int r = std::max(radius - 1 - yo / 2, 0);
      leaves_row(chunk, cfg, rng, ox, crown_base, oz, r, yo, double_trunk, placed);
    }
    break;
  }
  return placed;
}

// ---- fallen trees ----------------------------------------------------------

// `Direction.Plane.HORIZONTAL`'s faces in their own order (north, east, south,
// west), which is the array `Util.getRandom` indexes for the fall's direction.
constexpr int kFallenDirX[4] = {0, 1, 0, -1};
constexpr int kFallenDirZ[4] = {-1, 0, 1, 0};

// `FallenTreeFeature.getSidewaysStateModifier`: a fallen log's state is the
// trunk provider's own with `RotatedPillarBlock.AXIS` retargeted to the fall's
// axis (`x` or `z`). A state that carries no `axis` property is left as it is,
// exactly as `trySetValue` leaves it.
BlockStateKey sideways_log(const BlockStateKey& state, int dx) {
  if (state.properties.find("axis=") == std::string::npos)
    return state;
  return BlockStateKey{state.name, with_property(state.properties, "axis", dx != 0 ? "x" : "z")};
}

// `TrunkVineDecorator.place`: for every log the decorator holds, one draw per
// side in the source's own order (west, east, north, south); a nonzero draw
// puts a vine on that side when the cell is air. Each side's vine carries the
// face pointing back at the log (`VineBlock.EAST` on the west side and so on),
// with the other faces false as the block's default state has them.
void place_trunk_vine(Chunk& chunk, WgRandom& rng, const std::vector<Pos>& logs, uint32_t& placed) {
  static constexpr struct {
    int dx;
    int dz;
    const char* properties;
  } kVines[4] = {
      {-1, 0, "east=true,north=false,south=false,up=false,west=false"},
      {1, 0, "east=false,north=false,south=false,up=false,west=true"},
      {0, -1, "east=false,north=false,south=true,up=false,west=false"},
      {0, 1, "east=false,north=true,south=false,up=false,west=false"},
  };
  for (const Pos& log : logs) {
    for (const auto& vine : kVines) {
      const int x = log.x + vine.dx;
      const int z = log.z + vine.dz;
      if (rng.next_int(3) > 0 && is_air_name(block_at(chunk, x, log.y, z).name)) {
        if (try_set(chunk, x, log.y, z, "minecraft:vine", vine.properties,
                    /*only_replaceable=*/false))
          ++placed;
      }
    }
  }
}

// `FallenTreeFeature.place`: the stump at the origin (`placeStump`, whose log is
// the trunk provider's state as it is, then the stump decorator), then the fall:
// a horizontal direction, the log length (the provider's sample reduced by two,
// as the vanilla field's own `- 2` does) and a start two or three cells along
// the direction. `setGroundHeightForFallenLogStartPos` walks that start one cell
// up and then up to five down for a cell that is both a valid tree position and
// over solid ground -- vanilla keeps the last position when the walk finds
// nothing, and so does this. `canPlaceEntireFallenLog` then reads every cell of
// the fall (a valid position, with no run of three unsupported ones) before
// anything is written; the logs take the sideways state and the logs' own
// decorator runs over all of them, `Util.shuffledCopy`'s shuffle included.
//
// Two named approximations: vanilla iterates the decorated log set in Java
// `HashSet` bucket order, this port in the fall's own order, so *which* log a
// decorator draw lands on can differ where several are decorated; and
// `markAboveForPostProcessing` has no counterpart because this stage has no
// post-processing pass (nothing reads it here).
uint32_t place_fallen_tree(Chunk& chunk, const WgFallenTreeConfig& cfg, WgRandom& rng, int ox,
                           int oy, int oz) {
  uint32_t placed = 0;

  // `placeStump`.
  const BlockStateKey stump = sample_state_provider(rng, cfg.trunk);
  if (try_set(chunk, ox, oy, oz, stump.name, stump.properties, /*only_replaceable=*/true))
    ++placed;
  if (cfg.stump_vine) {
    const std::vector<Pos> stump_logs{Pos{ox, oy, oz}};
    place_trunk_vine(chunk, rng, stump_logs, placed);
  }

  // The fall's own draws, in vanilla's order: direction, the 2-or-3 offset, and
  // the length.
  const int dir = rng.next_int(4);
  const int dx = kFallenDirX[dir];
  const int dz = kFallenDirZ[dir];
  const int log_length = sample_int_provider(rng, cfg.log_length) - 2;
  int sx = ox + dx * (2 + rng.next_int(2));
  int sz = oz + dz * (2 + rng.next_int(2));
  int sy = oy + 1;
  for (int i = 0; i < 6; ++i) {
    if (is_replaceable_by_trees_name(block_at(chunk, sx, sy, sz).name) &&
        block_blocks_motion(block_at(chunk, sx, sy - 1, sz).name))
      break;
    --sy;
  }

  int unsupported = 0;
  for (int i = 0; i < log_length; ++i) {
    const int x = sx + dx * i;
    const int z = sz + dz * i;
    if (!is_replaceable_by_trees_name(block_at(chunk, x, sy, z).name))
      return placed;
    if (block_blocks_motion(block_at(chunk, x, sy - 1, z).name)) {
      unsupported = 0;
    } else if (++unsupported > 2) {
      return placed;
    }
  }

  std::vector<Pos> logs;
  logs.reserve(log_length > 0 ? static_cast<size_t>(log_length) : 0);
  for (int i = 0; i < log_length; ++i) {
    const int x = sx + dx * i;
    const int z = sz + dz * i;
    const BlockStateKey state = sideways_log(sample_state_provider(rng, cfg.trunk), dx);
    if (try_set(chunk, x, sy, z, state.name, state.properties, /*only_replaceable=*/true))
      ++placed;
    logs.push_back(Pos{x, sy, z});
  }

  // `decorateLogs` for the logs' own decorator: `Util.shuffledCopy` first (one
  // bounded draw per element after the first), then per log the single `up`
  // direction's draw and the probability draw -- both consumed even where the
  // cell above is not air, because vanilla's `||` tests the draw first.
  if (cfg.log_block != UINT32_MAX && !logs.empty()) {
    for (size_t i = logs.size(); i > 1; --i)
      std::swap(logs[i - 1], logs[rng.next_int(static_cast<int32_t>(i))]);
    for (const Pos& log : logs) {
      (void)rng.next_int(1); // `Util.getRandom` over the pack's single `up`
      const bool roll = rng.next_float() <= cfg.probability;
      const int x = log.x;
      const int y = log.y + 1;
      const int z = log.z;
      if (!roll || !is_air_name(block_at(chunk, x, y, z).name))
        continue;
      const BlockStateKey mushroom = sample_state_provider(rng, cfg.log_block);
      if (try_set(chunk, x, y, z, mushroom.name, mushroom.properties, /*only_replaceable=*/false))
        ++placed;
    }
  }
  return placed;
}

// ---- placement modifiers ---------------------------------------------------

// The height a `_WG` heightmap answers during decoration. The generated column
// carries no stored `WORLD_SURFACE_WG`/`OCEAN_FLOOR_WG` array (`Chunk::height_at`
// answers the dimension floor for one), so the question is asked of the cells
// themselves: the first free y above the highest block the heightmap's own
// predicate counts.
int world_surface_height(const Chunk& chunk, int lx, int lz) {
  const int min_y = chunk_min_y(chunk);
  for (int y = chunk_max_y(chunk) - 1; y >= min_y; --y)
    if (!is_air_name(chunk.block_state_at(lx, y, lz).name))
      return y + 1;
  return min_y;
}

// `CountOnEveryLayerPlacement.findOnGroundYPosition`: from the column's motion
// blocking height, walk down and count solid-below/empty-above transitions
// (bedrock excluded), returning the empty cell above the `layer`-th one.
int ground_y_for_layer(const Chunk& chunk, int wx, int wz, int start_y, int layer) {
  const int min_y = chunk_min_y(chunk);
  const auto empty = [](const BlockStateKey& s) {
    return is_air_name(s.name) || s.name == "minecraft:water" || s.name == "minecraft:lava";
  };
  BlockStateKey state = block_at(chunk, wx, start_y, wz);
  int seen = 0;
  for (int y = start_y; y >= min_y + 1; --y) {
    const BlockStateKey below = block_at(chunk, wx, y - 1, wz);
    if (!empty(below) && empty(state) && below.name != "minecraft:bedrock") {
      if (seen == layer)
        return y;
      ++seen;
    }
    state = below;
  }
  return INT32_MAX;
}

int motion_blocking_no_leaves_height(const Chunk& chunk, int lx, int lz) {
  const int min_y = chunk_min_y(chunk);
  for (int y = chunk_max_y(chunk) - 1; y >= min_y; --y) {
    const BlockStateKey state = chunk.block_state_at(lx, y, lz);
    if (block_blocks_motion(state.name) && !is_leaf_name(state.name))
      return y + 1;
  }
  return min_y;
}

// One modifier applied to one position. `PlacedFeature.placeWithContext` builds
// `Stream.of(origin)` and wraps it in a `flatMap` per modifier, then consumes the
// stream with `forEach`; a lazy `flatMap` pulls a single element through the
// whole downstream chain before asking upstream for the next. Vanilla therefore
// draws for position n, places the feature there, and only then draws for
// position n+1. Applying one modifier to the whole vector (all `in_square`
// pairs, then all heights, then all features) hands the later positions
// different draws -- `ore_magma`'s second vein moved six blocks in y -- so the
// chain is walked depth-first below and each finished position is emitted at
// once.
bool apply_placement(const WgPlacement& m, Chunk& chunk, const char* placed_name, WgRandom& rng,
                     const Pos& p, std::vector<Pos>& out) {
  switch (m.kind) {
  case WgPlacementKind::kCount: {
    const int n = sample_count(rng, m.i0, m.i1);
    for (int k = 0; k < n && out.size() < kMaxPositions; ++k)
      out.push_back(p);
    break;
  }
  case WgPlacementKind::kRarityFilter: {
    const float chance = m.i0 > 0 ? 1.0f / static_cast<float>(m.i0) : 0.0f;
    if (rng.next_float() < chance)
      out.push_back(p);
    break;
  }
  case WgPlacementKind::kInSquare: {
    // `InSquarePlacement`: `random.nextInt(16)` for x, then for z. Two draws in
    // one braced initialiser would leave that order to the compiler.
    const int offset_x = rng.next_int(16);
    const int offset_z = rng.next_int(16);
    out.push_back(Pos{p.x + offset_x, p.y, p.z + offset_z});
    break;
  }
  case WgPlacementKind::kHeightRange: {
    Pos q = p;
    q.y = sample_height(rng, m, chunk);
    out.push_back(q);
    break;
  }
  case WgPlacementKind::kHeightmap: {
    Pos q = p;
    if (in_chunk(chunk, p.x, p.z))
      q.y = chunk.height_at(HeightmapType::MotionBlocking, p.x - chunk.x * 16, p.z - chunk.z * 16);
    out.push_back(q);
    break;
  }
  case WgPlacementKind::kRandomOffset: {
    // `RandomOffsetPlacement` samples `xz_spread` for x, `y_spread` for y and
    // `xz_spread` again for z -- three draws in that order. The row keeps only
    // each provider's upper bound, and every Nether row is the symmetric
    // zero-plateau `TrapezoidInt(0, 0, spread)`, whose sample is vanilla's
    // special case `nextInt(spread + 1) - nextInt(spread + 1)`. The old code
    // drew x and y as a uniform range and never offset z, so every fire and
    // root cell sat an axis short of vanilla; the two draws are split so their
    // order does not depend on the compiler.
    const auto spread_offset = [&rng](int spread) {
      if (spread <= 0)
        return 0;
      const int first = rng.next_int(spread + 1);
      const int second = rng.next_int(spread + 1);
      return first - second;
    };
    Pos q = p;
    q.x += spread_offset(m.i0);
    q.y += spread_offset(m.i1);
    q.z += spread_offset(m.i0);
    out.push_back(q);
    break;
  }
  case WgPlacementKind::kBiome:
    if (biome_lists(chunk, p.x, p.y, p.z, placed_name))
      out.push_back(p);
    break;
  case WgPlacementKind::kSurfaceWaterDepthFilter: {
    const int max_depth = m.i0;
    int depth = 0;
    while (depth <= max_depth && is_fluid_name(block_at(chunk, p.x, p.y + depth, p.z).name))
      ++depth;
    if (depth <= max_depth)
      out.push_back(p);
    break;
  }
  case WgPlacementKind::kBlockPredicateFilter:
    if (eval_predicate(chunk, static_cast<uint32_t>(m.i0), p.x, p.y, p.z))
      out.push_back(p);
    break;
  case WgPlacementKind::kNoiseThresholdCount: {
    // `NoiseThresholdCountPlacement.count`: `Biome.BIOME_INFO_NOISE` at
    // (x/200, z/200), below the level -> `below_noise` copies, else
    // `above_noise`. The noise is the surface stage's own instance of the same
    // `PerlinSimplexNoise` (seed 2345, octaves [0]).
    const double threshold = static_cast<double>(m.i2) / 1000.0;
    const double noise = wg_biome_info_noise().get_value(static_cast<double>(p.x) / 200.0,
                                                         static_cast<double>(p.z) / 200.0);
    const int n = noise < threshold ? m.i1 : m.i0;
    for (int k = 0; k < n && out.size() < kMaxPositions; ++k)
      out.push_back(p);
    break;
  }
  case WgPlacementKind::kSurfaceRelativeThresholdFilter: {
    // `SurfaceRelativeThresholdFilter.shouldPlace`: the column's height on the
    // named heightmap, then the origin's y inside `[height + min, height +
    // max]`, with each bound present only when the config carried it. (An
    // absent bound is `Integer.MIN_VALUE`/`MAX_VALUE` in vanilla, which is the
    // same as no bound for a worldgen y.) The `_WG` heightmaps are answered
    // from the cells (see `world_surface_height`), not from `height_at`,
    // because a generated column carries no such stored array.
    const int ordinal = m.i0;
    if (ordinal == 0)
      return false; // a heightmap this model cannot answer: count the skip
    const long long no_bound = std::numeric_limits<long long>::max();
    if (!in_chunk(chunk, p.x, p.z)) {
      out.push_back(p);
      break;
    }
    const int lx = p.x - chunk.x * 16;
    const int lz = p.z - chunk.z * 16;
    const int surface = ordinal == 1   ? world_surface_height(chunk, lx, lz)
                        : ordinal == 5 ? motion_blocking_no_leaves_height(chunk, lx, lz)
                                       : ocean_floor_height(chunk, lx, lz);
    const long long low = m.i2 != 0 ? static_cast<long long>(surface) + m.i1 : -no_bound;
    const long long high = m.i4 != 0 ? static_cast<long long>(surface) + m.i3 : no_bound;
    if (static_cast<long long>(p.y) >= low && static_cast<long long>(p.y) <= high)
      out.push_back(p);
    break;
  }
  case WgPlacementKind::kCountOnEveryLayer: {
    // `CountOnEveryLayerPlacement`: for layer 0, 1, ... sample `count` x/z
    // columns and take the layer-th solid/empty transition in each; stop at the
    // first layer that produces no position. The old approximation drew one
    // random y, which almost never landed on a nylium ledge and left crimson
    // fungi and vegetation with no positions at all.
    for (int layer = 0; layer < chunk_max_y(chunk) - chunk_min_y(chunk); ++layer) {
      const int n = sample_count(rng, m.i0, m.i1);
      bool found = false;
      for (int k = 0; k < n && out.size() < kMaxPositions; ++k) {
        const int x = p.x + rng.next_int(16);
        const int z = p.z + rng.next_int(16);
        // Vanilla `CountOnEveryLayerPlacement` reads `MOTION_BLOCKING`, not
        // `WORLD_SURFACE`.
        const int start_y =
            chunk.height_at(HeightmapType::MotionBlocking, x - chunk.x * 16, z - chunk.z * 16);
        const int y = ground_y_for_layer(chunk, x, z, start_y, layer);
        if (y == INT32_MAX)
          continue;
        out.push_back(Pos{x, y, z});
        found = true;
      }
      if (!found)
        break;
    }
    break;
  }
  case WgPlacementKind::kOther:
    // The generated table flattens `fixed_placement` to `kOther` because the
    // modifier's payload is a list of positions, not four integers. The End's
    // exit platform is the one feature that uses it; its pack position is
    // (100, 49, 0), and the `biome` modifier that follows still filters it.
    if (std::strcmp(placed_name, "minecraft:end_platform") == 0) {
      out.push_back(Pos{100, 49, 0});
      break;
    }
    return false; // a modifier this stage does not apply
  default:
    return false; // a modifier this stage does not apply
  }
  return true;
}

bool run_placement(Chunk& chunk, const WgPlacement* mods, uint32_t count, const char* placed_name,
                   WgRandom& rng, std::vector<Pos>& positions) {
  for (uint32_t i = 0; i < count; ++i) {
    std::vector<Pos> next;
    for (const Pos& p : positions) {
      if (!apply_placement(mods[i], chunk, placed_name, rng, p, next))
        return false;
    }
    positions.swap(next);
    if (positions.empty())
      break;
  }
  return true;
}

// `PlacedFeature.placeWithContext`'s lazy `flatMap` chain: one position runs the
// whole rest of the chain -- including the feature itself -- before the next
// position is drawn. `apply_placement` produces each modifier's positions; this
// walks them depth-first and hands every finished position to `emit`.
template <typename Emit>
bool run_placement_stream(Chunk& chunk, const WgPlacement* mods, uint32_t count, uint32_t index,
                          const char* placed_name, WgRandom& rng, const Pos& p, Emit emit) {
  if (index == count)
    return emit(p);
  std::vector<Pos> next;
  if (!apply_placement(mods[index], chunk, placed_name, rng, p, next))
    return false;
  for (const Pos& q : next)
    if (!run_placement_stream(chunk, mods, count, index + 1, placed_name, rng, q, emit))
      return false;
  return true;
}

uint32_t place_configured(const WgConfiguredFeature& cf, Chunk& chunk, WgRandom& rng,
                          int64_t world_seed, int x, int y, int z, int depth);
uint32_t place_placed(const WgPlacedFeature& placed, Chunk& chunk, WgRandom& rng,
                      int64_t world_seed, int x, int y, int z, int depth);

// A kind whose config the runtime reads from a generated table. `kMonsterRoom`
// and `kFreezeTopLayer` carry an empty config (`NoneFeatureConfiguration`, `{}`
// in the pack), so their `config` slot is legitimately UINT32_MAX rather than
// evidence the generator could not flatten one.
// Kinds whose config is `NoneFeatureConfiguration` (or a config the kind
// ignores): the generated row carries `UINT32_MAX` by design for these, so the
// "unparsed config" guard must not treat them as skips.
constexpr bool kind_has_config(WgFeatureKind kind) {
  switch (kind) {
  case WgFeatureKind::kOther:
  case WgFeatureKind::kMonsterRoom:
  case WgFeatureKind::kFreezeTopLayer:
  case WgFeatureKind::kVines:
  case WgFeatureKind::kDesertWell:
  case WgFeatureKind::kGlowstoneBlob:
  case WgFeatureKind::kBlueIce:
  case WgFeatureKind::kBonusChest:
  case WgFeatureKind::kEndPlatform:
  case WgFeatureKind::kEndIsland:
  case WgFeatureKind::kEndGateway:
  case WgFeatureKind::kEndSpike:
  case WgFeatureKind::kChorusPlant:
  case WgFeatureKind::kKelp:
  case WgFeatureKind::kBasaltPillar:
  case WgFeatureKind::kWeepingVines:
  case WgFeatureKind::kVoidStartPlatform:
    return false;
  default:
    return true;
  }
}

// The three selector features (`RandomSelectorFeature`, `SimpleRandomSelector`,
// `RandomBooleanSelector`). A child may be a placed feature (whose own
// placement runs first) or a configured feature placed directly at the origin.
uint32_t place_selector(Chunk& chunk, const WgSelector& sel, WgRandom& rng, int64_t world_seed,
                        int x, int y, int z, int depth) {
  const WgSelectorChild* kids = kWgSelectorChildren;
  const char* chosen = nullptr;
  uint8_t chosen_kind = 1;
  if (sel.kind == 2) { // random_boolean_selector
    if (sel.count >= 2) {
      const WgSelectorChild& c = kids[sel.offset + (rng.next_boolean() ? 0 : 1)];
      chosen = c.feature;
      chosen_kind = c.kind;
    }
  } else if (sel.kind == 1) { // simple_random_selector
    if (sel.count > 0) {
      const WgSelectorChild& c = kids[sel.offset + rng.next_int(static_cast<int32_t>(sel.count))];
      chosen = c.feature;
      chosen_kind = c.kind;
    }
  } else { // random_selector
    for (uint32_t i = 0; i < sel.count; ++i) {
      const WgSelectorChild& c = kids[sel.offset + i];
      if (rng.next_float() < c.chance) {
        chosen = c.feature;
        chosen_kind = c.kind;
        break;
      }
    }
  }
  if (chosen == nullptr || *chosen == '\0') {
    chosen = sel.default_feature;
    chosen_kind = 1;
    if (chosen == nullptr || *chosen == '\0')
      return 0;
  }
  if (chosen_kind == 0) {
    const WgPlacedFeature* child =
        find_row(kWgPlacedFeatures, std::size(kWgPlacedFeatures), chosen);
    if (child != nullptr)
      return place_placed(*child, chunk, rng, world_seed, x, y, z, depth + 1);
  }
  const WgConfiguredFeature* child =
      find_row(kWgConfiguredFeatures, std::size(kWgConfiguredFeatures), chosen);
  if (child != nullptr)
    return place_configured(*child, chunk, rng, world_seed, x, y, z, depth + 1);
  const WgPlacedFeature* fallback =
      find_row(kWgPlacedFeatures, std::size(kWgPlacedFeatures), chosen);
  if (fallback != nullptr)
    return place_placed(*fallback, chunk, rng, world_seed, x, y, z, depth + 1);
  return 0;
}

// ---- the flat extra configs ------------------------------------------------
//
// The kinds outside the older per-kind tables share one flat row (the
// generator's `extra_config_of`): three states in the order the kind's own
// algorithm names them, two `|` name lists, two recursed placed-feature
// references, and its ints/floats pooled out of line. Each port below names the
// slots it reads in its own comment, so a config change is a one-line follow-up
// rather than a hunt through the generated header.

const WgExtraConfig& extra_config(uint32_t index) {
  return index < std::size(kWgExtraConfigs) ? kWgExtraConfigs[index] : kWgExtraConfigs[0];
}

int extra_int(const WgExtraConfig& cfg, uint32_t index) {
  return index < cfg.int_count ? kWgExtraInts[cfg.int_offset + index] : 0;
}

float extra_float(const WgExtraConfig& cfg, uint32_t index) {
  return index < cfg.float_count ? kWgExtraFloats[cfg.float_offset + index] : 0.0f;
}

bool state_names(const BlockStateKey& key, const WgState& state) {
  return key.name == state.name;
}

bool list_names(const char* list, std::string_view name) {
  return list != nullptr && name_in_list(list, name);
}

bool extra_list_empty(const char* list) {
  return list == nullptr || *list == '\0';
}

// `Mth.randomBetweenInclusive`.
int between_inclusive(WgRandom& rng, int lo, int hi) {
  return lo >= hi ? lo : lo + rng.next_int(hi - lo + 1);
}

// Two draws subtracted, in a written-down order: the first bound's draw first.
// Written as `rng.next_int(a) - rng.next_int(b)` the two calls share one
// full-expression, and which runs first is the compiler's choice (clang left to
// right, gcc right to left) — the same seed would place different blocks on
// Linux. Every feature that scatters around its origin goes through here, so
// the order is stated once (`tools/check_rng_order.py` is the check).
int draw_difference(WgRandom& rng, int first_bound, int second_bound) {
  const int first = rng.next_int(first_bound);
  const int second = rng.next_int(second_bound);
  return first - second;
}

float draw_difference(WgRandom& rng) {
  const float first = rng.next_float();
  const float second = rng.next_float();
  return first - second;
}

bool is_air_key(const BlockStateKey& key) {
  // Vanilla's `BlockState.isAir()` covers every air variant. This local helper
  // used to accept only `minecraft:air`, so carved caves (`minecraft:cave_air`)
  // failed every feature's replaceable/air test and huge-fungus hats, roots and
  // other plants silently disappeared over cave air.
  return is_air_name(key.name);
}

bool is_water_key(const BlockStateKey& key) {
  return key.name == "minecraft:water";
}

// `BlockBlobFeature`: walk down to the first cell whose neighbour below matches
// the config's `can_place_on`, then stamp three overlapping ellipsoids.
uint32_t place_block_blob(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng, int x, int y,
                          int z) {
  while (y > kMinY + 3 && !list_names(cfg.ref, block_at(chunk, x, y - 1, z).name))
    --y;
  if (y <= kMinY + 3)
    return 0;
  uint32_t placed = 0;
  for (int c = 0; c < 3; ++c) {
    const int xr = rng.next_int(2);
    const int yr = rng.next_int(2);
    const int zr = rng.next_int(2);
    const float tr = static_cast<float>(xr + yr + zr) * 0.333f + 0.5f;
    for (int dx = -xr; dx <= xr; ++dx) {
      for (int dy = -yr; dy <= yr; ++dy) {
        for (int dz = -zr; dz <= zr; ++dz) {
          if (static_cast<float>(dx * dx + dy * dy + dz * dz) > tr * tr)
            continue;
          if (try_set(chunk, x + dx, y + dy, z + dz, cfg.a.name, cfg.a.properties, false))
            ++placed;
        }
      }
    }
    x += -1 + rng.next_int(2);
    y += -rng.next_int(2);
    z += -1 + rng.next_int(2);
  }
  return placed;
}

// `DesertWellFeature`: the sandstone ring, the four-cell water surface, the
// slab frame and the two suspicious-sand cells under the water.
uint32_t place_desert_well(Chunk& chunk, WgRandom& rng, int x, int y, int z) {
  const BlockStateKey kSand{"minecraft:sand", ""};
  const BlockStateKey kSlab{"minecraft:sandstone_slab", "type=bottom"};
  const BlockStateKey kStone{"minecraft:sandstone", ""};
  const BlockStateKey kWater{"minecraft:water", "level=0"};
  ++y;
  while (is_air_key(block_at(chunk, x, y, z)) && y > kMinY + 2)
    --y;
  if (!state_names(block_at(chunk, x, y, z), WgState{"minecraft:sand", ""}))
    return 0;
  for (int ox = -2; ox <= 2; ++ox)
    for (int oz = -2; oz <= 2; ++oz)
      if (is_air_key(block_at(chunk, x + ox, y - 1, z + oz)) &&
          is_air_key(block_at(chunk, x + ox, y - 2, z + oz)))
        return 0;
  const auto put = [&chunk](int px, int py, int pz, const BlockStateKey& key) {
    return try_set(chunk, px, py, pz, key.name, key.properties, false) ? 1u : 0u;
  };
  uint32_t placed = 0;
  for (int oy = -2; oy <= 0; ++oy)
    for (int ox = -2; ox <= 2; ++ox)
      for (int oz = -2; oz <= 2; ++oz)
        placed += put(x + ox, y + oy, z + oz, kStone);
  placed += put(x, y, z, kWater);
  static constexpr int kDx[4] = {0, 0, -1, 1};
  static constexpr int kDz[4] = {-1, 1, 0, 0};
  for (int i = 0; i < 4; ++i)
    placed += put(x + kDx[i], y, z + kDz[i], kWater);
  placed += put(x, y - 1, z, kSand);
  for (int i = 0; i < 4; ++i)
    placed += put(x + kDx[i], y - 1, z + kDz[i], kSand);
  for (int ox = -2; ox <= 2; ++ox)
    for (int oz = -2; oz <= 2; ++oz) {
      if (ox != -2 && ox != 2 && oz != -2 && oz != 2)
        continue;
      placed += put(x + ox, y + 1, z + oz, kStone);
    }
  placed += put(x + 2, y + 1, z, kSlab);
  placed += put(x - 2, y + 1, z, kSlab);
  placed += put(x, y + 1, z + 2, kSlab);
  placed += put(x, y + 1, z - 2, kSlab);
  for (int ox = -1; ox <= 1; ++ox)
    for (int oz = -1; oz <= 1; ++oz)
      placed += put(x + ox, y + 4, z + oz, ox == 0 && oz == 0 ? kStone : kSlab);
  for (int oy = 1; oy <= 3; ++oy) {
    placed += put(x - 1, y + oy, z - 1, kStone);
    placed += put(x - 1, y + oy, z + 1, kStone);
    placed += put(x + 1, y + oy, z - 1, kStone);
    placed += put(x + 1, y + oy, z + 1, kStone);
  }
  // `placeSusSand` picks one of the five water positions twice (vanilla draws
  // with `Util.getRandom`, i.e. one `nextInt(5)` each). The suspicious-sand
  // block is written; its brushable loot table is a block-entity fact this
  // stage does not model, the same way the monster room's chests carry none.
  const int well[5][2] = {{0, 0}, {1, 0}, {0, 1}, {-1, 0}, {0, -1}};
  for (int i = 0; i < 2; ++i) {
    const int pick = rng.next_int(5);
    placed += put(x + well[pick][0], y - 1 - i, z + well[pick][1],
                  BlockStateKey{"minecraft:suspicious_sand", ""});
  }
  return placed;
}

// `GlowstoneFeature`: a glowstone seed under netherrack/basalt/blackstone, then
// 1500 attempts at an air cell touching exactly one glowstone.
uint32_t place_glowstone_blob(Chunk& chunk, WgRandom& rng, int x, int y, int z) {
  if (!is_air_key(block_at(chunk, x, y, z)))
    return 0;
  const std::string_view above = block_at(chunk, x, y + 1, z).name;
  if (above != "minecraft:netherrack" && above != "minecraft:basalt" &&
      above != "minecraft:blackstone")
    return 0;
  uint32_t placed = try_set(chunk, x, y, z, "minecraft:glowstone", "", false) ? 1u : 0u;
  static constexpr int kDx[6] = {0, 0, 0, 0, -1, 1};
  static constexpr int kDy[6] = {-1, 1, 0, 0, 0, 0};
  static constexpr int kDz[6] = {0, 0, -1, 1, 0, 0};
  for (int i = 0; i < 1500; ++i) {
    const int px = x + draw_difference(rng, 8, 8);
    const int py = y - rng.next_int(12);
    const int pz = z + draw_difference(rng, 8, 8);
    if (!is_air_key(block_at(chunk, px, py, pz)))
      continue;
    int neighbours = 0;
    for (int f = 0; f < 6 && neighbours <= 1; ++f)
      if (block_at(chunk, px + kDx[f], py + kDy[f], pz + kDz[f]).name == "minecraft:glowstone")
        ++neighbours;
    if (neighbours != 1)
      continue;
    placed += try_set(chunk, px, py, pz, "minecraft:glowstone", "", false) ? 1u : 0u;
  }
  return placed;
}

// `BlueIceFeature`: a water cell under the sea beside packed ice, then the
// 200-attempt growth that only ever extends an existing blue-ice face.
uint32_t place_blue_ice(Chunk& chunk, WgRandom& rng, int x, int y, int z) {
  constexpr int kSeaLevel = 63;
  if (y > kSeaLevel - 1)
    return 0;
  if (!is_water_key(block_at(chunk, x, y, z)) && !is_water_key(block_at(chunk, x, y - 1, z)))
    return 0;
  static constexpr int kDx[6] = {0, 0, 0, 0, -1, 1};
  static constexpr int kDy[6] = {-1, 1, 0, 0, 0, 0};
  static constexpr int kDz[6] = {0, 0, -1, 1, 0, 0};
  bool packed = false;
  for (int f = 0; f < 6 && !packed; ++f) {
    if (f == 0) // DOWN
      continue;
    packed = block_at(chunk, x + kDx[f], y + kDy[f], z + kDz[f]).name == "minecraft:packed_ice";
  }
  if (!packed)
    return 0;
  uint32_t placed = try_set(chunk, x, y, z, "minecraft:blue_ice", "", false) ? 1u : 0u;
  for (int i = 0; i < 200; ++i) {
    const int y_off = draw_difference(rng, 5, 6);
    int xz = 3;
    if (y_off < 2)
      xz += y_off / 2;
    if (xz < 1)
      continue;
    const int px = x + draw_difference(rng, xz, xz);
    const int pz = z + draw_difference(rng, xz, xz);
    const int py = y + y_off;
    const BlockStateKey key = block_at(chunk, px, py, pz);
    if (!is_air_key(key) && !is_water_key(key) && key.name != "minecraft:packed_ice" &&
        key.name != "minecraft:ice")
      continue;
    for (int f = 0; f < 6; ++f) {
      if (block_at(chunk, px + kDx[f], py + kDy[f], pz + kDz[f]).name != "minecraft:blue_ice")
        continue;
      placed += try_set(chunk, px, py, pz, "minecraft:blue_ice", "", false) ? 1u : 0u;
      break;
    }
  }
  return placed;
}

// `SpikeFeature` (the `minecraft:ice_spike` kind): walk down from the origin to
// the first non-air cell, require the config's `can_place_on` block there, then
// stamp the spike — a stack of shrinking ellipses above and below the start,
// with the border cells thinned by a 0.75 draw — and carry its trunk down
// through air and the config's replaceable tag until it meets solid ground. The
// downward mirror and the trunk are the two places vanilla writes below the
// origin, which is why the trunk walk hard-codes `y > 50` (the ice-spike
// placement's own sea-level floor) rather than the world's minimum.
//
// Config slots: `a` is the spike state; `ref` is `can_place_on`; `ref2` is
// `can_replace` (the `minecraft:ice_spike_replaceable` tag, resolved).
uint32_t place_ice_spike(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng, int x, int y,
                         int z) {
  while (is_air_key(block_at(chunk, x, y, z)) && y > kMinY + 2)
    --y;
  if (!list_names(cfg.ref, block_at(chunk, x, y, z).name))
    return 0;
  y += rng.next_int(4);
  const int height = rng.next_int(4) + 7;
  int width = height / 4 + rng.next_int(2);
  if (width > 1 && rng.next_int(60) == 0)
    y += 10 + rng.next_int(30);
  uint32_t placed = 0;
  for (int y_off = 0; y_off < height; ++y_off) {
    const float scale =
        (1.0f - static_cast<float>(y_off) / static_cast<float>(height)) * static_cast<float>(width);
    const int new_width = static_cast<int>(std::ceil(scale));
    for (int xo = -new_width; xo <= new_width; ++xo) {
      const float dx = static_cast<float>(std::abs(xo)) - 0.25f;
      for (int zo = -new_width; zo <= new_width; ++zo) {
        const float dz = static_cast<float>(std::abs(zo)) - 0.25f;
        if ((xo != 0 || zo != 0) && dx * dx + dz * dz > scale * scale)
          continue;
        if ((xo == -new_width || xo == new_width || zo == -new_width || zo == new_width) &&
            rng.next_float() > 0.75f)
          continue;
        const BlockStateKey above = block_at(chunk, x + xo, y + y_off, z + zo);
        if (is_air_key(above) || list_names(cfg.ref2, above.name))
          placed += try_set(chunk, x + xo, y + y_off, z + zo, cfg.a.name, cfg.a.properties, false)
                        ? 1u
                        : 0u;
        if (y_off == 0 || new_width <= 1)
          continue;
        const BlockStateKey below = block_at(chunk, x + xo, y - y_off, z + zo);
        if (is_air_key(below) || list_names(cfg.ref2, below.name))
          placed += try_set(chunk, x + xo, y - y_off, z + zo, cfg.a.name, cfg.a.properties, false)
                        ? 1u
                        : 0u;
      }
    }
  }
  int pillar_width = width - 1;
  pillar_width = pillar_width < 0 ? 0 : (pillar_width > 1 ? 1 : pillar_width);
  for (int xo = -pillar_width; xo <= pillar_width; ++xo) {
    for (int zo = -pillar_width; zo <= pillar_width; ++zo) {
      int cursor_y = y - 1;
      int run_length = 50;
      if (std::abs(xo) == 1 && std::abs(zo) == 1)
        run_length = rng.next_int(5);
      for (;;) {
        if (cursor_y <= 50)
          break;
        const BlockStateKey key = block_at(chunk, x + xo, cursor_y, z + zo);
        if (!is_air_key(key) && !list_names(cfg.ref2, key.name) && !is_state(key, cfg.a))
          break;
        placed +=
            try_set(chunk, x + xo, cursor_y, z + zo, cfg.a.name, cfg.a.properties, false) ? 1u : 0u;
        --cursor_y;
        if (--run_length > 0)
          continue;
        cursor_y -= rng.next_int(5) + 1;
        run_length = rng.next_int(5);
      }
    }
  }
  return placed;
}

// `Feature.safeSetBlock`: write only when the current block is not in the
// config's `cannot_replace` set (the set is a `HolderSet<Block>` in vanilla and
// a resolved `|` name list here). Returns whether the write happened.
uint32_t safe_set(Chunk& chunk, const char* cannot_replace, int wx, int wy, int wz,
                  std::string_view name, std::string_view properties) {
  if (list_names(cannot_replace, block_at(chunk, wx, wy, wz).name))
    return 0;
  return try_set(chunk, wx, wy, wz, name, properties, false) ? 1u : 0u;
}

// A `|`-joined `name;props` list (the geode's inner placements, encoded by the
// generator) split into `(name, props)` pairs. Properties may contain commas but
// no `;` or `|`, which is what makes the two separators unambiguous.
void parse_state_list(const char* list, std::vector<std::pair<std::string, std::string>>* out) {
  out->clear();
  if (list == nullptr || *list == '\0')
    return;
  const std::string_view text(list);
  size_t start = 0;
  for (;;) {
    const size_t bar = text.find('|', start);
    const std::string_view entry =
        text.substr(start, bar == std::string_view::npos ? std::string_view::npos : bar - start);
    const size_t semi = entry.find(';');
    if (semi != std::string_view::npos) {
      out->emplace_back(std::string(entry.substr(0, semi)), std::string(entry.substr(semi + 1)));
    } else if (!entry.empty()) {
      out->emplace_back(std::string(entry), std::string());
    }
    if (bar == std::string_view::npos)
      break;
    start = bar + 1;
  }
}

// `GeodeFeature`: a noise-displaced shell of smooth basalt, calcite, amethyst
// block and budding amethyst around an air-and-crystal core, built from three
// or four distribution points and an optional crack.
//
// Config slots: `a` filling, `b` inner layer, `c` alternate inner layer, `d`
// middle layer, `e` outer layer; `ref` cannot_replace, `ref2` invalid_blocks;
// `child2` the inner placements (`name;props` joined by `|`); ints 0..1
// outer-wall distance, 2..3 distribution points, 4..5 point offset, 6 min gen
// offset, 7 max gen offset, 8 invalid-blocks threshold; floats 0 noise
// multiplier, 1 potential-placements chance, 2 alternate-layer-0 chance, 3
// placements-require-alternate, 4..7 filling/inner/middle/outer layer, 8..10
// crack chance/base size/point offset.
//
// Two honest differences from the jar, both inherited from this stage's own
// contract (see the header): the geode's normal noise is `WgNormalNoise` seeded
// from the world seed through this port's Xoroshiro stack, where vanilla builds
// a `LegacyRandomSource`-seeded `NormalNoise`, so the shell is this port's
// shape; and a fluid cell the hollow exposes is not scheduled for a tick (this
// stage writes blocks, it does not run the fluid loop).
uint32_t place_geode(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng, int64_t world_seed,
                     int x, int y, int z) {
  const int outer_lo = extra_int(cfg, 0), outer_hi = extra_int(cfg, 1);
  const int dist_lo = extra_int(cfg, 2), dist_hi = extra_int(cfg, 3);
  const int point_lo = extra_int(cfg, 4), point_hi = extra_int(cfg, 5);
  const int min_offset = extra_int(cfg, 6), max_offset = extra_int(cfg, 7);
  const int invalid_threshold = extra_int(cfg, 8);
  const double noise_multiplier = extra_float(cfg, 0);
  const double potential_chance = extra_float(cfg, 1);
  const double alternate_chance = extra_float(cfg, 2);
  const bool require_alternate = extra_float(cfg, 3) != 0.0f;
  const double filling_layer = extra_float(cfg, 4);
  const double inner_layer = extra_float(cfg, 5);
  const double middle_layer = extra_float(cfg, 6);
  const double outer_layer = extra_float(cfg, 7);
  const double crack_chance = extra_float(cfg, 8);
  const double base_crack_size = extra_float(cfg, 9);
  const int crack_point_offset = static_cast<int>(extra_float(cfg, 10));

  const int num_points = between_inclusive(rng, dist_lo, dist_hi);
  WgNormalNoise noise;
  {
    double amplitudes[1] = {1.0};
    WgRandom noise_random(world_seed);
    noise.init(noise_random, -4, amplitudes, 1);
  }
  const double crack_size_adjustment =
      static_cast<double>(num_points) / static_cast<double>(outer_hi > 0 ? outer_hi : 1);
  const double inner_air = 1.0 / std::sqrt(filling_layer);
  const double innermost_block_layer = 1.0 / std::sqrt(inner_layer + crack_size_adjustment);
  const double inner_crust = 1.0 / std::sqrt(middle_layer + crack_size_adjustment);
  const double outer_crust = 1.0 / std::sqrt(outer_layer + crack_size_adjustment);
  const double crack_size = 1.0 / std::sqrt(base_crack_size + rng.next_double() / 2.0 +
                                            (num_points > 3 ? crack_size_adjustment : 0.0));
  const bool should_generate_crack = static_cast<double>(rng.next_float()) < crack_chance;

  struct Point {
    int x, y, z;
    int offset;
  };
  std::vector<Point> points;
  points.reserve(static_cast<size_t>(num_points));
  int invalid_points = 0;
  for (int i = 0; i < num_points; ++i) {
    const int px = between_inclusive(rng, outer_lo, outer_hi);
    const int py = between_inclusive(rng, outer_lo, outer_hi);
    const int pz = between_inclusive(rng, outer_lo, outer_hi);
    // The validity scan reads a cell this stage can see only inside its own
    // column. A point that falls outside is not counted either way (the write
    // it implies is dropped by the same confinement rule, which the header
    // names), rather than reading as the air a synthetic out-of-chunk cell
    // answers — that would abort a geode on the chunk border.
    if (in_chunk(chunk, x + px, z + pz) && y + py >= kMinY && y + py < kMinY + kHeight) {
      const BlockStateKey state = block_at(chunk, x + px, y + py, z + pz);
      if ((is_air_key(state) || list_names(cfg.ref2, state.name)) &&
          ++invalid_points > invalid_threshold)
        return 0;
    }
    points.push_back(Point{x + px, y + py, z + pz, between_inclusive(rng, point_lo, point_hi)});
  }

  std::vector<Pos> crack_points;
  if (should_generate_crack) {
    const int index = rng.next_int(4);
    const int crack_offset = num_points * 2 + 1;
    if (index == 0) {
      crack_points.push_back(Pos{x + crack_offset, y + 7, z});
      crack_points.push_back(Pos{x + crack_offset, y + 5, z});
      crack_points.push_back(Pos{x + crack_offset, y + 1, z});
    } else if (index == 1) {
      crack_points.push_back(Pos{x, y + 7, z + crack_offset});
      crack_points.push_back(Pos{x, y + 5, z + crack_offset});
      crack_points.push_back(Pos{x, y + 1, z + crack_offset});
    } else if (index == 2) {
      crack_points.push_back(Pos{x + crack_offset, y + 7, z + crack_offset});
      crack_points.push_back(Pos{x + crack_offset, y + 5, z + crack_offset});
      crack_points.push_back(Pos{x + crack_offset, y + 1, z + crack_offset});
    } else {
      crack_points.push_back(Pos{x, y + 7, z});
      crack_points.push_back(Pos{x, y + 5, z});
      crack_points.push_back(Pos{x, y + 1, z});
    }
  }

  std::vector<Pos> potential_placements;
  uint32_t placed = 0;
  // `BlockPos.betweenClosed`: x fastest, then y, then z.
  for (int wz = z + min_offset; wz <= z + max_offset; ++wz) {
    for (int wy = y + min_offset; wy <= y + max_offset; ++wy) {
      for (int wx = x + min_offset; wx <= x + max_offset; ++wx) {
        const double noise_offset =
            noise.get_value(static_cast<double>(wx), static_cast<double>(wy),
                            static_cast<double>(wz)) *
            noise_multiplier;
        double shell = 0.0;
        for (const Point& p : points) {
          const double dx = static_cast<double>(wx - p.x);
          const double dy = static_cast<double>(wy - p.y);
          const double dz = static_cast<double>(wz - p.z);
          shell += 1.0 / std::sqrt(dx * dx + dy * dy + dz * dz + p.offset) + noise_offset;
        }
        double crack = 0.0;
        for (const Pos& p : crack_points) {
          const double dx = static_cast<double>(wx - p.x);
          const double dy = static_cast<double>(wy - p.y);
          const double dz = static_cast<double>(wz - p.z);
          crack += 1.0 / std::sqrt(dx * dx + dy * dy + dz * dz + crack_point_offset) + noise_offset;
        }
        if (shell < outer_crust)
          continue;
        if (should_generate_crack && crack >= crack_size && shell < inner_air) {
          placed += safe_set(chunk, cfg.ref, wx, wy, wz, cfg.a.name, cfg.a.properties);
          continue;
        }
        if (shell >= inner_air) {
          placed += safe_set(chunk, cfg.ref, wx, wy, wz, cfg.a.name, cfg.a.properties);
          continue;
        }
        if (shell >= innermost_block_layer) {
          const bool alternate = static_cast<double>(rng.next_float()) < alternate_chance;
          placed += alternate ? safe_set(chunk, cfg.ref, wx, wy, wz, cfg.c.name, cfg.c.properties)
                              : safe_set(chunk, cfg.ref, wx, wy, wz, cfg.b.name, cfg.b.properties);
          if ((require_alternate && !alternate) ||
              !(static_cast<double>(rng.next_float()) < potential_chance))
            continue;
          potential_placements.push_back(Pos{wx, wy, wz});
          continue;
        }
        if (shell >= inner_crust) {
          placed += safe_set(chunk, cfg.ref, wx, wy, wz, cfg.d.name, cfg.d.properties);
          continue;
        }
        if (shell >= outer_crust)
          placed += safe_set(chunk, cfg.ref, wx, wy, wz, cfg.e.name, cfg.e.properties);
      }
    }
  }

  // The crystal pass: one inner placement per potential cell, against the first
  // direction whose neighbour it may grow into (`Direction.values()`'s order).
  std::vector<std::pair<std::string, std::string>> inner_states;
  parse_state_list(cfg.child2, &inner_states);
  if (!inner_states.empty()) {
    static constexpr const char* kDirections[6] = {"down", "up", "north", "south", "west", "east"};
    static constexpr int kDx[6] = {0, 0, 0, 0, -1, 1};
    static constexpr int kDy[6] = {-1, 1, 0, 0, 0, 0};
    static constexpr int kDz[6] = {0, 0, -1, 1, 0, 0};
    for (const Pos& crystal : potential_placements) {
      const auto& state = inner_states[static_cast<size_t>(
          rng.next_int(static_cast<int32_t>(inner_states.size())))];
      for (int d = 0; d < 6; ++d) {
        const int px = crystal.x + kDx[d];
        const int py = crystal.y + kDy[d];
        const int pz = crystal.z + kDz[d];
        const BlockStateKey target = block_at(chunk, px, py, pz);
        const bool waterlogged = is_full_water(target);
        if (!is_air_key(target) && !waterlogged)
          continue;
        std::string properties = state.second;
        if (!properties.empty())
          properties += ',';
        properties += "facing=";
        properties += kDirections[d];
        properties += waterlogged ? ",waterlogged=true" : ",waterlogged=false";
        placed += safe_set(chunk, cfg.ref, px, py, pz, state.first, properties);
        break;
      }
    }
  }
  return placed;
}

// `BonusChestFeature`: the first free cell of a shuffled 16x16 column pair gets
// the chest, its loot table and the torches that survive beside it. The loot
// table is a block-entity fact this stage does not model.
uint32_t place_bonus_chest(Chunk& chunk, WgRandom& rng, int /*x*/, int /*z*/) {
  // The origin is inside this chunk by construction (`decorate` hands the
  // chunk's own origin to the feature), so the 16 x 16 scan below is the one
  // vanilla's `chunkPos` walk covers; the coordinates are only the callback's
  // shape.
  int xs[16];
  int zs[16];
  for (int i = 0; i < 16; ++i) {
    xs[i] = i;
    zs[i] = i;
  }
  for (int i = 15; i > 0; --i) {
    const int j = rng.next_int(i + 1);
    std::swap(xs[i], xs[j]);
    const int k = rng.next_int(i + 1);
    std::swap(zs[i], zs[k]);
  }
  for (int ix = 0; ix < 16; ++ix) {
    for (int iz = 0; iz < 16; ++iz) {
      const int wx = chunk.x * 16 + xs[ix];
      const int wz = chunk.z * 16 + zs[iz];
      const int wy = chunk.height_at(HeightmapType::MotionBlockingNoLeaves, xs[ix], zs[iz]);
      if (!is_air_key(block_at(chunk, wx, wy, wz)))
        continue;
      uint32_t placed = try_set(chunk, wx, wy, wz, "minecraft:chest", "", false) ? 1u : 0u;
      static constexpr int kDx[4] = {0, 0, -1, 1};
      static constexpr int kDz[4] = {-1, 1, 0, 0};
      for (int f = 0; f < 4; ++f) {
        const int tx = wx + kDx[f], tz = wz + kDz[f];
        if (is_air_key(block_at(chunk, tx, wy, tz)) && !is_air_key(block_at(chunk, tx, wy - 1, tz)))
          placed += try_set(chunk, tx, wy, tz, "minecraft:torch", "", false) ? 1u : 0u;
      }
      return placed;
    }
  }
  return 0;
}

// `EndPlatformFeature.createEndPlatform`: a 5x5x4 obsidian shell with an air
// interior.
uint32_t place_end_platform(Chunk& chunk, int x, int y, int z) {
  uint32_t placed = 0;
  for (int dz = -2; dz <= 2; ++dz)
    for (int dx = -2; dx <= 2; ++dx)
      for (int dy = -1; dy < 3; ++dy) {
        const bool want = dy == -1;
        const BlockStateKey key = block_at(chunk, x + dx, y + dy, z + dz);
        if (want ? key.name == "minecraft:obsidian" : is_air_key(key))
          continue;
        placed += try_set(chunk, x + dx, y + dy, z + dz,
                          want ? "minecraft:obsidian" : "minecraft:air", "", false)
                      ? 1u
                      : 0u;
      }
  return placed;
}

// `EndIslandFeature`: shrinking end-stone disks, each one cell lower.
uint32_t place_end_island(Chunk& chunk, WgRandom& rng, int x, int y, int z) {
  float size = static_cast<float>(rng.next_int(3)) + 4.0f;
  int oy = 0;
  uint32_t placed = 0;
  while (size > 0.5f) {
    for (int dx = static_cast<int>(std::floor(-size)); dx <= static_cast<int>(std::ceil(size));
         ++dx)
      for (int dz = static_cast<int>(std::floor(-size)); dz <= static_cast<int>(std::ceil(size));
           ++dz) {
        if (static_cast<float>(dx * dx + dz * dz) > (size + 1.0f) * (size + 1.0f))
          continue;
        placed +=
            try_set(chunk, x + dx, y + oy, z + dz, "minecraft:end_stone", "", false) ? 1u : 0u;
      }
    size -= static_cast<float>(rng.next_int(2)) + 0.5f;
    --oy;
  }
  return placed;
}

// `EndGatewayFeature`: the 3x5x3 shell. The exit position lives on the block
// entity, which this stage does not model.
uint32_t place_end_gateway(Chunk& chunk, int x, int y, int z) {
  uint32_t placed = 0;
  for (int ox = -1; ox <= 1; ++ox)
    for (int oy = -2; oy <= 2; ++oy)
      for (int oz = -1; oz <= 1; ++oz) {
        const bool same_x = ox == 0;
        const bool same_y = oy == 0;
        const bool same_z = oz == 0;
        const bool end = std::abs(oy) == 2;
        std::string_view name;
        if (same_x && same_y && same_z)
          name = "minecraft:end_gateway";
        else if (same_y)
          name = "minecraft:air";
        else if (end && same_x && same_z)
          name = "minecraft:bedrock";
        else if (!same_x && !same_z)
          name = end ? "minecraft:air" : "minecraft:bedrock";
        else if (end)
          name = "minecraft:air";
        else
          name = "minecraft:bedrock";
        placed += try_set(chunk, x + ox, y + oy, z + oz, name, "", false) ? 1u : 0u;
      }
  return placed;
}

// `EndSpikeFeature`: the ten spikes ring the origin at 42 blocks. The pack's
// configured feature carries an empty spike list, so vanilla derives the sizes
// from the level seed. `EndSpikeFeature.createSpikes` uses
// `RandomSource.createThreadLocalInstance(seed)`, which is a
// `SingleThreadedRandomSource` -- the 48-bit `BitRandomSource` LCG, not the
// Xoroshiro stream the feature RNG uses. `nextLong() & 0xFFFF` seeds a second
// LCG, `Util.toShuffledList(0..9, ...)` shuffles the sizes, and the shuffle
// order assigns a size to each fixed ring position. (The old code shuffled
// with the feature RNG and consequently picked a different, larger pillar at
// `(2,0)`.) The geometry (ring, radius/height formulas, guarded cage) is
// vanilla's.
uint32_t place_end_spike(Chunk& chunk, int64_t world_seed, int x, int y, int z) {
  (void)y;
  WgLegacyRandom level_random(world_seed);
  const int shuffle_seed = static_cast<int>(level_random.next_long() & 65535);
  WgLegacyRandom rng(shuffle_seed);
  int order[10];
  for (int i = 0; i < 10; ++i)
    order[i] = i;
  for (int i = 9; i > 0; --i) {
    const int j = rng.next_int(i + 1);
    std::swap(order[i], order[j]);
  }
  uint32_t placed = 0;
  for (int i = 0; i < 10; ++i) {
    const int cx = static_cast<int>(
        std::floor(42.0 * std::cos(2.0 * (-3.14159265358979323846 + 0.3141592653589793 * i))));
    const int cz = static_cast<int>(
        std::floor(42.0 * std::sin(2.0 * (-3.14159265358979323846 + 0.3141592653589793 * i))));
    const int size = order[i];
    const int radius = 2 + size / 3;
    const int height = 76 + size * 3;
    const bool guarded = size == 1 || size == 2;
    if ((cx >> 4) != (x >> 4) || (cz >> 4) != (z >> 4))
      continue;
    for (int dx = -radius; dx <= radius; ++dx)
      for (int dz = -radius; dz <= radius; ++dz)
        for (int py = kMinY; py <= height + 10; ++py) {
          const bool inside = dx * dx + dz * dz <= radius * radius + 1 && py < height;
          if (inside) {
            placed +=
                try_set(chunk, cx + dx, py, cz + dz, "minecraft:obsidian", "", false) ? 1u : 0u;
          } else if (py > 65) {
            placed += try_set(chunk, cx + dx, py, cz + dz, "minecraft:air", "", false) ? 1u : 0u;
          }
        }
    if (guarded) {
      for (int dx = -2; dx <= 2; ++dx)
        for (int dz = -2; dz <= 2; ++dz)
          for (int dy = 0; dy <= 3; ++dy) {
            const bool top = dy == 3;
            const bool x_side = std::abs(dx) == 2;
            const bool z_side = std::abs(dz) == 2;
            if (!x_side && !z_side && !top)
              continue;
            const bool x_edge = dx == -2 || dx == 2 || top;
            const bool z_edge = dz == -2 || dz == 2 || top;
            std::string props = "north=false,south=false,east=false,west=false";
            props = with_property(props, "north", x_edge && dz != -2 ? "true" : "false");
            props = with_property(props, "south", x_edge && dz != 2 ? "true" : "false");
            props = with_property(props, "west", z_edge && dx != -2 ? "true" : "false");
            props = with_property(props, "east", z_edge && dx != 2 ? "true" : "false");
            placed +=
                try_set(chunk, cx + dx, height + dy, cz + dz, "minecraft:iron_bars", props, false)
                    ? 1u
                    : 0u;
          }
    }
    placed += try_set(chunk, cx, height, cz, "minecraft:bedrock", "", false) ? 1u : 0u;
    placed += try_set(chunk, cx, height + 1, cz, "minecraft:fire", "", false) ? 1u : 0u;
  }
  return placed;
}

// `ChorusPlantFeature` + `ChorusFlowerBlock.generatePlant`: the recursive chorus
// growth, bounded to eight calls deep and to the trunk cells the flower allows.
uint32_t place_chorus_plant(Chunk& chunk, WgRandom& rng, int x, int y, int z) {
  if (!is_air_key(block_at(chunk, x, y, z)))
    return 0;
  const std::string_view below = block_at(chunk, x, y - 1, z).name;
  if (below != "minecraft:end_stone" && below != "minecraft:chorus_plant" &&
      below != "minecraft:chorus_flower")
    return 0;
  // The recursive growth is a small deterministic tree: each cell either becomes
  // a flower (and stops) or a plant that grows one cell up, and the side shoots
  // are the same walk one cell out. `generatePlant`'s own `maxHorizontal` and
  // `age` rules are reproduced as the depth bound and the 4-in-5 chance.
  struct Node {
    int x, y, z, depth;
  };
  uint32_t placed = 0;
  std::vector<Node> stack;
  stack.push_back({x, y, z, 0});
  while (!stack.empty()) {
    const Node node = stack.back();
    stack.pop_back();
    if (!in_chunk(chunk, node.x, node.z) || node.y >= kMinY + kHeight)
      continue;
    const bool flower = node.depth >= 4 || rng.next_int(5) == 0;
    placed += try_set(chunk, node.x, node.y, node.z,
                      flower ? "minecraft:chorus_flower" : "minecraft:chorus_plant", "", false)
                  ? 1u
                  : 0u;
    if (flower || node.depth >= 7)
      continue;
    stack.push_back({node.x, node.y + 1, node.z, node.depth + 1});
    if (node.depth >= 2) {
      static constexpr int kDx[4] = {0, 0, -1, 1};
      static constexpr int kDz[4] = {-1, 1, 0, 0};
      for (int f = 0; f < 4; ++f)
        if (rng.next_int(4) == 0)
          stack.push_back({node.x + kDx[f], node.y, node.z + kDz[f], node.depth + 1});
    }
  }
  return placed;
}

// `KelpFeature`: the column from the ocean floor, a one-in-four head age, and
// the truncated tip when the water column ends early.
uint32_t place_kelp(Chunk& chunk, WgRandom& rng, int x, int y, int z) {
  (void)y;
  const int lx = x - chunk.x * 16;
  const int lz = z - chunk.z * 16;
  if (lx < 0 || lx >= 16 || lz < 0 || lz >= 16)
    return 0;
  const int floor_y = chunk.height_at(HeightmapType::OceanFloor, lx, lz);
  int py = floor_y;
  if (!is_water_key(block_at(chunk, x, py, z)))
    return 0;
  const int height = 1 + rng.next_int(10);
  uint32_t placed = 0;
  for (int h = 0; h <= height; ++h) {
    if (is_water_key(block_at(chunk, x, py, z)) && is_water_key(block_at(chunk, x, py + 1, z)) &&
        !is_air_key(block_at(chunk, x, py - 1, z))) {
      if (h == height) {
        placed += try_set(chunk, x, py, z, "minecraft:kelp",
                          "age=" + std::to_string(rng.next_int(4) + 20), false)
                      ? 1u
                      : 0u;
      } else {
        placed += try_set(chunk, x, py, z, "minecraft:kelp_plant", "", false) ? 1u : 0u;
      }
    } else if (h > 0) {
      if (is_air_key(block_at(chunk, x, py - 1, z)) ||
          block_at(chunk, x, py - 2, z).name == "minecraft:kelp")
        break;
      placed += try_set(chunk, x, py - 1, z, "minecraft:kelp",
                        "age=" + std::to_string(rng.next_int(4) + 20), false)
                    ? 1u
                    : 0u;
      break;
    }
    ++py;
  }
  return placed;
}

// `SeaPickleFeature`: `count` cells at the column's OCEAN_FLOOR height, each a
// water cell with a pickle count of one to four.
uint32_t place_sea_pickle(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng, int x, int y,
                          int z) {
  (void)y;
  const int count = between_inclusive(rng, extra_int(cfg, 0), extra_int(cfg, 1));
  uint32_t placed = 0;
  for (int i = 0; i < count; ++i) {
    const int dx = draw_difference(rng, 8, 8);
    const int dz = draw_difference(rng, 8, 8);
    const int lx = x + dx - chunk.x * 16;
    const int lz = z + dz - chunk.z * 16;
    if (lx < 0 || lx >= 16 || lz < 0 || lz >= 16)
      continue;
    const int py = chunk.height_at(HeightmapType::OceanFloor, lx, lz);
    if (!is_water_key(block_at(chunk, x + dx, py, z + dz)))
      continue;
    if (is_air_key(block_at(chunk, x + dx, py - 1, z + dz)))
      continue;
    placed += try_set(chunk, x + dx, py, z + dz, "minecraft:sea_pickle",
                      "pickles=" + std::to_string(rng.next_int(4) + 1), false)
                  ? 1u
                  : 0u;
  }
  return placed;
}

// `BasaltPillarFeature`: the hanging pillar and its stubbed base.
uint32_t place_basalt_pillar(Chunk& chunk, WgRandom& rng, int x, int y, int z) {
  if (!is_air_key(block_at(chunk, x, y, z)) || is_air_key(block_at(chunk, x, y + 1, z)))
    return 0;
  uint32_t placed = 0;
  int py = y;
  bool north = true, south = true, west = true, east = true;
  static constexpr int kDx[4] = {0, 0, -1, 1};
  static constexpr int kDz[4] = {-1, 1, 0, 0};
  while (is_air_key(block_at(chunk, x, py, z))) {
    if (py < kMinY || py >= kMinY + kHeight)
      return placed;
    placed += try_set(chunk, x, py, z, "minecraft:basalt", "axis=y", false) ? 1u : 0u;
    bool* flags[4] = {&north, &south, &west, &east};
    for (int f = 0; f < 4; ++f) {
      if (*flags[f]) {
        if (rng.next_int(10) != 0)
          placed += try_set(chunk, x + kDx[f], py, z + kDz[f], "minecraft:basalt", "axis=y", false)
                        ? 1u
                        : 0u;
        else
          *flags[f] = false;
      }
    }
    --py;
  }
  ++py;
  for (int f = 0; f < 4; ++f)
    if (rng.next_boolean())
      placed +=
          try_set(chunk, x + kDx[f], py, z + kDz[f], "minecraft:basalt", "axis=y", false) ? 1u : 0u;
  --py;
  for (int dx = -3; dx < 4; ++dx)
    for (int dz = -3; dz < 4; ++dz) {
      const int probability = std::abs(dx) * std::abs(dz);
      if (rng.next_int(10) >= 10 - probability)
        continue;
      int bx = x + dx, bz = z + dz, by = py;
      int max_drop = 3;
      while (is_air_key(block_at(chunk, bx, by - 1, bz)) && --max_drop > 0)
        --by;
      if (is_air_key(block_at(chunk, bx, by - 1, bz)))
        continue;
      placed += try_set(chunk, bx, by, bz, "minecraft:basalt", "axis=y", false) ? 1u : 0u;
    }
  return placed;
}

// `BasaltColumnsFeature`: the clustered/unclustered columns above the lava sea.
uint32_t place_basalt_columns(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng, int x, int y,
                              int z) {
  constexpr int kLavaSeaLevel = 32;
  const auto cannot_place_on = [](std::string_view name) {
    return name == "minecraft:lava" || name == "minecraft:bedrock" ||
           name == "minecraft:magma_block" || name == "minecraft:soul_sand" ||
           name == "minecraft:nether_bricks" || name == "minecraft:nether_brick_fence" ||
           name == "minecraft:nether_brick_stairs" || name == "minecraft:nether_wart" ||
           name == "minecraft:chest" || name == "minecraft:spawner";
  };
  const auto is_air_or_lava = [&chunk](int px, int py, int pz) {
    const BlockStateKey key = block_at(chunk, px, py, pz);
    return is_air_key(key) || (key.name == "minecraft:lava" && py <= kLavaSeaLevel);
  };
  if (!is_air_or_lava(x, y, z))
    return 0;
  if (cannot_place_on(block_at(chunk, x, y - 1, z).name) ||
      is_air_key(block_at(chunk, x, y - 1, z)))
    return 0;
  const int column_height = between_inclusive(rng, extra_int(cfg, 0), extra_int(cfg, 1));
  const bool clustered = rng.next_float() < 0.9f;
  const int reach = std::min(column_height, clustered ? 5 : 8);
  const int count = clustered ? 50 : 15;
  uint32_t placed = 0;
  for (int i = 0; i < count; ++i) {
    const int px = x + between_inclusive(rng, -reach, reach);
    const int pz = z + between_inclusive(rng, -reach, reach);
    const int to_place = column_height - (std::abs(px - x) + std::abs(pz - z));
    if (to_place < 0)
      continue;
    const int column_reach = between_inclusive(rng, extra_int(cfg, 2), extra_int(cfg, 3));
    for (int dx = -column_reach; dx <= column_reach; ++dx)
      for (int dz = -column_reach; dz <= column_reach; ++dz) {
        const int step_limit = std::abs(dx) + std::abs(dz);
        int cy = y;
        if (is_air_or_lava(px + dx, cy, pz + dz)) {
          int limit = step_limit;
          while (cy > kMinY + 1 && limit > 0) {
            --limit;
            if (is_air_or_lava(px + dx, cy, pz + dz) &&
                !cannot_place_on(block_at(chunk, px + dx, cy - 1, pz + dz).name) &&
                !is_air_key(block_at(chunk, px + dx, cy - 1, pz + dz)))
              break;
            --cy;
          }
          if (limit == 0)
            continue;
        } else {
          int limit = step_limit;
          bool found = false;
          while (cy <= kMinY + kHeight - 1 && limit > 0) {
            --limit;
            const BlockStateKey key = block_at(chunk, px + dx, cy, pz + dz);
            if (cannot_place_on(key.name))
              break;
            if (is_air_key(key)) {
              found = true;
              break;
            }
            ++cy;
          }
          if (!found)
            continue;
        }
        for (int blocks_y = to_place - step_limit / 2; blocks_y >= 0; --blocks_y) {
          if (is_air_or_lava(px + dx, cy, pz + dz)) {
            placed +=
                try_set(chunk, px + dx, cy, pz + dz, "minecraft:basalt", "axis=y", false) ? 1u : 0u;
            ++cy;
          } else if (block_at(chunk, px + dx, cy, pz + dz).name == "minecraft:basalt") {
            ++cy;
          } else {
            break;
          }
        }
      }
  }
  return placed;
}

// `ReplaceBlobsFeature` (`netherrack_replace_blobs`): find the target block below
// the origin, then replace the Manhattan ball of it.
uint32_t place_replace_blobs(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng, int x, int y,
                             int z) {
  // `ReplaceBlobsFeature.findTarget` clamps the origin into the build height
  // before walking down. `blackstone_blobs` is placed at y=0, so without the
  // clamp every attempt starts on bedrock and the feature never fires.
  const int min_y = chunk_min_y(chunk);
  const int max_y = chunk_max_y(chunk);
  int cy = std::clamp(y, min_y + 1, max_y - 1);
  while (cy > min_y + 1 && !state_names(block_at(chunk, x, cy, z), cfg.a))
    --cy;
  if (!state_names(block_at(chunk, x, cy, z), cfg.a))
    return 0;
  const int rx = between_inclusive(rng, extra_int(cfg, 0), extra_int(cfg, 1));
  const int ry = between_inclusive(rng, extra_int(cfg, 0), extra_int(cfg, 1));
  const int rz = between_inclusive(rng, extra_int(cfg, 0), extra_int(cfg, 1));
  const int limit = std::max(rx, std::max(ry, rz));
  uint32_t placed = 0;
  for (int dx = -rx; dx <= rx; ++dx)
    for (int dy = -ry; dy <= ry; ++dy)
      for (int dz = -rz; dz <= rz; ++dz) {
        if (std::abs(dx) + std::abs(dy) + std::abs(dz) > limit)
          continue;
        if (!state_names(block_at(chunk, x + dx, cy + dy, z + dz), cfg.a))
          continue;
        placed +=
            try_set(chunk, x + dx, cy + dy, z + dz, cfg.b.name, cfg.b.properties, false) ? 1u : 0u;
      }
  return placed;
}

// `DeltaFeature`: the rim/contents shells around the origin.
uint32_t place_delta(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng, int x, int y, int z) {
  const auto cannot_replace = [](std::string_view name) {
    return name == "minecraft:bedrock" || name == "minecraft:nether_bricks" ||
           name == "minecraft:nether_brick_fence" || name == "minecraft:nether_brick_stairs" ||
           name == "minecraft:nether_wart" || name == "minecraft:chest" ||
           name == "minecraft:spawner";
  };
  static constexpr int kDx[6] = {0, 0, 0, 0, -1, 1};
  static constexpr int kDy[6] = {-1, 1, 0, 0, 0, 0};
  static constexpr int kDz[6] = {0, 0, -1, 1, 0, 0};
  const auto is_clear = [&](int px, int py, int pz) {
    const BlockStateKey key = block_at(chunk, px, py, pz);
    if (key.name == cfg.a.name)
      return false;
    if (cannot_replace(key.name))
      return false;
    for (int f = 0; f < 6; ++f) {
      const bool air = is_air_key(block_at(chunk, px + kDx[f], py + kDy[f], pz + kDz[f]));
      if (f == 1) {
        if (air)
          return false; // UP must not be air
      } else if (!air) {
        return false;
      }
    }
    return true;
  };
  const bool spawn_rim = rng.next_double() < 0.9;
  const int rim_x = spawn_rim ? between_inclusive(rng, extra_int(cfg, 2), extra_int(cfg, 3)) : 0;
  const int rim_z = spawn_rim ? between_inclusive(rng, extra_int(cfg, 2), extra_int(cfg, 3)) : 0;
  const bool has_rim = spawn_rim && rim_x != 0 && rim_z != 0;
  const int radius_x = between_inclusive(rng, extra_int(cfg, 0), extra_int(cfg, 1));
  const int radius_z = between_inclusive(rng, extra_int(cfg, 0), extra_int(cfg, 1));
  const int radius_limit = std::max(radius_x, radius_z);
  uint32_t placed = 0;
  for (int dx = -radius_x; dx <= radius_x; ++dx)
    for (int dz = -radius_z; dz <= radius_z; ++dz) {
      if (std::abs(dx) + std::abs(dz) > radius_limit)
        continue;
      if (!is_clear(x + dx, y, z + dz))
        continue;
      if (has_rim)
        placed += try_set(chunk, x + dx, y, z + dz, cfg.b.name, cfg.b.properties, false) ? 1u : 0u;
      if (!is_clear(x + dx + rim_x, y, z + dz + rim_z))
        continue;
      placed +=
          try_set(chunk, x + dx + rim_x, y, z + dz + rim_z, cfg.a.name, cfg.a.properties, false)
              ? 1u
              : 0u;
    }
  return placed;
}

// `NetherForestVegetationFeature`.
uint32_t place_nether_forest_vegetation(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng,
                                        int x, int y, int z) {
  const std::string_view below = block_at(chunk, x, y - 1, z).name;
  if (below != "minecraft:crimson_nylium" && below != "minecraft:warped_nylium")
    return 0;
  if (y < kMinY + 1 || y + 1 > kMinY + kHeight)
    return 0;
  // The state provider is a weighted list in the crimson/warped variants and a
  // simple state (no draw) for `nether_sprouts`. Vanilla resolves it after the
  // three offsets and before the air test, so a rejected cell still spends its
  // weighted pick -- that draw is why the early port drifted on every slot.
  struct Plant {
    const char* name;
    int weight;
  };
  static constexpr Plant kCrimson[] = {{"minecraft:crimson_roots", 87},
                                       {"minecraft:crimson_fungus", 11},
                                       {"minecraft:warped_fungus", 1}};
  static constexpr Plant kWarped[] = {{"minecraft:warped_roots", 85},
                                      {"minecraft:crimson_roots", 1},
                                      {"minecraft:warped_fungus", 13},
                                      {"minecraft:crimson_fungus", 1}};
  const Plant* options = nullptr;
  uint32_t option_count = 0;
  int option_total = 0;
  if (cfg.a.name == std::string_view("minecraft:crimson_roots")) {
    options = kCrimson;
    option_count = 3;
    option_total = 99;
  } else if (cfg.a.name == std::string_view("minecraft:warped_roots")) {
    options = kWarped;
    option_count = 4;
    option_total = 100;
  }
  const int spread_w = extra_int(cfg, 0);
  const int spread_h = extra_int(cfg, 1);
  uint32_t placed = 0;
  for (int i = 0; i < spread_w * spread_w; ++i) {
    const int px = x + draw_difference(rng, spread_w, spread_w);
    const int py = y + draw_difference(rng, spread_h, spread_h);
    const int pz = z + draw_difference(rng, spread_w, spread_w);
    std::string_view name = cfg.a.name;
    if (options != nullptr) {
      int roll = rng.next_int(option_total);
      for (uint32_t k = 0; k < option_count; ++k) {
        roll -= options[k].weight;
        if (roll < 0) {
          name = options[k].name;
          break;
        }
      }
    }
    if (!is_air_key(block_at(chunk, px, py, pz)) || py <= kMinY)
      continue;
    if (!would_survive(chunk, name, px, py, pz))
      continue;
    placed += try_set(chunk, px, py, pz, name, name == cfg.a.name ? cfg.a.properties : "", false)
                  ? 1u
                  : 0u;
  }
  return placed;
}

// `TwistingVinesFeature` and `WeepingVinesFeature` share the column writer.
uint32_t place_vine_column(Chunk& chunk, WgRandom& rng, int x, int y, int z, int total_height,
                           bool twisting, int age_min, int age_max) {
  uint32_t placed = 0;
  int py = y;
  for (int h = 0; h <= total_height; ++h) {
    if (!is_air_key(block_at(chunk, x, py, z)))
      break;
    const bool last =
        h == total_height || !is_air_key(block_at(chunk, x, py + (twisting ? 1 : -1), z));
    if (last) {
      placed += try_set(chunk, x, py, z,
                        twisting ? "minecraft:twisting_vines" : "minecraft:weeping_vines",
                        "age=" + std::to_string(between_inclusive(rng, age_min, age_max)), false)
                    ? 1u
                    : 0u;
      break;
    }
    placed += try_set(chunk, x, py, z,
                      twisting ? "minecraft:twisting_vines_plant" : "minecraft:weeping_vines_plant",
                      "", false)
                  ? 1u
                  : 0u;
    py += twisting ? 1 : -1;
  }
  return placed;
}

uint32_t place_twisting_vines(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng, int x, int y,
                              int z) {
  const auto invalid = [&chunk](int px, int py, int pz) {
    if (!is_air_key(block_at(chunk, px, py, pz)))
      return true;
    const std::string_view below = block_at(chunk, px, py - 1, pz).name;
    return below != "minecraft:netherrack" && below != "minecraft:warped_nylium" &&
           below != "minecraft:warped_wart_block";
  };
  if (invalid(x, y, z))
    return 0;
  const int spread_w = extra_int(cfg, 0);
  const int spread_h = extra_int(cfg, 1);
  const int max_height = extra_int(cfg, 2);
  uint32_t placed = 0;
  for (int i = 0; i < spread_w * spread_w; ++i) {
    int px = x + between_inclusive(rng, -spread_w, spread_w);
    int py = y + between_inclusive(rng, -spread_h, spread_h);
    int pz = z + between_inclusive(rng, -spread_w, spread_w);
    // `findFirstAirBlockAboveGround`: the first air cell above the ground.
    while (py > kMinY && is_air_key(block_at(chunk, px, py - 1, pz)))
      --py;
    if (invalid(px, py, pz))
      continue;
    int height = between_inclusive(rng, 1, std::max(1, max_height));
    if (rng.next_int(6) == 0)
      height *= 2;
    if (rng.next_int(5) == 0)
      height = 1;
    placed += place_vine_column(chunk, rng, px, py, pz, height, true, 17, 25);
  }
  return placed;
}

uint32_t place_weeping_vines(Chunk& chunk, WgRandom& rng, int x, int y, int z) {
  if (!is_air_key(block_at(chunk, x, y, z)))
    return 0;
  const std::string_view above = block_at(chunk, x, y + 1, z).name;
  if (above != "minecraft:netherrack" && above != "minecraft:nether_wart_block")
    return 0;
  static constexpr int kDx[6] = {0, 0, 0, 0, -1, 1};
  static constexpr int kDy[6] = {-1, 1, 0, 0, 0, 0};
  static constexpr int kDz[6] = {0, 0, -1, 1, 0, 0};
  uint32_t placed = try_set(chunk, x, y, z, "minecraft:nether_wart_block", "", false) ? 1u : 0u;
  for (int i = 0; i < 200; ++i) {
    const int px = x + draw_difference(rng, 6, 6);
    const int py = y + draw_difference(rng, 2, 5);
    const int pz = z + draw_difference(rng, 6, 6);
    if (!is_air_key(block_at(chunk, px, py, pz)))
      continue;
    int neighbours = 0;
    for (int f = 0; f < 6 && neighbours <= 1; ++f) {
      const std::string_view name = block_at(chunk, px + kDx[f], py + kDy[f], pz + kDz[f]).name;
      if (name == "minecraft:netherrack" || name == "minecraft:nether_wart_block")
        ++neighbours;
    }
    if (neighbours != 1)
      continue;
    placed += try_set(chunk, px, py, pz, "minecraft:nether_wart_block", "", false) ? 1u : 0u;
  }
  for (int i = 0; i < 100; ++i) {
    const int px = x + draw_difference(rng, 8, 8);
    const int py = y + draw_difference(rng, 2, 7);
    const int pz = z + draw_difference(rng, 8, 8);
    if (!is_air_key(block_at(chunk, px, py, pz)))
      continue;
    const std::string_view up = block_at(chunk, px, py + 1, pz).name;
    if (up != "minecraft:netherrack" && up != "minecraft:nether_wart_block")
      continue;
    int height = between_inclusive(rng, 1, 8);
    if (rng.next_int(6) == 0)
      height *= 2;
    if (rng.next_int(5) == 0)
      height = 1;
    placed += place_vine_column(chunk, rng, px, py, pz, height, false, 23, 25);
  }
  return placed;
}

// `BambooFeature`.
uint32_t place_bamboo(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng, int x, int y, int z) {
  if (!is_air_key(block_at(chunk, x, y, z)))
    return 0;
  const std::string_view below = block_at(chunk, x, y - 1, z).name;
  if (below != "minecraft:grass_block" && below != "minecraft:dirt" &&
      below != "minecraft:podzol" && below != "minecraft:coarse_dirt" &&
      below != "minecraft:moss_block" && below != "minecraft:mud")
    return 0;
  uint32_t placed = 0;
  const int height = rng.next_int(12) + 5;
  if (rng.next_float() < extra_float(cfg, 0)) {
    const int r = rng.next_int(4) + 1;
    for (int dx = -r; dx <= r; ++dx)
      for (int dz = -r; dz <= r; ++dz) {
        if (dx * dx + dz * dz > r * r)
          continue;
        const int lx = x + dx - chunk.x * 16;
        const int lz = z + dz - chunk.z * 16;
        if (lx < 0 || lx >= 16 || lz < 0 || lz >= 16)
          continue;
        const int gy = chunk.height_at(HeightmapType::WorldSurface, lx, lz) - 1;
        const std::string_view ground = block_at(chunk, x + dx, gy, z + dz).name;
        if (ground != "minecraft:grass_block" && ground != "minecraft:dirt" &&
            ground != "minecraft:coarse_dirt" && ground != "minecraft:podzol")
          continue;
        placed +=
            try_set(chunk, x + dx, gy, z + dz, "minecraft:podzol", "snowy=false", false) ? 1u : 0u;
      }
  }
  int py = y;
  int grown = 0;
  for (int i = 0; i < height && is_air_key(block_at(chunk, x, py, z)); ++i, ++py, ++grown)
    placed +=
        try_set(chunk, x, py, z, "minecraft:bamboo", "age=1,leaves=none,stage=0", false) ? 1u : 0u;
  if (grown >= 3) {
    placed +=
        try_set(chunk, x, py, z, "minecraft:bamboo", "age=1,leaves=large,stage=1", false) ? 1u : 0u;
    placed += try_set(chunk, x, py - 1, z, "minecraft:bamboo", "age=1,leaves=large,stage=0", false)
                  ? 1u
                  : 0u;
    placed += try_set(chunk, x, py - 2, z, "minecraft:bamboo", "age=1,leaves=small,stage=0", false)
                  ? 1u
                  : 0u;
  }
  return placed;
}

// ---- the structural extra kinds --------------------------------------------

uint32_t place_placed(const WgPlacedFeature& placed, Chunk& chunk, WgRandom& rng,
                      int64_t world_seed, int x, int y, int z, int depth);

const WgPlacedFeature* find_placed(std::string_view name) {
  if (name.empty())
    return nullptr;
  return find_row(kWgPlacedFeatures, std::size(kWgPlacedFeatures), name);
}

// The kinds in this section all need the same "is this cell a cave void" test
// and the same column walk, so they share one spelled-out implementation rather
// than a lambda over captured coordinates.
struct ColumnScan {
  bool floor_ok = false;
  bool ceiling_ok = false;
  int floor = 0;   // the highest non-void cell at or below the origin
  int ceiling = 0; // the lowest non-void cell at or above the origin
};

ColumnScan scan_cave_column(const Chunk& chunk, int x, int y, int z, int range,
                            bool water_is_void) {
  const auto void_at = [&chunk, water_is_void](int px, int py, int pz) {
    const BlockStateKey key = block_at(chunk, px, py, pz);
    return is_air_key(key) || (water_is_void && is_water_key(key));
  };
  ColumnScan out;
  int py = y;
  int steps = 0;
  while (py <= kMinY + kHeight - 1 && void_at(x, py, z) && steps++ < range)
    ++py;
  if (py <= kMinY + kHeight - 1 && !void_at(x, py, z)) {
    out.ceiling_ok = true;
    out.ceiling = py;
  }
  py = y;
  steps = 0;
  while (py >= kMinY && void_at(x, py, z) && steps++ < range)
    --py;
  if (py >= kMinY && !void_at(x, py, z)) {
    out.floor_ok = true;
    out.floor = py;
  }
  return out;
}

// `LakeFeature`: a 16x8x16 boolean grid carved by four to seven ellipsoids, then
// the fluid fill and the stone barrier.
//
// Two honest approximations, both from the pack's own config: the three block
// predicates are the tag lists `features_cannot_replace` /
// `lava_pool_stone_cannot_replace`, which this port answers with the named
// blocks those tags hold (the generated table carries the barrier/fluid states
// only, not the predicates); and the ice cap is omitted because a chunk this
// stage decorates is not the chunk the biome's `shouldFreeze` is asked about
// (the freeze-top-layer feature already applies the same rule chunk-wide).
uint32_t place_lake(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng, int x, int y, int z) {
  if (y <= kMinY + 4)
    return 0;
  const int ox = x - 8;
  const int oy = y - 4;
  const int oz = z - 8;
  std::array<bool, 2048> grid{};
  const int spots = rng.next_int(4) + 4;
  for (int i = 0; i < spots; ++i) {
    const double xr = rng.next_double() * 6.0 + 3.0;
    const double yr = rng.next_double() * 4.0 + 2.0;
    const double zr = rng.next_double() * 6.0 + 3.0;
    const double xp = rng.next_double() * (16.0 - xr - 2.0) + 1.0 + xr / 2.0;
    const double yp = rng.next_double() * (8.0 - yr - 4.0) + 2.0 + yr / 2.0;
    const double zp = rng.next_double() * (16.0 - zr - 2.0) + 1.0 + zr / 2.0;
    for (int gx = 1; gx < 15; ++gx)
      for (int gz = 1; gz < 15; ++gz)
        for (int gy = 1; gy < 7; ++gy) {
          const double xd = (gx - xp) / (xr / 2.0);
          const double yd = (gy - yp) / (yr / 2.0);
          const double zd = (gz - zp) / (zr / 2.0);
          if (xd * xd + yd * yd + zd * zd < 1.0)
            grid[static_cast<size_t>((gx * 16 + gz) * 8 + gy)] = true;
        }
  }
  const auto cannot_replace = [](std::string_view name) {
    return name == "minecraft:bedrock" || name == "minecraft:spawner" ||
           name == "minecraft:chest" || name == "minecraft:end_portal_frame" ||
           name == "minecraft:reinforced_deepslate" || name == "minecraft:trial_spawner" ||
           name == "minecraft:vault";
  };
  const auto at = [&grid](int gx, int gy, int gz) {
    return grid[static_cast<size_t>((gx * 16 + gz) * 8 + gy)];
  };
  const auto border = [&at](int gx, int gy, int gz) {
    return !at(gx, gy, gz) && ((gx < 15 && at(gx + 1, gy, gz)) || (gx > 0 && at(gx - 1, gy, gz)) ||
                               (gz < 15 && at(gx, gy, gz + 1)) || (gz > 0 && at(gx, gy, gz - 1)) ||
                               (gy < 7 && at(gx, gy + 1, gz)) ||
                               // The cell below, not `z - 1` again: the sixth neighbour is the
                               // one under the cell being tested. Written as `gz - 1` this read a
                               // z-neighbour a second time and, for gz == 0, one column before the
                               // grid — a stack-buffer-underflow ASan catches in this test, and a
                               // lake border that disagreed with vanilla everywhere else.
                               (gy > 0 && at(gx, gy - 1, gz)));
  };
  // The pre-flight: a cell the feature would cut must not already be liquid
  // above the waterline, must not be a non-solid below it, and must satisfy the
  // config's `can_place_feature` (always true in the pack's lava lake).
  for (int gx = 0; gx < 16; ++gx)
    for (int gz = 0; gz < 16; ++gz)
      for (int gy = 0; gy < 8; ++gy) {
        if (!border(gx, gy, gz))
          continue;
        const BlockStateKey key = block_at(chunk, ox + gx, oy + gy, oz + gz);
        if (gy >= 4 && is_fluid_name(key.name))
          return 0;
        if (gy < 4 && !is_air_key(key) && !is_fluid_name(key.name) && key.name != cfg.a.name)
          return 0;
        if (cannot_replace(key.name))
          return 0;
      }
  uint32_t placed = 0;
  for (int gx = 0; gx < 16; ++gx)
    for (int gz = 0; gz < 16; ++gz)
      for (int gy = 0; gy < 8; ++gy) {
        if (!at(gx, gy, gz))
          continue;
        const int px = ox + gx, py = oy + gy, pz = oz + gz;
        if (cannot_replace(block_at(chunk, px, py, pz).name))
          continue;
        const bool air = gy >= 4;
        placed += try_set(chunk, px, py, pz, air ? "minecraft:cave_air" : cfg.a.name,
                          air ? "" : cfg.a.properties, false)
                      ? 1u
                      : 0u;
      }
  // `WgState::name` is a `const char*` into the generated table, so it is
  // compared by content: a pointer compare against the literal would only
  // answer "not air" because the compiler happened to pool the two strings.
  if (!is_air_key(block_at(chunk, 0, 0, 0)) && std::string_view(cfg.b.name) != "minecraft:air") {
    for (int gx = 0; gx < 16; ++gx)
      for (int gz = 0; gz < 16; ++gz)
        for (int gy = 0; gy < 8; ++gy) {
          if (!border(gx, gy, gz))
            continue;
          if (gy >= 4 && rng.next_int(2) == 0)
            continue;
          const BlockStateKey key = block_at(chunk, ox + gx, oy + gy, oz + gz);
          if (is_air_key(key) || cannot_replace(key.name))
            continue;
          placed += try_set(chunk, ox + gx, oy + gy, oz + gz, cfg.b.name, cfg.b.properties, false)
                        ? 1u
                        : 0u;
        }
  }
  return placed;
}

// `VegetationPatchFeature`: the ground disk, then one vegetation application per
// surviving column at the config's chance.
uint32_t place_vegetation_patch(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng,
                                int64_t world_seed, int x, int y, int z, int depth,
                                bool waterlogged) {
  const bool ceiling = extra_int(cfg, 5) != 0;
  const int inwards = ceiling ? -1 : 1;
  const int x_radius = between_inclusive(rng, extra_int(cfg, 0), extra_int(cfg, 1)) + 1;
  const int z_radius = between_inclusive(rng, extra_int(cfg, 0), extra_int(cfg, 1)) + 1;
  const float edge_chance = extra_float(cfg, 0);
  const float bottom_chance = extra_float(cfg, 1);
  const float vegetation_chance = extra_float(cfg, 2);
  const int vertical_range = extra_int(cfg, 4);
  const auto replaceable = [&cfg](std::string_view name) {
    return extra_list_empty(cfg.ref) ? is_replaceable_name(name) : list_names(cfg.ref, name);
  };
  std::vector<std::array<int, 3>> surface;
  uint32_t placed = 0;
  for (int dx = -x_radius; dx <= x_radius; ++dx)
    for (int dz = -z_radius; dz <= z_radius; ++dz) {
      const bool x_edge = dx == -x_radius || dx == x_radius;
      const bool z_edge = dz == -z_radius || dz == z_radius;
      const bool corner = x_edge && z_edge;
      if (corner)
        continue;
      if ((x_edge || z_edge) && edge_chance > 0.0f && rng.next_float() > edge_chance)
        continue;
      int py = y;
      int steps = 0;
      while (is_air_key(block_at(chunk, x + dx, py, z + dz)) && steps++ < vertical_range)
        py += inwards;
      steps = 0;
      while (!is_air_key(block_at(chunk, x + dx, py, z + dz)) && steps++ < vertical_range)
        py -= inwards;
      const int below_y = py + inwards;
      if (!is_air_key(block_at(chunk, x + dx, py, z + dz)))
        continue;
      const BlockStateKey support = block_at(chunk, x + dx, below_y, z + dz);
      if (is_air_key(support) || is_fluid_name(support.name))
        continue;
      int ground_depth = between_inclusive(rng, extra_int(cfg, 2), extra_int(cfg, 3));
      if (bottom_chance > 0.0f && rng.next_float() < bottom_chance)
        ++ground_depth;
      int gy = below_y;
      bool okay = true;
      for (int i = 0; i < ground_depth; ++i) {
        const BlockStateKey key = block_at(chunk, x + dx, gy, z + dz);
        if (!replaceable(key.name)) {
          okay = i != 0;
          break;
        }
        placed += try_set(chunk, x + dx, gy, z + dz, cfg.a.name, cfg.a.properties, false) ? 1u : 0u;
        gy += inwards;
      }
      if (!okay)
        continue;
      surface.push_back({x + dx, below_y, z + dz});
    }
  if (surface.empty())
    return placed;
  const WgPlacedFeature* vegetation = find_placed(cfg.child);
  for (const auto& pos : surface) {
    if (vegetation_chance <= 0.0f || rng.next_float() >= vegetation_chance)
      continue;
    const int vy = pos[1] - inwards;
    if (vegetation == nullptr)
      continue;
    placed += place_placed(*vegetation, chunk, rng, world_seed, pos[0], vy, pos[2], depth + 1);
  }
  (void)waterlogged;
  return placed;
}

// `SculkPatchFeature`: the sculk spreader. Each round drops `charge_count`
// cursors at the origin, then `spread_attempts` grow moves; the first
// `spread_rounds` write veins and the rest write solid sculk. The catalyst and
// the rare shriekers follow, exactly as the feature's own tail does.
uint32_t place_sculk_patch(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng, int x, int y,
                           int z) {
  const auto can_spread_from = [&chunk](int px, int py, int pz) {
    const BlockStateKey start = block_at(chunk, px, py, pz);
    if (start.name == "minecraft:sculk" || start.name == "minecraft:sculk_vein" ||
        start.name == "minecraft:sculk_catalyst" || start.name == "minecraft:sculk_sensor" ||
        start.name == "minecraft:sculk_shrieker")
      return true;
    if (!is_air_key(start) && !is_water_key(start))
      return false;
    static constexpr int kDx[6] = {0, 0, 0, 0, -1, 1};
    static constexpr int kDy[6] = {-1, 1, 0, 0, 0, 0};
    static constexpr int kDz[6] = {0, 0, -1, 1, 0, 0};
    for (int f = 0; f < 6; ++f)
      if (!is_air_key(block_at(chunk, px + kDx[f], py + kDy[f], pz + kDz[f])) &&
          !is_fluid_name(block_at(chunk, px + kDx[f], py + kDy[f], pz + kDz[f]).name))
        return true;
    return false;
  };
  if (!can_spread_from(x, y, z))
    return 0;
  struct Cursor {
    int x, y, z, charge;
  };
  std::vector<Cursor> cursors;
  const int charge_count = extra_int(cfg, 0);
  const int amount = extra_int(cfg, 1);
  const int spread_rounds = extra_int(cfg, 2);
  const int growth_rounds = extra_int(cfg, 3);
  const int attempts = extra_int(cfg, 4);
  static constexpr int kDx[6] = {0, 0, 0, 0, -1, 1};
  static constexpr int kDy[6] = {-1, 1, 0, 0, 0, 0};
  static constexpr int kDz[6] = {0, 0, -1, 1, 0, 0};
  uint32_t placed = 0;
  for (int round = 0; round < spread_rounds + growth_rounds; ++round) {
    for (int i = 0; i < charge_count; ++i)
      cursors.push_back({x, y, z, amount});
    const bool veins = round < spread_rounds;
    for (int i = 0; i < attempts && !cursors.empty(); ++i) {
      const size_t pick = static_cast<size_t>(rng.next_int(static_cast<int>(cursors.size())));
      Cursor& cursor = cursors[pick];
      const int face = rng.next_int(6);
      const int nx = cursor.x + kDx[face];
      const int ny = cursor.y + kDy[face];
      const int nz = cursor.z + kDz[face];
      const BlockStateKey target = block_at(chunk, nx, ny, nz);
      if (is_air_key(target) || is_water_key(target) || is_replaceable_name(target.name)) {
        const std::string_view name = veins ? "minecraft:sculk_vein" : "minecraft:sculk";
        placed += try_set(chunk, nx, ny, nz, name, "", false) ? 1u : 0u;
        if (cursor.charge > 0) {
          cursors.push_back({nx, ny, nz, cursor.charge - 1});
          cursor.charge = 0;
        }
      }
      if (cursor.charge <= 0)
        cursors.erase(cursors.begin() + static_cast<long>(pick));
    }
    cursors.clear();
  }
  // The catalyst: only with a full support below the origin.
  const float catalyst_chance = extra_float(cfg, 0);
  if (rng.next_float() <= catalyst_chance) {
    const BlockStateKey below = block_at(chunk, x, y - 1, z);
    if (!is_air_key(below) && !is_fluid_name(below.name))
      placed += try_set(chunk, x, y, z, "minecraft:sculk_catalyst", "", false) ? 1u : 0u;
  }
  const int rare = between_inclusive(rng, extra_int(cfg, 5), extra_int(cfg, 6));
  for (int i = 0; i < rare; ++i) {
    const int sx = x + rng.next_int(5) - 2;
    const int sz = z + rng.next_int(5) - 2;
    if (!is_air_key(block_at(chunk, sx, y, sz)))
      continue;
    const BlockStateKey below = block_at(chunk, sx, y - 1, sz);
    if (is_air_key(below) || is_fluid_name(below.name))
      continue;
    placed +=
        try_set(chunk, sx, y, sz, "minecraft:sculk_shrieker", "can_summon=true", false) ? 1u : 0u;
  }
  return placed;
}

// `RootSystemFeature`: try the column upward for a spot the tree fits, place the
// tree, then the rooted-dirt column under it and the hanging roots below it.
uint32_t place_root_system(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng,
                           int64_t world_seed, int x, int y, int z, int depth) {
  if (!is_air_key(block_at(chunk, x, y, z)))
    return 0;
  const WgPlacedFeature* tree = find_placed(cfg.child);
  if (tree == nullptr)
    return 0;
  const int required_space = extra_int(cfg, 5);
  const int allowed_water = extra_int(cfg, 6);
  const int level_distance = extra_int(cfg, 7);
  const int max_deviation = extra_int(cfg, 8);
  const int max_column = extra_int(cfg, 9);
  const auto space_for_tree = [&](int py) {
    for (int i = 1; i <= required_space; ++i) {
      const BlockStateKey key = block_at(chunk, x, py + i, z);
      if (is_air_key(key))
        continue;
      if (i + 1 <= allowed_water && is_water_key(key))
        continue;
      return false;
    }
    if (level_distance > 0) {
      static constexpr int kDx[4] = {0, 0, -1, 1};
      static constexpr int kDz[4] = {-1, 1, 0, 0};
      for (int f = 0; f < 4; ++f) {
        const int cx = x + kDx[f] * level_distance;
        const int cz = z + kDz[f] * level_distance;
        if (is_air_key(block_at(chunk, cx, py - max_deviation, cz)))
          return false;
        if (!is_air_key(block_at(chunk, cx, py + max_deviation, cz)))
          return false;
      }
    }
    return true;
  };
  uint32_t placed = 0;
  int target_y = -1;
  for (int dy = 0; dy < max_column; ++dy) {
    const int py = y + dy + 1;
    const int lx = x - chunk.x * 16, lz = z - chunk.z * 16;
    if (lx < 0 || lx >= 16 || lz < 0 || lz >= 16)
      break;
    if (chunk.height_at(HeightmapType::WorldSurface, lx, lz) < py)
      return placed;
    if (!is_air_key(block_at(chunk, x, py, z)) || !space_for_tree(py))
      continue;
    const BlockStateKey below = block_at(chunk, x, py - 1, z);
    if (below.name == "minecraft:lava" || is_air_key(below) || is_fluid_name(below.name))
      return placed;
    const uint32_t tree_placed = place_placed(*tree, chunk, rng, world_seed, x, py, z, depth + 1);
    if (tree_placed == 0)
      continue;
    placed += tree_placed;
    target_y = y + dy;
    break;
  }
  if (target_y < 0)
    return placed;
  const int root_radius = extra_int(cfg, 0);
  const int root_attempts = extra_int(cfg, 1);
  for (int py = y; py <= target_y + 1; ++py) {
    for (int i = 0; i < root_attempts; ++i) {
      const int rx = x + draw_difference(rng, std::max(1, root_radius), std::max(1, root_radius));
      const int rz = z + draw_difference(rng, std::max(1, root_radius), std::max(1, root_radius));
      const BlockStateKey key = block_at(chunk, rx, py, rz);
      const bool replaceable =
          extra_list_empty(cfg.ref) ? is_replaceable_name(key.name) : list_names(cfg.ref, key.name);
      if (!replaceable)
        continue;
      placed += try_set(chunk, rx, py, rz, cfg.a.name, cfg.a.properties, false) ? 1u : 0u;
    }
  }
  const int hang_radius = extra_int(cfg, 2);
  const int hang_span = extra_int(cfg, 3);
  const int hang_attempts = extra_int(cfg, 4);
  for (int i = 0; i < hang_attempts; ++i) {
    const int rx = x + draw_difference(rng, std::max(1, hang_radius), std::max(1, hang_radius));
    const int ry = y + draw_difference(rng, std::max(1, hang_span), std::max(1, hang_span));
    const int rz = z + draw_difference(rng, std::max(1, hang_radius), std::max(1, hang_radius));
    if (!is_air_key(block_at(chunk, rx, ry, rz)))
      continue;
    const BlockStateKey above = block_at(chunk, rx, ry + 1, rz);
    if (is_air_key(above) || is_fluid_name(above.name))
      continue;
    placed += try_set(chunk, rx, ry, rz, cfg.b.name, cfg.b.properties, false) ? 1u : 0u;
  }
  return placed;
}

// `UnderwaterMagmaFeature`: the water column's floor, then a random dump of
// magma cells that are not visible from outside.
uint32_t place_underwater_magma(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng, int x, int y,
                                int z) {
  const int search = extra_int(cfg, 0);
  const int radius = extra_int(cfg, 1);
  const float probability = extra_float(cfg, 0);
  int py = y;
  int steps = 0;
  while (py >= kMinY && is_water_key(block_at(chunk, x, py, z)) && steps++ < search)
    --py;
  if (!is_water_key(block_at(chunk, x, py + 1, z)))
    return 0;
  const int floor_y = py;
  const auto visible = [&chunk](int px, int py2, int pz, int face) {
    static constexpr int kDx[6] = {0, 0, 0, 0, -1, 1};
    static constexpr int kDy[6] = {-1, 1, 0, 0, 0, 0};
    static constexpr int kDz[6] = {0, 0, -1, 1, 0, 0};
    const BlockStateKey key = block_at(chunk, px + kDx[face], py2 + kDy[face], pz + kDz[face]);
    return is_air_key(key) || is_water_key(key);
  };
  uint32_t placed = 0;
  for (int dx = -radius; dx <= radius; ++dx)
    for (int dy = -radius; dy <= radius; ++dy)
      for (int dz = -radius; dz <= radius; ++dz) {
        if (rng.next_float() >= probability)
          continue;
        const int px = x + dx, py2 = floor_y + dy, pz = z + dz;
        const BlockStateKey key = block_at(chunk, px, py2, pz);
        if (is_air_key(key) || is_water_key(key))
          continue;
        if (visible(px, py2, pz, 1))
          continue;
        static constexpr int kDx[4] = {0, 0, -1, 1};
        static constexpr int kDz[4] = {-1, 1, 0, 0};
        bool exposed = false;
        for (int f = 0; f < 4 && !exposed; ++f) {
          const int nx = px + kDx[f], nz = pz + kDz[f];
          if (is_air_key(block_at(chunk, nx, py2, nz)) ||
              is_water_key(block_at(chunk, nx, py2, nz)))
            exposed = true;
        }
        if (exposed)
          continue;
        placed += try_set(chunk, px, py2, pz, "minecraft:magma_block", "", false) ? 1u : 0u;
      }
  return placed;
}

// `SpeleothemUtils.buildBaseToTipColumn`: the base/middle/frustum/tip shape.
uint32_t grow_speleothem(Chunk& chunk, int x, int y, int z, int direction, int height,
                         bool merged_tip, std::string_view pointed, bool waterlog, WgRandom& rng) {
  const auto write = [&](int thickness) {
    std::string props = "thickness=";
    switch (thickness) {
    case 0:
      props += "base";
      break;
    case 1:
      props += "middle";
      break;
    case 2:
      props += "frustum";
      break;
    case 3:
      props += "tip";
      break;
    default:
      props += "tip_merge";
      break;
    }
    props += std::string(",vertical_direction=") + (direction > 0 ? "up" : "down");
    props += std::string(",waterlogged=") + (waterlog ? "true" : "false");
    return try_set(chunk, x, y, z, pointed, props, false) ? 1u : 0u;
  };
  uint32_t placed = 0;
  int py = y;
  if (height >= 3) {
    placed += write(0);
    py += direction;
    for (int i = 0; i < height - 3; ++i, py += direction)
      placed += write(1);
  }
  if (height >= 2) {
    placed += write(2);
    py += direction;
  }
  if (height >= 1)
    placed += write(merged_tip ? 4 : 3);
  (void)rng;
  return placed;
}

bool speleothem_base(const Chunk& chunk, int x, int y, int z, const WgExtraConfig& cfg) {
  const BlockStateKey key = block_at(chunk, x, y, z);
  if (key.name == cfg.a.name)
    return true;
  return extra_list_empty(cfg.ref) ? is_replaceable_name(key.name) : list_names(cfg.ref, key.name);
}

// `SpeleothemFeature` (pointed dripstone): the tip direction from the two base
// tests, a patch of base blocks around the root, then a one- or two-cell
// speleothem.
uint32_t place_speleothem(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng, int x, int y,
                          int z) {
  const bool above = speleothem_base(chunk, x, y + 1, z, cfg);
  const bool below = speleothem_base(chunk, x, y - 1, z, cfg);
  if (!above && !below)
    return 0;
  const int direction = above && below ? (rng.next_boolean() ? -1 : 1) : (above ? -1 : 1);
  static constexpr int kDx[4] = {0, 0, -1, 1};
  static constexpr int kDz[4] = {-1, 1, 0, 0};
  const int root_y = y - direction;
  uint32_t placed = 0;
  const auto put_base = [&](int px, int py, int pz) {
    if (!speleothem_base(chunk, px, py, pz, cfg) &&
        !(extra_list_empty(cfg.ref) ? is_replaceable_name(block_at(chunk, px, py, pz).name)
                                    : list_names(cfg.ref, block_at(chunk, px, py, pz).name)))
      return 0u;
    return try_set(chunk, px, py, pz, cfg.a.name, cfg.a.properties, false) ? 1u : 0u;
  };
  placed += put_base(x, root_y, z);
  for (int f = 0; f < 4; ++f) {
    if (rng.next_float() > extra_float(cfg, 1))
      continue;
    const int x1 = x + kDx[f], z1 = z + kDz[f];
    placed += put_base(x1, root_y, z1);
    if (rng.next_float() > extra_float(cfg, 2))
      continue;
    const int f2 = rng.next_int(4);
    const int x2 = x1 + kDx[f2], z2 = z1 + kDz[f2];
    placed += put_base(x2, root_y, z2);
    if (rng.next_float() > extra_float(cfg, 3))
      continue;
    const int f3 = rng.next_int(4);
    placed += put_base(x2 + kDx[f3], root_y, z2 + kDz[f3]);
  }
  const bool taller = rng.next_float() < extra_float(cfg, 0) &&
                      (is_air_key(block_at(chunk, x, y + direction, z)) ||
                       is_water_key(block_at(chunk, x, y + direction, z)));
  placed += grow_speleothem(chunk, x, y, z, direction, taller ? 2 : 1, false, cfg.b.name,
                            is_water_key(block_at(chunk, x, y, z)), rng);
  return placed;
}

// `SpeleothemClusterFeature`: the stalactite/stalagmite cluster in one cave
// column. `ClampedNormalFloat.sample` is approximated by the symmetric
// triangular draw clamped to the same bounds; the geometry around it (the pool,
// the merge tip, the height-difference draw) is the feature's own.
uint32_t place_speleothem_cluster(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng, int x,
                                  int y, int z) {
  const auto empty_or_water = [&chunk](int px, int py, int pz) {
    const BlockStateKey key = block_at(chunk, px, py, pz);
    return is_air_key(key) || is_water_key(key);
  };
  if (!empty_or_water(x, y, z))
    return 0;
  const int height = between_inclusive(rng, extra_int(cfg, 0), extra_int(cfg, 1));
  const int x_radius = between_inclusive(rng, extra_int(cfg, 2), extra_int(cfg, 3));
  const int z_radius = between_inclusive(rng, extra_int(cfg, 2), extra_int(cfg, 3));
  const float wetness =
      extra_float(cfg, 0) + (extra_float(cfg, 1) - extra_float(cfg, 0)) * rng.next_float();
  const float density =
      extra_float(cfg, 2) + (extra_float(cfg, 3) - extra_float(cfg, 2)) * rng.next_float();
  const float chance_at_max = extra_float(cfg, 4);
  const float max_edge_distance = extra_float(cfg, 5);
  const float max_bias_distance = extra_float(cfg, 6);
  const float deviation = extra_float(cfg, 7);
  const int search = between_inclusive(rng, extra_int(cfg, 4), extra_int(cfg, 5));
  const int layer_thickness = between_inclusive(rng, extra_int(cfg, 6), extra_int(cfg, 7)) + 1;
  const int height_diff = extra_int(cfg, 8);
  const auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
  const auto height_for = [&](int distance) {
    if (rng.next_float() > density)
      return 0;
    const float mean = max_bias_distance <= 0.0f
                           ? static_cast<float>(height) / 2.0f
                           : (static_cast<float>(height) / 2.0f) *
                                 (1.0f - clamp01(static_cast<float>(distance) / max_bias_distance));
    const float draw = mean + draw_difference(rng) * deviation;
    return static_cast<int>(std::clamp(draw, 0.0f, static_cast<float>(height)));
  };
  uint32_t placed = 0;
  for (int dx = -x_radius; dx <= x_radius; ++dx)
    for (int dz = -z_radius; dz <= z_radius; ++dz) {
      const int edge = std::min(x_radius - std::abs(dx), z_radius - std::abs(dz));
      const float chance =
          max_edge_distance <= 0.0f
              ? 1.0f
              : chance_at_max + (1.0f - chance_at_max) *
                                    (1.0f - clamp01(static_cast<float>(edge) / max_edge_distance));
      const int px = x + dx, pz = z + dz;
      const ColumnScan column = scan_cave_column(chunk, px, y, pz, search, true);
      if (!column.floor_ok && !column.ceiling_ok)
        continue;
      int floor_y = column.floor;
      int ceiling_y = column.ceiling;
      const bool pool = rng.next_float() < wetness;
      if (pool && column.floor_ok && !is_air_key(block_at(chunk, px, floor_y, pz)) &&
          !is_water_key(block_at(chunk, px, floor_y, pz)) &&
          !is_water_key(block_at(chunk, px, floor_y + 1, pz))) {
        placed += try_set(chunk, px, floor_y, pz, "minecraft:water", "level=0", false) ? 1u : 0u;
        --floor_y;
      }
      int stalactite = 0;
      int stalagmite = 0;
      if (column.ceiling_ok && rng.next_double() < chance) {
        const int py = ceiling_y;
        for (int i = 0; i < layer_thickness; ++i) {
          if (!speleothem_base(chunk, px, py - i, pz, cfg) &&
              !(extra_list_empty(cfg.ref)
                    ? is_replaceable_name(block_at(chunk, px, py - i, pz).name)
                    : list_names(cfg.ref, block_at(chunk, px, py - i, pz).name)))
            break;
          placed += try_set(chunk, px, py - i, pz, cfg.a.name, cfg.a.properties, false) ? 1u : 0u;
        }
        const int max_h = column.floor_ok ? std::min(height, py - column.floor) : height;
        (void)max_h;
        stalactite = height_for(std::abs(dx) + std::abs(dz));
      }
      if (column.floor_ok && rng.next_double() < chance) {
        const int py = floor_y;
        for (int i = 0; i < layer_thickness; ++i) {
          if (!speleothem_base(chunk, px, py + i, pz, cfg) &&
              !(extra_list_empty(cfg.ref)
                    ? is_replaceable_name(block_at(chunk, px, py + i, pz).name)
                    : list_names(cfg.ref, block_at(chunk, px, py + i, pz).name)))
            break;
          placed += try_set(chunk, px, py + i, pz, cfg.a.name, cfg.a.properties, false) ? 1u : 0u;
        }
        stalagmite =
            column.ceiling_ok
                ? std::max(0, stalactite + between_inclusive(rng, -height_diff, height_diff))
                : height_for(std::abs(dx) + std::abs(dz));
      }
      bool merge = false;
      if (column.ceiling_ok && column.floor_ok && ceiling_y - stalactite <= floor_y + stalagmite) {
        const int low = std::max(ceiling_y - stalactite, floor_y + 1);
        const int high = std::min(floor_y + stalagmite, ceiling_y - 1);
        const int bottom = between_inclusive(rng, low, high + 1);
        stalactite = ceiling_y - bottom;
        stalagmite = bottom - 1 - floor_y;
      }
      merge = rng.next_boolean() && stalactite > 0 && stalagmite > 0 &&
              ceiling_y - floor_y - 1 == stalactite + stalagmite;
      if (column.ceiling_ok)
        placed += grow_speleothem(chunk, px, ceiling_y - 1, pz, -1, stalactite, merge, cfg.b.name,
                                  is_water_key(block_at(chunk, px, ceiling_y - 1, pz)), rng);
      if (column.floor_ok)
        placed += grow_speleothem(chunk, px, floor_y + 1, pz, 1, stalagmite, merge, cfg.b.name,
                                  is_water_key(block_at(chunk, px, floor_y + 1, pz)), rng);
    }
  return placed;
}

// `LargeDripstoneFeature`: the tapered stalactite/stalagmite pair over a cave
// column. `getSpeleothemHeight` is `SpeleothemUtils`'s own curve.
uint32_t place_large_dripstone(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng, int x, int y,
                               int z) {
  const auto empty_or_water = [&chunk](int px, int py, int pz) {
    const BlockStateKey key = block_at(chunk, px, py, pz);
    return is_air_key(key) || is_water_key(key);
  };
  if (!empty_or_water(x, y, z))
    return 0;
  const int search = extra_int(cfg, 3);
  const ColumnScan column = scan_cave_column(chunk, x, y, z, search, true);
  if (!column.ceiling_ok || !column.floor_ok || column.ceiling - column.floor < 4)
    return 0;
  const int column_height = column.ceiling - column.floor - 1;
  const int min_radius = extra_int(cfg, 0);
  const int max_radius = extra_int(cfg, 1);
  const int ratio_cap = static_cast<int>(static_cast<float>(column_height) * extra_float(cfg, 6));
  const int radius_cap = std::clamp(ratio_cap, min_radius, max_radius);
  const int radius = between_inclusive(rng, min_radius, std::max(min_radius, radius_cap));
  if (radius < 2)
    return 0;
  // `getSpeleothemHeight`: the same 0.75 r^1.333 - r^0.667 - 0.333 ln r curve.
  // The curve as ported does not read `scale`; the draw that produces it stays
  // because it is part of the feature's random stream, so it is still spent.
  const auto height_at = [&](float distance, int max_radius2, float scale, float bluntness) {
    (void)scale;
    const double d = std::max(static_cast<double>(distance), static_cast<double>(bluntness));
    const double r = d / max_radius2 * 0.384;
    if (r <= 0.0)
      return 0.0;
    const double part = 0.75 * std::pow(r, 1.3333333333333333) - std::pow(r, 0.6666666666666666) -
                        0.3333333333333333 * std::log(r);
    return std::max(part, 0.0) / 0.384 * max_radius2;
  };
  const float scale =
      extra_float(cfg, 0) + (extra_float(cfg, 1) - extra_float(cfg, 0)) * rng.next_float();
  const float stalactite_blunt =
      extra_float(cfg, 2) + (extra_float(cfg, 3) - extra_float(cfg, 2)) * rng.next_float();
  const float stalagmite_blunt =
      extra_float(cfg, 4) + (extra_float(cfg, 5) - extra_float(cfg, 4)) * rng.next_float();
  uint32_t placed = 0;
  const auto place_half = [&](int root_y, int direction, float bluntness) {
    uint32_t half = 0;
    for (int dx = -radius; dx <= radius; ++dx)
      for (int dz = -radius; dz <= radius; ++dz) {
        const float distance = std::sqrt(static_cast<float>(dx * dx + dz * dz));
        if (distance > radius)
          continue;
        int h = static_cast<int>(height_at(distance, radius, scale, bluntness));
        if (h <= 0)
          continue;
        if (rng.next_float() < 0.2f)
          h = static_cast<int>(static_cast<float>(h) * (0.8f + 0.2f * rng.next_float()));
        int py = root_y;
        bool out_of_stone = false;
        for (int i = 0; i < h; ++i, py += direction) {
          const BlockStateKey key = block_at(chunk, x + dx, py, z + dz);
          if (is_air_key(key) || is_water_key(key) || key.name == "minecraft:lava") {
            out_of_stone = true;
            half += try_set(chunk, x + dx, py, z + dz, "minecraft:dripstone_block", "", false) ? 1u
                                                                                               : 0u;
          } else if (out_of_stone) {
            break;
          }
        }
      }
    return half;
  };
  placed += place_half(column.ceiling - 1, -1, stalactite_blunt);
  placed += place_half(column.floor + 1, 1, stalagmite_blunt);
  return placed;
}

// `AbstractHugeMushroomFeature`: the stem, then the cap. The brown cap sets the
// four face properties; the red cap is the simpler 3x3 dome.
uint32_t place_huge_mushroom(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng, int x, int y,
                             int z) {
  const bool brown = extra_int(cfg, 1) != 0;
  int height = rng.next_int(3) + 4;
  if (rng.next_int(12) == 0)
    height *= 2;
  if (y < kMinY + 1 || y + height + 1 > kMinY + kHeight)
    return 0;
  if (!extra_list_empty(cfg.ref) && !list_names(cfg.ref, block_at(chunk, x, y - 1, z).name))
    return 0;
  const int radius = extra_int(cfg, 0);
  const auto cap_radius = [&](int dy) { return brown && dy > 3 ? radius : (brown ? 0 : radius); };
  for (int dy = 0; dy <= height; ++dy) {
    const int r = cap_radius(dy);
    for (int dx = -r; dx <= r; ++dx)
      for (int dz = -r; dz <= r; ++dz) {
        const BlockStateKey key = block_at(chunk, x + dx, y + dy, z + dz);
        if (is_air_key(key) || key.name == "minecraft:oak_leaves" ||
            key.name.find("leaves") != std::string_view::npos)
          continue;
        return 0;
      }
  }
  uint32_t placed = 0;
  for (int dx = -radius; dx <= radius; ++dx)
    for (int dz = -radius; dz <= radius; ++dz) {
      const bool min_x = dx == -radius;
      const bool max_x = dx == radius;
      const bool min_z = dz == -radius;
      const bool max_z = dz == radius;
      const bool x_edge = min_x || max_x;
      const bool z_edge = min_z || max_z;
      if (brown) {
        if (x_edge && z_edge)
          continue;
        std::string props = cfg.b.properties;
        props = with_property(props, "west",
                              (min_x || (z_edge && dx == 1 - radius)) ? "true" : "false");
        props = with_property(props, "east",
                              (max_x || (z_edge && dx == radius - 1)) ? "true" : "false");
        props = with_property(props, "north",
                              (min_z || (x_edge && dz == 1 - radius)) ? "true" : "false");
        props = with_property(props, "south",
                              (max_z || (x_edge && dz == radius - 1)) ? "true" : "false");
        placed += try_set(chunk, x + dx, y + height, z + dz, cfg.b.name, props, false) ? 1u : 0u;
      } else if (!(std::abs(dx) == radius && std::abs(dz) == radius)) {
        placed += try_set(chunk, x + dx, y + height, z + dz, cfg.b.name, cfg.b.properties, false)
                      ? 1u
                      : 0u;
      }
    }
  for (int dy = 0; dy < height; ++dy)
    placed += try_set(chunk, x, y + dy, z, cfg.a.name, cfg.a.properties, false) ? 1u : 0u;
  return placed;
}

// `CoralFeature` and its three subclasses share the water-cell guard and the
// coral-block state; the shapes differ in how the branch grows.
uint32_t place_coral(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng, int x, int y, int z) {
  if (!is_water_key(block_at(chunk, x, y, z)))
    return 0;
  const auto solid_below = [&chunk](int px, int py, int pz) {
    const BlockStateKey key = block_at(chunk, px, py, pz);
    return !is_air_key(key) && !is_water_key(key);
  };
  const int kind = extra_int(cfg, 0);
  uint32_t placed = 0;
  const auto put = [&](int px, int py, int pz) {
    if (!is_water_key(block_at(chunk, px, py, pz)))
      return 0u;
    return try_set(chunk, px, py, pz, cfg.a.name, cfg.a.properties, false) ? 1u : 0u;
  };
  if (kind == 0) { // coral_tree: a trunk that branches into up to three arms
    const int height = 1 + rng.next_int(3);
    int tip_x = x, tip_y = y, tip_z = z;
    for (int i = 0; i < height; ++i) {
      placed += put(x, y + i, z);
      tip_y = y + i;
    }
    for (int arm = 0; arm < 3; ++arm) {
      static constexpr int kDx[4] = {0, 0, -1, 1};
      static constexpr int kDz[4] = {-1, 1, 0, 0};
      const int f = rng.next_int(4);
      const int len = 1 + rng.next_int(2);
      for (int i = 1; i <= len; ++i)
        placed += put(tip_x + kDx[f] * i, tip_y, tip_z + kDz[f] * i);
    }
  } else if (kind == 1) { // coral_claw: a flat one-cell arm fan
    for (int i = 0; i < 4; ++i) {
      static constexpr int kDx[4] = {0, 0, -1, 1};
      static constexpr int kDz[4] = {-1, 1, 0, 0};
      if (!solid_below(x + kDx[i], y - 1, z + kDz[i]))
        continue;
      placed += put(x + kDx[i], y, z + kDz[i]);
      if (rng.next_boolean())
        placed += put(x + kDx[i] * 2, y, z + kDz[i] * 2);
    }
  } else { // coral_mushroom: the disk of the same radius on the floor
    const int r = 1 + rng.next_int(2);
    for (int dx = -r; dx <= r; ++dx)
      for (int dz = -r; dz <= r; ++dz) {
        if (dx * dx + dz * dz > r * r)
          continue;
        if (!solid_below(x + dx, y - 1, z + dz))
          continue;
        placed += put(x + dx, y, z + dz);
      }
  }
  return placed;
}

// `SequenceFeature`: every child placed feature, stopping at the first failure.
uint32_t place_sequence(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng, int64_t world_seed,
                        int x, int y, int z, int depth) {
  if (cfg.child2 == nullptr || *cfg.child2 == '\0')
    return 0;
  uint32_t placed = 0;
  std::string_view list = cfg.child2;
  while (!list.empty()) {
    const size_t bar = list.find('|');
    const std::string_view name = list.substr(0, bar);
    const WgPlacedFeature* child = find_placed(name);
    if (child == nullptr)
      return placed;
    const uint32_t child_placed = place_placed(*child, chunk, rng, world_seed, x, y, z, depth + 1);
    if (child_placed == 0)
      return placed;
    placed += child_placed;
    if (bar == std::string_view::npos)
      break;
    list.remove_prefix(bar + 1);
  }
  return placed;
}

// `WeightedRandomSelector`: one child by weight. The list is the config's own
// `child2` `name,weight|name,weight` string.
uint32_t place_weighted_selector(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng,
                                 int64_t world_seed, int x, int y, int z, int depth) {
  if (cfg.child2 == nullptr || *cfg.child2 == '\0')
    return 0;
  int total = 0;
  std::string_view list = cfg.child2;
  while (!list.empty()) {
    const size_t bar = list.find('|');
    const std::string_view entry = list.substr(0, bar);
    const size_t comma = entry.rfind(',');
    if (comma != std::string_view::npos)
      total += std::max(0, std::atoi(std::string(entry.substr(comma + 1)).c_str()));
    if (bar == std::string_view::npos)
      break;
    list.remove_prefix(bar + 1);
  }
  if (total <= 0)
    return 0;
  int roll = rng.next_int(total);
  list = cfg.child2;
  while (!list.empty()) {
    const size_t bar = list.find('|');
    const std::string_view entry = list.substr(0, bar);
    const size_t comma = entry.rfind(',');
    const std::string_view name = comma == std::string_view::npos ? entry : entry.substr(0, comma);
    const int weight = comma == std::string_view::npos
                           ? 0
                           : std::max(0, std::atoi(std::string(entry.substr(comma + 1)).c_str()));
    roll -= weight;
    if (roll < 0) {
      const WgPlacedFeature* child = find_placed(name);
      if (child == nullptr)
        return 0;
      return place_placed(*child, chunk, rng, world_seed, x, y, z, depth + 1);
    }
    if (bar == std::string_view::npos)
      break;
    list.remove_prefix(bar + 1);
  }
  return 0;
}

// `HugeFungusFeature`: the stem (one wide, or a 3x3 huge stem), then the hat
// whose brim drops through the layer below it, with the weeping-vine tails.
uint32_t place_huge_fungus(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng, int x, int y,
                           int z) {
  if (block_at(chunk, x, y - 1, z).name != cfg.ref2)
    return 0;
  // `HugeFungusFeature`: `Mth.nextInt(random, 4, 13)` (inclusive), then a
  // one-in-twelve chance to double the stem.
  const int base_height = between_inclusive(rng, 4, 13);
  const int double_roll = rng.next_int(12);
  int height = double_roll == 0 ? base_height * 2 : base_height;
  const bool planted = extra_int(cfg, 0) != 0;
  if (!planted && y + height + 1 >= kMinY + kHeight)
    return 0;
  const bool huge = !planted && rng.next_float() < 0.06f;
  uint32_t placed = 0;
  // `HugeFungusFeature.isReplaceable(..., true)` for the stem is air *or* the
  // config's replaceable-block predicate; the hat passes `false`, so it is air
  // only. Using one broad predicate for both made the hat loop take random
  // draws on cells vanilla skips, which shifted the shared feature RNG and
  // changed later fungi (one became huge when vanilla's was 1x1).
  const auto replaceable_stem = [&](int px, int py, int pz) {
    const BlockStateKey key = block_at(chunk, px, py, pz);
    return is_air_key(key) || (!extra_list_empty(cfg.ref) && list_names(cfg.ref, key.name));
  };
  const auto replaceable_hat = [&](int px, int py, int pz) {
    return is_air_key(block_at(chunk, px, py, pz));
  };
  placed += try_set(chunk, x, y, z, "minecraft:air", "", false) ? 1u : 0u;
  const int stem_radius = huge ? 1 : 0;
  for (int dx = -stem_radius; dx <= stem_radius; ++dx)
    for (int dz = -stem_radius; dz <= stem_radius; ++dz) {
      const bool corner = huge && std::abs(dx) == stem_radius && std::abs(dz) == stem_radius;
      for (int dy = 0; dy < height; ++dy) {
        if (!replaceable_stem(x + dx, y + dy, z + dz))
          continue;
        if (corner && rng.next_float() >= 0.1f)
          continue;
        placed +=
            try_set(chunk, x + dx, y + dy, z + dz, cfg.a.name, cfg.a.properties, false) ? 1u : 0u;
      }
    }
  const bool vines = std::string_view(cfg.b.name) == "minecraft:nether_wart_block";
  const int hat_height = std::min(rng.next_int(1 + height / 3) + 5, height);
  const int hat_start = height - hat_height;
  const auto weeping = [&](int px, int py, int pz) {
    if (!is_air_key(block_at(chunk, px, py - 1, pz)))
      return 0u;
    int goal = between_inclusive(rng, 1, 5);
    if (rng.next_int(7) == 0)
      goal *= 2;
    return place_vine_column(chunk, rng, px, py - 1, pz, goal, false, 23, 25);
  };
  for (int dy = hat_start; dy <= height; ++dy) {
    int radius = dy < height - rng.next_int(3) ? 2 : 1;
    if (hat_height > 8 && dy < hat_start + 4)
      radius = 3;
    if (huge)
      ++radius;
    for (int dx = -radius; dx <= radius; ++dx)
      for (int dz = -radius; dz <= radius; ++dz) {
        const bool edge_x = dx == -radius || dx == radius;
        const bool edge_z = dz == -radius || dz == radius;
        const bool inside = !edge_x && !edge_z && dy != height;
        const bool corner = edge_x && edge_z;
        const bool bottom = dy < hat_start + 3;
        if (!replaceable_hat(x + dx, y + dy, z + dz))
          continue;
        if (bottom) {
          if (inside)
            continue;
          if (block_at(chunk, x + dx, y + dy - 1, z + dz).name == cfg.b.name) {
            placed += try_set(chunk, x + dx, y + dy, z + dz, cfg.b.name, cfg.b.properties, false)
                          ? 1u
                          : 0u;
          } else if (rng.next_float() < 0.15f) {
            placed += try_set(chunk, x + dx, y + dy, z + dz, cfg.b.name, cfg.b.properties, false)
                          ? 1u
                          : 0u;
            if (vines && rng.next_int(11) == 0)
              placed += weeping(x + dx, y + dy, z + dz);
          }
          continue;
        }
        float decor_chance = 5.0e-4f;
        float hat_chance = 0.98f;
        float vine_chance = vines ? 0.07f : 0.0f;
        if (inside) {
          decor_chance = 0.1f;
          hat_chance = 0.2f;
          vine_chance = vines ? 0.1f : 0.0f;
        } else if (corner) {
          decor_chance = 0.01f;
          hat_chance = 0.7f;
          vine_chance = vines ? 0.083f : 0.0f;
        }
        if (rng.next_float() < decor_chance) {
          if (try_set(chunk, x + dx, y + dy, z + dz, cfg.c.name, cfg.c.properties, false)) {
            ++placed;
          }
        } else if (rng.next_float() < hat_chance) {
          if (try_set(chunk, x + dx, y + dy, z + dz, cfg.b.name, cfg.b.properties, false)) {
            ++placed;
          }
          if (rng.next_float() < vine_chance)
            placed += weeping(x + dx, y + dy, z + dz);
        }
      }
  }
  return placed;
}

// `IcebergFeature`: the ellipse/round iceberg above the sea line, its steep
// underside, the optional cut-out and the smooth pass. Ported from the decoded
// feature; the random stream is this stage's own (see the stage header).
uint32_t place_iceberg(Chunk& chunk, const WgExtraConfig& cfg, WgRandom& rng, int x, int y, int z) {
  constexpr int kSeaLevel = 63;
  const auto iceberg_state = [](std::string_view name) {
    return name == "minecraft:packed_ice" || name == "minecraft:snow_block" ||
           name == "minecraft:blue_ice";
  };
  const int origin_y = kSeaLevel;
  y = origin_y;
  const bool snow = rng.next_double() > 0.7;
  const double angle = rng.next_double() * 2.0 * 3.14159265358979323846;
  const int ellipse_a = 11 - rng.next_int(5);
  const int ellipse_c = 3 + rng.next_int(3);
  const bool is_ellipse = rng.next_double() > 0.7;
  const int over_height_base = is_ellipse ? rng.next_int(6) + 6 : rng.next_int(15) + 3;
  int over_height = over_height_base;
  if (!is_ellipse && rng.next_double() > 0.9)
    over_height += rng.next_int(19) + 7;
  const int under_height = std::min(over_height + rng.next_int(11), 18);
  const int width = std::min(over_height + draw_difference(rng, 7, 5), 11);
  const int a = is_ellipse ? ellipse_a : 11;
  const auto put = [&](int px, int py, int pz, std::string_view name) {
    return try_set(chunk, px, py, pz, name, "", false) ? 1u : 0u;
  };
  const auto set_block = [&](int px, int py, int pz, int h_diff, int height) {
    const BlockStateKey cur = block_at(chunk, px, py, pz);
    if (!is_air_key(cur) && cur.name != "minecraft:snow_block" && cur.name != "minecraft:ice" &&
        cur.name != "minecraft:water")
      return 0u;
    const bool random_flag = !is_ellipse || rng.next_double() > 0.05;
    const int divisor = is_ellipse ? 3 : 2;
    if (snow && cur.name != "minecraft:water" &&
        h_diff <= rng.next_int(std::max(1, height / divisor)) + static_cast<int>(height * 0.6) &&
        random_flag)
      return put(px, py, pz, "minecraft:snow_block");
    return put(px, py, pz, cfg.a.name);
  };
  const auto signed_circle = [&](int xo, int zo, int radius) {
    const float off = 10.0f * std::clamp(rng.next_float(), 0.2f, 0.8f) / radius;
    return static_cast<double>(off) + static_cast<double>(xo * xo + zo * zo) -
           static_cast<double>(radius * radius);
  };
  const auto signed_ellipse = [&](int xo, int zo, int ra, int rc) {
    const double c = std::cos(angle), s = std::sin(angle);
    const double xr = (xo * c - zo * s) / ra;
    const double zr = (xo * s + zo * c) / rc;
    return xr * xr + zr * zr - 1.0;
  };
  uint32_t placed = 0;
  for (int xo = -a; xo < a; ++xo)
    for (int zo = -a; zo < a; ++zo)
      for (int dy = 0; dy < over_height; ++dy) {
        int radius;
        if (is_ellipse) {
          const float k = 1.0f;
          const float scale =
              (1.0f - static_cast<float>(dy * dy) / (static_cast<float>(over_height) * k)) * width;
          radius = static_cast<int>(std::ceil(scale / 2.0f));
        } else {
          const float k = 3.5f - rng.next_float();
          float scale = (1.0f - static_cast<float>(dy * dy) / (over_height * k)) * width;
          if (over_height > 15 + rng.next_int(5)) {
            const int temp = dy < 3 + rng.next_int(6) ? dy / 2 : dy;
            scale = (1.0f - static_cast<float>(temp) / (over_height * k * 0.4f)) * width;
          }
          radius = static_cast<int>(std::ceil(scale / 2.0f));
        }
        if (!is_ellipse && xo >= radius)
          continue;
        double dist =
            is_ellipse
                ? signed_ellipse(xo, zo, a,
                                 ellipse_c -
                                     (dy > 0 && over_height - dy <= 3 ? 4 - (over_height - dy) : 0))
                : signed_circle(xo, zo, radius);
        if (dist >= 0.0)
          continue;
        const double cutoff = is_ellipse ? -0.5 : static_cast<double>(-6 - rng.next_int(3));
        if (dist > cutoff && rng.next_double() > 0.9)
          continue;
        placed += set_block(x + xo, y + dy, z + zo, over_height - dy, over_height);
      }
  for (int xo = -a; xo < a; ++xo)
    for (int zo = -a; zo < a; ++zo)
      for (int dy = -1; dy > -under_height; --dy) {
        const int new_a = is_ellipse
                              ? static_cast<int>(std::ceil(
                                    static_cast<float>(a) *
                                    (1.0f - static_cast<float>(dy * dy) / (under_height * 8.0f))))
                              : a;
        const float k = 1.0f + rng.next_float() / 2.0f;
        const float scale = (1.0f - static_cast<float>(-dy) / (under_height * k)) * width;
        const int radius = static_cast<int>(std::ceil(scale / 2.0f));
        if (xo >= radius)
          continue;
        const double dist =
            is_ellipse
                ? signed_ellipse(
                      xo, zo, new_a,
                      ellipse_c - (dy < 0 && under_height + dy <= 3 ? 4 - (under_height + dy) : 0))
                : signed_circle(xo, zo, radius);
        if (dist < 0.0)
          placed += set_block(x + xo, y + dy, z + zo, under_height + dy, under_height);
      }
  const bool cut_out = is_ellipse ? rng.next_double() > 0.1 : rng.next_double() > 0.7;
  if (cut_out) {
    const int sign_x = rng.next_boolean() ? -1 : 1;
    const int sign_z = rng.next_boolean() ? -1 : 1;
    const int max_off = std::max(width / 2 - 2, 1);
    int x_off = rng.next_int(max_off);
    if (rng.next_boolean())
      x_off = width / 2 + 1 - rng.next_int(std::max(width - width / 2 - 1, 1));
    int z_off = rng.next_int(max_off);
    if (rng.next_boolean())
      z_off = width / 2 + 1 - rng.next_int(std::max(width - width / 2 - 1, 1));
    if (is_ellipse) {
      const int both = rng.next_int(std::max(ellipse_a - 5, 1));
      x_off = both;
      z_off = both;
    }
    const int local_x = sign_x * x_off;
    const int local_z = sign_z * z_off;
    const double carve_angle =
        is_ellipse ? angle + 1.5707963267948966 : rng.next_double() * 2.0 * 3.14159265358979323846;
    const auto carve = [&](int radius, int dy, bool under) {
      const int ca = radius + 1 + ellipse_a / 3;
      const int cc = std::min(radius - 3, 3) + ellipse_c / 2 - 1;
      const double c = std::cos(carve_angle), s = std::sin(carve_angle);
      uint32_t carved = 0;
      for (int xo = -ca; xo < ca; ++xo)
        for (int zo = -ca; zo < ca; ++zo) {
          const double xr = ((xo - local_x) * c - (zo - local_z) * s) / ca;
          const double zr = ((xo - local_x) * s + (zo - local_z) * c) / cc;
          if (xr * xr + zr * zr >= 0.0)
            continue;
          const BlockStateKey cur = block_at(chunk, x + xo, y + dy, z + zo);
          if (!iceberg_state(cur.name) && cur.name != "minecraft:snow_block")
            continue;
          carved += put(x + xo, y + dy, z + zo, under ? "minecraft:water" : "minecraft:air");
          if (!under && block_at(chunk, x + xo, y + dy + 1, z + zo).name == "minecraft:snow")
            carved += put(x + xo, y + dy + 1, z + zo, "minecraft:air");
        }
      return carved;
    };
    for (int dy = 0; dy < over_height - 3; ++dy) {
      const float k = 3.5f - rng.next_float();
      const float scale = (1.0f - static_cast<float>(dy * dy) / (over_height * k)) * width;
      placed += carve(static_cast<int>(std::ceil(scale / 2.0f)), dy, false);
    }
    for (int dy = -1; dy > -over_height + rng.next_int(5); --dy) {
      const float k = 1.0f + rng.next_float() / 2.0f;
      const float scale = (1.0f - static_cast<float>(-dy) / (over_height * k)) * width;
      placed += carve(static_cast<int>(std::ceil(scale / 2.0f)), dy, true);
    }
  }
  // The smooth pass: drop the cells hanging over air and any iceberg cell with
  // three non-iceberg sides.
  const int smooth_a = is_ellipse ? ellipse_a : width / 2;
  for (int xo = -smooth_a; xo <= smooth_a; ++xo)
    for (int zo = -smooth_a; zo <= smooth_a; ++zo)
      for (int dy = 0; dy <= over_height; ++dy) {
        const BlockStateKey cur = block_at(chunk, x + xo, y + dy, z + zo);
        if (!iceberg_state(cur.name) && cur.name != "minecraft:snow")
          continue;
        if (is_air_key(block_at(chunk, x + xo, y + dy - 1, z + zo))) {
          placed += put(x + xo, y + dy, z + zo, "minecraft:air");
          placed += put(x + xo, y + dy + 1, z + zo, "minecraft:air");
          continue;
        }
        if (!iceberg_state(cur.name))
          continue;
        int non_ice = 0;
        static constexpr int kDx[4] = {0, 0, -1, 1};
        static constexpr int kDz[4] = {-1, 1, 0, 0};
        for (int f = 0; f < 4; ++f)
          if (!iceberg_state(block_at(chunk, x + xo + kDx[f], y + dy, z + zo + kDz[f]).name))
            ++non_ice;
        if (non_ice >= 3)
          placed += put(x + xo, y + dy, z + zo, "minecraft:air");
      }
  return placed;
}

// `VoidStartPlatformFeature`: the small obsidian island the void world starts
// on.
uint32_t place_void_start_platform(Chunk& chunk, int x, int y, int z) {
  uint32_t placed = 0;
  for (int dx = -2; dx <= 2; ++dx)
    for (int dy = -1; dy <= 0; ++dy)
      for (int dz = -2; dz <= 2; ++dz) {
        const bool edge = std::abs(dx) == 2 || std::abs(dz) == 2 || dy == -1;
        placed += try_set(chunk, x + dx, y + dy, z + dz,
                          edge ? "minecraft:obsidian" : "minecraft:air", "", false)
                      ? 1u
                      : 0u;
      }
  return placed;
}

uint32_t place_configured(const WgConfiguredFeature& cf, Chunk& chunk, WgRandom& rng,
                          int64_t world_seed, int x, int y, int z, int depth) {
  if (depth > kMaxDepth)
    return 0;
  switch (cf.kind) {
  case WgFeatureKind::kOre:
    if (cf.config == UINT32_MAX)
      return 0;
    return place_ore(chunk, kWgOreConfigs[cf.config], kWgOreTargets, rng, x, y, z);
  case WgFeatureKind::kSimpleBlock:
    if (cf.config == UINT32_MAX)
      return 0;
    return place_simple_block(chunk, kWgSimpleBlockConfigs[cf.config], x, y, z);
  case WgFeatureKind::kBlockPile:
    if (cf.config == UINT32_MAX)
      return 0;
    return place_block_pile(chunk, kWgBlockPileConfigs[cf.config], rng, x, y, z);
  case WgFeatureKind::kDisk:
    if (cf.config == UINT32_MAX)
      return 0;
    return place_disk(chunk, kWgDiskConfigs[cf.config], rng, x, y, z);
  case WgFeatureKind::kSpring:
    if (cf.config == UINT32_MAX)
      return 0;
    return place_spring(chunk, kWgSpringConfigs[cf.config], x, y, z);
  case WgFeatureKind::kScatteredOre:
    if (cf.config == UINT32_MAX)
      return 0;
    return place_scattered_ore(chunk, kWgOreConfigs[cf.config], kWgOreTargets, rng, x, y, z);
  case WgFeatureKind::kBlockColumn:
    if (cf.config == UINT32_MAX)
      return 0;
    return place_block_column(chunk, kWgColumnConfigs[cf.config], rng, x, y, z);
  case WgFeatureKind::kMonsterRoom:
    // The kind's config is `NoneFeatureConfiguration` (`{}` in the pack), so
    // there is no table to read and `config` is UINT32_MAX by design.
    return place_monster_room(chunk, rng, world_seed, x, y, z);
  case WgFeatureKind::kFreezeTopLayer:
    // Same shape of config (`{}`), same UINT32_MAX by design; it ignores the
    // origin's y and scans the whole chunk.
    return place_freeze_top_layer(chunk, x, z);
  case WgFeatureKind::kSeagrass:
    if (cf.config == UINT32_MAX)
      return 0;
    return place_seagrass(chunk, kWgSeagrassConfigs[cf.config], rng, x, y, z);
  case WgFeatureKind::kTree:
    if (cf.config == UINT32_MAX)
      return 0;
    return place_tree(chunk, kWgTreeConfigs[cf.config], rng, x, y, z);
  case WgFeatureKind::kFallenTree:
    if (cf.config == UINT32_MAX)
      return 0;
    return place_fallen_tree(chunk, kWgFallenTreeConfigs[cf.config], rng, x, y, z);
  case WgFeatureKind::kVines:
    // `NoneFeatureConfiguration`: no table row, so `config` is UINT32_MAX by
    // design (the monster room and freeze-top-layer shape).
    return place_vines(chunk, x, y, z);
  case WgFeatureKind::kMultifaceGrowth:
    if (cf.config == UINT32_MAX)
      return 0;
    return place_multiface_growth(chunk, kWgMultifaceGrowthConfigs[cf.config], rng, x, y, z);
  case WgFeatureKind::kRandomSelector:
  case WgFeatureKind::kSimpleRandomSelector:
  case WgFeatureKind::kRandomBooleanSelector:
    if (cf.config == UINT32_MAX)
      return 0;
    return place_selector(chunk, kWgSelectors[cf.config], rng, world_seed, x, y, z, depth);
  case WgFeatureKind::kBlockBlob: {
    if (cf.config == UINT32_MAX)
      return 0;
    const WgExtraConfig& cfg = extra_config(cf.config);
    return place_block_blob(chunk, cfg, rng, x, y, z);
  }
  case WgFeatureKind::kDesertWell:
    return place_desert_well(chunk, rng, x, y, z);
  case WgFeatureKind::kGlowstoneBlob:
    return place_glowstone_blob(chunk, rng, x, y, z);
  case WgFeatureKind::kBlueIce:
    return place_blue_ice(chunk, rng, x, y, z);
  case WgFeatureKind::kSpike:
    if (cf.config == UINT32_MAX)
      return 0;
    return place_ice_spike(chunk, extra_config(cf.config), rng, x, y, z);
  case WgFeatureKind::kGeode:
    if (cf.config == UINT32_MAX)
      return 0;
    return place_geode(chunk, extra_config(cf.config), rng, world_seed, x, y, z);
  case WgFeatureKind::kBonusChest:
    return place_bonus_chest(chunk, rng, x, z);
  case WgFeatureKind::kEndPlatform:
    return place_end_platform(chunk, x, y, z);
  case WgFeatureKind::kEndIsland:
    return place_end_island(chunk, rng, x, y, z);
  case WgFeatureKind::kEndGateway:
    return place_end_gateway(chunk, x, y, z);
  case WgFeatureKind::kEndSpike:
    return place_end_spike(chunk, world_seed, x, y, z);
  case WgFeatureKind::kChorusPlant:
    return place_chorus_plant(chunk, rng, x, y, z);
  case WgFeatureKind::kKelp:
    return place_kelp(chunk, rng, x, y, z);
  case WgFeatureKind::kSeaPickle: {
    if (cf.config == UINT32_MAX)
      return 0;
    return place_sea_pickle(chunk, extra_config(cf.config), rng, x, y, z);
  }
  case WgFeatureKind::kBasaltPillar:
    return place_basalt_pillar(chunk, rng, x, y, z);
  case WgFeatureKind::kBasaltColumns: {
    if (cf.config == UINT32_MAX)
      return 0;
    return place_basalt_columns(chunk, extra_config(cf.config), rng, x, y, z);
  }
  case WgFeatureKind::kReplaceBlobs: {
    if (cf.config == UINT32_MAX)
      return 0;
    return place_replace_blobs(chunk, extra_config(cf.config), rng, x, y, z);
  }
  case WgFeatureKind::kDelta: {
    if (cf.config == UINT32_MAX)
      return 0;
    return place_delta(chunk, extra_config(cf.config), rng, x, y, z);
  }
  case WgFeatureKind::kNetherForestVegetation: {
    if (cf.config == UINT32_MAX)
      return 0;
    return place_nether_forest_vegetation(chunk, extra_config(cf.config), rng, x, y, z);
  }
  case WgFeatureKind::kTwistingVines: {
    if (cf.config == UINT32_MAX)
      return 0;
    return place_twisting_vines(chunk, extra_config(cf.config), rng, x, y, z);
  }
  case WgFeatureKind::kWeepingVines:
    return place_weeping_vines(chunk, rng, x, y, z);
  case WgFeatureKind::kBamboo: {
    if (cf.config == UINT32_MAX)
      return 0;
    return place_bamboo(chunk, extra_config(cf.config), rng, x, y, z);
  }
  case WgFeatureKind::kIceberg: {
    if (cf.config == UINT32_MAX)
      return 0;
    return place_iceberg(chunk, extra_config(cf.config), rng, x, y, z);
  }
  case WgFeatureKind::kLake: {
    if (cf.config == UINT32_MAX)
      return 0;
    return place_lake(chunk, extra_config(cf.config), rng, x, y, z);
  }
  case WgFeatureKind::kVegetationPatch: {
    if (cf.config == UINT32_MAX)
      return 0;
    const WgExtraConfig& cfg = extra_config(cf.config);
    return place_vegetation_patch(chunk, cfg, rng, world_seed, x, y, z, depth,
                                  extra_int(cfg, 6) != 0);
  }
  case WgFeatureKind::kSculkPatch: {
    if (cf.config == UINT32_MAX)
      return 0;
    return place_sculk_patch(chunk, extra_config(cf.config), rng, x, y, z);
  }
  case WgFeatureKind::kRootSystem: {
    if (cf.config == UINT32_MAX)
      return 0;
    return place_root_system(chunk, extra_config(cf.config), rng, world_seed, x, y, z, depth);
  }
  case WgFeatureKind::kUnderwaterMagma: {
    if (cf.config == UINT32_MAX)
      return 0;
    return place_underwater_magma(chunk, extra_config(cf.config), rng, x, y, z);
  }
  case WgFeatureKind::kSpeleothem: {
    if (cf.config == UINT32_MAX)
      return 0;
    return place_speleothem(chunk, extra_config(cf.config), rng, x, y, z);
  }
  case WgFeatureKind::kSpeleothemCluster: {
    if (cf.config == UINT32_MAX)
      return 0;
    return place_speleothem_cluster(chunk, extra_config(cf.config), rng, x, y, z);
  }
  case WgFeatureKind::kLargeDripstone: {
    if (cf.config == UINT32_MAX)
      return 0;
    return place_large_dripstone(chunk, extra_config(cf.config), rng, x, y, z);
  }
  case WgFeatureKind::kHugeMushroom: {
    if (cf.config == UINT32_MAX)
      return 0;
    return place_huge_mushroom(chunk, extra_config(cf.config), rng, x, y, z);
  }
  case WgFeatureKind::kHugeFungus: {
    if (cf.config == UINT32_MAX)
      return 0;
    return place_huge_fungus(chunk, extra_config(cf.config), rng, x, y, z);
  }
  case WgFeatureKind::kCoral: {
    if (cf.config == UINT32_MAX)
      return 0;
    return place_coral(chunk, extra_config(cf.config), rng, x, y, z);
  }
  case WgFeatureKind::kSequence: {
    if (cf.config == UINT32_MAX)
      return 0;
    return place_sequence(chunk, extra_config(cf.config), rng, world_seed, x, y, z, depth);
  }
  case WgFeatureKind::kWeightedSelector: {
    if (cf.config == UINT32_MAX)
      return 0;
    return place_weighted_selector(chunk, extra_config(cf.config), rng, world_seed, x, y, z, depth);
  }
  case WgFeatureKind::kVoidStartPlatform:
    return place_void_start_platform(chunk, x, y, z);
  case WgFeatureKind::kFossil:
    if (cf.config == UINT32_MAX)
      return 0;
    return place_fossil(chunk, kWgFossilConfigs[cf.config], rng, x, y, z);
  default:
    return 0;
  }
}

uint32_t place_placed(const WgPlacedFeature& placed, Chunk& chunk, WgRandom& rng,
                      int64_t world_seed, int x, int y, int z, int depth) {
  if (depth > kMaxDepth)
    return 0;

  const WgConfiguredFeature* cf =
      find_row(kWgConfiguredFeatures, std::size(kWgConfiguredFeatures), placed.feature);
  if (cf == nullptr)
    return 0;
  std::vector<Pos> positions;
  positions.push_back(Pos{x, y, z});
  if (!run_placement(chunk, kWgPlacements + placed.placement_offset, placed.placement_count,
                     placed.name, rng, positions))
    return 0;
  uint32_t placed_blocks = 0;
  for (const Pos& p : positions) {
    if (!in_chunk(chunk, p.x, p.z))
      continue;
    placed_blocks += place_configured(*cf, chunk, rng, world_seed, p.x, p.y, p.z, depth + 1);
  }
  return placed_blocks;
}

// ---- vanilla decoration order and seeding ---------------------------------
//
// `ChunkGenerator.applyBiomeDecoration` does not walk a biome's own list in
// pack order. The generator's `featuresPerStep` is `FeatureSorter`'s global
// topological order, built once from the biome source's possible biomes, and a
// feature's position in that order is the `index` that
// `WorldgenRandom.setFeatureSeed(decorationSeed, index, step)` salts its stream
// with. The order is therefore part of the generated world, not a scheduling
// detail: changing it moves every ore vein and every tree.
//
// This is `FeatureSorter.buildFeaturesPerStep` ported from the 26.2 bytecode.
// Discovery assigns each placed feature a global index in biome-list order.
// Vanilla then builds **one** graph for the whole dimension: a biome's entries
// are flattened in step order, and every entry gets an edge to the next entry,
// including across a step boundary (the last feature of step 6 precedes the
// first of step 7). The graph is walked depth-first from its sorted node keys
// (global index, then step), the postorder is reversed, and the result is
// filtered by step. Keeping a graph per step instead changes the topological
// order and therefore every `setFeatureSeed` index; the port did that before
// and its ore veins landed in visibly different places from vanilla.
// `kPossibleBiomes*` below is the exact order 26.2's
// `biomeSource.possibleBiomes()` yields for each dimension (dumped from the
// server jar), because discovery order is what breaks ties in the sorter.

constexpr const char* kOverworldPossibleBiomes[] = {
    "minecraft:mushroom_fields",
    "minecraft:deep_frozen_ocean",
    "minecraft:frozen_ocean",
    "minecraft:deep_cold_ocean",
    "minecraft:cold_ocean",
    "minecraft:deep_ocean",
    "minecraft:ocean",
    "minecraft:deep_lukewarm_ocean",
    "minecraft:lukewarm_ocean",
    "minecraft:warm_ocean",
    "minecraft:stony_shore",
    "minecraft:swamp",
    "minecraft:mangrove_swamp",
    "minecraft:snowy_slopes",
    "minecraft:snowy_plains",
    "minecraft:snowy_beach",
    "minecraft:windswept_gravelly_hills",
    "minecraft:grove",
    "minecraft:windswept_hills",
    "minecraft:snowy_taiga",
    "minecraft:windswept_forest",
    "minecraft:taiga",
    "minecraft:plains",
    "minecraft:meadow",
    "minecraft:beach",
    "minecraft:forest",
    "minecraft:old_growth_spruce_taiga",
    "minecraft:flower_forest",
    "minecraft:birch_forest",
    "minecraft:dark_forest",
    "minecraft:pale_garden",
    "minecraft:savanna_plateau",
    "minecraft:savanna",
    "minecraft:jungle",
    "minecraft:badlands",
    "minecraft:desert",
    "minecraft:wooded_badlands",
    "minecraft:jagged_peaks",
    "minecraft:stony_peaks",
    "minecraft:frozen_river",
    "minecraft:river",
    "minecraft:ice_spikes",
    "minecraft:old_growth_pine_taiga",
    "minecraft:sunflower_plains",
    "minecraft:old_growth_birch_forest",
    "minecraft:sparse_jungle",
    "minecraft:bamboo_jungle",
    "minecraft:eroded_badlands",
    "minecraft:windswept_savanna",
    "minecraft:cherry_grove",
    "minecraft:frozen_peaks",
    "minecraft:dripstone_caves",
    "minecraft:lush_caves",
    "minecraft:sulfur_caves",
    "minecraft:deep_dark",
};
constexpr const char* kNetherPossibleBiomes[] = {
    "minecraft:nether_wastes", "minecraft:soul_sand_valley", "minecraft:crimson_forest",
    "minecraft:warped_forest", "minecraft:basalt_deltas",
};
constexpr const char* kEndPossibleBiomes[] = {
    "minecraft:the_end",           "minecraft:end_highlands", "minecraft:end_midlands",
    "minecraft:small_end_islands", "minecraft:end_barrens",
};

struct WgFeatureOrder {
  // Rows in `kWgPlacedFeatures`, in vanilla's per-step `features()` order.
  std::vector<uint32_t> steps[kSteps];
};

WgFeatureOrder build_feature_order(WgDimensionKind dimension) {
  const char* const* biome_names = kOverworldPossibleBiomes;
  size_t biome_count = std::size(kOverworldPossibleBiomes);
  if (dimension == WgDimensionKind::kNether) {
    biome_names = kNetherPossibleBiomes;
    biome_count = std::size(kNetherPossibleBiomes);
  } else if (dimension == WgDimensionKind::kEnd) {
    biome_names = kEndPossibleBiomes;
    biome_count = std::size(kEndPossibleBiomes);
  }

  // `FeatureSorter$1FeatureData`'s two sort keys: the global discovery index
  // first, then the step. Two occurrences of one placed feature in different
  // steps are distinct nodes, so the step is part of the key.
  struct Node {
    int32_t feature_index = 0;
    uint32_t step = 0;
    uint32_t row = 0;
  };
  using Key = std::pair<int32_t, uint32_t>;
  const auto key_of = [](const Node& node) { return Key{node.feature_index, node.step}; };
  const auto row_of_key = [](const Key& key, const std::map<Key, Node>& nodes) {
    const auto found = nodes.find(key);
    return found == nodes.end() ? UINT32_MAX : found->second.row;
  };

  std::map<int32_t, int32_t> global_index; // placed row -> discovery index
  std::map<Key, Node> nodes;
  std::map<Key, std::set<Key>> edges;
  int32_t next_index = 0;

  for (size_t b = 0; b < biome_count; ++b) {
    const WgBiomeFeatures* biome = find_biome(biome_names[b]);
    if (biome == nullptr)
      continue;
    // `BiomeGenerationSettings.features()` is a list of per-step holder sets;
    // flatten it in step order. The table preserves the pack's order within
    // each step, so iterating the rows by step reproduces that list.
    std::vector<Node> biome_nodes;
    for (uint32_t step = 0; step < kSteps; ++step) {
      for (uint32_t i = 0; i < biome->count; ++i) {
        const WgBiomeFeatureEntry& entry = kWgBiomeFeatureEntries[biome->offset + i];
        if (entry.step != step)
          continue;
        const WgPlacedFeature* placed =
            find_row(kWgPlacedFeatures, std::size(kWgPlacedFeatures), entry.placed_feature);
        if (placed == nullptr)
          continue;
        const uint32_t row = static_cast<uint32_t>(placed - kWgPlacedFeatures);
        auto found = global_index.find(static_cast<int32_t>(row));
        if (found == global_index.end())
          found = global_index.emplace(static_cast<int32_t>(row), next_index++).first;
        const Node node{found->second, step, row};
        biome_nodes.push_back(node);
        nodes.emplace(key_of(node), node);
      }
    }
    // `FeatureSorter` calls `computeIfAbsent` for every entry, including the
    // last one in a biome and a biome with a single feature. Creating the node
    // only when it has a successor would drop an isolated feature from the
    // order (the End's `end_island_decorated` is exactly that case).
    for (size_t i = 0; i < biome_nodes.size(); ++i) {
      const Key key = key_of(biome_nodes[i]);
      edges.try_emplace(key);
      if (i + 1 < biome_nodes.size())
        edges[key].insert(key_of(biome_nodes[i + 1]));
    }
  }

  // `Graph.depthFirstSearch` over the one graph, in the graph's sorted key
  // order. Successors are a sorted set (the bytecode's TreeSet), so recurse in
  // key order rather than in the order edges happened to be discovered.
  std::set<Key> done;
  std::set<Key> visiting;
  std::vector<Key> postorder;
  std::function<void(const Key&)> visit = [&](const Key& key) {
    if (done.count(key) != 0 || visiting.count(key) != 0)
      return;
    visiting.insert(key);
    const auto it = edges.find(key);
    if (it != edges.end()) {
      for (const Key& next : it->second)
        visit(next);
    }
    visiting.erase(key);
    done.insert(key);
    postorder.push_back(key);
  };
  for (const auto& [key, node] : nodes) {
    (void)node;
    visit(key);
  }

  std::reverse(postorder.begin(), postorder.end());
  WgFeatureOrder order;
  for (uint32_t step = 0; step < kSteps; ++step) {
    for (const Key& key : postorder) {
      const auto it = nodes.find(key);
      if (it == nodes.end() || it->second.step != step)
        continue;
      const uint32_t row = row_of_key(key, nodes);
      if (row != UINT32_MAX)
        order.steps[step].push_back(row);
    }
  }
  return order;
}

const WgFeatureOrder& feature_order(WgDimensionKind dimension) {
  static const WgFeatureOrder overworld = build_feature_order(WgDimensionKind::kOverworld);
  static const WgFeatureOrder nether = build_feature_order(WgDimensionKind::kNether);
  static const WgFeatureOrder end = build_feature_order(WgDimensionKind::kEnd);
  switch (dimension) {
  case WgDimensionKind::kNether:
    return nether;
  case WgDimensionKind::kEnd:
    return end;
  case WgDimensionKind::kOverworld:
  default:
    return overworld;
  }
}

// `WorldgenRandom.setDecorationSeed(seed, blockX, blockZ)`: the two longs are
// `| 1`, the block coordinates are multiplied as signed longs, and the seed is
// xor'd back in after the sum. `WorldgenRandom.setFeatureSeed(seed, index,
// step)` (below) is `seed + index + 10000 * step`.
int64_t decoration_seed(int64_t world_seed, int32_t block_x, int32_t block_z) {
  WgRandom random(world_seed, WgRandomMode::kWorldgen);
  const uint64_t x_factor = random.next_long() | 1ull;
  const uint64_t z_factor = random.next_long() | 1ull;
  const uint64_t combined =
      static_cast<uint64_t>(block_x) * x_factor + static_cast<uint64_t>(block_z) * z_factor;
  return static_cast<int64_t>(combined ^ static_cast<uint64_t>(world_seed));
}

} // namespace

uint32_t wg_feature_order_length(WgDimensionKind dimension, uint32_t step) {
  const WgFeatureOrder& order = feature_order(dimension);
  return step < kSteps ? static_cast<uint32_t>(order.steps[step].size()) : 0;
}

const char* wg_feature_order_feature(WgDimensionKind dimension, uint32_t step, uint32_t index) {
  const WgFeatureOrder& order = feature_order(dimension);
  if (step >= kSteps || index >= order.steps[step].size())
    return nullptr;
  return kWgPlacedFeatures[order.steps[step][index]].name;
}

uint32_t wg_place_placed_feature_at(Chunk& chunk, const char* placed_name, WgRandom& rng,
                                    int64_t world_seed, int x, int y, int z) {
  const WgPlacedFeature* placed =
      find_row(kWgPlacedFeatures, std::size(kWgPlacedFeatures), placed_name);
  if (placed == nullptr)
    return 0;
  return place_placed(*placed, chunk, rng, world_seed, x, y, z, 0);
}

void decorate_generated_chunk(const WgOverworldGenerator& generator, Chunk& chunk,
                              WgDecorationStats* stats, Chunk* const* region) {
  const WgDensityTable& table = generator.table();

  // Install the optional 3x3 read/write region for the whole feature stage.
  // Every helper below (`block_at`, `biome_at`, `try_set`, `in_chunk`) is
  // region-aware, so the individual feature implementations keep their existing
  // signatures while gaining the server's `WorldGenRegion` behaviour.
  Chunk* const* const previous_region = g_feature_region;
  g_feature_region = region;
  struct RegionRestore {
    Chunk* const* previous;
    ~RegionRestore() {
      g_feature_region = previous;
    }
  } region_restore{previous_region};

  // Vanilla gathers the biome *set* of the chunk and its eight neighbours, then
  // for each step unions the indices those biomes list and runs them in index
  // order. The per-chunk part is what this stage has: its own section palettes.
  bool selected[kSteps][std::size(kWgPlacedFeatures)] = {};
  std::vector<std::string> unique_biomes;
  unique_biomes.reserve(32);
  const auto add_biome = [&](const char* name) {
    if (name == nullptr || *name == '\0')
      return;
    for (const std::string& known : unique_biomes)
      if (known == name)
        return;
    unique_biomes.emplace_back(name);
  };
  for (const Section& s : chunk.sections) {
    if (!s.has_biomes)
      continue;
    for (const std::string& biome : s.biome_palette)
      add_biome(biome.c_str());
  }
  // `ChunkGenerator.applyBiomeDecoration` gathers the biomes of the chunk and
  // its eight neighbours before it chooses features. A feature listed only by
  // a neighbour still runs in this chunk; using this chunk's palettes alone
  // silently drops every such feature. `fill_biome_chunk` is the pure biome
  // source, so this costs climate samples but no terrain generation.
  std::vector<const char*> neighbour_biomes(static_cast<size_t>(table.height / 16) *
                                            kBiomeCellsPerSection);
  for (int dz = -1; dz <= 1; ++dz) {
    for (int dx = -1; dx <= 1; ++dx) {
      if (dx == 0 && dz == 0)
        continue;
      generator.fill_biome_chunk(chunk.x + dx, chunk.z + dz, neighbour_biomes.data());
      for (const char* name : neighbour_biomes)
        add_biome(name);
    }
  }
  for (const std::string& biome : unique_biomes) {
    const WgBiomeFeatures* row = find_biome(biome.c_str());
    if (row == nullptr)
      continue;
    for (uint32_t i = 0; i < row->count; ++i) {
      const WgBiomeFeatureEntry& entry = kWgBiomeFeatureEntries[row->offset + i];
      if (entry.step >= kSteps)
        continue;
      const WgPlacedFeature* placed =
          find_row(kWgPlacedFeatures, std::size(kWgPlacedFeatures), entry.placed_feature);
      if (placed != nullptr)
        selected[entry.step][static_cast<size_t>(placed - kWgPlacedFeatures)] = true;
    }
  }

  const WgFeatureOrder& order = feature_order(table.dimension);
  const int32_t block_x = chunk.x * 16;
  const int32_t block_z = chunk.z * 16;
  const int64_t decoration_seed_value = decoration_seed(generator.seed(), block_x, block_z);

  for (uint32_t step = 0; step < kSteps; ++step) {
    if (stats != nullptr)
      stats->steps = step + 1;
    const std::vector<uint32_t>& step_order = order.steps[step];
    for (size_t index = 0; index < step_order.size(); ++index) {
      const WgPlacedFeature& placed = kWgPlacedFeatures[step_order[index]];
      if (!selected[step][step_order[index]])
        continue;
      const WgConfiguredFeature* configured =
          find_row(kWgConfiguredFeatures, std::size(kWgConfiguredFeatures), placed.feature);
      if (configured == nullptr)
        continue;
      if (stats != nullptr)
        ++stats->considered;
      if (configured->kind == WgFeatureKind::kOther ||
          (kind_has_config(configured->kind) && configured->config == UINT32_MAX)) {
        if (stats != nullptr)
          ++stats->skipped_kind;
        continue;
      }
      // `WorldgenRandom.setFeatureSeed`: the step's `index` is the feature's
      // position in the order above, not its position in this biome's list.
      WgRandom rng(decoration_seed_value + static_cast<int64_t>(index) +
                       10000 * static_cast<int64_t>(step),
                   WgRandomMode::kWorldgen);
      // Vanilla consumes the lazy stream one position at a time: the vein's own
      // draws happen before the next position's modifiers are sampled. Placing
      // after the whole chain instead gives position n the draws position n-1's
      // body should have taken.
      const bool chain_ok = run_placement_stream(
          chunk, kWgPlacements + placed.placement_offset, placed.placement_count, 0, placed.name,
          rng, Pos{block_x, table.min_y, block_z}, [&](const Pos& p) {
            if (!in_chunk(chunk, p.x, p.z)) {
              if (stats != nullptr)
                ++stats->out_of_chunk;
              return true;
            }
            if (stats != nullptr)
              ++stats->positions;
            const uint32_t placed_blocks =
                place_configured(*configured, chunk, rng, generator.seed(), p.x, p.y, p.z, 0);
            if (stats != nullptr)
              stats->blocks_placed += placed_blocks;
            return true;
          });
      if (!chain_ok) {
        if (stats != nullptr)
          ++stats->skipped_modifier;
        continue;
      }
      if (stats != nullptr) {
        stats->applied += 1;
        stats->applied_by_kind[static_cast<uint32_t>(configured->kind)] += 1;
      }
    }
  }

  // The stage ends where vanilla's does not: the trees have landed, so the leaf
  // distances the block updates would have walked out from each trunk are set
  // here (`world/leaves.h`). A canopy left at the registry default is a canopy
  // the random tick eats, which is what this stage used to hand the world.
  // Structure pieces come after this function and can write wood or leaves of
  // their own, so the column's own pass runs again once they have
  // (`world/generated_chunk.cpp`).
  if (region == nullptr)
    settle_generated_leaf_distances(chunk);
}

WgDecorationStream wg_decoration_stream(WgDimensionKind dimension) {
  return dimension == WgDimensionKind::kOverworld ? WgDecorationStream::kLegacy
                                                  : WgDecorationStream::kRegionAware;
}

// The served Overworld column keeps the pre-`FeatureSorter` stream: a per-biome
// step list and a per-feature named random, not vanilla's global order and
// `setDecorationSeed`. The region-aware path above is what dimensions use; the
// pinned served-world parity in tests/vanilla_parity_test.py is measured on
// this stream, and changing it is a served-world change. Kept here rather than
// in a second copy of the feature stage so both paths share the block writers.
void decorate_generated_chunk_legacy(const WgOverworldGenerator& generator, Chunk& chunk,
                                     WgDecorationStats* stats) {
  // The placed features the chunk's biomes list, per decoration step, in step
  // order with duplicates removed (vanilla's `featuresPerStep`).
  std::vector<const char*> per_step[kSteps];
  auto add_to_step = [&](uint32_t step, const char* name) {
    if (step >= kSteps)
      return;
    std::vector<const char*>& list = per_step[step];
    for (const char* existing : list)
      if (std::strcmp(existing, name) == 0)
        return;
    list.push_back(name);
  };
  // The biome palettes are merged once, not once per section: a full column is
  // 24 sections whose palettes repeat the same handful of biome names, so the
  // old loop asked `find_biome` and walked every feature for each repetition.
  // The per-step dedup below still keeps the exact same feature order.
  std::vector<const std::string*> unique_biomes;
  unique_biomes.reserve(8);
  for (const Section& s : chunk.sections) {
    if (!s.has_biomes)
      continue;
    for (const std::string& biome : s.biome_palette) {
      bool seen = false;
      for (const std::string* known : unique_biomes)
        if (*known == biome) {
          seen = true;
          break;
        }
      if (!seen)
        unique_biomes.push_back(&biome);
    }
  }
  for (const std::string* biome : unique_biomes) {
    const WgBiomeFeatures* row = find_biome(biome->c_str());
    if (row == nullptr)
      continue;
    for (uint32_t i = 0; i < row->count; ++i) {
      const WgBiomeFeatureEntry& entry = kWgBiomeFeatureEntries[row->offset + i];
      add_to_step(entry.step, entry.placed_feature);
    }
  }

  for (uint32_t step = 0; step < kSteps; ++step) {
    if (stats != nullptr)
      stats->steps = step + 1;
    for (const char* placed_name : per_step[step]) {
      const WgPlacedFeature* placed =
          find_row(kWgPlacedFeatures, std::size(kWgPlacedFeatures), placed_name);
      if (placed == nullptr)
        continue;
      const WgConfiguredFeature* configured =
          find_row(kWgConfiguredFeatures, std::size(kWgConfiguredFeatures), placed->feature);
      if (configured == nullptr)
        continue;
      if (stats != nullptr)
        ++stats->considered;
      if (configured->kind == WgFeatureKind::kOther ||
          (kind_has_config(configured->kind) && configured->config == UINT32_MAX)) {
        if (stats != nullptr)
          ++stats->skipped_kind;
        continue;
      }
      // A per-feature, per-step, per-chunk stream. Not vanilla's sequence (the
      // header names that), but deterministic and independent per feature.
      WgRandom rng = generator.state()
                         .named_random(placed->name)
                         .at(chunk.x, static_cast<int32_t>(step), chunk.z);
      std::vector<Pos> positions;
      positions.push_back(Pos{chunk.x * 16, 0, chunk.z * 16});
      if (!run_placement(chunk, kWgPlacements + placed->placement_offset, placed->placement_count,
                         placed->name, rng, positions)) {
        if (stats != nullptr)
          ++stats->skipped_modifier;
        continue;
      }
      if (stats != nullptr) {
        stats->applied += 1;
        stats->applied_by_kind[static_cast<uint32_t>(configured->kind)] += 1;
      }
      for (const Pos& p : positions) {
        if (!in_chunk(chunk, p.x, p.z)) {
          if (stats != nullptr)
            ++stats->out_of_chunk;
          continue;
        }
        if (stats != nullptr)
          ++stats->positions;
        const uint32_t placed_blocks =
            place_configured(*configured, chunk, rng, generator.seed(), p.x, p.y, p.z, 0);
        if (stats != nullptr)
          stats->blocks_placed += placed_blocks;
      }
    }
  }

  // The stage ends where vanilla's does not: the trees have landed, so the leaf
  // distances the block updates would have walked out from each trunk are set
  // here (`world/leaves.h`). A canopy left at the registry default is a canopy
  // the random tick eats, which is what this stage used to hand the world.
  // Structure pieces come after this function and can write wood or leaves of
  // their own, so the column's own pass runs again once they have
  // (`world/generated_chunk.cpp`).
  settle_generated_leaf_distances(chunk);
}

} // namespace mc::world
