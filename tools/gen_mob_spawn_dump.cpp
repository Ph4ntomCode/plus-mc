// SPDX-License-Identifier: GPL-3.0-or-later
//
// Writes the natural spawner's golden output for a small set of recorded scenes:
// the run parameters (`mob_spawn_meta.tsv`), the world each run starts from
// (`mob_spawn_scene.tsv`) and one interaction trace per run
// (`mob_spawn_seed42_<run>.tsv`).
//
// This is **not** a vanilla dump, and cannot be. The pass is driven by the
// level's own random (`Level.random`, a `LegacyRandomSource` whose algorithm is
// ported exactly but whose *state* at any tick belongs to the server) and the
// draw order through the group/walk/pick/pack chain is what a caller sees, not
// something a jar dump could be compared against. So this is a regression pin of
// this runtime's own output, and what it pins is the half
// `tests/mob_spawn_test.cpp` cannot: that test scripts every draw, so it pins
// each *branch* but never the sequence a real stream produces through the chain.
//
// The trace is a full interaction log, one column wider than the other dumps'
// because a run is many ticks long: every row carries the tick it happened in,
// and a row whose tick column is not the current tick is drift.
//
//   surface  <tick> <x> <z> <height>                (`chunk.getHeight(WORLD_SURFACE, x, z)`)
//   chunk    <tick> <cx> <cz> <true|false>          (`canSpawnEntitiesInChunk`)
//   read     <tick> <x> <y> <z> <state>             (`get_block`)
//   light    <tick> <sky|block> <x> <y> <z> <level> (`getLight`)
//   biome    <tick> <x> <y> <z> <name>              (`getBiome`)
//   draw i   <tick> <bound> <result>                (`nextInt(bound)`)
//   draw u   <tick> <result>                        (`nextFloat`)
//   spawn    <tick> <type> <x> <y> <z> <yaw>        (one created mob)
//
// A run is `ticks` passes over chunk (0, 0) — vanilla's
// `NaturalSpawner.spawnForChunk`, once per chunk per tick — through **one**
// `SpawnState`, because that is what a caller does: the state carries the charges
// a spawn banks and the counts the caps are checked against, so tick 9 sees what
// ticks 0-8 did. A single pass would pin the draw chain of one attempt; the loop
// pins the chain *and* the state it feeds back into, which is the difference
// between a transcript of a pass and a run of the spawner.
//
// The draws are the sequence, so they are logged as they happen, in order; the
// spawned mobs are the *result* of a tick's attempt — the pass returns them at the
// end of the tick, and there is no hook inside `spawn_category_for_position` — so
// the `spawn` rows follow the draws of their own tick, in the order the pass
// created them. The reads, lights and biomes are the predicates' own queries,
// which pins which cells each gate consults and in what order.
//
// Usage:
//   plusmc-mob-spawn-dump <fixtures_dir> [seed] [--diff]
//
// `--diff` writes nothing: it re-runs every scene in memory, reads what is
// checked in, and prints a patch of each file that would change (invariant
// violations included), exiting 1 while the fixture differs. The review is
//
//   ./build/plusmc-mob-spawn-dump <fixtures_dir> --diff   # read the patch
//   ./build/plusmc-mob-spawn-dump <fixtures_dir>          # then rewrite it
//   ctest -R mob_spawn                                    # and re-pin it
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "fixture_diff.h"
#include "world/chunk.h"
#include "world/mob_category.h"
#include "world/mob_spawn.h"
#include "world/worldgen_random.h"

namespace {

using mc::world::BlockStateKey;
using mc::world::GeneratedMob;
using mc::world::LightLayer;
using mc::world::SpawnedMob;
using mc::world::SpawnEntity;
using mc::world::SpawnEnvironment;
using mc::world::SpawnPlayer;
using mc::world::SpawnState;

struct Placement {
  int x;
  int y;
  int z;
  const char* name;
  const char* properties;
};

struct LightCell {
  bool sky; // else block light
  int x;
  int y;
  int z;
  int level;
};

struct EntityCell {
  const char* type;
  uint8_t category;
  int x;
  int y;
  int z;
  bool persistence_required;
};

struct PlayerCell {
  double x;
  double y;
  double z;
  bool spectator;
};

struct Scene {
  const char* name;
  const char* biome;
  int default_surface;
  int default_sky;
  int default_block_light;
  bool can_see_sky;
  std::vector<Placement> blocks;
  std::vector<LightCell> lights;
  std::vector<EntityCell> entities;
  std::vector<PlayerCell> players;
};

// A 16x16 stone floor at `y`: the platform every scene's spawn attempts land on.
std::vector<Placement> floor_at(int y) {
  std::vector<Placement> out;
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z)
      out.push_back({x, y, z, "minecraft:stone", ""});
  return out;
}

// A packed herd: `count` plains creatures standing on the platform, which is how
// a scene puts a category at (or near) its cap without 70 hand-written rows.
std::vector<EntityCell> herd(const char* type, uint8_t category, int count, int y) {
  std::vector<EntityCell> out;
  for (int i = 0; i < count; ++i)
    out.push_back({type, category, 1 + (i % 14), y, 1 + ((i / 14) % 14), false});
  return out;
}

// The scenes. Every one is a 16x16 chunk at (0,0) over a solid floor, so a
// candidate drawn at `surface + 1` stands on ground the placement table accepts;
// what differs is the light (the monster gate), the ground block (the animal
// rule's tag), the biome (the spawn list and the two spawn-cost biomes) and the
// population (the caps and the potential-energy charges).
std::vector<Scene> build_scenes() {
  std::vector<Scene> scenes;

  // The dark platform: sealed and unlit, so the monster gate passes and the
  // plains monster list is what the picks draw from.
  Scene dark{"dark_platform", "minecraft:plains", -61, 0, 0, false, floor_at(-61), {}, {}, {}};
  dark.players.push_back({40.5, -60.0, 8.5, false});
  scenes.push_back(dark);

  // The same platform lit: `Monster.isDarkEnoughToSpawn` refuses every column, so
  // this is the run that pins the draws the gate still takes.
  Scene lit{"lit_platform", "minecraft:plains", -61, 15, 15, true, floor_at(-61), {}, {}, {}};
  // One dark pocket in the lit platform, so the per-cell override is exercised:
  // a candidate that lands there really can pass the block-light gate.
  lit.lights.push_back({false, 5, -60, 5, 0});
  lit.players.push_back({40.5, -60.0, 8.5, false});
  scenes.push_back(lit);

  // A grass platform in daylight: `Animal.isBrightEnoughToSpawn` plus the
  // `dirt`/`grass_block` ground tag, so the creature list can actually produce
  // something. The player stands the same 40 blocks away.
  Scene grass{
      "grass_platform", "minecraft:plains", -61, 15, 0, true, std::vector<Placement>{}, {}, {}, {}};
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z)
      grass.blocks.push_back({x, -61, z, "minecraft:grass_block", ""});
  grass.players.push_back({40.5, -60.0, 8.5, false});
  scenes.push_back(grass);

  // Soul sand valley: the one biome whose monsters carry a spawn cost, with two
  // skeletons (not persistence-required, so their charges are banked) standing on
  // the platform. This is the run that exercises `PotentialCalculator`.
  Scene valley{"soul_valley", "minecraft:soul_sand_valley", -61, 0,  0,
               false,         std::vector<Placement>{},     {},  {}, {}};
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z)
      valley.blocks.push_back({x, -61, z, "minecraft:soul_sand", ""});
  valley.entities.push_back({"minecraft:skeleton", mc::world::kSpawnMonster, 4, -60, 4, false});
  valley.entities.push_back({"minecraft:skeleton", mc::world::kSpawnMonster, 12, -60, 12, false});
  valley.players.push_back({40.5, -60.0, 8.5, false});
  scenes.push_back(valley);

  // The dark platform with nobody watching: `canSpawnForCategoryLocal` refuses
  // every chunk (no player within 128 blocks), so the pass must not draw at all.
  Scene unwatched{"unwatched", "minecraft:plains", -61, 0, 0, false, floor_at(-61), {}, {}, {}};
  scenes.push_back(unwatched);

  // The dark platform with the monster cap already spent: 70 monsters is
  // `getMaxInstancesPerChunk()` for a 289-chunk spawnable area, so
  // `filtered_spawning_categories` drops MONSTER before any chunk is visited.
  Scene capped{"capped", "minecraft:plains", -61, 0, 0, false, floor_at(-61), {}, {}, {}};
  capped.entities = herd("minecraft:zombie", mc::world::kSpawnMonster, 70, -60);
  capped.players.push_back({40.5, -60.0, 8.5, false});
  scenes.push_back(capped);

  return scenes;
}

struct RecordedRun {
  const char* name;
  const char* scene;
  uint8_t category;
  bool spawn_enemies;
  bool spawn_persistent;
  int ticks;      // passes over chunk (0, 0), one "tick" each, through one SpawnState
  int min_spawns; // the coverage this run exists for
  bool no_draws;  // a chunk that must be skipped before the random source is touched
};

// Six runs. `dark_monster` is the ordinary tick; `lit_monster` is the same
// platform with the light gate refusing (it still draws); `lit_creature` is the
// persistent pass over a daylight grass field; `valley_monster` is the
// spawn-cost biome; and `unwatched` (no player within 128 blocks, so the local
// cap refuses) and `capped` (the monster cap already spent, so the filter drops
// the category) are the two ways a chunk is skipped before any draw at all.
//
// The tick counts are chosen for coverage, not for size: a candidate's y is drawn
// from the whole world column (`getRandomPosWithin`), so on a platform at the
// bottom of the world only one draw in five lands on it, and the monster and
// animal runs need enough ticks for the packs that follow — while the two skipped
// runs need only enough to show the skip repeating, and the lit run's point is
// that the gate refuses however many times it is asked.
constexpr RecordedRun kRuns[] = {
    {"dark_monster", "dark_platform", mc::world::kSpawnMonster, true, false, 32, 1, false},
    {"lit_monster", "lit_platform", mc::world::kSpawnMonster, true, false, 8, 0, false},
    {"lit_creature", "grass_platform", mc::world::kSpawnCreature, true, true, 32, 1, false},
    {"valley_monster", "soul_valley", mc::world::kSpawnMonster, true, false, 16, 0, false},
    {"unwatched", "unwatched", mc::world::kSpawnMonster, true, false, 8, 0, true},
    {"capped", "capped", mc::world::kSpawnMonster, true, false, 8, 0, true},
};

constexpr int kChunkX = 0;
constexpr int kChunkZ = 0;
constexpr int kMinY = -64;
constexpr int kSpawnableChunks = 289; // one cap, `17^2`
constexpr double kSpawnPointX = 1000.0;
constexpr double kSpawnPointY = 1000.0;
constexpr double kSpawnPointZ = 1000.0;
// `Level.getSkyDarken()` and `Level.isThundering()`: not varied by these runs
// (the pre-dawn dark is the same in every one of them), recorded because the
// monster gate reads them.
constexpr int kSkyDarken = 0;
constexpr bool kThundering = false;

const char* light_layer_name(LightLayer layer) {
  return layer == LightLayer::kSky ? "sky" : "block";
}

std::string canonical_state(const BlockStateKey& key) {
  std::string text = key.name;
  if (!key.properties.empty()) {
    text += '[';
    text += key.properties;
    text += ']';
  }
  return text;
}

std::string category_names(const std::vector<uint8_t>& categories) {
  std::string text;
  for (size_t i = 0; i < categories.size(); ++i) {
    if (i != 0)
      text += ',';
    text += mc::world::kMobSpawnCategoryNames[categories[i]];
  }
  return text;
}

struct RunResult {
  std::vector<std::string> rows;
  std::string categories;
  size_t draws = 0; // nextInt calls
  size_t units = 0; // nextFloat calls
  size_t spawns = 0;
};

// ---- one run -----------------------------------------------------------------

RunResult run_scene(int64_t seed, const RecordedRun& run, const Scene& scene) {
  RunResult result;
  // The tick every logger below stamps its rows with; the loop at the end moves
  // it, and a row that carries a different one is a drift the comparison sees.
  int tick = 0;
  auto push = [&result, &tick](const char* kind, const char* body) {
    result.rows.push_back(std::string(kind) + "\t" + std::to_string(tick) + "\t" + body);
  };

  mc::world::WgLegacyRandom randomness(seed);
  SpawnEnvironment env;
  env.get_block = [&scene, &push](int x, int y, int z) {
    BlockStateKey block{"minecraft:air", ""};
    for (const Placement& placement : scene.blocks)
      if (placement.x == x && placement.y == y && placement.z == z)
        block = BlockStateKey{placement.name, placement.properties};
    char text[320];
    std::snprintf(text, sizeof(text), "%d\t%d\t%d\t%s", x, y, z, canonical_state(block).c_str());
    push("read", text);
    return block;
  };
  env.light_level = [&scene, &push](LightLayer layer, int x, int y, int z) {
    int level = layer == LightLayer::kSky ? scene.default_sky : scene.default_block_light;
    for (const LightCell& cell : scene.lights)
      if (cell.sky == (layer == LightLayer::kSky) && cell.x == x && cell.y == y && cell.z == z)
        level = cell.level;
    char text[128];
    std::snprintf(text, sizeof(text), "%s\t%d\t%d\t%d\t%d", light_layer_name(layer), x, y, z,
                  level);
    push("light", text);
    return level;
  };
  env.biome_name = [&scene, &push](int x, int y, int z) {
    char text[320];
    std::snprintf(text, sizeof(text), "%d\t%d\t%d\t%s", x, y, z, scene.biome);
    push("biome", text);
    return std::string(scene.biome);
  };
  env.world_surface_height = [&scene, &push](int x, int z) {
    char text[128];
    std::snprintf(text, sizeof(text), "%d\t%d\t%d", x, z, scene.default_surface);
    push("surface", text);
    return scene.default_surface;
  };
  // Always installed: an empty `can_see_sky` means "true" (a caller with no sky
  // question to ask), which would silently turn a sealed room into an open one.
  const bool can_see_sky = scene.can_see_sky;
  env.can_see_sky = [can_see_sky](int, int, int) { return can_see_sky; };
  env.can_spawn_entities_in_chunk = [&push](int cx, int cz) {
    char text[128];
    std::snprintf(text, sizeof(text), "%d\t%d\ttrue", cx, cz);
    push("chunk", text);
    return true;
  };
  env.spawn_point_x = kSpawnPointX;
  env.spawn_point_y = kSpawnPointY;
  env.spawn_point_z = kSpawnPointZ;
  env.min_y = kMinY;
  env.sky_darken = kSkyDarken;
  env.thundering = kThundering;

  env.next_int = [&randomness, &result, &push](int bound) {
    const int value = randomness.next_int(bound);
    ++result.draws;
    char text[128];
    std::snprintf(text, sizeof(text), "i\t%d\t%d", bound, value);
    push("draw", text);
    return value;
  };
  env.unit_random = [&randomness, &result, &push]() {
    const float value = randomness.next_float();
    ++result.units;
    char text[64];
    std::snprintf(text, sizeof(text), "u\t%.6f", static_cast<double>(value));
    push("draw", text);
    return static_cast<double>(value);
  };

  std::vector<SpawnEntity> entities;
  for (const EntityCell& cell : scene.entities)
    entities.push_back(
        {cell.type, cell.category, cell.x, cell.y, cell.z, cell.persistence_required});
  std::vector<SpawnPlayer> players;
  for (const PlayerCell& cell : scene.players)
    players.push_back({cell.x, cell.y, cell.z, cell.spectator});

  SpawnState state = SpawnState::build(kSpawnableChunks, entities, players, env);
  const std::vector<uint8_t> categories =
      mc::world::filtered_spawning_categories(state, run.spawn_enemies, run.spawn_persistent);
  result.categories = category_names(categories);
  // One pass over chunk (0, 0) per tick, through the one `SpawnState`: a spawn
  // banks a charge and moves the category's count, so what tick 9 draws and what
  // it can place both depend on ticks 0-8. That feedback is the half a single
  // pass cannot pin.
  for (tick = 0; tick < run.ticks; ++tick) {
    const std::vector<uint8_t> one{run.category};
    const std::vector<SpawnedMob> spawned =
        mc::world::spawn_for_chunk(state, one, kChunkX, kChunkZ, env);
    for (const SpawnedMob& mob : spawned) {
      ++result.spawns;
      char text[320];
      std::snprintf(text, sizeof(text), "%s\t%.1f\t%d\t%.1f\t%.4f", mob.type->name, mob.x,
                    static_cast<int>(mob.y), mob.z, static_cast<double>(mob.yaw));
      push("spawn", text);
    }
  }
  return result;
}

// ---- the artifacts ------------------------------------------------------------

// The scene a run records, so the meta can name the environment terms the run
// was actually given rather than the generator's own defaults.
const Scene* find_scene(const std::vector<Scene>& scenes, const char* name) {
  for (const Scene& scene : scenes)
    if (std::strcmp(scene.name, name) == 0)
      return &scene;
  return nullptr;
}

std::string meta_text(int64_t seed, const std::vector<RecordedRun>& runs,
                      const std::vector<RunResult>& results, const std::vector<Scene>& scenes) {
  std::string text;
  text += "# Phase 4 milestone C: the natural spawner's golden run parameters.\n"
          "#\n"
          "# NOT a vanilla dump: the pass is driven by `Level.random`, a legacy LCG the"
          " server owns,\n"
          "# so this is a regression pin of this runtime's own output rather than a"
          " comparison\n"
          "# against the game jar. `ctest -R mob_spawn` replays every row below through"
          " the real\n"
          "# `SpawnState::build` / `filtered_spawning_categories` / `spawn_for_chunk`"
          " and compares\n"
          "# the run's trace line for line, so a reordered gate or one draw too many"
          " cannot land\n"
          "# silently. Regenerate deliberately with `plusmc-mob-spawn-dump"
          " <fixtures_dir>`,\n"
          "# reviewing the change first with its `--diff` mode.\n"
          "#\n"
          "# `categories` is what `getFilteredSpawningCategories` answered for this run,"
          " in\n"
          "# `MobCategory.values()` order (the replay recomputes it and must agree), so"
          " a run can\n"
          "# be read as \"which categories were even considered\". `ticks` is how many"
          " passes over\n"
          "# the chunk the run made, each one a `spawnForChunk` through the same state, and"
          " the\n"
          "# trace's own tick column has to cover all of them in order. `spawnableChunks` is"
          " the\n"
          "# `DistanceManager` count the global cap scales by (289 is exactly one cap"
          " of 70\n"
          "# monsters), `minY`/`skyDarken`/`thundering` are the environment terms the"
          " predicates\n"
          "# read, and the spawn point is the world spawn `isRightDistanceToPlayerAnd"
          " SpawnPoint`\n"
          "# keeps clear. `draws`/`units` are the two streams' call counts and `spawns`"
          " the mobs\n"
          "# created; `rows` is the trace's length.\n"
          "#\n"
          "# seed\trun\tscene\tchunkX\tchunkZ\tcategory\tspawnEnemies\tspawnPersistent"
          "\tticks\tspawnableChunks\tminY\tskyDarken\tthundering\tcanSeeSky\tspawnPointX"
          "\tspawnPointY\tspawnPointZ\tcategories\tdraws\tunits\tspawns\trows\n";
  for (size_t i = 0; i < runs.size(); ++i) {
    const RunResult& result = results[i];
    const Scene* scene = find_scene(scenes, runs[i].scene);
    if (scene == nullptr)
      continue;
    char line[640];
    std::snprintf(
        line, sizeof(line),
        "%lld\t%s\t%s\t%d\t%d\t%s\t%s\t%s\t%d\t%d\t%d\t%d\t%s\t%s\t%.1f\t%.1f"
        "\t%.1f\t%s\t%zu\t%zu\t%zu\t%zu\n",
        static_cast<long long>(seed), runs[i].name, runs[i].scene, kChunkX, kChunkZ,
        mc::world::kMobSpawnCategoryNames[runs[i].category],
        runs[i].spawn_enemies ? "true" : "false", runs[i].spawn_persistent ? "true" : "false",
        runs[i].ticks, kSpawnableChunks, kMinY, kSkyDarken, kThundering ? "true" : "false",
        scene->can_see_sky ? "true" : "false", kSpawnPointX, kSpawnPointY, kSpawnPointZ,
        result.categories.c_str(), result.draws, result.units, result.spawns, result.rows.size());
    text += line;
  }
  return text;
}

std::string scene_text(const std::vector<Scene>& scenes) {
  std::string text = "# The world each recorded run starts from. Everything not listed is air;"
                     " every cell\n"
                     "# has the scene's default biome, light and surface height unless a row"
                     " overrides it.\n"
                     "# Entities and players are the population the caps and the player distances"
                     " are\n"
                     "# measured against, exactly as the caller builds it.\n"
                     "#\n"
                     "# kind\tscene\t...\n";
  for (const Scene& scene : scenes) {
    char row[400];
    std::snprintf(row, sizeof(row), "default_biome\t%s\t%s\n", scene.name, scene.biome);
    text += row;
    std::snprintf(row, sizeof(row), "default_surface\t%s\t%d\n", scene.name, scene.default_surface);
    text += row;
    std::snprintf(row, sizeof(row), "default_light\t%s\tsky\t%d\n", scene.name, scene.default_sky);
    text += row;
    std::snprintf(row, sizeof(row), "default_light\t%s\tblock\t%d\n", scene.name,
                  scene.default_block_light);
    text += row;
    std::snprintf(row, sizeof(row), "default_can_see_sky\t%s\t%s\n", scene.name,
                  scene.can_see_sky ? "true" : "false");
    text += row;
    for (const Placement& placement : scene.blocks) {
      std::snprintf(row, sizeof(row), "block\t%s\t%d\t%d\t%d\t%s\n", scene.name, placement.x,
                    placement.y, placement.z,
                    canonical_state(BlockStateKey{placement.name, placement.properties}).c_str());
      text += row;
    }
    for (const LightCell& cell : scene.lights) {
      std::snprintf(row, sizeof(row), "light\t%s\t%s\t%d\t%d\t%d\t%d\n", scene.name,
                    cell.sky ? "sky" : "block", cell.x, cell.y, cell.z, cell.level);
      text += row;
    }
    for (const EntityCell& cell : scene.entities) {
      std::snprintf(row, sizeof(row), "entity\t%s\t%s\t%s\t%d\t%d\t%d\t%s\n", scene.name, cell.type,
                    mc::world::kMobSpawnCategoryNames[cell.category], cell.x, cell.y, cell.z,
                    cell.persistence_required ? "true" : "false");
      text += row;
    }
    for (const PlayerCell& cell : scene.players) {
      std::snprintf(row, sizeof(row), "player\t%s\t%.1f\t%.1f\t%.1f\t%s\n", scene.name, cell.x,
                    cell.y, cell.z, cell.spectator ? "true" : "false");
      text += row;
    }
  }
  return text;
}

std::string trace_text(int64_t seed, const RecordedRun& run, const RunResult& result) {
  std::string text;
  text += "# Phase 4 milestone C: the natural spawner's interaction trace for run '";
  text += run.name;
  text += "' (scene '";
  text += run.scene;
  text += "' category '";
  text += mc::world::kMobSpawnCategoryNames[run.category];
  text += "'), seed ";
  text += std::to_string(seed);
  text += ".\n";
  text += "#\n"
          "# NOT a vanilla dump (see mob_spawn_meta.tsv): every row is one interaction"
          " the pass had\n"
          "# with the level or one draw it took, in order, and the column after the kind"
          " is the\n"
          "# tick it happened in — the run is `ticks` passes over one chunk through one"
          " state,\n"
          "# so the tick boundary is where one pass's rows end and the next begin."
          " `surface` is the\n"
          "# column the attempt started from (`getHeight(WORLD_SURFACE, x, z)`); `chunk`"
          " is\n"
          "# `canSpawnEntitiesInChunk`; `read`, `light` and `biome` are the placement"
          " rule and\n"
          "# cost predicates' own queries, so the order the gates consult the world is"
          " visible;\n"
          "# `draw i` is a `nextInt(bound)` (the columns are the pass's own chain: 16,"
          " 16, the\n"
          "# surface span, then the group and walk draws) and `draw u` a `nextFloat`."
          " `spawn` rows\n"
          "# are the mobs that tick's attempt created, in the order the pass made them —"
          " the pass\n"
          "# returns them at the end of the tick, so they follow that tick's draws"
          " rather than\n"
          "# interleaving with them.\n"
          "#\n"
          "# kind\ttick\t...\n";
  for (const std::string& row : result.rows)
    text += row + "\n";
  return text;
}

std::vector<std::string> to_lines(const std::string& text) {
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t newline = text.find('\n', pos);
    if (newline == std::string::npos) {
      lines.push_back(text.substr(pos));
      break;
    }
    lines.push_back(text.substr(pos, newline - pos));
    pos = newline + 1;
  }
  return lines;
}

std::string trace_path(const std::string& dir, int64_t seed, const char* run) {
  return dir + "/mob_spawn_seed" + std::to_string(seed) + "_" + run + ".tsv";
}

// The tick column of a trace row, or -1 for a row that has none of ours.
long row_tick(const std::string& row) {
  const size_t kind = row.find('\t');
  if (kind == std::string::npos)
    return -1;
  return std::strtol(row.c_str() + kind + 1, nullptr, 10);
}

size_t count_of_kind(const std::vector<std::string>& rows, const char* kind) {
  size_t count = 0;
  const std::string prefix = std::string(kind) + "\t";
  for (const std::string& row : rows)
    if (row.compare(0, prefix.size(), prefix) == 0)
      ++count;
  return count;
}

// The invariants that make a run's trace a stream pin rather than a transcript.
// The writer refuses to write a fixture that violates one; `--diff` reports them
// and still prints the patch, because a violated invariant is itself the drift a
// reviewer is looking for.
std::vector<std::string> invariant_violations(const RecordedRun& run, const RunResult& result) {
  std::vector<std::string> out;
  char message[512];
  if (count_of_kind(result.rows, "spawn") != result.spawns ||
      count_of_kind(result.rows, "draw") != result.draws + result.units) {
    std::snprintf(message, sizeof(message), "run '%s': the counters disagree with the trace",
                  run.name);
    out.push_back(message);
  }
  if (result.spawns < static_cast<size_t>(run.min_spawns)) {
    std::snprintf(message, sizeof(message),
                  "run '%s': %zu spawns, and this run exists for at least %d", run.name,
                  result.spawns, run.min_spawns);
    out.push_back(message);
  }
  // The tick column is what makes the trace a run rather than one pass: its rows
  // have to be in order and inside the ticks the run claims. It is deliberately
  // *not* required to end on the last tick: the local cap refuses a chunk from
  // the tick the category is full, and those ticks legitimately log nothing —
  // where a run's trace stops is visible in its own tick column (`lit_creature`
  // stops at 14 of 32), and the row-for-row comparison is what holds it there.
  long previous = -1;
  long last = -1;
  bool ordered = true;
  for (const std::string& row : result.rows) {
    const long tick = row_tick(row);
    if (tick < previous)
      ordered = false;
    previous = tick;
    if (tick > last)
      last = tick;
  }
  if (run.ticks <= 0) {
    std::snprintf(message, sizeof(message), "run '%s': no ticks to run", run.name);
    out.push_back(message);
  } else if (result.rows.empty()) {
    if (!run.no_draws) {
      std::snprintf(message, sizeof(message),
                    "run '%s': a visited chunk logged nothing over %d ticks", run.name, run.ticks);
      out.push_back(message);
    }
  } else if (!ordered) {
    std::snprintf(message, sizeof(message), "run '%s': the tick column goes backwards", run.name);
    out.push_back(message);
  } else if (last >= run.ticks) {
    std::snprintf(message, sizeof(message), "run '%s': a row at tick %ld, and the run is %d ticks",
                  run.name, last, run.ticks);
    out.push_back(message);
  }
  // A chunk nobody watches, or a category already at its cap, is skipped before
  // the pass touches the random source at all: no surface query, no draw.
  if (run.no_draws && (result.draws != 0 || result.units != 0 || result.spawns != 0 ||
                       count_of_kind(result.rows, "surface") != 0)) {
    std::snprintf(message, sizeof(message), "run '%s': a skipped chunk drew from the random source",
                  run.name);
    out.push_back(message);
  }
  if (!run.no_draws && result.draws == 0) {
    std::snprintf(message, sizeof(message), "run '%s': a visited chunk drew nothing", run.name);
    out.push_back(message);
  }
  return out;
}

bool diff_text(const std::string& label, const std::string& path, const std::string& regenerated) {
  const fixture_diff::Lines disk = fixture_diff::read_lines(path);
  if (!disk.exists)
    std::printf("  (%s does not exist yet)\n", label.c_str());
  fixture_diff::print_row_diff(label, disk.lines, to_lines(regenerated), /*max_rows=*/60);
  return !disk.exists || disk.lines != to_lines(regenerated);
}

} // namespace

int main(int argc, char** argv) {
  bool diff = false;
  std::string dir;
  int64_t seed = 42;
  bool have_seed = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--diff") == 0) {
      diff = true;
    } else if (dir.empty()) {
      dir = argv[i];
    } else if (!have_seed) {
      seed = std::strtoll(argv[i], nullptr, 10);
      have_seed = true;
    } else {
      std::fprintf(stderr, "usage: %s <fixtures_dir> [seed] [--diff]\n", argv[0]);
      return 2;
    }
  }
  if (dir.empty()) {
    std::fprintf(stderr, "usage: %s <fixtures_dir> [seed] [--diff]\n", argv[0]);
    return 2;
  }

  const std::vector<Scene> scenes = build_scenes();
  std::vector<RecordedRun> runs(kRuns, kRuns + sizeof(kRuns) / sizeof(kRuns[0]));
  std::vector<RunResult> results;
  std::vector<std::string> violations;
  results.reserve(runs.size());
  for (const RecordedRun& run : runs) {
    const Scene* scene = find_scene(scenes, run.scene);
    if (scene == nullptr) {
      std::fprintf(stderr, "error: run '%s' names an unknown scene '%s'\n", run.name, run.scene);
      return 2;
    }
    results.push_back(run_scene(seed, run, *scene));
    const std::vector<std::string> here = invariant_violations(run, results.back());
    violations.insert(violations.end(), here.begin(), here.end());
  }
  if (!violations.empty()) {
    for (const std::string& violation : violations)
      std::fprintf(diff ? stdout : stderr, diff ? "!! invariant: %s\n" : "error: %s\n",
                   violation.c_str());
    if (!diff) {
      std::fprintf(stderr, "refusing to write a fixture that pins this\n");
      return 1;
    }
  }

  const std::string meta_path = dir + "/mob_spawn_meta.tsv";
  const std::string scene_path = dir + "/mob_spawn_scene.tsv";
  const std::string meta = meta_text(seed, runs, results, scenes);
  const std::string scene = scene_text(scenes);

  if (diff) {
    std::printf("mob-spawn fixture: %zu runs, seed %lld\n", runs.size(),
                static_cast<long long>(seed));
    bool changed = false;
    changed |= diff_text("mob_spawn_meta.tsv", meta_path, meta);
    changed |= diff_text("mob_spawn_scene.tsv", scene_path, scene);
    for (size_t i = 0; i < runs.size(); ++i) {
      const std::string name =
          std::string("mob_spawn_seed") + std::to_string(seed) + "_" + runs[i].name + ".tsv";
      changed |= diff_text(name, trace_path(dir, seed, runs[i].name),
                           trace_text(seed, runs[i], results[i]));
    }
    if (!changed) {
      std::printf("summary: in sync with this run's output\n");
      return 0;
    }
    std::printf("summary: the checked-in fixture is OUT OF SYNC\n");
    std::printf("re-run without --diff to rewrite it, then `ctest -R mob_spawn`\n");
    return 1;
  }

  std::FILE* meta_file = std::fopen(meta_path.c_str(), "w");
  if (meta_file == nullptr) {
    std::fprintf(stderr, "error: cannot write %s\n", meta_path.c_str());
    return 1;
  }
  std::fputs(meta.c_str(), meta_file);
  std::fclose(meta_file);
  std::FILE* scene_file = std::fopen(scene_path.c_str(), "w");
  if (scene_file == nullptr) {
    std::fprintf(stderr, "error: cannot write %s\n", scene_path.c_str());
    return 1;
  }
  std::fputs(scene.c_str(), scene_file);
  std::fclose(scene_file);
  std::printf("wrote %s (%zu runs)\n", meta_path.c_str(), runs.size());
  std::printf("wrote %s (%zu scenes)\n", scene_path.c_str(), scenes.size());
  for (size_t i = 0; i < runs.size(); ++i) {
    const std::string path = trace_path(dir, seed, runs[i].name);
    std::FILE* trace = std::fopen(path.c_str(), "w");
    if (trace == nullptr) {
      std::fprintf(stderr, "error: cannot write %s\n", path.c_str());
      return 1;
    }
    std::fputs(trace_text(seed, runs[i], results[i]).c_str(), trace);
    std::fclose(trace);
    std::printf("wrote mob_spawn_seed%lld_%s.tsv (%zu rows: %zu draws, %zu units, %zu spawns, "
                "categories '%s')\n",
                static_cast<long long>(seed), runs[i].name, results[i].rows.size(),
                results[i].draws, results[i].units, results[i].spawns,
                results[i].categories.c_str());
  }
  return 0;
}
