// SPDX-License-Identifier: GPL-3.0-or-later
//
// The TNT rules: the primed entity's tick, and `ServerExplosion`'s ray-cast,
// visibility sample, damage and knockback. See world/tnt.h for what is modelled
// and where the boundary sits.
#include "world/tnt.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "world/blast_resistance_table.h"

namespace mc::world {
namespace {

// The epsilon the movement uses to stay off a boundary it has just been clipped
// to, so the next tick's box test does not report the block it is touching.
constexpr double kMoveEps = 1e-7;
// A cube move is split into sub-steps no longer than this, so a fast fall
// cannot tunnel through a floor. (`AABB.clip` is a swept test in vanilla; a
// sub-stepped position test is the same answer with less geometry.)
constexpr double kMaxSubStep = 0.4;

int floor_to_int(double v) {
  return static_cast<int>(std::floor(v));
}

// Whether the primed entity's box at (x, y, z) overlaps a solid cell. The box is
// `sized(0.98, 0.98)` centred on x/z, with y at its bottom.
bool cube_collides(const TntEnvironment& env, double x, double y, double z) {
  if (!env.is_solid)
    return false;
  const double min_x = x - kPrimedTntHalfWidth;
  const double max_x = x + kPrimedTntHalfWidth;
  const double min_y = y;
  const double max_y = y + kPrimedTntHeight;
  const double min_z = z - kPrimedTntHalfWidth;
  const double max_z = z + kPrimedTntHalfWidth;
  // The bounds are pulled in by the epsilon for the same reason `Level.clip`
  // does: a box resting exactly on a boundary does not reach the cell above.
  for (int bx = floor_to_int(min_x + kMoveEps); bx <= floor_to_int(max_x - kMoveEps); ++bx)
    for (int by = floor_to_int(min_y + kMoveEps); by <= floor_to_int(max_y - kMoveEps); ++by)
      for (int bz = floor_to_int(min_z + kMoveEps); bz <= floor_to_int(max_z - kMoveEps); ++bz)
        if (env.is_solid(bx, by, bz))
          return true;
  return false;
}

// `Entity.move(MoverType.SELF, delta)` for the cube: one axis at a time, the
// blocked component zeroed and the position clipped to the cell boundary the
// leading edge stopped at. The axes are walked in `Direction.axisStepOrder`
// order (world/axis_step.h) — Y first, then the dominant horizontal one — read
// from the delta before any of it is resolved, exactly as `collideWithShapes`
// does.
struct CubeMoveResult {
  bool on_ground = false;
};

CubeMoveResult move_cube(PrimedTnt& tnt, const TntEnvironment& env) {
  CubeMoveResult result;
  if (!env.is_solid)
    return result;
  int step_order[3];
  axis_step_order(tnt.vx, tnt.vz, step_order);
  // How far the box reaches below/above the position on each axis: the y axis
  // sits at the box's bottom, x and z are centred.
  const double below[3] = {kPrimedTntHalfWidth, 0.0, kPrimedTntHalfWidth};
  const double above[3] = {kPrimedTntHalfWidth, kPrimedTntHeight, kPrimedTntHalfWidth};
  double* const pos[3] = {&tnt.x, &tnt.y, &tnt.z};
  double* const vel[3] = {&tnt.vx, &tnt.vy, &tnt.vz};
  for (int slot = 0; slot < 3; ++slot) {
    const int axis = step_order[slot];
    double left = std::fabs(*vel[axis]);
    const int sign = *vel[axis] < 0.0 ? -1 : 1;
    // Sub-stepped so a terminal-velocity fall cannot tunnel through a floor.
    while (left > 0.0) {
      const double step = std::min(left, kMaxSubStep) * sign;
      const double to = *pos[axis] + step;
      double probe[3] = {tnt.x, tnt.y, tnt.z};
      probe[axis] = to;
      if (!cube_collides(env, probe[0], probe[1], probe[2])) {
        *pos[axis] = to;
        left -= std::fabs(step);
        continue;
      }
      // The leading edge stops one epsilon short of the cell it would enter:
      // the cell's far face minus the box's reach.
      if (step > 0.0)
        *pos[axis] = std::floor(to + above[axis]) - above[axis] - kMoveEps;
      else
        *pos[axis] = std::floor(to - below[axis]) + 1.0 + below[axis] + kMoveEps;
      *vel[axis] = 0.0;
      if (axis == 1 && step < 0.0)
        result.on_ground = true;
      break;
    }
  }
  return result;
}

double lerp(double t, double a, double b) {
  return a + t * (b - a);
}

} // namespace

int primed_tnt_short_fuse(int fuse, int draw) {
  return draw + fuse / 8;
}

void primed_tnt_launch(PrimedTnt& tnt, double angle_radians) {
  tnt.vx = -std::sin(angle_radians) * 0.02;
  tnt.vy = 0.2;
  tnt.vz = -std::cos(angle_radians) * 0.02;
}

bool tick_primed_tnt(PrimedTnt& tnt, const TntEnvironment& env) {
  // `applyGravity()`: the delta gains -0.04 before the move.
  tnt.vy -= kPrimedTntGravity;
  const bool on_ground = move_cube(tnt, env).on_ground;
  // `setDeltaMovement(getDeltaMovement().scale(getAirDrag()))`.
  tnt.vx *= kPrimedTntAirDrag;
  tnt.vy *= kPrimedTntAirDrag;
  tnt.vz *= kPrimedTntAirDrag;
  if (on_ground) {
    tnt.vx *= kPrimedTntGroundFriction;
    tnt.vy *= kPrimedTntGroundBounce;
    tnt.vz *= kPrimedTntGroundFriction;
  }
  tnt.fuse -= 1;
  return tnt.fuse <= 0;
}

std::vector<BlockPosKey> explosion_blocks(const ExplosionLevel& level, double center_x,
                                          double center_y, double center_z, float radius) {
  std::set<BlockPosKey> blown;
  if (!level.get_block)
    return {};
  for (int xx = 0; xx < kExplosionRaySamples; ++xx) {
    for (int yy = 0; yy < kExplosionRaySamples; ++yy) {
      for (int zz = 0; zz < kExplosionRaySamples; ++zz) {
        // Only the shell of the 16^3 cube casts a ray.
        if (xx != 0 && xx != kExplosionRaySamples - 1 && yy != 0 &&
            yy != kExplosionRaySamples - 1 && zz != 0 && zz != kExplosionRaySamples - 1)
          continue;
        const double scale = 1.0 / (kExplosionRaySamples - 1);
        double xd = xx * scale * 2.0 - 1.0;
        double yd = yy * scale * 2.0 - 1.0;
        double zd = zz * scale * 2.0 - 1.0;
        const double length = std::sqrt(xd * xd + yd * yd + zd * zd);
        if (length == 0.0)
          continue;
        xd /= length;
        yd /= length;
        zd /= length;
        double xp = center_x;
        double yp = center_y;
        double zp = center_z;
        const double jitter = level.next_float ? 0.7 + level.next_float() * 0.6 : 1.0;
        for (float remaining = static_cast<float>(radius * jitter); remaining > 0.0f;
             remaining -= kExplosionStepDecay) {
          const BlockPosKey pos{floor_to_int(xp), floor_to_int(yp), floor_to_int(zp)};
          if (level.in_bounds && !level.in_bounds(pos.x, pos.y, pos.z))
            break; // vanilla's `continue block2`: the whole ray is dropped
          const BlockStateKey state = level.get_block(pos.x, pos.y, pos.z);
          if (block_has_explosion_resistance(state))
            remaining -=
                (block_blast_resistance(state.name) + kExplosionResistanceBias) * kExplosionStep;
          // `shouldBlockExplode` is always true for a TNT blast, so every cell
          // that survives the walk is marked — the air cells included, which is
          // what vanilla's explosion packet counts. The caller skips the empty
          // ones when it drops loot.
          if (remaining > 0.0f)
            blown.insert(pos);
          xp += xd * kExplosionStep;
          yp += yd * kExplosionStep;
          zp += zd * kExplosionStep;
        }
      }
    }
  }
  return std::vector<BlockPosKey>(blown.begin(), blown.end());
}

void explosion_shuffle(std::vector<BlockPosKey>& blocks, const std::function<int(int)>& next_int) {
  if (!next_int)
    return;
  // `for (int i = list.size(); i > 1; --i) list.set(i - 1, list.set(nextInt(i), list.get(i - 1)))`
  // — the two `set` calls are a swap, and `nextInt(i)` draws from [0, i).
  for (int i = static_cast<int>(blocks.size()); i > 1; --i)
    std::swap(blocks[static_cast<size_t>(i - 1)], blocks[static_cast<size_t>(next_int(i))]);
}

float explosion_seen_percent(
    double min_x, double min_y, double min_z, double max_x, double max_y, double max_z,
    double center_x, double center_y, double center_z,
    const std::function<bool(double, double, double, double, double, double)>& clip_miss) {
  const double xs = 1.0 / ((max_x - min_x) * 2.0 + 1.0);
  const double ys = 1.0 / ((max_y - min_y) * 2.0 + 1.0);
  const double zs = 1.0 / ((max_z - min_z) * 2.0 + 1.0);
  const double x_offset = (1.0 - std::floor(1.0 / xs) * xs) / 2.0;
  const double z_offset = (1.0 - std::floor(1.0 / zs) * zs) / 2.0;
  if (xs < 0.0 || ys < 0.0 || zs < 0.0)
    return 0.0f;
  int hits = 0;
  int count = 0;
  for (double xx = 0.0; xx <= 1.0; xx += xs) {
    for (double yy = 0.0; yy <= 1.0; yy += ys) {
      for (double zz = 0.0; zz <= 1.0; zz += zs) {
        const double from_x = lerp(xx, min_x, max_x) + x_offset;
        const double from_y = lerp(yy, min_y, max_y);
        const double from_z = lerp(zz, min_z, max_z) + z_offset;
        if (!clip_miss || clip_miss(from_x, from_y, from_z, center_x, center_y, center_z))
          ++hits;
        ++count;
      }
    }
  }
  return count == 0 ? 0.0f : static_cast<float>(hits) / static_cast<float>(count);
}

float explosion_entity_damage(float radius, double distance_sqr, float exposure) {
  const double double_radius = radius * 2.0;
  const double distance = std::sqrt(distance_sqr) / double_radius;
  const double power = (1.0 - distance) * exposure;
  return static_cast<float>((power * power + power) / 2.0 * 7.0 * double_radius + 1.0);
}

ExplosionPush explosion_knockback(double center_x, double center_y, double center_z,
                                  double origin_x, double origin_y, double origin_z,
                                  double distance_sqr, float radius, float exposure,
                                  double knockback_resistance) {
  const double dx = origin_x - center_x;
  const double dy = origin_y - center_y;
  const double dz = origin_z - center_z;
  const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (length < 1e-8)
    return {};
  // `Vec3.normalize()` then `scale(knockbackPower)`, where the power is
  // `(1 - distance / 2r) * exposure * knockbackMultiplier * (1 - resistance)`
  // and the multiplier is 1.0 for anything this server explodes with.
  const double double_radius = radius * 2.0;
  const double distance = std::sqrt(distance_sqr) / double_radius;
  const double power = (1.0 - distance) * exposure * (1.0 - knockback_resistance);
  ExplosionPush push;
  push.x = dx / length * power;
  push.y = dy / length * power;
  push.z = dz / length * power;
  return push;
}

int explosion_drop_count(int count, float radius, const std::function<double()>& roll) {
  if (count <= 0)
    return 0;
  if (!roll || radius <= 0.0f)
    return count; // no radius in the loot context leaves the stack alone
  const double chance = 1.0 / static_cast<double>(radius);
  int kept = 0;
  for (int i = 0; i < count; ++i)
    if (roll() <= chance)
      ++kept;
  return kept;
}

float block_blast_resistance(std::string_view block_name) {
  int lo = 0;
  int hi = kBlockBlastResistanceCount - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    // The table is sorted by name at generation time.
    int cmp = block_name.compare(kBlockBlastResistance[mid].name);
    if (cmp == 0)
      return kBlockBlastResistance[mid].resistance;
    if (cmp < 0)
      hi = mid - 1;
    else
      lo = mid + 1;
  }
  return 0.0f;
}

bool block_has_explosion_resistance(const BlockStateKey& state) {
  // `Block.isAir() && fluid.isEmpty()` has no resistance entry at all; anything
  // else costs its table value, which for this server includes water and lava
  // (they are blocks here, and vanilla's fluids are 100 either way).
  return state.name != "minecraft:air" && state.name != "minecraft:cave_air" &&
         state.name != "minecraft:void_air";
}

} // namespace mc::world
