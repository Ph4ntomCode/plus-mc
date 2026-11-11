// SPDX-License-Identifier: GPL-3.0-or-later
//
// Natural spawning: `NaturalSpawner` (Phase 4 milestone C). A mob appears in a
// loaded chunk because the server asked, once per tick, whether each candidate
// chunk should try to spawn something — and this is that question, answerable
// without a server.
//
// The shape is the one the rest of `world/` uses: the world is a set of
// predicates (`SpawnEnvironment`), the population is a plain vector, and the
// result is a list of positions to create — the *caller* makes the entity, tells
// the clients and holds the registry. So the caps, the light gate, the biome
// pick and the pack sizes are unit-testable directly, and the session's copy of
// them is a wiring exercise rather than a second implementation.
//
// What is mirrored, and from where (all of 26.2, `build/_decomp`):
//
//   * **the tick's driver** — `ServerChunkCache.tickChunks` builds one
//     `SpawnState` per tick from every entity, filters the categories by the
//     difficulty and the 400-tick persistent pass
//     (`NaturalSpawner.getFilteredSpawningCategories`), then walks the candidate
//     chunks. The candidates are the chunks within eight of a player
//     (`SPAWN_DISTANCE_CHUNK`) that some non-spectator player is within 128
//     blocks of (`ChunkMap.collectSpawningChunks`).
//   * **the cap** — `SpawnState.canSpawnForCategoryGlobal` is
//     `count < maxInstancesPerChunk * spawnableChunkCount / 289`, the 289 being
//     `17^2`, i.e. the 17x17 chunk area a view distance of 8 covers. The local
//     half (`LocalMobCapCalculator.canSpawn`) is per player and *not* per chunk:
//     `MobCounts` is one running total per player per category, and a candidate
//     chunk is refused only when every player within 128 blocks of it already
//     has `maxInstancesPerChunk` mobs of that category *nearby* (anywhere inside
//     their own spawning range, not in this chunk).
//   * **the potential energy** — `PotentialCalculator`: a mob of a type named by
//     a biome spawn cost carries a charge, and a candidate position is refused
//     when the summed `charge / sqrt(distSqr)` of the nearby charges times this
//     one exceeds the biome's budget. Only two biomes set a cost in 26.2
//     (`kBiomeSpawnCosts`).
//   * **one attempt** — `spawnCategoryForPosition`: up to three groups, each
//     starting from the chunk's random position and walking up to five cells per
//     axis, one type picked per group from the biome's weighted list, a pack size
//     drawn from that entry's `minCount..maxCount`, and each member refused or
//     accepted by the placement and rule predicates below.
//   * **the predicates** — `SpawnPlacements.isSpawnPositionOk` (the generated
//     placement table: `ON_GROUND` asks the block below to be a valid spawn
//     ground and the two cells of the mob's box to be empty), `checkSpawnRules`
//     (`Monster::checkMonsterSpawnRules`' light gate, `Animal::
//     checkAnimalSpawnRules`' brightness and ground tag), and
//     `NaturalSpawner.isValidEmptySpawnBlock` (not a full collision shape, not a
//     signal source, no fluid, not `prevent_mob_spawning_inside`, and not
//     `EntityType.isBlockDangerous`).
//
// What is approximated, and what is not modelled at all — every one of these is
// a documented boundary rather than a silent difference:
//
//   * **block shapes.** `isCollisionShapeFullBlock` and `isValidSpawn`'s default
//     `isFaceSturdy(UP)` are shape questions and this engine's block data has no
//     shapes (the same approximation world/light.cpp documents), so both are
//     answered by `block_blocks_motion` — "the block collides". A slab, stair,
//     fence, wall, carpet, button or pressure plate therefore reads as a full
//     cube, which is wrong in both directions: a slab refuses a spawn vanilla
//     would allow, and a carpet allows one vanilla would refuse. Correcting it
//     needs shapes. The *other* clause of that default is exact rather than
//     approximated: `getLightEmission() < 14` comes from the light engine's own
//     table, so a glowstone floor refuses a monster here as it does in vanilla.
//   * **structures.** There are none in this engine, so
//     `isInNetherFortressBounds` (which replaces a monster's spawn list with the
//     fortress's) has no analogue; a nether fortress spawns from its biome.
//   * **the difficulty.** `SpawnPlacements.checkSpawnRules`' PEACEFUL gate is
//     keyed on the *server's* difficulty, and the world tick has no session
//     settings to read one from: it fixes the difficulty at normal, exactly as
//     its fire tick does (`kDifficultyNormal` in server/session.cpp), which is
//     also the value every session is configured with. `spawn_enemies` is
//     therefore always true and `isAllowedInPeaceful` never decides anything.
//   * **`EntityType` flags with no column in world/mob_table.h**: `canSummon`
//     (true for all 90), `canSpawnFarFromPlayer` (true for the pillager and the
//     shulker alone in vanilla; not carried, and nothing here spawns far from a
//     player anyway) and `isAllowedInPeaceful` (true, for the reason above).
//     `fireImmune` now *has* its own column (the table's `fire_immune`) — the
//     probe reads `EntityType.fireImmune()` — and that column is what
//     `kSpawnRuleFireImmune` and the burning-block check resolve to.
//   * **the entity half of `Mob.checkSpawnObstruction`.** Vanilla's is
//     `!containsAnyLiquid(box) && level.isUnobstructed(this)`, and
//     `isUnobstructed` refuses a box that overlaps another *entity* — a player
//     included. The population here carries positions but no boxes, so two mobs
//     can be placed into one cell where vanilla keeps the second out. The fluid
//     half is modelled.
//   * **`Mob.finalizeSpawn`**, group data (a zombie's reinforcements, a slime's
//     size), riders, `MobCategory.MISC`'s anything, `WeightedList`'s
//     `REDUCED_WATER_AMBIENT_SPAWNS` 0.98 draw (a category no mob table row has),
//     and the `IN_WATER` / `IN_LAVA` / `NO_RESTRICTIONS` types the mob table has
//     no row for.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "world/biome_spawn_table.h"
#include "world/chunk.h"
#include "world/light.h"
#include "world/mob_category.h"
#include "world/mob_table.h"
#include "world/signal_source_table.h"
#include "world/spawn_placement_table.h"
#include "world/spawn_rule_table.h"

namespace mc::world {

// ---- the numbers -----------------------------------------------------------

// `NaturalSpawner.MIN_SPAWN_DISTANCE`: nothing spawns within 24 blocks of the
// nearest player, and nothing within 24 blocks of the world spawn point.
constexpr int kSpawnMinDistance = 24;
// `NaturalSpawner.SPAWN_DISTANCE_CHUNK` / `SPAWN_DISTANCE_BLOCK`: the band the
// candidate chunks are gathered from.
constexpr int kSpawnDistanceChunk = 8;
constexpr int kSpawnDistanceBlock = 128;
// `INSCRIBED_SQUARE_SPAWN_DISTANCE_CHUNK` = `floor(8 / sqrt(2))`: the chunks a
// player is unconditionally close enough to.
constexpr int kInscribedSquareSpawnDistanceChunk = 5;
// `MAGIC_NUMBER` = `17^2`, the chunk count a view distance of 8 covers and the
// divisor the global cap scales by.
constexpr int kMobCapChunkDivisor = 289;
// The group loop's own bounds: three groups, a five-cell walk per axis (six in
// each draw, minus the other), and `Mob.getMaxSpawnClusterSize`'s four per chunk
// attempt.
constexpr int kSpawnGroupCount = 3;
constexpr int kSpawnWalkBound = 6;
constexpr int kMobMaxClusterSize = 4;
// `dimension_type/overworld.json`: `monster_spawn_block_light_limit` 0 and
// `monster_spawn_light_level` the uniform 0..7.
constexpr int kMonsterSpawnBlockLightLimit = 0;
constexpr int kMonsterSpawnLightTestMax = 7;
// `Animal.isBrightEnoughToSpawn`: `getRawBrightness(pos, 0) > 8`.
constexpr int kAnimalSpawnRawBrightness = 8;
// `Monster.isDarkEnoughToSpawn`'s thundering branch: `getMaxLocalRawBrightness(
// pos, 10)`, i.e. the raw brightness with the sky darkened by ten.
constexpr int kThunderRawBrightnessDarken = 10;

// ---- the population --------------------------------------------------------

// One entity, as `NaturalSpawner.createState` sees it. The caller filters
// nothing: `persistence_required` is the `Mob.isPersistenceRequired() ||
// requiresCustomPersistence()` half of the skip, and `MISC` is the other.
struct SpawnEntity {
  const char* type_name = nullptr; // "minecraft:zombie" (the biome cost lookup)
  uint8_t category = kSpawnMisc;
  int x = 0, y = 0, z = 0;
  bool persistence_required = false;
};

// One player, as the spawner sees them. `spectator` is what
// `EntitySelector.NO_SPECTATORS` tests in `getNearestPlayer` (a creative player
// still counts, and still spawns mobs), and the local cap ignores them entirely.
struct SpawnPlayer {
  double x = 0.0, y = 0.0, z = 0.0;
  bool spectator = false;
};

// `MobSpawnSettings.MobSpawnCost`.
struct MobSpawnCost {
  double charge = 0.0;
  double energy_budget = 0.0;
};

// ---- the world a spawn pass reads ------------------------------------------

struct SpawnEnvironment {
  // The block at those coordinates. Required: every placement question reads it.
  std::function<BlockStateKey(int, int, int)> get_block;
  // `Level.getBrightness(LightLayer, pos)`: one layer's own value, so this
  // module can answer `getRawBrightness(pos, amount)` the way
  // `chunk_raw_brightness` does. A world with no light of its own answers 0,
  // which is the dark cave the monster gate wants.
  std::function<int(LightLayer, int, int, int)> light_level;
  // `Level.getBiome(pos)`: the biome whose spawn list and spawn costs apply.
  std::function<std::string(int, int, int)> biome_name;
  // `chunk.getHeight(Heightmap.Types.WORLD_SURFACE, x, z)`: the top a random
  // position's y is drawn up to. Empty means no attempt at all, which is what a
  // chunk whose surface is unknown should answer.
  std::function<int(int, int)> world_surface_height;
  // `Level.canSeeSky(pos)`, for the surface-monster predicate. Empty means true.
  std::function<bool(int, int, int)> can_see_sky;
  // `ServerLevel.canSpawnEntitiesInChunk(pos)`: whether that chunk is ticking
  // for entities. A group can walk across a chunk border and the neighbour it
  // lands in has to answer, so this is asked per position, not per candidate
  // chunk. Vanilla's own test short-circuits for the *starting* chunk
  // (`Objects.equals(chunkPos, chunk.getPos()) || ...`), which this does not:
  // a caller whose candidate chunks are all loadable answers the same either
  // way, and one that wants the short-circuit can answer true for its own
  // chunk. Empty means true.
  std::function<bool(int, int)> can_spawn_entities_in_chunk;
  // `LevelData.RespawnData`: the world spawn point, which
  // `isRightDistanceToPlayerAndSpawnPoint` keeps clear to 24 blocks.
  double spawn_point_x = 0.0, spawn_point_y = 0.0, spawn_point_z = 0.0;
  // `Level.getMinY()`: the lowest y a random position may take, and the floor
  // below which an attempt is abandoned.
  int min_y = -64;
  // `Level.getSkyDarken()` and `Level.isThundering()`: the two environment terms
  // `isDarkEnoughToSpawn` blends into `getMaxLocalRawBrightness`.
  int sky_darken = 0;
  bool thundering = false;
  // `Level.random`: one bounded integer draw (`nextInt(bound)`) and one float in
  // [0, 1) (`nextFloat`). The caller owns them so a test can pin a pass exactly.
  std::function<int(int)> next_int;
  std::function<double()> unit_random;
};

// ---- the population the caps are measured against --------------------------

// `NaturalSpawner.SpawnState`: built once per tick from every entity, and
// *updated* as the pass spawns (`after_spawn`), which is how the cap holds
// within a single tick.
class SpawnState {
public:
  SpawnState() = default;

  // `NaturalSpawner.createState(spawnableChunkCount, entities, chunkGetter,
  // localMobCapCalculator)`. `spawnable_chunk_count` is
  // `DistanceManager.getNaturalSpawnChunkCount()`, i.e. the chunks within
  // spawning range of a player.
  static SpawnState build(int spawnable_chunk_count, const std::vector<SpawnEntity>& entities,
                          const std::vector<SpawnPlayer>& players, const SpawnEnvironment& env);

  int spawnable_chunk_count() const {
    return spawnable_chunk_count_;
  }
  // `getMobCategoryCounts().getInt(category)`.
  int count_of(uint8_t category) const {
    return category < kSpawnCategoryCount ? counts_[category] : 0;
  }
  const std::vector<SpawnPlayer>& players() const {
    return players_;
  }
  // The global cap: `count < maxInstancesPerChunk * chunks / 289`.
  bool can_spawn_for_category_global(uint8_t category) const;
  // The local cap (`LocalMobCapCalculator.canSpawn`): some player within 128
  // blocks of the chunk's middle is below `maxInstancesPerChunk` *total* mobs of
  // the category, counting every mob of it within their own spawning range. With
  // no player nearby the answer is false, which is also the reason a mob cannot
  // spawn in a chunk nobody is watching.
  bool can_spawn_for_category_local(uint8_t category, int cx, int cz) const;
  // `SpawnState.canSpawn`: the spawn-cost gate, and the only place the last
  // checked position is remembered — so `after_spawn` does not pay for the biome
  // lookup again, exactly as vanilla's callback discipline arranges it.
  bool can_spawn(const GeneratedMob& type, int x, int y, int z, const SpawnEnvironment& env);
  // `SpawnState.afterSpawn`: bank the charge, count the mob, count the local
  // density for every player near it.
  void after_spawn(const GeneratedMob& type, int x, int y, int z, const SpawnEnvironment& env);
  // `PotentialCalculator.getPotentialEnergyChange`, exposed because it is the
  // one piece of arithmetic worth pinning on its own.
  double potential_energy_change(int x, int y, int z, double charge) const;
  // The charges banked so far, for a test that wants to see the cost path.
  size_t charge_count() const {
    return charges_.size();
  }

private:
  // `PotentialCalculator.PointCharge`.
  struct Charge {
    int x = 0, y = 0, z = 0;
    double charge = 0.0;
  };
  void add_local_count(uint8_t category, int cx, int cz);

  std::vector<SpawnPlayer> players_;
  // `LocalMobCapCalculator.MobCounts`: per player, per category — a running
  // total over every mob of the category that player is close enough to spawn.
  std::vector<std::array<int, kSpawnCategoryCount>> per_player_;
  std::vector<Charge> charges_;
  std::array<int, kSpawnCategoryCount> counts_{};
  int spawnable_chunk_count_ = 0;
  // `lastCheckedPos` / `lastCheckedType` / `lastCharge`.
  int last_x_ = 0, last_y_ = 0, last_z_ = 0;
  const GeneratedMob* last_type_ = nullptr;
  double last_charge_ = 0.0;
  bool has_last_ = false;
};

// ---- the tables the module reads -------------------------------------------

// `MobSpawnSettings.getMobs(category)`: the biome's spawn list for one category,
// as a slice of the generated table. A biome with no spawns at all, or no
// entries for the category, is an empty list.
struct BiomeSpawnList {
  const GeneratedBiomeSpawn* entries = nullptr;
  int count = 0;
  int weight_total = 0; // over the entries this engine can actually create
  bool empty() const {
    return count == 0;
  }
};
BiomeSpawnList biome_spawn_list(std::string_view biome, uint8_t category);

// `MobSpawnSettings.getMobSpawnCost(type)`, or nullopt for a biome that sets
// none (which is every biome but soul_sand_valley and warped_forest).
std::optional<MobSpawnCost> biome_spawn_cost(std::string_view biome, std::string_view entity);

// One `WeightedList.getRandom` draw: the entry (for its pack size) and the mob
// the mob table resolves it to. An entry for a type this engine has no row for
// is dropped and the rest of the list carries its weight, which is why a
// weighted pick can return an entry whose `weight` looks too small to have won.
struct BiomeSpawnPick {
  const GeneratedBiomeSpawn* entry = nullptr;
  const GeneratedMob* type = nullptr;
  bool empty() const {
    return type == nullptr;
  }
};
BiomeSpawnPick biome_spawn_pick(std::string_view biome, uint8_t category,
                                const SpawnEnvironment& env);

// `SpawnPlacements.getPlacementType` / `checkSpawnRules`, resolved through the
// generated table. An unregistered type answers `NO_RESTRICTIONS` for the
// placement and *passes* the rules — vanilla's own shape for a type nobody
// registered, whose absent `Data` means "no predicate" rather than
// `Mob::checkMobSpawnRules`.
const GeneratedSpawnPlacement* spawn_placement_of(const GeneratedMob& type);

// ---- the predicates --------------------------------------------------------

// `BlockState.isValidSpawn(level, pos, type)` for the block *below* a spawn
// position: the generated overrides, or the default, which this engine answers
// as "the block collides" (see the shape note at the top of this file).
bool block_is_valid_spawn(const BlockStateKey& below, const GeneratedMob& type);

// `BlockState.isSignalSource()`: a redstone component refuses a spawn inside it.
bool block_is_signal_source(std::string_view block_name);

// `NaturalSpawner.isValidEmptySpawnBlock`: not a full collision shape, not a
// signal source, no fluid, not `prevent_mob_spawning_inside`, and not a block
// this type finds dangerous (fire, lava, a lit campfire, a cactus, a berry bush,
// a wither rose, powder snow).
bool is_valid_empty_spawn_block(const SpawnEnvironment& env, int x, int y, int z,
                                const GeneratedMob& type);

// `SpawnPlacements.isSpawnPositionOk`: the placement type's own question —
// `ON_GROUND` (a valid ground below, and this cell plus the one above it empty),
// `IN_WATER`, `IN_LAVA`, or nothing at all.
bool spawn_position_ok(const SpawnEnvironment& env, const GeneratedMob& type, int x, int y, int z);

// `SpawnPlacements.checkSpawnRules(type, level, NATURAL, pos, random)`. The light
// draws are made even when the answer is already known false, because vanilla's
// random source has no such short-circuit — a test that pins the sequence sees
// the same number of draws either way.
bool check_spawn_rules(const SpawnEnvironment& env, const GeneratedMob& type, int x, int y, int z);

// `Monster.isDarkEnoughToSpawn`.
bool is_dark_enough_to_spawn(const SpawnEnvironment& env, int x, int y, int z);

// `Level.getRawBrightness(pos, sky_darken)`: this module's own arithmetic, kept
// here because four predicates share it.
int raw_brightness(const SpawnEnvironment& env, int x, int y, int z, int sky_darken);

// `Level.noCollision(type.getSpawnAABB(x, y, z))` and `Mob.checkSpawnObstruction`:
// whether the box a mob of `type` would occupy at that position holds no
// colliding block, and — when `fluid_matters` — no fluid cell either. The
// *entity* half of `checkSpawnObstruction` (`Level.isUnobstructed`) is not
// modelled; see the list at the top of this file.
bool spawn_box_is_clear(const SpawnEnvironment& env, const GeneratedMob& type, double x, double y,
                        double z, bool fluid_matters);

// `NaturalSpawner.isRightDistanceToPlayerAndSpawnPoint`: at least 24 blocks from
// the nearest non-spectator player, at least 24 from the world spawn, and in a
// chunk that is ticking for entities.
bool right_distance_to_player_and_spawn_point(const SpawnEnvironment& env,
                                              std::vector<SpawnPlayer> const& players, int x, int y,
                                              int z, double nearest_player_dist_sqr);

// The nearest non-spectator player's squared distance to (x + 0.5, y, z + 0.5),
// or -1 when there is none.
double nearest_player_distance_sqr(const std::vector<SpawnPlayer>& players, int x, int y, int z);

// ---- the pass --------------------------------------------------------------

// `getFilteredSpawningCategories`: every category but MISC, minus the monsters
// when enemies are off, minus the persistent ones unless this is the 400-tick
// pass, minus the categories already at their global cap.
std::vector<uint8_t> filtered_spawning_categories(const SpawnState& state, bool spawn_enemies,
                                                  bool spawn_persistent);

// One mob to create. The caller makes the entity, places it at exactly these
// coordinates and tells the clients; everything the spawner decided is here.
struct SpawnedMob {
  const GeneratedMob* type = nullptr;
  double x = 0.0, y = 0.0, z = 0.0;
  float yaw = 0.0f;
};

// `NaturalSpawner.spawnCategoryForChunk`: a random position in the chunk, then
// one attempt there. Empty when that position is below the world.
std::vector<SpawnedMob> spawn_category_for_chunk(SpawnState& state, uint8_t category, int cx,
                                                 int cz, const SpawnEnvironment& env);

// `NaturalSpawner.spawnCategoryForPosition`: the attempt itself — up to three
// groups of one type each, walking from the start position. `state` is updated as
// mobs are added.
std::vector<SpawnedMob> spawn_category_for_position(SpawnState& state, uint8_t category,
                                                    int start_x, int start_y, int start_z,
                                                    const SpawnEnvironment& env);

// `NaturalSpawner.spawnForChunk`: every filtered category, gated per category on
// the local cap, over one chunk. The caller walks the chunks.
std::vector<SpawnedMob> spawn_for_chunk(SpawnState& state, const std::vector<uint8_t>& categories,
                                        int cx, int cz, const SpawnEnvironment& env);

} // namespace mc::world
