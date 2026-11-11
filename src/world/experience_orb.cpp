// SPDX-License-Identifier: GPL-3.0-or-later
//
// Experience orbs: the simulation behind world/experience_orb.h. Every constant
// and every branch is drawn from the decompiled ExperienceOrb (26.2) and its
// parents; the comment on each block names the vanilla member it mirrors.
#include "world/experience_orb.h"

#include <algorithm>
#include <cmath>

#include "world/axis_step.h"

namespace mc::world {
namespace {

// Player's bounding box: width 0.6, height 1.8 (EntityDimensions.standing).
constexpr double kPlayerHalfWidth = 0.3;
constexpr double kPlayerHeight = 1.8;
// Player.getEyeHeight(), halved: the pull aims at the middle of the player's
// eye box, `player.getY() + getEyeHeight() / 2`.
constexpr double kPlayerEyeHeightHalf = 1.62 / 2.0;

// ExperienceOrb.getExperienceValue's ladder, largest first.
constexpr int kValueLadder[] = {2477, 1237, 617, 307, 149, 73, 37, 17, 7, 3, 1};

// The cells an orb's box covers, so a box test is a handful of block lookups.
bool box_hits_block(const OrbBox& box, int bx, int by, int bz) {
  return box.min_x < static_cast<double>(bx) + 1.0 && box.max_x > static_cast<double>(bx) &&
         box.min_y < static_cast<double>(by) + 1.0 && box.max_y > static_cast<double>(by) &&
         box.min_z < static_cast<double>(bz) + 1.0 && box.max_z > static_cast<double>(bz);
}

// Substitutes for `!level.noCollision(box)`: does the orb's box overlap a block
// the world calls solid? A world with no solidity probe reports "no collision",
// which is what lets an orb fall through the void world for its whole lifetime.
bool box_blocked(const OrbEnvironment& env, double x, double y, double z) {
  if (!env.is_solid)
    return false;
  const OrbBox box{.min_x = x - kOrbWidth / 2.0,
                   .min_y = y,
                   .min_z = z - kOrbWidth / 2.0,
                   .max_x = x + kOrbWidth / 2.0,
                   .max_y = y + kOrbHeight,
                   .max_z = z + kOrbWidth / 2.0};
  const int x0 = static_cast<int>(std::floor(box.min_x));
  const int x1 = static_cast<int>(std::ceil(box.max_x)) - 1;
  const int y0 = static_cast<int>(std::floor(box.min_y));
  const int y1 = static_cast<int>(std::ceil(box.max_y)) - 1;
  const int z0 = static_cast<int>(std::floor(box.min_z));
  const int z1 = static_cast<int>(std::ceil(box.max_z)) - 1;
  for (int bx = x0; bx <= x1; ++bx)
    for (int by = y0; by <= y1; ++by)
      for (int bz = z0; bz <= z1; ++bz)
        if (box_hits_block(box, bx, by, bz) && env.is_solid(bx, by, bz))
          return true;
  return false;
}

double distance_sqr(double ax, double ay, double az, double bx, double by, double bz) {
  const double dx = ax - bx, dy = ay - by, dz = az - bz;
  return dx * dx + dy * dy + dz * dz;
}

const OrbPlayer* find_player(const OrbEnvironment& env, int32_t entity_id) {
  for (const OrbPlayer& p : env.players)
    if (p.entity_id == entity_id)
      return &p;
  return nullptr;
}

// ExperienceOrb.followNearbyPlayer -> Level.getNearestPlayer(this, 8.0).
const OrbPlayer* nearest_player(const OrbEnvironment& env, const ExperienceOrb& orb) {
  const OrbPlayer* best = nullptr;
  double best_sqr = kOrbMaxFollowDistance * kOrbMaxFollowDistance;
  for (const OrbPlayer& p : env.players) {
    if (!p.can_collect)
      continue; // spectator / dead: `!isSpectator() && !isDeadOrDying()`
    const double d = distance_sqr(p.x, p.y, p.z, orb.x, orb.y, orb.z);
    if (d <= best_sqr) {
      best_sqr = d;
      best = &p;
    }
  }
  return best;
}

// The pull itself, in velocity units: the direction to the middle of the
// player's eye box, scaled by power^2 * 0.1 where power = 1 - distance / 8.
void apply_pull(ExperienceOrb& orb, const OrbPlayer& player) {
  const double dx = player.x - orb.x;
  const double dy = player.y + kPlayerEyeHeightHalf - orb.y;
  const double dz = player.z - orb.z;
  const double length_sqr = dx * dx + dy * dy + dz * dz;
  if (length_sqr <= 0.0)
    return;
  const double length = std::sqrt(length_sqr);
  const double power = 1.0 - length / kOrbMaxFollowDistance;
  if (power <= 0.0)
    return;
  const double scale = power * power * 0.1 / length;
  orb.vx += dx * scale;
  orb.vy += dy * scale;
  orb.vz += dz * scale;
}

} // namespace

int orb_value_for(int max_value) {
  for (int v : kValueLadder)
    if (max_value >= v)
      return v;
  return 1;
}

std::vector<int> split_orb_award(int amount) {
  std::vector<int> orbs;
  while (amount > 0) {
    const int value = orb_value_for(amount);
    amount -= value;
    orbs.push_back(value);
  }
  return orbs;
}

void orb_launch(ExperienceOrb& orb, double x_draw, double y_draw, double z_draw) {
  orb.vx = (x_draw * 0.2 - 0.1) * 2.0;
  orb.vy = y_draw * 0.2 * 2.0;
  orb.vz = (z_draw * 0.2 - 0.1) * 2.0;
}

OrbBox orb_box(const ExperienceOrb& orb) {
  return OrbBox{.min_x = orb.x - kOrbWidth / 2.0,
                .min_y = orb.y,
                .min_z = orb.z - kOrbWidth / 2.0,
                .max_x = orb.x + kOrbWidth / 2.0,
                .max_y = orb.y + kOrbHeight,
                .max_z = orb.z + kOrbWidth / 2.0};
}

bool orb_box_overlaps_player(const OrbBox& box, const OrbPlayer& player) {
  return box.min_x < player.x + kPlayerHalfWidth && box.max_x > player.x - kPlayerHalfWidth &&
         box.min_y < player.y + kPlayerHeight && box.max_y > player.y &&
         box.min_z < player.z + kPlayerHalfWidth && box.max_z > player.z - kPlayerHalfWidth;
}

bool orbs_can_merge(const ExperienceOrb& a, const ExperienceOrb& b) {
  if (a.removed || b.removed || a.value != b.value)
    return false;
  const int64_t diff = static_cast<int64_t>(b.entity_id) - static_cast<int64_t>(a.entity_id);
  return diff % kOrbGroupsPerArea == 0;
}

bool orb_merge_into(ExperienceOrb& into, ExperienceOrb& other) {
  if (&into == &other || !orbs_can_merge(into, other))
    return false;
  into.count += other.count;
  into.age = std::min(into.age, other.age);
  other.removed = true;
  return true;
}

namespace {

// ExperienceOrb.unstuckIfPossible, approximated. It matters more than it looks:
// a block entity hands its payout to an orb centred in the *block*, and a
// furnace is solid, so without this an orb spent inside one would never move,
// never fall and never be collected. Vanilla searches a VoxelShape for the
// closest free position; this walks a small shell of candidates around the orb,
// preferring the free spot straight above it — which is the direction a
// furnace's payout visibly pops.
void unstuck(ExperienceOrb& orb, const OrbEnvironment& env) {
  static constexpr double kCandidates[][3] = {
      {0.0, 1.0, 0.0},  {0.0, 0.5, 0.0}, {-0.5, 0.0, 0.0}, {0.5, 0.0, 0.0},
      {0.0, 0.0, -0.5}, {0.0, 0.0, 0.5}, {-0.5, 0.5, 0.0}, {0.5, 0.5, 0.0},
      {0.0, 0.5, -0.5}, {0.0, 0.5, 0.5}, {-0.9, 0.0, 0.0}, {0.9, 0.0, 0.0},
      {0.0, 0.0, -0.9}, {0.0, 0.0, 0.9}, {0.0, -0.5, 0.0}, {0.0, -1.0, 0.0},
  };
  for (const auto& candidate : kCandidates) {
    const double x = orb.x + candidate[0];
    const double y = orb.y + candidate[1];
    const double z = orb.z + candidate[2];
    if (box_blocked(env, x, y, z))
      continue;
    orb.x = x;
    orb.y = y;
    orb.z = z;
    return;
  }
}

// The merge scan (ExperienceOrb.scanForMerges): orbs whose boxes come within
// ORB_MERGE_DISTANCE, sharing this orb's id residue class and value, are
// absorbed. Vanilla's box is `getBoundingBox().inflate(0.5)` on each side.
void scan_for_merges(std::vector<ExperienceOrb>& orbs, size_t index) {
  ExperienceOrb& self = orbs[index];
  const OrbBox mine = orb_box(self);
  for (size_t j = 0; j < orbs.size(); ++j) {
    if (j == index || orbs[j].removed)
      continue;
    const OrbBox theirs = orb_box(orbs[j]);
    const double gap = kOrbMergeDistance;
    if (mine.min_x - gap >= theirs.max_x || mine.max_x + gap <= theirs.min_x)
      continue;
    if (mine.min_y - gap >= theirs.max_y || mine.max_y + gap <= theirs.min_y)
      continue;
    if (mine.min_z - gap >= theirs.max_z || mine.max_z + gap <= theirs.min_z)
      continue;
    orb_merge_into(self, orbs[j]);
  }
}

// One axis of Entity.move's collision handling, block-granular: the orb either
// completes the step, or stops at the face it hit. `on_block_hit` reports the
// hit so the vertical axis can bounce (ExperienceOrb.tick's
// `verticalCollisionBelow && fallSpeed < -gravity` -> `-fallSpeed * 0.4`).
void move_axis(ExperienceOrb& orb, const OrbEnvironment& env, int axis) {
  const double before = axis == 0 ? orb.x : (axis == 1 ? orb.y : orb.z);
  const double delta = axis == 0 ? orb.vx : (axis == 1 ? orb.vy : orb.vz);
  const double after = before + delta;
  auto blocked_at = [&](double value) {
    return axis == 0 ? box_blocked(env, value, orb.y, orb.z)
                     : (axis == 1 ? box_blocked(env, orb.x, value, orb.z)
                                  : box_blocked(env, orb.x, orb.y, value));
  };
  if (!blocked_at(after)) {
    if (axis == 0)
      orb.x = after;
    else if (axis == 1)
      orb.y = after;
    else
      orb.z = after;
    return;
  }
  // Blocked: stand the orb on the face it reached for (a downward step lands on
  // the floor's top face) and lose the axis speed.
  if (axis == 1 && delta < 0.0) {
    const double surface = std::floor(after) + 1.0;
    if (!blocked_at(surface)) {
      orb.y = surface;
      orb.on_ground = true;
      orb.vy = delta < -kOrbGravity ? -delta * 0.4 : 0.0;
      return;
    }
  }
  if (axis == 0)
    orb.vx = 0.0;
  else if (axis == 1)
    orb.vy = 0.0;
  else
    orb.vz = 0.0;
}

} // namespace

OrbTickReport tick_orbs(std::vector<ExperienceOrb>& orbs, const OrbEnvironment& env) {
  OrbTickReport report;
  if (orbs.empty())
    return report;

  // ExperienceOrb.scanForMerges is due when tickCount % 20 == 1, and `age` is
  // the count of ticks already run, so tickCount is age + 1.
  for (size_t i = 0; i < orbs.size(); ++i)
    if (orbs[i].age % kOrbScanPeriod == 0)
      scan_for_merges(orbs, i);
  // Orbs swallowed by a merge are gone as far as the clients are concerned (the
  // absorbing orb keeps their value in its count).
  for (const ExperienceOrb& orb : orbs)
    if (orb.removed)
      report.removed.push_back(orb.entity_id);

  for (ExperienceOrb& orb : orbs) {
    if (orb.removed)
      continue;
    const double start_x = orb.x, start_y = orb.y, start_z = orb.z;
    ++orb.age; // Entity.tickCount / ExperienceOrb.age
    if (orb.age >= kOrbLifetime) {
      orb.removed = true;
      report.removed.push_back(orb.entity_id);
      continue;
    }

    // Out of the block it spawned in, if it spawned in one (the constructor's
    // `if (!level.noCollision(...)) unstuckIfPossible(size)`).
    if (box_blocked(env, orb.x, orb.y, orb.z))
      unstuck(orb, env);

    // `colliding = !level.noCollision(getBoundingBox())`: an orb inside a block
    // does not accumulate gravity, which is what lets it rest on the floor.
    if (box_blocked(env, orb.x, orb.y, orb.z)) {
      orb.on_ground = true;
    } else {
      orb.on_ground = false;
      orb.vy -= kOrbGravity; // Entity.applyGravity -> getDefaultGravity() = 0.03
    }

    // followNearbyPlayer: keep chasing the current target until it leaves the
    // 8-block range, otherwise take the nearest one.
    const OrbPlayer* target = nullptr;
    if (orb.following >= 0) {
      target = find_player(env, orb.following);
      if (target != nullptr && (!target->can_collect ||
                                distance_sqr(target->x, target->y, target->z, orb.x, orb.y, orb.z) >
                                    kOrbMaxFollowDistance * kOrbMaxFollowDistance))
        target = nullptr;
    }
    if (target == nullptr)
      target = nearest_player(env, orb);
    orb.following = target != nullptr ? target->entity_id : -1;
    if (target != nullptr)
      apply_pull(orb, *target);

    // Entity.move(MoverType.SELF, getDeltaMovement()), axis by axis, in
    // `Direction.axisStepOrder`'s order — Y first, then the horizontal axis the
    // orb is travelling along harder.
    int step_order[3];
    axis_step_order(orb.vx, orb.vz, step_order);
    move_axis(orb, env, step_order[0]);
    move_axis(orb, env, step_order[1]);
    move_axis(orb, env, step_order[2]);

    // `friction = getAirDrag()` (x the block friction when on the ground),
    // then `setDeltaMovement(getDeltaMovement().scale(friction))`.
    const double friction = orb.on_ground ? kOrbGroundDrag : kOrbAirDrag;
    orb.vx *= friction;
    orb.vy *= friction;
    orb.vz *= friction;

    // playerTouch: the first player whose box overlaps takes one orb out of the
    // stack. takeXpDelay — which the caller counts down and sets again on a
    // pickup — is what stops a merged stack being drained in a single tick.
    const OrbBox box = orb_box(orb);
    for (const OrbPlayer& player : env.players) {
      if (!player.can_collect || player.take_xp_delay > 0 || !orb_box_overlaps_player(box, player))
        continue;
      report.collections.push_back(OrbCollection{orb.entity_id, player.entity_id, orb.value});
      --orb.count;
      if (orb.count <= 0) {
        orb.count = 0;
        orb.removed = true;
      }
      break;
    }

    if (!orb.removed && (orb.x != start_x || orb.y != start_y || orb.z != start_z))
      report.moved.push_back(orb.entity_id);
  }

  orbs.erase(std::remove_if(orbs.begin(), orbs.end(),
                            [](const ExperienceOrb& orb) { return orb.removed; }),
             orbs.end());
  return report;
}

} // namespace mc::world
