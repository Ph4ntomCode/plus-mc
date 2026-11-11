// SPDX-License-Identifier: GPL-3.0-or-later
//
// The light engine (Phase 4 milestone A). See world/light.h for what is modelled
// and where the boundary sits; this file is the tables and the flood.
#include "world/light.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "world/block_states.h"
#include "world/registry.h"

namespace mc::world {
namespace {

// The light region is the 3x3 chunks around the one being computed: 15 blocks
// of reach fit inside it, which is why vanilla keeps the same shape.
constexpr int kRegionWidth = 48;

// ---- the canonical property string ------------------------------------------
//
// `BlockStateKey::properties` is the sorted "k=v,k=v" form the chunk parser
// normalizes to, so a lookup is a substring scan for "k=" at a key boundary.
std::string_view property_value(std::string_view props, std::string_view key) {
  size_t pos = 0;
  while (pos < props.size()) {
    const size_t end = props.find(',', pos);
    const std::string_view entry =
        props.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
    const size_t eq = entry.find('=');
    if (eq != std::string_view::npos && entry.substr(0, eq) == key)
      return entry.substr(eq + 1);
    if (end == std::string_view::npos)
      break;
    pos = end + 1;
  }
  return {};
}

bool property_is_true(std::string_view props, std::string_view key) {
  return property_value(props, key) == "true";
}

int property_int(std::string_view props, std::string_view key, int fallback) {
  const std::string_view v = property_value(props, key);
  if (v.empty())
    return fallback;
  int value = 0;
  for (const char c : v) {
    if (c < '0' || c > '9')
      return fallback;
    value = value * 10 + (c - '0');
  }
  return value;
}

bool ends_with(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Vanilla `LeavesBlock` (all "*_leaves") and the two fluid blocks, which the
// generated tables deliberately do not classify: leaves are `noOcclusion()` yet
// still cost a level (`LeavesBlock.getLightDampening` is 1), and water and lava
// are collision-less fluids (`LiquidBlock.propagatesSkylightDown` is false).
bool is_leaves(std::string_view name) {
  return ends_with(name, "_leaves");
}

bool is_fluid(std::string_view name) {
  return name == "minecraft:water" || name == "minecraft:lava";
}

// ---- emission ---------------------------------------------------------------
//
// Transcribed from the decompiled 26.2 `Blocks.java`, where each block is
// registered with a `lightLevel(...)`; the `lit`-conditional rows are
// `Blocks.litBlockEmission(n)` (`state.getValue(LIT) != false ? n : 0`) and the
// scaled ones are the state-shaped forms named in `light_emission` below.
//
// A row belongs here only if its level is the *state-independent* one. Partly
// conditional rows live in `light_emission`: `CaveVines.emission(14)`,
// `GlowLichenBlock.emission(7)`, `TrialSpawnerState.lightLevel()`,
// `VaultState.LightLevel`, the lightLevels that read one integer property
// (`LightBlock`, `RespawnAnchorBlock.getScaledChargeLevel`), and the four
// families whose level is a formula over a property (candles, sea pickles,
// candle cakes, copper bulbs).
struct Emission {
  const char* name;
  int level;
};

// A constant `lightLevel(state -> n)`.
constexpr Emission kEmission[] = {
    {"minecraft:amethyst_cluster", 5},
    {"minecraft:beacon", 15},
    {"minecraft:brewing_stand", 1},
    {"minecraft:brown_mushroom", 1},
    {"minecraft:conduit", 15},
    {"minecraft:crying_obsidian", 10},
    {"minecraft:dragon_egg", 1},
    {"minecraft:enchanting_table", 7},
    {"minecraft:end_gateway", 15},
    {"minecraft:end_portal", 15},
    {"minecraft:end_portal_frame", 1},
    {"minecraft:end_rod", 14},
    {"minecraft:fire", 15},
    {"minecraft:firefly_bush", 2},
    {"minecraft:glowstone", 15},
    {"minecraft:jack_o_lantern", 15},
    {"minecraft:large_amethyst_bud", 4},
    {"minecraft:lava", 15},
    {"minecraft:lava_cauldron", 15},
    {"minecraft:magma_block", 3},
    {"minecraft:medium_amethyst_bud", 2},
    {"minecraft:nether_portal", 11},
    {"minecraft:ochre_froglight", 15},
    {"minecraft:pearlescent_froglight", 15},
    {"minecraft:sculk_catalyst", 6},
    {"minecraft:sculk_sensor", 1},
    {"minecraft:sea_lantern", 15},
    {"minecraft:shroomlight", 15},
    {"minecraft:small_amethyst_bud", 1},
    {"minecraft:soul_fire", 10},
    {"minecraft:soul_lantern", 10},
    {"minecraft:soul_torch", 10},
    {"minecraft:soul_wall_torch", 10},
    {"minecraft:torch", 14},
    {"minecraft:verdant_froglight", 15},
    {"minecraft:wall_torch", 14},
    {"minecraft:copper_torch", 14},
    {"minecraft:copper_wall_torch", 14},
};

// `Blocks.litBlockEmission(n)`: emission only while the `lit` property is true.
constexpr Emission kEmissionLit[] = {
    {"minecraft:blast_furnace", 13},
    {"minecraft:campfire", 15},
    {"minecraft:candle_cake", 3},
    {"minecraft:deepslate_redstone_ore", 9},
    {"minecraft:furnace", 13},
    {"minecraft:redstone_lamp", 15},
    {"minecraft:redstone_ore", 9},
    {"minecraft:redstone_torch", 7},
    {"minecraft:redstone_wall_torch", 7},
    {"minecraft:smoker", 13},
    {"minecraft:soul_campfire", 10},
};

// The candle family is registered through a property factory
// (`Blocks.candleProperties`), one block per dye, all with
// `lightLevel(CandleBlock.LIGHT_EMISSION)`:
//
//     lit ? 3 * candles : 0
//
// so `candles=1..4` is 3, 6, 9, 12.
int candle_emission(const BlockStateKey& state) {
  if (!property_is_true(state.properties, "lit"))
    return 0;
  return 3 * property_int(state.properties, "candles", 1);
}

// `SeaPickleBlock`: `isDead ? 0 : 3 + 3 * pickles`.
int sea_pickle_emission(const BlockStateKey& state) {
  if (property_is_true(state.properties, "dead"))
    return 0;
  return 3 + 3 * property_int(state.properties, "pickles", 1);
}

// `LightBlock.LIGHT_EMISSION`: the `level` property, 0..15.
int light_block_emission(const BlockStateKey& state) {
  return std::clamp(property_int(state.properties, "level", 15), 0, kMaxLightLevel);
}

// `RespawnAnchorBlock.getScaledChargeLevel(state, 15)`: `floor(charge / 4 * 15)`
// — 0, 3, 7, 11, 15.
int respawn_anchor_emission(const BlockStateKey& state) {
  return property_int(state.properties, "charge", 0) * 15 / 4;
}

// `CaveVines.emission(14)`: `state -> state.getValue(BERRIES) != false ? 14 : 0`.
// Both halves of the plant carry the property — the head (`cave_vines`, with an
// `age`) and the body (`cave_vines_plant`) — and only a cell carrying berries
// emits. A cave's glow comes from the berries, which is why a picked vine is
// dark: the fixture chunk caught exactly that (berries=false cells the table
// called 14 while the disk said 8..13 from their neighbours).
int cave_vines_emission(const BlockStateKey& state) {
  return property_is_true(state.properties, "berries") ? 14 : 0;
}

// `MultifaceBlock.hasAnyFace(state)`: at least one of the six faces is true.
bool multiface_has_any_face(const BlockStateKey& state) {
  for (const char* face : {"down", "up", "north", "south", "west", "east"})
    if (property_is_true(state.properties, face))
      return true;
  return false;
}

// `GlowLichenBlock.emission(7)`: `hasAnyFace(state) ? 7 : 0`. The 64-face
// blockstate has 63 members that emit and one (no face at all) that cannot be
// placed and does not.
int glow_lichen_emission(const BlockStateKey& state) {
  return multiface_has_any_face(state) ? 7 : 0;
}

// `TrialSpawnerState.lightLevel()`: `inactive` and `cooldown` are 0,
// `waiting_for_players` is 4 and the three states that have a mob out are 8.
int trial_spawner_emission(const BlockStateKey& state) {
  const std::string_view value = property_value(state.properties, "trial_spawner_state");
  if (value == "waiting_for_players")
    return 4;
  if (value == "active" || value == "waiting_for_reward_ejection" || value == "ejecting_reward")
    return 8;
  return 0; // inactive, cooldown, or a state this code does not know
}

// `VaultState.LightLevel`: `HALF_LIT(6)` while inactive, `LIT(12)` once it is
// working (`active`, `unlocking`, `ejecting`).
int vault_emission(const BlockStateKey& state) {
  const std::string_view value = property_value(state.properties, "vault_state");
  if (value == "active" || value == "unlocking" || value == "ejecting")
    return 12;
  if (value == "inactive")
    return 6;
  return 0;
}

} // namespace

int light_nibble_index(int x, int y, int z) {
  return (y << 8) | (z << 4) | x;
}

namespace {

// The nibble accessors over raw bytes. A region's light lives in one flat
// buffer (`RegionStore`), so its per-cell reads and writes go through these;
// the vector forms below are the same two functions with a container in front
// of them.
int nibble_get(const uint8_t* data, size_t size, int x, int y, int z) {
  const int index = light_nibble_index(x, y, z);
  const size_t byte = static_cast<size_t>(index) >> 1;
  if (byte >= size)
    return 0;
  return (data[byte] >> (4 * (index & 1))) & 0xF;
}

void nibble_set(uint8_t* data, size_t size, int x, int y, int z, int level) {
  const int index = light_nibble_index(x, y, z);
  const size_t byte = static_cast<size_t>(index) >> 1;
  if (byte >= size)
    return;
  const int shift = 4 * (index & 1);
  data[byte] = static_cast<uint8_t>((data[byte] & ~(0xF << shift)) | ((level & 0xF) << shift));
}

} // namespace

int light_nibble_bytes(const uint8_t* data, size_t size, int x, int y, int z) {
  return nibble_get(data, size, x, y, z);
}

void set_light_nibble_bytes(uint8_t* data, size_t size, int x, int y, int z, int level) {
  nibble_set(data, size, x, y, z, level);
}

bool light_is_opaque(const BlockStateKey& state) {
  // `LeavesBlock.getLightDampening` is 1 and `TintedGlassBlock`'s is 15, both
  // overriding `isSolidRender`.
  if (is_leaves(state.name) || state.name == "minecraft:tinted_glass")
    return state.name == "minecraft:tinted_glass";
  // `isSolidRender()` = `isShapeFullBlock(occlusionShape)`: a full cube that
  // occludes. Both halves are generated tables (see the header).
  return block_blocks_motion(state.name) && !block_no_occlusion(state.name);
}

// The direct computation, kept for the memo below to prime itself from and for
// keys the generated registry does not carry (a synthetic state a test
// invents answers exactly as it always did).
int direct_dampening(const BlockStateKey& state) {
  if (is_leaves(state.name))
    return 1; // `LeavesBlock`
  if (state.name == "minecraft:tinted_glass")
    return 15; // `TintedGlassBlock`
  if (block_blocks_motion(state.name) && !block_no_occlusion(state.name))
    return 15; // `isSolidRender()`
  // `propagatesSkylightDown(state)` = "not a full block *and* no fluid", so a
  // cell holding a fluid (or a waterlogged block) costs one level where plain
  // air costs none. `LiquidBlock`/`PipeBlock`/`WallBlock`'s overrides are the
  // fluid half as the block state expresses it.
  if (is_fluid(state.name) || property_is_true(state.properties, "waterlogged"))
    return 1;
  return 0;
}

int direct_emission(const BlockStateKey& state) {
  const std::string_view name = state.name;
  // The scaled forms first: their names collide with nothing, and the candle
  // family is 16 blocks wide.
  if (name == "minecraft:candle" || ends_with(name, "_candle"))
    return candle_emission(state);
  if (name == "minecraft:sea_pickle")
    return sea_pickle_emission(state);
  if (name == "minecraft:light")
    return light_block_emission(state);
  if (name == "minecraft:respawn_anchor")
    return respawn_anchor_emission(state);
  if (name == "minecraft:cave_vines" || name == "minecraft:cave_vines_plant")
    return cave_vines_emission(state);
  if (name == "minecraft:glow_lichen")
    return glow_lichen_emission(state);
  if (name == "minecraft:trial_spawner")
    return trial_spawner_emission(state);
  if (name == "minecraft:vault")
    return vault_emission(state);
  if (ends_with(name, "_candle_cake")) {
    // `CANDLE_CAKE` and the 16 dyed ones: `litBlockEmission(3)`.
    return property_is_true(state.properties, "lit") ? 3 : 0;
  }
  if (name == "minecraft:copper_bulb" || ends_with(name, "_copper_bulb")) {
    // `lightLevel(litBlockEmission(unaffected 15, exposed 12, weathered 8,
    // oxidized 4))`, and the waxed variants are copies of the same four.
    const bool lit = property_is_true(state.properties, "lit");
    if (!lit)
      return 0;
    if (name.find("oxidized") != std::string_view::npos)
      return 4;
    if (name.find("weathered") != std::string_view::npos)
      return 8;
    if (name.find("exposed") != std::string_view::npos)
      return 12;
    return 15;
  }
  for (const Emission& row : kEmissionLit) {
    if (name == row.name && property_is_true(state.properties, "lit"))
      return row.level;
  }
  for (const Emission& row : kEmission) {
    if (name == row.name)
      return row.level;
  }
  return 0;
}

// ---- the memo ----------------------------------------------------------------
//
// The flood asks these per palette entry per section, and LightWorld's write
// path asks four times per block — each answer above a chain of string
// comparisons and, for emission, a linear walk of the tables above (the
// non-emitting common case pays every row). The generated registry interns
// every state anyway, so compute both values per *state* once, on first use
// (one pass over kBlockStates), and answer later calls with one registry hash
// and two array reads.
struct LightMemo {
  std::vector<int8_t> dampening;
  std::vector<int8_t> emission;
};

const LightMemo& light_memo() {
  static const LightMemo memo = [] {
    LightMemo m;
    size_t states = 0;
    for (const auto& row : kBlockStates)
      if (row.id >= 0 && static_cast<size_t>(row.id) + 1 > states)
        states = static_cast<size_t>(row.id) + 1;
    m.dampening.assign(states, -1);
    m.emission.assign(states, -1);
    for (const auto& row : kBlockStates) {
      if (row.id < 0 || m.dampening[row.id] >= 0)
        continue;
      const BlockStateKey key{std::string(row.name), std::string(row.props)};
      m.dampening[row.id] = static_cast<int8_t>(direct_dampening(key));
      m.emission[row.id] = static_cast<int8_t>(direct_emission(key));
    }
    return m;
  }();
  return memo;
}

int light_dampening(const BlockStateKey& state) {
  if (const auto id = full_block_states().lookup(state)) {
    const LightMemo& m = light_memo();
    if (*id >= 0 && static_cast<size_t>(*id) < m.dampening.size() &&
        m.dampening[static_cast<size_t>(*id)] >= 0)
      return m.dampening[static_cast<size_t>(*id)];
  }
  return direct_dampening(state);
}

int light_emission(const BlockStateKey& state) {
  if (const auto id = full_block_states().lookup(state)) {
    const LightMemo& m = light_memo();
    if (*id >= 0 && static_cast<size_t>(*id) < m.emission.size() &&
        m.emission[static_cast<size_t>(*id)] >= 0)
      return m.emission[static_cast<size_t>(*id)];
  }
  return direct_emission(state);
}

int light_kinds(const BlockStateKey& state) {
  if (const auto id = full_block_states().lookup(state)) {
    const LightMemo& m = light_memo();
    const size_t i = static_cast<size_t>(*id);
    // Both arrays are filled in the same pass (see light_memo), so a valid
    // entry has both values; `-1` means "no state with this id".
    if (*id >= 0 && i < m.dampening.size() && i < m.emission.size() && m.dampening[i] >= 0 &&
        m.emission[i] >= 0)
      return (static_cast<int>(m.emission[i]) << 4) | (static_cast<int>(m.dampening[i]) & 0xF);
  }
  return (direct_emission(state) << 4) | (direct_dampening(state) & 0xF);
}

namespace {

// The propagation is a level-ordered flood: vanilla's `LightEngine` keeps one
// queue per level, so a cell is only expanded once it can no longer be
// out-shadowed. Buckets of (x, y, z) per level, processed 15 down to 1.
struct Flood {
  using Cell = std::array<int, 3>;
  std::array<std::vector<Cell>, kMaxLightLevel + 1> buckets;
  // Set by the caller before seeding; `push` then records the world-y sections
  // the flood actually reaches. A section the block flood never touches can
  // only be zero, so the write-out resets it instead of scanning 4,096 cells.
  int min_y = 0;
  std::array<bool, 64> touched{};

  void push(int x, int y, int z, int level) {
    if (level <= 0 || level > kMaxLightLevel)
      return;
    buckets[static_cast<size_t>(level)].push_back({x, y, z});
    const int section = (y - min_y) >> 4;
    if (section >= 0 && section < static_cast<int>(touched.size()))
      touched[static_cast<size_t>(section)] = true;
  }
};

// A 3x3 region of light stores, indexed by world coordinates: the centre chunk
// is the one that gets written out, and the ring is read (and written) so a
// source across a border lights the cells it reaches. Vanilla keeps the same
// 3x3 region for the same reason — 15 blocks of reach fit inside it.
struct RegionStore {
  int base_x = 0; // the region's world x of local 0
  int base_z = 0;
  int min_y = 0;
  int section_count = 0;
  // Chunks per side: 3 for the whole-chunk recompute, 5 for the delta (the
  // centre 3x3 plus one halo chunk). One packing for both keeps the cell
  // arithmetic identical; only the arena's size changes.
  int width = 3;
  // Every (chunk, section, layer) nibble array, back to back in one buffer of
  // `9 * section_count * 2 * kLightArrayBytes` bytes, carved into views by
  // `offset()`. One buffer cleared in one pass, instead of 432 vectors
  // allocated and zeroed per relight. The bytes are owned by a per-thread
  // scratch (see `compute_chunk_light`) so they survive from one relight to the
  // next; this pointer is a plain view into them, which is what keeps the
  // flood's per-cell read a pointer dereference rather than a `thread_local`
  // lookup.
  uint8_t* arena = nullptr;
  // The write-out's buffer, one light array reused for every (section, layer)
  // of every relight: the nibbles are built here and only then handed to the
  // section, so a layer that comes out empty costs no allocation at all.
  std::array<uint8_t, static_cast<size_t>(kLightArrayBytes)> computed{};

  // `ci` indexes the region's 3x3 chunks in row order: 0..2 is the south row,
  // 3..5 the middle and 6..8 the north one. (The caller passes the *offset*
  // from `base_x`/`base_z`, so it is already 0..2.)
  static int chunk_index(int cx, int cz, int width) {
    return cz * width + cx;
  }

  // Point the region at `bytes` (the per-thread scratch) sized for `sections`
  // sections, with every byte back at zero. `assign` keeps the vector's
  // existing buffer whenever it is already large enough, so after the first
  // relight this is one bulk clear and nothing else.
  void prepare(std::vector<uint8_t>& bytes, int bx, int bz, int min_section_y, int sections,
               int chunks_per_side = 3) {
    base_x = bx;
    base_z = bz;
    min_y = min_section_y;
    section_count = sections;
    width = chunks_per_side;
    bytes.assign(static_cast<size_t>(width) * static_cast<size_t>(width) *
                     static_cast<size_t>(sections) * 2 * static_cast<size_t>(kLightArrayBytes),
                 static_cast<uint8_t>(0));
    arena = bytes.data();
  }

  bool contains(int x, int y, int z) const {
    if (x < base_x || x >= base_x + width * 16 || z < base_z || z >= base_z + width * 16)
      return false;
    const int section = y >> 4;
    return section >= min_y && section < min_y + section_count;
  }

  // Where one (chunk, section, layer) array starts: chunks in `chunk_index`
  // order, sections within the chunk, then the two layers. The one place that
  // knows the packing, because its callers arrive from different directions — a
  // world coordinate and a chunk-and-section — and neither should re-derive it.
  size_t offset(int ci, int section, LightLayer layer) const {
    const size_t index =
        static_cast<size_t>(ci) * static_cast<size_t>(section_count) + static_cast<size_t>(section);
    return (index * 2 + static_cast<size_t>(layer)) * static_cast<size_t>(kLightArrayBytes);
  }

  // The centre chunk's own array for `section` (its index within that chunk) and
  // `layer`. The region's slot for a cell of the centre chunk *is* the centre
  // chunk's own slot, which is what lets the write-out take the array once for
  // its 4096 cells.
  const uint8_t* centre(int section, LightLayer which) const {
    return arena + offset(chunk_index(width / 2, width / 2, width), section, which);
  }

  uint8_t* at(int x, int y, int z, LightLayer which) {
    if (!contains(x, y, z))
      return nullptr;
    const int ci = chunk_index((x - base_x) >> 4, (z - base_z) >> 4, width);
    return arena + offset(ci, (y >> 4) - min_y, which);
  }

  int get(int x, int y, int z, LightLayer layer) {
    const uint8_t* data = at(x, y, z, layer);
    if (data == nullptr)
      return 0;
    return nibble_get(data, static_cast<size_t>(kLightArrayBytes), x & 15, y & 15, z & 15);
  }

  void set(int x, int y, int z, LightLayer layer, int level) {
    uint8_t* data = at(x, y, z, layer);
    if (data != nullptr)
      nibble_set(data, static_cast<size_t>(kLightArrayBytes), x & 15, y & 15, z & 15, level);
  }
};

// A cell's two engine-facing facts, packed into one byte so a cell read is a
// single array index: low nibble `light_dampening`, high nibble
// `light_emission`.
struct SectionKinds {
  std::vector<uint8_t> by_palette;
  // Whether any entry in `by_palette` emits (its high nibble is non-zero). One
  // palette serves all 4096 cells of a section, so a section with no emitting
  // entry answers "emission 0" for every cell in it — which is what lets the
  // emitter scan below skip the section whole instead of asking each of its
  // 4096 cells. It is the same shape as the count `random_tick_blocks` keeps
  // per section (chunk.h): a per-section summary of a per-state predicate,
  // there to let a scan pass over a section of stone without reading it.
  bool any_emitter = false;
  // The same summary for the low nibble: whether any entry dampens (is not
  // clear). The sky-source walk below reads *down* a column looking for the
  // first cell that stops the sky, so a section whose palette cannot stop it
  // (pure air, pure glass, a column of nothing) is skipped whole too.
  bool any_dampening = false;
};

} // namespace

void compute_chunk_light(Chunk& chunk, const LightEnvironment& env) {
  const int base_x = chunk.x * 16;
  const int base_z = chunk.z * 16;
  const int min_y = chunk.min_section_y * 16;
  const int section_count = static_cast<int>(chunk.sections.size());
  if (section_count <= 0)
    return;

  const int section_top = min_y + section_count * 16;
  // ~864 KiB of light arrays for the region, kept between relights (per thread)
  // so a relight is one bulk clear instead of 432 fresh, zeroed vectors. The
  // store itself is a local pointing into those bytes, so the flood below reads
  // a plain pointer member and not a `thread_local` lookup per cell.
  static thread_local std::vector<uint8_t> region_arena_scratch;
  RegionStore region;
  region.prepare(region_arena_scratch, base_x - 16, base_z - 16, min_y / 16, section_count);

  // The region's nine chunks in `RegionStore`'s own slot order (`chunk_index`:
  // x fastest, so slot `((z - base_z) >> 4) * 3 + ((x - base_x) >> 4)`). A slot
  // outside the caller's region is "no chunk", which reads as air — the halo
  // the header documents as unmodelled.
  std::array<Chunk*, 9> slot_chunks{};
  if (env.region != nullptr) {
    for (int dz = -1; dz <= 1; ++dz)
      for (int dx = -1; dx <= 1; ++dx)
        slot_chunks[static_cast<size_t>((dz + 1) * 3 + (dx + 1))] = env.region->at(dx, dz);
  }
  const auto slot_of = [&](int x, int z) {
    if (x < region.base_x || x >= region.base_x + kRegionWidth || z < region.base_z ||
        z >= region.base_z + kRegionWidth)
      return -1;
    return ((z - region.base_z) >> 4) * 3 + ((x - region.base_x) >> 4);
  };

  // `light_dampening` and `light_emission` are both table searches: the generated
  // collision and occlusion tables are binary searches over block *names*, and
  // the emission rows are name comparisons. Asking them per *cell* is what made a
  // full-height chunk cost ~120 ms — a region scan is a few hundred thousand
  // cells and each one paid two searches. A section has a few dozen palette
  // entries and 4096 cells, so both are memoized per palette index exactly the
  // way `chunk_recount_random_tick_blocks` memoizes its own predicate, built on
  // a slot's first use (a scan touches only some of the nine).
  std::array<std::vector<SectionKinds>, 9> slot_kinds;
  std::array<bool, 9> slot_kinds_ready{};
  const auto kinds_for = [&](int slot) -> const std::vector<SectionKinds>& {
    if (!slot_kinds_ready[static_cast<size_t>(slot)]) {
      slot_kinds_ready[static_cast<size_t>(slot)] = true;
      const Chunk* c = slot_chunks[static_cast<size_t>(slot)];
      std::vector<SectionKinds>& out = slot_kinds[static_cast<size_t>(slot)];
      if (c != nullptr) {
        out.resize(c->sections.size());
        for (size_t i = 0; i < c->sections.size(); ++i) {
          const Section& s = c->sections[i];
          std::vector<uint8_t>& table = out[i].by_palette;
          table.resize(s.block_palette.size(), 0);
          for (size_t p = 0; p < s.block_palette.size(); ++p) {
            // One lookup for the pair, not two: this walks every palette entry
            // of every section in the 3x3 region.
            const uint8_t kinds = static_cast<uint8_t>(light_kinds(s.block_palette[p]));
            table[p] = kinds;
            if ((kinds >> 4) != 0)
              out[i].any_emitter = true;
            if ((kinds & 0xF) != 0)
              out[i].any_dampening = true;
          }
        }
      }
    }
    return slot_kinds[static_cast<size_t>(slot)];
  };

  // The two ways the engine reads a cell: through the caller's 3x3 region when
  // it has one (the memoized palette lookup above), or through `env.get_block`
  // (which for a server is a lock and a virtual call per cell, so the region is
  // the path that matters in production). `kind_at` returns the packed dampening
  // and emission; `dampening_at` is only the fallback's own form.
  const auto kind_at = [&](int x, int y, int z) -> uint8_t {
    if (env.region == nullptr) {
      const BlockStateKey state =
          env.get_block ? env.get_block(x, y, z) : BlockStateKey{"minecraft:air", ""};
      return static_cast<uint8_t>(light_kinds(state));
    }
    const int slot = slot_of(x, z);
    if (slot < 0)
      return 0; // the halo beyond the ring
    const Chunk* c = slot_chunks[static_cast<size_t>(slot)];
    const Chunk::CellIndex index =
        c != nullptr ? c->cell_index_at(x & 15, y, z & 15) : Chunk::CellIndex{};
    if (index.section < 0 || index.palette < 0)
      return 0;
    const std::vector<uint8_t>& table =
        kinds_for(slot)[static_cast<size_t>(index.section)].by_palette;
    const size_t p = static_cast<size_t>(index.palette);
    return p < table.size() ? table[p] : 0;
  };
  const auto dampening_at = [&](int x, int y, int z) {
    return static_cast<int>(kind_at(x, y, z) & 0xF);
  };
  // `Heightmap.Types.MOTION_BLOCKING` for a column: from the region when there
  // is one, otherwise the caller's own answer.
  const auto height_at = [&](int x, int z) {
    const int slot = slot_of(x, z);
    if (slot >= 0) {
      // Through the one height read, which refreshes a marked column.
      Chunk* c = slot_chunks[static_cast<size_t>(slot)];
      if (c != nullptr)
        return chunk_motion_blocking_height(*c, x & 15, z & 15);
    }
    if (env.region != nullptr)
      return min_y;
    return env.height ? env.height(x, z) : min_y;
  };
  // The region's highest cell worth looking at. Every *block* above a column's
  // motion-blocking height is either motion-blocking itself (leaves, a floating
  // platform — so it is in the height) or a shape with no collision (a torch, a
  // flower, fire), so `height + 1` bounds the region tightly instead of running
  // the flood over 384 empty cells per column.
  int top_y = min_y;
  for (int x = region.base_x; x < region.base_x + 48; ++x)
    for (int z = region.base_z; z < region.base_z + 48; ++z)
      top_y = std::max(top_y, height_at(x, z) + 1);
  top_y = std::clamp(top_y, min_y, section_top);

  // ---- `ChunkSkyLightSources`: what stops each column ----------------------
  //
  // Vanilla does not decrement sky light on the way *down*: it finds, per
  // column, the lowest y from which the sky is unbroken (`lowestSourceY`) and
  // then declares every cell at or above it a full 15. That single rule is what
  // makes a shaft lit to its floor, a column under a canopy 0 at the canopy and
  // leaving it to the flood, and everything in the open 15 without the flood
  // touching it.
  //
  // `ChunkSkyLightSources.isEdgeOccluded(top, bottom)` stops the walk when the
  // bottom cell's `getLightDampening()` is not 0, or when the two cells' faces
  // occlude each other (a block with a full face between them). Only the first
  // half is data the engine has; the face half is the documented shape
  // approximation, folded into `light_dampening` (a colliding shape reads 15
  // there, so it stops a column the way a slab or a stair does).
  const auto cell_index = [&](int x, int z) {
    return static_cast<size_t>((z - region.base_z) * kRegionWidth + (x - region.base_x));
  };
  //
  // The walk asks the same per-cell question as the emitter scan below — the
  // cell's palette entry's dampening — so it takes the same section-level
  // shortcut: a section with no dampening entry cannot stop the walk anywhere,
  // and is stepped over instead of read cell by cell. `top_y` is the highest
  // surface of the whole 3x3 region, so a column whose own ground is far below a
  // neighbour's hill would otherwise read every cell between the two.
  std::vector<int> source_y(static_cast<size_t>(kRegionWidth) * kRegionWidth, min_y);
  for (int x = region.base_x; x < region.base_x + kRegionWidth; ++x) {
    for (int z = region.base_z; z < region.base_z + kRegionWidth; ++z) {
      // A centre-only pass has no neighbours to ask, so the halo columns are
      // unmodelled rather than "never scanned and fully open": give them a
      // source above the world so the seeding loop below leaves them dark.
      if (env.centre_only && (x < base_x || x >= base_x + 16 || z < base_z || z >= base_z + 16)) {
        source_y[cell_index(x, z)] = section_top;
        continue;
      }
      const int slot = slot_of(x, z);
      const Chunk* column = slot >= 0 ? slot_chunks[static_cast<size_t>(slot)] : nullptr;
      const std::vector<SectionKinds>* kinds = slot >= 0 ? &kinds_for(slot) : nullptr;
      bool found = false;
      for (int y = top_y - 1; y >= min_y && !found;) {
        const int section_bottom = (y >> 4) * 16;
        const int bottom = section_bottom > min_y ? section_bottom : min_y;
        // A summary is only sound for a cell the *region* answers: without one
        // `kind_at` goes through `env.get_block` and every cell must be asked.
        // With one, a missing column is air and a section can be skipped when
        // its palette cannot stop the walk.
        bool dampens = true;
        if (env.region != nullptr) {
          dampens = false;
          if (column != nullptr) {
            const Chunk::CellIndex index = column->cell_index_at(x & 15, y, z & 15);
            dampens = index.section >= 0 && kinds != nullptr &&
                      static_cast<size_t>(index.section) < kinds->size() &&
                      (*kinds)[static_cast<size_t>(index.section)].any_dampening;
          }
        }
        if (!dampens) {
          y = bottom - 1;
          continue;
        }
        for (; y >= bottom; --y) {
          if ((kind_at(x, y, z) & 0xF) != 0) {
            source_y[cell_index(x, z)] = y + 1;
            found = true;
            break;
          }
        }
      }
    }
  }
  // A cell outside the 3x3 region was never scanned and never lit; `min_y`
  // makes the sky's own guard below skip it, and the halo beyond the ring is
  // the same "unmodelled" cell (see the header).
  const auto source_at = [&](int x, int z) {
    if (x < region.base_x || x >= region.base_x + kRegionWidth || z < region.base_z ||
        z >= region.base_z + kRegionWidth)
      return min_y;
    return source_y[cell_index(x, z)];
  };

  // ---- seed the sky --------------------------------------------------------
  //
  // The cells at and above each column's source are set to 15. Only the ones
  // that can leak into a neighbour still in that neighbour's shadow are queued
  // (vanilla's `addSourcesAbove` skips the rest): a cell at or above every
  // neighbour's source is surrounded by 15 and can raise nothing.
  Flood sky_flood;
  sky_flood.min_y = min_y;
  const auto neighbour_source_max = [&](int x, int z) {
    return std::max(std::max(source_at(x - 1, z), source_at(x + 1, z)),
                    std::max(source_at(x, z - 1), source_at(x, z + 1)));
  };
  for (int x = region.base_x; x < region.base_x + kRegionWidth; ++x) {
    for (int z = region.base_z; z < region.base_z + kRegionWidth; ++z) {
      const int source = source_at(x, z);
      const int neighbour_max = neighbour_source_max(x, z);
      for (int y = source; y < top_y; ++y) {
        region.set(x, y, z, LightLayer::kSky, kMaxLightLevel);
        if (y == source || y < neighbour_max)
          sky_flood.push(x, y, z, kMaxLightLevel);
      }
    }
  }

  // ---- seed the emitters ---------------------------------------------------
  //
  // Every cell below `emitter_top` is asked for its emission, but the answer only
  // depends on the cell's *palette entry*, and a section carries at most a few
  // dozen of those for its 4096 cells. So a section whose palette holds no
  // emitting entry — stone, dirt, air, water, every plant — is skipped whole:
  // its cells cannot answer anything but 0. On a full-height overworld column
  // that turns the scan from "every cell of the 3x3 region below the highest
  // surface" into "the cells of the few sections that hold a torch, a lava cell
  // or a glowing block", and the cells visited are visited in the same order.
  //
  // The bound is `top_y` *or* the top of the highest section that holds an
  // emitter, whichever is higher, because `top_y` alone is the region's
  // *motion-blocking* height and an emitter need not block motion: a torch, a
  // soul torch, a candle and fire are all in `kNonMotionBlocking`, so a torch
  // standing on a slab in the open sits one cell above the whole region's
  // heightmap and a scan bounded by `top_y` never asks it — it would light
  // nothing. The per-section summary the skip below already uses answers where
  // emitters can be at all, so the higher bound costs nine short scans per
  // relight. With no region there is no summary to ask (a fixture's own height
  // is all the caller gave us), and the scan keeps `top_y`.
  int emitter_top = top_y;
  if (env.region != nullptr) {
    for (int slot = 0; slot < 9; ++slot) {
      const Chunk* column = slot_chunks[static_cast<size_t>(slot)];
      if (column == nullptr)
        continue;
      const std::vector<SectionKinds>& kinds = kinds_for(slot);
      for (size_t s = kinds.size(); s > 0; --s) {
        if (!kinds[s - 1].any_emitter)
          continue;
        // Section `s - 1` of that chunk is `min_section_y + s - 1`, so it ends
        // at `min_section_y + s` blocks.
        const int top = (column->min_section_y + static_cast<int>(s)) * 16;
        emitter_top = std::max(emitter_top, std::min(top, section_top));
        break;
      }
    }
  }
  Flood block_flood;
  block_flood.min_y = min_y;
  for (int x = region.base_x; x < region.base_x + kRegionWidth; ++x) {
    for (int z = region.base_z; z < region.base_z + kRegionWidth; ++z) {
      const int slot = slot_of(x, z);
      const Chunk* column = slot >= 0 ? slot_chunks[static_cast<size_t>(slot)] : nullptr;
      const std::vector<SectionKinds>* kinds = slot >= 0 ? &kinds_for(slot) : nullptr;
      int y = min_y;
      while (y < emitter_top) {
        // Sections are 16 cells tall and aligned, so this is the y one past the
        // one `y` is in, clamped to the scan's own top.
        const int section_top = ((y >> 4) + 1) * 16;
        const int end = section_top < emitter_top ? section_top : emitter_top;
        // The section by the same rule `kind_at` uses, so the skip is exactly
        // "this section cannot answer anything but emission 0". As in the sky
        // walk above, a cell with no region behind it is asked cell by cell.
        bool emitters = true;
        if (env.region != nullptr) {
          emitters = false;
          if (column != nullptr) {
            const Chunk::CellIndex index = column->cell_index_at(x & 15, y, z & 15);
            emitters = index.section >= 0 && kinds != nullptr &&
                       static_cast<size_t>(index.section) < kinds->size() &&
                       (*kinds)[static_cast<size_t>(index.section)].any_emitter;
          }
        }
        if (!emitters) {
          y = end;
          continue;
        }
        for (; y < end; ++y) {
          const int emission = kind_at(x, y, z) >> 4;
          if (emission > 0) {
            region.set(x, y, z, LightLayer::kBlock, emission);
            block_flood.push(x, y, z, emission);
          }
        }
      }
    }
  }

  // ---- flood ---------------------------------------------------------------
  // `LightEngine.propagateIncrease`: a step costs `max(1, getLightDampening())`
  // of the cell entered, 16 (blocked) when the two cells' faces occlude —
  // which is the same 15 the dampening table gives a colliding shape, so one
  // test covers both.
  const int dx[6] = {0, 0, -1, 1, 0, 0};
  const int dy[6] = {1, -1, 0, 0, 0, 0};
  const int dz[6] = {0, 0, 0, 0, -1, 1};
  const auto propagate = [&](LightLayer layer, Flood& flood) {
    for (int level = kMaxLightLevel; level >= 1; --level) {
      std::vector<Flood::Cell>& bucket = flood.buckets[static_cast<size_t>(level)];
      for (size_t i = 0; i < bucket.size(); ++i) {
        const Flood::Cell cell = bucket[i];
        // The cell may have been raised since it was queued (a brighter path
        // found it first); expand at the level it actually holds.
        const int here = region.get(cell[0], cell[1], cell[2], layer);
        if (here != level)
          continue;
        for (int dir = 0; dir < 6; ++dir) {
          const int nx = cell[0] + dx[dir];
          const int ny = cell[1] + dy[dir];
          const int nz = cell[2] + dz[dir];
          // A centre-only pass does not spill into the unmodelled halo.
          if (env.centre_only &&
              (nx < base_x || nx >= base_x + 16 || nz < base_z || nz >= base_z + 16))
            continue;
          // A sky cell at or above its column's source is already 15 by the
          // rule above: nothing the flood could say about it is brighter.
          if (layer == LightLayer::kSky && ny >= source_at(nx, nz))
            continue;
          const int cost = std::max(1, dampening_at(nx, ny, nz));
          if (cost >= kMaxLightLevel)
            continue; // opaque: the light stops here (the covering cell itself
                      // may still emit, and does, from the emitter seeding)
          const int next = here - cost;
          if (next <= 0 || next <= region.get(nx, ny, nz, layer))
            continue;
          region.set(nx, ny, nz, layer, next);
          flood.push(nx, ny, nz, next);
        }
      }
      bucket.clear();
    }
  };
  propagate(LightLayer::kSky, sky_flood);
  propagate(LightLayer::kBlock, block_flood);

  // ---- write the centre chunk out ------------------------------------------
  // Read back through the region (which owns the ring as well) so a source
  // across a border has already had its say, and apply the sky rule one last
  // time: a cell at or above its column's source is 15 even where the region
  // never stored it — which is every cell above `top_y`.
  //
  // The highest source in the centre chunk's own columns. A sky section whose
  // *bottom* is at or above it is 15 in every one of its 4,096 cells, so the
  // loop below can fill the array instead of reading 4,096 nibbles and applying
  // the rule to each. On a generated column that is every open-air section above
  // the surface — the majority of a 24-section column's sky work, and a third of
  // the whole write-out.
  int centre_source_top = min_y;
  for (int z = 0; z < 16; ++z)
    for (int x = 0; x < 16; ++x)
      centre_source_top = std::max(centre_source_top, source_at(base_x + x, base_z + z));
  for (int index = 0; index < section_count; ++index) {
    Section& section = chunk.sections[static_cast<size_t>(index)];
    const int section_bottom = min_y + index * 16;
    for (const LightLayer layer : {LightLayer::kSky, LightLayer::kBlock}) {
      std::optional<LightArray>& out =
          layer == LightLayer::kSky ? section.sky_light : section.block_light;
      if (layer == LightLayer::kBlock && index < static_cast<int>(block_flood.touched.size()) &&
          !block_flood.touched[static_cast<size_t>(index)]) {
        // Block light only exists where the flood touched a cell. A section it
        // never reached (no emitter in range) is zero, so reset it and skip the
        // 4,096-cell write-out; absent and all-zero are the same downstream.
        out.reset();
        continue;
      }
      // A sky section whose bottom is at or above the highest source in the
      // centre chunk is full 15 in every cell. That is now a *derived* array:
      // one level and no buffer (world/chunk.h's `LightArray`), so the 2 KiB
      // fill the old write-out paid for this case is gone along with the bytes.
      if (layer == LightLayer::kSky && section_bottom >= centre_source_top) {
        out = LightArray::uniform(kMaxLightLevel);
        continue;
      }
      std::array<uint8_t, static_cast<size_t>(kLightArrayBytes)>& computed = region.computed;
      std::fill(computed.begin(), computed.end(), static_cast<uint8_t>(0));
      // Every cell of this loop is in the centre chunk's own section, so its
      // array is taken from the region once (`RegionStore::centre`) instead of
      // re-derived per cell the way the flood's cross-chunk steps must.
      const uint8_t* stored = region.centre(index, layer);
      bool any = false;
      for (int y = 0; y < 16; ++y) {
        for (int z = 0; z < 16; ++z) {
          for (int x = 0; x < 16; ++x) {
            const int world_x = base_x + x;
            const int world_z = base_z + z;
            const int world_y = section_bottom + y;
            int level = nibble_get(stored, static_cast<size_t>(kLightArrayBytes), x, y, z);
            if (layer == LightLayer::kSky && world_y >= source_at(world_x, world_z))
              level = kMaxLightLevel;
            if (level > 0) {
              nibble_set(computed.data(), static_cast<size_t>(kLightArrayBytes), x, y, z, level);
              any = true;
            }
          }
        }
      }
      // An all-zero layer is dropped rather than stored: the chunk encoder
      // treats "absent" and "all zero" identically (an empty section on the
      // wire), and the Anvil writer skips what is not there. A layer that does
      // carry light is written through `assign_from`, which reuses the section's
      // existing buffer when the content is not uniform and stores a derived
      // array (no buffer at all) when it is — so the steady state allocates
      // nothing here, and an open-air section releases what it used to hold.
      if (!any) {
        out.reset();
      } else {
        if (!out)
          out.emplace();
        out->assign_from(computed.begin(), computed.end());
      }
    }
  }
}

// The incremental relight (world/light.h). The arena is 5x5 chunks, the centre
// 3x3 is the result, and the ring is the halo that makes a neighbour's border
// exact when part of its light comes from beyond the 3x3.
//
// The removal half clears the edited cell's old level and walks the cells lit
// through it (a child is always strictly dimmer, every step costs at least one),
// remembering the still-lit boundary as re-add seeds. The increase half floods
// from those seeds and the edited cell's new emission. A column whose sky source
// moved has its whole run and the cells it fed rebuilt over the 5x5 first, so
// the centre 3x3 is exact there too; block light stays on the per-edit path.
bool compute_region_light_delta(const std::array<Chunk*, 25>& chunks, int centre_cx, int centre_cz,
                                const std::vector<LightDeltaEdit>& edits) {
  Chunk* centre = chunks[12];
  if (centre == nullptr || centre->sections.empty() || edits.empty())
    return false;

  const int base_x = (centre_cx - 2) * 16;
  const int base_z = (centre_cz - 2) * 16;
  const int sections = static_cast<int>(centre->sections.size());
  for (const Chunk* chunk : chunks) {
    if (chunk == nullptr)
      continue;
    if (chunk->min_section_y != centre->min_section_y ||
        chunk->sections.size() != centre->sections.size())
      return false;
    // The arena is seeded from the light the columns hold; a column that was
    // never lit would seed zeros and be used as if they were its answer. The
    // whole-chunk path lights such a column, so refuse instead.
    if (!chunk_has_light(*chunk))
      return false;
  }

  static thread_local std::vector<uint8_t> delta_scratch;
  RegionStore store;
  store.prepare(delta_scratch, base_x, base_z, centre->min_section_y, sections, 5);
  for (size_t slot = 0; slot < chunks.size(); ++slot) {
    const Chunk* chunk = chunks[slot];
    if (chunk == nullptr)
      continue;
    for (int index = 0; index < sections; ++index) {
      const Section& section = chunk->sections[static_cast<size_t>(index)];
      for (const LightLayer layer : {LightLayer::kSky, LightLayer::kBlock}) {
        const std::optional<LightArray>& held =
            layer == LightLayer::kSky ? section.sky_light : section.block_light;
        if (!held)
          continue;
        std::memcpy(store.arena + store.offset(static_cast<int>(slot), index, layer), held->data(),
                    static_cast<size_t>(kLightArrayBytes));
      }
    }
  }

  std::unordered_map<uint64_t, uint8_t> kinds_cache;
  const auto slot_of = [&](int x, int z) {
    if (x < base_x || x >= base_x + 5 * 16 || z < base_z || z >= base_z + 5 * 16)
      return -1;
    return ((z - base_z) >> 4) * 5 + ((x - base_x) >> 4);
  };
  const auto kinds_at = [&](int x, int y, int z) -> uint8_t {
    const int slot = slot_of(x, z);
    if (slot < 0)
      return 0;
    const Chunk* chunk = chunks[static_cast<size_t>(slot)];
    if (chunk == nullptr)
      return 0;
    const Chunk::CellIndex index = chunk->cell_index_at(x & 15, y, z & 15);
    if (index.section < 0 || index.palette < 0)
      return 0;
    const Section& section = chunk->sections[static_cast<size_t>(index.section)];
    const size_t palette = static_cast<size_t>(index.palette);
    if (palette >= section.block_palette.size())
      return 0;
    const uint64_t key = (static_cast<uint64_t>(slot) << 32) |
                         (static_cast<uint64_t>(index.section) << 16) |
                         static_cast<uint64_t>(palette);
    const auto found = kinds_cache.find(key);
    if (found != kinds_cache.end())
      return found->second;
    const uint8_t kinds = static_cast<uint8_t>(light_kinds(section.block_palette[palette]));
    kinds_cache.emplace(key, kinds);
    return kinds;
  };

  struct Cell {
    int x = 0;
    int y = 0;
    int z = 0;
    int level = 0;
  };
  const int dx[6] = {0, 0, -1, 1, 0, 0};
  const int dy[6] = {1, -1, 0, 0, 0, 0};
  const int dz[6] = {0, 0, 0, 0, -1, 1};

  // `RegionStore::min_y` is the first **section index**, not a world y (the
  // whole-chunk region uses the same field that way). The delta's scans are in
  // world coordinates, so the bottom of the arena is the section index times 16;
  // using the index here stopped every source scan at y = -4 and put the whole
  // underground below it out of reach.
  const int min_block_y = store.min_y * 16;
  const int section_top = min_block_y + sections * 16;
  int top_y = min_block_y;
  for (size_t slot = 0; slot < chunks.size(); ++slot) {
    Chunk* chunk = chunks[slot]; // non-const: the read may refresh a lazy heightmap
    if (chunk == nullptr)
      continue;
    for (int lz = 0; lz < 16; ++lz)
      for (int lx = 0; lx < 16; ++lx)
        top_y = std::max(top_y, chunk_motion_blocking_height(*chunk, lx, lz) + 1);
  }
  top_y = std::clamp(top_y, min_block_y, section_top);

  // A removed block can put the old source above the after heightmap, so every
  // edit's y + 1 is a floor for the scan; without it a top-block removal scans
  // only the new (lower) column and reads its own before and after as equal.
  for (const LightDeltaEdit& edit : edits)
    top_y = std::max(top_y, edit.y + 1);
  top_y = std::clamp(top_y, min_block_y, section_top);

  // The source is the column's first dampening cell from the top, plus one.
  // The before side is the live column with **every** edited cell put back to
  // its recorded before-kind: checking one edit at a time against the live
  // column lets two edits in a column move the source together while each one
  // looks stable. The column comes from the edit's (x, z), not its y.
  const auto cell_key = [](int x, int y, int z) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
           (static_cast<uint64_t>(static_cast<uint32_t>(z)) << 16) |
           static_cast<uint64_t>(static_cast<uint16_t>(y));
  };
  std::unordered_map<uint64_t, uint8_t> before_at;
  std::unordered_map<uint64_t, uint8_t> columns;
  for (const LightDeltaEdit& edit : edits) {
    before_at.emplace(cell_key(edit.x, edit.y, edit.z),
                      static_cast<uint8_t>(edit.before_kinds & 0xFF));
    columns.emplace((static_cast<uint64_t>(static_cast<uint32_t>(edit.x)) << 32) |
                        static_cast<uint32_t>(edit.z),
                    0);
  }
  const auto source_of = [&](int x, int z, bool before) {
    for (int cy = top_y - 1; cy >= min_block_y; --cy) {
      uint8_t kinds = kinds_at(x, cy, z);
      if (before) {
        const auto found = before_at.find(cell_key(x, cy, z));
        if (found != before_at.end())
          kinds = found->second;
      }
      if ((kinds & 0xF) != 0)
        return cy + 1;
    }
    return store.min_y;
  };
  bool source_moved = false;
  for (const auto& [key, unused] : columns) {
    (void)unused;
    const int x = static_cast<int>(static_cast<int32_t>(key >> 32));
    const int z = static_cast<int>(static_cast<int32_t>(key & 0xFFFFFFFFu));
    if (source_of(x, z, true) != source_of(x, z, false)) {
      source_moved = true;
      break;
    }
  }

  // A source move changes a column's solar run and can feed cells another source
  // also reaches; patching only the old run clears the other source's boundary.
  // Rebuild the whole sky layer over the 5x5 from the live columns instead: the
  // 3x3 is exact and the halo is the boundary the next call relies on. Block
  // light stays incremental below.
  if (source_moved) {
    for (int slot = 0; slot < 25; ++slot)
      for (int index = 0; index < sections; ++index)
        std::memset(store.arena + store.offset(slot, index, LightLayer::kSky), 0,
                    static_cast<size_t>(kLightArrayBytes));

    std::vector<int> source_y(25 * 256, min_block_y);
    for (int slot = 0; slot < 25; ++slot) {
      const Chunk* chunk = chunks[static_cast<size_t>(slot)];
      if (chunk == nullptr)
        continue;
      const int cx = centre_cx - 2 + (slot % 5);
      const int cz = centre_cz - 2 + (slot / 5);
      for (int lz = 0; lz < 16; ++lz) {
        for (int lx = 0; lx < 16; ++lx) {
          const int wx = cx * 16 + lx;
          const int wz = cz * 16 + lz;
          int source = min_block_y;
          for (int y = top_y - 1; y >= min_block_y; --y) {
            if ((kinds_at(wx, y, wz) & 0xF) != 0) {
              source = y + 1;
              break;
            }
          }
          source_y[static_cast<size_t>(slot) * 256 + static_cast<size_t>(lz) * 16 +
                   static_cast<size_t>(lx)] = source;
        }
      }
    }
    const auto source_at = [&](int wx, int wz) {
      const int slot = slot_of(wx, wz);
      if (slot < 0)
        return min_block_y;
      return source_y[static_cast<size_t>(slot) * 256 + static_cast<size_t>(wz & 15) * 16 +
                      static_cast<size_t>(wx & 15)];
    };
    std::array<std::vector<Cell>, kMaxLightLevel + 1> increase;
    const auto raise = [&](int x, int y, int z, int level) {
      if (level > 0 && level <= kMaxLightLevel)
        increase[static_cast<size_t>(level)].push_back({x, y, z, level});
    };
    for (int slot = 0; slot < 25; ++slot) {
      const Chunk* chunk = chunks[static_cast<size_t>(slot)];
      if (chunk == nullptr)
        continue;
      const int cx = centre_cx - 2 + (slot % 5);
      const int cz = centre_cz - 2 + (slot / 5);
      for (int lz = 0; lz < 16; ++lz) {
        for (int lx = 0; lx < 16; ++lx) {
          const int wx = cx * 16 + lx;
          const int wz = cz * 16 + lz;
          const int source = source_at(wx, wz);
          const int neighbour_max =
              std::max(std::max(source_at(wx - 1, wz), source_at(wx + 1, wz)),
                       std::max(source_at(wx, wz - 1), source_at(wx, wz + 1)));
          for (int y = source; y < section_top; ++y) {
            store.set(wx, y, wz, LightLayer::kSky, kMaxLightLevel);
            if (y == source || y < neighbour_max)
              raise(wx, y, wz, kMaxLightLevel);
          }
        }
      }
    }
    for (int level = kMaxLightLevel; level >= 1; --level) {
      std::vector<Cell>& bucket = increase[static_cast<size_t>(level)];
      for (size_t q = 0; q < bucket.size(); ++q) {
        const Cell cell = bucket[q];
        if (store.get(cell.x, cell.y, cell.z, LightLayer::kSky) != level)
          continue;
        for (int dir = 0; dir < 6; ++dir) {
          const int nx = cell.x + dx[dir];
          const int ny = cell.y + dy[dir];
          const int nz = cell.z + dz[dir];
          if (!store.contains(nx, ny, nz))
            continue;
          if (store.get(nx, ny, nz, LightLayer::kSky) == kMaxLightLevel)
            continue;
          const int cost = std::max(1, static_cast<int>(kinds_at(nx, ny, nz) & 0xF));
          if (cost >= kMaxLightLevel)
            continue;
          const int next = level - cost;
          if (next <= 0 || next <= store.get(nx, ny, nz, LightLayer::kSky))
            continue;
          store.set(nx, ny, nz, LightLayer::kSky, next);
          raise(nx, ny, nz, next);
        }
      }
      bucket.clear();
    }
  }

  for (const LightDeltaEdit& edit : edits) {
    if (!store.contains(edit.x, edit.y, edit.z))
      return false;
    const uint8_t before = static_cast<uint8_t>(edit.before_kinds & 0xFF);
    const uint8_t after = kinds_at(edit.x, edit.y, edit.z);
    const bool before_dampens = (before & 0xF) != 0;
    const bool after_dampens = (after & 0xF) != 0;
    for (const LightLayer layer : {LightLayer::kSky, LightLayer::kBlock}) {
      if (layer == LightLayer::kSky && (source_moved || (!before_dampens && !after_dampens)))
        continue; // the sky layer was rebuilt for the whole 5x5
      const int old = store.get(edit.x, edit.y, edit.z, layer);
      const int base = layer == LightLayer::kBlock ? (after >> 4) : 0;

      std::vector<Cell> removal;
      std::array<std::vector<Cell>, kMaxLightLevel + 1> increase;
      const auto raise = [&](int x, int y, int z, int level) {
        if (level > 0 && level <= kMaxLightLevel)
          increase[static_cast<size_t>(level)].push_back({x, y, z, level});
      };

      store.set(edit.x, edit.y, edit.z, layer, base);
      if (old > base)
        removal.push_back({edit.x, edit.y, edit.z, old});
      for (size_t q = 0; q < removal.size(); ++q) {
        const Cell cell = removal[q];
        for (int dir = 0; dir < 6; ++dir) {
          const int nx = cell.x + dx[dir];
          const int ny = cell.y + dy[dir];
          const int nz = cell.z + dz[dir];
          if (!store.contains(nx, ny, nz))
            continue;
          const int level = store.get(nx, ny, nz, layer);
          if (level == 0)
            continue;
          if (layer == LightLayer::kSky && level == kMaxLightLevel) {
            raise(nx, ny, nz, level); // solar: a source, never cleared
            continue;
          }
          const int next_base = layer == LightLayer::kBlock ? (kinds_at(nx, ny, nz) >> 4) : 0;
          if (level < cell.level) {
            if (next_base > 0 && level <= next_base) {
              raise(nx, ny, nz, level);
              continue;
            }
            store.set(nx, ny, nz, layer, next_base);
            removal.push_back({nx, ny, nz, level});
            if (next_base > 0)
              raise(nx, ny, nz, next_base);
          } else {
            raise(nx, ny, nz, level);
          }
        }
      }

      for (int dir = 0; dir < 6; ++dir) {
        const int nx = edit.x + dx[dir];
        const int ny = edit.y + dy[dir];
        const int nz = edit.z + dz[dir];
        const int level = store.get(nx, ny, nz, layer);
        if (level > 0)
          raise(nx, ny, nz, level);
      }
      if (base > 0)
        raise(edit.x, edit.y, edit.z, base);

      for (int level = kMaxLightLevel; level >= 1; --level) {
        std::vector<Cell>& bucket = increase[static_cast<size_t>(level)];
        for (size_t q = 0; q < bucket.size(); ++q) {
          const Cell cell = bucket[q];
          if (store.get(cell.x, cell.y, cell.z, layer) != level)
            continue;
          for (int dir = 0; dir < 6; ++dir) {
            const int nx = cell.x + dx[dir];
            const int ny = cell.y + dy[dir];
            const int nz = cell.z + dz[dir];
            if (!store.contains(nx, ny, nz))
              continue;
            if (layer == LightLayer::kSky && store.get(nx, ny, nz, layer) == kMaxLightLevel)
              continue;
            const int cost = std::max(1, static_cast<int>(kinds_at(nx, ny, nz) & 0xF));
            if (cost >= kMaxLightLevel)
              continue;
            const int next = level - cost;
            if (next <= 0 || next <= store.get(nx, ny, nz, layer))
              continue;
            store.set(nx, ny, nz, layer, next);
            raise(nx, ny, nz, next);
          }
        }
        bucket.clear();
      }
    }
  }

  // Write all 25 back; the halo is the next call's boundary. An all-zero layer
  // is dropped as the whole-chunk write-out drops it.
  for (size_t slot = 0; slot < chunks.size(); ++slot) {
    Chunk* chunk = chunks[slot];
    if (chunk == nullptr)
      continue;
    for (int index = 0; index < sections; ++index) {
      Section& section = chunk->sections[static_cast<size_t>(index)];
      for (const LightLayer layer : {LightLayer::kSky, LightLayer::kBlock}) {
        const uint8_t* bytes = store.arena + store.offset(static_cast<int>(slot), index, layer);
        std::optional<LightArray>& out =
            layer == LightLayer::kSky ? section.sky_light : section.block_light;
        bool nonzero = false;
        for (int i = 0; i < kLightArrayBytes; ++i) {
          if (bytes[i] != 0) {
            nonzero = true;
            break;
          }
        }
        if (!nonzero) {
          out.reset();
          continue;
        }
        if (out && out->size() == static_cast<size_t>(kLightArrayBytes) &&
            std::memcmp(out->data(), bytes, static_cast<size_t>(kLightArrayBytes)) == 0)
          continue;
        if (!out)
          out.emplace();
        out->assign_from(bytes, bytes + kLightArrayBytes);
      }
    }
  }
  return true;
}

int chunk_light_level(const Chunk& chunk, int x, int y, int z, LightLayer layer) {
  if (x < 0 || x > 15 || z < 0 || z > 15)
    return 0;
  // Floor division, not `/`: half the overworld's build height is negative
  // (sections below y = 0), and `-60 / 16` truncates to -3 where the section is
  // -4. `Chunk::cell_index_at` divides the same way, so a cell read through this
  // function and one read through the chunk agree on which section they mean.
  const int index = (y >= 0 ? y / 16 : (y - 15) / 16) - chunk.min_section_y;
  if (index < 0 || index >= static_cast<int>(chunk.sections.size()))
    return 0;
  const Section& section = chunk.sections[static_cast<size_t>(index)];
  const std::optional<LightArray>& data =
      layer == LightLayer::kSky ? section.sky_light : section.block_light;
  if (!data)
    return 0;
  return light_nibble(*data, x, y & 15, z);
}

int chunk_raw_brightness(const Chunk& chunk, int x, int y, int z, int sky_darken) {
  const int block = chunk_light_level(chunk, x, y, z, LightLayer::kBlock);
  const int sky = chunk_light_level(chunk, x, y, z, LightLayer::kSky);
  return std::max(block, sky - sky_darken);
}

} // namespace mc::world
