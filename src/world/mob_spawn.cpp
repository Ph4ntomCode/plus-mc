// SPDX-License-Identifier: GPL-3.0-or-later
//
// `NaturalSpawner`, the parts of it this engine can answer. See
// world/mob_spawn.h for what is mirrored, what is approximated and what is not
// modelled; the comments here name the branch each piece is written from.
#include "world/mob_spawn.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "world/block_tags.h"
#include "world/container.h" // block_property

namespace mc::world {
namespace {

// `Mth.floor((double)value / 16.0)` for the chunk a block position falls in. The
// truncating `/` is wrong below zero, exactly as world/light.cpp documents.
int floor_div16(int value) {
  return value >= 0 ? value / 16 : (value - 15) / 16;
}

// `Entities.SPAWNING_CATEGORIES`: every category but MISC, in `values()` order.
constexpr uint8_t kSpawningCategories[] = {
    kSpawnMonster,
    kSpawnCreature,
    kSpawnAmbient,
    kSpawnAxolotls,
    kSpawnUndergroundWaterCreature,
    kSpawnWaterCreature,
    kSpawnWaterAmbient,
};

// `BlockState.getFluidState().isEmpty()`: the fluid blocks themselves, plus a
// waterlogged block, whose fluid state is the water it holds. The engine has no
// fluid simulation, so this is the whole of the question it can answer — and it
// is the right answer for both ends of it, since a water source and a
// waterlogged slab each refuse a spawn in vanilla.
bool block_has_fluid(const BlockStateKey& state) {
  if (state.name == "minecraft:water" || state.name == "minecraft:lava" ||
      state.name == "minecraft:bubble_column")
    return true;
  return block_property(state.properties, "waterlogged") == "true";
}

// `NodeEvaluator.isBurningBlock`: the fire tag, lava, magma, a *lit* campfire,
// and a lava cauldron. `CampfireBlock.isLitCampfire` is the state's own
// `lit=true`, which the engine carries as a property.
bool block_is_burning(const BlockStateKey& state) {
  if (state.name == "minecraft:fire" || state.name == "minecraft:soul_fire")
    return true;
  if (state.name == "minecraft:lava" || state.name == "minecraft:magma_block" ||
      state.name == "minecraft:lava_cauldron")
    return true;
  if (state.name == "minecraft:campfire" || state.name == "minecraft:soul_campfire")
    return block_property(state.properties, "lit") != "false";
  return false;
}

} // namespace

bool block_is_signal_source(std::string_view block_name) {
  // Sorted, so this is one binary search (world/signal_source_table.h).
  const char* const* begin = kSignalSourceBlocks;
  const char* const* end = kSignalSourceBlocks + kSignalSourceBlockCount;
  const char* const* it =
      std::lower_bound(begin, end, block_name, [](const char* entry, std::string_view needle) {
        return std::string_view(entry) < needle;
      });
  return it != end && std::string_view(*it) == block_name;
}

// ---- the tables ------------------------------------------------------------

BiomeSpawnList biome_spawn_list(std::string_view biome, uint8_t category) {
  BiomeSpawnList list;
  const GeneratedBiomeSpawnRange* begin = kBiomeSpawnRanges;
  const GeneratedBiomeSpawnRange* end = kBiomeSpawnRanges + kBiomeSpawnRangeCount;
  const GeneratedBiomeSpawnRange* it = std::lower_bound(
      begin, end, biome, [](const GeneratedBiomeSpawnRange& entry, std::string_view name) {
        return std::string_view(entry.biome) < name;
      });
  if (it == end || std::string_view(it->biome) != biome)
    return list; // a biome with no spawns at all
  // The slice is every category in order, so the category's own entries are the
  // contiguous run that matches — a category with no entries is a zero-length gap
  // between two others.
  const GeneratedBiomeSpawn* entries = kBiomeSpawns + it->first;
  int first = -1;
  int count = 0;
  for (int i = 0; i < it->count; ++i) {
    if (entries[i].category != category) {
      if (first >= 0)
        break;
      continue;
    }
    if (first < 0)
      first = i;
    ++count;
  }
  if (first < 0)
    return list;
  list.entries = entries + first;
  list.count = count;
  // The weight total is over the entries the mob table can create: an entry for
  // a type this engine has no row for is dropped from the pick, so the list is
  // renormalized over what can actually spawn (the generated table's own note).
  for (int i = 0; i < count; ++i) {
    if (mob_by_name(list.entries[i].entity) != nullptr)
      list.weight_total += list.entries[i].weight;
  }
  return list;
}

std::optional<MobSpawnCost> biome_spawn_cost(std::string_view biome, std::string_view entity) {
  // Sorted by (biome, entity): the first row at or after the biome, then a walk
  // over that biome's rows.
  const GeneratedBiomeSpawnCost* begin = kBiomeSpawnCosts;
  const GeneratedBiomeSpawnCost* end = kBiomeSpawnCosts + kBiomeSpawnCostCount;
  const GeneratedBiomeSpawnCost* it = std::lower_bound(
      begin, end, biome, [](const GeneratedBiomeSpawnCost& entry, std::string_view name) {
        return std::string_view(entry.biome) < name;
      });
  for (; it != end && std::string_view(it->biome) == biome; ++it) {
    if (std::string_view(it->entity) == entity)
      return MobSpawnCost{it->charge, it->energy_budget};
  }
  return std::nullopt;
}

BiomeSpawnPick biome_spawn_pick(std::string_view biome, uint8_t category,
                                const SpawnEnvironment& env) {
  BiomeSpawnPick pick;
  if (!env.unit_random)
    return pick;
  const BiomeSpawnList list = biome_spawn_list(biome, category);
  if (list.empty() || list.weight_total <= 0)
    return pick;
  // `WeightedList.getRandom`: one draw in [0, weightTotal) walked against the
  // cumulative weights.
  const int roll = static_cast<int>(env.unit_random() * static_cast<double>(list.weight_total));
  int cumulative = 0;
  for (int i = 0; i < list.count; ++i) {
    const GeneratedMob* type = mob_by_name(list.entries[i].entity);
    if (type == nullptr)
      continue;
    cumulative += list.entries[i].weight;
    if (roll < cumulative) {
      pick.entry = &list.entries[i];
      pick.type = type;
      return pick;
    }
  }
  return pick;
}

const GeneratedSpawnPlacement* spawn_placement_of(const GeneratedMob& type) {
  const GeneratedSpawnPlacement* begin = kSpawnPlacements;
  const GeneratedSpawnPlacement* end = kSpawnPlacements + kSpawnPlacementCount;
  const GeneratedSpawnPlacement* it =
      std::lower_bound(begin, end, std::string_view(type.name),
                       [](const GeneratedSpawnPlacement& entry, std::string_view name) {
                         return std::string_view(entry.entity) < name;
                       });
  if (it == end || std::string_view(it->entity) != type.name)
    return nullptr; // unregistered: NO_RESTRICTIONS + Mob::checkMobSpawnRules
  return it;
}

// ---- the predicates --------------------------------------------------------

// `BlockBehaviour`'s unoverridden predicate, the one `spawn_rule_table.h`
// records as `kSpawnRuleDefault`: `isFaceSturdy(UP) && getLightEmission() < 14`.
// The shape half is the collision approximation this file's header describes;
// the emission half is exact, because the light engine already carries every
// block's own emission — which is what keeps a monster off a glowstone floor.
static bool default_is_valid_spawn(const BlockStateKey& below) {
  return block_blocks_motion(below.name) && light_emission(below) < 14;
}

bool block_is_valid_spawn(const BlockStateKey& below, const GeneratedMob& type) {
  const GeneratedSpawnRule* begin = kBlockSpawnRules;
  const GeneratedSpawnRule* end = kBlockSpawnRules + kBlockSpawnRuleCount;
  const GeneratedSpawnRule* it =
      std::lower_bound(begin, end, std::string_view(below.name),
                       [](const GeneratedSpawnRule& entry, std::string_view name) {
                         return std::string_view(entry.name) < name;
                       });
  if (it == end || std::string_view(it->name) != below.name)
    return default_is_valid_spawn(below);
  switch (it->rule) {
  case kSpawnRuleNever:
    return false;
  case kSpawnRuleAlways:
    return true;
  case kSpawnRulePolarBear:
    return std::string_view(type.name) == "minecraft:polar_bear";
  case kSpawnRuleOcelotOrParrot:
    return std::string_view(type.name) == "minecraft:ocelot" ||
           std::string_view(type.name) == "minecraft:parrot";
  case kSpawnRuleFireImmune:
    // `entityType.fireImmune()`: the table's own column, straight from the probe
    // — a magma block (`Blocks.MAGMA_BLOCK`'s `isValidSpawn`) admits a blaze, a
    // magma cube, a strider and every other type the game marks immune, and
    // refuses the rest.
    return type.fire_immune;
  case kSpawnRuleDefault:
  default:
    return default_is_valid_spawn(below);
  }
}

bool is_valid_empty_spawn_block(const SpawnEnvironment& env, int x, int y, int z,
                                const GeneratedMob& type) {
  if (!env.get_block)
    return true;
  const BlockStateKey state = env.get_block(x, y, z);
  if (block_blocks_motion(state.name))
    return false; // isCollisionShapeFullBlock
  if (block_is_signal_source(state.name))
    return false; // isSignalSource
  if (block_has_fluid(state))
    return false; // fluidState is not empty
  if (block_has_tag(kBlockTagPreventMobSpawningInside, state.name))
    return false; // BlockTags.PREVENT_MOB_SPAWNING_INSIDE (the rails)
  if (block_is_burning(state) && !type.fire_immune) {
    // `EntityType.isBlockDangerous`'s `!this.fireImmune && isBurningBlock(state)`
    // — the same `fire_immune` column the magma block's spawn rule reads, so a
    // fire-immune type may stand in the flames and nothing else may. (Vanilla
    // checks the type's `immuneTo` block tag before this; that tag is a further
    // column the table does not carry yet, and the header lists it as such.)
    return false;
  }
  if (state.name == "minecraft:wither_rose" || state.name == "minecraft:sweet_berry_bush" ||
      state.name == "minecraft:cactus" || state.name == "minecraft:powder_snow")
    return false; // the four blocks `isBlockDangerous` names outright
  return true;
}

bool spawn_position_ok(const SpawnEnvironment& env, const GeneratedMob& type, int x, int y, int z) {
  const GeneratedSpawnPlacement* placement = spawn_placement_of(type);
  switch (placement ? placement->placement : kPlacementNoRestrictions) {
  case kPlacementOnGround: {
    if (!env.get_block)
      return true;
    if (!block_is_valid_spawn(env.get_block(x, y - 1, z), type))
      return false;
    return is_valid_empty_spawn_block(env, x, y, z, type) &&
           is_valid_empty_spawn_block(env, x, y + 1, z, type);
  }
  case kPlacementInWater: {
    if (!env.get_block)
      return true;
    const BlockStateKey here = env.get_block(x, y, z);
    const bool water =
        here.name == "minecraft:water" || block_property(here.properties, "waterlogged") == "true";
    if (!water)
      return false;
    // `!level.getBlockState(above).isRedstoneConductor(level, above)`.
    return !block_blocks_motion(env.get_block(x, y + 1, z).name);
  }
  case kPlacementInLava:
    return !env.get_block || env.get_block(x, y, z).name == "minecraft:lava";
  case kPlacementNoRestrictions:
  default:
    return true;
  }
}

int raw_brightness(const SpawnEnvironment& env, int x, int y, int z, int sky_darken) {
  if (!env.light_level)
    return 0;
  const int block = env.light_level(LightLayer::kBlock, x, y, z);
  const int sky = env.light_level(LightLayer::kSky, x, y, z);
  return std::max(block, sky - sky_darken);
}

bool is_dark_enough_to_spawn(const SpawnEnvironment& env, int x, int y, int z) {
  if (!env.light_level || !env.next_int)
    return true;
  // `if (level.getBrightness(LightLayer.SKY, pos) > random.nextInt(32)) return
  // false;` — the sky layer's own value, not the darkened blend.
  if (env.light_level(LightLayer::kSky, x, y, z) > env.next_int(32))
    return false;
  if (kMonsterSpawnBlockLightLimit < 15 &&
      env.light_level(LightLayer::kBlock, x, y, z) > kMonsterSpawnBlockLightLimit)
    return false;
  const int darken = env.thundering ? kThunderRawBrightnessDarken : env.sky_darken;
  return raw_brightness(env, x, y, z, darken) <= env.next_int(kMonsterSpawnLightTestMax + 1);
}

bool check_spawn_rules(const SpawnEnvironment& env, const GeneratedMob& type, int x, int y, int z) {
  const GeneratedSpawnPlacement* placement = spawn_placement_of(type);
  // `SpawnPlacements.checkSpawnRules`: a type with no registration *passes* —
  // vanilla reads a missing `Data` as "no predicate", after the PEACEFUL gate
  // that this engine's normal difficulty and its `isAllowedInPeaceful` mobs
  // never trip. It does not fall through to `Mob::checkMobSpawnRules`.
  if (placement == nullptr)
    return true;
  const uint8_t rule = placement->rule;
  switch (rule) {
  case kSpawnRuleMonster:
    if (!is_dark_enough_to_spawn(env, x, y, z))
      return false;
    [[fallthrough]];
  case kSpawnRuleMob:
    // `Mob.checkMobSpawnRules`: `isSpawner(reason) || below.isValidSpawn(...)`.
    return !env.get_block || block_is_valid_spawn(env.get_block(x, y - 1, z), type);
  case kSpawnRuleAnyLightMonster:
    return !env.get_block || block_is_valid_spawn(env.get_block(x, y - 1, z), type);
  case kSpawnRuleSurfaceMonster:
    if (!env.get_block || !block_is_valid_spawn(env.get_block(x, y - 1, z), type))
      return false;
    if (!is_dark_enough_to_spawn(env, x, y, z))
      return false;
    return !env.can_see_sky || env.can_see_sky(x, y, z);
  case kSpawnRuleAnimal: {
    if (!env.get_block)
      return true;
    if (!block_has_tag(kBlockTagAnimalsSpawnableOn, env.get_block(x, y - 1, z).name))
      return false;
    // `Animal.isBrightEnoughToSpawn`: `getRawBrightness(pos, 0) > 8` — the sky's
    // own 15, day or night, which is why an animal needs open sky rather than
    // daylight.
    return raw_brightness(env, x, y, z, 0) > kAnimalSpawnRawBrightness;
  }
  case kSpawnRuleUnmodelled:
  default:
    return false;
  }
}

bool spawn_box_is_clear(const SpawnEnvironment& env, const GeneratedMob& type, double x, double y,
                        double z, bool fluid_matters) {
  if (!env.get_block)
    return true;
  // `type.getSpawnAABB(x, y, z)`: the mob's own width, but never taller than its
  // height, centred on the position.
  const double half = static_cast<double>(type.width) * 0.5;
  const double top = y + static_cast<double>(type.height);
  const int min_x = static_cast<int>(std::floor(x - half));
  const int max_x = static_cast<int>(std::floor(x + half - 1.0e-7));
  const int min_y = static_cast<int>(std::floor(y));
  const int max_y = static_cast<int>(std::floor(top - 1.0e-7));
  const int min_z = static_cast<int>(std::floor(z - half));
  const int max_z = static_cast<int>(std::floor(z + half - 1.0e-7));
  for (int bx = min_x; bx <= max_x; ++bx) {
    for (int by = min_y; by <= max_y; ++by) {
      for (int bz = min_z; bz <= max_z; ++bz) {
        const BlockStateKey state = env.get_block(bx, by, bz);
        if (block_blocks_motion(state.name))
          return false; // Level.noCollision / isUnobstructed
        if (fluid_matters && block_has_fluid(state))
          return false; // Mob.checkSpawnObstruction's containsAnyLiquid
      }
    }
  }
  return true;
}

double nearest_player_distance_sqr(const std::vector<SpawnPlayer>& players, int x, int y, int z) {
  const double xx = static_cast<double>(x) + 0.5;
  const double zz = static_cast<double>(z) + 0.5;
  double best = -1.0;
  for (const SpawnPlayer& player : players) {
    if (player.spectator)
      continue;
    const double dx = player.x - xx;
    const double dy = player.y - static_cast<double>(y);
    const double dz = player.z - zz;
    const double dist = dx * dx + dy * dy + dz * dz;
    if (best < 0.0 || dist < best)
      best = dist;
  }
  return best;
}

bool right_distance_to_player_and_spawn_point(const SpawnEnvironment& env,
                                              std::vector<SpawnPlayer> const& players, int x, int y,
                                              int z, double nearest_player_dist_sqr) {
  // `nearestPlayerDistanceSqr <= 576.0`: the `MIN_SPAWN_DISTANCE` of 24 squared.
  if (nearest_player_dist_sqr <=
      static_cast<double>(kSpawnMinDistance) * static_cast<double>(kSpawnMinDistance))
    return false;
  (void)players;
  const double dx = static_cast<double>(x) + 0.5 - env.spawn_point_x;
  const double dy = static_cast<double>(y) - env.spawn_point_y;
  const double dz = static_cast<double>(z) + 0.5 - env.spawn_point_z;
  if (std::sqrt(dx * dx + dy * dy + dz * dz) < static_cast<double>(kSpawnMinDistance))
    return false;
  if (!env.can_spawn_entities_in_chunk)
    return true;
  return env.can_spawn_entities_in_chunk(floor_div16(x), floor_div16(z));
}

// ---- the population --------------------------------------------------------

void SpawnState::add_local_count(uint8_t category, int cx, int cz) {
  // `LocalMobCapCalculator.addMob(pos, category)`: every player close enough to
  // the *mob's* chunk counts the mob in their own running total for the
  // category. `playerIsCloseEnoughForSpawning` measures the chunk's centre
  // against the player's position in x/z only (the player's own y never enters
  // it), and skips spectators.
  for (size_t i = 0; i < players_.size(); ++i) {
    const SpawnPlayer& player = players_[i];
    if (player.spectator)
      continue;
    const double dx = player.x - static_cast<double>(cx * 16 + 8);
    const double dz = player.z - static_cast<double>(cz * 16 + 8);
    if (dx * dx + dz * dz >=
        static_cast<double>(kSpawnDistanceBlock) * static_cast<double>(kSpawnDistanceBlock))
      continue;
    per_player_[i][category] += 1;
  }
}

SpawnState SpawnState::build(int spawnable_chunk_count, const std::vector<SpawnEntity>& entities,
                             const std::vector<SpawnPlayer>& players, const SpawnEnvironment& env) {
  SpawnState state;
  state.spawnable_chunk_count_ = spawnable_chunk_count;
  state.players_ = players;
  state.per_player_.assign(players.size(), {});
  for (const SpawnEntity& entity : entities) {
    // `if ((entity instanceof Mob && (mob.isPersistenceRequired() ||
    // mob.requiresCustomPersistence())) || category == MISC) continue;`
    if (entity.persistence_required || entity.category == kSpawnMisc)
      continue;
    // `spawnPotential.addCharge(entity.blockPosition(), cost.charge())`: the
    // cost is the *rough* biome's, the one at the entity's own position.
    const std::string biome =
        env.biome_name ? env.biome_name(entity.x, entity.y, entity.z) : std::string();
    const std::optional<MobSpawnCost> cost = (!entity.type_name || biome.empty())
                                                 ? std::nullopt
                                                 : biome_spawn_cost(biome, entity.type_name);
    if (cost && cost->charge != 0.0)
      state.charges_.push_back(Charge{entity.x, entity.y, entity.z, cost->charge});
    state.add_local_count(entity.category, floor_div16(entity.x), floor_div16(entity.z));
    state.counts_[entity.category] += 1;
  }
  return state;
}

bool SpawnState::can_spawn_for_category_global(uint8_t category) const {
  // `maxInstancesPerChunk * spawnableChunkCount / MAGIC_NUMBER`, integer
  // arithmetic throughout — a spawnable count of 289 is exactly one cap.
  const int max_mob_count = mob_category_stats(category).max_instances_per_chunk *
                            spawnable_chunk_count_ / kMobCapChunkDivisor;
  return counts_[category] < max_mob_count;
}

bool SpawnState::can_spawn_for_category_local(uint8_t category, int cx, int cz) const {
  const MobCategoryStats& stats = mob_category_stats(category);
  for (size_t i = 0; i < players_.size(); ++i) {
    const SpawnPlayer& player = players_[i];
    if (player.spectator)
      continue;
    const double dx = player.x - static_cast<double>(cx * 16 + 8);
    const double dz = player.z - static_cast<double>(cz * 16 + 8);
    if (dx * dx + dz * dz >=
        static_cast<double>(kSpawnDistanceBlock) * static_cast<double>(kSpawnDistanceBlock))
      continue;
    // `mobCounts != null && !mobCounts.canSpawn(category) -> continue`: a player
    // who has never had a mob counted passes, which is the common case, and one
    // player below the cap is enough for the whole chunk.
    if (per_player_[i][category] >= stats.max_instances_per_chunk)
      continue;
    return true;
  }
  return false;
}

bool SpawnState::can_spawn(const GeneratedMob& type, int x, int y, int z,
                           const SpawnEnvironment& env) {
  last_x_ = x;
  last_y_ = y;
  last_z_ = z;
  last_type_ = &type;
  has_last_ = true;
  const std::string biome = env.biome_name ? env.biome_name(x, y, z) : std::string();
  const std::optional<MobSpawnCost> cost =
      biome.empty() ? std::nullopt : biome_spawn_cost(biome, type.name);
  if (!cost) {
    last_charge_ = 0.0;
    return true;
  }
  last_charge_ = cost->charge;
  return potential_energy_change(x, y, z, cost->charge) <= cost->energy_budget;
}

void SpawnState::after_spawn(const GeneratedMob& type, int x, int y, int z,
                             const SpawnEnvironment& env) {
  double charge = 0.0;
  if (has_last_ && last_type_ == &type && x == last_x_ && y == last_y_ && z == last_z_) {
    charge = last_charge_;
  } else {
    const std::string biome = env.biome_name ? env.biome_name(x, y, z) : std::string();
    const std::optional<MobSpawnCost> cost =
        biome.empty() ? std::nullopt : biome_spawn_cost(biome, type.name);
    if (cost)
      charge = cost->charge;
  }
  if (charge != 0.0)
    charges_.push_back(Charge{x, y, z, charge});
  counts_[type.category] += 1;
  add_local_count(type.category, floor_div16(x), floor_div16(z));
}

double SpawnState::potential_energy_change(int x, int y, int z, double charge) const {
  if (charge == 0.0)
    return 0.0;
  double potential = 0.0;
  for (const Charge& point : charges_) {
    const double dx = static_cast<double>(point.x - x);
    const double dy = static_cast<double>(point.y - y);
    const double dz = static_cast<double>(point.z - z);
    const double dist_sqr = dx * dx + dy * dy + dz * dz;
    // `distSqr == 0 -> POSITIVE_INFINITY`: a point charge on the candidate
    // position refuses it outright however small the charge is.
    if (dist_sqr == 0.0)
      return std::numeric_limits<double>::infinity();
    potential += point.charge / std::sqrt(dist_sqr);
  }
  return potential * charge;
}

// ---- the pass --------------------------------------------------------------

std::vector<uint8_t> filtered_spawning_categories(const SpawnState& state, bool spawn_enemies,
                                                  bool spawn_persistent) {
  std::vector<uint8_t> categories;
  for (uint8_t category : kSpawningCategories) {
    const MobCategoryStats& stats = mob_category_stats(category);
    if (!spawn_enemies && !stats.friendly)
      continue;
    if (!spawn_persistent && stats.persistent)
      continue;
    if (!state.can_spawn_for_category_global(category))
      continue;
    categories.push_back(category);
  }
  return categories;
}

std::vector<SpawnedMob> spawn_category_for_chunk(SpawnState& state, uint8_t category, int cx,
                                                 int cz, const SpawnEnvironment& env) {
  if (!env.next_int || !env.world_surface_height)
    return {};
  const int x = cx * 16 + env.next_int(16);
  const int z = cz * 16 + env.next_int(16);
  // `getRandomPosWithin`: `chunk.getHeight(Heightmap.Types.WORLD_SURFACE, x, z)
  // + 1`, then a y anywhere from the world's floor up to it.
  const int top_empty_y = env.world_surface_height(x, z) + 1;
  const int span = top_empty_y - env.min_y + 1;
  const int y = env.min_y + (span > 0 ? env.next_int(span) : 0);
  if (y < env.min_y + 1)
    return {};
  return spawn_category_for_position(state, category, x, y, z, env);
}

std::vector<SpawnedMob> spawn_category_for_position(SpawnState& state, uint8_t category,
                                                    int start_x, int start_y, int start_z,
                                                    const SpawnEnvironment& env) {
  std::vector<SpawnedMob> spawned;
  if (!env.get_block || !env.next_int || !env.unit_random)
    return spawned;
  // `if (state.isRedstoneConductor(chunk, start)) return;` — a full collision
  // shape where the attempt starts refuses the whole attempt.
  if (block_blocks_motion(env.get_block(start_x, start_y, start_z).name))
    return spawned;

  int cluster_size = 0;
  for (int group = 0; group < kSpawnGroupCount; ++group) {
    int x = start_x;
    int z = start_z;
    const GeneratedBiomeSpawn* group_entry = nullptr;
    const GeneratedMob* group_type = nullptr;
    // `Mth.ceil(random.nextFloat() * 4.0f)`: the group is bounded by that many
    // attempts until a type is picked, and by the picked entry's `minCount ..
    // maxCount` afterwards — vanilla re-assigns `max` and the loop condition
    // re-reads it, which is what makes a large pack possible and a small one end
    // the group early.
    int max_attempts = static_cast<int>(std::ceil(env.unit_random() * 4.0));
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
      // `i += random.nextInt(5) - random.nextInt(5)` per axis, in vanilla's
      // left-to-right order. The two draws share the generator, so inline they
      // would take whichever order the compiler picked — and `-` would let it
      // hand the first draw to the subtrahend and flip the step's sign.
      const int walk_x_added = env.next_int(kSpawnWalkBound);
      const int walk_x_subtracted = env.next_int(kSpawnWalkBound);
      x += walk_x_added - walk_x_subtracted;
      const int walk_z_added = env.next_int(kSpawnWalkBound);
      const int walk_z_subtracted = env.next_int(kSpawnWalkBound);
      z += walk_z_added - walk_z_subtracted;
      const double xx = static_cast<double>(x) + 0.5;
      const double zz = static_cast<double>(z) + 0.5;
      // `level.getNearestPlayer(xx, yStart, zz, -1.0, false)`: no distance bound,
      // spectators excluded.
      const double nearest_dist_sqr = nearest_player_distance_sqr(state.players(), x, start_y, z);
      if (nearest_dist_sqr < 0.0 || !right_distance_to_player_and_spawn_point(
                                        env, state.players(), x, start_y, z, nearest_dist_sqr))
        continue;
      if (group_type == nullptr) {
        const std::string biome = env.biome_name ? env.biome_name(x, start_y, z) : std::string();
        const BiomeSpawnPick pick =
            biome.empty() ? BiomeSpawnPick{} : biome_spawn_pick(biome, category, env);
        // `Optional.empty() -> continue block0`: an empty list ends the attempt
        // for this chunk, it does not move on to another group.
        if (pick.empty())
          break;
        group_entry = pick.entry;
        group_type = pick.type;
        max_attempts = group_entry->min_count +
                       env.next_int(1 + group_entry->max_count - group_entry->min_count);
      }
      const MobCategoryStats& stats = mob_category_stats(category);
      // `!type.canSpawnFarFromPlayer() && distSqr > despawnDistance^2`.
      if (nearest_dist_sqr >
          static_cast<double>(stats.despawn_distance) * static_cast<double>(stats.despawn_distance))
        continue;
      // `SpawnPlacements.isSpawnPositionOk` then `checkSpawnRules`, then the
      // box test — `level.noCollision(type.getSpawnAABB(...))`.
      if (!spawn_position_ok(env, *group_type, x, start_y, z))
        continue;
      if (!check_spawn_rules(env, *group_type, x, start_y, z))
        continue;
      if (!spawn_box_is_clear(env, *group_type, xx, static_cast<double>(start_y), zz, false))
        continue;
      // `state::canSpawn`: the biome's spawn-cost budget.
      if (!state.can_spawn(*group_type, x, start_y, z, env))
        continue;
      // `isValidPositionForMob`: `mob.checkSpawnRules` (true by default) and
      // `mob.checkSpawnObstruction` (no fluid in the box, plus vanilla's
      // `isUnobstructed` entity overlap, which world/mob_spawn.h records as not
      // modelled). The far-away branch was already decided above, so only the
      // obstruction test is new here.
      if (!spawn_box_is_clear(env, *group_type, xx, static_cast<double>(start_y), zz, true))
        continue;
      SpawnedMob mob;
      mob.type = group_type;
      mob.x = xx;
      mob.y = static_cast<double>(start_y);
      mob.z = zz;
      mob.yaw = static_cast<float>(env.unit_random() * 360.0);
      state.after_spawn(*group_type, x, start_y, z, env);
      spawned.push_back(mob);
      // `if (++clusterSize >= mob.getMaxSpawnClusterSize()) return;` — four mobs
      // per chunk attempt, whatever the packs were.
      if (++cluster_size >= kMobMaxClusterSize)
        return spawned;
      // `mob.isMaxGroupSizeReached(groupSize)` is false for every mob this engine
      // has (`Mob.java:855`), so the group is bounded by `max_attempts` alone.
    }
  }
  return spawned;
}

std::vector<SpawnedMob> spawn_for_chunk(SpawnState& state, const std::vector<uint8_t>& categories,
                                        int cx, int cz, const SpawnEnvironment& env) {
  std::vector<SpawnedMob> spawned;
  for (uint8_t category : categories) {
    if (!state.can_spawn_for_category_local(category, cx, cz))
      continue;
    std::vector<SpawnedMob> here = spawn_category_for_chunk(state, category, cx, cz, env);
    if (here.empty())
      continue;
    spawned.insert(spawned.end(), here.begin(), here.end());
  }
  return spawned;
}

} // namespace mc::world
