// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the natural spawner (Phase 4 milestone C):
//   * the generated tables: the biome spawn lists and their renormalization over
//     the types world/mob_table.h can create, the two biome spawn costs, the
//     placement/rule registration and the block rules;
//   * `SpawnState`: the global cap (`max * chunks / 289`), the local cap's
//     per-player running total, the potential-energy arithmetic and `after_spawn`;
//   * the gates: `Monster.isDarkEnoughToSpawn`'s three draws and its thundering
//     branch, `Animal.isBrightEnoughToSpawn`, the placement predicates and the
//     24-block player/spawn-point distances;
//   * the pass itself: a dark sealed platform spawns monsters and a lit one
//     spawns nothing, over a seeded generator;
//   * `Mob.checkDespawn`: the instant far branch, the animals that never
//     despawn, the 600-tick roll and the persistence flag — and the same through
//     `tick_mobs`, which must report a discard as a despawn and not a death.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include "world/memory_world.h"
#include "world/mob.h"
#include "world/mob_category.h"
#include "world/mob_spawn.h"
#include "world/mob_table.h"
#include "world/worldgen_random.h" // WgLegacyRandom: the level's own stream

using namespace mc::world;

static int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECK_EQ(a, b)                                                                             \
  do {                                                                                             \
    const long long va = static_cast<long long>(a), vb = static_cast<long long>(b);                \
    if (va != vb) {                                                                                \
      std::fprintf(stderr, "FAIL %s:%d: %s == %lld, want %lld\n", __FILE__, __LINE__, #a, va, vb); \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

namespace {

BlockStateKey state(const char* name, const char* props = "") {
  return BlockStateKey{name, props};
}

// ---- a scripted world -------------------------------------------------------
//
// One block, one light level and one biome per cell, all from functions, so a
// test can describe a platform in a few lines and pin every draw the pass makes.
struct FakeWorld {
  // The block at a cell: anything the test did not place is air.
  std::function<BlockStateKey(int, int, int)> block = [](int, int, int) {
    return state("minecraft:air");
  };
  std::function<int(LightLayer, int, int, int)> light = [](LightLayer, int, int, int) { return 0; };
  std::function<std::string(int, int, int)> biome = [](int, int, int) {
    return std::string("minecraft:plains");
  };
  std::function<int(int, int)> surface = [](int, int) { return -64; };
  bool can_see_sky = false;
  // `ServerLevel.canSpawnEntitiesInChunk` — always present, because vanilla's is.
  bool chunk_exists = true;
  double spawn_point_x = 1000.0, spawn_point_y = 1000.0, spawn_point_z = 1000.0;
  int min_y = -64;
  int sky_darken = 0;
  bool thundering = false;

  SpawnEnvironment env(std::function<int(int)> next_int,
                       std::function<double()> unit_random) const {
    SpawnEnvironment e;
    e.get_block = block;
    e.light_level = light;
    e.biome_name = biome;
    e.world_surface_height = surface;
    if (can_see_sky)
      e.can_see_sky = [](int, int, int) { return true; };
    const bool exists = chunk_exists;
    e.can_spawn_entities_in_chunk = [exists](int, int) { return exists; };
    e.spawn_point_x = spawn_point_x;
    e.spawn_point_y = spawn_point_y;
    e.spawn_point_z = spawn_point_z;
    e.min_y = min_y;
    e.sky_darken = sky_darken;
    e.thundering = thundering;
    e.next_int = std::move(next_int);
    e.unit_random = std::move(unit_random);
    return e;
  }
};

// A scripted sequence of draws: the test says what each call answers, and any
// call past the end answers `fallback` (which is what makes a "and then it keeps
// trying" assertion possible without listing a thousand draws).
struct Script {
  std::deque<int> ints;
  std::deque<double> units;
  int fallback_int = 0;
  double fallback_unit = 0.0;

  std::function<int(int)> next_int() {
    return [this](int bound) {
      if (ints.empty())
        return bound <= 0 ? 0 : fallback_int % bound;
      const int v = ints.front();
      ints.pop_front();
      return v;
    };
  }
  std::function<double()> unit_random() {
    return [this]() {
      if (units.empty())
        return fallback_unit;
      const double v = units.front();
      units.pop_front();
      return v;
    };
  }
};

// A seeded generator, for the pass tests that only care *that* something spawned
// and what the gates let through.
struct Seeded {
  std::mt19937 rng{12345};
  std::function<int(int)> next_int() {
    return [this](int bound) {
      if (bound <= 0)
        return 0;
      return static_cast<int>(std::uniform_int_distribution<int>(0, bound - 1)(rng));
    };
  }
  std::function<double()> unit_random() {
    return [this]() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng); };
  }
};

// A flat stone plane with two cells of air above it, and a light function that
// returns `sky`/`block` for the air cells (the platform's own light) and 0 for
// every solid one.
SpawnEnvironment platform_env(Seeded& seeded, int floor_y, int sky, int block_light,
                              std::function<std::string(int, int, int)> biome) {
  SpawnEnvironment e;
  e.get_block = [floor_y](int, int y, int) {
    return y <= floor_y ? state("minecraft:stone") : state("minecraft:air");
  };
  e.light_level = [floor_y, sky, block_light](LightLayer layer, int, int y, int) {
    if (y <= floor_y)
      return 0;
    return layer == LightLayer::kSky ? sky : block_light;
  };
  e.biome_name = std::move(biome);
  // `getHeight(WORLD_SURFACE)`: one past the top solid block.
  e.world_surface_height = [floor_y](int, int) { return floor_y; };
  e.can_spawn_entities_in_chunk = [](int, int) { return true; };
  e.spawn_point_x = 1000.0;
  e.spawn_point_y = 1000.0;
  e.spawn_point_z = 1000.0;
  e.min_y = -64;
  e.next_int = seeded.next_int();
  e.unit_random = seeded.unit_random();
  return e;
}

SpawnPlayer watcher(double x, double y, double z, bool spectator = false) {
  SpawnPlayer p;
  p.x = x;
  p.y = y;
  p.z = z;
  p.spectator = spectator;
  return p;
}

const GeneratedMob& mob(const char* name) {
  const GeneratedMob* t = mob_by_name(name);
  CHECK(t != nullptr);
  // The CHECK above fails the run for a name this table does not know; the
  // fallback only keeps the caller alive, and it is written field-for-field so a
  // new column cannot silently shift it.
  static const GeneratedMob fallback{.name = "minecraft:?",
                                     .display_name = "?",
                                     .entity_type_id = 0,
                                     .category = kSpawnMonster,
                                     .flags = 0,
                                     .fire_immune = false,
                                     .width = 0.6f,
                                     .height = 1.8f,
                                     .baby_width = 0.0f,
                                     .baby_height = 0.0f,
                                     .max_health = 20.0f,
                                     .movement_speed = 0.23,
                                     .attack_damage = 3.0f,
                                     .follow_range = 16.0,
                                     .follow_parent_speed = 0.0f,
                                     .armor = 0.0f,
                                     .xp_min = 5,
                                     .xp_max = 5};
  return t ? *t : fallback;
}

// ---- the generated tables ---------------------------------------------------

void test_generated_tables() {
  // `minecraft:plains`'s monster list: nine entries in the data pack, and every
  // one of them is now a type world/mob_table.h can create (the table carries
  // every living entity type), so the weight total the pick renormalizes over is
  // the pack's own sum: 100 + 90 + 5 + 5 + 100 + 100 + 100 + 10 + 5 = 515.
  const BiomeSpawnList monsters = biome_spawn_list("minecraft:plains", kSpawnMonster);
  CHECK_EQ(monsters.count, 9);
  CHECK_EQ(monsters.weight_total, 515);
  const BiomeSpawnList animals = biome_spawn_list("minecraft:plains", kSpawnCreature);
  CHECK_EQ(animals.count, 6);
  CHECK_EQ(animals.weight_total, 46); // sheep 12 + pig 10 + chicken 10 + cow 8 + horse 5 + donkey 1
  // A category the biome does not list, and a biome that lists nothing at all.
  CHECK(biome_spawn_list("minecraft:plains", kSpawnWaterCreature).empty());
  CHECK(biome_spawn_list("minecraft:the_void", kSpawnMonster).empty());
  CHECK_EQ(biome_spawn_list("minecraft:the_void", kSpawnMonster).weight_total, 0);

  // `MobSpawnSettings.getMobSpawnCost`: only two biomes set one in 26.2.
  const auto cost = biome_spawn_cost("minecraft:soul_sand_valley", "minecraft:ghast");
  CHECK(cost.has_value());
  if (cost) {
    CHECK(cost->charge > 0.0);
    CHECK(cost->energy_budget > 0.0);
  }
  CHECK(!biome_spawn_cost("minecraft:plains", "minecraft:zombie").has_value());

  // `SpawnPlacements`: the registration for a type is only *usable* when its
  // spawn-rule predicate is one this engine implements — the mob table now
  // carries every living entity type, so the check is over both halves: the
  // shared predicates resolve and the mob-specific ones stay refused.
  CHECK_EQ(spawn_placement_of(mob("minecraft:zombie"))->placement, kPlacementOnGround);
  CHECK_EQ(spawn_placement_of(mob("minecraft:zombie"))->rule, kSpawnRuleMonster);
  CHECK_EQ(spawn_placement_of(mob("minecraft:spider"))->rule, kSpawnRuleMonster);
  CHECK_EQ(spawn_placement_of(mob("minecraft:skeleton"))->rule, kSpawnRuleMonster);
  CHECK_EQ(spawn_placement_of(mob("minecraft:creeper"))->rule, kSpawnRuleMonster);
  CHECK_EQ(spawn_placement_of(mob("minecraft:enderman"))->rule, kSpawnRuleMonster);
  CHECK_EQ(spawn_placement_of(mob("minecraft:zombie_villager"))->rule, kSpawnRuleMonster);
  CHECK_EQ(spawn_placement_of(mob("minecraft:pig"))->rule, kSpawnRuleAnimal);
  CHECK_EQ(spawn_placement_of(mob("minecraft:cow"))->placement, kPlacementOnGround);
  CHECK_EQ(spawn_placement_of(mob("minecraft:chicken"))->rule, kSpawnRuleAnimal);
  CHECK_EQ(spawn_placement_of(mob("minecraft:sheep"))->rule, kSpawnRuleAnimal);
  CHECK_EQ(spawn_placement_of(mob("minecraft:horse"))->rule, kSpawnRuleAnimal);
  CHECK_EQ(spawn_placement_of(mob("minecraft:villager"))->rule, kSpawnRuleMob);
  CHECK_EQ(spawn_placement_of(mob("minecraft:iron_golem"))->rule, kSpawnRuleMob);
  // The mob-specific predicates are carried for provenance and refused: a
  // slime, a wolf and a bat do not spawn until their own rules are written.
  CHECK_EQ(spawn_placement_of(mob("minecraft:slime"))->rule, kSpawnRuleUnmodelled);
  CHECK_EQ(spawn_placement_of(mob("minecraft:wolf"))->rule, kSpawnRuleUnmodelled);
  CHECK_EQ(spawn_placement_of(mob("minecraft:bat"))->rule, kSpawnRuleUnmodelled);

  // `BlockState.isValidSpawn`'s default: a block that collides is a valid ground.
  CHECK(block_is_valid_spawn(state("minecraft:stone"), mob("minecraft:zombie")));
  CHECK(block_is_valid_spawn(state("minecraft:grass_block"), mob("minecraft:pig")));
  CHECK(!block_is_valid_spawn(state("minecraft:air"), mob("minecraft:zombie")));
  CHECK(!block_is_valid_spawn(state("minecraft:water"), mob("minecraft:zombie")));
  // ...and its second clause, `getLightEmission() < 14`: glowstone collides, so
  // the collision half alone would accept it as a floor, and vanilla does not.
  // The number comes from the light engine's own table, so this is exact rather
  // than an approximation of a shape.
  CHECK_EQ(light_emission(state("minecraft:glowstone")), 15);
  CHECK(light_emission(state("minecraft:stone")) < 14);
  CHECK(!block_is_valid_spawn(state("minecraft:glowstone"), mob("minecraft:zombie")));
  // A registered override still wins over the default: a carved pumpkin is an
  // unconditional yes in Blocks.java however it glows.
  CHECK(block_is_valid_spawn(state("minecraft:carved_pumpkin"), mob("minecraft:zombie")));
  // A magma block's own `isValidSpawn` is `entityType.fireImmune()` — the
  // generated `kSpawnRuleFireImmune`, now resolved off the table's own column:
  // it admits a magma cube and refuses a zombie.
  CHECK(block_is_valid_spawn(state("minecraft:magma_block"), mob("minecraft:magma_cube")));
  CHECK(!block_is_valid_spawn(state("minecraft:magma_block"), mob("minecraft:zombie")));

  // `NaturalSpawner.isValidEmptySpawnBlock`: air is empty, a signal source, a
  // fluid, a rail and a burning block are not.
  FakeWorld world;
  const SpawnEnvironment env = world.env([](int) { return 0; }, []() { return 0.0; });
  CHECK(is_valid_empty_spawn_block(env, 0, 0, 0, mob("minecraft:zombie")));
  auto with_block = [&](const char* name) {
    SpawnEnvironment e = env;
    e.get_block = [name](int, int, int) { return state(name); };
    return e;
  };
  CHECK(
      !is_valid_empty_spawn_block(with_block("minecraft:stone"), 0, 0, 0, mob("minecraft:zombie")));
  CHECK(
      !is_valid_empty_spawn_block(with_block("minecraft:water"), 0, 0, 0, mob("minecraft:zombie")));
  CHECK(
      !is_valid_empty_spawn_block(with_block("minecraft:rail"), 0, 0, 0, mob("minecraft:zombie")));
  CHECK(
      !is_valid_empty_spawn_block(with_block("minecraft:fire"), 0, 0, 0, mob("minecraft:zombie")));
  // ...but a fire-immune type may stand in it (`EntityType.isBlockDangerous`).
  // Fire is the one burning block with no collision and no fluid, so it is the
  // only one that ever reaches this check; a magma block or a lava cauldron is
  // refused a step earlier, immune or not.
  CHECK(is_valid_empty_spawn_block(with_block("minecraft:fire"), 0, 0, 0, mob("minecraft:blaze")));
  CHECK(!is_valid_empty_spawn_block(with_block("minecraft:cactus"), 0, 0, 0,
                                    mob("minecraft:zombie")));
  CHECK(!is_valid_empty_spawn_block(with_block("minecraft:oak_button"), 0, 0, 0,
                                    mob("minecraft:zombie")));

  // The categories' own numbers, pinned by name so a reordering of the generated
  // enum and this table cannot pass.
  CHECK_EQ(kMobCategories[kSpawnMonster].max_instances_per_chunk, 70);
  CHECK_EQ(kMobCategories[kSpawnCreature].max_instances_per_chunk, 10);
  CHECK_EQ(kMobCategories[kSpawnMonster].despawn_distance, 128);
  CHECK_EQ(kMobCategories[kSpawnMonster].no_despawn_distance, 32);
  CHECK_EQ(kMobCategories[kSpawnWaterAmbient].despawn_distance, 64);
  CHECK(!kMobCategories[kSpawnMonster].friendly);
  CHECK(kMobCategories[kSpawnCreature].friendly);
  CHECK(kMobCategories[kSpawnCreature].persistent);
}

// ---- the caps ---------------------------------------------------------------

void test_caps() {
  FakeWorld world;
  const SpawnEnvironment env = world.env([](int) { return 0; }, []() { return 0.0; });
  const std::vector<SpawnPlayer> players{watcher(8.5, -60.0, 8.5)};

  // The global cap: `max * chunks / 289` with integer arithmetic, so 289
  // spawnable chunks is exactly one cap of 70 monsters.
  std::vector<SpawnEntity> none;
  SpawnState state = SpawnState::build(289, none, players, env);
  CHECK_EQ(state.spawnable_chunk_count(), 289);
  CHECK(state.can_spawn_for_category_global(kSpawnMonster));
  CHECK_EQ(state.count_of(kSpawnMonster), 0);

  std::vector<SpawnEntity> seventy(
      70, SpawnEntity{"minecraft:zombie", kSpawnMonster, 0, -60, 0, false});
  state = SpawnState::build(289, seventy, players, env);
  CHECK_EQ(state.count_of(kSpawnMonster), 70);
  CHECK(!state.can_spawn_for_category_global(kSpawnMonster)); // 70 < 70 is false
  // The creature cap is its own (10 * 289 / 289 = 10) and the category is empty,
  // so 70 monsters do not block it.
  CHECK_EQ(state.count_of(kSpawnCreature), 0);
  CHECK(state.can_spawn_for_category_global(kSpawnCreature));

  // Half the chunks: half the cap, and 70 monsters now refuse too (35 < 70).
  state = SpawnState::build(144, seventy, players, env);
  CHECK(!state.can_spawn_for_category_global(kSpawnMonster));

  // A persistent mob is not counted at all (`SpawnState.createState` skips it),
  // and neither is a MISC one.
  std::vector<SpawnEntity> persistent(
      70, SpawnEntity{"minecraft:zombie", kSpawnMonster, 0, -60, 0, true});
  state = SpawnState::build(289, persistent, players, env);
  CHECK_EQ(state.count_of(kSpawnMonster), 0);
  CHECK(state.can_spawn_for_category_global(kSpawnMonster));

  // The local cap is one running total per player, not one per chunk: a player
  // with 70 monsters *nearby* blocks the category in every chunk they watch,
  // even one they have no mob in.
  // (All 70 sit inside chunk (0, 0), the chunk the player is watching, so each
  // is counted in that player's own running total.)
  std::vector<SpawnEntity> spread;
  for (int i = 0; i < 70; ++i)
    spread.push_back(
        SpawnEntity{"minecraft:zombie", kSpawnMonster, i % 16, -60, (i / 16) * 2, false});
  state = SpawnState::build(289, spread, players, env);
  CHECK(!state.can_spawn_for_category_local(kSpawnMonster, 0, 0));
  CHECK(!state.can_spawn_for_category_local(kSpawnMonster, 4, 4));

  // A player who is not close enough to the chunk does not count at all — the
  // answer is then false, which is why a mob cannot spawn in a chunk nobody is
  // watching.
  CHECK(!state.can_spawn_for_category_local(kSpawnMonster, 60, 60));
  // And an empty category passes.
  CHECK(state.can_spawn_for_category_local(kSpawnCreature, 0, 0));

  // A spectator is not a player as far as the local cap is concerned.
  std::vector<SpawnPlayer> spectator_only{watcher(8.5, -60.0, 8.5, /*spectator=*/true)};
  SpawnState spec = SpawnState::build(289, spread, spectator_only, env);
  CHECK(!spec.can_spawn_for_category_local(kSpawnMonster, 0, 0));

  // `PotentialCalculator.getPotentialEnergyChange`: `sum(charge / dist) *
  // charge`, and a point charge on the candidate position is infinite.
  std::vector<SpawnEntity> charged;
  SpawnState charged_state = SpawnState::build(289, charged, players, env);
  CHECK_EQ(charged_state.charge_count(), size_t(0));
  // (The two biomes that set a cost are not the fake world's biome, so no charge
  // is banked here; the arithmetic is tested directly below.)
  CHECK_EQ(charged_state.potential_energy_change(0, 0, 0, 0.0), 0.0);

  // `after_spawn` banks a charge for a type the biome charges for.
  FakeWorld charged_world = world;
  charged_world.biome = [](int, int, int) { return std::string("minecraft:soul_sand_valley"); };
  const SpawnEnvironment soul_env = charged_world.env([](int) { return 0; }, []() { return 0.0; });
  SpawnState soul = SpawnState::build(289, charged, players, soul_env);
  const GeneratedMob& ghast = mob("minecraft:zombie"); // any type name works for the lookup
  (void)ghast;
  soul.after_spawn(mob("minecraft:zombie"), 0, -60, 0, soul_env);
  CHECK_EQ(soul.count_of(kSpawnMonster), 1);
  // The charge is only banked when the biome names the *type*; zombie is not a
  // cost-bearing type in soul sand valley (the ghast is), so nothing was banked
  // and a candidate far away still passes.
  CHECK(soul.can_spawn(mob("minecraft:zombie"), 200, -60, 0, soul_env));
}

// ---- the gates --------------------------------------------------------------

void test_light_gate() {
  FakeWorld world;
  // A monster's gate draws twice: the sky layer against `nextInt(32)` first,
  // then the raw brightness against `nextInt(8)`. The block-layer test is the
  // dimension's `monsterSpawnBlockLightLimit` of 0 compared against the layer
  // itself, with no draw at all — so a scripted sequence is two entries long.
  {
    Script script;
    script.ints = {10, 0}; // sky draw 10: 15 > 10 refuses, and stops there
    SpawnEnvironment e = world.env(script.next_int(), script.unit_random());
    e.light_level = [](LightLayer layer, int, int, int) {
      return layer == LightLayer::kSky ? 15 : 0;
    };
    CHECK(!is_dark_enough_to_spawn(e, 0, 0, 0));
    CHECK_EQ(script.ints.size(), size_t(1)); // the first draw ended it
  }
  {
    // Block light 1 > the limit of 0 refuses, once the sky draw let it through,
    // and the raw-brightness draw is never made.
    Script script;
    script.ints = {31, 5};
    SpawnEnvironment e = world.env(script.next_int(), script.unit_random());
    e.light_level = [](LightLayer layer, int, int, int) {
      return layer == LightLayer::kSky ? 15 : 1;
    };
    CHECK(!is_dark_enough_to_spawn(e, 0, 0, 0));
    CHECK_EQ(script.ints.size(), size_t(1)); // the second draw was never reached
  }
  {
    // Daylight in the open: sky 15, no block light, and the raw brightness of 15
    // is above any `nextInt(8)` — so the two draws are both made and both fail
    // the test.
    Script script;
    script.ints = {31, 7};
    SpawnEnvironment e = world.env(script.next_int(), script.unit_random());
    e.light_level = [](LightLayer layer, int, int, int) {
      return layer == LightLayer::kSky ? 15 : 0;
    };
    CHECK(!is_dark_enough_to_spawn(e, 0, 0, 0));
    CHECK(script.ints.empty());
  }
  {
    // A sealed dark cell: sky 0 and block 0 pass whatever the draws are, because
    // `0 > nextInt(32)` cannot be true.
    Seeded seeded;
    SpawnEnvironment e = world.env(seeded.next_int(), seeded.unit_random());
    e.light_level = [](LightLayer, int, int, int) { return 0; };
    for (int i = 0; i < 20; ++i)
      CHECK(is_dark_enough_to_spawn(e, 0, 0, 0));
  }
  {
    // Thundering: `getMaxLocalRawBrightness(pos, 10)`, i.e. the sky darkened by
    // ten, so a sky-lit cell at 5 passes where the same cell at noon (darken 0,
    // raw 15) does not.
    Script script;
    script.ints = {31, 5}; // sky draw 31 lets it through; raw 15 - 10 = 5 <= 5
    SpawnEnvironment e = world.env(script.next_int(), script.unit_random());
    e.light_level = [](LightLayer layer, int, int, int) {
      return layer == LightLayer::kSky ? 15 : 0;
    };
    e.thundering = true;
    CHECK(is_dark_enough_to_spawn(e, 0, 0, 0));

    // The same cell at noon: darken 0 leaves the raw brightness at 15, above the
    // draw of 5.
    Script noon;
    noon.ints = {31, 5};
    SpawnEnvironment e2 = world.env(noon.next_int(), noon.unit_random());
    e2.light_level = e.light_level;
    CHECK(!is_dark_enough_to_spawn(e2, 0, 0, 0));
  }
  {
    // `raw_brightness` itself: `max(block, sky - darken)`.
    SpawnEnvironment e = world.env([](int) { return 0; }, []() { return 0.0; });
    e.light_level = [](LightLayer layer, int, int, int) {
      return layer == LightLayer::kSky ? 12 : 4;
    };
    CHECK_EQ(raw_brightness(e, 0, 0, 0, 0), 12);
    CHECK_EQ(raw_brightness(e, 0, 0, 0, 11), 4); // max(4, 12 - 11)
    CHECK_EQ(raw_brightness(e, 0, 0, 0, 20), 4); // max(4, -8)
  }

  // The animal gate: open sky (raw 15 > 8) *and* an `animals_spawnable_on`
  // block below.
  FakeWorld animal_world;
  animal_world.block = [](int, int y, int) {
    return y <= -61 ? state("minecraft:grass_block") : state("minecraft:air");
  };
  animal_world.light = [](LightLayer layer, int, int, int) {
    return layer == LightLayer::kSky ? 15 : 0;
  };
  const SpawnEnvironment animal_env = animal_world.env([](int) { return 0; }, []() { return 0.0; });
  CHECK(check_spawn_rules(animal_env, mob("minecraft:pig"), 0, -60, 0));
  // The same cell over stone is refused however bright it is.
  FakeWorld stone_world = animal_world;
  stone_world.block = [](int, int y, int) {
    return y <= -61 ? state("minecraft:stone") : state("minecraft:air");
  };
  const SpawnEnvironment stone_env = stone_world.env([](int) { return 0; }, []() { return 0.0; });
  CHECK(!check_spawn_rules(stone_env, mob("minecraft:pig"), 0, -60, 0));
  // And grass in the dark is refused too (raw 0 is not > 8).
  FakeWorld dark_grass = animal_world;
  dark_grass.light = [](LightLayer, int, int, int) { return 0; };
  const SpawnEnvironment dark_env = dark_grass.env([](int) { return 0; }, []() { return 0.0; });
  CHECK(!check_spawn_rules(dark_env, mob("minecraft:pig"), 0, -60, 0));

  // A monster needs the same ground rule plus darkness, so a zombie on grass at
  // noon is refused by the light and accepted in the dark.
  CHECK(!check_spawn_rules(animal_env, mob("minecraft:zombie"), 0, -60, 0));
  CHECK(check_spawn_rules(dark_env, mob("minecraft:zombie"), 0, -60, 0));

  // `SpawnPlacements.checkSpawnRules` for a type nobody registered: vanilla's
  // absent `Data` means "no predicate", so the answer is yes — it does *not*
  // fall through to `Mob::checkMobSpawnRules`. The contrast is the same call
  // over an all-air world, where a registered mob's ground rule refuses.
  FakeWorld air_world;
  const SpawnEnvironment air_env = air_world.env([](int) { return 0; }, []() { return 0.0; });
  CHECK(!check_spawn_rules(air_env, mob("minecraft:zombie"), 0, -60, 0));
  GeneratedMob unregistered{};
  unregistered.name = "minecraft:test_mob";
  CHECK(spawn_placement_of(unregistered) == nullptr);
  CHECK(check_spawn_rules(air_env, unregistered, 0, -60, 0));
}

void test_placement_and_distances() {
  FakeWorld world;
  // `SpawnPlacements.isSpawnPositionOk` for ON_GROUND: a valid ground below and
  // the cell plus the one above it empty.
  FakeWorld ground;
  ground.block = [](int, int y, int) {
    return y <= -61 ? state("minecraft:stone") : state("minecraft:air");
  };
  const SpawnEnvironment env = ground.env([](int) { return 0; }, []() { return 0.0; });
  CHECK(spawn_position_ok(env, mob("minecraft:zombie"), 0, -60, 0));
  CHECK(!spawn_position_ok(env, mob("minecraft:zombie"), 0, -61, 0)); // inside the floor
  CHECK(!spawn_position_ok(env, mob("minecraft:zombie"), 0, -59, 0)); // no ground below
  // A roof one cell above refuses it: the box would not fit.
  FakeWorld roofed = ground;
  roofed.block = [](int, int y, int) {
    if (y <= -61)
      return state("minecraft:stone");
    return y == -59 ? state("minecraft:stone") : state("minecraft:air");
  };
  const SpawnEnvironment roofed_env = roofed.env([](int) { return 0; }, []() { return 0.0; });
  CHECK(!spawn_position_ok(roofed_env, mob("minecraft:zombie"), 0, -60, 0));

  // The box test: a wall inside the mob's own width is an obstruction, and the
  // fluid half only matters for `Mob.checkSpawnObstruction`.
  CHECK(spawn_box_is_clear(env, mob("minecraft:zombie"), 0.5, -60.0, 0.5, false));
  // A wall one block east of a mob standing at x = 1.5 is inside its 0.6-wide
  // box (1.2 .. 1.8); the same wall is *outside* the box of a mob at x = 0.5
  // (0.2 .. 0.8), which is the half-block of clearance a mob's width buys.
  FakeWorld wall = ground;
  wall.block = [](int x, int y, int z) {
    if (y <= -61)
      return state("minecraft:stone");
    return (x == 1 && z == 0) ? state("minecraft:stone") : state("minecraft:air");
  };
  const SpawnEnvironment wall_env = wall.env([](int) { return 0; }, []() { return 0.0; });
  CHECK(!spawn_box_is_clear(wall_env, mob("minecraft:zombie"), 1.5, -60.0, 0.5, false));
  CHECK(spawn_box_is_clear(wall_env, mob("minecraft:zombie"), 0.5, -60.0, 0.5, false));
  FakeWorld wet = ground;
  wet.block = [](int, int y, int) {
    if (y <= -61)
      return state("minecraft:stone");
    return y == -60 ? state("minecraft:water") : state("minecraft:air");
  };
  const SpawnEnvironment wet_env = wet.env([](int) { return 0; }, []() { return 0.0; });
  CHECK(spawn_box_is_clear(wet_env, mob("minecraft:zombie"), 0.5, -60.0, 0.5, false));
  CHECK(!spawn_box_is_clear(wet_env, mob("minecraft:zombie"), 0.5, -60.0, 0.5, true));

  // `isRightDistanceToPlayerAndSpawnPoint`: 24 blocks from the player, 24 from
  // the world spawn, and in a chunk that is ticking for entities.
  std::vector<SpawnPlayer> players{watcher(0.5, -60.0, 0.5)};
  const double near = nearest_player_distance_sqr(players, 9, -60, 0); // 9 blocks east
  CHECK(near < 576.0);
  CHECK(!right_distance_to_player_and_spawn_point(env, players, 9, -60, 0, near));
  const double far = nearest_player_distance_sqr(players, 40, -60, 0); // 40 east
  CHECK(far > 576.0);
  CHECK(right_distance_to_player_and_spawn_point(env, players, 40, -60, 0, far));
  // Exactly 24 blocks is refused (`<= 576`), 25 is not.
  CHECK(!right_distance_to_player_and_spawn_point(
      env, players, 24, -60, 0, nearest_player_distance_sqr(players, 24, -60, 0)));
  CHECK(right_distance_to_player_and_spawn_point(env, players, 25, -60, 0,
                                                 nearest_player_distance_sqr(players, 25, -60, 0)));
  // The spawn point's own 24-block clearance, with no player in the world at all
  // (`nearestPlayerDistanceSqr` is a large sentinel, so only the spawn point's
  // own test can refuse).
  std::vector<SpawnPlayer> none;
  FakeWorld near_spawn = ground;
  near_spawn.spawn_point_x = 8.5;
  near_spawn.spawn_point_y = -60.0;
  near_spawn.spawn_point_z = 8.5;
  const SpawnEnvironment spawn_env = near_spawn.env([](int) { return 0; }, []() { return 0.0; });
  CHECK(!right_distance_to_player_and_spawn_point(spawn_env, none, 8, -60, 8, 10000.0));
  CHECK(right_distance_to_player_and_spawn_point(spawn_env, none, 40, -60, 40, 10000.0));
  // A chunk the world cannot tick refuses.
  FakeWorld no_chunk;
  no_chunk.chunk_exists = false;
  const SpawnEnvironment no_chunk_env = no_chunk.env([](int) { return 0; }, []() { return 0.0; });
  CHECK(!right_distance_to_player_and_spawn_point(no_chunk_env, none, 40, -60, 40, 10000.0));
  // A spectator is not a nearest player at all.
  std::vector<SpawnPlayer> spectators{watcher(0.5, -60.0, 0.5, /*spectator=*/true)};
  CHECK_EQ(nearest_player_distance_sqr(spectators, 9, -60, 0), -1.0);
}

// ---- the pass ---------------------------------------------------------------

void test_the_pass() {
  // A dark sealed platform: a stone floor, two cells of air, no light at all
  // (sky 0 and block 0, which is what a sealed room reads). Every cell of the
  // chunk is a valid spawn position, so the pass must find monsters — and only
  // monsters, because the biome lists them and the light refuses nothing.
  const auto plains = [](int, int, int) { return std::string("minecraft:plains"); };
  // The list the pick draws from, so the spawn can be held to the biome's own
  // entries rather than to a hard-coded pair of names.
  const BiomeSpawnList plains_monsters = biome_spawn_list("minecraft:plains", kSpawnMonster);
  {
    Seeded seeded;
    const SpawnEnvironment env = platform_env(seeded, /*floor_y=*/-61, /*sky=*/0,
                                              /*block_light=*/0, plains);
    // The watcher is 40 blocks east of the chunk's middle, so every position in
    // it is beyond the 24-block minimum.
    const std::vector<SpawnPlayer> players{watcher(40.5, -60.0, 8.5)};
    std::vector<SpawnEntity> none;
    SpawnState state = SpawnState::build(289, none, players, env);
    const std::vector<uint8_t> categories =
        filtered_spawning_categories(state, /*spawn_enemies=*/true, /*spawn_persistent=*/false);
    // Every category but the persistent one is in the list, in `values()` order:
    // the plains biome's ambient and water entries resolve to spawn-rule
    // predicates this engine does not model (a bat's, a glow squid's), so only
    // MONSTER can actually produce anything here.
    CHECK_EQ(categories.size(), size_t(6));
    CHECK_EQ(categories[0], kSpawnMonster);

    int spawned = 0;
    for (int tick = 0; tick < 400 && spawned == 0; ++tick) {
      std::vector<SpawnedMob> here = spawn_for_chunk(state, categories, 0, 0, env);
      for (const SpawnedMob& m : here) {
        ++spawned;
        CHECK(m.type != nullptr);
        CHECK_EQ(m.type->category, kSpawnMonster);
        // The type is one of the plains list's own entries — the pick cannot
        // invent a mob the biome does not name.
        bool listed = false;
        for (int i = 0; i < plains_monsters.count; ++i)
          listed = listed || m.type->name == std::string(plains_monsters.entries[i].entity);
        CHECK(listed);
        // The spawn always lands on the floor's first air cell. The *walk* can
        // leave the starting chunk (vanilla's group walks up to five cells per
        // axis), so x/z are deliberately not bounded to the chunk here.
        CHECK(std::fabs(m.y - (-61.0 + 1.0)) < 1e-9);
      }
    }
    CHECK(spawned > 0);
  }
  {
    // The same platform, lit: one glowstone's worth of block light in the air
    // cells. `Monster.isDarkEnoughToSpawn` refuses every column of the chunk
    // (block light 15 > the dimension's limit of 0), so 400 passes spawn
    // nothing at all.
    Seeded seeded;
    const SpawnEnvironment env = platform_env(seeded, /*floor_y=*/-61, /*sky=*/0,
                                              /*block_light=*/15, plains);
    const std::vector<SpawnPlayer> players{watcher(40.5, -60.0, 8.5)};
    std::vector<SpawnEntity> none;
    SpawnState state = SpawnState::build(289, none, players, env);
    const std::vector<uint8_t> categories = filtered_spawning_categories(state, true, false);
    for (int tick = 0; tick < 400; ++tick) {
      const std::vector<SpawnedMob> here = spawn_for_chunk(state, categories, 0, 0, env);
      CHECK(here.empty());
    }
  }
  {
    // A biome with no monster list spawns nothing however dark it is: the
    // in-memory world's `minecraft:the_void` is the case that matters, because
    // it is what a chunk this server made from nothing carries.
    Seeded seeded;
    const SpawnEnvironment env =
        platform_env(seeded, /*floor_y=*/-61, /*sky=*/0, /*block_light=*/0,
                     [](int, int, int) { return std::string("minecraft:the_void"); });
    const std::vector<SpawnPlayer> players{watcher(40.5, -60.0, 8.5)};
    std::vector<SpawnEntity> none;
    SpawnState state = SpawnState::build(289, none, players, env);
    const std::vector<uint8_t> categories = filtered_spawning_categories(state, true, false);
    for (int tick = 0; tick < 200; ++tick)
      CHECK(spawn_for_chunk(state, categories, 0, 0, env).empty());
  }
  {
    // The player's own 24 blocks: with the watcher standing *on* the platform
    // every candidate is inside 24 blocks of them, so the pass refuses them all.
    Seeded seeded;
    const SpawnEnvironment env = platform_env(seeded, -61, 0, 0, plains);
    const std::vector<SpawnPlayer> players{watcher(8.5, -60.0, 8.5)};
    std::vector<SpawnEntity> none;
    SpawnState state = SpawnState::build(289, none, players, env);
    const std::vector<uint8_t> categories = filtered_spawning_categories(state, true, false);
    for (int tick = 0; tick < 200; ++tick)
      CHECK(spawn_for_chunk(state, categories, 0, 0, env).empty());
  }
  {
    // The persistent categories only spawn on the 400-tick pass: CREATURE is
    // filtered out of an ordinary tick and comes back on the persistent one.
    Seeded seeded;
    const SpawnEnvironment env = platform_env(seeded, -61, 15, 0, plains);
    const std::vector<SpawnPlayer> players{watcher(40.5, -60.0, 8.5)};
    std::vector<SpawnEntity> none;
    SpawnState state = SpawnState::build(289, none, players, env);
    // CREATURE is the only persistent category, so it is the only one an
    // ordinary tick drops; adding it back is what the 400-tick pass is.
    const std::vector<uint8_t> ordinary = filtered_spawning_categories(state, true, false);
    const std::vector<uint8_t> persistent = filtered_spawning_categories(state, true, true);
    CHECK_EQ(ordinary.size(), size_t(6));
    CHECK_EQ(persistent.size(), size_t(7));
    bool creature_seen = false;
    for (uint8_t c : ordinary)
      if (c == kSpawnCreature)
        creature_seen = true;
    CHECK(!creature_seen);
    creature_seen = false;
    for (uint8_t c : persistent)
      if (c == kSpawnCreature)
        creature_seen = true;
    CHECK(creature_seen);
    // With enemies off the monsters go and the friendly categories stay.
    const std::vector<uint8_t> peaceful = filtered_spawning_categories(state, false, true);
    CHECK_EQ(peaceful.size(), size_t(6));
    for (uint8_t c : peaceful)
      CHECK(c != kSpawnMonster);
  }
  {
    // The global cap holds *within* a tick: `after_spawn` counts each one, so a
    // chunk that keeps producing stops at the cap.
    Seeded seeded;
    const SpawnEnvironment env = platform_env(seeded, -61, 0, 0, plains);
    const std::vector<SpawnPlayer> players{watcher(40.5, -60.0, 8.5)};
    // 289 spawnable chunks is a cap of 70 monsters; the pass over one chunk can
    // add at most four per tick, so after 20 ticks the count has moved and the
    // local/global checks still hold.
    std::vector<SpawnEntity> none;
    SpawnState state = SpawnState::build(289, none, players, env);
    const std::vector<uint8_t> categories = filtered_spawning_categories(state, true, false);
    int total = 0;
    for (int tick = 0; tick < 20; ++tick)
      total += static_cast<int>(spawn_for_chunk(state, categories, 0, 0, env).size());
    CHECK_EQ(state.count_of(kSpawnMonster), total);
    CHECK(total > 0);
    CHECK(total <= 80); // four per chunk attempt, twenty attempts at most
  }
  {
    // The spawn-point clearance is real: the same dark platform, with the world
    // spawn sitting in the middle of it, spawns nothing.
    Seeded seeded;
    SpawnEnvironment env = platform_env(seeded, -61, 0, 0, plains);
    env.spawn_point_x = 8.5;
    env.spawn_point_y = -60.0;
    env.spawn_point_z = 8.5;
    const std::vector<SpawnPlayer> players{watcher(40.5, -60.0, 8.5)};
    std::vector<SpawnEntity> none;
    SpawnState state = SpawnState::build(289, none, players, env);
    const std::vector<uint8_t> categories = filtered_spawning_categories(state, true, false);
    for (int tick = 0; tick < 200; ++tick)
      CHECK(spawn_for_chunk(state, categories, 0, 0, env).empty());
  }
  {
    // `getRandomPosWithin`'s floor: a start below `minY + 1` abandons the whole
    // attempt, and a position the world cannot describe at all (no surface)
    // never starts one.
    Script script;
    script.ints = {0, 0, 0}; // y = minY = -64 < minY + 1
    FakeWorld world;
    const SpawnEnvironment env = world.env(script.next_int(), script.unit_random());
    std::vector<SpawnEntity> none;
    SpawnState state = SpawnState::build(289, none, {watcher(40.5, -60.0, 8.5)}, env);
    CHECK(spawn_category_for_chunk(state, kSpawnMonster, 0, 0, env).empty());
    SpawnEnvironment no_height = env;
    no_height.world_surface_height = nullptr;
    CHECK(spawn_category_for_chunk(state, kSpawnMonster, 0, 0, no_height).empty());
  }
  {
    // A full collision shape where the attempt starts refuses the whole attempt
    // (`state.isRedstoneConductor`), before any group is drawn.
    Script script;
    script.ints = {8, 8, 4};
    FakeWorld solid;
    solid.block = [](int, int, int) { return state("minecraft:stone"); };
    const SpawnEnvironment env = solid.env(script.next_int(), script.unit_random());
    std::vector<SpawnEntity> none;
    SpawnState state = SpawnState::build(289, none, {watcher(40.5, -60.0, 8.5)}, env);
    CHECK(spawn_category_for_position(state, kSpawnMonster, 8, -60, 8, env).empty());
  }
}

// ---- the pass's cost ------------------------------------------------------

void test_pass_cost() {
  // The pass walks every candidate chunk every tick, so the price of one chunk
  // attempt is the milestone's cost model. This is a *measurement*, not an
  // assertion: 289 chunks is one player's `SPAWN_DISTANCE_CHUNK` square — what
  // `getNaturalSpawnChunkCount` counts and what the global cap scales by — and
  // the number it prints is what docs/PHASE4.md quotes.
  Seeded seeded;
  const SpawnEnvironment env = platform_env(
      seeded, -61, 0, 0, [](int, int, int) { return std::string("minecraft:plains"); });
  const std::vector<SpawnPlayer> players{watcher(40.5, -60.0, 8.5)};
  std::vector<SpawnEntity> none;
  const int chunks = kSpawnDistanceChunk * 2 + 1; // 17x17
  // One `SpawnState` per tick and one local-cap check per category per chunk,
  // which is the shape the session drives: the state is *not* rebuilt per chunk.
  SpawnState chunk_state = SpawnState::build(289, none, players, env);
  const std::vector<uint8_t> categories = filtered_spawning_categories(chunk_state, true, false);
  const auto start = std::chrono::steady_clock::now();
  int spawned = 0;
  for (int cx = 0; cx < chunks; ++cx)
    for (int cz = 0; cz < chunks; ++cz)
      spawned += static_cast<int>(spawn_for_chunk(chunk_state, categories, cx, cz, env).size());
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  std::printf("mob_spawn: %d chunk attempts over a predicate world in %.1f ms "
              "(%.3f ms each), %d spawns\n",
              chunks * chunks, ms, ms / static_cast<double>(chunks * chunks), spawned);
  CHECK(spawned > 0);

  // ... and against a real world, where each cell the attempt asks about is a
  // locked column read and a palette lookup rather than a lambda. This is the
  // number that matters: the pass's arithmetic is cheap, the world's reads are
  // not, and the pass is what pays for them.
  MemoryWorld world;
  for (int cx = -1; cx <= 1; ++cx)
    for (int cz = -1; cz <= 1; ++cz) {
      world.modify_chunk(cx, cz);
      for (int x = 0; x < 16; ++x)
        for (int z = 0; z < 16; ++z) {
          world.set_block_state(cx, cz, x, -64, z, state("minecraft:bedrock"));
          world.set_block_state(cx, cz, x, -63, z, state("minecraft:dirt"));
          world.set_block_state(cx, cz, x, -62, z, state("minecraft:dirt"));
          world.set_block_state(cx, cz, x, -61, z, state("minecraft:grass_block"));
        }
    }
  SpawnEnvironment real;
  real.get_block = [&world](int x, int y, int z) {
    const int cx = x >= 0 ? x / 16 : (x - 15) / 16;
    const int cz = z >= 0 ? z / 16 : (z - 15) / 16;
    return world.get_block_state(cx, cz, x - cx * 16, y, z - cz * 16);
  };
  real.light_level = [&world](LightLayer layer, int x, int y, int z) {
    return world.light_level(layer, x, y, z);
  };
  real.biome_name = [&world](int x, int y, int z) { return world.biome_name_at(x, y, z); };
  real.world_surface_height = [&world](int x, int z) { return world.world_surface_height(x, z); };
  real.can_spawn_entities_in_chunk = [&world](int cx, int cz) {
    return world.load_chunk(cx, cz).has_value();
  };
  real.spawn_point_x = 1000.0;
  real.spawn_point_y = 1000.0;
  real.spawn_point_z = 1000.0;
  real.min_y = -64;
  real.next_int = seeded.next_int();
  real.unit_random = seeded.unit_random();
  SpawnState real_state = SpawnState::build(289, none, players, real);
  const std::vector<uint8_t> real_categories =
      filtered_spawning_categories(real_state, true, false);
  const auto real_start = std::chrono::steady_clock::now();
  int real_spawned = 0;
  for (int cx = 0; cx < chunks; ++cx)
    for (int cz = 0; cz < chunks; ++cz)
      real_spawned +=
          static_cast<int>(spawn_for_chunk(real_state, real_categories, cx, cz, real).size());
  const double real_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - real_start)
          .count();
  std::printf("mob_spawn: the same %d attempts over a MemoryWorld in %.1f ms "
              "(%.3f ms each), %d spawns\n",
              chunks * chunks, real_ms, real_ms / static_cast<double>(chunks * chunks),
              real_spawned);
}

// ---- despawning -------------------------------------------------------------

Mob make_mob(const char* name, double x, double y, double z) {
  Mob m;
  MobEnvironment env;
  env.unit_random = []() { return 0.0; };
  mob_place(m, mob(name), env, x, y, z, 0.0f);
  m.entity_id = 7;
  return m;
}

void test_despawn() {
  MobEnvironment env;
  env.unit_random = []() { return 0.0; };
  env.players.push_back(MobPlayer{1, 0.0, -60.0, 0.0, 0.6f, 1.8f, 1.62f, true, false});

  // `Mob.checkDespawn`'s instant branch: further than the category's 128 blocks
  // and `removeWhenFarAway` true — which is every monster.
  Mob zombie = make_mob("minecraft:zombie", 200.0, -60.0, 0.0);
  CHECK(check_despawn(zombie, env));
  // An Animal's `removeWhenFarAway` is false, so it is kept however far it
  // wanders — at any distance and whatever the activity counter says.
  Mob pig = make_mob("minecraft:pig", 200.0, -60.0, 0.0);
  pig.no_action_time = 100000;
  CHECK(!check_despawn(pig, env));

  // Inside 128: the slow branch. `noActionTime > 600` *and* a 1-in-800 roll
  // *and* beyond the 32-block no-despawn distance.
  Mob idle = make_mob("minecraft:zombie", 40.0, -60.0, 0.0);
  idle.no_action_time = 601;
  env.unit_random = []() { return 0.0; }; // the roll lands
  CHECK(check_despawn(idle, env));
  idle = make_mob("minecraft:zombie", 40.0, -60.0, 0.0);
  idle.no_action_time = 600; // strictly greater than 600, so this one is kept
  CHECK(!check_despawn(idle, env));
  CHECK_EQ(idle.no_action_time, 600); // and not reset: it is beyond 32 blocks
  idle = make_mob("minecraft:zombie", 40.0, -60.0, 0.0);
  idle.no_action_time = 601;
  env.unit_random = []() { return 0.5; }; // 0.5 is not < 1/800
  CHECK(!check_despawn(idle, env));

  // Inside the 32-block no-despawn distance the counter is zeroed and nothing
  // despawns, however long the mob has been idle.
  Mob close = make_mob("minecraft:zombie", 10.0, -60.0, 0.0);
  close.no_action_time = 100000;
  env.unit_random = []() { return 0.0; };
  CHECK(!check_despawn(close, env));
  CHECK_EQ(close.no_action_time, 0);

  // A mob that must persist is kept and its counter zeroed.
  Mob tagged = make_mob("minecraft:zombie", 200.0, -60.0, 0.0);
  tagged.persistence_required = true;
  tagged.no_action_time = 999;
  CHECK(!check_despawn(tagged, env));
  CHECK_EQ(tagged.no_action_time, 0);

  // Spectators do not keep a mob alive: the nearest-player lookup skips them,
  // so a mob with only a spectator beside it is as good as unobserved.
  MobEnvironment spectated;
  spectated.players.push_back(MobPlayer{1, 0.0, -60.0, 0.0, 0.6f, 1.8f, 1.62f, false, true});
  spectated.unit_random = []() { return 0.0; };
  Mob abandoned = make_mob("minecraft:zombie", 0.0, -60.0, 0.0);
  CHECK(!check_despawn(abandoned, spectated)); // no non-spectator player at all
  Mob distant = make_mob("minecraft:zombie", 200.0, -60.0, 0.0);
  CHECK(!check_despawn(distant, spectated));

  // And through the tick: a discard is reported as a despawn — never as a death,
  // so no loot, no experience and no death event a client would draw.
  {
    MobEnvironment tick_env;
    tick_env.players.push_back(MobPlayer{1, 0.0, -60.0, 0.0, 0.6f, 1.8f, 1.62f, true, false});
    tick_env.unit_random = []() { return 0.0; };
    std::vector<Mob> mobs;
    mobs.push_back(make_mob("minecraft:zombie", 200.0, -60.0, 0.0));
    mobs.push_back(make_mob("minecraft:zombie", 5.0, -60.0, 0.0));
    mobs[1].entity_id = 8;
    const MobTickReport report = tick_mobs(mobs, tick_env);
    CHECK_EQ(report.despawned.size(), size_t(1));
    CHECK_EQ(report.despawned[0], 7);
    CHECK(report.deaths.empty());
    CHECK_EQ(mobs.size(), size_t(1));
    CHECK_EQ(mobs[0].entity_id, 8);
    // A kept mob's counter advanced by exactly one tick.
    CHECK_EQ(mobs[0].no_action_time, 1);
  }
  {
    // `noActionTime` is what the slow branch counts, so a mob that goes idle on
    // its own eventually reaches the roll. The zombie stands in a solid world so
    // it cannot stroll out of the 32..128 band, and the roll is scripted to land
    // on the tick the counter passes 600.
    MobEnvironment tick_env;
    tick_env.players.push_back(MobPlayer{1, 120.0, -60.0, 0.0, 0.6f, 1.8f, 1.62f, true, false});
    tick_env.is_solid = [](int, int, int) { return true; };
    bool roll_lands = false;
    tick_env.unit_random = [&roll_lands]() { return roll_lands ? 0.0 : 1.0; };
    std::vector<Mob> mobs;
    mobs.push_back(make_mob("minecraft:zombie", 0.0, -60.0, 0.0));
    int discarded_at = -1;
    for (int tick = 0; tick < 900 && discarded_at < 0; ++tick) {
      roll_lands = tick >= 700;
      const MobTickReport report = tick_mobs(mobs, tick_env);
      if (!report.despawned.empty())
        discarded_at = tick;
    }
    // The counter is checked *before* it is bumped, so the discard lands on the
    // seventh hundredth tick rather than the sixth, and never earlier: nothing
    // else in the band could have taken it.
    CHECK_EQ(discarded_at, 700);
    CHECK(mobs.empty());
  }
}

// ---- the golden trace -------------------------------------------------------

std::vector<std::string> split_tabs(const std::string& line) {
  std::vector<std::string> fields;
  size_t pos = 0;
  while (pos <= line.size()) {
    const size_t tab = line.find('\t', pos);
    if (tab == std::string::npos) {
      fields.push_back(line.substr(pos));
      break;
    }
    fields.push_back(line.substr(pos, tab - pos));
    pos = tab + 1;
  }
  return fields;
}

// A fixture file's data rows, i.e. everything that is not a `#` comment. Returns
// false when the file cannot be opened, so a missing fixture fails loudly instead
// of comparing against nothing.
bool read_fixture_rows(const std::string& path, std::vector<std::string>* rows) {
  std::ifstream file(path);
  if (!file)
    return false;
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;
    rows->push_back(line);
  }
  return true;
}

// The `name[properties]` form the scene file writes, back to a state.
BlockStateKey parse_state(const std::string& text) {
  const size_t open = text.find('[');
  if (open == std::string::npos)
    return state(text.c_str());
  return BlockStateKey{text.substr(0, open), text.substr(open + 1, text.size() - open - 2)};
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

uint8_t category_of_name(const std::string& name) {
  for (uint8_t i = 0; i < kSpawnCategoryCount; ++i)
    if (name == kMobSpawnCategoryNames[i])
      return i;
  return kSpawnMisc;
}

struct RecordedRun {
  int64_t seed = 42;
  std::string name;
  std::string scene;
  int32_t chunk_x = 0;
  int32_t chunk_z = 0;
  uint8_t category = kSpawnMonster;
  bool spawn_enemies = true;
  bool spawn_persistent = false;
  int ticks = 0; // passes over the chunk, each one a `spawnForChunk`, through one state
  int spawnable_chunks = 289;
  int min_y = -64;
  int sky_darken = 0;
  bool thundering = false;
  bool can_see_sky = false;
  double spawn_point_x = 0.0;
  double spawn_point_y = 0.0;
  double spawn_point_z = 0.0;
  std::string categories;
  size_t draws = 0;
  size_t units = 0;
  size_t spawns = 0;
  size_t rows = 0;
};

struct RecordedScene {
  std::string name;
  std::string biome;
  int default_surface = 0;
  int default_sky = 0;
  int default_block_light = 0;
  bool can_see_sky = false;
  std::vector<std::tuple<int, int, int, BlockStateKey>> blocks; // x, y, z, state
  std::vector<std::tuple<bool, int, int, int, int>> lights;     // sky, x, y, z, level
  // The population, built after every row is parsed: `SpawnEntity::type_name` is a
  // pointer, so it has to point into a string list that is complete and will not
  // be reallocated again.
  std::vector<std::string> entity_types;
  std::vector<std::tuple<uint8_t, int, int, int, bool>> entity_specs;
  std::vector<SpawnEntity> entities;
  std::vector<SpawnPlayer> players;

  void finish_entities() {
    entities.clear();
    for (size_t i = 0; i < entity_specs.size(); ++i)
      entities.push_back(SpawnEntity{entity_types[i].c_str(), std::get<0>(entity_specs[i]),
                                     std::get<1>(entity_specs[i]), std::get<2>(entity_specs[i]),
                                     std::get<3>(entity_specs[i]), std::get<4>(entity_specs[i])});
  }
};

struct SpawnReplay {
  std::vector<std::string> rows;
  std::string categories;
  size_t draws = 0;
  size_t units = 0;
  size_t spawns = 0;
};

std::string category_names(const std::vector<uint8_t>& categories) {
  std::string text;
  for (size_t i = 0; i < categories.size(); ++i) {
    if (i != 0)
      text += ',';
    text += kMobSpawnCategoryNames[categories[i]];
  }
  return text;
}

// Replays one recorded run through the real pass, with the level's own stream
// seeded the way the generator seeded it, logging every interaction the pass has
// with the world and every draw it takes.
SpawnReplay replay_spawn_run(const RecordedRun& run, const RecordedScene& scene) {
  SpawnReplay replay;
  // The tick every logger below stamps its rows with, moved by the loop at the
  // end: the fixture's rows carry it, so a replay that ran a different number of
  // ticks, or a gate that moved a row across a tick boundary, differs.
  int tick = 0;
  auto push = [&replay, &tick](const char* kind, const char* body) {
    replay.rows.push_back(std::string(kind) + "\t" + std::to_string(tick) + "\t" + body);
  };

  WgLegacyRandom randomness(run.seed);
  SpawnEnvironment env;
  env.get_block = [&scene, &push](int x, int y, int z) {
    BlockStateKey block = state("minecraft:air");
    for (const auto& cell : scene.blocks)
      if (std::get<0>(cell) == x && std::get<1>(cell) == y && std::get<2>(cell) == z)
        block = std::get<3>(cell);
    char text[320];
    std::snprintf(text, sizeof(text), "%d\t%d\t%d\t%s", x, y, z, canonical_state(block).c_str());
    push("read", text);
    return block;
  };
  env.light_level = [&scene, &push](LightLayer layer, int x, int y, int z) {
    int level = layer == LightLayer::kSky ? scene.default_sky : scene.default_block_light;
    for (const auto& cell : scene.lights)
      if (std::get<0>(cell) == (layer == LightLayer::kSky) && std::get<1>(cell) == x &&
          std::get<2>(cell) == y && std::get<3>(cell) == z)
        level = std::get<4>(cell);
    char text[128];
    std::snprintf(text, sizeof(text), "%s\t%d\t%d\t%d\t%d",
                  layer == LightLayer::kSky ? "sky" : "block", x, y, z, level);
    push("light", text);
    return level;
  };
  env.biome_name = [&scene, &push](int x, int y, int z) {
    char text[320];
    std::snprintf(text, sizeof(text), "%d\t%d\t%d\t%s", x, y, z, scene.biome.c_str());
    push("biome", text);
    return scene.biome;
  };
  env.world_surface_height = [&scene, &push](int x, int z) {
    char text[128];
    std::snprintf(text, sizeof(text), "%d\t%d\t%d", x, z, scene.default_surface);
    push("surface", text);
    return scene.default_surface;
  };
  const bool can_see_sky = scene.can_see_sky;
  env.can_see_sky = [can_see_sky](int, int, int) { return can_see_sky; };
  env.can_spawn_entities_in_chunk = [&push](int cx, int cz) {
    char text[128];
    std::snprintf(text, sizeof(text), "%d\t%d\ttrue", cx, cz);
    push("chunk", text);
    return true;
  };
  env.spawn_point_x = run.spawn_point_x;
  env.spawn_point_y = run.spawn_point_y;
  env.spawn_point_z = run.spawn_point_z;
  env.min_y = run.min_y;
  env.sky_darken = run.sky_darken;
  env.thundering = run.thundering;
  env.next_int = [&randomness, &replay, &push](int bound) {
    const int value = randomness.next_int(bound);
    ++replay.draws;
    char text[128];
    std::snprintf(text, sizeof(text), "i\t%d\t%d", bound, value);
    push("draw", text);
    return value;
  };
  env.unit_random = [&randomness, &replay, &push]() {
    const float value = randomness.next_float();
    ++replay.units;
    char text[64];
    std::snprintf(text, sizeof(text), "u\t%.6f", static_cast<double>(value));
    push("draw", text);
    return static_cast<double>(value);
  };

  SpawnState state_now =
      SpawnState::build(run.spawnable_chunks, scene.entities, scene.players, env);
  const std::vector<uint8_t> categories =
      filtered_spawning_categories(state_now, run.spawn_enemies, run.spawn_persistent);
  replay.categories = category_names(categories);
  // One pass over the chunk per tick, through the one state, exactly as the
  // generator ran it.
  for (tick = 0; tick < run.ticks; ++tick) {
    const std::vector<uint8_t> one{run.category};
    const std::vector<SpawnedMob> spawned =
        spawn_for_chunk(state_now, one, run.chunk_x, run.chunk_z, env);
    for (const SpawnedMob& mob : spawned) {
      ++replay.spawns;
      char text[320];
      std::snprintf(text, sizeof(text), "%s\t%.1f\t%d\t%.1f\t%.4f", mob.type->name, mob.x,
                    static_cast<int>(mob.y), mob.z, static_cast<double>(mob.yaw));
      push("spawn", text);
    }
  }
  return replay;
}

// Prints the first few rows that differ, so a failure names the drift instead of
// only counting it.
void report_spawn_mismatch(const RecordedRun& run, const std::vector<std::string>& on_disk,
                           const std::vector<std::string>& replayed) {
  std::fprintf(stderr, "FAIL %s:%d: run '%s': %zu rows on disk, %zu replayed\n", __FILE__, __LINE__,
               run.name.c_str(), on_disk.size(), replayed.size());
  size_t shown = 0;
  const size_t common = std::min(on_disk.size(), replayed.size());
  for (size_t i = 0; i < common && shown < 8; ++i) {
    if (on_disk[i] == replayed[i])
      continue;
    std::fprintf(stderr, "  row %zu:\n    on disk:   %s\n    replayed:  %s\n", i,
                 on_disk[i].c_str(), replayed[i].c_str());
    ++shown;
  }
  for (size_t i = common; i < on_disk.size() && shown < 8; ++i, ++shown)
    std::fprintf(stderr, "  row %zu:\n    on disk:   %s\n    replayed:  <absent>\n", i,
                 on_disk[i].c_str());
  for (size_t i = common; i < replayed.size() && shown < 8; ++i, ++shown)
    std::fprintf(stderr, "  row %zu:\n    on disk:   <absent>\n    replayed:  %s\n", i,
                 replayed[i].c_str());
}

// The golden half: replay every run the meta records and compare its whole
// interaction trace against the checked-in one, plus the meta's own counters and
// the filter's answer — so a gate that stops consulting the world, or a draw
// chain that shifts, fails here rather than only in the properties above.
void test_golden_spawn(const std::string& fixtures_dir) {
  std::vector<std::string> meta_rows;
  std::vector<std::string> scene_rows;
  const bool have_meta = read_fixture_rows(fixtures_dir + "/mob_spawn_meta.tsv", &meta_rows);
  const bool have_scene = read_fixture_rows(fixtures_dir + "/mob_spawn_scene.tsv", &scene_rows);
  CHECK(have_meta);
  CHECK(have_scene);
  if (!have_meta || !have_scene)
    return;
  CHECK(!meta_rows.empty());

  std::vector<RecordedRun> runs;
  for (const std::string& row : meta_rows) {
    const std::vector<std::string> fields = split_tabs(row);
    CHECK_EQ(fields.size(), size_t(22));
    if (fields.size() != 22)
      continue;
    RecordedRun run;
    run.seed = std::strtoll(fields[0].c_str(), nullptr, 10);
    run.name = fields[1];
    run.scene = fields[2];
    run.chunk_x = static_cast<int32_t>(std::strtol(fields[3].c_str(), nullptr, 10));
    run.chunk_z = static_cast<int32_t>(std::strtol(fields[4].c_str(), nullptr, 10));
    run.category = category_of_name(fields[5]);
    run.spawn_enemies = fields[6] == "true";
    run.spawn_persistent = fields[7] == "true";
    run.ticks = std::stoi(fields[8]);
    run.spawnable_chunks = std::stoi(fields[9]);
    run.min_y = std::stoi(fields[10]);
    run.sky_darken = std::stoi(fields[11]);
    run.thundering = fields[12] == "true";
    run.can_see_sky = fields[13] == "true";
    run.spawn_point_x = std::stod(fields[14]);
    run.spawn_point_y = std::stod(fields[15]);
    run.spawn_point_z = std::stod(fields[16]);
    run.categories = fields[17];
    run.draws = static_cast<size_t>(std::strtoull(fields[18].c_str(), nullptr, 10));
    run.units = static_cast<size_t>(std::strtoull(fields[19].c_str(), nullptr, 10));
    run.spawns = static_cast<size_t>(std::strtoull(fields[20].c_str(), nullptr, 10));
    run.rows = static_cast<size_t>(std::strtoull(fields[21].c_str(), nullptr, 10));
    runs.push_back(run);
  }

  std::vector<RecordedScene> scenes;
  auto scene_for = [&scenes](const std::string& name) -> RecordedScene* {
    for (RecordedScene& scene : scenes)
      if (scene.name == name)
        return &scene;
    return nullptr;
  };
  for (const std::string& row : scene_rows) {
    const std::vector<std::string> fields = split_tabs(row);
    CHECK(fields.size() >= 3);
    if (fields.size() < 3)
      continue;
    if (fields[0] == "default_biome" && fields.size() == 3) {
      scenes.push_back(RecordedScene{});
      scenes.back().name = fields[1];
      scenes.back().biome = fields[2];
    } else if (fields[0] == "default_surface" && fields.size() == 3) {
      RecordedScene* scene = scene_for(fields[1]);
      CHECK(scene != nullptr);
      if (scene != nullptr)
        scene->default_surface = std::stoi(fields[2]);
    } else if (fields[0] == "default_light" && fields.size() == 4) {
      RecordedScene* scene = scene_for(fields[1]);
      CHECK(scene != nullptr);
      if (scene != nullptr) {
        const int level = std::stoi(fields[3]);
        if (fields[2] == "sky")
          scene->default_sky = level;
        else
          scene->default_block_light = level;
      }
    } else if (fields[0] == "default_can_see_sky" && fields.size() == 3) {
      RecordedScene* scene = scene_for(fields[1]);
      CHECK(scene != nullptr);
      if (scene != nullptr)
        scene->can_see_sky = fields[2] == "true";
    } else if (fields[0] == "block" && fields.size() == 6) {
      RecordedScene* scene = scene_for(fields[1]);
      CHECK(scene != nullptr);
      if (scene != nullptr)
        scene->blocks.emplace_back(std::stoi(fields[2]), std::stoi(fields[3]), std::stoi(fields[4]),
                                   parse_state(fields[5]));
    } else if (fields[0] == "light" && fields.size() == 7) {
      RecordedScene* scene = scene_for(fields[1]);
      CHECK(scene != nullptr);
      if (scene != nullptr)
        scene->lights.emplace_back(fields[2] == "sky", std::stoi(fields[3]), std::stoi(fields[4]),
                                   std::stoi(fields[5]), std::stoi(fields[6]));
    } else if (fields[0] == "entity" && fields.size() == 8) {
      RecordedScene* scene = scene_for(fields[1]);
      CHECK(scene != nullptr);
      if (scene != nullptr) {
        scene->entity_types.push_back(fields[2]);
        scene->entity_specs.emplace_back(category_of_name(fields[3]), std::stoi(fields[4]),
                                         std::stoi(fields[5]), std::stoi(fields[6]),
                                         fields[7] == "true");
      }
    } else if (fields[0] == "player" && fields.size() == 6) {
      RecordedScene* scene = scene_for(fields[1]);
      CHECK(scene != nullptr);
      if (scene != nullptr)
        scene->players.push_back(watcher(std::stod(fields[2]), std::stod(fields[3]),
                                         std::stod(fields[4]), fields[5] == "true"));
    } else {
      CHECK(false); // a kind this replayer does not know
    }
  }

  for (RecordedScene& scene : scenes)
    scene.finish_entities();

  size_t checked_rows = 0;
  for (const RecordedRun& run : runs) {
    const RecordedScene* scene = scene_for(run.scene);
    CHECK(scene != nullptr);
    if (scene == nullptr)
      continue;
    // The meta and the scene have to agree about the environment the run was
    // given, or one of the two is lying about what was replayed.
    CHECK(run.can_see_sky == scene->can_see_sky);

    const SpawnReplay replay = replay_spawn_run(run, *scene);
    CHECK(replay.categories == run.categories);
    CHECK_EQ(replay.draws, run.draws);
    CHECK_EQ(replay.units, run.units);
    CHECK_EQ(replay.spawns, run.spawns);
    CHECK_EQ(replay.rows.size(), run.rows);

    std::vector<std::string> trace;
    const std::string path =
        fixtures_dir + "/mob_spawn_seed" + std::to_string(run.seed) + "_" + run.name + ".tsv";
    const bool have_trace = read_fixture_rows(path, &trace);
    CHECK(have_trace);
    if (!have_trace)
      continue;
    if (trace == replay.rows) {
      checked_rows += trace.size();
      continue;
    }
    report_spawn_mismatch(run, trace, replay.rows);
    ++failures;
  }
  std::printf("mob_spawn: golden spawn replayed for %zu runs, %zu rows compared\n", runs.size(),
              checked_rows);
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <fixtures_dir>\n", argv[0]);
    return 2;
  }
  test_generated_tables();
  test_caps();
  test_light_gate();
  test_placement_and_distances();
  test_the_pass();
  test_pass_cost();
  test_despawn();
  test_golden_spawn(argv[1]);
  if (failures == 0)
    std::printf("mob_spawn: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
