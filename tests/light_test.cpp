// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the light engine (Phase 4 milestone A):
//   * the emission and dampening tables against the decompiled `Blocks.java`
//     rows they were transcribed from,
//   * the nibble layout (the disk's `DataLayer.getIndex` order),
//   * `compute_chunk_light` on synthetic terrain: the sky column, a shaft, an
//     overhang and a torch's 14,
//   * the strongest check the fixture allows — the engine's own light for a real
//     vanilla chunk, cell by cell against the light that chunk carries on disk.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "world/anvil.h"
#include "world/chunk.h"
#include "world/light.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  if (ok) {
    std::printf("  ok   %s\n", what.c_str());
    return;
  }
  std::printf("  FAIL %s\n", what.c_str());
  ++failures;
}

mc::world::BlockStateKey key(const char* name, const char* props = "") {
  return mc::world::BlockStateKey{name, props};
}

// A test chunk with one air section stack, mutated in place.
struct Fixture {
  mc::world::Chunk chunk;
  std::map<std::string, mc::world::BlockStateKey> cells;

  explicit Fixture(int sections = 3) {
    chunk.x = 0;
    chunk.z = 0;
    chunk.min_section_y = 0;
    chunk.sections.resize(static_cast<size_t>(sections));
    for (int i = 0; i < sections; ++i) {
      chunk.sections[static_cast<size_t>(i)].y = i;
      chunk.sections[static_cast<size_t>(i)].has_blocks = true;
      chunk.sections[static_cast<size_t>(i)].block_palette = {key("minecraft:air")};
      chunk.sections[static_cast<size_t>(i)].block_cells.assign(mc::world::kBlockCellsPerSection,
                                                                0);
    }
  }

  static std::string cell(int x, int y, int z) {
    return std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z);
  }

  void set(int x, int y, int z, mc::world::BlockStateKey state) {
    cells[cell(x, y, z)] = state;
  }

  mc::world::BlockStateKey get(int x, int y, int z) const {
    const auto it = cells.find(cell(x, y, z));
    return it == cells.end() ? key("minecraft:air") : it->second;
  }

  // The column's motion-blocking height: the y one past the highest block that
  // blocks motion, which for this fixture is the highest non-air cell.
  int height(int x, int z) const {
    int top = 0;
    for (const auto& [where, state] : cells) {
      int cx = 0, cy = 0, cz = 0;
      std::sscanf(where.c_str(), "%d,%d,%d", &cx, &cy, &cz);
      if (cx != x || cz != z)
        continue;
      if (!mc::world::block_blocks_motion(state.name))
        continue;
      top = std::max(top, cy + 1);
    }
    return top;
  }

  // Cells outside the chunk are the chunk's own cells wrapped around (`x & 15`),
  // i.e. the fixture *is* one chunk of an infinite repetition of itself. The
  // engine reads a 3x3 region (light reaches the border from up to 15 blocks
  // away), so a fixture has to say what surrounds it, and repeating it is the
  // least arbitrary answer: it neither leaks 15 into every edge cell (an air
  // ring) nor fakes a wall (a rock ring). It does repeat the fixture's *sources*
  // too, so a test that reasons about a distance must measure it in the wrapped
  // space — see the sky-and-shafts case below.
  mc::world::LightEnvironment env() {
    mc::world::LightEnvironment out;
    out.min_y = 0;
    out.max_y = static_cast<int>(chunk.sections.size()) * 16;
    out.get_block = [this](int x, int y, int z) { return get(x & 15, y, z & 15); };
    out.height = [this](int x, int z) { return height(x & 15, z & 15); };
    return out;
  }

  int light(int x, int y, int z, mc::world::LightLayer layer) const {
    return mc::world::chunk_light_level(chunk, x, y, z, layer);
  }
};

void test_tables() {
  std::printf("-- the tables\n");
  check(mc::world::light_emission(key("minecraft:torch")) == 14, "a torch emits 14");
  check(mc::world::light_emission(key("minecraft:glowstone")) == 15, "glowstone emits 15");
  check(mc::world::light_emission(key("minecraft:lava")) == 15, "lava emits 15");
  check(mc::world::light_emission(key("minecraft:fire")) == 15, "fire emits 15");
  check(mc::world::light_emission(key("minecraft:soul_torch")) == 10, "a soul torch emits 10");
  check(mc::world::light_emission(key("minecraft:redstone_ore", "lit=false")) == 0,
        "an unlit redstone ore emits nothing");
  check(mc::world::light_emission(key("minecraft:redstone_ore", "lit=true")) == 9,
        "a lit redstone ore emits 9");
  check(mc::world::light_emission(key("minecraft:furnace", "facing=north,lit=true")) == 13,
        "a lit furnace emits 13");
  check(mc::world::light_emission(key("minecraft:furnace", "facing=north,lit=false")) == 0,
        "an unlit furnace emits nothing");
  check(mc::world::light_emission(key("minecraft:candle", "candles=3,lit=true")) == 9,
        "three candles emit 9");
  check(mc::world::light_emission(key("minecraft:candle", "candles=3,lit=false")) == 0,
        "three unlit candles emit nothing");
  check(mc::world::light_emission(key("minecraft:white_candle_cake", "lit=true")) == 3,
        "a lit candle cake emits 3");
  check(mc::world::light_emission(key("minecraft:sea_pickle", "pickles=2,waterlogged=true")) == 9,
        "two live sea pickles emit 3 + 3 * 2");
  check(mc::world::light_emission(key("minecraft:sea_pickle", "dead=true,pickles=4")) == 0,
        "a dead sea pickle emits nothing");
  check(mc::world::light_emission(key("minecraft:light", "level=9,waterlogged=false")) == 9,
        "a light block emits its level property");
  check(mc::world::light_emission(key("minecraft:respawn_anchor", "charge=3")) == 11,
        "a charged respawn anchor scales its charge");
  check(mc::world::light_emission(key("minecraft:copper_bulb", "lit=true,powered=true")) == 15,
        "an unaffected copper bulb emits 15 when lit");
  check(mc::world::light_emission(key("minecraft:oxidized_copper_bulb", "lit=true,powered=true")) ==
            4,
        "an oxidized copper bulb emits 4 when lit");
  check(mc::world::light_emission(key("minecraft:lava_cauldron", "waterlogged=false")) == 15,
        "a lava cauldron emits 15 (Properties.ofLegacyCopy(CAULDRON).lightLevel(15))");
  // `CaveVines.emission(14)`: the berries are the emitter, not the vine.
  check(mc::world::light_emission(
            key("minecraft:cave_vines", "age=25,berries=true,waterlogged=false")) == 14,
        "a berried cave vine emits 14");
  check(mc::world::light_emission(
            key("minecraft:cave_vines", "age=25,berries=false,waterlogged=false")) == 0,
        "a picked one emits nothing");
  check(mc::world::light_emission(key("minecraft:cave_vines_plant", "berries=true")) == 14,
        "and so does the plant body, when it has berries");
  check(mc::world::light_emission(key("minecraft:cave_vines_plant", "berries=false")) == 0,
        "the plant body without them does not");
  // `GlowLichenBlock.emission(7)`: `hasAnyFace`.
  check(mc::world::light_emission(key("minecraft:glow_lichen",
                                      "down=false,east=false,north=false,south=true,up=false,"
                                      "waterlogged=true,west=false")) == 7,
        "glow lichen on a face emits 7");
  check(mc::world::light_emission(key("minecraft:glow_lichen",
                                      "down=false,east=false,north=false,south=false,up=false,"
                                      "waterlogged=false,west=false")) == 0,
        "the faceless state cannot be placed and does not emit");
  // `TrialSpawnerState.lightLevel()` and `VaultState.LightLevel`.
  check(mc::world::light_emission(key("minecraft:trial_spawner", "trial_spawner_state=inactive")) ==
            0,
        "an inactive trial spawner is dark");
  check(mc::world::light_emission(
            key("minecraft:trial_spawner", "trial_spawner_state=waiting_for_players")) == 4,
        "a waiting one emits 4");
  check(mc::world::light_emission(key("minecraft:trial_spawner", "trial_spawner_state=active")) ==
            8,
        "an active one emits 8");
  check(mc::world::light_emission(key("minecraft:trial_spawner", "trial_spawner_state=cooldown")) ==
            0,
        "and a cooling-down one is dark again");
  check(mc::world::light_emission(key("minecraft:vault", "vault_state=inactive")) == 6,
        "an inactive vault is half lit (6)");
  check(mc::world::light_emission(key("minecraft:vault", "vault_state=unlocking")) == 12,
        "an unlocking one is lit (12)");

  check(mc::world::light_dampening(key("minecraft:stone")) == 15, "stone stops light");
  check(mc::world::light_dampening(key("minecraft:air")) == 0, "air costs nothing");
  check(mc::world::light_dampening(key("minecraft:glass")) == 0,
        "glass is a full cube that does not occlude: it costs nothing");
  check(mc::world::light_dampening(key("minecraft:ice")) == 0, "ice costs nothing");
  check(mc::world::light_dampening(key("minecraft:oak_leaves")) == 1,
        "leaves cost one level (LeavesBlock.getLightDampening)");
  check(mc::world::light_dampening(key("minecraft:tinted_glass")) == 15,
        "tinted glass is opaque to light (TintedGlassBlock's override)");
  check(mc::world::light_dampening(key("minecraft:water", "level=0")) == 1,
        "water costs one level (LiquidBlock.propagatesSkylightDown is false)");
  // Vanilla's `getLightDampening` asks the *shape* (`isSolidRender`), so a
  // slab's is 0 and it stops the sky through the separate face rule. Shapes are
  // code, not data, so the engine folds both into the generated tables and a
  // colliding shape that occludes reads 15 — the documented approximation, and
  // the reason a slab roof is dark here (which is what a slab roof is for).
  check(mc::world::light_dampening(key("minecraft:oak_slab", "type=bottom")) == 15,
        "a colliding shape that occludes is opaque to light (vanilla: 0, plus the "
        "face rule on top)");
  check(mc::world::light_dampening(key("minecraft:oak_fence")) == 15,
        "and so is a fence, which vanilla's face rule lets light past");
  check(mc::world::light_dampening(key("minecraft:seagrass", "waterlogged=false")) == 0,
        "a non-colliding block costs nothing");
  check(mc::world::light_dampening(key("minecraft:seagrass", "waterlogged=true")) == 1,
        "and one level once it holds a fluid");
}

void test_nibbles() {
  std::printf("-- the nibble layout\n");
  std::vector<uint8_t> data(mc::world::kLightArrayBytes, 0);
  // `DataLayer.getIndex(x, y, z)` is `y << 8 | z << 4 | x`, the low nibble of a
  // byte holding the even index.
  check(mc::world::light_nibble_index(0, 0, 0) == 0, "index(0,0,0) is 0");
  check(mc::world::light_nibble_index(1, 0, 0) == 1, "index(1,0,0) is 1");
  check(mc::world::light_nibble_index(0, 0, 1) == 16, "index(0,0,1) is 16");
  check(mc::world::light_nibble_index(0, 1, 0) == 256, "index(0,1,0) is 256");
  mc::world::set_light_nibble(data, 1, 0, 0, 15);
  mc::world::set_light_nibble(data, 0, 0, 0, 7);
  check(data[0] == 0xF7, "x=0 is the low nibble of byte 0 and x=1 the high one");
  check(mc::world::light_nibble(data, 0, 0, 0) == 7, "and it reads back");
  check(mc::world::light_nibble(data, 1, 0, 0) == 15, "both do");
  mc::world::set_light_nibble(data, 1, 0, 0, 3);
  check(mc::world::light_nibble(data, 0, 0, 0) == 7,
        "writing one nibble leaves its neighbour alone");
}

void test_sky_and_shafts() {
  std::printf("-- the sky, a shaft and a roof\n");
  Fixture f(3); // y 0..47
  // A stone floor at y=10 over the whole chunk, a stone cave floor at y=4 under
  // it, and one shaft through the upper slab at (3, 3).
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z) {
      f.set(x, 4, z, key("minecraft:stone"));
      f.set(x, 10, z, key("minecraft:stone"));
    }
  for (int y = 5; y <= 10; ++y)
    f.set(3, y, 3, key("minecraft:air"));
  // A stone roof at y=30 over x, z in 8..11.
  for (int x = 8; x < 12; ++x)
    for (int z = 8; z < 12; ++z)
      f.set(x, 30, z, key("minecraft:stone"));

  mc::world::compute_chunk_light(f.chunk, f.env());

  check(f.light(3, 40, 3, mc::world::LightLayer::kSky) == 15, "open sky is 15");
  check(f.light(3, 11, 3, mc::world::LightLayer::kSky) == 15, "the ground under open sky is 15");
  // The shaft's column has no roof anywhere in it, so its source is the stone at
  // y=4 and every cell above that is a full 15 — no decrement on the way down.
  check(f.light(3, 10, 3, mc::world::LightLayer::kSky) == 15,
        "a shaft keeps full sky straight down (LightEngine's no-decrement drop)");
  check(f.light(3, 5, 3, mc::world::LightLayer::kSky) == 15, "to its floor");
  check(f.light(3, 4, 3, mc::world::LightLayer::kSky) == 0, "the stone it lands on holds no light");
  // The pocket under the upper slab is fed only by the shaft, one level per step.
  check(f.light(2, 9, 3, mc::world::LightLayer::kSky) == 14, "one step out of the shaft is 14");
  check(f.light(1, 9, 3, mc::world::LightLayer::kSky) == 13, "then 13");
  check(f.light(0, 9, 3, mc::world::LightLayer::kSky) == 12, "then 12");
  // Careful with the wrap-around: this fixture *is* the neighbouring chunks as
  // well, so the shaft at (3, 3) exists again at every 16-block offset, and the
  // pocket is fed by four images of it. Light moves to face neighbours only, so
  // a cell's level is `15 - (its wrapped Manhattan distance to the nearest
  // shaft)`. The corner (15, 9, 15) is four steps out in each axis, hence 7 — the
  // *farthest* cell, (11, 9, 11) at eight and eight, is the one that goes dark.
  check(f.light(15, 9, 15, mc::world::LightLayer::kSky) == 7,
        "the wrapped shaft lights the far corner 15 - 8 = 7");
  check(f.light(11, 9, 11, mc::world::LightLayer::kSky) == 0,
        "and the cell 16 steps from every image of the shaft is dark");
  check(f.light(7, 9, 3, mc::world::LightLayer::kSky) == 11,
        "four steps along one axis from the shaft is 11");
  // The roof: nothing comes down through it, so the cells under it are fed only
  // by the open columns around it, one level per step.
  check(f.light(9, 31, 9, mc::world::LightLayer::kSky) == 15, "above the roof is 15");
  check(f.light(8, 29, 9, mc::world::LightLayer::kSky) == 14,
        "under the roof's edge light leaks in one level per step");
  check(f.light(9, 29, 9, mc::world::LightLayer::kSky) == 13, "two steps in it is 13");
  check(f.light(9, 10, 9, mc::world::LightLayer::kSky) == 0,
        "and the floor under the roof's middle is dark");
}

void test_emitters() {
  std::printf("-- a torch, and light through glass\n");
  Fixture f(3);
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z)
      f.set(x, 10, z, key("minecraft:stone"));
  f.set(8, 11, 8, key("minecraft:torch"));
  mc::world::compute_chunk_light(f.chunk, f.env());

  check(f.light(8, 11, 8, mc::world::LightLayer::kBlock) == 14, "the torch's own cell is 14");
  check(f.light(9, 11, 8, mc::world::LightLayer::kBlock) == 13, "one step away is 13");
  check(f.light(8, 13, 8, mc::world::LightLayer::kBlock) == 12, "two steps away is 12");
  check(f.light(0, 11, 0, mc::world::LightLayer::kBlock) == 0, "and ten blocks away is dark");
  check(mc::world::chunk_raw_brightness(f.chunk, 8, 11, 8, 0) == 15,
        "raw brightness is max(blockLight, skyLight - dark): the sky over the torch wins");
  check(mc::world::chunk_raw_brightness(f.chunk, 0, 12, 0, 0) == 15, "open sky is 15");
  check(mc::world::chunk_raw_brightness(f.chunk, 0, 12, 0, 11) == 4,
        "and dusk (skyDarken 11) is 15 - 11");

  // A glass roof: a full cube that does not occlude, so the sky keeps 15 below.
  Fixture g(3);
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z) {
      g.set(x, 10, z, key("minecraft:stone"));
      g.set(x, 20, z, key("minecraft:glass"));
    }
  mc::world::compute_chunk_light(g.chunk, g.env());
  check(g.light(5, 19, 5, mc::world::LightLayer::kSky) == 15, "under glass the sky is still 15");
  check(g.light(5, 11, 5, mc::world::LightLayer::kSky) == 15, "down to the floor");

  // The same roof in leaves (`LeavesBlock.getLightDampening` is 1) costs one
  // level per cell, vertically and sideways: the leaf layer itself is 14 and the
  // cell under it 13, where glass kept 15 and 15.
  Fixture l(3);
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z) {
      l.set(x, 10, z, key("minecraft:stone"));
      l.set(x, 20, z, key("minecraft:oak_leaves", "distance=7,persistent=false,waterlogged=false"));
    }
  mc::world::compute_chunk_light(l.chunk, l.env());
  check(l.light(5, 20, 5, mc::world::LightLayer::kSky) == 14, "a leaf layer is 14");
  check(l.light(5, 19, 5, mc::world::LightLayer::kSky) == 13, "under it 13");
  check(l.light(5, 18, 5, mc::world::LightLayer::kSky) == 12, "and one further down 12");
}

// ---- the fixture's own light ------------------------------------------------
//
// A real vanilla 26.2 chunk carries the light its server computed. The engine is
// a re-implementation of that computation, so the honest test is not "does it
// look lit" but "does it agree with vanilla's own answer, cell by cell".
//
// One caveat shapes the whole comparison. The fixture region holds this chunk
// and nothing else, so the engine's 3x3 halo read finds *air* where vanilla's
// own lighting pass saw the real neighbours — a chunk-sized trench of sky, and a
// trench light can travel along. That is a property of the fixture, not of the
// engine, so the honest claims are two: clear of the ring that trench can reach,
// the engine agrees with vanilla exactly, and inside the ring the differences are
// reported with their distances rather than asserted away.
//
// The measured differences, by how many cells in from the chunk's own edge they
// sit (this is the `differences by distance` line the test prints):
//
//   sky    0:253  1:138  2:98  3:38  4:5   — the halo is bright, so it lifts the
//           cells it reaches; with an *opaque* halo the same run leaves 31 at
//           distance 0 and none anywhere else, which is what pins the cause.
//   block  0:73  1:11  2:6  3:4  4:1  5:1   — two causes. The halo lets block
//           light from a source near the border tunnel around the chunk's stone
//           and come back in further along (the water cells the disk holds 0);
//           and the engine's shape approximation — "a block that collides and can
//           occlude is a full cube" — calls this chunk's ten `moss_carpet` cells
//           opaque (15) where vanilla's `getLightDampening` for a shape that is
//           not a full cube is 0, so the disk holds 4..6 in them and the engine
//           holds 0.
constexpr int kUnaffectedDistance = 6;

void test_against_a_real_chunk(const std::string& fixtures_dir) {
  std::printf("-- against a real chunk's stored light\n");
  if (fixtures_dir.empty()) {
    std::printf("  SKIP no fixtures directory given on the command line\n");
    return;
  }
  const std::string region = fixtures_dir + "/ow3region";
  auto world = mc::world::AnvilWorld::open_region_dir(region);
  if (!world) {
    std::printf("  SKIP ow3region fixture missing (%s)\n", region.c_str());
    return;
  }
  // Non-const: the height read below brings a marked column current, and the
  // one query form takes the mutable column on purpose (world/chunk.h).
  auto chunk = world->load_chunk(-2, -1);
  if (!chunk) {
    std::printf("  FAIL the fixture chunk (-2, -1) did not load\n");
    ++failures;
    return;
  }
  const std::vector<mc::world::Section>& sections = chunk->sections;
  mc::world::Chunk copy = *chunk;

  int disk_sky_cells = 0;
  for (const mc::world::Section& s : sections)
    if (s.sky_light)
      disk_sky_cells += mc::world::kLightArrayBytes;

  mc::world::LightEnvironment env;
  env.min_y = chunk->min_section_y * 16;
  env.max_y = (chunk->min_section_y + static_cast<int>(sections.size())) * 16;
  // The fixture region holds this one chunk, so the 3x3 read ring around it is
  // air — which is what the engine's own halo read would find there.
  //
  // The bounds have to be the chunk's *own*, `[x*16, x*16+16)`, not `[-32, 0)`:
  // a look-alike ring that is off by one chunk reads the chunk's own blocks for
  // the nearest 16 columns of halo, i.e. a wrapped copy of the chunk — including
  // a copy of its own light sources sitting one block outside the border, which
  // then lights the border cells it was meant to be measured against.
  const int base_x = chunk->x * 16;
  const int base_z = chunk->z * 16;
  const auto in_chunk = [base_x, base_z](int x, int z) {
    return x >= base_x && x < base_x + 16 && z >= base_z && z < base_z + 16;
  };
  env.get_block = [&](int x, int y, int z) {
    if (!in_chunk(x, z))
      return key("minecraft:air");
    // `block_state_at` takes the local x/z and the *absolute* y, and answers air
    // outside the column, so it is exactly the read the engine wants here.
    return chunk->block_state_at(x & 15, y, z & 15);
  };
  env.height = [&](int x, int z) {
    if (!in_chunk(x, z))
      return env.min_y;
    return mc::world::chunk_motion_blocking_height(*chunk, x & 15, z & 15);
  };

  const auto started = std::chrono::steady_clock::now();
  mc::world::compute_chunk_light(copy, env);
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

  int compared = 0;
  int sky_match = 0;
  int block_match = 0;
  int block_cells = 0;
  // The same counts restricted to the cells the fixture's missing neighbours
  // cannot reach (see the bars below): those are the ones that must agree exactly.
  int deep_compared = 0;
  int deep_sky_match = 0;
  int deep_block_cells = 0;
  int deep_block_match = 0;
  // Every sky and block difference, keyed by how far the cell sits from the
  // chunk's own edge. Printed rather than asserted: this is the line that says
  // *why* the border differs.
  int sky_hist[16] = {0};
  int block_hist[16] = {0};
  for (size_t index = 0; index < sections.size(); ++index) {
    const mc::world::Section& from_disk = sections[index];
    const mc::world::Section& computed = copy.sections[index];
    for (int y = 0; y < 16; ++y) {
      for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
          const int edge_distance =
              std::min(std::min(std::min(x, 15 - x), std::min(z, 15 - z)), 15);
          const bool deep = edge_distance >= kUnaffectedDistance;
          if (from_disk.sky_light) {
            ++compared;
            const auto& a = *from_disk.sky_light;
            const int b =
                computed.sky_light ? mc::world::light_nibble(*computed.sky_light, x, y, z) : 0;
            const bool match = mc::world::light_nibble(a, x, y, z) == b;
            if (match)
              ++sky_match;
            else
              ++sky_hist[edge_distance];
            if (deep) {
              ++deep_compared;
              if (match)
                ++deep_sky_match;
            }
          }
          ++block_cells;
          const int from =
              from_disk.block_light ? mc::world::light_nibble(*from_disk.block_light, x, y, z) : 0;
          const int mine =
              computed.block_light ? mc::world::light_nibble(*computed.block_light, x, y, z) : 0;
          const bool block_match_here = from == mine;
          if (block_match_here)
            ++block_match;
          else
            ++block_hist[edge_distance];
          if (deep) {
            ++deep_block_cells;
            if (block_match_here)
              ++deep_block_match;
          }
        }
      }
    }
  }
  std::printf("  (chunk (%d,%d): %d sky cells vs disk, %d block cells; %.0f ms to compute)\n",
              chunk->x, chunk->z, compared, block_cells, ms);
  std::printf("  (sky: %d/%d match, of which %d/%d clear of the edge; block: %d/%d, of which "
              "%d/%d clear)\n",
              sky_match, compared, deep_sky_match, deep_compared, block_match, block_cells,
              deep_block_match, deep_block_cells);
  std::printf("  (differences by distance from the edge — sky:");
  for (int d = 0; d < 16; ++d)
    if (sky_hist[d] != 0)
      std::printf(" %d:%d", d, sky_hist[d]);
  std::printf("; block:");
  for (int d = 0; d < 16; ++d)
    if (block_hist[d] != 0)
      std::printf(" %d:%d", d, block_hist[d]);
  std::printf(")\n");
  check(disk_sky_cells > 0, "the fixture chunk carries light on disk");
  // The claim with teeth, and the check that found the picked cave vine: clear
  // of the ring the empty halo can reach, the engine's light *is* vanilla's, cell
  // for cell, on both layers at once. A wrong emission level, a wrong dampening
  // or a wrong flood order moves cells out here and fails this.
  check(deep_compared > 0 && deep_sky_match == deep_compared,
        "clear of the empty halo's reach, the engine's sky light is vanilla's exactly");
  check(deep_block_cells > 0 && deep_block_match == deep_block_cells, "and so is its block light");
  // Floors over the whole chunk, border included: they cannot fail while the two
  // above pass, and they are here so that a change which breaks the *border*
  // rather than the model (a wrong dampening there, say) is caught too.
  check(compared > 0 && sky_match * 100 >= compared * 96,
        "and at least 96% of the sky light overall, the fixture's edge included");
  check(block_match * 100 >= block_cells * 99, "and at least 99% of the block light overall");
}

} // namespace

int main(int argc, char** argv) {
  // A crash must not eat the checks that came before it.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("light_test\n");
  test_tables();
  test_nibbles();
  test_sky_and_shafts();
  test_emitters();
  test_against_a_real_chunk(argc > 1 ? argv[1] : "");
  if (failures != 0) {
    std::printf("light_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("light_test: all checks passed\n");
  return 0;
}
