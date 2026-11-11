// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mob simulation: see world/mob.h for what is mirrored and what is not.
#include "world/mob.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

#include "world/axis_step.h"    // Entity.move's Direction.axisStepOrder
#include "world/damage.h"       // damage_after_absorb: CombatRules' armour step
#include "world/item.h"         // normalize_item_name: the shared identifier normalizer
#include "world/mob_category.h" // MobCategory's despawn distances (check_despawn)

namespace mc::world {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRadToDeg = 180.0 / kPi;

double unit(const MobEnvironment& env) {
  return env.unit_random ? env.unit_random() : 0.0;
}

double ranged(const MobEnvironment& env, double lo, double hi) {
  return lo + unit(env) * (hi - lo);
}

int ranged_int(const MobEnvironment& env, int lo, int hi) {
  if (hi <= lo)
    return lo;
  const int v = lo + static_cast<int>(unit(env) * static_cast<double>(hi - lo + 1));
  return std::min(v, hi);
}

bool solid_at(const MobEnvironment& env, int x, int y, int z) {
  return env.is_solid && env.is_solid(x, y, z);
}

// Whether a mob-sized box at that position is clear of solid blocks. The box is
// shrunk by a hair so a mob resting exactly on a block's top face (feet y on the
// integer boundary) does not count the floor it stands on as a collision.
bool mob_box_free(const MobEnvironment& env, double x, double y, double z, double half,
                  double height) {
  constexpr double kEps = 1.0e-7;
  const int x0 = static_cast<int>(std::floor(x - half + kEps));
  const int x1 = static_cast<int>(std::floor(x + half - kEps));
  const int y0 = static_cast<int>(std::floor(y + kEps));
  const int y1 = static_cast<int>(std::floor(y + height - kEps));
  const int z0 = static_cast<int>(std::floor(z - half + kEps));
  const int z1 = static_cast<int>(std::floor(z + half - kEps));
  for (int bx = x0; bx <= x1; ++bx)
    for (int by = y0; by <= y1; ++by)
      for (int bz = z0; bz <= z1; ++bz)
        if (solid_at(env, bx, by, bz))
          return false;
  return true;
}

// Whether a mob-sized box overlaps the block cell `(bx, by, bz)` at all.
// `AABB.intersects` is strict on every axis, so a box resting exactly on a
// block's top face (feet y on the integer boundary) does not overlap the floor
// under it — which is what lets the escape rule below tell "standing on" from
// "inside".
bool box_overlaps_cell(double x, double y, double z, double half, double height, int bx, int by,
                       int bz) {
  return x - half < static_cast<double>(bx + 1) && x + half > static_cast<double>(bx) &&
         y < static_cast<double>(by + 1) && y + height > static_cast<double>(by) &&
         z - half < static_cast<double>(bz + 1) && z + half > static_cast<double>(bz);
}

// The collision test `Entity.move` actually applies. `VoxelShape.collideX`
// sweeps only the shape slices beyond the moving box's *leading face*
// (`for a = aMax + 1; ...`), so a block the box already intersects is not a
// collision for that axis at all. That is what lets an entity that ended up
// inside geometry walk (or fall) back out: every candidate position still
// overlaps, so a plain "is the target box free" test would pin it for good,
// while vanilla's per-axis sweep does not.
//
// So: a solid cell the box at `from_*` already overlaps is not counted; every
// other solid cell under the target box is. With a box that overlaps nothing
// this is exactly `mob_box_free`.
bool mob_box_free_escaping(const MobEnvironment& env, double from_x, double from_y, double from_z,
                           double x, double y, double z, double half, double height) {
  constexpr double kEps = 1.0e-7;
  const int x0 = static_cast<int>(std::floor(x - half + kEps));
  const int x1 = static_cast<int>(std::floor(x + half - kEps));
  const int y0 = static_cast<int>(std::floor(y + kEps));
  const int y1 = static_cast<int>(std::floor(y + height - kEps));
  const int z0 = static_cast<int>(std::floor(z - half + kEps));
  const int z1 = static_cast<int>(std::floor(z + half - kEps));
  for (int bx = x0; bx <= x1; ++bx)
    for (int by = y0; by <= y1; ++by)
      for (int bz = z0; bz <= z1; ++bz) {
        if (!solid_at(env, bx, by, bz))
          continue;
        if (box_overlaps_cell(from_x, from_y, from_z, half, height, bx, by, bz))
          continue; // already inside it: vanilla's sweep does not look there
        return false;
      }
  return true;
}

// The yaw that faces a horizontal direction: MoveControl's
// `(atan2(dz, dx) * 180/pi) - 90`, so the look vector (-sin yaw, 0, cos yaw)
// points that way.
float facing_yaw(double dx, double dz) {
  if (dx == 0.0 && dz == 0.0)
    return 0.0f;
  return static_cast<float>(std::atan2(dz, dx) * kRadToDeg - 90.0);
}

// The five rows that register `RangedBowAttackGoal`: `AbstractSkeleton` and its
// four 26.2 subclasses (`stray`, `bogged`, `parched`, `wither_skeleton`). The
// table's other ranged abilities are *different* goals this engine still does
// not model — a pillager's or piglin's crossbow, an illusioner's bow, a witch's
// potion, a ghast's or blaze's fireball — and `mob_table.h` names them where the
// row cannot express one.
bool mob_bow_armed(const Mob& mob) {
  if (mob.type == nullptr)
    return false;
  const std::string_view name = mob.type->name;
  return name == "minecraft:skeleton" || name == "minecraft:stray" || name == "minecraft:bogged" ||
         name == "minecraft:parched" || name == "minecraft:wither_skeleton";
}

NavEnvironment nav_env(const MobEnvironment& env, const GeneratedMob& type) {
  return NavEnvironment{env.is_solid, static_cast<double>(type.height)};
}

const MobPlayer* find_player(const MobEnvironment& env, int32_t entity_id) {
  if (entity_id < 0)
    return nullptr;
  for (const MobPlayer& p : env.players)
    if (p.entity_id == entity_id)
      return &p;
  return nullptr;
}

} // namespace

// The two exported reach helpers (world/mob.h): declared at namespace scope so
// the tests can call the same arithmetic the goal uses.
//
// MeleeAttackGoal.getAttackReachSqr: (bbWidth * 2)^2 + target width.
double attack_reach_sqr(const Mob& mob, const MobPlayer& p) {
  const double reach = static_cast<double>(mob.type->width) * 2.0;
  return reach * reach + static_cast<double>(p.width);
}

// MeleeAttackGoal's perceived distance: the gap between the mob's position and
// the target's bounding box (0 inside it), squared.
double perceived_distance_sqr(const Mob& mob, const MobPlayer& p) {
  const double dx = std::max(0.0, std::abs(p.x - mob.x) - p.width * 0.5);
  const double dz = std::max(0.0, std::abs(p.z - mob.z) - p.width * 0.5);
  const double dy =
      std::max(0.0, std::max(p.y - (mob.y + mob.body_height()), mob.y - (p.y + p.height)));
  return dx * dx + dy * dy + dz * dz;
}

namespace {

// One tick of physics: gravity, the drag, then the axis-separated collision
// response, in `Entity.move`'s order — Y first (`Direction.axisStepOrder`,
// world/axis_step.h), then whichever of X and Z the mob is moving along harder.
// `dir_x`/`dir_z` is the unit direction the AI wants to walk in; `walking` false
// means "coast to a stop".
void move_mob(Mob& mob, const MobEnvironment& env, double dir_x, double dir_z, bool walking) {
  const double half = mob.half_width();
  const double height = mob.body_height();
  // `Entity.move` clips each axis against the box as it stands when that axis
  // is resolved, ignoring solid cells the box already overlaps; a box that
  // overlaps nothing moves exactly as the plain free test describes, and one
  // embedded in geometry is never pinned by penetration the way a
  // target-box-only test would pin it. The `from` position is read live: X and
  // Z are resolved in sequence (so Z starts from the moved X), and each Y
  // sub-step starts from the position the previous one reached.
  const auto free_at = [&](double x, double y, double z) {
    return mob_box_free_escaping(env, mob.x, mob.y, mob.z, x, y, z, half, height);
  };
  // `PathNavigation.followThePath`'s `speedModifier * mob.getSpeed()`: the
  // modifier is whatever the last `moveTo` set (`follow_path` writes it here),
  // 1.0 for every goal this engine carries except `FollowParentGoal`.
  const double speed = mob_speed_per_tick(*mob.type) * mob.move_speed_scale;

  // The horizontal acceleration that makes the terminal speed exactly `speed`:
  // each tick vanilla adds `amount` and then multiplies the whole delta by the
  // friction, so the terminal value is amount / (1 - friction) and adding
  // `speed * (1 - friction)` lands on `speed`. An airborne mob gets no push here
  // (vanilla's flying speed, 0.02, is a twentieth of a walk).
  const double friction = mob.on_ground ? kMobGroundFriction : kMobAirFriction;
  if (walking && mob.on_ground) {
    mob.vx += dir_x * speed * (1.0 - friction);
    mob.vz += dir_z * speed * (1.0 - friction);
  }
  mob.vy = std::max(mob.vy - kMobGravity, -kMobTerminalVelocity);

  // --- Y ---
  // Entity.move sweeps the movement and stops at the first block it meets. This
  // walks the same distance in sub-steps no larger than the smallest mob, so a
  // fast fall cannot tunnel through a one-block floor, and then snaps to the
  // surface it landed on (a mob rests *on* the block, not wherever the last
  // free sub-step left it).
  constexpr double kMaxSubStep = 0.5;
  {
    double remaining = mob.vy;
    while (remaining != 0.0) {
      const double step = std::max(-kMaxSubStep, std::min(kMaxSubStep, remaining));
      // The fall starts at the y the mob last stood at, which is where this
      // downward step began (Entity.fallDistance measures the whole drop).
      if (step < 0.0 && !mob.falling) {
        mob.falling = true;
        mob.fall_start_y = mob.y;
      }
      const double next = mob.y + step;
      if (free_at(mob.x, next, mob.z)) {
        mob.y = next;
        remaining -= step;
        mob.on_ground = false;
        continue;
      }
      if (remaining < 0.0) {
        mob.on_ground = true; // landed: snap down onto the block's top face
        const double surface = std::floor(mob.y + 1.0e-7);
        if (surface < mob.y && mob_box_free(env, mob.x, surface, mob.z, half, height))
          mob.y = surface;
      }
      mob.vy = 0.0;
      break;
    }
  }

  // --- X and Z, in `Direction.axisStepOrder`'s order ---
  // Not X-then-Z: `collideWithShapes` walks whichever horizontal axis carries
  // the larger movement first, and which one wins decides the corner a mob
  // walking diagonally slides around. Read from the delta before either axis is
  // resolved.
  int step_order[3];
  axis_step_order(mob.vx, mob.vz, step_order);
  mob.horizontal_collision = false;
  for (int slot = 1; slot < 3; ++slot) {
    const int axis = step_order[slot];
    double* const pos = axis == 0 ? &mob.x : &mob.z;
    double* const vel = axis == 0 ? &mob.vx : &mob.vz;
    if (*vel == 0.0)
      continue;
    const double next = *pos + *vel;
    const bool free = axis == 0 ? free_at(next, mob.y, mob.z) : free_at(mob.x, mob.y, next);
    if (free)
      *pos = next;
    else {
      *vel = 0.0;
      mob.horizontal_collision = true;
    }
  }

  // --- the drag ---
  if (mob.on_ground) {
    mob.vx *= kMobGroundFriction;
    mob.vz *= kMobGroundFriction;
  } else {
    mob.vx *= kMobAirFriction;
    mob.vz *= kMobAirFriction;
  }
  mob.vy *= kMobVerticalDrag;
  if (mob.on_ground) {
    if (std::abs(mob.vx) < 1.0e-4)
      mob.vx = 0.0;
    if (std::abs(mob.vz) < 1.0e-4)
      mob.vz = 0.0;
  }

  // Entity.checkFallDamage spends the whole drop on the landing; the distance is
  // the exact difference so rounding never turns a four-block fall into three.
  if (mob.on_ground && mob.falling) {
    mob.fall_distance = std::max(0.0, mob.fall_start_y - mob.y);
    mob.falling = false;
  }
}

// PathNavigation.followThePath + JumpControl: re-plan when the goal moved or the
// plan aged out, then walk toward the current waypoint. `walk`/`dir_*` are the
// movement input the caller gives the physics.
//
// `replan` is MeleeAttackGoal.tick's gate on *looking* for a new path —
// `followingTargetEvenIfNotSeen || hasLineOfSight(target)` — so a blind mob
// walks the path it already had. A stroll's goal is a fixed spot no one can
// hide from, so it always passes true.
void follow_path(Mob& mob, const MobEnvironment& env, double goal_x, double goal_y, double goal_z,
                 bool replan, double speed, bool* walk, double* dir_x, double* dir_z) {
  // `PathNavigation.moveTo` sets the speed modifier the path follower scales the
  // mob's own speed by: every caller here passes its goal registration's number
  // — 1.0 for the stroll, the chase and the breed, 1.1/1.25 for the follow.
  mob.move_speed_scale = speed;
  *walk = false;
  *dir_x = 0.0;
  *dir_z = 0.0;
  const NavEnvironment nav = nav_env(env, *mob.type);
  const NavCell goal = nav_cell_at(goal_x, goal_y, goal_z);

  const double gx = mob.path_to_x - goal_x;
  const double gy = mob.path_to_y - goal_y;
  const double gz = mob.path_to_z - goal_z;
  // MeleeAttackGoal.tick's `target.distanceToSqr(pathedTarget*) >= 1.0`.
  const bool goal_moved =
      !mob.has_path_goal || gx * gx + gy * gy + gz * gz >= kMobRepathDistance * kMobRepathDistance;
  bool plan = false;
  if (replan) {
    plan = !mob.has_path_goal || mob.path_cooldown <= 0 || goal_moved ||
           (mob.path.done() && nav_cell_at(mob.path_to_x, mob.path_to_y, mob.path_to_z) != goal);
  } else {
    // Blind: MeleeAttackGoal.canContinueToUse is `!navigation.isDone()`, so the
    // goal only ends when the mob has walked the whole path — and canUse, whose
    // `lastCanUseCheck` gate allows one start per twenty ticks, is the only way
    // back in. That fresh path aims at the target's *current* position, since
    // Navigation.createPath does not care about sight.
    plan = mob.path.done() && mob.path_cooldown <= 0;
  }
  if (plan) {
    mob.path = find_path(nav_cell_at(mob.x, mob.y, mob.z), goal, nav, nullptr);
    mob.path_to_x = goal_x;
    mob.path_to_y = goal_y;
    mob.path_to_z = goal_z;
    mob.has_path_goal = true;
    // `PathNavigation.moveTo` re-seeds `lastStuckCheck` on every new path, and
    // this block is the port's moveTo — but the window is deliberately NOT
    // re-seeded here. This engine's one path follower re-plans a stroll on a
    // 4-10 tick cooldown (vanilla's `RandomStrollGoal` calls moveTo only when
    // its path is done), so re-seeding on every plan would let a wedged walk
    // suppress the hundred-tick check for good. That check is the recovery a
    // frozen stroll needs; the per-node timeout below is unaffected either way.
    // MeleeAttackGoal.tick's `4 + random.nextInt(7)`, or the twenty-tick restart
    // gate when the mob is feeling its way blind.
    mob.path_cooldown = replan ? ranged_int(env, kMobRepathMinInterval, kMobRepathMaxInterval)
                               : kMobBlindRepathInterval;
  }
  if (mob.path.done())
    return;

  // `PathNavigation.doStuckDetection`, which vanilla runs at the end of
  // `followThePath` — every tick the navigation is in progress. It stops the
  // navigation in two cases: the mob has not actually moved for a hundred ticks
  // (measured against a budget derived from its own movement attribute), or one
  // node has stayed the next one for three times the walk to it should have
  // taken. `stop()` clears the path, so `isDone()` becomes true — and the goal
  // that owns the navigation ends with it, because `RandomStrollGoal
  // .canContinueToUse` is `!navigation.isDone()`. `stroll` only picks a new spot
  // once its path is done, so without this a mob that is wedged against
  // geometry (or holding a node it cannot reach) keeps that target and pushes
  // into the wall forever: its walk is frozen for the rest of its life.
  ++mob.nav_tick;
  const double attribute_speed = mob.type->movement_speed;
  if (mob.nav_tick - mob.last_stuck_check > 100) {
    // `Mob.getSpeed()` is the movement *attribute*, and vanilla squares
    // anything under 1.0 before turning it into a distance, so a cow's 0.25
    // becomes a 1.56-block budget for the whole hundred ticks: only a mob that
    // really is not moving trips this.
    const double effective =
        attribute_speed >= 1.0 ? attribute_speed : attribute_speed * attribute_speed;
    const double threshold = effective * 100.0 * 0.25;
    // `mobPos.distanceToSqr(lastStuckCheckPos)`, so the vertical term counts
    // too: a mob bobbing in place on a block it cannot climb is still stuck.
    const double check_x = mob.x - mob.last_stuck_check_x;
    const double check_y = mob.y - mob.last_stuck_check_y;
    const double check_z = mob.z - mob.last_stuck_check_z;
    mob.last_stuck_check = mob.nav_tick;
    mob.last_stuck_check_x = mob.x;
    mob.last_stuck_check_y = mob.y;
    mob.last_stuck_check_z = mob.z;
    if (check_x * check_x + check_y * check_y + check_z * check_z < threshold * threshold) {
      mob.is_stuck = true;
      mob.path = NavPath{}; // `stop()`: the path goes, and the goal with it
      return;
    }
    mob.is_stuck = false;
  }
  // The per-node timeout: the same next node for longer than three times the
  // walk it should take, counted in ticks. `timeoutPath()` resets the cache and
  // stops the navigation.
  if (const NavCell* next = mob.path.current()) {
    if (mob.has_timeout_node && mob.timeout_node == *next) {
      mob.timeout_timer += mob.age - mob.timeout_last_check;
    } else {
      mob.timeout_node = *next;
      mob.has_timeout_node = true;
      mob.timeout_timer = 0;
      const double node_x = nav_cell_centre_x(*next) - mob.x;
      const double node_y = static_cast<double>(next->y) - mob.y;
      const double node_z = nav_cell_centre_z(*next) - mob.z;
      const double to_node = std::sqrt(node_x * node_x + node_y * node_y + node_z * node_z);
      mob.timeout_limit = attribute_speed > 0.0 ? to_node / attribute_speed * 20.0 : 0.0;
    }
    if (mob.timeout_limit > 0.0 &&
        static_cast<double>(mob.timeout_timer) > mob.timeout_limit * 3.0) {
      mob.has_timeout_node = false;
      mob.timeout_timer = 0;
      mob.timeout_limit = 0.0;
      mob.is_stuck = false;
      mob.path = NavPath{}; // `timeoutPath()` -> `resetStuckTimeout()` + `stop()`
      return;
    }
    mob.timeout_last_check = mob.age;
  }

  const NavCell* node = mob.path.current();
  double tx = nav_cell_centre_x(*node);
  double tz = nav_cell_centre_z(*node);
  const double reach = std::max(kMobWaypointReach, mob.half_width() * 0.75);
  if ((tx - mob.x) * (tx - mob.x) + (tz - mob.z) * (tz - mob.z) <= reach * reach) {
    mob.path.advance();
    if (mob.path.done())
      return;
    node = mob.path.current();
    tx = nav_cell_centre_x(*node);
    tz = nav_cell_centre_z(*node);
  }
  const double dx = tx - mob.x;
  const double dz = tz - mob.z;
  const double len = std::sqrt(dx * dx + dz * dz);
  if (len > 1.0e-6) {
    *dir_x = dx / len;
    *dir_z = dz / len;
    *walk = true;
    mob.yaw = facing_yaw(*dir_x, *dir_z);
  }
  // JumpControl: a node above the mob's feet is a block to climb; a horizontal
  // collision with the node level or above is a lip to get over.
  const int feet = static_cast<int>(std::floor(mob.y + 1.0e-7));
  if (mob.on_ground && (node->y > feet || (mob.horizontal_collision && node->y >= feet)))
    mob.vy = kMobJumpVelocity;
}

// WaterAvoidingRandomStrollGoal: walk to a random spot a few blocks away, then
// rest before choosing again.
void stroll(Mob& mob, const MobEnvironment& env, bool* walk, double* dir_x, double* dir_z) {
  *walk = false;
  *dir_x = 0.0;
  *dir_z = 0.0;
  const bool walking_plan = mob.has_path_goal && !mob.path.done();
  if (!walking_plan) {
    // The walk is over — either the spot was reached, or the path could not be
    // finished — and both end the stroll (`Navigation.isDone` is the goal's own
    // `canContinueToUse`) — so the target is dropped and a fresh one is picked
    // off the cooldown. A done path with the spot still away must drop it too:
    // nothing walks a finished path, and holding the target would leave the mob
    // standing for the rest of its life. A path another goal left behind — the
    // breed's partner, the chase's target, the follow's parent — is exactly that
    // shape when the goal that owns it stops mid-walk.
    mob.has_path_goal = false;
    if (mob.stray_cooldown > 0) {
      --mob.stray_cooldown;
      mob.goal = MobGoal::Idle;
      return;
    }
    // A random spot 1..3 blocks away in a random direction.
    const double angle = ranged(env, 0.0, 2.0 * kPi);
    const double dist = ranged(env, 1.0, 3.0);
    mob.path = NavPath{};
    mob.path_cooldown = 0;
    mob.path_to_x = mob.x + std::cos(angle) * dist;
    mob.path_to_y = mob.y;
    mob.path_to_z = mob.z + std::sin(angle) * dist;
    mob.has_path_goal = true;
    mob.stray_cooldown = ranged_int(env, kMobStrayMinDelay, kMobStrayMaxDelay);
  }
  mob.goal = MobGoal::Stroll;
  follow_path(mob, env, mob.path_to_x, mob.path_to_y, mob.path_to_z, /*replan=*/true, /*speed=*/1.0,
              walk, dir_x, dir_z);
}

// --- the target selector (TargetGoal and its two subclasses) -----------------

// TargetGoal.stop() plus MeleeAttackGoal.stop(): they share the TARGET and MOVE
// flags in vanilla, so losing the target also clears the navigation and the mob
// falls back to whatever else wants to move it — here, strolling.
void drop_target(Mob& mob) {
  mob.target_entity_id = -1;
  mob.target_provoked = false;
  mob.target_visible = false;
  mob.unseen_ticks = 0;
  mob.path = NavPath{};
  mob.has_path_goal = false;
}

// TargetGoal.start() on a newly acquired target: the sight memory restarts, and
// the goal that takes over builds a fresh path (MeleeAttackGoal.start calls
// `navigation.moveTo`).
void set_target(Mob& mob, int32_t entity_id, bool provoked) {
  if (mob.target_entity_id != entity_id) {
    mob.path = NavPath{};
    mob.has_path_goal = false;
    mob.path_cooldown = 0;
  }
  mob.target_entity_id = entity_id;
  mob.target_provoked = provoked;
  mob.unseen_ticks = 0;
}

// The target selector, in vanilla's priority order: HurtByTargetGoal (1) then
// NearestAttackableTargetGoal (2). Both hold the TARGET flag, and GoalSelector
// only asks a goal to start when none is running, so the scan below is reached
// only when the mob has no target — which is why a mob does not switch to a
// nearer player while it is chasing one it already has.
void update_target(Mob& mob, const MobEnvironment& env) {
  mob.target_visible = false;
  // A mob that neither hunts (hostile) nor retaliates (HurtByTargetGoal) has no
  // targeting goal this engine models at all — a passive animal is not merely a
  // hostile with an empty scan, it is a separate selector.
  if (!mob.hostile() && !mob.retaliates()) {
    if (mob.target_entity_id >= 0)
      drop_target(mob);
    return;
  }

  // --- HurtByTargetGoal.canUse: a fresh `lastHurtByMob` timestamp, and
  // `canAttack(target, HURT_BY_TARGETING)` whose conditions ignore line of sight
  // and invisibility — so being hit is enough, even from out of sight. ---
  if (mob.provoked_fresh) {
    mob.provoked_fresh = false;
    const MobPlayer* attacker = find_player(env, mob.provoked_entity_id);
    if (attacker != nullptr && attacker->can_be_targeted)
      set_target(mob, attacker->entity_id, /*provoked=*/true);
    // No `return`: the retention below is what fills in `target_visible` for the
    // brand new target, and the goal that takes over reads it this same tick.
  }

  const MobPlayer* target = find_player(env, mob.target_entity_id);
  if (mob.target_entity_id >= 0 && target == nullptr)
    drop_target(mob); // they left the world

  // --- the running goal's TargetGoal.canContinueToUse ---
  if (target != nullptr) {
    if (!target->can_be_targeted) {
      // Mob.getTarget() runs asValidTarget, so a target that turned creative,
      // went spectator or died stops being a target at all.
      drop_target(mob);
    } else {
      const double follow = mob.type->follow_range;
      const double dx = target->x - mob.x;
      const double dy = target->y - mob.y;
      const double dz = target->z - mob.z;
      if (dx * dx + dy * dy + dz * dz > follow * follow) {
        // The follow distance is a hard limit on the *running* goal too, so a
        // target that walks out of FOLLOW_RANGE is forgotten at once — and the
        // sight answer is dead here (`drop_target` clears it whatever the test
        // said), so the segment is never walked.
        drop_target(mob);
      } else if (const bool seen =
                     mob_has_line_of_sight(mob, env, target->x, player_eye_y(*target), target->z);
                 seen) {
        mob.target_visible = true;
        // TargetGoal.canContinueToUse keeps the target it has — and the scan
        // below is what "no goal is running" means, so a visible target must
        // *not* fall through to it or a nearer player would steal the chase.
        mob.unseen_ticks = 0;
        return;
      } else if (++mob.unseen_ticks > mob.unseen_memory_ticks()) {
        // The unseen memory ran out: the goal stops and the mob forgets.
        drop_target(mob);
      } else {
        return; // still after something it cannot see
      }
    }
  }

  // --- NearestAttackableTargetGoal.canUse: the nearest targetable player inside
  // FOLLOW_RANGE that passes `TargetingConditions.forCombat()`, which for a
  // `mustSee` goal is the line of sight test. The 1-in-10 random interval on the
  // every-other-tick goal tick is about one scan per ten real ticks. ---
  //
  // Only a *hostile* mob has this goal: a retaliator's targeting selector holds
  // HurtByTargetGoal and nothing else, so a dropped provocation leaves it with
  // no target rather than falling through to a hunt.
  if (!mob.hostile())
    return;
  if (mob.target_scan_cooldown > 0)
    return;
  mob.target_scan_cooldown = kMobTargetScanInterval;
  const double follow = mob.type->follow_range;
  double best = follow * follow;
  int32_t best_id = -1;
  for (const MobPlayer& p : env.players) {
    if (!p.can_be_targeted)
      continue;
    const double dx = p.x - mob.x;
    const double dy = p.y - mob.y;
    const double dz = p.z - mob.z;
    const double d2 = dx * dx + dy * dy + dz * dz;
    if (d2 >= best)
      continue;
    if (mob.must_see() && !mob_has_line_of_sight(mob, env, p.x, player_eye_y(p), p.z))
      continue;
    best = d2;
    best_id = p.entity_id;
  }
  if (best_id >= 0)
    set_target(mob, best_id, /*provoked=*/false);
}

} // namespace

double mob_speed_per_tick(const GeneratedMob& type) {
  return type.movement_speed * type.movement_speed * 43.1718 / 20.0;
}

double mob_eye_y(const Mob& mob) {
  // Entity.getEyeY is `getY() + getEyeHeight()`, and EntityType.Builder.sized
  // fills in EntityDimensions' eye height as `height * 0.85` for every mob.
  return mob.y + mob.body_height() * 0.85;
}

double player_eye_y(const MobPlayer& player) {
  // Player.getEyeHeight while standing (PLAYER_DIMENSIONS eyeHeight 1.62).
  return player.y + player.eye_height;
}

bool eye_line_of_sight(const std::function<bool(int, int, int)>& is_solid, double from_x,
                       double from_y, double from_z, double to_x, double to_y, double to_z) {
  // LivingEntity.hasLineOfSight(target): the segment from this entity's eye to
  // the other's reaches nothing on the way (`Block.COLLIDER`, fluids ignored),
  // and it is refused outright past 128 blocks.
  if (!is_solid)
    return true;
  const double dx = to_x - from_x;
  const double dy = to_y - from_y;
  const double dz = to_z - from_z;
  const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (distance > kMobMaxSightDistance)
    return false;
  if (distance <= 1.0e-9)
    return true;
  // Vanilla clips the segment voxel by voxel; sampling it in half-block steps
  // (a block is one wide) reaches the same answer for every block the segment
  // crosses rather than corners, which is the approximation the rest of this
  // module's collision also makes.
  //
  // A cell is `floor(pos)` on each axis and `std::floor` already lowers to the
  // cheapest sequence the target has (measured against a truncate-and-correct
  // replacement, which serializes the conversion chain and is 50% slower), so
  // the walk's cost is the predicate calls, one per sample.
  const int steps = static_cast<int>(std::ceil(distance / 0.5));
  for (int i = 1; i < steps; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(steps);
    const int bx = static_cast<int>(std::floor(from_x + dx * t));
    const int by = static_cast<int>(std::floor(from_y + dy * t));
    const int bz = static_cast<int>(std::floor(from_z + dz * t));
    if (is_solid(bx, by, bz))
      return false;
  }
  return true;
}

bool mob_has_line_of_sight(const Mob& mob, const MobEnvironment& env, double target_x,
                           double target_y, double target_z) {
  return eye_line_of_sight(env.is_solid, mob.x, mob_eye_y(mob), mob.z, target_x, target_y,
                           target_z);
}

void mob_place(Mob& mob, const GeneratedMob& type, const MobEnvironment& env, double x, double y,
               double z, float yaw) {
  mob.type = &type;
  mob.x = x;
  mob.y = y;
  mob.z = z;
  mob.vx = mob.vy = mob.vz = 0.0;
  mob.yaw = yaw;
  mob.health = type.max_health;
  mob.invulnerable_time = 0.0f;
  mob.last_hurt = 0.0f;
  mob.on_ground = false;
  mob.removed = false;
  // A fresh entity has no target and no memory of being hurt.
  mob.goal = MobGoal::Idle;
  mob.target_entity_id = -1;
  mob.target_provoked = false;
  mob.target_visible = false;
  mob.unseen_ticks = 0;
  mob.provoked_entity_id = -1;
  mob.provoked_fresh = false;
  mob.path = NavPath{};
  mob.has_path_goal = false;
  mob.path_cooldown = 0;
  mob.stray_cooldown = 0;
  mob.attack_cooldown = 0;
  // A fresh entity has never been idle and is not persistent.
  mob.no_action_time = 0;
  mob.persistence_required = false;
  // ExperienceOrb-style unstuck, with one correction: a mob summoned into a
  // block is moved to the nearest *supported* free cell on its own level first,
  // and only lifted when the whole level is solid.
  //
  // The lift alone was wrong for a walled scene. `/summon`'s ring is two blocks
  // out at the summoner's own height, so a summoner standing beside a wall puts
  // a cow inside the wall's column; the lift then stands it on the wall's top,
  // two blocks above the floor — off the floor it was summoned onto, and one
  // stroll away from walking off the outside of a platform with nothing beyond
  // it. A lateral nudge keeps the animal on the floor: the nearest free cell in
  // a walled corridor is on the interior side of the wall, which is exactly
  // where the summon meant to put it. The vertical lift stays as the last
  // resort (a mob summoned into solid rock is merely buried, not teleported into
  // the sky), and a cell with no floor beneath it is not a candidate — being
  // nudged off a platform's edge would trade one containment failure for
  // another.
  const double half = mob.half_width();
  const double height = mob.body_height();
  if (!mob_box_free(env, mob.x, mob.y, mob.z, half, height)) {
    // Nearest first, on a fixed 4-block reach: squared distance as the outer
    // loop, then a fixed scan order, so the choice is deterministic.
    constexpr int kReach = 4;
    bool nudged = false;
    for (int d2 = 1; d2 <= kReach * kReach && !nudged; ++d2) {
      for (int dx = -kReach; dx <= kReach && !nudged; ++dx) {
        for (int dz = -kReach; dz <= kReach; ++dz) {
          if (dx * dx + dz * dz != d2)
            continue;
          const double nx = mob.x + dx;
          const double nz = mob.z + dz;
          const int cell_x = static_cast<int>(std::floor(nx));
          const int cell_z = static_cast<int>(std::floor(nz));
          if (!solid_at(env, cell_x, static_cast<int>(std::floor(mob.y)) - 1, cell_z))
            continue; // nothing to stand on
          if (!mob_box_free(env, nx, mob.y, nz, half, height))
            continue;
          mob.x = nx;
          mob.z = nz;
          nudged = true;
          break;
        }
      }
    }
    for (int i = 0; i < 4 && !mob_box_free(env, mob.x, mob.y, mob.z, half, height); ++i)
      mob.y += 1.0;
  }
}

bool mob_wither_powered(const Mob& mob) {
  return mob.type != nullptr && std::string_view(mob.type->name) == "minecraft:wither" &&
         mob.health <= mob.type->max_health * 0.5f;
}

float mob_armor_value(const Mob& mob) {
  if (mob.type == nullptr)
    return 0.0f;
  // `WitherBoss.getArmorValue`: the ARMOR attribute's 4, plus the 4 the second
  // phase adds. The generated table already carries the base 4 for this row.
  return mob.type->armor + (mob_wither_powered(mob) ? 4.0f : 0.0f);
}

bool mob_deflects_projectiles(const Mob& mob) {
  return mob_wither_powered(mob);
}

bool mob_is_wither(const Mob& mob) {
  return mob.type != nullptr && std::string_view(mob.type->name) == "minecraft:wither";
}

bool mob_is_dragon(const Mob& mob) {
  return mob.type != nullptr && std::string_view(mob.type->name) == "minecraft:ender_dragon";
}

bool mob_wither_arrow_immune(const Mob& mob) {
  // One rule, one implementation: `WitherBoss.isPowered` is the threshold both
  // the arrow refusal and the armour step read.
  return mob_wither_powered(mob);
}

MobHurtResult hurt_mob(Mob& mob, float damage, int32_t attacker_entity_id,
                       std::string_view attacker_name, double knockback, double knockback_x,
                       double knockback_z, bool bypasses_armor) {
  MobHurtResult out;
  if (mob.type == nullptr || !mob.is_alive() || damage <= 0.0f)
    return out;

  // `WitherBoss.isInvulnerableTo`: nothing reaches a wither during its spawn
  // shield. Vanilla exempts OUT_OF_WORLD, which never travels through this sink
  // (`/mobs kill` discards the entity instead).
  if (mob.wither_shield_ticks > 0)
    return out;

  // LivingEntity.hurtServer's invulnerability window. The BYPASSES_COOLDOWN tag
  // is empty in vanilla, so a hit inside the window only lands for the part that
  // exceeds the previous one.
  float landed = damage;
  if (mob.invulnerable_time > kMobInvulnerableThreshold) {
    if (landed <= mob.last_hurt)
      return out;
    landed -= mob.last_hurt;
    mob.last_hurt = damage;
    out.reduced = true;
  } else {
    mob.last_hurt = damage;
    mob.invulnerable_time = kMobInvulnerableTicks;
  }

  // Mob.actuallyHurt: getDamageAfterArmorAbsorb, then the enchantment step (no
  // enchantments here). A type that bypasses armour skips the whole thing.
  // The armour value is the type's attribute plus the wither's phase bonus, so
  // the half-health threshold changes this arithmetic on the same hit that
  // crosses it (see `mob_armor_value`).
  const float after_armor =
      bypasses_armor ? landed : damage_after_absorb(landed, mob_armor_value(mob), 0.0f);
  const float to_health = std::max(after_armor, 0.0f);
  out.accepted = true;
  if (to_health <= 0.0f)
    return out;

  if (attacker_entity_id >= 0) {
    mob.last_attacker_entity_id = attacker_entity_id;
    mob.last_attacker_name.assign(attacker_name);
    mob.killed_by_player = true;
    // LivingEntity.hurtServer's `setLastHurtByMob(attacker)`: the timestamp is
    // what HurtByTargetGoal.canUse watches, and the target selector turns it into
    // a target on the next tick — which is how a player's sword and a /damage
    // both make a mob come looking for them.
    mob.provoked_entity_id = attacker_entity_id;
    mob.provoked_fresh = true;
  }
  mob.health = std::max(mob.health - to_health, 0.0f);
  out.applied = to_health;

  // LivingEntity.knockback.
  if (knockback > 0.0) {
    double dx = knockback_x;
    double dz = knockback_z;
    const double len = std::sqrt(dx * dx + dz * dz);
    if (len > 1.0e-6) {
      dx /= len;
      dz /= len;
    } else {
      dx = 0.0;
      dz = 0.0;
    }
    const double vertical = mob.on_ground ? std::min(0.4, mob.vy / 2.0 + knockback) : mob.vy;
    mob.vx = mob.vx / 2.0 - dx * knockback;
    mob.vz = mob.vz / 2.0 - dz * knockback;
    mob.vy = vertical;
    mob.on_ground = false;
  }

  if (!mob.is_alive()) {
    mob.health = 0.0f;
    out.died = true;
    mob.path = NavPath{};
    mob.has_path_goal = false;
  }
  return out;
}

bool mob_within_player_reach(const Mob& mob, double eye_x, double eye_y, double eye_z,
                             double reach) {
  const double half = mob.half_width();
  const double dx = std::max(0.0, std::abs(eye_x - mob.x) - half);
  const double dz = std::max(0.0, std::abs(eye_z - mob.z) - half);
  const double dy = std::max(0.0, std::max(mob.y - eye_y, eye_y - (mob.y + mob.body_height())));
  return dx * dx + dy * dy + dz * dz < reach * reach;
}

bool mob_in_sweep_range(const Mob& target, const Mob& other) {
  // Player.doSweepAttack: `entity.getBoundingBox().inflate(1.0, 0.25, 1.0)`,
  // with `Level.getEntitiesOfClass` having already kept the boxes that
  // intersect it. AABB.intersects is an overlap on all three axes.
  const double reach_x = target.half_width() + 1.0 + other.half_width();
  const double reach_z = reach_x;
  const double low = target.y - 0.25;
  const double high = target.y + target.body_height() + 0.25;
  if (std::abs(other.x - target.x) > reach_x || std::abs(other.z - target.z) > reach_z)
    return false;
  if (other.y + other.body_height() <= low || other.y >= high)
    return false;
  // `!(this.distanceToSqr(nearby) < 9.0)`: Entity.distanceToSqr measures between
  // the two positions (the feet centres), not between their boxes.
  const double dx = other.x - target.x;
  const double dy = other.y - target.y;
  const double dz = other.z - target.z;
  return dx * dx + dy * dy + dz * dz < 9.0;
}

bool check_despawn(Mob& mob, const MobEnvironment& env) {
  if (mob.type == nullptr)
    return false;
  // `if (level.getDifficulty() == PEACEFUL && !type.isAllowedInPeaceful())
  // discard();` — the server has no difficulty (world/mob_spawn.h documents the
  // same boundary), so this branch is unreachable here.
  //
  // `if (isPersistenceRequired() || requiresCustomPersistence()) {
  // noActionTime = 0; return; }` — nothing in the engine requires custom
  // persistence (no passengers, no leads), so only the flag is left.
  if (mob.persistence_required) {
    mob.no_action_time = 0;
    return false;
  }
  const MobCategoryStats& stats = mob_category_stats(mob.type->category);
  // `removeWhenFarAway(distSqr)`: false for every Animal, true for everything
  // else this engine has (world/mob_table.h's kMobNoFarDespawn).
  const bool removes_when_far = (mob.type->flags & kMobNoFarDespawn) == 0;
  const double despawn_sqr =
      static_cast<double>(stats.despawn_distance) * static_cast<double>(stats.despawn_distance);
  const double no_despawn_sqr = static_cast<double>(stats.no_despawn_distance) *
                                static_cast<double>(stats.no_despawn_distance);
  // `Level.getNearestPlayer(this, -1.0)`: unbounded, spectators excluded. The
  // exact distance is only ever compared against the two radii above, so the
  // scan stops at the first player inside the no-despawn radius: that player
  // settles every branch below (no discard, and the counter reset) whatever the
  // true minimum turns out to be. A mob with players nearby — the ordinary case
  // — costs one iteration instead of all of them.
  double nearest = -1.0;
  bool player_in_no_despawn_range = false;
  for (const MobPlayer& player : env.players) {
    if (player.spectator)
      continue;
    const double dx = player.x - mob.x;
    const double dy = player.y - mob.y;
    const double dz = player.z - mob.z;
    const double d2 = dx * dx + dy * dy + dz * dz;
    if (d2 < no_despawn_sqr) {
      player_in_no_despawn_range = true;
      break;
    }
    if (nearest < 0.0 || d2 < nearest)
      nearest = d2;
  }
  if (player_in_no_despawn_range) {
    // `if (distSqr < NO_DESPAWN_DISTANCE * NO_DESPAWN_DISTANCE) noActionTime = 0;`
    // — the only side effect the near branch has. The idle roll above it still
    // *draws* first: `random.nextInt(800)` is evaluated before the distance
    // test short-circuits, so a caller's stream must see that draw even though
    // its value cannot change the answer.
    if (mob.no_action_time > kMobDespawnTicks && env.unit_random)
      env.unit_random();
    mob.no_action_time = 0;
    return false;
  }
  if (nearest < 0.0)
    return false; // nobody around: `getNearestPlayer` answered null
  if (nearest > despawn_sqr && removes_when_far)
    return true;
  // `noActionTime > 600 && random.nextInt(800) == 0 && distSqr > 32^2 &&
  // removeWhenFarAway(distSqr)` — the draw happens only once the activity
  // counter has run out, and `nextInt(800) == 0` is a uniform 1 in 800.
  if (mob.no_action_time > kMobDespawnTicks && env.unit_random &&
      env.unit_random() < kMobDespawnChance && nearest > no_despawn_sqr && removes_when_far)
    return true;
  return false;
}

// ---- animal feeding + breeding (Phase 4 milestone I) ------------------------

bool mob_eats(const Mob& mob, std::string_view item_name) {
  if (!mob.animal())
    return false;
  const char* mob_name = mob.type->name;
  // Eleven rows, walked in one pass: `Animal.isFood` is one `is(tag)` call per
  // right-click, and the table is already sorted by (mob, item) for a reader.
  for (int i = 0; i < kAnimalFoodCount; ++i) {
    if (std::string_view(kAnimalFoods[i].mob) != mob_name)
      continue;
    if (std::string_view(kAnimalFoods[i].item) == item_name)
      return true;
  }
  return false;
}

// `Animal.getSpeedUpSecondsWhenFeeding(ticksUntilAdult)`: the whole seconds a
// feeding takes off, `(int)((ticksUntilAdult / 20) * 0.1f)`. The integer
// division happens *before* the tenth, which is what keeps the amount below the
// remainder: for `ticksUntilAdult = 24000` that is 120 seconds (2400 ticks),
// and the last feeding before adulthood (`ticksUntilAdult < 20`) is zero.
int animal_speed_up_seconds(int ticks_until_adult) {
  return static_cast<int>(static_cast<float>(ticks_until_adult / 20) * 0.1f);
}

void mob_age_up(Mob& mob, int seconds, bool forced) {
  const int old_age = mob.breed_age;
  int age = old_age + seconds * 20;
  if (age > 0)
    age = 0;
  const int delta = age - old_age;
  mob.breed_age = age;
  if (forced)
    mob.forced_age += delta;
  if (mob.breed_age == 0)
    mob.breed_age = mob.forced_age;
}

MobFeedResult feed_mob(Mob& mob, bool is_food, std::string_view love_cause) {
  MobFeedResult out;
  if (!is_food || !mob.animal())
    return out;
  // `Animal.mobInteract` reads the age *once*, before either branch.
  const int age = mob.breed_age;
  // The adult branch is `age == 0 && this.canFallInLove()`: an adult that is
  // already in love is not fed a second time (the stack is not spent and no
  // second heart burst is broadcast), and an adult on its post-breeding
  // cooldown has an age that is not 0, so it is refused here too.
  if (age == 0 && mob.can_fall_in_love()) {
    mob.in_love = kAnimalInLoveTicks;
    // `setInLove(player)`: the cause is recorded beside the countdown, and only
    // the adult branch has one to record.
    mob.love_cause = std::string(love_cause);
    out.consumed = true;
    out.fell_in_love = true;
    return out;
  }
  // The baby branch: `canAgeUp()` — a baby fed its food grows a tenth of the
  // way up, and a baby whose age has stalled below the feeding floor (see
  // `animal_speed_up_seconds`) is still fed: vanilla spends the item and adds
  // nothing. There is no love to clear here (`Animal.aiStep` clears it for any
  // age that is not 0).
  if (mob.can_age_up()) {
    mob_age_up(mob, animal_speed_up_seconds(-age), /*forced=*/true);
    out.consumed = true;
    out.aged_up = true;
  }
  return out;
}

namespace {

// Entity id -> slot in the population, built once per `tick_mobs` call and
// handed to the goals that resolve an id every tick.
//
// Every in-love cow resolves its mate and every baby resolves its parent, which
// used to be a scan of the whole population per mob per tick — O(mobs^2) on a
// full server, and the `follow` scene of `bench/mob_tick_probe.cpp` is where it
// shows (400 mobs, half of them babies: 80,000 id comparisons a tick). The
// table is open-addressed over two flat vectors, cleared by `build` rather than
// destroyed, so a tick allocates nothing after the first; a lookup is one
// `* 2654435761 >> 16` and a short probe chain.
//
// The population's order is stable for the whole tick — `tick_mobs` only erases
// the dead at the end — so a stored slot stays valid until the next build. An
// id of 0 is stored as 1 so an empty slot is a zero word; a duplicate id keeps
// the first slot, which is the one the old linear scan would have returned.
struct MobIndex {
  std::vector<int32_t> keys;  // entity_id + 1; 0 = empty
  std::vector<int32_t> slots; // index into the population
  size_t mask = 0;

  static size_t home(int32_t entity_id, size_t mask) {
    return (static_cast<size_t>(static_cast<uint32_t>(entity_id) * 2654435761u) >> 16) & mask;
  }

  void build(const std::vector<Mob>& mobs) {
    size_t capacity = 16;
    while (capacity < mobs.size() * 2)
      capacity <<= 1;
    keys.assign(capacity, 0);
    slots.assign(capacity, 0);
    mask = capacity - 1;
    for (size_t i = 0; i < mobs.size(); ++i) {
      const int32_t entity_id = mobs[i].entity_id;
      if (entity_id < 0)
        continue;
      size_t at = home(entity_id, mask);
      while (keys[at] != 0)
        at = (at + 1) & mask;
      keys[at] = entity_id + 1;
      slots[at] = static_cast<int32_t>(i);
    }
  }

  Mob* find(std::vector<Mob>& mobs, int32_t entity_id) const {
    if (entity_id < 0 || keys.empty())
      return nullptr;
    for (size_t at = home(entity_id, mask);; at = (at + 1) & mask) {
      if (keys[at] == 0)
        return nullptr;
      if (keys[at] == entity_id + 1)
        return &mobs[static_cast<size_t>(slots[at])];
    }
  }
};

// The population bucketed by position, rebuilt once per tick and used by the
// two animal scans (`BreedGoal.getFreePartner`, `FollowParentGoal.canUse`).
//
// Both are `level.getEntitiesOfClass` box queries in vanilla, and the level
// answers those from its *sectioned* entity storage — a query reads the few
// 16-block sections its box touches, never the whole population. This engine's
// scans read every mob, which is what a pen of babies and parents costs: the
// follow scene in `bench/mob_tick_probe.cpp` (200 babies, 200 adults, one pen)
// spends about two microseconds per mob tick, almost all of it re-scanning the
// population for a parent every other tick (`FollowParentGoal` drops the parent
// whenever it walks inside three blocks, then has to find it again). The grid
// answers the same box query from the 8-block buckets it touches.
//
// Two properties keep it honest:
//   * the *candidate order* does not matter, because both scans break ties the
//     way the old loop did — a later population index wins an equal distance —
//     so the winner is identical whether the candidates arrive in population
//     order or bucket order;
//   * it is built from the positions at the start of the tick, and mobs move
//     while the tick runs. A candidate that crosses into the query radius after
//     the build is found on the next tick instead of this one, which is one
//     tick of latency on an AI acquisition; the query box is widened a block
//     for exactly that. Below `kMinPopulation` the grid is not built at all:
//     the linear scan is cheaper than the rebuild, and it is the exact
//     live-position scan the digest corpus pins.
struct MobGrid {
  static constexpr int kShift = 3; // 8-block buckets
  static constexpr int kMaxCells = 1 << 14;
  // Where the grid stops losing and starts winning, measured on the tick
  // probe's `follow` scene (babies and parents three blocks apart) as a
  // same-source A/B: the same translation unit built with this constant at 0
  // and at a million, alternating one process per arm, paired repeats per
  // population.
  //
  //   animals        32     64     80     96    128
  //   grid / linear  1.14   1.03   0.94   0.93   0.89   (lower is better)
  //   pairs won      2/9   9/20  28/42  18/29   7/9
  //
  // At 32 the grid is 14 % slower and wins two pairs in nine: that population
  // is shorter than the ~50 candidates a ten-block query returns in a landscape
  // spaced three blocks apart, so the rebuild is paid and nothing is saved. At
  // 64 it still loses in most pairs (nine of twenty win). From 80 the pairs
  // favour it, which is where the switch sits. 80 and 96 read the same within
  // noise (0.94 against 0.93), so the value between them does not matter; the
  // first population whose pairs reliably favour the grid is the honest place
  // for it. Repeating 80 on a quiet box (load 5-6) gave 0.92 over eleven pairs
  // and 1.05 over the next eleven -- the coin-flip the pooled 28 of 42
  // describes. At 128 the win is 11 % and unambiguous (seven of nine).
  //
  // `[[maybe_unused]]` is for the A/B's scan build (`PLUSMC_MOB_GRID_OFF`),
  // which compiles this file without ever reading the number.
  [[maybe_unused]] static constexpr size_t kMinPopulation = 80;

  // The A/B arm is a build, not a setting: `PLUSMC_MOB_GRID_OFF` (see
  // `mob_grid_enabled_for` below) compiles this one file for the population
  // scan, and `ctest -R mob-grid-ab` links that object into a second digest
  // binary. Nothing in the tick path reads the environment, so a deployed
  // server cannot be talked out of the grid.

  std::vector<int32_t> starts; // bucket -> first slot, plus one past the last
  std::vector<int32_t> items;  // population indices, grouped by bucket
  std::vector<int32_t> cursor; // scratch for the counting sort
  int min_cx = 0, min_cz = 0, nx = 0, nz = 0;
  bool gridded = false;

  static int cell_of(double v) {
    return static_cast<int>(std::floor(v * (1.0 / static_cast<double>(1 << kShift))));
  }

  size_t index_of(int cx, int cz) const {
    return static_cast<size_t>(cx - min_cx) * static_cast<size_t>(nz) +
           static_cast<size_t>(cz - min_cz);
  }

  void build(const std::vector<Mob>& mobs) {
    gridded = false;
    if (!mob_grid_enabled_for(mobs.size()))
      return;
    int lo_x = std::numeric_limits<int>::max();
    int hi_x = std::numeric_limits<int>::min();
    int lo_z = std::numeric_limits<int>::max();
    int hi_z = std::numeric_limits<int>::min();
    for (const Mob& mob : mobs) {
      const int cx = cell_of(mob.x);
      const int cz = cell_of(mob.z);
      lo_x = std::min(lo_x, cx);
      hi_x = std::max(hi_x, cx);
      lo_z = std::min(lo_z, cz);
      hi_z = std::max(hi_z, cz);
    }
    const long long span_x = static_cast<long long>(hi_x) - lo_x + 1;
    const long long span_z = static_cast<long long>(hi_z) - lo_z + 1;
    // A population spread across a whole world is not worth the memory: keep the
    // scan (the server only ticks mobs near players, so this is the rare case).
    if (span_x * span_z > kMaxCells)
      return;
    min_cx = lo_x;
    min_cz = lo_z;
    nx = static_cast<int>(span_x);
    nz = static_cast<int>(span_z);
    const size_t cells = static_cast<size_t>(nx) * static_cast<size_t>(nz);
    starts.assign(cells + 1, 0);
    for (const Mob& mob : mobs)
      ++starts[index_of(cell_of(mob.x), cell_of(mob.z)) + 1];
    for (size_t i = 1; i <= cells; ++i)
      starts[i] += starts[i - 1];
    cursor.assign(starts.begin(), starts.end() - 1);
    items.resize(mobs.size());
    for (size_t i = 0; i < mobs.size(); ++i)
      items[static_cast<size_t>(cursor[index_of(cell_of(mobs[i].x), cell_of(mobs[i].z))]++)] =
          static_cast<int32_t>(i);
    gridded = true;
  }

  // Every mob whose bucket lies within `radius` blocks of (x, z), or the whole
  // population when the grid was not built. The caller applies its own box
  // test; the extra candidates a bucket adds are cheap and harmless.
  template <class F>
  void for_each_near(double x, double z, double radius, std::vector<Mob>& mobs, F&& f) const {
    if (!gridded) {
      for (size_t i = 0; i < mobs.size(); ++i)
        f(i, mobs[i]);
      return;
    }
    const int cx0 = std::max(min_cx, cell_of(x - radius));
    const int cx1 = std::min(min_cx + nx - 1, cell_of(x + radius));
    const int cz0 = std::max(min_cz, cell_of(z - radius));
    const int cz1 = std::min(min_cz + nz - 1, cell_of(z + radius));
    for (int cx = cx0; cx <= cx1; ++cx)
      for (int cz = cz0; cz <= cz1; ++cz) {
        const size_t bucket = index_of(cx, cz);
        for (int32_t at = starts[bucket]; at < starts[bucket + 1]; ++at) {
          const size_t i = static_cast<size_t>(items[static_cast<size_t>(at)]);
          f(i, mobs[i]);
        }
      }
  }
};

// `Mob.serverAiStep` and the goal selector's cadence: the full selector tick is
// `(tickCount + id) % 2 == 0 || tickCount <= 1`, and the goals that do not
// `requiresUpdateEveryTick` (BreedGoal among them) only get their `canUse` and
// `tick` on those ticks. The engine's goal step runs every tick, so this is the
// gate the *observable* part of the cadence uses: `love_time` advances and a
// partner is looked for only on the vanilla parity.
bool goal_selector_tick(const Mob& mob) {
  return ((mob.age + mob.entity_id) % 2) == 0 || mob.age <= 1;
}

// `BreedGoal`, for one mob: acquire the nearest same-species animal that can
// mate inside the eight-block partner range, walk to it, and breed once the
// goal has run its thirty goal ticks (sixty real ticks) inside three blocks.
//
// Both parents are mutated here — `finalizeSpawnChildFromBreeding` sets *their*
// age to 6000 and clears *both* loves — and the child is reported rather than
// created: the module owns no population, so the caller makes the entity (see
// `MobBirth`).
void breed_step(Mob& mob, std::vector<Mob>& mobs, const MobIndex& index, const MobGrid& grid,
                const MobEnvironment& env, bool goal_tick, MobTickReport* report, bool* walk,
                double* dir_x, double* dir_z) {
  *walk = false;
  *dir_x = 0.0;
  *dir_z = 0.0;
  Mob* partner = index.find(mobs, mob.mate_entity_id);
  // `BreedGoal.canContinueToUse`: the partner must still be alive and in love
  // (this engine has no panic state, so the decomp's `!isPanicking()` is
  // vacuously true), and the goal has not run its raw sixty goal ticks.
  if (partner != nullptr && !(partner->is_alive() && mob.can_mate_with(*partner) &&
                              mob.love_time < kAnimalLoveContinueTime))
    partner = nullptr;
  if (partner == nullptr) {
    // `GoalSelector.tick`'s cleanup runs `stop()` on a goal whose
    // `canContinueToUse` said no — partner cleared, loveTime back to 0.
    mob.mate_entity_id = -1;
    mob.love_time = 0;
    if (!goal_tick)
      return; // `canUse` is only consulted on a full selector tick
    // `BreedGoal.canUse` → `getFreePartner`: the nearest animal of the same
    // class in `getBoundingBox().inflate(8.0)` that this mob `canMate`s. The
    // partner conditions are `forNonCombat().range(8.0)`, so the search is an
    // eight-block distance test (there is no line-of-sight condition to
    // evaluate — `ignoreLineOfSight`).
    double best = kAnimalBreedRange * kAnimalBreedRange;
    size_t best_index = 0;
    bool have_partner = false;
    // Same query, fewer candidates: the grid's buckets (or the whole population
    // when it was not built). Its positions are the tick's start, so the query
    // is a block wider than the range; the box test below is unchanged. Ties go
    // to the later population index, which is what the old `<=` scan did.
    grid.for_each_near(mob.x, mob.z, kAnimalBreedRange + 1.0, mobs, [&](size_t i, Mob& other) {
      if (other.removed || !mob.can_mate_with(other))
        return;
      const double dx = other.x - mob.x;
      const double dy = other.y - mob.y;
      const double dz = other.z - mob.z;
      const double d2 = dx * dx + dy * dy + dz * dz;
      if (d2 > best)
        return;
      if (have_partner && d2 == best && i < best_index)
        return;
      best = d2;
      best_index = i;
      have_partner = true;
      partner = &other;
    });
    if (partner == nullptr)
      return;
    mob.mate_entity_id = partner->entity_id;
  }
  // `BreedGoal.tick`: look at the partner and `navigation.moveTo(partner, 1.0)`
  // — every goal tick, so the path follower is asked to re-plan on its own
  // cadence (the engine's one path follower). A mob already inside the
  // waypoint reach stands still and turns.
  follow_path(mob, env, partner->x, partner->y, partner->z, /*replan=*/true, /*speed=*/1.0, walk,
              dir_x, dir_z);
  if (!*walk)
    mob.yaw = facing_yaw(partner->x - mob.x, partner->z - mob.z);
  if (goal_tick)
    ++mob.love_time;
  const double dx = partner->x - mob.x;
  const double dy = partner->y - mob.y;
  const double dz = partner->z - mob.z;
  if (mob.love_time >= kAnimalBreedLoveTime &&
      dx * dx + dy * dy + dz * dz < kAnimalBreedDistanceSqr) {
    // `Animal.spawnChildFromBreeding` + `finalizeSpawnChildFromBreeding`. The
    // child is `setBaby(true)` and `snapTo`'d at the initiator's own position
    // (`this` is the mob whose goal bred); the caller creates it.
    MobBirth birth;
    birth.parent_entity_id = mob.entity_id;
    birth.partner_entity_id = partner->entity_id;
    birth.type = mob.type;
    // `finalizeSpawnChildFromBreeding`'s cause: this mob's own love cause, or
    // the partner's when only the partner was fed by hand.
    birth.love_cause = mob.love_cause.empty() ? partner->love_cause : mob.love_cause;
    birth.x = mob.x;
    birth.y = mob.y;
    birth.z = mob.z;
    birth.yaw = 0.0f;
    report->births.push_back(birth);
    mob.breed_age = kAnimalParentAgeAfterBreeding;
    partner->breed_age = kAnimalParentAgeAfterBreeding;
    // `resetLove()`: the countdown and the cause go together.
    mob.in_love = 0;
    partner->in_love = 0;
    mob.love_cause.clear();
    partner->love_cause.clear();
    mob.mate_entity_id = -1;
    mob.love_time = 0;
    partner->mate_entity_id = -1;
    partner->love_time = 0;
  }
}

// `FollowParentGoal`, for one mob: a baby looks for the nearest *adult* of its
// own class whose box touches its own inflated by (8, 4, 8), and walks to it —
// refusing the goal while the nearest of them is still inside three blocks, and
// letting a parent go again past sixteen. The parent is held by entity id across
// ticks, and the goal asks the navigation for a path once every five of its own
// ticks (`adjustedTickDelay(10)`); the one path follower walks the path it was
// given in between.
//
// Returns whether the goal is running — and so owns the movement this tick. A
// baby with no parent in range falls through to the stroll, which is what
// `GoalSelector` does when this goal's `canUse` says no.
bool follow_parent_step(Mob& mob, std::vector<Mob>& mobs, const MobIndex& index,
                        const MobGrid& grid, const MobEnvironment& env, bool goal_tick, bool* walk,
                        double* dir_x, double* dir_z) {
  *walk = false;
  *dir_x = 0.0;
  *dir_z = 0.0;
  Mob* parent = index.find(mobs, mob.parent_entity_id);
  if (parent != nullptr) {
    // `canContinueToUse`: a baby (the grow-up crossing is what ends it), a
    // parent that is still alive, and a gap that has neither closed under three
    // blocks nor opened past sixteen.
    const double dx = parent->x - mob.x;
    const double dy = parent->y - mob.y;
    const double dz = parent->z - mob.z;
    const double d2 = dx * dx + dy * dy + dz * dz;
    if (!(mob.baby() && parent->is_alive() && !(d2 < kAnimalFollowMinDistanceSqr) &&
          !(d2 > kAnimalFollowMaxDistanceSqr)))
      parent = nullptr; // `GoalSelector.tick`'s cleanup -> `stop()`
  }
  if (parent == nullptr) {
    // `stop()`: the parent is dropped, and the goal can only come back through
    // `canUse`, which is a question for the selector's full ticks.
    mob.parent_entity_id = -1;
    mob.follow_repath = 0;
    if (!goal_tick || !mob.baby())
      return false;
    // `canUse`: the same-class adults the inflated box touches (the query is a
    // *box* query, so `AABB.intersects` — strict on every axis — is the test),
    // the nearest of those by `distanceToSqr`, and a nearest that is still
    // closer than three blocks refuses the goal outright. A candidate of the
    // class that is itself a baby is skipped (`parent.getAge() < 0`), which is
    // also what skips the baby's own box: it intersects its own query.
    const auto in_box = [&mob](const Mob& other) {
      const double reach = kAnimalFollowHorizontalRange + mob.half_width() + other.half_width();
      if (std::abs(other.x - mob.x) >= reach || std::abs(other.z - mob.z) >= reach)
        return false;
      return other.y + other.body_height() > mob.y - kAnimalFollowVerticalRange &&
             other.y < mob.y + mob.body_height() + kAnimalFollowVerticalRange;
    };
    double closest = std::numeric_limits<double>::max();
    size_t best_index = 0;
    bool have_parent = false;
    // The same box query through the grid (or the whole population when it was
    // not built). The radius covers the box's horizontal reach plus the widen
    // the tick-start positions need; `in_box` still decides. `> closestDistSqr`
    // skipped, so a tie kept the *later* population index — here `i`.
    grid.for_each_near(mob.x, mob.z, kAnimalFollowHorizontalRange + 2.0, mobs,
                       [&](size_t i, Mob& other) {
                         // A dead or removed entity is not in the level's entity
                         // list any more (`LivingEntity.die` removes it), so the
                         // query cannot return it.
                         if (other.removed || !other.is_alive() || other.type != mob.type ||
                             other.baby() || !in_box(other))
                           return;
                         const double dx = other.x - mob.x;
                         const double dy = other.y - mob.y;
                         const double dz = other.z - mob.z;
                         const double d2 = dx * dx + dy * dy + dz * dz;
                         if (d2 > closest)
                           return;
                         if (have_parent && d2 == closest && i < best_index)
                           return;
                         closest = d2;
                         best_index = i;
                         have_parent = true;
                         parent = &other;
                       });
    if (parent == nullptr || closest < kAnimalFollowMinDistanceSqr)
      return false;
    // `start()`: the countdown starts at zero, so the run's first tick plans at
    // once.
    mob.parent_entity_id = parent->entity_id;
    mob.follow_repath = 0;
  }
  // `tick()`: `if (--this.timeToRecalcPath > 0) return; this.timeToRecalcPath =
  // this.adjustedTickDelay(10); this.animal.getNavigation().moveTo(parent,
  // this.speedModifier);`. The countdown moves only on the goal's own ticks,
  // and the early return skips only the *re-plan* — the path follower walks the
  // path every tick either way. The speed modifier is the goal registration's
  // own (1.1 for the pig, sheep and chicken, 1.25 for the cow).
  bool replan = false;
  if (goal_tick) {
    --mob.follow_repath;
    if (mob.follow_repath <= 0) {
      mob.follow_repath = kAnimalFollowRepathTicks;
      replan = true;
    }
  }
  mob.goal = MobGoal::FollowParent;
  follow_path(mob, env, parent->x, parent->y, parent->z, replan,
              static_cast<double>(mob.type->follow_parent_speed), walk, dir_x, dir_z);
  return true;
}

} // namespace

// Whether `population` is large enough for `MobGrid` to be built, as this
// *build* decided: the switch above, or never when the A/B arm asked for the
// population scan.
//
// `PLUSMC_MOB_GRID_OFF` is the scan arm of `ctest -R mob-grid-ab`, and it is a
// compile-time define on purpose. The grid is a production tick path, and a
// runtime arm there would be a switch an operator (or a stray variable in a
// service unit) could turn off on a live server. Instead CMake compiles this
// one translation unit a second time with the define and links that object into
// a second copy of the digest test. Both arms report which one they are, and the
// scan arm exists only in that test binary: a deployed server has no switch to
// turn the grid off with.
bool mob_grid_enabled_for(size_t population) {
#ifdef PLUSMC_MOB_GRID_OFF
  (void)population;
  return false;
#else
  return population >= MobGrid::kMinPopulation;
#endif
}

namespace {

// The dragon is 16 wide and flies, so its box is sampled at eight corners plus
// the mid-height ring instead of a path node's clearance. A null `is_solid` is
// the void world, where every position is free.
bool dragon_box_free(const MobEnvironment& env, double x, double y, double z, double half,
                     double height) {
  if (!env.is_solid)
    return true;
  constexpr double kInset = 1.0e-4;
  for (int iy = 0; iy <= 2; ++iy) {
    const double sy = y + height * 0.5 * static_cast<double>(iy) - (iy == 2 ? kInset : 0.0);
    for (int ix = 0; ix < 2; ++ix) {
      const double sx = x + (ix == 0 ? -half : half) * (1.0 - kInset);
      for (int iz = 0; iz < 2; ++iz) {
        const double sz = z + (iz == 0 ? -half : half) * (1.0 - kInset);
        if (env.is_solid(static_cast<int>(std::floor(sx)), static_cast<int>(std::floor(sy)),
                         static_cast<int>(std::floor(sz))))
          return false;
      }
    }
  }
  return true;
}

// One step toward a point, with a full-box test and an axis fallback: the
// dragon's flight has no gravity and no path graph, only a destination. The
// axis fallback is what lets a perch descent slide along a wall instead of
// pinning the whole move.
void dragon_fly_toward(Mob& mob, const MobEnvironment& env, double target_x, double target_y,
                       double target_z, double speed) {
  const double dx = target_x - mob.x;
  const double dy = target_y - mob.y;
  const double dz = target_z - mob.z;
  const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (distance < 1.0e-6)
    return;
  const double step = std::min(speed, distance);
  const double nx = mob.x + dx / distance * step;
  const double ny = mob.y + dy / distance * step;
  const double nz = mob.z + dz / distance * step;
  // A steering box, not the 16 x 8 hitbox: the hitbox's corners reach eight
  // blocks out and snag the first hillside, which pins a dragon that spawned
  // beside one. Vanilla's flight steers clear of terrain with a much smaller
  // collision shape; this is that shape.
  constexpr double kFlightHalf = 2.0;
  constexpr double kFlightHeight = 3.0;
  const double half = kFlightHalf;
  const double height = kFlightHeight;
  const auto free_at = [&](double x, double y, double z) {
    return dragon_box_free(env, x, y, z, half, height);
  };
  // A dragon summoned on the ground starts with its steering box over the
  // floor. Vanilla's dragon is not pinned by that overlap — it climbs out — so
  // a box that is already blocked does not veto the move. This is also what
  // lets a perch descent slide along a wall instead of pinning the whole move.
  const bool was_free = free_at(mob.x, mob.y, mob.z);
  const auto can_move = [&](double x, double y, double z) { return !was_free || free_at(x, y, z); };
  if (can_move(nx, ny, nz)) {
    mob.x = nx;
    mob.y = ny;
    mob.z = nz;
  } else if (can_move(nx, mob.y, mob.z)) {
    mob.x = nx;
  } else if (can_move(mob.x, ny, mob.z)) {
    mob.y = ny;
  } else if (can_move(mob.x, mob.y, nz)) {
    mob.z = nz;
  } else if (target_y > mob.y) {
    // A dragon can spawn with its steering box inside a hillside. Vanilla's
    // takeoff clears terrain; this is that escape, climbing straight up until
    // the box is out of the block it started in.
    mob.y += std::min(0.5, target_y - mob.y);
  }
  mob.on_ground = false;
}

// `EnderDragon`'s phase manager, in this engine's terms. The dragon circles the
// target column at `kDragonCircleHeight`, dives every `kDragonCircleTicks`, and
// at half health drops to its perch for `kDragonPerchTicks` before taking off
// again. It has no melee attack; the crystals (session-owned) do the healing and
// the bar follows the health like every other boss.
void dragon_step(Mob& mob, const MobEnvironment& env, const MobPlayer* target) {
  ++mob.dragon_phase_ticks;
  if (mob.dragon_perch_cooldown > 0)
    --mob.dragon_perch_cooldown;
  const double perch_y = mob.dragon_perch_y + 2.0;
  switch (mob.dragon_phase) {
  case 2: // perched
    dragon_fly_toward(mob, env, mob.dragon_perch_x, perch_y, mob.dragon_perch_z, 0.5);
    if (mob.dragon_phase_ticks >= kDragonPerchTicks) {
      mob.dragon_phase = 3;
      mob.dragon_phase_ticks = 0;
    }
    break;
  case 3: // takeoff
    dragon_fly_toward(mob, env, mob.dragon_perch_x, mob.dragon_perch_y + kDragonCircleHeight + 6.0,
                      mob.dragon_perch_z, 0.5);
    if (mob.dragon_phase_ticks >= kDragonTakeoffTicks) {
      mob.dragon_phase = 0;
      mob.dragon_phase_ticks = 0;
      mob.dragon_perch_cooldown = kDragonPerchCooldownTicks;
    }
    break;
  case 1: // dive at the target
    if (target != nullptr) {
      dragon_fly_toward(mob, env, target->x, target->y + target->height * 0.5, target->z, 0.9);
      mob.yaw = facing_yaw(target->x - mob.x, target->z - mob.z);
    }
    if (target == nullptr || mob.dragon_phase_ticks >= kDragonChargeTicks) {
      mob.dragon_phase = 0;
      mob.dragon_phase_ticks = 0;
    }
    break;
  default: { // circling
    if (mob.health <= mob.type->max_health * 0.5f && mob.dragon_perch_cooldown == 0) {
      mob.dragon_phase = 2;
      mob.dragon_phase_ticks = 0;
      dragon_fly_toward(mob, env, mob.dragon_perch_x, perch_y, mob.dragon_perch_z, 0.5);
      break;
    }
    const double center_x = target != nullptr ? target->x : mob.dragon_perch_x;
    const double center_z = target != nullptr ? target->z : mob.dragon_perch_z;
    const double angle = static_cast<double>(mob.dragon_phase_ticks) * 0.05;
    const double tx = center_x + std::cos(angle) * kDragonCircleRadius;
    const double tz = center_z + std::sin(angle) * kDragonCircleRadius;
    const double ty = std::max(mob.dragon_perch_y + kDragonCircleHeight,
                               target != nullptr ? target->y + 6.0 : 0.0);
    dragon_fly_toward(mob, env, tx, ty, tz, 0.35);
    mob.yaw = facing_yaw(tx - mob.x, tz - mob.z);
    if (mob.dragon_phase_ticks >= kDragonCircleTicks) {
      mob.dragon_phase = 1;
      mob.dragon_phase_ticks = 0;
    }
    break;
  }
  }
  mob.goal = MobGoal::RangedAttack;
}

} // namespace

MobTickReport tick_mobs(std::vector<Mob>& mobs, const MobEnvironment& env) {
  MobTickReport report;
  // LivingEntity.die runs on the server's tick, not in the packet handler that
  // landed the last hit: a mob whose health reached zero on another thread is
  // therefore picked up here on the very next tick, and both paths share this.
  const auto record_death = [&report](Mob& mob) {
    MobDeath death;
    death.entity_id = mob.entity_id;
    death.type = mob.type;
    death.x = mob.x;
    death.y = mob.y;
    death.z = mob.z;
    death.killed_by_player = mob.killed_by_player;
    death.killer_entity_id = mob.last_attacker_entity_id;
    death.killer_name = mob.last_attacker_name;
    report.deaths.push_back(std::move(death));
    mob.removed = true;
  };
  // One id table for the whole tick: every in-love cow and every baby resolves
  // a mate or a parent, and this makes each of those a probe instead of a scan
  // of the population (see `MobIndex`). The population's order does not change
  // inside this loop, so the slots stay valid.
  MobIndex index;
  index.build(mobs);
  // ...and one position grid for the animal scans (see `MobGrid`): the breed
  // partner and the follow parent are box queries in vanilla, not population
  // scans, and this is what makes them box queries here too.
  MobGrid grid;
  grid.build(mobs);
  for (Mob& mob : mobs) {
    if (mob.type == nullptr || mob.removed)
      continue;
    if (!mob.is_alive()) {
      record_death(mob);
      continue;
    }
    // `ServerLevel`'s entity forEach calls `checkDespawn` *before* `entity.tick`,
    // so a mob that is gone is gone before it can move, hit or catch fire.
    if (check_despawn(mob, env)) {
      report.despawned.push_back(mob.entity_id);
      mob.removed = true;
      continue;
    }
    const double before_x = mob.x, before_y = mob.y, before_z = mob.z;
    const float before_yaw = mob.yaw;
    const bool was_on_fire = mob.burning.on_fire();
    const bool was_glowing = mob.glowing;
    ++mob.age;
    // `Mob.serverAiStep`: the activity counter the slow despawn branch reads.
    ++mob.no_action_time;
    // Entity.baseTick's fire block: the countdown, and the 1.0 `minecraft:on_fire`
    // damage every 20 ticks of it. A mob's `getFireImmuneTicks` is 0, so unlike a
    // player it has no grace period at all before it catches — and a fire-immune
    // type clears instead of counting down, because `Burning::tick` is
    // `Entity.isInvulnerableTo`'s `IS_FIRE` refusal in countdown form.
    if (mob.burning.tick(mob.type->fire_immune)) {
      // `minecraft:on_fire` is in DamageTypeTags.BYPASSES_ARMOR.
      if (hurt_mob(mob, 1.0f, -1, {}, 0.0, 0.0, 0.0, /*bypasses_armor=*/true).accepted)
        report.hurt.push_back(mob.entity_id);
      if (!mob.is_alive()) {
        record_death(mob);
        continue;
      }
    }
    // `Entity.baseTick`'s `checkBelowWorld()`, right after the fire block: an
    // entity whose feet fall more than 64 blocks under the dimension's min
    // build height is taken by `onBelowWorld()` -> `discard()` — the client is
    // told to forget it and nothing else follows (no death event, no loot, no
    // experience), exactly like `checkDespawn`'s discard above. The overworld's
    // min build height is -64, so a mob falling through the void goes at
    // y = -128 instead of falling forever.
    if (mob.y < static_cast<double>(env.min_y - 64)) {
      report.despawned.push_back(mob.entity_id);
      mob.removed = true;
      continue;
    }
    if (mob.invulnerable_time > 0.0f)
      mob.invulnerable_time = std::max(0.0f, mob.invulnerable_time - 1.0f);
    if (mob.attack_cooldown > 0)
      --mob.attack_cooldown;
    if (mob.wither_shot_cooldown > 0)
      --mob.wither_shot_cooldown;
    if (mob.path_cooldown > 0)
      --mob.path_cooldown;
    if (mob.target_scan_cooldown > 0)
      --mob.target_scan_cooldown;
    // `LivingEntity.tick` -> `tickEffects()`, on the same clock as the rest of
    // the living tick: the lasting effects age here and the one bit the wire
    // shows (the glowing flag) is refreshed from the set the tick just pruned.
    mob_tick_effects(mob);

    // --- the wither's spawn shield (`WitherBoss.customServerAiStep`) ---
    // 220 ticks of refusing damage while the boss bar fills from 1 to full, and
    // a power-7 blast on the tick it ends. The `continue` is deliberate: a
    // shielding wither neither scans for a target, walks nor swings, and the
    // tick's own bookkeeping below (death, flags, syncs) has nothing to say
    // about a mob that did not move and cannot be hurt.
    if (mob_is_wither(mob) && mob.wither_shield_ticks > 0) {
      --mob.wither_shield_ticks;
      const float max_health = mob.type->max_health;
      mob.health = 1.0f + (max_health - 1.0f) *
                              static_cast<float>(kWitherShieldTicks - mob.wither_shield_ticks) /
                              static_cast<float>(kWitherShieldTicks);
      if (mob.wither_shield_ticks == 0) {
        mob.health = max_health;
        MobExplosion blast;
        blast.entity_id = mob.entity_id;
        blast.x = mob.x;
        blast.y = mob.y;
        blast.z = mob.z;
        blast.power = kWitherShieldExplosionPower;
        report.explosions.push_back(blast);
      }
      continue;
    }

    // --- `Villager.customServerAiStep`'s merchant block (milestone L) ---
    // The 40-tick countdown `rewardTradeXp` arms. It only runs while nobody is
    // trading (`!this.isTrading()`, which is `tradingPlayer == null`), so a
    // player who leaves the trade screen open keeps the villager at its old
    // level; at zero `increaseMerchantCareer` raises the level and appends the
    // new level's offers. The regeneration the same branch adds is a real
    // effect on the mob; the raid/gossip lines above it have no state here, and
    // the `lastTradedPlayer` branch below is what `villager_traded` reports.
    if (mob.is_villager()) {
      if (mob.villager.trading_player < 0 && mob.villager.update_merchant_timer > 0) {
        --mob.villager.update_merchant_timer;
        if (mob.villager.update_merchant_timer <= 0) {
          if (mob.villager.increase_level_on_update) {
            villager_increase_career(&mob.villager, mob.entity_id);
            mob.villager.increase_level_on_update = false;
            report.villager_leveled.push_back(mob.entity_id);
          }
          // `addEffect(new MobEffectInstance(REGENERATION, 200, 0))`: ten
          // seconds at level one, on the mob's own effect set.
          if (const GeneratedMobEffect* regeneration =
                  mob_effect_by_name("minecraft:regeneration")) {
            MobEffectInstance instance;
            instance.effect = mob_effect_id_of(*regeneration);
            instance.duration = 200;
            mob_add_effect(mob, instance);
          }
        }
      }
      // `if (this.lastTradedPlayer != null)`: the trade's reputation event
      // (unmodelled) and a happy-villager burst for every client tracking the
      // villager, then the field is cleared — so each trade raises exactly one
      // event, on the first tick after it.
      if (mob.villager.last_traded_player >= 0) {
        mob.villager.last_traded_player = -1;
        report.villager_traded.push_back(mob.entity_id);
      }
    }

    // --- the animal half of `LivingEntity.aiStep` (milestone I) ---
    // `AgeableMob.aiStep`'s server branch: a baby grows a tick, and an adult's
    // post-breeding age counts down. Then `Animal.aiStep`/`customServerAiStep`:
    // love does not survive an age that is not 0 — a baby and a parent on its
    // cooldown both clear it — and a love that does lasts one tick less.
    if (mob.animal()) {
      const bool was_baby = mob.baby();
      if (mob.can_age_up())
        ++mob.breed_age;
      else if (mob.breed_age > 0)
        --mob.breed_age;
      if (mob.breed_age != 0)
        mob.in_love = 0;
      if (mob.in_love > 0)
        --mob.in_love;
      // `AgeableMob.setAge` flips `DATA_BABY_ID` exactly on the crossing.
      if (was_baby && !mob.baby())
        report.grew_up.push_back(mob.entity_id);
    }

    // --- the target selector (HurtByTargetGoal, then the nearest-target
    // scan) before the goals that read the target ---
    update_target(mob, env);

    // --- the goal ---
    bool walk = false;
    double dir_x = 0.0, dir_z = 0.0;
    const MobPlayer* target = find_player(env, mob.target_entity_id);
    const bool dragon = mob_is_dragon(mob);
    if (dragon) {
      dragon_step(mob, env, target);
    } else if (mob_is_wither(mob) && target != nullptr) {
      // `WitherBoss.customServerAiStep`'s ranged branch: inside 40 blocks with
      // sight it holds still, faces the target and looses a skull on the 40-tick
      // cadence; outside that it closes in. It has no melee goal at all.
      const double dx = target->x - mob.x;
      const double dz = target->z - mob.z;
      const double horizontal = std::sqrt(dx * dx + dz * dz);
      const double distance_sqr = perceived_distance_sqr(mob, *target);
      if (mob.target_visible && distance_sqr <= kWitherSkullRange * kWitherSkullRange) {
        mob.goal = MobGoal::RangedAttack;
        mob.yaw = facing_yaw(dx, dz);
        mob.path = NavPath{};
        mob.has_path_goal = false;
        if (mob.wither_shot_cooldown <= 0) {
          mob.wither_shot_cooldown = kWitherSkullIntervalTicks;
          const double aim_rise =
              (target->y + static_cast<double>(target->height) * 0.5) - mob_eye_y(mob);
          MobShot shot;
          shot.kind = MobShotKind::kWitherSkull;
          shot.mob_entity_id = mob.entity_id;
          shot.x = mob.x;
          shot.y = mob_eye_y(mob);
          shot.z = mob.z;
          shot.yaw = facing_yaw(dx, dz);
          shot.pitch = static_cast<float>(-std::atan2(aim_rise, horizontal) * kRadToDeg);
          report.shots.push_back(shot);
        }
      } else {
        mob.goal = MobGoal::Chase;
        follow_path(mob, env, target->x, target->y, target->z, /*replan=*/true, /*speed=*/1.0,
                    &walk, &dir_x, &dir_z);
      }
    } else if ((mob.hostile() || mob.retaliates()) && target != nullptr) {
      const bool in_reach = perceived_distance_sqr(mob, *target) <= attack_reach_sqr(mob, *target);
      // `RangedBowAttackGoal` outranks the melee goal for the skeleton family
      // and never swings: inside its 15-block radius, with the target visible,
      // it stands, faces the target and looses an arrow every twenty ticks. Its
      // `canUse` wants both `mob.getTarget() != null` and `mob.isHolding(Items.BOW)`
      // — the family table below is that second answer for every row the table
      // carries — and `canContinueToUse` re-checks the radius and the sight.
      const bool bow_armed = mob_bow_armed(mob);
      if (bow_armed && mob.target_visible &&
          perceived_distance_sqr(mob, *target) <= kMobBowRange * kMobBowRange) {
        mob.goal = MobGoal::RangedAttack;
        mob.yaw = facing_yaw(target->x - mob.x, target->z - mob.z);
        // `RangedBowAttackGoal.tick`'s `mob.getNavigation().stop()` and the
        // `lookControl.setLookAt(target, 30.0F, 30.0F)`: it aims without walking.
        mob.path = NavPath{};
        mob.has_path_goal = false;
        if (mob.attack_cooldown <= 0) {
          mob.attack_cooldown = kMobBowInterval;
          // `AbstractSkeleton.performRangedAttack`:
          //
          //     d1 = target.getY(0.3333333) - arrow.getY()
          //     d3 = sqrt(dx*dx + dz*dz)
          //     shoot(dx, d1 + d3 * 0.2, dz, 1.6F, inaccuracy)
          //
          // The `d3 * 0.2` is the lift that fights the arrow's own drop, and
          // the pitch is the negative of the elevation angle: the client's
          // convention, which `shoot_arrow` and `launch` already speak.
          const double dx = target->x - mob.x;
          const double dz = target->z - mob.z;
          const double horizontal = std::sqrt(dx * dx + dz * dz);
          const double aim_rise =
              (target->y + static_cast<double>(target->height) * 0.3333333333333333) -
              mob_eye_y(mob);
          MobShot shot;
          shot.mob_entity_id = mob.entity_id;
          shot.x = mob.x;
          shot.y = mob_eye_y(mob);
          shot.z = mob.z;
          shot.yaw = facing_yaw(dx, dz);
          shot.pitch = static_cast<float>(
              -std::atan2(aim_rise + horizontal * kMobArrowArc, horizontal) * kRadToDeg);
          report.shots.push_back(shot);
        }
      } else if (in_reach) {
        mob.goal = MobGoal::Attack;
        mob.yaw = facing_yaw(target->x - mob.x, target->z - mob.z);
        mob.path = NavPath{};
        mob.has_path_goal = false; // PathNavigation.stop()
        // MeleeAttackGoal's "reached the target" branch stops the mob and hits
        // on the interval — and `canPerformAttack` wants line of sight as well,
        // so a mob on the wrong side of a wall swings at nothing. A mob with no
        // ATTACK_DAMAGE attribute at all (a ghast, a shulker, the dragon, a
        // sulfur cube) closes in and never swings: its real attack is one of the
        // unmodelled abilities the table's header names.
        if (mob.attack_cooldown <= 0 && mob.target_visible && mob.type->attack_damage > 0.0f) {
          mob.attack_cooldown = kMobAttackInterval;
          MobAttack attack;
          attack.mob_entity_id = mob.entity_id;
          attack.player_entity_id = target->entity_id;
          attack.damage = mob.type->attack_damage;
          attack.mob_name = mob.display_name();
          report.attacks.push_back(std::move(attack));
        }
      } else {
        mob.goal = MobGoal::Chase;
        // MeleeAttackGoal.tick: re-plan the path while the target can be seen
        // (or the goal was built with followingTargetEvenIfNotSeen), and walk
        // the path already taken when it cannot.
        follow_path(mob, env, target->x, target->y, target->z,
                    mob.follows_target_unseen() || mob.target_visible, /*speed=*/1.0, &walk, &dir_x,
                    &dir_z);
      }
    } else if (mob.animal() && mob.is_in_love()) {
      // `BreedGoal` outranks `FollowParentGoal` and `WaterAvoidingRandomStrollGoal`
      // (priority 2 or 3 against 4/5 and 5/6), and a mob only ever acquires a
      // partner while in love. A baby is never in love — `Animal.aiStep` clears
      // it for any age that is not 0 — so the breed and follow goals cannot both
      // want the mob.
      breed_step(mob, mobs, index, grid, env, goal_selector_tick(mob), &report, &walk, &dir_x,
                 &dir_z);
    } else if (mob.animal() && follow_parent_step(mob, mobs, index, grid, env,
                                                  goal_selector_tick(mob), &walk, &dir_x, &dir_z)) {
      // `FollowParentGoal` is running: the baby is walking to its parent and the
      // stroll is outranked while that holds. It already set the movement.
    } else {
      // A mob that is not in love has no running breed goal (`canUse` fails),
      // and `GoalSelector.tick`'s cleanup stops one that was running: the
      // partner and the love clock go with it. A baby with no parent in range
      // reaches this branch the same way, and strolls like any other animal.
      mob.mate_entity_id = -1;
      mob.love_time = 0;
      stroll(mob, env, &walk, &dir_x, &dir_z);
    }

    if (!dragon)
      move_mob(mob, env, dir_x, dir_z, walk);

    // --- the landing (Entity.checkFallDamage) ---
    // move_mob filled `fall_distance` in when the fall ended, so this is the
    // "landed with a distance to spend" branch. The dragon's flight is not
    // gravity, so it has no landing to spend.
    if (!dragon && mob.on_ground && mob.fall_distance > 0.0) {
      const double distance = mob.fall_distance;
      mob.fall_distance = 0.0;
      mob.falling = false;
      const double damage = std::floor(std::max(0.0, distance - kMobSafeFallDistance));
      // minecraft:fall bypasses armour (DamageTypeTags.BYPASSES_ARMOR).
      if (damage > 0.0 && hurt_mob(mob, static_cast<float>(damage), -1, {}, 0.0, 0.0, 0.0,
                                   /*bypasses_armor=*/true)
                              .accepted)
        report.hurt.push_back(mob.entity_id);
    }

    // --- the block the mob is standing in (`LivingEntity.tick` ->
    // `applyEffectsFromBlocks`) ---
    // BaseFireBlock.entityInside: ignite, then take the block's own damage (1.0
    // for fire, 2.0 for soul fire) on every tick the box overlaps it. A mob has
    // no `nextInt(1, 3)` — that is a player's — and its idle countdown is 0, so
    // the eight-second ignite lands on the first tick.
    {
      // `Entity.isInRain`: the feet cell or the cell at the top of the box, so a
      // mob under a roof is not put out by the storm above it. This runs even
      // with no fire block in reach — vanilla's `clearFire` is unconditional.
      bool in_rain = false;
      if (env.is_raining_at) {
        const int fx = static_cast<int>(std::floor(mob.x));
        const int fz = static_cast<int>(std::floor(mob.z));
        in_rain =
            env.is_raining_at(fx, static_cast<int>(std::floor(mob.y)), fz) ||
            env.is_raining_at(fx, static_cast<int>(std::floor(mob.y + mob.body_height())), fz);
      }
      float fire_damage = 0.0f;
      bool ignited = false;
      if (env.get_block) {
        mc::world::FireWorld fire;
        fire.get_block = env.get_block;
        const double half = mob.half_width();
        if (mc::world::fire_entity_inside(fire, mob.x - half, mob.y, mob.z - half, mob.x + half,
                                          mob.y + mob.body_height(), mob.z + half, &fire_damage)) {
          const bool fire_immune = mob.type->fire_immune;
          ignited = mc::world::fire_ignite(mob.burning, fire_immune, /*is_player=*/false, 0);
          // `Entity.isInvulnerableTo` refuses `minecraft:in_fire` for a
          // fire-immune type (`source.is(DamageTypeTags.IS_FIRE) &&
          // this.fireImmune()`), on top of `fireIgnite`'s own early return — so a
          // blaze in a fire is neither lit nor scratched. `minecraft:in_fire` is
          // also *not* in DamageTypeTags.BYPASSES_ARMOR.
          if (!fire_immune && hurt_mob(mob, fire_damage, -1, {}, 0.0, 0.0, 0.0).accepted)
            report.hurt.push_back(mob.entity_id);
        }
      }
      mob.burning.reset_if_not_burning(ignited, in_rain);
    }

    if (!mob.is_alive()) {
      record_death(mob);
      continue;
    }

    // The flags the clients draw, both of them through SynchedEntityData.set
    // (which only fires on a change): the flames of `setSharedFlagOnFire` and
    // the outline of `updateGlowingStatus`. They share one byte, so either bit
    // moving resends it with both.
    if (mob.burning.on_fire() != was_on_fire || mob.glowing != was_glowing)
      report.flags_changed.push_back(mob.entity_id);

    if (mob.x != before_x || mob.y != before_y || mob.z != before_z || mob.yaw != before_yaw)
      report.moved.push_back(mob.entity_id);
  }

  // Drop the dead (their data lives on in the report).
  mobs.erase(std::remove_if(mobs.begin(), mobs.end(), [](const Mob& m) { return m.removed; }),
             mobs.end());
  return report;
}

bool mob_add_effect(Mob& mob, const MobEffectInstance& instance) {
  // The outcome's `events` are dropped on purpose (see the header): what a mob
  // can do with an effect today is hold it and show the glowing bit.
  EffectOutcome outcome;
  const bool changed = mob.effects.add(instance, outcome);
  mob.glowing = effects_glow(mob.effects);
  return changed;
}

bool mob_tick_effects(Mob& mob) {
  const bool was_glowing = mob.glowing;
  const EffectTickContext ctx{mob.health,
                              mob.type ? static_cast<float>(mob.type->max_health) : mob.health,
                              /*absorption=*/0.0f};
  EffectOutcome outcome;
  mob.effects.tick(mob.age, ctx, outcome);
  mob.glowing = effects_glow(mob.effects);
  return mob.glowing != was_glowing;
}

const GeneratedMob* mob_by_name(std::string_view name) {
  // The same normalizer /give and /summon use for item and block names: case
  // and the `minecraft:` prefix are both optional.
  const std::string normalized = normalize_item_name(name);
  if (normalized.empty())
    return nullptr;
  for (int i = 0; i < kMobTypeCount; ++i)
    if (normalized == kMobs[i].name)
      return &kMobs[i];
  return nullptr;
}

const GeneratedMob* mob_by_entity_type_id(int32_t entity_type_id) {
  for (int i = 0; i < kMobTypeCount; ++i)
    if (kMobs[i].entity_type_id == entity_type_id)
      return &kMobs[i];
  return nullptr;
}

} // namespace mc::world
