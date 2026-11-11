// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for mobs (Phase 3 milestone I):
//   * the stat table — lookup by name and entity id, the vanilla attribute
//     values, and the measured speed conversion,
//   * Entity.move's physics: gravity, the landing, the fall distance, the jump
//     arc and the wall a mob cannot walk through,
//   * the AI: a hostile mob finds a player, walks to it and hits it on the
//     20-tick interval; a passive mob never attacks; a creative player is not
//     targetable,
//   * the target goals: acquisition needs line of sight (`mustSee`), a target it
//     already has is kept — and chased — for the sixty ticks of unseen memory
//     (three hundred for one that hit it) before the goal gives up, a nearer
//     player does not steal a target that is already held, being hurt provokes a
//     chase that ignores line of sight, and a blind mob cannot land a blow,
//   * hurt_mob: the armour step, the invulnerability window, knockback, death
//     attribution and the death report,
//   * path navigation's stuck detection: a mob that cannot move for a hundred
//     ticks drops its path (so the goal that owns it re-targets), a held node
//     whose walk overruns `timeoutLimit * 3` does the same, and a mob making
//     normal progress is never flagged,
//   * fire: a mob standing in a `minecraft:fire` block catches on the first tick
//     (it has no `getFireImmuneTicks` to walk up from) for the full eight
//     seconds, reports the flame flag on the change, takes `minecraft:in_fire`
//     while it is inside, and then burns down for eight `minecraft:on_fire`
//     ticks once it is out,
//   * the entity loot table (resolve_mob_drops) and the mob packets' bytes.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "server/entity_packets.h"
#include "world/loot.h"
#include "world/mob.h"
#include "world/mob_category.h"

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

#define CHECK_NEAR(a, b, eps)                                                                      \
  do {                                                                                             \
    const double va = (a), vb = (b);                                                               \
    if (std::fabs(va - vb) > (eps)) {                                                              \
      std::fprintf(stderr, "FAIL %s:%d: %s == %f, want %f\n", __FILE__, __LINE__, #a, va, vb);     \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

namespace {

std::function<double()> high_roll() {
  return [] { return 0.9; };
}

MobEnvironment flat_world(double height = 1.95) {
  (void)height;
  MobEnvironment env;
  env.is_solid = [](int, int y, int) { return y <= 0; };
  env.unit_random = [] { return 0.5; }; // deterministic: no stray variety
  return env;
}

MobPlayer player_at(double x, double y, double z, bool targetable = true, int32_t id = 100) {
  MobPlayer p;
  p.entity_id = id;
  p.x = x;
  p.y = y;
  p.z = z;
  p.can_be_targeted = targetable;
  return p;
}

Mob make_mob(const char* name, double x, double y, double z, int32_t id = 1) {
  Mob mob;
  mob.entity_id = id;
  const GeneratedMob* type = mob_by_name(name);
  CHECK(type != nullptr);
  if (type == nullptr)
    return mob;
  MobEnvironment env = flat_world();
  mob_place(mob, *type, env, x, y, z, 0.0f);
  return mob;
}

void test_table() {
  const GeneratedMob* zombie = mob_by_name("zombie");
  CHECK(zombie != nullptr && std::strcmp(zombie->name, "minecraft:zombie") == 0);
  CHECK(mob_by_name("Minecraft:ZOMBIE") == zombie);
  CHECK(mob_by_name("minecraft:zombie") == zombie);
  // The table now carries every living entity type, so the six Phase-3 mobs and
  // everything else resolve; only a name no entity type has stays unknown.
  CHECK(mob_by_name("creeper") != nullptr);
  CHECK(mob_by_name("minecraft:not_a_mob") == nullptr);
  CHECK(mob_by_entity_type_id(154) == zombie);
  CHECK(mob_by_entity_type_id(127) != nullptr);
  CHECK(mob_by_entity_type_id(9999) == nullptr);
  CHECK((zombie->flags & kMobHostile) != 0);
  CHECK(zombie->max_health == 20.0f);
  CHECK(zombie->attack_damage == 3.0f);
  CHECK(zombie->follow_range == 35.0);
  CHECK(zombie->armor == 2.0f);
  CHECK(zombie->xp_min == 5 && zombie->xp_max == 5);
  CHECK_NEAR(zombie->width, 0.6, 1e-6);
  CHECK_NEAR(zombie->height, 1.95, 1e-6);

  const GeneratedMob* pig = mob_by_name("pig");
  CHECK(pig != nullptr && (pig->flags & kMobHostile) == 0);
  CHECK(pig->attack_damage == 0.0f);
  CHECK(pig->xp_min == 1 && pig->xp_max == 3);

  const GeneratedMob* spider = mob_by_name("spider");
  CHECK(spider != nullptr && (spider->flags & kMobHostile) != 0);
  CHECK(spider->max_health == 16.0f && spider->attack_damage == 2.0f);

  // The measured conversion: m/s = attribute^2 * 43.1718, / 20 ticks.
  CHECK_NEAR(mob_speed_per_tick(*zombie), 0.23 * 0.23 * 43.1718 / 20.0, 1e-9);
  CHECK_NEAR(mob_speed_per_tick(*zombie), 0.1142, 1e-3);
  CHECK_NEAR(mob_speed_per_tick(*spider), 0.194, 1e-3);

  // Every living entity type the 26.2 server registers is in the table (the
  // player, the armour stand and the mannequin are the three the generator
  // drops), each row is self-consistent, and names and ids are unique.
  CHECK_EQ(kMobTypeCount, 90);
  for (int i = 0; i < kMobTypeCount; ++i) {
    const GeneratedMob& m = kMobs[i];
    CHECK(std::strncmp(m.name, "minecraft:", 10) == 0);
    CHECK(m.display_name != nullptr && m.display_name[0] != '\0');
    CHECK(m.entity_type_id > 0);
    CHECK(m.category < kSpawnCategoryCount);
    CHECK(m.width > 0.0f && m.height > 0.0f);
    CHECK(m.max_health > 0.0f);
    CHECK(m.movement_speed > 0.0);
    CHECK(m.attack_damage >= 0.0f);
    CHECK(m.follow_range >= 0.0);
    CHECK(m.xp_min >= 0 && m.xp_max >= m.xp_min);
    // Only an `Animal` has a baby box, and a baby that cannot be sized cannot
    // be born — so every animal row must have one.
    if ((m.flags & kMobAnimal) != 0)
      CHECK(m.baby_width > 0.0f && m.baby_height > 0.0f);
    CHECK(mob_by_entity_type_id(m.entity_type_id) == &kMobs[i]);
    for (int j = i + 1; j < kMobTypeCount; ++j) {
      CHECK(std::strcmp(m.name, kMobs[j].name) != 0);
      CHECK(m.entity_type_id != kMobs[j].entity_type_id);
    }
  }

  // The families the expansion added, one probe each: a retaliating animal
  // (wolf), an undead archer (skeleton), a neutral golem, a non-hostile
  // villager, and the two rows whose numbers are not 20 / 5.
  const GeneratedMob* wolf = mob_by_name("wolf");
  CHECK(wolf != nullptr && (wolf->flags & kMobAnimal) != 0 && (wolf->flags & kMobRetaliates) != 0 &&
        (wolf->flags & kMobHostile) == 0);
  const GeneratedMob* skeleton = mob_by_name("skeleton");
  CHECK(skeleton != nullptr && (skeleton->flags & kMobHostile) != 0 &&
        (skeleton->flags & kMobInvertedHealAndHarm) != 0);
  const GeneratedMob* golem = mob_by_name("iron_golem");
  CHECK(golem != nullptr && (golem->flags & kMobHostile) == 0 &&
        (golem->flags & kMobRetaliates) != 0 && golem->max_health == 100.0f);
  const GeneratedMob* villager = mob_by_name("villager");
  CHECK(villager != nullptr && (villager->flags & kMobHostile) == 0 &&
        (villager->flags & kMobAnimal) == 0);
  const GeneratedMob* warden = mob_by_name("warden");
  CHECK(warden != nullptr && warden->max_health == 500.0f &&
        (warden->flags & kMobNoFarDespawn) != 0);
  const GeneratedMob* dragon = mob_by_name("ender_dragon");
  CHECK(dragon != nullptr && (dragon->flags & kMobHostile) != 0 && dragon->xp_max == 0);
  // Fire immunity is the game's own column (`EntityType.fireImmune()`): the
  // nether types are immune, the overworld's ordinary ones are not — and the
  // strider is both an `Animal` and immune, so the two are independent.
  CHECK(mob_by_name("blaze")->fire_immune && mob_by_name("magma_cube")->fire_immune &&
        mob_by_name("strider")->fire_immune && mob_by_name("ender_dragon")->fire_immune);
  CHECK(!mob_by_name("zombie")->fire_immune && !mob_by_name("cow")->fire_immune &&
        !mob_by_name("iron_golem")->fire_immune);
}

void test_place_and_gravity() {
  Mob mob = make_mob("zombie", 0.5, 5.0, 0.5);
  CHECK(mob.health == 20.0f);
  CHECK(mob.type != nullptr && mob.type == mob_by_name("zombie"));

  // It falls and lands on the floor at y = 1, and stops there.
  MobEnvironment env = flat_world();
  std::vector<Mob> mobs{mob};
  for (int i = 0; i < 200; ++i)
    tick_mobs(mobs, env);
  CHECK(mobs.size() == 1);
  CHECK_NEAR(mobs[0].y, 1.0, 1e-6);
  CHECK(mobs[0].on_ground);

  // The drop is 4 blocks (5 -> 1), which is 1 above the safe fall distance, so
  // the landing costs exactly 1.0 and the mob survives.
  CHECK_NEAR(mobs[0].health, 20.0f - 1.0f, 1e-4);
  CHECK_NEAR(mobs[0].fall_distance, 0.0, 1e-9);

  // Unstuck: a mob summoned inside a block is lifted out of it.
  MobEnvironment with_roof;
  with_roof.is_solid = [](int, int y, int) { return y <= 0 || y == 1; };
  Mob buried;
  mob_place(buried, *mob_by_name("pig"), with_roof, 0.5, 1.0, 0.5, 0.0f);
  CHECK(buried.y >= 2.0);
}

void test_wall_stops_it() {
  // A one-block-high ridge across the whole world: the zombie can see the player
  // over it (the eyes are at 2.62, well above a single block) so it chases, and
  // the collision response — not the sight — is what stops it. The ridge runs the
  // full length of z so there is nothing to path around.
  MobEnvironment wall;
  wall.is_solid = [](int x, int y, int) {
    if (y <= 0)
      return true;
    return y == 1 && x >= 2;
  };
  wall.unit_random = [] { return 0.0; };
  wall.players.push_back(player_at(8.5, 1.0, 0.5));
  std::vector<Mob> mobs{make_mob("zombie", 0.5, 1.0, 0.5)};
  for (int i = 0; i < 120; ++i)
    tick_mobs(mobs, wall);
  CHECK(!mobs.empty());
  CHECK(mobs[0].target_entity_id == 100); // it is chasing
  CHECK(mobs[0].x > 1.0);                 // and it walked up to the ridge
  CHECK(mobs[0].x < 2.0);                 // but never into it (box stops at 1.7)
}

void test_pen_rim_is_not_climbable() {
  // The breeding scene's shape in one process: a floor slab, a two-block rim
  // around it, animals left to stroll with no player, for forty-eight seconds
  // of world clock. `kMobJumpVelocity` peaks under one block (0.42 against a
  // 0.08 gravity step), so the rim's top is 2.0 above the floor and no stroll
  // can reach it; `breeding_scene_soak.py` measures the same property from the
  // wire. The other way an animal ever reached a rim like this one — a summon
  // whose placement was lifted out of the rim column — is closed by
  // `mob_place`'s lateral nudge (`test_place_nudges_a_mob_off_a_rim`), so with
  // every animal starting on an interior cell the only force left is the
  // stroll.
  //
  // Interior cells are x = -5..5 by z = -3..3; the rim fills the columns at
  // x = +-6 and z = +-4, two blocks high.
  MobEnvironment pen;
  pen.is_solid = [](int x, int y, int z) {
    if (y <= 0)
      return true; // the floor slab the mobs stand on
    const bool rim_column = std::abs(x) == 6 || std::abs(z) == 4;
    const bool inside_footprint = std::abs(x) <= 6 && std::abs(z) <= 4;
    return inside_footprint && rim_column && y <= 2;
  };
  // A real stream of rolls: a pinned `unit_random` sends every stroll the same
  // way, and a cow that never reaches a wall is not a test of the wall.
  uint64_t state = 0x9E3779B97F4A7C15ull;
  pen.unit_random = [&state] {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<double>(state >> 11) * (1.0 / 9007199254740992.0);
  };
  std::vector<Mob> mobs;
  for (int i = 0; i < 6; ++i) {
    // Three columns across the interior, not the rim: the rim's own column is
    // |x| = 6, so the old `* 3.0` put the third cow at 6.5 — inside the wall,
    // where the first tick ended the stroll (`inside` is the loop's condition).
    Mob cow = make_mob("cow", 0.5 + (i % 3) * 2.5, 1.0, 0.5 + (i / 3) * 2.0, /*id=*/i);
    mobs.push_back(std::move(cow));
  }
  std::vector<double> start_x, start_z;
  for (const Mob& mob : mobs) {
    // The scene is what it claims to be before the first tick: nobody starts on
    // the rim or in the air, so a containment failure below is a stroll's.
    CHECK_NEAR(mob.y, 1.0, 1e-9);
    CHECK(std::floor(mob.x) >= -5.0 && std::floor(mob.x) <= 5.0);
    CHECK(std::floor(mob.z) >= -3.0 && std::floor(mob.z) <= 3.0);
    start_x.push_back(mob.x);
    start_z.push_back(mob.z);
  }
  bool on_floor = true;
  bool inside = true;
  double travelled = 0.0;
  for (int tick = 0; tick < 2400 && on_floor && inside; ++tick) {
    tick_mobs(mobs, pen);
    for (size_t i = 0; i < mobs.size(); ++i) {
      const Mob& mob = mobs[i];
      if (std::fabs(mob.y - 1.0) > 1.0e-6)
        on_floor = false; // feet off the floor: on the rim's top or falling
      if (std::floor(mob.x) < -5.0 || std::floor(mob.x) > 5.0 || std::floor(mob.z) < -3.0 ||
          std::floor(mob.z) > 3.0)
        inside = false; // the feet cell left the interior: it is in the rim
      travelled = std::max(travelled, std::hypot(mob.x - start_x[i], mob.z - start_z[i]));
    }
  }
  CHECK(on_floor);
  CHECK(inside);
  // The scene is not vacuous: the strolls walked at least a block and a half.
  CHECK(travelled > 1.5);
}

void test_overlapping_box_walks_out() {
  // `Entity.move` clips each axis only against the shape slices beyond the box's
  // *leading face* (`VoxelShape.collideX` starts at `aMax + 1`), so a block the
  // box already intersects is not a collision for that step. A cow that ends up
  // inside the ridge therefore walks back out — the port's target-box-only test
  // would freeze it, because every candidate position still overlaps the wall.
  MobEnvironment wall;
  wall.is_solid = [](int x, int y, int) { return y <= 0 || (y == 1 && x >= 2); };
  wall.unit_random = [] { return 0.5; };
  Mob cow = make_mob("cow", 2.2, 1.0, 0.5, /*id=*/0);
  // The box is [1.75, 2.65], so it overlaps the block cell x = 2 the ridge puts
  // there. (`mob_place` nudges a mob out of blocks on placement; the wire case
  // this mirrors is one that drifted in afterwards, so the test sets the
  // position directly.)
  CHECK(cow.x + cow.half_width() > 2.0 && cow.x - cow.half_width() < 2.0);
  cow.stray_cooldown = 10000;
  cow.has_path_goal = true;
  cow.path_to_x = cow.x;
  cow.path_to_y = cow.y;
  cow.path_to_z = cow.z;
  cow.path.points = {NavCell{-4, 1, 0}};
  cow.path_cooldown = 100000;
  cow.last_stuck_check_x = cow.x;
  cow.last_stuck_check_y = cow.y;
  cow.last_stuck_check_z = cow.z;
  std::vector<Mob> mobs{std::move(cow)};
  for (int i = 0; i < 40; ++i)
    tick_mobs(mobs, wall);
  CHECK(mobs[0].x + mobs[0].half_width() < 2.0); // clear of the cell it started in
  CHECK(mobs[0].x < 1.8);                        // and it kept walking west

  // The other half: a box that merely *touches* the ridge is still stopped. This
  // is what keeps the escape rule from turning the ridge into a doorway.
  Mob flush = make_mob("cow", 1.0, 1.0, 0.5, /*id=*/0);
  flush.stray_cooldown = 10000;
  flush.has_path_goal = true;
  flush.path_to_x = flush.x;
  flush.path_to_y = flush.y;
  flush.path_to_z = flush.z;
  flush.path.points = {NavCell{8, 1, 0}};
  flush.path_cooldown = 100000;
  flush.last_stuck_check_x = flush.x;
  flush.last_stuck_check_y = flush.y;
  flush.last_stuck_check_z = flush.z;
  std::vector<Mob> walkers{std::move(flush)};
  for (int i = 0; i < 60; ++i)
    tick_mobs(walkers, wall);
  // It walked up to the ridge, but never into it.
  CHECK(walkers[0].x > 1.4);
  CHECK(walkers[0].x + walkers[0].half_width() <= 2.0 + 1.0e-9);
}

void test_line_of_sight() {
  MobEnvironment open;
  open.is_solid = [](int, int y, int) { return y <= 0; };
  Mob zombie = make_mob("zombie", 0.5, 1.0, 0.5);
  // Entity.getEyeY: the dimensions' eye height is `height * 0.85`.
  CHECK_NEAR(mob_eye_y(zombie), 1.0 + 1.95 * 0.85, 1e-6);
  const MobPlayer p = player_at(3.5, 1.0, 0.5);
  CHECK_NEAR(player_eye_y(p), 2.62, 1e-6);
  CHECK(mob_has_line_of_sight(zombie, open, p.x, player_eye_y(p), p.z));

  // One block at eye level between them is enough (`Block.COLLIDER`), and the
  // clip is refused outright past 128 blocks.
  MobEnvironment wall;
  wall.is_solid = [](int x, int y, int) { return y <= 0 || (x == 2 && y == 2); };
  CHECK(!mob_has_line_of_sight(zombie, wall, p.x, player_eye_y(p), p.z));
  CHECK(!mob_has_line_of_sight(zombie, open, 200.0, 2.62, 0.5));

  // The raw segment is what the session's player-attack gate uses, with the
  // *player's* eye at the near end — LivingEntity.hasLineOfSight does not care
  // which way round it is called and neither does this, so a sword and a claw see
  // the same wall.
  CHECK(eye_line_of_sight(open.is_solid, p.x, player_eye_y(p), p.z, zombie.x, mob_eye_y(zombie),
                          zombie.z));
  CHECK(!eye_line_of_sight(wall.is_solid, p.x, player_eye_y(p), p.z, zombie.x, mob_eye_y(zombie),
                           zombie.z));
  CHECK(mob_has_line_of_sight(zombie, open, p.x, player_eye_y(p), p.z) ==
        eye_line_of_sight(open.is_solid, p.x, player_eye_y(p), p.z, zombie.x, mob_eye_y(zombie),
                          zombie.z));
  // A missing predicate is a world with nothing in it.
  const std::function<bool(int, int, int)> none;
  CHECK(eye_line_of_sight(none, p.x, player_eye_y(p), p.z, zombie.x, mob_eye_y(zombie), zombie.z));
}

void test_target_needs_sight() {
  // NearestAttackableTargetGoal was built with mustSee, so a player behind a
  // wall is never *acquired*: no target, and the mob just wanders.
  MobEnvironment blocked;
  blocked.is_solid = [](int x, int y, int) { return y <= 0 || (x == 3 && y >= 1); };
  blocked.unit_random = [] { return 0.5; };
  blocked.players.push_back(player_at(6.5, 1.0, 0.5));
  std::vector<Mob> mobs{make_mob("zombie", 0.5, 1.0, 0.5)};
  for (int i = 0; i < 40; ++i)
    tick_mobs(mobs, blocked);
  CHECK(mobs[0].target_entity_id == -1);
  CHECK(mobs[0].goal != MobGoal::Chase); // it wanders or stands, but never hunts
  CHECK(mobs[0].unseen_ticks == 0);
}

void test_target_memory_after_line_of_sight_breaks() {
  // The whole point of TargetGoal.unseenMemoryTicks: a mob that has seen a player
  // keeps after them — walking the path it had — when the player ducks out of
  // sight, instead of forgetting them the moment it cannot be seen.
  bool wall = false;
  MobEnvironment env;
  env.is_solid = [&wall](int x, int y, int) { return y <= 0 || (wall && x == 15 && y >= 1); };
  env.unit_random = [] { return 0.5; };
  env.players.push_back(player_at(20.5, 1.0, 0.5));
  std::vector<Mob> mobs{make_mob("zombie", 0.5, 1.0, 0.5)};
  for (int i = 0; i < 30; ++i)
    tick_mobs(mobs, env);
  CHECK(mobs[0].target_entity_id == 100);
  CHECK(mobs[0].target_visible);

  wall = true;
  const double seen_x = mobs[0].x;
  tick_mobs(mobs, env);
  CHECK(mobs[0].target_entity_id == 100);
  CHECK(!mobs[0].target_visible);
  CHECK(mobs[0].unseen_ticks == 1);
  for (int i = 0; i < kMobUnseenMemoryTicks - 1; ++i)
    tick_mobs(mobs, env);
  CHECK(mobs[0].unseen_ticks == kMobUnseenMemoryTicks);
  CHECK(mobs[0].target_entity_id == 100); // still inside the memory
  CHECK(mobs[0].goal == MobGoal::Chase);
  CHECK(mobs[0].x > seen_x + 4.0); // and it kept closing on where it last saw them

  tick_mobs(mobs, env); // one tick past the memory
  CHECK(mobs[0].target_entity_id == -1);
  CHECK(mobs[0].goal == MobGoal::Stroll); // MeleeAttackGoal.stop() -> wandering
  CHECK(mobs[0].unseen_ticks == 0);
}

void test_line_of_sight_walk_is_the_documented_sampling() {
  // `eye_line_of_sight` samples the segment in half-block steps — `steps =
  // ceil(distance / 0.5)` samples at `t = i / steps` for `i = 1 .. steps - 1`,
  // never the two endpoints — and asks the predicate about `floor(pos)` on each
  // axis. That is the walk's contract, and it is the tick's LOS cost: one world
  // lookup per sample. Pinned against a reference implementation over a seeded
  // corpus of segments, so a cheaper traversal (a voxel walk, a cell cache, an
  // early-out) has to keep every answer rather than only the common ones.
  const auto world = [](int x, int y, int z) {
    if (y <= 0)
      return true;
    return y <= 2 && (x % 7 == 0 || z % 7 == 0);
  };
  uint32_t state = 0x12345678u;
  const auto next = [&state]() {
    state = state * 1664525u + 1013904223u;
    return static_cast<double>(state >> 8) / 16777216.0;
  };
  int disagreements = 0;
  int blocked = 0;
  for (int i = 0; i < 4000; ++i) {
    const double fx = -20.0 + next() * 40.0;
    const double fy = 1.0 + next() * 2.0;
    const double fz = -20.0 + next() * 40.0;
    const double tx = -20.0 + next() * 40.0;
    const double ty = 1.0 + next() * 2.0;
    const double tz = -20.0 + next() * 40.0;
    const double dx = tx - fx, dy = ty - fy, dz = tz - fz;
    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    bool expected = true;
    if (distance > kMobMaxSightDistance) {
      expected = false;
    } else if (distance > 1.0e-9) {
      const int steps = static_cast<int>(std::ceil(distance / 0.5));
      for (int s = 1; s < steps; ++s) {
        const double t = static_cast<double>(s) / static_cast<double>(steps);
        const int bx = static_cast<int>(std::floor(fx + dx * t));
        const int by = static_cast<int>(std::floor(fy + dy * t));
        const int bz = static_cast<int>(std::floor(fz + dz * t));
        if (world(bx, by, bz)) {
          expected = false;
          break;
        }
      }
    }
    blocked += expected ? 0 : 1;
    if (eye_line_of_sight(world, fx, fy, fz, tx, ty, tz) != expected)
      ++disagreements;
  }
  CHECK_EQ(disagreements, 0);
  // The corpus has to be worth checking: some segments are blocked by the grid.
  CHECK(blocked > 250 && blocked < 3950);

  // A segment out of reach is refused without asking the world at all, and the
  // empty predicate means "no world" rather than "blocked".
  int asked = 0;
  const auto counting = [&asked](int, int, int) {
    ++asked;
    return false;
  };
  CHECK(!eye_line_of_sight(counting, 0.5, 2.0, 0.5, 200.5, 2.0, 0.5));
  CHECK_EQ(asked, 0);
  CHECK(eye_line_of_sight(std::function<bool(int, int, int)>(), 0.5, 2.0, 0.5, 200.5, 2.0, 0.5));
}

void test_hurt_provokes_the_chase() {
  // HurtByTargetGoal: the player who hit the mob becomes its target even though
  // it can neither see nor be seen, and that target is remembered for 300 unseen
  // ticks rather than the scan's 60.
  MobEnvironment env;
  env.is_solid = [](int x, int y, int) { return y <= 0 || (x == 3 && y >= 1); };
  env.unit_random = [] { return 0.5; };
  env.players.push_back(player_at(6.5, 1.0, 0.5));
  std::vector<Mob> mobs{make_mob("zombie", 0.5, 1.0, 0.5)};
  for (int i = 0; i < 30; ++i)
    tick_mobs(mobs, env);
  CHECK(mobs[0].target_entity_id == -1); // out of sight, so not acquired

  hurt_mob(mobs[0], 4.0f, 100, "Steve", 0.0, 0.0, 0.0);
  CHECK(mobs[0].provoked_fresh);
  tick_mobs(mobs, env);
  CHECK(mobs[0].provoked_entity_id == 100);
  CHECK(!mobs[0].provoked_fresh); // the goal reacted to the hit once
  CHECK(mobs[0].target_entity_id == 100);
  CHECK(mobs[0].target_provoked);
  CHECK(mobs[0].goal == MobGoal::Chase);

  // Long past the nearest-target memory the provoked one still stands.
  for (int i = 0; i < kMobUnseenMemoryTicks + 20; ++i)
    tick_mobs(mobs, env);
  CHECK(mobs[0].target_entity_id == 100);
  CHECK(mobs[0].unseen_ticks > kMobUnseenMemoryTicks);
  CHECK(mobs[0].unseen_ticks <= kMobProvokedMemoryTicks);
  CHECK(mobs[0].x > 1.0); // it went looking for the player behind the wall

  // ...and then the memory runs out like any other.
  for (int i = 0; i < kMobProvokedMemoryTicks; ++i)
    tick_mobs(mobs, env);
  CHECK(mobs[0].target_entity_id == -1);
}

void test_retaliation() {
  // A wolf is an `Animal`, not a `Monster`: its target selector holds
  // HurtByTargetGoal and nothing else, so a player standing in the open is
  // never acquired by a scan...
  MobEnvironment env = flat_world();
  env.players.push_back(player_at(6.5, 1.0, 0.5));
  std::vector<Mob> mobs{make_mob("wolf", 0.5, 1.0, 0.5)};
  for (int i = 0; i < 100; ++i)
    CHECK(tick_mobs(mobs, env).attacks.empty());
  CHECK(mobs[0].target_entity_id == -1);
  CHECK(mobs[0].goal != MobGoal::Chase);

  // ...but the player who hits it becomes its target, sight unseen, exactly
  // like the zombie's provoked chase — and the swing carries the wolf's own
  // attack damage rather than a zombie's 3.0.
  hurt_mob(mobs[0], 4.0f, 100, "Steve", 0.0, 0.0, 0.0);
  tick_mobs(mobs, env);
  CHECK(mobs[0].target_entity_id == 100);
  CHECK(mobs[0].target_provoked);
  CHECK(mobs[0].goal == MobGoal::Chase);
  int attacks = 0;
  for (int i = 0; i < 200; ++i) {
    for (const MobAttack& attack : tick_mobs(mobs, env).attacks) {
      ++attacks;
      CHECK(attack.mob_name == "Wolf");
      CHECK_NEAR(attack.damage, 4.0, 1e-6);
    }
  }
  CHECK(attacks > 3);

  // The other retaliation family: an iron golem is not hostile either, but it
  // answers a hit with its own 15.0 (and has no far-despawn, per its row).
  MobEnvironment yard = flat_world();
  yard.players.push_back(player_at(3.0, 1.0, 0.5));
  std::vector<Mob> golems{make_mob("iron_golem", 0.5, 1.0, 0.5)};
  hurt_mob(golems[0], 1.0f, 100, "Steve", 0.0, 0.0, 0.0);
  bool golem_hit = false;
  for (int i = 0; i < 100 && !golem_hit; ++i)
    for (const MobAttack& attack : tick_mobs(golems, yard).attacks) {
      golem_hit = true;
      CHECK(attack.mob_name == "Iron Golem");
      CHECK_NEAR(attack.damage, 15.0, 1e-6);
    }
  CHECK(golem_hit);

  // A pig has no targeting goal at all: `hurt_mob` records the blow, but the
  // selector never turns it into a target and no swing ever comes.
  MobEnvironment pen = flat_world();
  pen.players.push_back(player_at(1.0, 1.0, 0.5));
  std::vector<Mob> pigs{make_mob("pig", 0.5, 1.0, 0.5)};
  hurt_mob(pigs[0], 1.0f, 100, "Steve", 0.0, 0.0, 0.0);
  CHECK(pigs[0].provoked_fresh); // the hit set lastHurtByMob
  for (int i = 0; i < 40; ++i)
    CHECK(tick_mobs(pigs, pen).attacks.empty());
  CHECK(pigs[0].target_entity_id == -1);
  CHECK(pigs[0].goal != MobGoal::Chase);
}

void test_target_is_not_swapped() {
  // GoalSelector only asks a goal to start when none is running, so a second,
  // much nearer player does not steal a target the mob already has. Once the
  // first one is gone the scan runs again and takes the nearest.
  MobEnvironment env = flat_world();
  env.players.push_back(player_at(6.5, 1.0, 0.5, true, 100));
  std::vector<Mob> mobs{make_mob("zombie", 0.5, 1.0, 0.5)};
  for (int i = 0; i < 20; ++i)
    tick_mobs(mobs, env);
  CHECK(mobs[0].target_entity_id == 100);

  env.players.push_back(player_at(2.0, 1.0, 0.5, true, 200));
  for (int i = 0; i < 40; ++i)
    tick_mobs(mobs, env);
  CHECK(mobs[0].target_entity_id == 100); // the nearer one did not win

  env.players[0].can_be_targeted = false; // the first one turns creative
  for (int i = 0; i < 5; ++i)
    tick_mobs(mobs, env);
  CHECK(mobs[0].target_entity_id == 200);
}

void test_blind_mob_cannot_hit() {
  // MeleeAttackGoal.canPerformAttack wants `isWithinMeleeAttackRange &&
  // hasLineOfSight`. A spider reaching past a block at eye level is in reach and
  // out of sight, so the swing never happens — and it starts the instant the
  // block is gone.
  bool blocked = true;
  MobEnvironment env;
  env.is_solid = [&blocked](int x, int y, int) { return y <= 0 || (blocked && x == 1 && y == 2); };
  env.unit_random = [] { return 0.5; };
  env.players.push_back(player_at(2.0, 1.0, 0.5));
  std::vector<Mob> mobs{make_mob("spider", 0.5, 1.0, 0.5)};
  CHECK(tick_mobs(mobs, env).attacks.empty());
  CHECK(mobs[0].target_entity_id == -1); // the scan needs sight too

  // Being hit still provokes it, and it comes and stands in range...
  hurt_mob(mobs[0], 4.0f, 100, "Steve", 0.0, 0.0, 0.0);
  for (int i = 0; i < 60; ++i)
    CHECK(tick_mobs(mobs, env).attacks.empty());
  CHECK(mobs[0].target_entity_id == 100);
  CHECK(mobs[0].goal == MobGoal::Attack);
  CHECK(!mobs[0].target_visible);

  // ...but not until it can see.
  blocked = false;
  bool hit = false;
  for (int i = 0; i < 5 && !hit; ++i)
    hit = !tick_mobs(mobs, env).attacks.empty();
  CHECK(hit);
}

void test_hostile_chase_and_attack() {
  MobEnvironment env = flat_world();
  env.players.push_back(player_at(6.5, 1.0, 0.5));
  std::vector<Mob> mobs{make_mob("zombie", 0.5, 1.0, 0.5)};
  const double start = mobs[0].x;
  int attacks = 0;
  int first_attack_tick = -1;
  for (int i = 0; i < 200; ++i) {
    const MobTickReport report = tick_mobs(mobs, env);
    if (!report.attacks.empty()) {
      ++attacks;
      if (first_attack_tick < 0)
        first_attack_tick = i;
      CHECK(report.attacks[0].player_entity_id == 100);
      CHECK_NEAR(report.attacks[0].damage, 3.0, 1e-6);
      CHECK(report.attacks[0].mob_name == "Zombie");
      // A swing is only allowed from inside the goal's own stop line
      // (`isWithinMeleeAttackRange`), whatever the mob's position on the tick.
      CHECK(perceived_distance_sqr(mobs[0], env.players[0]) <=
            attack_reach_sqr(mobs[0], env.players[0]));
    }
  }
  CHECK(!mobs.empty());
  CHECK(mobs[0].x > start); // it closed the distance
  // ... and stopped at its reach, not at a calibrated distance: the goal walks
  // while `perceivedDistanceSqr > attackReachSqr` and swings from inside it, so
  // the end state is the *band* `perceived <= reach` (both read from the same
  // pair of helpers the tick uses) — inside the line, and short of the player
  // rather than through them.
  CHECK(perceived_distance_sqr(mobs[0], env.players[0]) <=
        attack_reach_sqr(mobs[0], env.players[0]));
  CHECK(mobs[0].x < env.players[0].x);
  CHECK(attacks > 3); // 200 ticks / 20-tick interval
  CHECK(first_attack_tick >= 0 && first_attack_tick < 100);

  // The melee interval is 20 ticks: no two attacks closer than that.
  MobEnvironment near = flat_world();
  near.players.push_back(player_at(1.0, 1.0, 0.5));
  std::vector<Mob> adjacent{make_mob("zombie", 0.5, 1.0, 0.5)};
  int ticks_since = 999;
  int seen = 0;
  for (int i = 0; i < 100; ++i) {
    ++ticks_since;
    if (!tick_mobs(adjacent, near).attacks.empty()) {
      if (seen > 0)
        CHECK(ticks_since >= kMobAttackInterval);
      ticks_since = 0;
      ++seen;
    }
  }
  CHECK(seen >= 4);
}

void test_passive_and_untargetable() {
  // A pig never attacks, even with a player standing on it.
  MobEnvironment env = flat_world();
  env.players.push_back(player_at(1.0, 1.0, 0.5));
  std::vector<Mob> pigs{make_mob("pig", 0.5, 1.0, 0.5)};
  for (int i = 0; i < 200; ++i)
    CHECK(tick_mobs(pigs, env).attacks.empty());
  CHECK(pigs[0].target_entity_id == -1);

  // A creative player is not a target (EntitySelector.NO_CREATIVE_OR_SPECTATOR).
  MobEnvironment creative = flat_world();
  creative.players.push_back(player_at(2.5, 1.0, 0.5, /*targetable=*/false));
  std::vector<Mob> mobs{make_mob("zombie", 0.5, 1.0, 0.5)};
  for (int i = 0; i < 40; ++i)
    CHECK(tick_mobs(mobs, creative).attacks.empty());
  CHECK(mobs[0].target_entity_id == -1);

  // The acquisition range is FOLLOW_RANGE (35), pinned as a *range* from both
  // ends rather than from the far one alone: the scan skips a candidate whose
  // `d2 >= follow * follow`, so 34.5 blocks of gap is acquired and exactly 35.0
  // is not — the line is strict, and a reader of the old comment ("inside it,
  // found") had nothing that checked the finding.
  MobEnvironment range = flat_world();
  range.players.push_back(player_at(35.0, 1.0, 0.5)); // a gap of 34.5
  std::vector<Mob> inside{make_mob("zombie", 0.5, 1.0, 0.5)};
  tick_mobs(inside, range);
  CHECK(inside[0].target_entity_id == 100);
  range.players[0] = player_at(35.5, 1.0, 0.5); // a gap of exactly 35.0
  std::vector<Mob> edge{make_mob("zombie", 0.5, 1.0, 0.5)};
  tick_mobs(edge, range);
  CHECK(edge[0].target_entity_id == -1);
  range.players[0] = player_at(60.5, 1.0, 0.5);
  std::vector<Mob> walkers{make_mob("zombie", 0.5, 1.0, 0.5)};
  tick_mobs(walkers, range);
  CHECK(walkers[0].target_entity_id == -1);
  CHECK_NEAR(mobs[0].type->follow_range, 35.0, 1e-9);
}

void test_stray_walk() {
  // A passive mob wanders: with a fixed random draw it picks a direction and
  // actually moves, and it never leaves the ground.
  MobEnvironment env = flat_world();
  env.unit_random = [] { return 0.25; };
  std::vector<Mob> pigs{make_mob("pig", 0.5, 1.0, 0.5)};
  bool moved = false;
  bool picked = false;
  for (int i = 0; i < 120; ++i) {
    // The tick's own start position, so the leg can be measured from where the
    // spot was picked (the stroll runs before `move_mob`).
    const double x_before = pigs[0].x;
    const double z_before = pigs[0].z;
    tick_mobs(pigs, env);
    if (pigs[0].has_path_goal && !picked) {
      // The stroll's promise is a *band*, not a mean: `stroll` picks a spot
      // `ranged(env, 1.0, 3.0)` blocks away, and that is what the target reads
      // back (1.5 with the pinned 0.25 draw, the whole band is the claim).
      const double leg = std::hypot(pigs[0].path_to_x - x_before, pigs[0].path_to_z - z_before);
      CHECK(leg >= 1.0 && leg <= 3.0);
      picked = true;
    }
    if (std::abs(pigs[0].x - 0.5) + std::abs(pigs[0].z - 0.5) > 0.5)
      moved = true;
  }
  CHECK(moved);
  CHECK(picked);
  CHECK_NEAR(pigs[0].y, 1.0, 1e-6);
}

void test_stuck_detection() {
  // `PathNavigation.doStuckDetection`'s movement check: a mob that has moved
  // less than `effectiveSpeed * 100 * 0.25` blocks in a hundred ticks has its
  // navigation stopped, which is what ends the goal that owns it. A cow's 0.2
  // movement attribute squares to 0.04, so the budget is exactly one block.
  //
  // The scene pins the shape the breeding wire test hit: a held path that
  // points through a wall the physics will not let the mob pass. The cow starts
  // flush against the ridge and pushes at it for the whole window, so the
  // displacement is ~0.05 blocks, not the ~8.6 a walking cow covers.
  MobEnvironment wall;
  wall.is_solid = [](int x, int y, int) { return y <= 0 || (y == 1 && x >= 2); };
  wall.unit_random = [] { return 0.5; };
  Mob cow = make_mob("cow", 1.5, 1.0, 0.5, /*id=*/0);
  cow.stray_cooldown = 10000; // the stroll keeps this held path (no new target)
  cow.has_path_goal = true;
  cow.path_to_x = cow.x;
  cow.path_to_y = cow.y;
  cow.path_to_z = cow.z;
  cow.path.points = {NavCell{6, 1, 0}};
  cow.path_cooldown = 100000;
  // A path installed through `moveTo` seeds the window from the mob's position
  // (the test assigns the path directly, so it does what moveTo would).
  cow.last_stuck_check_x = cow.x;
  cow.last_stuck_check_y = cow.y;
  cow.last_stuck_check_z = cow.z;
  std::vector<Mob> mobs{std::move(cow)};
  int stuck_on = -1;
  for (int i = 0; i < 110 && stuck_on < 0; ++i) {
    tick_mobs(mobs, wall);
    if (mobs[0].is_stuck)
      stuck_on = i + 1;
  }
  // The check is `tick - lastStuckCheck > 100`, with nav_tick starting at 1 on
  // the first navigation tick and the last check at 0.
  CHECK_EQ(stuck_on, 101);
  CHECK(mobs[0].path.done()); // `stop()` cleared it
  CHECK(mobs[0].is_stuck);
  // And a cleared path is what ends the stroll: the next tick drops the target
  // and idles out the cooldown instead of holding the dead plan forever.
  tick_mobs(mobs, wall);
  CHECK(!mobs[0].has_path_goal);
  CHECK(mobs[0].goal == MobGoal::Idle);
}

void test_stuck_detection_timeout_path() {
  // `doStuckDetection`'s other arm: the same next node held for longer than
  // three times the walk it should take. The movement check is pushed out of
  // reach so this exercises `timeoutPath` alone. The node 0.95 blocks away is
  // cached first, then accumulates `mob.age`; with a 0.2 attribute the limit is
  // 0.95 / 0.2 * 20 = 95 ticks and the path must go just past 285.
  MobEnvironment wall;
  wall.is_solid = [](int x, int y, int) { return y <= 0 || (y == 1 && x >= 2); };
  wall.unit_random = [] { return 0.5; };
  Mob cow = make_mob("cow", 1.5, 1.0, 0.5, /*id=*/0);
  cow.stray_cooldown = 10000;
  cow.has_path_goal = true;
  cow.path_to_x = cow.x;
  cow.path_to_y = cow.y;
  cow.path_to_z = cow.z;
  cow.path.points = {NavCell{2, 1, 0}};
  cow.path_cooldown = 100000;
  cow.last_stuck_check_x = cow.x;
  cow.last_stuck_check_y = cow.y;
  cow.last_stuck_check_z = cow.z;
  cow.last_stuck_check = 1000000; // only the timeout arm can fire
  std::vector<Mob> mobs{std::move(cow)};
  for (int i = 0; i < 250; ++i)
    tick_mobs(mobs, wall);
  CHECK(!mobs[0].path.done()); // still inside the budget
  for (int i = 0; i < 80 && !mobs[0].path.done(); ++i)
    tick_mobs(mobs, wall);
  CHECK(mobs[0].path.done()); // `timeoutPath` stopped it
  CHECK(!mobs[0].is_stuck);   // `resetStuckTimeout` clears the flag
}

void test_stuck_detection_never_fires_on_a_walk() {
  // The negative control: a mob covering normal ground must never trip either
  // arm. A cow walks ~0.086 blocks a tick, so a hundred-tick window carries it
  // ~8.6 blocks against the one-block budget; the path is long enough that it
  // is still walking at the end of the run.
  MobEnvironment flat = flat_world();
  Mob cow = make_mob("cow", 0.5, 1.0, 0.5, /*id=*/0);
  cow.stray_cooldown = 10000;
  cow.has_path_goal = true;
  cow.path_to_x = cow.x;
  cow.path_to_y = cow.y;
  cow.path_to_z = cow.z;
  for (int x = 1; x <= 60; ++x)
    cow.path.points.push_back(NavCell{x, 1, 0});
  cow.path_cooldown = 100000;
  cow.last_stuck_check_x = cow.x;
  cow.last_stuck_check_y = cow.y;
  cow.last_stuck_check_z = cow.z;
  std::vector<Mob> mobs{std::move(cow)};
  bool flagged = false;
  for (int i = 0; i < 400; ++i) {
    tick_mobs(mobs, flat);
    flagged = flagged || mobs[0].is_stuck;
  }
  CHECK(!flagged);
  CHECK(mobs[0].x > 20.0);     // it really walked
  CHECK(!mobs[0].path.done()); // and the plan was not cleared
}

void test_hurt_armour_and_window() {
  Mob mob = make_mob("zombie", 0.5, 1.0, 0.5);
  // CombatRules.getDamageAfterAbsorb with a zombie's 2.0 armour:
  //   f = 2 + 0/4 = 2; g = clamp(2 - 10/2, 0.4, 20) = 0.4; 10 * (1 - 0.4/25) = 9.84.
  const MobHurtResult hit = hurt_mob(mob, 10.0f, 100, "Steve", 0.0, 0.0, 0.0);
  CHECK(hit.accepted);
  CHECK_NEAR(hit.applied, 9.84f, 1e-3);
  CHECK_NEAR(mob.health, 20.0f - 9.84f, 1e-3);
  CHECK(mob.killed_by_player);
  CHECK(mob.last_attacker_entity_id == 100);
  CHECK(mob.last_attacker_name == "Steve");

  // Inside the invulnerability window a second, smaller hit is refused...
  const MobHurtResult second = hurt_mob(mob, 5.0f, 100, "Steve", 0.0, 0.0, 0.0);
  CHECK(!second.accepted);
  // ...and a bigger one only lands for the difference (LivingEntity.hurt).
  const MobHurtResult bigger = hurt_mob(mob, 12.0f, 100, "Steve", 0.0, 0.0, 0.0);
  CHECK(bigger.accepted && bigger.reduced);
  // Only the 2.0 above the first hit lands, then the same armour step:
  // g = clamp(2 - 2/2, 0.4, 20) = 1.0; 2 * (1 - 1/25) = 1.92.
  CHECK_NEAR(bigger.applied, 1.92f, 1e-3);

  // The window runs out on the simulation's clock.
  MobEnvironment env = flat_world();
  std::vector<Mob> mobs{mob};
  for (int i = 0; i < static_cast<int>(kMobInvulnerableTicks) + 1; ++i)
    tick_mobs(mobs, env);
  CHECK(mobs[0].invulnerable_time == 0.0f);
  const MobHurtResult third = hurt_mob(mobs[0], 4.0f, 100, "Steve", 0.0, 0.0, 0.0);
  CHECK(third.accepted && !third.reduced);
}

void test_wither_armour_phase() {
  Mob wither = make_mob("wither", 0.5, 1.0, 0.5);
  CHECK_NEAR(wither.health, 300.0f, 1e-6);
  CHECK(!mob_wither_powered(wither));
  CHECK_NEAR(mob_armor_value(wither), 4.0f, 1e-6);
  CHECK(!mob_deflects_projectiles(wither));

  // The phase starts exactly at half health (`<=`), and the armour follows.
  wither.health = 150.0f;
  CHECK(mob_wither_powered(wither));
  CHECK_NEAR(mob_armor_value(wither), 8.0f, 1e-6);
  CHECK(mob_deflects_projectiles(wither));

  // The armour step reads that value: 10 damage against 4 points is
  //   clamp(4 - 10/2, 0.8, 20) = 0.8; 10 * (1 - 0.8/25) = 9.68,
  // and against the powered 8 it is 8.8. The phase is therefore a real
  // reduction on the same blow that crosses it, not a later tick's bonus.
  wither.invulnerable_time = 0.0f;
  wither.health = 300.0f;
  const MobHurtResult full = hurt_mob(wither, 10.0f, 100, "Steve", 0.0, 0.0, 0.0);
  CHECK_NEAR(full.applied, 9.68f, 1e-3);
  wither.invulnerable_time = 0.0f;
  wither.health = 150.0f;
  const MobHurtResult powered = hurt_mob(wither, 10.0f, 100, "Steve", 0.0, 0.0, 0.0);
  CHECK_NEAR(powered.applied, 8.8f, 1e-3);

  // A mob that is not the wither keeps its own row's armour and never deflects.
  Mob pig = make_mob("pig", 0.5, 1.0, 0.5);
  CHECK(!mob_wither_powered(pig));
  CHECK(!mob_deflects_projectiles(pig));
  CHECK_NEAR(mob_armor_value(pig), 0.0f, 1e-6);
}

void test_hurt_knockback_and_death() {
  Mob mob = make_mob("pig", 0.5, 1.0, 0.5);
  mob.on_ground = true;
  // LivingEntity.knockback along +x: the mob's momentum halves and it is pushed.
  hurt_mob(mob, 1.0f, 100, "Steve", 0.5, 1.0, 0.0);
  CHECK(mob.vx < 0.0); // delta.x / 2 - push
  CHECK(mob.vy > 0.0);
  CHECK(!mob.on_ground);

  // A lethal hit kills and attributes it.
  Mob victim = make_mob("zombie", 0.5, 1.0, 0.5);
  const MobHurtResult lethal = hurt_mob(victim, 1000.0f, 42, "Alex", 0.0, 0.0, 0.0);
  CHECK(lethal.died);
  CHECK_NEAR(victim.health, 0.0f, 1e-6);

  // The next tick reports the death with the killer, the position and the
  // "killed by player" flag the loot gate reads, and erases the mob.
  MobEnvironment env = flat_world();
  std::vector<Mob> mobs{victim};
  const MobTickReport report = tick_mobs(mobs, env);
  CHECK(mobs.empty());
  CHECK(report.deaths.size() == 1);
  CHECK(report.deaths[0].killed_by_player);
  CHECK(report.deaths[0].killer_entity_id == 42);
  CHECK(report.deaths[0].killer_name == "Alex");
  CHECK(report.deaths[0].type != nullptr &&
        std::strcmp(report.deaths[0].type->name, "minecraft:zombie") == 0);
  CHECK_NEAR(report.deaths[0].y, 1.0, 1e-6);

  // A mob killed outside the tick is picked up on the next one (the session's
  // attack path relies on it).
  std::vector<Mob> pending{make_mob("sheep", 0.5, 1.0, 0.5)};
  mobs.swap(pending);
  hurt_mob(mobs[0], 100.0f, 7, "Alex", 0.0, 0.0, 0.0);
  const MobTickReport later = tick_mobs(mobs, env);
  CHECK(mobs.empty());
  CHECK(later.deaths.size() == 1 && later.deaths[0].killed_by_player);
}

void test_fall_damage_kills() {
  // A fall of 15 blocks: 12 damage, and the landing is attributed to nobody, so
  // it drops nothing (that is what the "killed by player" gate is for).
  MobEnvironment env = flat_world();
  std::vector<Mob> mobs{make_mob("chicken", 0.5, 16.0, 0.5)};
  // A chicken has 4 health; give it more so the fall is not lethal, to test the
  // arithmetic rather than the death.
  mobs[0].health = 20.0f;
  for (int i = 0; i < 200; ++i)
    tick_mobs(mobs, env);
  CHECK(!mobs.empty());
  CHECK_NEAR(mobs[0].y, 1.0, 1e-6);
  CHECK_NEAR(mobs[0].health, 20.0f - 12.0f, 1e-3); // floor(15 - 3)
}

void test_below_world_discard() {
  // `Entity.checkBelowWorld`, in `Entity.baseTick` just after the fire block: a
  // mob whose feet fall more than 64 blocks under the dimension's min build
  // height is taken by `onBelowWorld()` -> `discard()`. The overworld's min
  // build height is -64, so the floor is y = -128 — a mob that falls off the
  // world is removed there instead of falling forever. The discard is a bare
  // removal: no death event, no loot, no experience, exactly like a despawn.
  MobEnvironment void_world; // nothing is solid: the mob falls freely
  void_world.unit_random = [] { return 0.5; };
  CHECK_EQ(void_world.min_y, -64); // the overworld's floor is the default

  // Falling from the sky, the mob is gone within a bounded number of ticks, and
  // the tick that takes it is a discard (one entry in `despawned`, no death) —
  // it never falls forever.
  std::vector<Mob> mobs{make_mob("zombie", 0.5, 100.0, 0.5)};
  bool discarded = false;
  for (int i = 0; i < 3000 && !mobs.empty(); ++i) {
    const MobTickReport report = tick_mobs(mobs, void_world);
    if (!report.despawned.empty()) {
      CHECK_EQ(report.despawned.size(), 1u);
      CHECK_EQ(report.despawned[0], 1);
      CHECK(report.deaths.empty()); // a discard is not a death
      discarded = true;
    }
  }
  CHECK(discarded);
  CHECK(mobs.empty());

  // The boundary is strict, exactly like the `<` in `checkBelowWorld`: y = -128
  // is still in the world, and the first position under it is not. The check
  // runs at the top of the tick (the previous tick's landing position), so a
  // mob dropped straight past the line goes on the next tick.
  std::vector<Mob> edge{make_mob("zombie", 0.5, -128.0, 0.5)};
  CHECK_EQ(tick_mobs(edge, void_world).despawned.size(), 0u);
  CHECK_EQ(edge.size(), 1u);
  edge[0].y = -128.5;
  CHECK_EQ(tick_mobs(edge, void_world).despawned.size(), 1u);
  CHECK(edge.empty());

  // The line moves with the dimension: a level whose min build height is 0
  // discards below -64, not -128 — and a mob above that floor stays.
  MobEnvironment raised = void_world;
  raised.min_y = 0;
  std::vector<Mob> deep{make_mob("pig", 0.5, -70.0, 0.5)};
  CHECK_EQ(tick_mobs(deep, raised).despawned.size(), 1u);
  CHECK(deep.empty());
  std::vector<Mob> shallow{make_mob("pig", 0.5, -60.0, 0.5)};
  CHECK_EQ(tick_mobs(shallow, raised).despawned.size(), 0u);
  CHECK_EQ(shallow.size(), 1u);
}

void test_despawn_scan_order() {
  // `Mob.checkDespawn`'s nearest-player scan only ever feeds two comparisons
  // against the type's own radii, so it stops at the first player inside the
  // no-despawn radius. That is a reorder of the scan, not a change of the rule:
  // the discard, the counter reset and — the trap — the RNG draw all have to
  // stay exactly as vanilla orders them.
  int draws = 0;
  MobEnvironment env = flat_world();
  env.players.clear();
  env.unit_random = [&draws] {
    ++draws;
    return 0.5;
  };
  for (int i = 0; i < 10; ++i) {
    const double angle = 0.7 * i;
    const double radius = 20.0 + i * 9.0; // the first is inside the 32-block radius
    env.players.push_back(
        player_at(std::cos(angle) * radius, 1.0, std::sin(angle) * radius, true, 100 + i));
  }
  Mob mob = make_mob("zombie", 0.5, 1.0, 0.5);
  mob.no_action_time = kMobDespawnTicks + 1;
  CHECK(!check_despawn(mob, env));
  // `noActionTime > 600` draws `random.nextInt(800)` *before* the distance test
  // short-circuits, so standing next to a player still costs the caller one draw
  // per tick and the stream cannot shift under the early-out.
  CHECK_EQ(draws, 1);
  CHECK_EQ(mob.no_action_time, 0); // and the near branch is the reset

  // The roll is only drawn once the activity counter has run out.
  draws = 0;
  mob.no_action_time = kMobDespawnTicks;
  CHECK(!check_despawn(mob, env));
  CHECK_EQ(draws, 0);

  // Persistence comes first of all: no scan, no draw, and the counter is
  // cleared (vanilla's `if (isPersistenceRequired()) { noActionTime = 0; }`).
  draws = 0;
  mob.persistence_required = true;
  mob.no_action_time = 900;
  CHECK(!check_despawn(mob, env));
  CHECK_EQ(draws, 0);
  CHECK_EQ(mob.no_action_time, 0);

  // The far branch still discards at once, without a draw: 200 blocks from the
  // only player is past a hostile's 128-block despawn distance.
  draws = 0;
  mob.persistence_required = false;
  mob.no_action_time = 900;
  MobEnvironment far = env;
  far.players.clear();
  far.players.push_back(player_at(200.5, 1.0, 0.5));
  CHECK(check_despawn(mob, far));
  CHECK_EQ(draws, 0);

  // ...and the whole rule against a corpus of layouts and counters, with the
  // nearest distance computed here: the reorder has to agree with it on every
  // discard, every counter value and every draw.
  uint32_t state = 0x9e3779b9u;
  const auto next = [&state]() {
    state = state * 1664525u + 1013904223u;
    return static_cast<double>(state >> 8) / 16777216.0;
  };
  int checked = 0;
  for (int round = 0; round < 500; ++round) {
    const bool cow = (round % 3) == 0;
    Mob m = make_mob(cow ? "cow" : "zombie", next() * 20.0, 1.0, next() * 20.0);
    m.no_action_time = static_cast<int>(next() * 900.0);
    m.persistence_required = next() < 0.2;
    MobEnvironment e = flat_world();
    e.players.clear();
    const int count = static_cast<int>(next() * 7.0);
    for (int i = 0; i < count; ++i) {
      MobPlayer p =
          player_at(m.x + (next() - 0.5) * 300.0, 1.0, m.z + (next() - 0.5) * 300.0, true, 200 + i);
      p.spectator = (i == 0 && count > 1 && next() < 0.3);
      e.players.push_back(p);
    }
    int used = 0;
    e.unit_random = [&used] {
      ++used;
      return 0.5; // the 1-in-800 roll never passes at 0.5
    };
    // The nearest non-spectator player, as vanilla's scan would find it.
    double nearest = -1.0;
    for (const MobPlayer& p : e.players) {
      if (p.spectator)
        continue;
      const double dx = p.x - m.x, dy = p.y - m.y, dz = p.z - m.z;
      const double d2 = dx * dx + dy * dy + dz * dz;
      if (nearest < 0.0 || d2 < nearest)
        nearest = d2;
    }
    const MobCategoryStats& stats = mob_category_stats(m.type->category);
    const bool removes = (m.type->flags & kMobNoFarDespawn) == 0;
    const double despawn_sqr =
        static_cast<double>(stats.despawn_distance) * static_cast<double>(stats.despawn_distance);
    const double no_despawn_sqr = static_cast<double>(stats.no_despawn_distance) *
                                  static_cast<double>(stats.no_despawn_distance);
    const int before = m.no_action_time;
    const bool expected_discard =
        !m.persistence_required && nearest >= 0.0 && nearest > despawn_sqr && removes;
    const bool expected_draw =
        !m.persistence_required && !expected_discard && nearest >= 0.0 && before > kMobDespawnTicks;
    const bool discarded = check_despawn(m, e);
    CHECK_EQ(discarded, expected_discard);
    CHECK_EQ(used, expected_draw ? 1 : 0);
    int expected_action = before;
    if (m.persistence_required)
      expected_action = 0;
    else if (!discarded && nearest >= 0.0 && nearest < no_despawn_sqr)
      expected_action = 0;
    CHECK_EQ(m.no_action_time, expected_action);
    ++checked;
  }
  CHECK_EQ(checked, 500);
}

void test_hurt_then_tick_reports_hurt() {
  MobEnvironment env = flat_world();
  std::vector<Mob> mobs{make_mob("chicken", 0.5, 8.0, 0.5)};
  bool saw_hurt = false;
  for (int i = 0; i < 200; ++i) {
    const MobTickReport report = tick_mobs(mobs, env);
    if (!report.hurt.empty())
      saw_hurt = true;
  }
  CHECK(saw_hurt); // the landing raised a hurt animation
}

void test_fire_burns_a_mob() {
  // A mob standing in a fire block. `LivingEntity.tick -> applyEffectsFromBlocks`
  // runs `BaseFireBlock.entityInside` for it every tick: the mob catches on the
  // *first* one (its `getFireImmuneTicks` is 0, where a player's is 20) for the
  // full eight seconds, takes `minecraft:in_fire` while it is inside, and reports
  // the flame-flag change exactly once.
  auto lit = std::make_shared<bool>(true);
  MobEnvironment env = flat_world();
  env.get_block = [lit](int, int y, int) {
    if (*lit && y == 1)
      return BlockStateKey{"minecraft:fire", "age=0"};
    return BlockStateKey{"minecraft:air", ""};
  };
  std::vector<Mob> mobs{make_mob("zombie", 0.5, 1.0, 0.5)};
  CHECK(mobs[0].health == 20.0f);
  CHECK(!mobs[0].burning.on_fire());

  int flames_on = 0;
  bool took_fire_damage = false;
  for (int i = 0; i < 3; ++i) {
    const MobTickReport report = tick_mobs(mobs, env);
    CHECK(!mobs.empty());
    if (!report.hurt.empty())
      took_fire_damage = true;
    for (int32_t id : report.flags_changed) {
      CHECK(id == mobs[0].entity_id);
      CHECK(mobs[0].burning.on_fire());
      ++flames_on;
    }
  }
  CHECK(took_fire_damage); // `minecraft:in_fire` landed
  CHECK_EQ(flames_on, 1);  // `setSharedFlagOnFire` only sends on the change
  CHECK(mobs[0].burning.on_fire());
  CHECK_EQ(mobs[0].burning.remaining_ticks, 160); // `igniteForSeconds(8.0f)`
  CHECK(mobs[0].health < 20.0f);

  // Out of the fire, the eight seconds burn down: `minecraft:on_fire` every 20
  // ticks (it bypasses armour, so each is a full 1.0) and the flag flips back
  // once at the end. A mob's idle countdown is 0, not a player's -20.
  *lit = false;
  const float before = mobs[0].health;
  int flames_off = 0;
  int on_fire_hits = 0;
  for (int i = 0; i < 200; ++i) {
    const MobTickReport report = tick_mobs(mobs, env);
    CHECK(!mobs.empty());
    for (int32_t id : report.hurt)
      if (id == mobs[0].entity_id)
        ++on_fire_hits;
    for (int32_t id : report.flags_changed) {
      CHECK(id == mobs[0].entity_id);
      CHECK(!mobs[0].burning.on_fire());
      ++flames_off;
    }
  }
  CHECK(!mobs[0].burning.on_fire());
  CHECK_EQ(mobs[0].burning.remaining_ticks, 0);
  CHECK_EQ(flames_off, 1);
  // The countdown is 160 ticks and `baseTick` hurts on the twentieth, so eight
  // ticks of the burn are due. The *first* of them can still fall inside the
  // tail of the last `in_fire` hit's ten-tick cooldown
  // (`LivingEntity.hurtServer`'s `invulnerableTime > 10` gate), which swallows
  // it — so the hits that land are counted from the report rather than assumed,
  // and each is a full point because `minecraft:on_fire` bypasses armour.
  CHECK(on_fire_hits >= 6);
  CHECK_NEAR(before - mobs[0].health, static_cast<float>(on_fire_hits), 1e-3);

  // A fire-immune type standing in the same flames is neither lit nor hurt:
  // `BaseFireBlock.fireIgnite` returns at once for it and
  // `Entity.isInvulnerableTo` refuses `minecraft:in_fire` (`IS_FIRE`), so the
  // health bar never moves and no flame flag is ever raised.
  std::vector<Mob> immune{make_mob("blaze", 0.5, 1.0, 0.5)};
  *lit = true;
  for (int i = 0; i < 60; ++i) {
    const MobTickReport report = tick_mobs(immune, env);
    CHECK(immune.size() == 1);
    CHECK(report.hurt.empty());
    CHECK(report.flags_changed.empty());
  }
  CHECK(!immune[0].burning.on_fire());
  CHECK_EQ(immune[0].burning.remaining_ticks, 0);
  CHECK_NEAR(immune[0].health, immune[0].type->max_health, 1e-6);
}

void test_spectral_glow_on_a_mob() {
  // `SpectralArrow.doPostHurtEffects` -> `mob.addEffect(new MobEffectInstance(
  // MobEffects.GLOWING, this.duration, 0))`, `DEFAULT_DURATION = 200`. The
  // effect is real state on the mob (`ActiveEffects`, the container the players
  // already use) and the mob's own tick is what ages it and what reports the one
  // bit a client can see for a mob — `LivingEntity.updateGlowingStatus`'s
  // shared-flag byte (flag 6), which is *only* sent on a change.
  MobEnvironment env = flat_world();
  std::vector<Mob> mobs{make_mob("zombie", 0.5, 1.0, 0.5)};
  const int32_t glowing = glowing_effect_id();
  CHECK(glowing >= 0);
  CHECK_EQ(glowing_effect_id(), glowing); // resolved once, stable after
  CHECK(!mobs[0].glowing);
  CHECK(mobs[0].effects.empty());

  MobEffectInstance arrow_effect;
  arrow_effect.effect = glowing;
  arrow_effect.duration = 200; // kSpectralArrowGlowTicks (projectile_test pins it)
  CHECK(mob_add_effect(mobs[0], arrow_effect));
  CHECK(mobs[0].glowing);
  CHECK(mobs[0].effects.has(glowing));
  CHECK_EQ(mobs[0].effects.all().size(), 1);
  // A second, weaker add changes nothing and does not move the bit.
  CHECK(!mob_add_effect(mobs[0], arrow_effect));
  CHECK(mobs[0].glowing);

  // The tick does *not* report the change the hit made: the bit moved outside
  // the tick, and the code that moved it (the impact sink, which is also where
  // the effect was added) is what sends the byte — the same way it sends the
  // on-fire byte it flips. What the tick reports is a move that happens *within*
  // it, which is the expiry below.
  //
  // The effect ages one tick per tick (`MobEffectInstance.tickServer`), so the
  // 199 ticks up to the expiry hold the bit — and a flag that has not changed is
  // a byte that must not be resent, once per tick.
  for (int i = 0; i < 199; ++i) {
    CHECK(tick_mobs(mobs, env).flags_changed.empty());
    CHECK(!mobs.empty());
    CHECK(mobs[0].glowing);
  }
  const MobEffectInstance* held = mobs[0].effects.get(glowing);
  CHECK(held != nullptr);
  CHECK_EQ(held == nullptr ? -1 : held->duration, 1); // 199 of the 200 spent

  // The two-hundredth tick is the expiry: the instance is dropped, the bit
  // falls with it and that fall is reported exactly once.
  int glow_reports = 0;
  {
    const MobTickReport report = tick_mobs(mobs, env);
    for (int32_t id : report.flags_changed) {
      CHECK(id == mobs[0].entity_id);
      ++glow_reports;
    }
  }
  CHECK_EQ(glow_reports, 1);
  CHECK(!mobs.empty());
  CHECK(!mobs[0].glowing);
  CHECK(mobs[0].effects.empty());
}

void test_attack_reach() {
  Mob mob = make_mob("zombie", 0.5, 1.0, 0.5);
  // Eye position at the mob's own feet: always in reach.
  CHECK(mob_within_player_reach(mob, 0.5, 1.62, 0.5, 6.0));
  // Five blocks away is inside 6.0; seven is not.
  CHECK(mob_within_player_reach(mob, 5.5, 1.62, 0.5, 6.0));
  CHECK(!mob_within_player_reach(mob, 8.5, 1.62, 0.5, 6.0));
}

void test_mob_loot() {
  // A roll script: each draw comes from `values` in order (cycling), so a test
  // can control the chance roll and the count roll separately.
  auto scripted = [](std::vector<double> values) {
    auto index = std::make_shared<size_t>(0);
    return [values, index]() {
      const double v = values.empty() ? 0.5 : values[(*index)++ % values.size()];
      return v;
    };
  };

  // Every roll is 0.5: the count ranges land in the middle and the 2.5% rare
  // pool does not fire.
  const std::function<double()> mid = [] { return 0.5; };
  const std::vector<BlockDrop> zombie = resolve_mob_drops("minecraft:zombie", true, mid);
  CHECK(zombie.size() == 1);
  CHECK(zombie[0].item == "minecraft:rotten_flesh");
  CHECK(zombie[0].count == 1); // uniform 0..2 at 0.5 -> 1

  // 0.5 for the flesh count, then 0.0 for the rare pool's chance roll and its
  // candidate pick: one of the three items, the first (iron ingot).
  const std::function<double()> lucky_roll = scripted({0.5, 0.0, 0.0});
  const std::vector<BlockDrop> lucky = resolve_mob_drops("minecraft:zombie", true, lucky_roll);
  CHECK(lucky.size() == 2);
  CHECK(lucky[1].item == "minecraft:iron_ingot");
  CHECK(lucky[1].count == 1);
  // The rare pool is gated on a player kill: a mob that fell to its death drops
  // only the always-on pool.
  const std::function<double()> no_player = scripted({0.5, 0.0, 0.0});
  const std::vector<BlockDrop> self_inflicted =
      resolve_mob_drops("minecraft:zombie", false, no_player);
  CHECK(self_inflicted.size() == 1 && self_inflicted[0].item == "minecraft:rotten_flesh");

  // The spider's eye pool rolls a uniform -1..1, so it is *sometimes* nothing.
  const std::function<double()> zero = [] { return 0.0; };
  bool has_eye = false;
  for (const BlockDrop& d : resolve_mob_drops("minecraft:spider", true, zero))
    if (d.item == "minecraft:spider_eye")
      has_eye = true;
  CHECK(!has_eye); // -1: no eye, and the string count is 0 too
  bool eye_at_high = false;
  for (const BlockDrop& d : resolve_mob_drops("minecraft:spider", true, high_roll()))
    if (d.item == "minecraft:spider_eye" && d.count == 1)
      eye_at_high = true;
  CHECK(eye_at_high);

  // The animals: mutton and white wool, porkchop, leather and beef, feather and
  // chicken.
  bool mutton = false, wool = false;
  for (const BlockDrop& d : resolve_mob_drops("minecraft:sheep", true, mid)) {
    mutton = mutton || d.item == "minecraft:mutton";
    wool = wool || d.item == "minecraft:white_wool";
  }
  CHECK(mutton && wool);
  const std::function<double()> high = [] { return 0.9; };
  bool porkchop = false;
  for (const BlockDrop& d : resolve_mob_drops("minecraft:pig", true, high))
    porkchop = porkchop || (d.item == "minecraft:porkchop" && d.count == 3);
  CHECK(porkchop); // uniform 1..3 at 0.9 -> 3
  for (const char* mob : {"minecraft:cow", "minecraft:chicken"})
    CHECK(!resolve_mob_drops(mob, true, mid).empty());
  // The expansion's poster child: a creeper's table is gunpowder (0..2), and a
  // mob with a genuinely empty table — an allay drops nothing in vanilla —
  // still resolves to no items.
  const std::vector<BlockDrop> creeper = resolve_mob_drops("minecraft:creeper", true, mid);
  CHECK(creeper.size() == 1 && creeper[0].item == "minecraft:gunpowder" && creeper[0].count == 1);
  CHECK(resolve_mob_drops("minecraft:allay", true, mid).empty());
}

// ---- animal breeding (Phase 4 milestone I) ---------------------------------

void test_animal_food_table() {
  const GeneratedMob* cow = mob_by_name("cow");
  const GeneratedMob* pig = mob_by_name("pig");
  const GeneratedMob* sheep = mob_by_name("sheep");
  const GeneratedMob* chicken = mob_by_name("chicken");
  CHECK(cow && pig && sheep && chicken);

  // The generated rows, out of the item tags: `AbstractCow`/`Sheep` are
  // #cow_food/#sheep_food (wheat), `Pig` is #pig_food, `Chicken` is
  // #chicken_food — and every other `kMobAnimal` row has its own class's tag.
  // 137 (mob, item) memberships over 32 animals in all (a parrot and a polar
  // bear declare no food tag at all: `Animal.isFood`'s own default is false).
  CHECK_EQ(kAnimalFoodCount, 137);
  const Mob cow_mob = make_mob("cow", 0.5, 1.0, 0.5);
  const Mob pig_mob = make_mob("pig", 0.5, 1.0, 0.5);
  const Mob sheep_mob = make_mob("sheep", 0.5, 1.0, 0.5);
  const Mob chicken_mob = make_mob("chicken", 0.5, 1.0, 0.5);
  CHECK(mob_eats(cow_mob, "minecraft:wheat"));
  CHECK(!mob_eats(cow_mob, "minecraft:carrot"));
  CHECK(mob_eats(sheep_mob, "minecraft:wheat"));
  CHECK(!mob_eats(sheep_mob, "minecraft:wheat_seeds"));
  CHECK(mob_eats(pig_mob, "minecraft:carrot"));
  CHECK(mob_eats(pig_mob, "minecraft:potato"));
  CHECK(mob_eats(pig_mob, "minecraft:beetroot"));
  CHECK(!mob_eats(pig_mob, "minecraft:wheat"));
  CHECK(mob_eats(chicken_mob, "minecraft:wheat_seeds"));
  CHECK(mob_eats(chicken_mob, "minecraft:melon_seeds"));
  CHECK(mob_eats(chicken_mob, "minecraft:torchflower_seeds"));
  CHECK(mob_eats(chicken_mob, "minecraft:pitcher_pod"));
  CHECK(!mob_eats(chicken_mob, "minecraft:wheat"));
  // The expansion, one probe per family: a wolf by #wolf_food (raw and cooked
  // meat and fish), a horse by #horse_food, a panda by bamboo, a turtle by
  // seagrass, and the two 26.2 oddities the tags really carry — a camel husk by
  // a rabbit's foot and an armadillo by a spider eye.
  const Mob wolf_mob = make_mob("wolf", 0.5, 1.0, 0.5);
  CHECK(mob_eats(wolf_mob, "minecraft:beef"));
  CHECK(mob_eats(wolf_mob, "minecraft:cooked_beef"));
  CHECK(!mob_eats(wolf_mob, "minecraft:wheat"));
  const Mob horse_mob = make_mob("horse", 0.5, 1.0, 0.5);
  CHECK(mob_eats(horse_mob, "minecraft:golden_apple"));
  CHECK(mob_eats(horse_mob, "minecraft:hay_block"));
  const Mob panda_mob = make_mob("panda", 0.5, 1.0, 0.5);
  CHECK(mob_eats(panda_mob, "minecraft:bamboo"));
  const Mob turtle_mob = make_mob("turtle", 0.5, 1.0, 0.5);
  CHECK(mob_eats(turtle_mob, "minecraft:seagrass"));
  const Mob camel_husk_mob = make_mob("camel_husk", 0.5, 1.0, 0.5);
  CHECK(mob_eats(camel_husk_mob, "minecraft:rabbit_foot"));
  const Mob armadillo_mob = make_mob("armadillo", 0.5, 1.0, 0.5);
  CHECK(mob_eats(armadillo_mob, "minecraft:spider_eye"));
  // A parrot's `isFood` is false — its seeds are a *taming* item, and feeding one
  // is not the breeding path this table is for.
  const Mob parrot_mob = make_mob("parrot", 0.5, 1.0, 0.5);
  CHECK(!mob_eats(parrot_mob, "minecraft:wheat_seeds"));
  // Not an `Animal`, so no food at all — `isFood` is the class's own override.
  const GeneratedMob* zombie = mob_by_name("zombie");
  CHECK(zombie && !mob_eats(make_mob("zombie", 0.5, 1.0, 0.5), "minecraft:wheat"));
  // The table is sorted by (mob, item), which is what `mob_eats`' one-pass walk
  // and a human reader both rely on.
  for (int i = 1; i < kAnimalFoodCount; ++i) {
    const std::string a = std::string(kAnimalFoods[i - 1].mob) + "\x01" + kAnimalFoods[i - 1].item;
    const std::string b = std::string(kAnimalFoods[i].mob) + "\x01" + kAnimalFoods[i].item;
    CHECK(a < b);
  }

  // The baby boxes are the four animals' own, and the zombie has none.
  CHECK(!(zombie->flags & kMobAnimal));
  CHECK((cow->flags & kMobAnimal) && (pig->flags & kMobAnimal) && (sheep->flags & kMobAnimal) &&
        (chicken->flags & kMobAnimal));
  CHECK_NEAR(cow->baby_width, 0.45, 1e-6);
  CHECK_NEAR(cow->baby_height, 0.7, 1e-6);
  CHECK_NEAR(pig->baby_width, 0.45, 1e-6);
  CHECK_NEAR(pig->baby_height, 0.45, 1e-6);
  CHECK_NEAR(sheep->baby_width, 0.45, 1e-6);
  CHECK_NEAR(sheep->baby_height, 0.65, 1e-6);
  CHECK_NEAR(chicken->baby_width, 0.3, 1e-6);
  CHECK_NEAR(chicken->baby_height, 0.4, 1e-6);
  CHECK_NEAR(zombie->baby_width, 0.0, 1e-6);

  // `getDefaultDimensions`: the baby box is what the physics and the reach
  // gates read while the age is negative, and the adult box comes back at 0.
  Mob baby = make_mob("cow", 0.5, 1.0, 0.5);
  baby.breed_age = kAnimalBabyStartAge;
  CHECK_NEAR(baby.half_width(), 0.225, 1e-6);
  CHECK_NEAR(baby.body_height(), 0.7, 1e-6);
  baby.breed_age = 0;
  CHECK_NEAR(baby.half_width(), 0.45, 1e-6);
  CHECK_NEAR(baby.body_height(), 1.4, 1e-6);
}

void test_feeding() {
  // `Animal.getSpeedUpSecondsWhenFeeding`: the floored tenth, integer division
  // first — 24000 ticks is 120 seconds (2400 ticks), and under twenty ticks to
  // go the feeding is worth nothing at all.
  CHECK_EQ(animal_speed_up_seconds(24000), 120);
  CHECK_EQ(animal_speed_up_seconds(2400), 12);
  CHECK_EQ(animal_speed_up_seconds(19), 0);
  CHECK_EQ(animal_speed_up_seconds(0), 0);

  // An adult at age 0 that can fall in love: 600 ticks of love and one item
  // spent. A second feeding is refused (already in love — `canFallInLove` is
  // `inLove <= 0`), and nothing is spent.
  Mob cow = make_mob("cow", 0.5, 1.0, 0.5);
  MobFeedResult fed = feed_mob(cow, /*is_food=*/true, "Alice");
  CHECK(fed.consumed && fed.fell_in_love && !fed.aged_up);
  CHECK_EQ(cow.in_love, kAnimalInLoveTicks);
  // `setInLove(player)` records the cause beside the countdown; it is what the
  // eventual birth awards `animals_bred` and `bred_animals` to.
  CHECK(cow.love_cause == "Alice");
  MobFeedResult again = feed_mob(cow, /*is_food=*/true);
  CHECK(!again.consumed && !again.fell_in_love && !again.aged_up);
  CHECK_EQ(cow.in_love, kAnimalInLoveTicks);
  // A non-food is never consumed, whatever the age — and a feeding with no
  // player behind it (the engine's own tests) records no cause.
  Mob carrotless = make_mob("cow", 0.5, 1.0, 0.5);
  CHECK(!feed_mob(carrotless, /*is_food=*/false).consumed);
  CHECK(feed_mob(carrotless, /*is_food=*/true).fell_in_love);
  CHECK(carrotless.love_cause.empty());

  // The love runs out after 600 ticks and the mob can fall in love again; a
  // parent on its post-breeding age cannot, because the age is not 0.
  cow.in_love = 0;
  CHECK(cow.can_fall_in_love());
  CHECK(feed_mob(cow, true).fell_in_love);
  cow.breed_age = kAnimalParentAgeAfterBreeding;
  cow.in_love = 0;
  CHECK(!feed_mob(cow, true).consumed);

  // A baby is fed to grow: a tenth of its remaining ticks, the amount banked in
  // `forcedAge`, and it was never in love.
  Mob baby = make_mob("cow", 0.5, 1.0, 0.5);
  baby.breed_age = kAnimalBabyStartAge;
  MobFeedResult grew = feed_mob(baby, /*is_food=*/true);
  CHECK(grew.consumed && grew.aged_up && !grew.fell_in_love);
  CHECK_EQ(baby.breed_age, -21600);
  CHECK_EQ(baby.forced_age, 2400);
  CHECK_EQ(baby.in_love, 0);
  CHECK(baby.baby() && baby.can_age_up());

  // `ageUp`'s clamp, and the jump at its end: an age that lands exactly on 0 is
  // replaced by the banked `forcedAge` (`AgeableMob.ageUp`'s own last line).
  Mob clamped = make_mob("cow", 0.5, 1.0, 0.5);
  clamped.breed_age = -100;
  mob_age_up(clamped, 10, /*forced=*/false);
  CHECK_EQ(clamped.breed_age, 0);
  CHECK_EQ(clamped.forced_age, 0);
  CHECK(!clamped.baby());

  // The feeding path cannot reach that line: every feeding is
  // `floor(remaining / 200)` seconds — the integer division by 20 happens
  // *before* the tenth-of-a-second floor — which for `remaining >= 200` is
  // strictly less than the remainder and for `remaining < 200` is zero. Feeding
  // a baby to adulthood therefore never happens — the age stalls below two
  // hundred ticks (ten seconds) and the *natural* growth finishes it. This is
  // why `forced_age` never fires in play, and it is pinned here rather than
  // asserted in a comment.
  Mob stalled = make_mob("cow", 0.5, 1.0, 0.5);
  stalled.breed_age = kAnimalBabyStartAge;
  int feeds = 0;
  while (true) {
    const int before = stalled.breed_age;
    CHECK(feed_mob(stalled, true).consumed);
    ++feeds;
    CHECK(stalled.breed_age != 0); // a feeding never lands on adulthood
    // `forced_age` accumulates exactly the progress the feedings bought.
    CHECK_EQ(stalled.forced_age, stalled.breed_age - kAnimalBabyStartAge);
    if (stalled.breed_age == before)
      break;
  }
  CHECK(stalled.baby());
  CHECK(feeds > 1 && feeds < 200); // food bites, then stops biting
  CHECK(stalled.breed_age < 0 && stalled.breed_age > -200);
  // ...and the tick's own `++age` is what grows it up from there.
  std::vector<Mob> finishing{stalled};
  MobEnvironment env = flat_world();
  int ticks = 0;
  while (finishing[0].baby() && ticks < 1000) {
    tick_mobs(finishing, env);
    ++ticks;
  }
  CHECK(!finishing[0].baby());
  CHECK_EQ(ticks, -stalled.breed_age);
}

void test_growth_and_love_in_the_tick() {
  Mob baby = make_mob("cow", 0.5, 1.0, 0.5, /*id=*/7);
  baby.breed_age = -3;
  MobEnvironment env = flat_world();
  std::vector<Mob> mobs{baby};
  // `AgeableMob.setAge` flips the baby bit exactly on the crossing, so the
  // report carries `grew_up` once — on the tick the age reaches 0.
  for (int i = 0; i < 2; ++i) {
    const MobTickReport report = tick_mobs(mobs, env);
    CHECK(report.grew_up.empty());
    CHECK(mobs[0].breed_age == -2 + i);
  }
  const MobTickReport crossed = tick_mobs(mobs, env);
  CHECK_EQ(crossed.grew_up.size(), 1u);
  CHECK_EQ(crossed.grew_up[0], 7);
  CHECK_EQ(mobs[0].breed_age, 0);
  CHECK(!mobs[0].baby());
  CHECK(tick_mobs(mobs, env).grew_up.empty());

  // An adult's post-breeding age counts down one tick at a time and never
  // reports a crossing (it was never a baby in this run).
  Mob parent = make_mob("cow", 0.5, 1.0, 0.5, /*id=*/8);
  parent.breed_age = 3;
  parent.in_love = kAnimalInLoveTicks;
  std::vector<Mob> parents{parent};
  for (int i = 0; i < 3; ++i)
    CHECK(tick_mobs(parents, env).grew_up.empty());
  CHECK_EQ(parents[0].breed_age, 0);
  // `Animal.aiStep` cleared the love on the first tick (the age was not 0).
  CHECK_EQ(parents[0].in_love, 0);

  // Love itself is a 600-tick countdown while the age is 0.
  Mob lover = make_mob("cow", 0.5, 1.0, 0.5, /*id=*/9);
  lover.in_love = 3;
  std::vector<Mob> lovers{lover};
  for (int i = 0; i < 3; ++i)
    tick_mobs(lovers, env);
  CHECK_EQ(lovers[0].in_love, 0);
  CHECK(!lovers[0].is_in_love());
}

// One run of the pair: how many births the tick reports and where the parents
// end up. The pair walks toward each other (the goal's own navigation), so the
// test only supplies the two animals and the flat world.
struct BreedingRun {
  std::vector<MobTickReport> reports;
  std::vector<Mob> mobs;
};

BreedingRun run_pair(std::vector<Mob> mobs, int ticks) {
  BreedingRun run;
  run.mobs = std::move(mobs);
  MobEnvironment env = flat_world();
  for (int i = 0; i < ticks; ++i)
    run.reports.push_back(tick_mobs(run.mobs, env));
  return run;
}

void test_breeding() {
  // Two in-love cows two blocks apart: `BreedGoal` runs its thirty goal ticks
  // (sixty real ticks on the selector's parity), finds the pair inside three
  // blocks, and `spawnChildFromBreeding` puts a baby at the initiator's own
  // position. Both parents take the 6000-tick age and lose their love.
  Mob a = make_mob("cow", 0.5, 1.0, 0.5, /*id=*/1);
  Mob b = make_mob("cow", 2.5, 1.0, 0.5, /*id=*/2);
  a.in_love = kAnimalInLoveTicks;
  b.in_love = kAnimalInLoveTicks;
  // Only the initiator was fed by hand: the birth carries that one cause
  // (`getLoveCause().or(partner::getLoveCause)`).
  a.love_cause = "Alice";
  // Tick only until the birth, so the parents can be inspected on the breeding
  // tick itself — `finalizeSpawnChildFromBreeding`'s `setAge(6000)`.
  BreedingRun run;
  run.mobs = {a, b};
  MobEnvironment env = flat_world();
  int breed_tick = -1;
  // The birth carries `snapTo(this)` — the initiator's own position at the
  // breed moment — so the tick's *start* positions are what it is checked
  // against. (The goal step runs before either parent's own `move_mob`, so a
  // comparison after the tick would be off by a step.) A box around the pair's
  // old midpoint would pass a child born at the wrong parent's feet; this does
  // not.
  Mob initiator_before;
  for (int i = 0; i < 200 && breed_tick < 0; ++i) {
    const Mob first_before = run.mobs[0];
    const Mob second_before = run.mobs[1];
    run.reports.push_back(tick_mobs(run.mobs, env));
    if (!run.reports.back().births.empty()) {
      breed_tick = i;
      const MobBirth& born = run.reports.back().births[0];
      initiator_before =
          born.parent_entity_id == first_before.entity_id ? first_before : second_before;
    }
  }
  CHECK(breed_tick >= 0);
  CHECK_EQ(run.reports[breed_tick].births.size(), 1u);
  const MobBirth birth = run.reports[breed_tick].births[0];
  CHECK(birth.type == mob_by_name("cow"));
  CHECK(birth.love_cause == "Alice");
  CHECK(birth.parent_entity_id == 1 || birth.parent_entity_id == 2);
  CHECK(birth.partner_entity_id != birth.parent_entity_id);
  // `snapTo(this)`: the child stands exactly where the initiator stood when it
  // bred — feet, box and all — not merely somewhere near the pair.
  CHECK_EQ(birth.parent_entity_id, initiator_before.entity_id);
  CHECK_NEAR(birth.x, initiator_before.x, 1e-12);
  CHECK_NEAR(birth.y, initiator_before.y, 1e-12);
  CHECK_NEAR(birth.z, initiator_before.z, 1e-12);
  for (const Mob& mob : run.mobs) {
    CHECK_EQ(mob.breed_age, kAnimalParentAgeAfterBreeding);
    CHECK_EQ(mob.in_love, 0);
    // `resetLove()` clears the cause with the countdown.
    CHECK(mob.love_cause.empty());
    CHECK_EQ(mob.mate_entity_id, -1);
    CHECK(!mob.baby());
  }
  // Their love is gone and the 6000-tick cooldown is what blocks a second
  // birth; the child is the *caller's* to create, so the population is still
  // two, and the rest of the run only counts the cooldown down.
  for (int i = breed_tick + 1; i < 200; ++i) {
    run.reports.push_back(tick_mobs(run.mobs, env));
    CHECK(run.reports.back().births.empty());
  }
  CHECK_EQ(run.mobs.size(), 2u);
  CHECK_EQ(run.mobs[0].breed_age, kAnimalParentAgeAfterBreeding - (199 - breed_tick));
  CHECK_EQ(run.mobs[1].breed_age, kAnimalParentAgeAfterBreeding - (199 - breed_tick));

  // Love expires: the same pair twenty blocks apart never mates, and both
  // clocks run out. (`BreedGoal.canContinueToUse`'s own give-up needs a partner
  // that stops being in love; with none acquired there is nothing to stop.)
  Mob far_a = make_mob("cow", 0.5, 1.0, 0.5, /*id=*/3);
  Mob far_b = make_mob("cow", 20.5, 1.0, 0.5, /*id=*/4);
  far_a.in_love = kAnimalInLoveTicks;
  far_b.in_love = kAnimalInLoveTicks;
  BreedingRun far = run_pair({far_a, far_b}, 700);
  for (const MobTickReport& report : far.reports)
    CHECK(report.births.empty());
  for (const Mob& mob : far.mobs) {
    CHECK_EQ(mob.in_love, 0);
    CHECK_EQ(mob.mate_entity_id, -1);
    CHECK_EQ(mob.breed_age, 0);
  }

  // `canMate` is same-class only: a cow and a pig in love beside each other
  // never breed, and neither does a lone cow.
  Mob cow = make_mob("cow", 0.5, 1.0, 0.5, /*id=*/5);
  Mob pig = make_mob("pig", 2.5, 1.0, 0.5, /*id=*/6);
  cow.in_love = kAnimalInLoveTicks;
  pig.in_love = kAnimalInLoveTicks;
  for (const MobTickReport& report : run_pair({cow, pig}, 200).reports)
    CHECK(report.births.empty());
  Mob lone = make_mob("cow", 0.5, 1.0, 0.5, /*id=*/10);
  lone.in_love = kAnimalInLoveTicks;
  for (const MobTickReport& report : run_pair({lone}, 200).reports)
    CHECK(report.births.empty());
  CHECK_EQ(run_pair({lone}, 200).mobs[0].mate_entity_id, -1);

  // One in love and one not: `canMate` wants both, at acquisition and at
  // continuation.
  Mob lover = make_mob("cow", 0.5, 1.0, 0.5, /*id=*/11);
  Mob cold = make_mob("cow", 2.5, 1.0, 0.5, /*id=*/12);
  lover.in_love = kAnimalInLoveTicks;
  for (const MobTickReport& report : run_pair({lover, cold}, 200).reports)
    CHECK(report.births.empty());

  // A partner that loses its love mid-approach stops the goal: the one in love
  // drops its partner and never breeds. (Ten blocks apart so the walk cannot
  // reach the three-block breed distance before the short love runs out.)
  Mob seeker = make_mob("cow", 0.5, 1.0, 0.5, /*id=*/13);
  Mob quitter = make_mob("cow", 10.5, 1.0, 0.5, /*id=*/14);
  seeker.in_love = kAnimalInLoveTicks;
  quitter.in_love = 5;
  for (const MobTickReport& report : run_pair({seeker, quitter}, 100).reports)
    CHECK(report.births.empty());

  // A mob that is not an `Animal` cannot breed however it is marked up.
  Mob zombie_a = make_mob("zombie", 0.5, 1.0, 0.5, /*id=*/15);
  Mob zombie_b = make_mob("zombie", 2.5, 1.0, 0.5, /*id=*/16);
  zombie_a.in_love = kAnimalInLoveTicks;
  zombie_b.in_love = kAnimalInLoveTicks;
  for (const MobTickReport& report : run_pair({zombie_a, zombie_b}, 200).reports)
    CHECK(report.births.empty());
}

// `FollowParentGoal` (Phase 4 milestone I's follow-up): the same-class adult a
// baby walks to, the three-block refusal that starts and ends the goal, the
// box the candidate query lives in, the grow-up/death/range rules that let the
// parent go, the five-goal-tick re-plan countdown, and the goal's own speed
// modifier.
void test_follow_parent() {
  // The registration's speedModifier is per animal, not one shared default:
  // `new FollowParentGoal(this, 1.1)` for the pig, sheep and chicken against
  // the cow's 1.25 (1.1 is the stroll's own number, so only the cow would be
  // distinguishable by watching a walk).
  CHECK_NEAR(mob_by_name("cow")->follow_parent_speed, 1.25, 1e-6);
  CHECK_NEAR(mob_by_name("pig")->follow_parent_speed, 1.1, 1e-6);
  CHECK_NEAR(mob_by_name("sheep")->follow_parent_speed, 1.1, 1e-6);
  CHECK_NEAR(mob_by_name("chicken")->follow_parent_speed, 1.1, 1e-6);
  // A mob that has no follow goal carries no modifier at all.
  CHECK_NEAR(mob_by_name("zombie")->follow_parent_speed, 0.0, 1e-6);

  MobEnvironment env = flat_world();
  const auto gap_sqr = [](const Mob& a, const Mob& b) {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    const double dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
  };
  // A baby cow with an adult five blocks east. The adult holds its stroll on a
  // long cooldown so only the baby moves.
  const auto make_baby = [](double x, int32_t id) {
    Mob mob = make_mob("cow", x, 1.0, 0.5, id);
    mob.breed_age = kAnimalBabyStartAge;
    return mob;
  };
  const auto make_parent = [](double x, int32_t id) {
    Mob mob = make_mob("cow", x, 1.0, 0.5, id);
    mob.stray_cooldown = 1000;
    return mob;
  };

  // --- acquisition and the three-block stop --------------------------------
  // Entity id 0 makes the first tick (age 1) a full selector tick, so the goal
  // acquires its parent there and plans its first path; `start()` zeroes the
  // countdown, so that same tick re-plans and the countdown reads five.
  {
    std::vector<Mob> mobs{make_baby(0.5, 0), make_parent(5.5, 1)};
    const MobTickReport first = tick_mobs(mobs, env);
    CHECK_EQ(mobs[0].parent_entity_id, 1);
    CHECK(mobs[0].goal == MobGoal::FollowParent);
    CHECK_EQ(mobs[0].follow_repath, kAnimalFollowRepathTicks);
    CHECK(mobs[0].has_path_goal);
    CHECK_NEAR(mobs[0].path_to_x, 5.5, 1e-9);
    CHECK(first.moved.size() == 1u && first.moved[0] == 0);
    // Walking east, to the parent: the first tick only turns the mob (it was
    // not `on_ground` for the acceleration yet), the second carries it.
    tick_mobs(mobs, env);
    CHECK(mobs[0].x > 0.5);
    // An adult never follows: it has no parent, whatever a baby does.
    CHECK_EQ(mobs[1].parent_entity_id, -1);
    CHECK(mobs[1].goal != MobGoal::FollowParent);

    // The baby closes the five blocks, the goal drops the parent at the same
    // three-block line that refused `canUse`, and the stroll that takes over is
    // pulled back once it carries the baby out past three again — the goal runs
    // while the gap is in [3, 16]. Every stroll here runs west (the pinned
    // `unit_random`), so the cycle is the two-block leg, a hundred-tick rest and
    // another leg out before the goal engages again.
    bool stopped_inside_three = false;
    bool reacquired = false;
    for (int i = 0; i < 240; ++i) {
      tick_mobs(mobs, env);
      if (gap_sqr(mobs[0], mobs[1]) < kAnimalFollowMinDistanceSqr && mobs[0].parent_entity_id == -1)
        stopped_inside_three = true;
      if (stopped_inside_three && mobs[0].goal == MobGoal::FollowParent)
        reacquired = true;
    }
    CHECK(stopped_inside_three);
    CHECK(reacquired);
    CHECK(gap_sqr(mobs[0], mobs[1]) < 5.0 * 5.0);
  }

  // --- the nearest candidate is what the three-block line measures ---------
  // Two adults, the near one inside three blocks and the far one six out: the
  // nearest wins the scan and refuses the goal outright, so the baby does not
  // "skip" to the one it *may* follow. The refusal hands the tick to the
  // stroll, which is what carries the baby away until a parent is out of the
  // three blocks again.
  {
    std::vector<Mob> mobs{make_baby(0.5, 0), make_parent(3.0, 1), make_parent(6.5, 2)};
    tick_mobs(mobs, env);
    CHECK_EQ(mobs[0].parent_entity_id, -1);
    CHECK(mobs[0].goal == MobGoal::Stroll);
  }
  // The same pair with the near adult moved out to four blocks: the near one is
  // the parent, not the far one.
  {
    std::vector<Mob> mobs{make_baby(0.5, 0), make_parent(4.5, 1), make_parent(7.5, 2)};
    tick_mobs(mobs, env);
    CHECK_EQ(mobs[0].parent_entity_id, 1);
  }

  // --- what counts as a parent ---------------------------------------------
  // Another animal is not a parent (the query is `animal.getClass()`): a pig.
  {
    Mob pig = make_mob("pig", 4.5, 1.0, 0.5, /*id=*/1);
    pig.stray_cooldown = 1000;
    std::vector<Mob> mobs{make_baby(0.5, 0), pig};
    tick_mobs(mobs, env);
    CHECK_EQ(mobs[0].parent_entity_id, -1);
  }
  // Nor is a baby: `parent.getAge() < 0` skips every candidate that is not an
  // adult — which is also what skips the baby's own box in the query.
  {
    std::vector<Mob> mobs{make_baby(0.5, 0), make_baby(4.5, 1)};
    tick_mobs(mobs, env);
    CHECK_EQ(mobs[0].parent_entity_id, -1);
  }
  // ... and neither is one that is already gone: a removed adult is not in the
  // level's entity list, and neither is a dead one.
  {
    Mob adult = make_parent(4.5, 1);
    adult.removed = true;
    std::vector<Mob> mobs{make_baby(0.5, 0), adult};
    tick_mobs(mobs, env);
    CHECK_EQ(mobs[0].parent_entity_id, -1);
  }

  // --- the box the query lives in ------------------------------------------
  // Horizontal: `getBoundingBox().inflate(8.0, 4.0, 8.0)` against the adult's
  // own box is a reach of 8 + both half widths — 8.675 for two cows — and
  // `AABB.intersects` is strict, so 8.5 blocks of gap is in and 8.8 is out.
  {
    std::vector<Mob> mobs{make_baby(0.5, 0), make_parent(9.0, 1)};
    tick_mobs(mobs, env);
    CHECK_EQ(mobs[0].parent_entity_id, 1);
  }
  {
    std::vector<Mob> mobs{make_baby(0.5, 0), make_parent(9.3, 1)};
    tick_mobs(mobs, env);
    CHECK_EQ(mobs[0].parent_entity_id, -1);
  }
  // Vertical: the top of the query is the *baby's* own height (0.7) plus four,
  // so feet 4.0 above the baby's are in and 4.8 are out.
  {
    Mob adult = make_parent(4.5, 1);
    adult.y = 5.0;
    std::vector<Mob> mobs{make_baby(0.5, 0), adult};
    tick_mobs(mobs, env);
    CHECK_EQ(mobs[0].parent_entity_id, 1);
  }
  {
    Mob adult = make_parent(4.5, 1);
    adult.y = 5.8;
    std::vector<Mob> mobs{make_baby(0.5, 0), adult};
    tick_mobs(mobs, env);
    CHECK_EQ(mobs[0].parent_entity_id, -1);
  }

  // --- the rules that let a held parent go ---------------------------------
  // Each run acquires its parent on the first tick and then breaks one rule.
  {
    // Back inside three blocks (`distSqr < 9.0`) — the same line that refuses
    // `canUse`: the parent is dropped and the stroll takes the tick.
    std::vector<Mob> mobs{make_baby(0.5, 0), make_parent(5.5, 1)};
    tick_mobs(mobs, env);
    CHECK_EQ(mobs[0].parent_entity_id, 1);
    mobs[1].x = 2.0; // a gap of 1.5
    tick_mobs(mobs, env);
    CHECK_EQ(mobs[0].parent_entity_id, -1);
    CHECK(mobs[0].goal == MobGoal::Stroll);
  }
  {
    // Past sixteen blocks (`distSqr > 256.0`), which is also past the query's
    // box, so the goal cannot come back on the same tick.
    std::vector<Mob> mobs{make_baby(0.5, 0), make_parent(5.5, 1)};
    tick_mobs(mobs, env);
    CHECK_EQ(mobs[0].parent_entity_id, 1);
    mobs[1].x = 20.5; // a gap of 20
    tick_mobs(mobs, env);
    CHECK_EQ(mobs[0].parent_entity_id, -1);
  }
  {
    // A dead parent (`!parent.isAlive()`): the death is recorded on that tick
    // and the baby's goal lets the parent go on the same one.
    std::vector<Mob> mobs{make_baby(0.5, 0), make_parent(5.5, 1)};
    tick_mobs(mobs, env);
    CHECK_EQ(mobs[0].parent_entity_id, 1);
    mobs[1].health = 0.0f;
    const MobTickReport dead = tick_mobs(mobs, env);
    CHECK_EQ(dead.deaths.size(), 1u);
    CHECK_EQ(mobs.size(), 1u); // the death erased it from the population
    CHECK_EQ(mobs[0].parent_entity_id, -1);
  }
  {
    // The grow-up crossing: both `canUse` and `canContinueToUse` want an age
    // below zero, so the tick the age reaches 0 is the tick the goal stops.
    std::vector<Mob> mobs{make_baby(0.5, 0), make_parent(5.5, 1)};
    tick_mobs(mobs, env);
    CHECK_EQ(mobs[0].parent_entity_id, 1);
    mobs[0].breed_age = -1; // the next `++age` crosses 0
    const MobTickReport grown = tick_mobs(mobs, env);
    CHECK_EQ(grown.grew_up.size(), 1u);
    CHECK(!mobs[0].baby());
    CHECK_EQ(mobs[0].parent_entity_id, -1);
    CHECK(mobs[0].goal == MobGoal::Stroll);
  }

  // --- the re-plan countdown ------------------------------------------------
  // `tick()` is `if (--timeToRecalcPath > 0) return; timeToRecalcPath =
  // adjustedTickDelay(10); navigation.moveTo(parent, speed)`. Entity id 0 makes
  // the selector's full ticks land on age 1 and then every even age, and a
  // re-plan shows itself: `path_to_x` is the position the path was built
  // toward, so a parent that moves without one leaves it where it was.
  {
    std::vector<Mob> mobs{make_baby(0.5, 0), make_parent(7.5, 1)};
    tick_mobs(mobs, env); // age 1: the acquisition and the run's first `moveTo`
    CHECK_EQ(mobs[0].follow_repath, kAnimalFollowRepathTicks);
    CHECK_NEAR(mobs[0].path_to_x, 7.5, 1e-9);
    mobs[1].x = 9.5; // two blocks further out: inside the sixteen, past the box
    for (int age = 2; age <= 9; ++age) {
      tick_mobs(mobs, env);
      CHECK_NEAR(mobs[0].path_to_x, 7.5, 1e-9); // no `moveTo` yet
    }
    CHECK_EQ(mobs[0].follow_repath, 1); // the then-current plan is spent
    tick_mobs(mobs, env);               // age 10: the fifth goal tick re-plans
    CHECK_EQ(mobs[0].follow_repath, kAnimalFollowRepathTicks);
    CHECK_NEAR(mobs[0].path_to_x, 9.5, 1e-9);
  }

  // --- the walk is at the registration's speed ------------------------------
  // `MoveControl` walks at `speedModifier * getSpeed()`, the goal's own
  // registration being where the modifier comes from, and `move_mob`'s
  // acceleration is sized so the distance the mob *covers* per tick settles on
  // that speed: a straight walk along +x reads it back after a settle-in.
  const auto walk_speed = [&env](const char* animal) {
    Mob baby = make_mob(animal, 0.5, 1.0, 0.5, /*id=*/0);
    baby.breed_age = kAnimalBabyStartAge;
    // Eight blocks out: inside the scan's box and well clear of the three the
    // goal stops at, so the whole measured stretch is a follow.
    Mob parent = make_mob(animal, 8.5, 1.0, 0.5, /*id=*/1);
    parent.stray_cooldown = 1000;
    std::vector<Mob> mobs{baby, parent};
    for (int i = 0; i < 20; ++i)
      tick_mobs(mobs, env);
    const double before = mobs[0].x;
    for (int i = 0; i < 10; ++i)
      tick_mobs(mobs, env);
    CHECK_EQ(mobs[0].parent_entity_id, 1);
    return (mobs[0].x - before) / 10.0;
  };
  CHECK_NEAR(walk_speed("cow"), mob_speed_per_tick(*mob_by_name("cow")) * 1.25, 2e-4);
  CHECK_NEAR(walk_speed("pig"), mob_speed_per_tick(*mob_by_name("pig")) * 1.1, 2e-4);
  CHECK_NEAR(walk_speed("sheep"), mob_speed_per_tick(*mob_by_name("sheep")) * 1.1, 2e-4);
  CHECK_NEAR(walk_speed("chicken"), mob_speed_per_tick(*mob_by_name("chicken")) * 1.1, 2e-4);
}

// `FollowParentGoal`'s band, pinned as a *band* and not as a distance the goal
// aims at. `tests/breeding_test.py` section 7 measures physical gaps over a
// 40-second window and bounds the window's tail mean by three blocks; that is
// only the right bound if the goal promises an interval for the child to live
// in rather than a mean for it to approach. Three facts make it an interval, and
// this test reads each one off the tick: inside three the goal is silent, at
// exactly three it engages, and past sixteen it lets go. A window sampled in
// the quiet part of the band is therefore a *resting distance* — the wire test's
// parked 1.47 / 2.07 tails — and must never be read as a flat-mean drift claim.
void test_follow_parent_band() {
  // The band's own numbers: `DONT_FOLLOW_IF_CLOSER_THAN = 3` squared is 9.0, and
  // `canContinueToUse`'s far end is 256.0 — sixteen blocks.
  CHECK_NEAR(kAnimalFollowMinDistanceSqr, 9.0, 1e-12);
  CHECK_NEAR(kAnimalFollowMaxDistanceSqr, 256.0, 1e-12);
  CHECK_NEAR(std::sqrt(kAnimalFollowMinDistanceSqr), 3.0, 1e-12);
  CHECK_NEAR(std::sqrt(kAnimalFollowMaxDistanceSqr), 16.0, 1e-12);

  MobEnvironment env = flat_world();
  const auto gap = [](const Mob& a, const Mob& b) {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    const double dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
  };
  // The baby holds a stroll cooldown longer than any run here and starts with no
  // path, so when the follow goal refuses — which is what it does inside three —
  // the stroll only idles. Whatever the child does in these runs, the follow
  // goal did it.
  const auto baby_at = [](double x) {
    Mob mob = make_mob("cow", x, 1.0, 0.5, /*id=*/0);
    mob.breed_age = kAnimalBabyStartAge;
    mob.stray_cooldown = 100000;
    return mob;
  };
  const auto adult_at = [](double x, int32_t id) {
    Mob mob = make_mob("cow", x, 1.0, 0.5, id);
    mob.stray_cooldown = 100000;
    return mob;
  };

  // --- inside three the goal is silent, and the child stays put -------------
  // `canUse`'s `closestDistSqr < 9.0` refuses the goal outright, so the child is
  // not walked anywhere at all: it stands at whatever distance it already had.
  // Any of these is a legal, permanent state, and a window over it reads that
  // number back exactly — 2.1 is the value a loaded wire run parked at. That is
  // why the window statistic cannot be a drift claim: a mean falling near two
  // blocks is the band's interior, not a trend toward anything.
  const auto rests_inside_band = [&](double parked_gap) {
    std::vector<Mob> mobs{baby_at(0.5), adult_at(0.5 + parked_gap, 1)};
    const double start_x = mobs[0].x;
    double sum = 0.0, worst = 0.0;
    bool ever_followed = false;
    for (int i = 0; i < 200; ++i) {
      tick_mobs(mobs, env);
      ever_followed = ever_followed || mobs[0].goal == MobGoal::FollowParent;
      const double g = gap(mobs[0], mobs[1]);
      sum += g;
      worst = std::max(worst, std::fabs(g - parked_gap));
    }
    CHECK(!ever_followed);
    CHECK_EQ(mobs[0].parent_entity_id, -1);
    CHECK_NEAR(mobs[0].x, start_x, 1e-12);
    CHECK_NEAR(sum / 200.0, parked_gap, 1e-9); // the window mean *is* the park
    CHECK_NEAR(worst, 0.0, 1e-9);
  };
  rests_inside_band(2.99); // the near edge, from inside
  rests_inside_band(2.1);
  rests_inside_band(1.5);

  // --- three itself is *in*, and the goal drops the parent below it --------
  // `closest < 9.0` refuses and `!(d2 < 9.0)` continues, so the boundary is
  // strict at three blocks: a child placed exactly there acquires the parent
  // and walks. The walk is what stops at the same line — the moment a tick
  // starts with the gap under three the held parent is dropped, so every tick
  // the goal actually walked was outside the band and the drop lands just
  // inside it.
  {
    std::vector<Mob> mobs{baby_at(0.5), adult_at(3.5, 1)};
    tick_mobs(mobs, env);
    CHECK_EQ(mobs[0].parent_entity_id, 1);
    CHECK(mobs[0].goal == MobGoal::FollowParent);
    bool dropped = false;
    double gap_at_drop = 0.0;
    for (int i = 0; i < 60 && !dropped; ++i) {
      tick_mobs(mobs, env);
      dropped = mobs[0].parent_entity_id == -1;
      if (dropped)
        gap_at_drop = gap(mobs[0], mobs[1]);
    }
    CHECK(dropped);               // it did cross the line and stop
    CHECK(gap_at_drop < 3.0);     // dropped for being inside the band
    CHECK(gap_at_drop > 2.0);     // by its own step, not a walk-through
    CHECK(mobs[0].x > 0.5);       // and it was the goal that moved it
    CHECK(mobs[0].x < mobs[1].x); // never past the parent
  }

  // --- the far end is strict too -------------------------------------------
  // A held parent at exactly sixteen blocks is still held (`d2 > 256` drops),
  // and a thousandth further it is gone. The far end is why the wire test's
  // `max(gaps) <= 8.0` is a scan bound rather than the goal's own reach: the
  // goal tolerates more than twice that before letting go.
  {
    std::vector<Mob> mobs{baby_at(0.5), adult_at(5.5, 1)};
    tick_mobs(mobs, env);
    CHECK_EQ(mobs[0].parent_entity_id, 1);
    mobs[1].x = mobs[0].x + 16.0; // exactly one band-width out
    tick_mobs(mobs, env);
    CHECK_EQ(mobs[0].parent_entity_id, 1);
    mobs[1].x = mobs[0].x + 16.001;
    tick_mobs(mobs, env);
    CHECK_EQ(mobs[0].parent_entity_id, -1);
  }
}

// What `FollowParentGoal` leaves behind when it stops mid-walk. `stop()` is
// `this.parent = null` and nothing else (decompiled
// `FollowParentGoal.java`), so the navigation is *not* cleared: the path the
// goal built is still walked after the goal is gone, and the low-priority
// `WaterAvoidingRandomStrollGoal` inherits it — its `canContinueToUse` is
// `!navigation.isDone()` and its own `stop()` is what eventually clears the
// navigation (`RandomStrollGoal.stop() -> getNavigation().stop()`), or its
// `start()`'s `moveTo` replaces the path. The port has no goal objects, so its
// stroll is the fallback handler that sees the live path and continues it: the
// handoff is vanilla-shaped, and this pins it rather than changing it. (An
// earlier reading of the wire test's red-check called the child walking to its
// parent's old cell a bug; it is what the decompiled stop() does.)
void test_follow_parent_stop_leaves_the_path() {
  MobEnvironment env = flat_world();
  // Baby at 0.5, adult at 5.5: the follow plans a path to the adult's cell
  // (5.5) on its first tick. The adult then steps in *behind* the baby, to 0.2 —
  // inside three, so `canContinueToUse` fails and the goal stops on the next
  // tick with that path still live and still aimed east at 5.5.
  Mob baby = make_mob("cow", 0.5, 1.0, 0.5, /*id=*/0);
  baby.breed_age = kAnimalBabyStartAge;
  baby.stray_cooldown = 100000;
  Mob adult = make_mob("cow", 5.5, 1.0, 0.5, /*id=*/1);
  adult.stray_cooldown = 100000;
  std::vector<Mob> mobs{std::move(baby), std::move(adult)};
  tick_mobs(mobs, env);
  CHECK_EQ(mobs[0].parent_entity_id, 1);
  CHECK_NEAR(mobs[0].path_to_x, 5.5, 1e-9);
  CHECK(mobs[0].has_path_goal);
  mobs[1].x = 0.2; // a gap of 0.3, well inside `DONT_FOLLOW_IF_CLOSER_THAN`
  tick_mobs(mobs, env);
  CHECK_EQ(mobs[0].parent_entity_id, -1); // `stop()`: the parent goes
  // ... and the path stays, which is the whole claim: `stop()` does not stop
  // the navigation. The stroll takes the tick and inherits that path.
  CHECK(mobs[0].has_path_goal);
  CHECK_NEAR(mobs[0].path_to_x, 5.5, 1e-9);
  CHECK(mobs[0].goal == MobGoal::Stroll);
  // The walk carries the child east, toward the *planned* cell — away from the
  // parent standing behind it — until the gap opens past three and the follow
  // can take hold again, which is where the stroll's inherited walk ends.
  double furthest = mobs[0].x;
  bool reacquired = false;
  for (int i = 0; i < 200 && !reacquired; ++i) {
    tick_mobs(mobs, env);
    furthest = std::max(furthest, mobs[0].x);
    reacquired = mobs[0].goal == MobGoal::FollowParent;
  }
  CHECK(reacquired);
  CHECK(furthest > 2.0); // it really walked the inherited path out
  CHECK(furthest < 5.6); // and it is the three-block line that ended the walk
}

void test_place_nudges_a_mob_off_a_rim() {
  // The breeding scene's containment, pinned as a mechanism. A cow summoned
  // with its box inside a two-block rim column used to be lifted a block at a
  // time until the box was clear — which put its feet on the rim's top, two
  // blocks above the floor, where one stroll walked it off the outside. The
  // unstuck search now prefers the nearest *supported* free cell on the summon's
  // own level, so the same cow lands on the corridor floor one block inward.
  MobEnvironment rim;
  rim.is_solid = [](int, int y, int z) { return y <= 0 || (z == 4 && (y == 1 || y == 2)); };
  Mob cow;
  mob_place(cow, *mob_by_name("cow"), rim, 0.5, 1.0, 3.6, 0.0f);
  CHECK_NEAR(cow.y, 1.0, 1e-9);                    // still on the floor
  CHECK(cow.z + cow.half_width() <= 4.0 + 1.0e-9); // and clear of the rim
  CHECK(cow.z < 3.6);                              // nudged inward, not out
  // The vertical lift is still the fallback when no same-level cell is free:
  // a mob placed with the whole level solid (the buried-pig case) goes up.
  MobEnvironment sealed;
  sealed.is_solid = [](int, int y, int) { return y <= 1; };
  Mob buried;
  mob_place(buried, *mob_by_name("cow"), sealed, 0.5, 1.0, 0.5, 0.0f);
  CHECK_NEAR(buried.y, 2.0, 1e-9);
}

void test_id_lookup_after_a_death() {
  // `tick_mobs` resolves a mate or a parent by entity id through a per-tick
  // index (`MobIndex`), so the target's distance in the population must not
  // matter: the baby here asks for an id that sits at the very end, behind
  // sixty idle sheep. A dead parent must then resolve to nothing in the same
  // tick it is erased, so the goal stops instead of walking to a stale slot.
  MobEnvironment env = flat_world();
  std::vector<Mob> mobs;
  for (int i = 0; i < 60; ++i) {
    Mob sheep = make_mob("sheep", 30.0 + i, 1.0, 30.0, /*id=*/1000 + i);
    sheep.stray_cooldown = 1000000;
    mobs.push_back(std::move(sheep));
  }
  Mob baby = make_mob("cow", 0.5, 1.0, 0.5, /*id=*/0);
  baby.breed_age = kAnimalBabyStartAge;
  mobs.push_back(std::move(baby));
  Mob parent = make_mob("cow", 4.5, 1.0, 0.5, /*id=*/77);
  parent.stray_cooldown = 1000000;
  mobs.push_back(std::move(parent));
  constexpr size_t kBaby = 60;
  constexpr size_t kParent = 61;

  tick_mobs(mobs, env);
  CHECK_EQ(mobs[kBaby].parent_entity_id, 77);
  CHECK(mobs[kBaby].goal == MobGoal::FollowParent);
  tick_mobs(mobs, env);
  CHECK(mobs[kBaby].x > 0.5); // it walks toward the parent it resolved

  // Death is picked up on the next tick, and the erase takes the parent's slot
  // with it; the baby's id must stop resolving that tick, not point at whatever
  // entity inherited the slot.
  mobs[kParent].health = 0.0f;
  tick_mobs(mobs, env);
  CHECK_EQ(mobs.size(), static_cast<size_t>(61));
  CHECK_EQ(mobs[kBaby].parent_entity_id, -1);
}

void test_packets() {
  // Mth.packDegrees: 90 degrees is a quarter turn, 0 is 0, 180 is 128 (the wrap).
  CHECK(mc::server::pack_degrees(0.0f) == 0);
  CHECK(mc::server::pack_degrees(90.0f) == 64);
  CHECK(mc::server::pack_degrees(180.0f) == 128);
  CHECK(mc::server::pack_degrees(270.0f) == 192);

  uint8_t uuid[16] = {0};
  const std::vector<uint8_t> spawn =
      mc::server::make_mob_spawn(7, uuid, 154, 1.5, 2.5, 3.5, 90.0f, -30.0f, 90.0f);
  // varint packet id (0x01), varint entity id, uuid, varint type, three f64,
  // LpVec3 (one 0x00 byte), pitch, yaw, head yaw, varint data.
  CHECK(spawn.size() == 2 + 16 + 2 + 24 + 1 + 3 + 1);
  CHECK(spawn[0] == mc::protocol::kPlaySpawnEntityId);
  CHECK(spawn[1] == 7);
  CHECK(spawn[18] == 154); // the entity type after the 16-byte uuid
  CHECK(spawn.back() == 0);
  CHECK(spawn[spawn.size() - 4] == mc::server::pack_degrees(-30.0f));
  CHECK(spawn[spawn.size() - 3] == mc::server::pack_degrees(90.0f));
  CHECK(spawn[spawn.size() - 2] == mc::server::pack_degrees(90.0f));

  // set_entity_data carrying health: LivingEntity.DATA_HEALTH is index 9 and the
  // value is a FLOAT (serializer 3), then the 0xFF terminator.
  const std::vector<uint8_t> health = mc::server::make_health_word(7, 17.5f);
  CHECK(health.size() == 1 + 1 + 1 + 1 + 4 + 1);
  CHECK(health[0] == mc::protocol::kPlaySetEntityDataId);
  CHECK(health[1] == 7);
  CHECK(health[2] == mc::server::kLivingEntityHealthIndex);
  CHECK(health[3] == mc::server::kFloatSerializerId);
  CHECK(health[8] == 0xFF); // the synched-data terminator comes after the float
  // 17.5f big-endian is 0x41 8C 00 00 (the writer is a network byte order float).
  CHECK(health[4] == 0x41);
  CHECK(health[5] == 0x8C);
  CHECK(health[6] == 0x00);
  CHECK(health[7] == 0x00);

  // The shared flags byte: `Entity.DATA_SHARED_FLAGS_ID` is index 0 and the
  // bit that matters here is `Entity.FLAG_ONFIRE` (0), with `FLAG_GLOWING` (6)
  // on a mob that has Glowing. They live in *one* byte — as do the bits a player
  // can raise and a mob cannot (`FLAG_SHIFT_KEY_DOWN` 1, `FLAG_SPRINTING` 3,
  // `FLAG_INVISIBLE` 5) — so a sender that knows about only one of them erases
  // the others, and the byte is built in one place (`shared_flags_byte`).
  CHECK_EQ(static_cast<int>(mc::server::kSharedFlagOnFire), 1 << 0);
  CHECK_EQ(static_cast<int>(mc::server::kSharedFlagShiftKeyDown), 1 << 1);
  CHECK_EQ(static_cast<int>(mc::server::kSharedFlagSprinting), 1 << 3);
  CHECK_EQ(static_cast<int>(mc::server::kSharedFlagInvisible), 1 << 5);
  CHECK_EQ(static_cast<int>(mc::server::kSharedFlagGlowing), 1 << 6);
  const std::vector<uint8_t> flags =
      mc::server::make_shared_flags(7, mc::server::shared_flags_byte(true, true, true, true, true));
  CHECK_EQ(flags.size(), 1u + 1u + 1u + 1u + 1u + 1u); // id, entity id, index, type, byte, end
  CHECK(flags[0] == mc::protocol::kPlaySetEntityDataId);
  CHECK(flags[1] == 7);
  CHECK(flags[2] == mc::server::kSharedFlagsIndex);
  CHECK(flags[3] == mc::server::kByteSerializerId);
  CHECK(flags[4] == 0x6B); // on fire, sneak, sprint, invisible and glowing together
  CHECK(flags[5] == 0xFF);
  auto byte = [](bool on_fire, bool glowing, bool invisible = false, bool sneak = false,
                 bool sprinting = false) {
    return mc::server::make_shared_flags(
        7, mc::server::shared_flags_byte(on_fire, glowing, invisible, sneak, sprinting))[4];
  };
  CHECK(byte(false, true) == 0x40);
  CHECK(byte(true, false) == 0x01);
  CHECK(byte(false, false) == 0x00);
  CHECK(byte(false, false, true) == 0x20);
  CHECK(byte(false, false, false, true) == 0x02);
  CHECK(byte(false, false, false, false, true) == 0x08);

  // The other byte of a LivingEntity's synched data:
  // `DATA_LIVING_ENTITY_FLAGS` is the *first* of LivingEntity's own values, so
  // 8, one ahead of health — and the one bit this server can raise is
  // `isUsingItem`'s (0x01); 0x02 (which hand) and 0x04 (the mace's spin) have no
  // writer here.
  CHECK_EQ(static_cast<int>(mc::server::kLivingEntityFlagsIndex), 8);
  CHECK_EQ(static_cast<int>(mc::server::kLivingEntityFlagsIndex + 1),
           static_cast<int>(mc::server::kLivingEntityHealthIndex));
  CHECK_EQ(static_cast<int>(mc::server::kLivingEntityFlagUsingItem), 1);
  CHECK(mc::server::living_entity_flags_byte(false) == 0x00);
  CHECK(mc::server::living_entity_flags_byte(true) == 0x01);
  CHECK(mc::server::living_entity_flags_byte(false, true, true) == 0x06);
  const std::vector<uint8_t> using_item =
      mc::server::make_living_entity_flags(7, mc::server::living_entity_flags_byte(true));
  CHECK_EQ(using_item.size(), 1u + 1u + 1u + 1u + 1u + 1u);
  CHECK(using_item[0] == mc::protocol::kPlaySetEntityDataId);
  CHECK(using_item[1] == 7);
  CHECK(using_item[2] == mc::server::kLivingEntityFlagsIndex);
  CHECK(using_item[3] == mc::server::kByteSerializerId);
  CHECK(using_item[4] == 0x01); // the shield is up
  CHECK(using_item[5] == 0xFF); // end of synched data
}

} // namespace

// `RangedBowAttackGoal`: the skeleton family stands inside fifteen blocks, aims
// at the target's one-third height plus `horizontal * 0.2` (the arc), and looses
// one arrow every twenty ticks. Everything here is the pure tick's report; the
// session turns each `MobShot` into the same projectile entity a player's bow
// spawns (`tests/skeleton_arrow_test.py` proves that half over the wire).
void test_ranged_attack() {
  MobEnvironment env = flat_world();
  env.players.push_back(player_at(8.5, 1.0, 0.5));
  std::vector<Mob> mobs{make_mob("skeleton", 0.5, 1.0, 0.5)};

  // Tick 1 is the scan: the target is acquired but its visibility is filled in
  // by the retention pass, so the goal reads it on tick 2 (`update_target`'s
  // own ordering, not the ranged goal's).
  const MobTickReport acquired = tick_mobs(mobs, env);
  CHECK(acquired.shots.empty());
  CHECK(mobs[0].target_entity_id == 100);
  CHECK(mobs[0].goal == MobGoal::Chase);

  const MobTickReport first = tick_mobs(mobs, env);
  CHECK_EQ(first.shots.size(), 1);
  CHECK(first.attacks.empty()); // a bow-armed mob never swings
  CHECK(mobs[0].goal == MobGoal::RangedAttack);
  CHECK_EQ(mobs[0].attack_cooldown, kMobBowInterval);
  CHECK(mobs[0].target_entity_id == 100);
  const MobShot& shot = first.shots[0];
  CHECK_EQ(shot.mob_entity_id, 1);
  // The aim is the shooter's eye, not its feet: an arrow is spawned from there
  // (`AbstractArrow`'s `getEyeY() - 0.1`).
  CHECK_NEAR(shot.y, mob_eye_y(mobs[0]), 1e-9);
  CHECK_NEAR(shot.x, 0.5, 1e-9);
  CHECK_NEAR(shot.z, 0.5, 1e-9);
  // Look vector (-sin yaw, cos yaw): +x is yaw -90.
  CHECK_NEAR(shot.yaw, -90.0, 1e-4);
  // `d1 + d3 * 0.2` with d1 negative (the player's one-third height is below the
  // skeleton's eye) but the 8-block arc larger, so the shot is angled *up*:
  // negative pitch, the client's convention, which `shoot_arrow` consumes.
  CHECK(shot.pitch < 0.0f);
  CHECK_NEAR(shot.pitch, -3.65, 0.2);

  // The interval: nineteen ticks of nothing, then the twentieth looses again
  // (`attack_cooldown` is decremented before the goal reads it).
  int shots = 0;
  for (int i = 0; i < kMobBowInterval - 1; ++i)
    shots += static_cast<int>(tick_mobs(mobs, env).shots.size());
  CHECK_EQ(shots, 0);
  CHECK_EQ(tick_mobs(mobs, env).shots.size(), 1);

  // Out of the fifteen-block radius it chases instead of shooting (the player
  // is still inside the skeleton's sixteen-block target scan, so the goal has
  // a target and the radius check is what keeps the bow quiet).
  MobEnvironment far = flat_world();
  far.players.push_back(player_at(15.9, 1.0, 0.5));
  std::vector<Mob> far_mobs{make_mob("skeleton", 0.5, 1.0, 0.5)};
  for (int i = 0; i < 3; ++i)
    CHECK(tick_mobs(far_mobs, far).shots.empty());
  CHECK(far_mobs[0].target_entity_id == 100);
  CHECK(far_mobs[0].goal == MobGoal::Chase);

  // Losing sight mid-fight stops the bow: the running goal's `canContinueToUse`
  // wants both the radius and the sight, so a wall turns the next shot into a
  // chase (the target itself is kept for the unseen memory).
  MobEnvironment walled = flat_world();
  walled.players.push_back(player_at(8.5, 1.0, 0.5));
  walled.is_solid = [](int x, int y, int) { return y <= 0 || (x == 4 && y >= 1 && y <= 3); };
  std::vector<Mob> walled_mobs{make_mob("skeleton", 0.5, 1.0, 0.5)};
  // Acquire while the wall is down: the scan on tick 1, the shot on tick 2.
  MobEnvironment open = flat_world();
  open.players = walled.players;
  tick_mobs(walled_mobs, open);
  CHECK_EQ(tick_mobs(walled_mobs, open).shots.size(), 1);
  // ...then raise it: the shot that would be due next never comes.
  int blocked = 0;
  for (int i = 0; i < kMobBowInterval + 5; ++i)
    blocked += static_cast<int>(tick_mobs(walled_mobs, walled).shots.size());
  CHECK_EQ(blocked, 0);
  CHECK(walled_mobs[0].target_entity_id == 100); // the memory still holds
  CHECK(walled_mobs[0].goal == MobGoal::Chase);

  // A pig holds no bow at all: the same scene produces no shot.
  MobEnvironment pen = flat_world();
  pen.players.push_back(player_at(5.5, 1.0, 0.5));
  std::vector<Mob> pigs{make_mob("pig", 0.5, 1.0, 0.5)};
  for (int i = 0; i < 40; ++i)
    CHECK(tick_mobs(pigs, pen).shots.empty());
}

static void test_dragon_flight() {
  // A dragon summoned at ground level starts with its steering box over the
  // floor. It has to climb out instead of being pinned by that first overlap.
  MobEnvironment env = flat_world();
  Mob dragon = make_mob("minecraft:ender_dragon", 0.5, 0.0, 0.5);
  dragon.y = 0.0;
  dragon.on_ground = false;
  dragon.persistence_required = true;
  dragon.dragon_perch_x = 0.5;
  dragon.dragon_perch_y = 0.0;
  dragon.dragon_perch_z = 0.5;
  if (dragon.type != nullptr)
    dragon.health = static_cast<float>(dragon.type->max_health);
  std::vector<Mob> mobs{dragon};
  const double start_y = mobs.front().y;
  for (int i = 0; i < 40; ++i)
    tick_mobs(mobs, env);
  CHECK(!mobs.empty());
  if (!mobs.empty())
    CHECK(mobs.front().y > start_y + 1.0);
}

static void test_dragon_perch() {
  // At half health the perch phase leaves the circling altitude and flies down
  // to the summon column. The flat floor keeps terrain out of this check; the
  // wire test covers the trip through generated terrain.
  MobEnvironment env = flat_world();
  Mob dragon = make_mob("minecraft:ender_dragon", 0.5, 20.0, 0.5);
  dragon.on_ground = false;
  dragon.persistence_required = true;
  dragon.dragon_perch_x = 0.5;
  dragon.dragon_perch_y = 0.0;
  dragon.dragon_perch_z = 0.5;
  if (dragon.type != nullptr)
    dragon.health = dragon.type->max_health * 0.4f;
  std::vector<Mob> mobs{dragon};
  const double start_y = mobs.front().y;
  for (int i = 0; i < 120; ++i)
    tick_mobs(mobs, env);
  CHECK(!mobs.empty());
  if (!mobs.empty()) {
    CHECK(mobs.front().dragon_phase == 2 || mobs.front().dragon_phase == 3);
    CHECK(mobs.front().y < start_y - 4.0);
  }
}

void test_wither_encounter() {
  // The spawn shield refuses damage and ends in the power-7 blast with the bar
  // full. One tick of a one-tick shield is the whole sequence.
  Mob wither = make_mob("wither", 0.5, 1.0, 0.5);
  wither.wither_shield_ticks = 1;
  wither.health = 1.0f;
  MobEnvironment env = flat_world();
  CHECK(!hurt_mob(wither, 50.0f, -1, {}, 0.0, 0.0, 0.0).accepted);
  std::vector<Mob> mobs{wither};
  const MobTickReport report = tick_mobs(mobs, env);
  CHECK_EQ(report.explosions.size(), 1);
  CHECK(report.explosions[0].power == kWitherShieldExplosionPower);
  CHECK_EQ(mobs[0].wither_shield_ticks, 0);
  CHECK(mobs[0].health == mobs[0].type->max_health);

  // The arrow armour is the half-health line and nothing else — not a flag that
  // sticks, and not something another boss inherits.
  Mob half = make_mob("wither", 0.5, 1.0, 0.5);
  half.health = 150.0f;
  CHECK(mob_wither_arrow_immune(half));
  half.health = 151.0f;
  CHECK(!mob_wither_arrow_immune(half));
  Mob dragon = make_mob("ender_dragon", 0.5, 1.0, 0.5);
  dragon.health = 1.0f;
  CHECK(!mob_wither_arrow_immune(dragon));
}

int main() {
  test_table();
  test_place_and_gravity();
  test_wall_stops_it();
  test_pen_rim_is_not_climbable();
  test_overlapping_box_walks_out();
  test_line_of_sight();
  test_line_of_sight_walk_is_the_documented_sampling();
  test_target_needs_sight();
  test_target_memory_after_line_of_sight_breaks();
  test_hurt_provokes_the_chase();
  test_retaliation();
  test_target_is_not_swapped();
  test_blind_mob_cannot_hit();
  test_hostile_chase_and_attack();
  test_passive_and_untargetable();
  test_stray_walk();
  test_stuck_detection();
  test_stuck_detection_timeout_path();
  test_stuck_detection_never_fires_on_a_walk();
  test_hurt_armour_and_window();
  test_wither_armour_phase();
  test_hurt_knockback_and_death();
  test_fall_damage_kills();
  test_below_world_discard();
  test_despawn_scan_order();
  test_hurt_then_tick_reports_hurt();
  test_fire_burns_a_mob();
  test_spectral_glow_on_a_mob();
  test_attack_reach();
  test_ranged_attack();
  test_dragon_flight();
  test_dragon_perch();
  test_wither_encounter();
  test_mob_loot();
  test_animal_food_table();
  test_feeding();
  test_growth_and_love_in_the_tick();
  test_breeding();
  test_follow_parent();
  test_follow_parent_band();
  test_follow_parent_stop_leaves_the_path();
  test_place_nudges_a_mob_off_a_rim();
  test_id_lookup_after_a_death();
  test_packets();
  if (failures == 0)
    std::printf("mob: all checks passed\n");
  else
    std::printf("mob: %d check(s) failed\n", failures);
  return failures == 0 ? 0 : 1;
}
