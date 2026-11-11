// SPDX-License-Identifier: GPL-3.0-or-later
//
// Projectiles: the arrow a bow fires and the three thrown items (Phase 4
// milestone H). See world/projectile.h for what is modelled and where the
// boundary sits.
//
// The two tick shapes are the whole difference between the kinds:
//
//   * a `ThrowableProjectile` applies gravity and air drag, then moves once and
//     resolves a single nearest hit (`ProjectileUtil.getHitResultOnMoveVector`)
//     — world/potion_projectile.cpp is the same shape for the bottles;
//   * an `AbstractArrow` checks whether it is buried *first*, then moves through
//     `stepMoveAndHit`, and only *afterwards* applies the drag and the gravity
//     (so a flown tick's drag acts on the velocity the arrow just moved with).
//     Its hit resolution is a piercing loop rather than a single hit, which is
//     why the arrow needs `find_hit_entities` and not the potion's
//     `clip_entities`.
//
// The one piece of geometry both this module and world/potion_projectile.cpp
// carry is the AABB slab clip and the box helpers below. They are deliberately
// identical to that file's (a reviewer can diff them); they are not shared
// because that file belongs to the potion work and the two must be readable
// side by side.
#include "world/projectile.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "world/enchantment.h"
#include "world/voxel_clip.h"

namespace mc::world {
namespace {

constexpr double kPi = 3.14159265358979323846;
// `Mth.atan2(...) * 57.2957763671875`, the constant the decompile spells out.
constexpr double kDegreesPerRadian = 57.2957763671875;
// The `minecraft:entity_type` the arrow is, as Power's and Punch's
// `direct_attacker` requirement names it (`#minecraft:arrows`).
constexpr const char* kArrowAttackerType = "minecraft:arrow";
// `AbstractArrow.onHitBlock` steps this far back along the sign of the velocity
// it buried with, and the same 0.06 cube `shouldFall` inflates its own box by.
constexpr double kArrowFallProbe = 0.06;

double degrees_to_radians(double degrees) {
  return degrees * kPi / 180.0;
}

// ---- box geometry (identical to world/potion_projectile.cpp) -----------------

ProjectileBox box_expand_towards(const ProjectileBox& box, double dx, double dy, double dz) {
  ProjectileBox out = box;
  if (dx < 0.0)
    out.min_x += dx;
  else
    out.max_x += dx;
  if (dy < 0.0)
    out.min_y += dy;
  else
    out.max_y += dy;
  if (dz < 0.0)
    out.min_z += dz;
  else
    out.max_z += dz;
  return out;
}

ProjectileBox box_inflate(const ProjectileBox& box, double dx, double dy, double dz) {
  ProjectileBox out = box;
  out.min_x -= dx;
  out.min_y -= dy;
  out.min_z -= dz;
  out.max_x += dx;
  out.max_y += dy;
  out.max_z += dz;
  return out;
}

bool box_intersects(const ProjectileBox& a, const ProjectileBox& b) {
  return a.min_x <= b.max_x && a.max_x >= b.min_x && a.min_y <= b.max_y && a.max_y >= b.min_y &&
         a.min_z <= b.max_z && a.max_z >= b.min_z;
}

// `AABB.clip`: the first point at which the segment enters the box, as a
// parameter in [0, 1]. The *entry* point, so a segment starting inside yields
// `from` rather than a face.
bool clip_box(const ProjectileBox& box, double from_x, double from_y, double from_z, double to_x,
              double to_y, double to_z, double* out_x, double* out_y, double* out_z) {
  const double from[3] = {from_x, from_y, from_z};
  const double delta[3] = {to_x - from_x, to_y - from_y, to_z - from_z};
  const double lo[3] = {box.min_x, box.min_y, box.min_z};
  const double hi[3] = {box.max_x, box.max_y, box.max_z};
  double t_min = 0.0;
  double t_max = 1.0;
  for (int axis = 0; axis < 3; ++axis) {
    if (delta[axis] == 0.0) {
      if (from[axis] < lo[axis] || from[axis] > hi[axis])
        return false;
      continue;
    }
    double t1 = (lo[axis] - from[axis]) / delta[axis];
    double t2 = (hi[axis] - from[axis]) / delta[axis];
    if (t1 > t2)
      std::swap(t1, t2);
    t_min = std::max(t_min, t1);
    t_max = std::min(t_max, t2);
    if (t_min > t_max)
      return false;
  }
  *out_x = from_x + delta[0] * t_min;
  *out_y = from_y + delta[1] * t_min;
  *out_z = from_z + delta[2] * t_min;
  return true;
}

double box_center_x(const ProjectileBox& box) {
  return (box.min_x + box.max_x) * 0.5;
}
double box_center_y(const ProjectileBox& box) {
  return (box.min_y + box.max_y) * 0.5;
}
double box_center_z(const ProjectileBox& box) {
  return (box.min_z + box.max_z) * 0.5;
}

double delta_length(const Projectile& projectile) {
  return std::sqrt(projectile.vx * projectile.vx + projectile.vy * projectile.vy +
                   projectile.vz * projectile.vz);
}

// ---- the two hit tests -------------------------------------------------------

// One entity the arrow reached, and where along the segment it was reached.
struct EntityHit {
  const ProjectileTarget* target = nullptr;
  double x = 0.0, y = 0.0, z = 0.0;
  // The sort key: `stepMoveAndHit` orders by `initialPosition.distanceToSqr(c
  // .getEntity().position())` — the *entity's* position, not the clip point.
  double distance = 0.0;
};

// `AbstractArrow.canHitEntity` + `Projectile.canHitEntity`: alive and pickable,
// not the firer until the shot has left their box, and not an entity this arrow
// has already pierced. (The PvP clause is not evaluated — see the header.)
bool can_hit(const Projectile& projectile, const ProjectileTarget& target) {
  if (!target.alive)
    return false;
  if (target.entity_id == projectile.owner_entity_id && !projectile.left_owner)
    return false;
  return std::find(projectile.pierced_entities.begin(), projectile.pierced_entities.end(),
                   target.entity_id) == projectile.pierced_entities.end();
}

// `ProjectileUtil.getEntityHitResult` (the single-entity path the potions and
// throwables use): the candidate boxes are clipped and the nearest clip point
// along the segment wins.
struct NearestHit {
  bool hit = false;
  const ProjectileTarget* target = nullptr;
  double x = 0.0, y = 0.0, z = 0.0;
};

NearestHit clip_nearest_entity(const ProjectileEnvironment& env, const Projectile& projectile,
                               double from_x, double from_y, double from_z, double to_x,
                               double to_y, double to_z, double dx, double dy, double dz,
                               float margin) {
  NearestHit result;
  const ProjectileBox search =
      box_inflate(box_expand_towards(projectile_box(projectile), dx, dy, dz), 1.0, 1.0, 1.0);
  double nearest = std::numeric_limits<double>::max();
  for (const ProjectileTarget& target : env.targets) {
    if (!can_hit(projectile, target))
      continue;
    const ProjectileBox box =
        projectile_target_box(target.x, target.y, target.z, target.half_width, target.height);
    if (!box_intersects(search, box))
      continue;
    const ProjectileBox inflated = box_inflate(box, margin, margin, margin);
    double hit_x = 0.0, hit_y = 0.0, hit_z = 0.0;
    if (!clip_box(inflated, from_x, from_y, from_z, to_x, to_y, to_z, &hit_x, &hit_y, &hit_z))
      continue;
    const double hx = hit_x - from_x;
    const double hy = hit_y - from_y;
    const double hz = hit_z - from_z;
    const double distance = hx * hx + hy * hy + hz * hz;
    if (distance >= nearest)
      continue;
    nearest = distance;
    result.hit = true;
    result.target = &target;
    result.x = hit_x;
    result.y = hit_y;
    result.z = hit_z;
  }
  return result;
}

// `ProjectileUtil.getManyEntityHitResult` as `AbstractArrow.findHitEntities`
// calls it: every reachable entity along the segment, in no particular order
// (stepMoveAndHit sorts). The two branches are vanilla's — the entity's own box
// clipped exactly, and failing that the margin case, where the box is inflated,
// clipped, and the resulting outside hit is re-clipped back towards the entity's
// centre with a block clip in the way.
std::vector<EntityHit> find_hit_entities(const ProjectileEnvironment& env,
                                         const Projectile& projectile, double from_x, double from_y,
                                         double from_z, double to_x, double to_y, double to_z,
                                         double dx, double dy, double dz, float margin) {
  std::vector<EntityHit> out;
  const ProjectileBox search =
      box_inflate(box_expand_towards(projectile_box(projectile), dx, dy, dz), 1.0, 1.0, 1.0);
  for (const ProjectileTarget& target : env.targets) {
    if (!can_hit(projectile, target))
      continue;
    const ProjectileBox box =
        projectile_target_box(target.x, target.y, target.z, target.half_width, target.height);
    if (!box_intersects(search, box))
      continue;
    double hit_x = 0.0, hit_y = 0.0, hit_z = 0.0;
    if (clip_box(box, from_x, from_y, from_z, to_x, to_y, to_z, &hit_x, &hit_y, &hit_z)) {
      out.push_back(EntityHit{&target, hit_x, hit_y, hit_z, 0.0});
      continue;
    }
    if (!(margin > 0.0f))
      continue;
    const ProjectileBox inflated = box_inflate(box, margin, margin, margin);
    double outside_x = 0.0, outside_y = 0.0, outside_z = 0.0;
    if (!clip_box(inflated, from_x, from_y, from_z, to_x, to_y, to_z, &outside_x, &outside_y,
                  &outside_z))
      continue;
    const double centre_x = box_center_x(box);
    const double centre_y = box_center_y(box);
    const double centre_z = box_center_z(box);
    const SolidHit blocked =
        clip_solids(env.is_solid, outside_x, outside_y, outside_z, centre_x - outside_x,
                    centre_y - outside_y, centre_z - outside_z);
    const double towards_x = blocked.hit ? blocked.x : centre_x;
    const double towards_y = blocked.hit ? blocked.y : centre_y;
    const double towards_z = blocked.hit ? blocked.z : centre_z;
    if (!clip_box(box, outside_x, outside_y, outside_z, towards_x, towards_y, towards_z, &hit_x,
                  &hit_y, &hit_z))
      continue;
    out.push_back(EntityHit{&target, hit_x, hit_y, hit_z, 0.0});
  }
  for (EntityHit& hit : out) {
    const double ex = hit.target->x - from_x;
    const double ey = hit.target->y - from_y;
    const double ez = hit.target->z - from_z;
    hit.distance = ex * ex + ey * ey + ez * ez;
  }
  std::sort(out.begin(), out.end(),
            [](const EntityHit& a, const EntityHit& b) { return a.distance < b.distance; });
  return out;
}

// `Projectile.isOutsideOwnerCollisionRange`: has the shot left its firer's box,
// expanded by its own movement and inflated by a whole block?
bool outside_owner_collision_range(const ProjectileEnvironment& env, const Projectile& projectile,
                                   const ProjectileBox& box, double dx, double dy, double dz) {
  if (projectile.owner_entity_id < 0)
    return true;
  const ProjectileBox swept = box_inflate(box_expand_towards(box, dx, dy, dz), 1.0, 1.0, 1.0);
  for (const ProjectileTarget& target : env.targets) {
    if (target.entity_id != projectile.owner_entity_id)
      continue;
    return !box_intersects(swept, projectile_target_box(target.x, target.y, target.z,
                                                        target.half_width, target.height));
  }
  return true;
}

// `Level.noCollision(new AABB(position, position).inflate(0.06))` in a
// solid-or-not world: is any cell the 0.12 cube around the point overlaps solid?
bool position_has_collision(const ProjectileEnvironment& env, double x, double y, double z) {
  if (!env.is_solid)
    return false;
  const int x0 = static_cast<int>(std::floor(x - kArrowFallProbe));
  const int x1 = static_cast<int>(std::floor(x + kArrowFallProbe));
  const int y0 = static_cast<int>(std::floor(y - kArrowFallProbe));
  const int y1 = static_cast<int>(std::floor(y + kArrowFallProbe));
  const int z0 = static_cast<int>(std::floor(z - kArrowFallProbe));
  const int z1 = static_cast<int>(std::floor(z + kArrowFallProbe));
  for (int bx = x0; bx <= x1; ++bx)
    for (int by = y0; by <= y1; ++by)
      for (int bz = z0; bz <= z1; ++bz)
        if (env.is_solid(bx, by, bz))
          return true;
  return false;
}

// The item stack a projectile was fired from, as Power and Punch see it.
MeleeEnchantments arrow_enchantments(const Projectile& projectile, const ProjectileTarget& target) {
  if (projectile.weapon.empty())
    return MeleeEnchantments{};
  return item_melee_enchantments(projectile.weapon, target.entity_type, kArrowAttackerType);
}

} // namespace

// ---- kinds -------------------------------------------------------------------

const char* projectile_entity_type_name(ProjectileKind kind) {
  switch (kind) {
  case ProjectileKind::kArrow:
    return "minecraft:arrow";
  case ProjectileKind::kSpectralArrow:
    return "minecraft:spectral_arrow";
  case ProjectileKind::kSnowball:
    return "minecraft:snowball";
  case ProjectileKind::kEgg:
    return "minecraft:egg";
  case ProjectileKind::kEnderPearl:
    return "minecraft:ender_pearl";
  case ProjectileKind::kWitherSkull:
    return "minecraft:wither_skull";
  }
  return "";
}

std::string_view projectile_item_name(ProjectileKind kind) {
  // Snowball/ThrownEgg/ThrownEnderpearl's `getDefaultItem`, and the arrow's
  // `ArrowItem.createArrow` pickup stack — the item the thrown entity carries
  // and, for an arrow, the one it would be picked up as.
  switch (kind) {
  case ProjectileKind::kArrow:
    return "minecraft:arrow";
  case ProjectileKind::kSpectralArrow:
    return "minecraft:spectral_arrow";
  case ProjectileKind::kSnowball:
    return "minecraft:snowball";
  case ProjectileKind::kEgg:
    return "minecraft:egg";
  case ProjectileKind::kEnderPearl:
    return "minecraft:ender_pearl";
  case ProjectileKind::kWitherSkull:
    return std::string_view(); // a skull is not a thrown item and cannot be picked up
  }
  return std::string_view();
}

bool projectile_is_arrow(ProjectileKind kind) {
  return kind == ProjectileKind::kArrow || kind == ProjectileKind::kSpectralArrow;
}

std::optional<ProjectileKind> thrown_projectile_kind(std::string_view item_name) {
  // `SnowballItem`, `EggItem` and `EnderpearlItem` are registered in code
  // (`Items.java`), so no data report names them; see the header.
  if (item_name == "minecraft:snowball")
    return ProjectileKind::kSnowball;
  if (item_name == "minecraft:egg")
    return ProjectileKind::kEgg;
  if (item_name == "minecraft:ender_pearl")
    return ProjectileKind::kEnderPearl;
  return std::nullopt;
}

ProjectileKind arrow_kind_for_item(std::string_view item_name) {
  // `SpectralArrowItem.createArrow` is the one override that changes the entity:
  // `ArrowItem.createArrow` returns a plain `Arrow` for every other item in
  // `minecraft:arrows`, so a tipped arrow is still an arrow entity.
  if (item_name == "minecraft:spectral_arrow")
    return ProjectileKind::kSpectralArrow;
  return ProjectileKind::kArrow;
}

// ---- the bow -----------------------------------------------------------------

float bow_power_for_time(int time_held_ticks) {
  // BowItem.getPowerForTime, in the same single-precision arithmetic the
  // decompile uses — the `pow == 1.0f` a caller compares against is exact only
  // because the clamp is `> 1.0f` on the quadratic, which reaches exactly 1.0 at
  // twenty ticks.
  float pow = static_cast<float>(time_held_ticks) / 20.0f;
  pow = (pow * pow + pow * 2.0f) / 3.0f;
  if (pow > 1.0f)
    pow = 1.0f;
  return pow;
}

BowShot bow_shot(int time_held_ticks) {
  BowShot shot;
  shot.power = bow_power_for_time(time_held_ticks);
  // `if ((double)pow < 0.1) return false;` — the release does not fire at all.
  shot.fires = !(static_cast<double>(shot.power) < 0.1);
  // BowItem.releaseUsing passes `pow == 1.0f` as the critical flag.
  shot.critical = shot.fires && shot.power == 1.0f;
  return shot;
}

// ---- boxes and rotation ------------------------------------------------------

ProjectileBox projectile_box(double x, double y, double z, double half_width, double height) {
  ProjectileBox box;
  box.min_x = x - half_width;
  box.max_x = x + half_width;
  // EntityDimensions.makeBoundingBox: the position's y is the box's *bottom*.
  box.min_y = y;
  box.max_y = y + height;
  box.min_z = z - half_width;
  box.max_z = z + half_width;
  return box;
}

ProjectileBox projectile_box(const Projectile& projectile) {
  if (projectile_is_arrow(projectile.kind))
    return projectile_box(projectile.x, projectile.y, projectile.z, kArrowWidth * 0.5,
                          kArrowHeight);
  if (projectile.kind == ProjectileKind::kWitherSkull)
    return projectile_box(projectile.x, projectile.y, projectile.z, kWitherSkullHalfWidth,
                          kWitherSkullHeight);
  return projectile_box(projectile.x, projectile.y, projectile.z, kThrowableWidth * 0.5,
                        kThrowableHeight);
}

ProjectileBox projectile_target_box(double x, double y, double z, double half_width,
                                    double height) {
  return projectile_box(x, y, z, half_width, height);
}

float projectile_entity_margin(int tick_count) {
  return static_cast<float>(
      std::max(0.0, std::min(0.3, (static_cast<double>(tick_count) - 2.0) / 20.0)));
}

void projectile_spawn_rotation(const Projectile& projectile, float* yaw, float* pitch) {
  const double horizontal =
      std::sqrt(projectile.vx * projectile.vx + projectile.vz * projectile.vz);
  if (yaw != nullptr)
    *yaw = static_cast<float>(std::atan2(projectile.vx, projectile.vz) * kDegreesPerRadian);
  if (pitch != nullptr)
    *pitch = static_cast<float>(std::atan2(projectile.vy, horizontal) * kDegreesPerRadian);
}

// ---- launching ---------------------------------------------------------------

namespace {

// `Projectile.getMovementToShoot` + `shootFromRotation`'s aim, shared by the bow
// and the three throwables: the horizontal aim comes from the *unmodified* pitch
// and only the vertical component takes the throw's pitch offset, then the
// unit vector carries one triangle spread per axis and the whole thing is scaled
// by the power. The firer's own movement is added last.
void launch(Projectile& projectile, double owner_vx, double owner_vy, double owner_vz,
            bool owner_on_ground, float yaw, float pitch, double pitch_offset, double power,
            const double unit_draws[6]) {
  const double x_rot = degrees_to_radians(static_cast<double>(pitch));
  const double y_rot = degrees_to_radians(static_cast<double>(yaw));
  const double shot_pitch = degrees_to_radians(static_cast<double>(pitch) + pitch_offset);
  const double aim_x = -std::sin(y_rot) * std::cos(x_rot);
  const double aim_y = -std::sin(shot_pitch);
  const double aim_z = std::cos(y_rot) * std::cos(x_rot);

  const double length = std::sqrt(aim_x * aim_x + aim_y * aim_y + aim_z * aim_z);
  const double spread = kProjectileSpread * static_cast<double>(kThrowableThrowUncertainty);
  const double jitter_x = spread * (unit_draws[0] - unit_draws[1]);
  const double jitter_y = spread * (unit_draws[2] - unit_draws[3]);
  const double jitter_z = spread * (unit_draws[4] - unit_draws[5]);
  const double scale = length > 0.0 ? power / length : 0.0;
  projectile.vx = aim_x * scale + jitter_x * power;
  projectile.vy = aim_y * scale + jitter_y * power;
  projectile.vz = aim_z * scale + jitter_z * power;

  // `setDeltaMovement(delta.add(sourceMovement.x, source.onGround() ? 0.0 :
  // sourceMovement.y, sourceMovement.z))`.
  projectile.vx += owner_vx;
  projectile.vy += owner_on_ground ? 0.0 : owner_vy;
  projectile.vz += owner_vz;
}

} // namespace

void shoot_arrow(Projectile& projectile, ProjectileKind kind, const ItemStack& weapon,
                 int32_t owner_entity_id, double owner_x, double owner_y, double owner_z,
                 float owner_yaw, float owner_pitch, double owner_vx, double owner_vy,
                 double owner_vz, bool owner_on_ground, const BowShot& shot,
                 const double unit_draws[6]) {
  projectile.kind = kind;
  projectile.weapon = weapon;
  projectile.owner_entity_id = owner_entity_id;
  // `AbstractArrow(type, mob, level, ...)` spawns at the firer's x/z and
  // `mob.getEyeY() - 0.1f` for y, which is the *bottom* of the arrow's box.
  projectile.x = owner_x;
  projectile.y = owner_y - kThrowableSpawnEyeOffset;
  projectile.z = owner_z;
  projectile.left_owner = false;
  projectile.age = 0;
  projectile.life = 0;
  projectile.in_ground = false;
  projectile.in_ground_time = 0;
  projectile.critical = shot.critical;
  projectile.base_damage = kArrowBaseDamage;
  // The spawn's own Add Entity burst carries the datum (`make_arrow_data`), so a
  // fresh arrow has nothing to resend.
  projectile.in_ground_changed = false;
  // A release the power gate refused fires nothing; the caller is expected to
  // have asked `bow_shot` first, and this keeps a stray call from leaving a
  // velocity behind.
  if (!shot.fires) {
    projectile.vx = projectile.vy = projectile.vz = 0.0;
    return;
  }
  // `BowItem.releaseUsing` -> `shootProjectile` -> `shootFromRotation(shooter,
  // shooter.getXRot(), shooter.getYRot() + angle, 0.0f, pow * 3.0f, 1.0f)`. The
  // `angle` is the multishot spread, which a bow without Multishot has as 0.
  launch(projectile, owner_vx, owner_vy, owner_vz, owner_on_ground, owner_yaw, owner_pitch,
         /*pitch_offset=*/0.0, static_cast<double>(shot.power) * kBowVelocityMultiplier,
         unit_draws);
}

void launch_wither_skull(Projectile& projectile, int32_t owner_entity_id, double owner_x,
                         double owner_y, double owner_z, float owner_yaw, float owner_pitch,
                         const double unit_draws[6]) {
  projectile.kind = ProjectileKind::kWitherSkull;
  projectile.owner_entity_id = owner_entity_id;
  projectile.x = owner_x;
  projectile.y = owner_y;
  projectile.z = owner_z;
  projectile.left_owner = false;
  projectile.age = 0;
  projectile.life = 0;
  projectile.in_ground = false;
  projectile.in_ground_time = 0;
  projectile.in_ground_changed = false;
  projectile.critical = false;
  // `WitherSkull` is an `AbstractHurtingProjectile`: `shoot(..., 0.6F, 0.5F)` in
  // `WitherBoss.performRangedAttack`, with no gravity and the 0.95 inertia its
  // class sets. The eye is the spawn point (the arrow's -0.1 is not its own).
  launch(projectile, 0.0, 0.0, 0.0, false, owner_yaw, owner_pitch, 0.0, 0.6, unit_draws);
}

void throw_item_projectile(Projectile& projectile, ProjectileKind kind, int32_t owner_entity_id,
                           double owner_x, double owner_y, double owner_z, float owner_yaw,
                           float owner_pitch, double owner_vx, double owner_vy, double owner_vz,
                           bool owner_on_ground, const double unit_draws[6]) {
  projectile.kind = kind;
  projectile.owner_entity_id = owner_entity_id;
  projectile.x = owner_x;
  projectile.y = owner_y - kThrowableSpawnEyeOffset;
  projectile.z = owner_z;
  projectile.left_owner = false;
  projectile.age = 0;
  projectile.life = 0;
  projectile.in_ground = false;
  projectile.in_ground_time = 0;
  projectile.in_ground_changed = false;
  projectile.critical = false;
  // `SnowballItem.use` and its two siblings: `spawnProjectileFromRotation(...,
  // 0.0f, 1.5f, 1.0f)` — no pitch offset at all, unlike the potions' -20.
  launch(projectile, owner_vx, owner_vy, owner_vz, owner_on_ground, owner_yaw, owner_pitch,
         static_cast<double>(kThrowableThrowPitchOffset), static_cast<double>(kThrowableShootPower),
         unit_draws);
}

// ---- the tick ----------------------------------------------------------------

namespace {

// `AbstractArrow.onHitBlock`: the 0.05 step back along the sign of the velocity
// it buried with, a full stop, and the arrow is in the ground.
void bury_arrow(Projectile& projectile, const SolidHit& block) {
  const double sign_x = projectile.vx > 0.0 ? 1.0 : (projectile.vx < 0.0 ? -1.0 : 0.0);
  const double sign_y = projectile.vy > 0.0 ? 1.0 : (projectile.vy < 0.0 ? -1.0 : 0.0);
  const double sign_z = projectile.vz > 0.0 ? 1.0 : (projectile.vz < 0.0 ? -1.0 : 0.0);
  projectile.x -= sign_x * kArrowGroundBackoff;
  projectile.y -= sign_y * kArrowGroundBackoff;
  projectile.z -= sign_z * kArrowGroundBackoff;
  projectile.vx = projectile.vy = projectile.vz = 0.0;
  projectile.in_ground = true;
  projectile.in_ground_changed = true;
  projectile.in_ground_time = 0;
  // `setCritArrow(false)`, `setPierceLevel(0)`, `resetPiercedEntities()`.
  projectile.critical = false;
  projectile.pierce_level = 0;
  projectile.pierced_entities.clear();
  (void)block;
}

void fill_block_half(ProjectileImpact* impact, const SolidHit& block, double x, double y,
                     double z) {
  impact->hit_block = true;
  impact->block_x = block.block_x;
  impact->block_y = block.block_y;
  impact->block_z = block.block_z;
  impact->face = block.face;
  impact->x = x;
  impact->y = y;
  impact->z = z;
}

// `Snowball.onHit` / `ThrownEgg.onHit` / `ThrownEnderpearl.onHit`: whatever was
// struck, the entity breaks — and an egg rolls for its chickens and a pearl
// teleports its thrower, both of which happen on a *block* hit as much as an
// entity one.
void finish_throwable_hit(Projectile& projectile, ProjectileImpact& impact,
                          const ProjectileEnvironment& env) {
  if (projectile.kind == ProjectileKind::kEgg) {
    // `if (random.nextInt(8) == 0) { count = 1; if (random.nextInt(32) == 0) count = 4; }`
    if (env.next_int && env.next_int(kEggChickenRoll) == 0) {
      impact.chickens = 1;
      if (env.next_int && env.next_int(kEggChickenQuadRoll) == 0)
        impact.chickens = 4;
    }
  } else if (projectile.kind == ProjectileKind::kEnderPearl && projectile.owner_entity_id >= 0) {
    // `this.oldPosition()`: where the pearl stood when *this* tick started, not
    // the impact point. Whether the owner may be teleported at all is the
    // caller's question (the owner's own liveness and dimension).
    impact.teleports_owner = true;
    impact.teleport_x = projectile.old_x;
    impact.teleport_y = projectile.old_y;
    impact.teleport_z = projectile.old_z;
  }
  impact.discarded = true;
  projectile.removed = true;
}

// `AbstractArrow.onHitEntity`, returning whether the arrow is still alive (the
// `if (entity.hurt(...))` branch's outcome decides its fate).
bool resolve_arrow_entity_hit(Projectile& projectile, const ProjectileTarget& target,
                              const ProjectileEnvironment& env, ProjectileTickReport* report) {
  ProjectileImpact impact;
  impact.projectile_entity_id = projectile.entity_id;
  impact.kind = projectile.kind;
  impact.owner_entity_id = projectile.owner_entity_id;
  impact.x = projectile.x;
  impact.y = projectile.y;
  impact.z = projectile.z;
  impact.hit_entity = true;
  impact.target_entity_id = target.entity_id;
  impact.target_is_player = target.is_player;

  // `WitherBoss.isInvulnerableTo`: at or below half health an arrow cannot hurt
  // the wither, so `hurt` is never asked and the arrow is spent. (Vanilla's
  // `deflect` would bounce it; a spent arrow is the same removal to the client.)
  if (target.arrow_immune) {
    impact.damage = 0.0f;
    impact.accepted = false;
    impact.discarded = true;
    projectile.removed = true;
    report->removed.push_back(projectile.entity_id);
    report->impacts.push_back(std::move(impact));
    return false;
  }

  const MeleeEnchantments enchants = arrow_enchantments(projectile, target);
  const double arrow_damage = projectile.base_damage + static_cast<double>(enchants.damage);
  const double raw = delta_length(projectile) * arrow_damage;
  int64_t damage = static_cast<int64_t>(std::ceil(std::min(std::max(raw, 0.0), 2147483647.0)));

  if (projectile.pierce_level > 0) {
    // `piercingIgnoreEntityIds`: the ids already pierced. The budget is
    // `pierce_level + 1` victims, and the *next* one discards the arrow before
    // anything is damaged.
    if (static_cast<int>(projectile.pierced_entities.size()) < projectile.pierce_level + 1) {
      projectile.pierced_entities.push_back(target.entity_id);
    } else {
      impact.discarded = true;
      projectile.removed = true;
      report->removed.push_back(projectile.entity_id);
      report->impacts.push_back(std::move(impact));
      return false;
    }
  }
  if (projectile.critical) {
    // `random.nextInt(damage / 2 + 2)` added to the damage, clamped to INT_MAX.
    const int64_t bonus = env.next_int ? env.next_int(static_cast<int>(damage / 2 + 2)) : 0;
    damage = std::min(damage + bonus, static_cast<int64_t>(2147483647));
  }
  impact.damage = static_cast<float>(damage);
  impact.critical = projectile.critical;

  // `doKnockback` (Punch): the horizontal velocity, unit, times
  // `knockback * 0.6 * resistance`, as one flat `Entity.push(x, 0.1, z)`.
  const double knockback = static_cast<double>(enchants.knockback);
  if (knockback > 0.0) {
    const double resistance = std::max(0.0, 1.0 - target.knockback_resistance);
    const double horizontal =
        std::sqrt(projectile.vx * projectile.vx + projectile.vz * projectile.vz);
    if (horizontal > 1.0e-5) {
      const double push_x = projectile.vx / horizontal * knockback * 0.6 * resistance;
      const double push_z = projectile.vz / horizontal * knockback * 0.6 * resistance;
      if (push_x * push_x + push_z * push_z > 0.0) {
        impact.push_x = push_x;
        impact.push_y = 0.1;
        impact.push_z = push_z;
      }
    }
  }

  // `SpectralArrow.doPostHurtEffects` (vanilla calls it inside the `if
  // (hurt)` branch, so the field is a statement of what lands *if* the hit is
  // accepted — the callback below does the accepting).
  impact.glowing_ticks =
      projectile.kind == ProjectileKind::kSpectralArrow ? kSpectralArrowGlowTicks : 0;
  impact.accepted = env.hurt ? env.hurt(impact) : false;
  if (impact.accepted) {
    if (projectile.pierce_level <= 0) {
      impact.discarded = true;
      projectile.removed = true;
    }
  } else {
    // `deflect(REVERSE, ...)` then `setDeltaMovement(delta.scale(0.2))`, and an
    // arrow that has all but stopped is discarded.
    projectile.vx *= 0.2;
    projectile.vy *= 0.2;
    projectile.vz *= 0.2;
    if (projectile.vx * projectile.vx + projectile.vy * projectile.vy +
            projectile.vz * projectile.vz <
        1.0e-7) {
      impact.discarded = true;
      projectile.removed = true;
    }
  }
  if (projectile.removed)
    report->removed.push_back(projectile.entity_id);
  report->impacts.push_back(std::move(impact));
  return !projectile.removed;
}

// `AbstractArrow.tick` + `stepMoveAndHit`. Returns true when the projectile must
// be erased from the vector.
bool tick_arrow(Projectile& projectile, const ProjectileEnvironment& env,
                ProjectileTickReport* report) {
  // `tick`'s first block: an arrow that finds itself inside a solid cell stops
  // there and buries itself. Vanilla tests the cell's collision shape against
  // the arrow's own position; this engine knows only solid-or-not, so the cell
  // the position is in is the test.
  if (!projectile.in_ground && env.is_solid &&
      env.is_solid(static_cast<int>(std::floor(projectile.x)),
                   static_cast<int>(std::floor(projectile.y)),
                   static_cast<int>(std::floor(projectile.z)))) {
    projectile.vx = projectile.vy = projectile.vz = 0.0;
    projectile.in_ground = true;
    // `setInGround(true)`, which is a synched datum: this tick decides it does
    // not move on its own, so the flag is what puts it in the report below.
    projectile.in_ground_changed = true;
  }

  if (projectile.in_ground) {
    // `if (lastState != blockState && shouldFall()) startFalling(); else
    // tickDespawn();` — vanilla's `lastState != blockState` short-circuit is not
    // modelled (it only avoids re-testing a cell nothing has written to).
    ++projectile.in_ground_time;
    if (!position_has_collision(env, projectile.x, projectile.y, projectile.z)) {
      // `startFalling`: un-stick, scale the (already zero, because burying zeroes
      // it) movement by three random 0..0.2 factors, and restart the clock.
      projectile.in_ground = false;
      // `setInGround(false)` is a synched datum too, and this tick is the tick
      // the client has to be told on: an in-ground arrow that is never told it
      // came free keeps drawing itself stuck while the server drops it. The
      // report carries the arrow for that datum, because the sync list is what
      // the caller writes arrow data from (it does not move on this tick, so
      // `moved` here is the datum and not a position).
      projectile.in_ground_changed = true;
      report->moved.push_back(projectile.entity_id);
      const double rx = env.unit_random ? env.unit_random() : 0.0;
      const double ry = env.unit_random ? env.unit_random() : 0.0;
      const double rz = env.unit_random ? env.unit_random() : 0.0;
      projectile.vx *= rx * 0.2;
      projectile.vy *= ry * 0.2;
      projectile.vz *= rz * 0.2;
      projectile.life = 0;
      return false;
    }
    // `tickDespawn`: only a buried arrow counts up, and only a buried one can
    // reach 1200.
    ++projectile.life;
    if (projectile.life >= kArrowDespawnTicks) {
      projectile.removed = true;
      report->removed.push_back(projectile.entity_id);
      return true;
    }
    // An in-ground arrow does not move, so it needs no position sync — it was
    // synced on the tick it buried itself. The one exception is the tick that
    // *found* it inside a solid cell above: that bury is a datum change, and so
    // is the only reason this arrow is reported at all.
    if (projectile.in_ground_changed)
      report->moved.push_back(projectile.entity_id);
    return false;
  }
  projectile.in_ground_time = 0;

  const double dx = projectile.vx;
  const double dy = projectile.vy;
  const double dz = projectile.vz;
  const float margin = projectile_entity_margin(projectile.age);

  // `clipIncludingBorder(new ClipContext(originalPosition,
  // originalPosition.add(movement), COLLIDER, Fluid.NONE, this))` — computed
  // once, before the piercing loop, from the tick's starting position.
  const SolidHit block =
      clip_solids(env.is_solid, projectile.x, projectile.y, projectile.z, dx, dy, dz);
  const double block_x = block.hit ? block.x : projectile.x + dx;
  const double block_y = block.hit ? block.y : projectile.y + dy;
  const double block_z = block.hit ? block.z : projectile.z + dz;

  while (!projectile.removed) {
    // `initialPosition = this.position()` — it follows the piercing loop.
    const double from_x = projectile.x;
    const double from_y = projectile.y;
    const double from_z = projectile.z;
    const std::vector<EntityHit> hits = find_hit_entities(
        env, projectile, from_x, from_y, from_z, block_x, block_y, block_z, dx, dy, dz, margin);
    if (hits.empty()) {
      projectile.x = block_x;
      projectile.y = block_y;
      projectile.z = block_z;
      if (!block.hit)
        break;
      // `hitTargetOrDeflectSelf(blockHitResult)` -> `onHitBlock`.
      ProjectileImpact impact;
      impact.projectile_entity_id = projectile.entity_id;
      impact.kind = projectile.kind;
      impact.owner_entity_id = projectile.owner_entity_id;
      fill_block_half(&impact, block, block.x, block.y, block.z);
      impact.stuck = true;
      bury_arrow(projectile, block);
      report->impacts.push_back(std::move(impact));
      break;
    }

    // `setPos(nextLocation)` with the nearest entity's clip point.
    projectile.x = hits[0].x;
    projectile.y = hits[0].y;
    projectile.z = hits[0].z;
    // `hitTargetsOrDeflectSelf(entitiesHit)` walks the list in that order and
    // stops at the first arrow that is no longer alive.
    for (const EntityHit& hit : hits) {
      if (!resolve_arrow_entity_hit(projectile, *hit.target, env, report))
        break;
    }
    // `if (getPierceLevel() > 0 && deflection == NONE) continue;` — otherwise
    // the arrow stops here.
    if (projectile.pierce_level > 0)
      continue;
    break;
  }
  if (projectile.removed)
    return true;

  // The drag, then the gravity — *after* the movement, which is the arrow's own
  // order (`stepMoveAndHit` runs before `applyInertia` before `applyGravity`;
  // `ThrowableProjectile.tick` is the other way round). The drag always runs and
  // the gravity is gated on `!isInGround()`, so an arrow that buried itself on
  // this tick keeps the zero velocity burying gave it.
  projectile.vx *= kArrowAirDrag;
  projectile.vy *= kArrowAirDrag;
  projectile.vz *= kArrowAirDrag;
  if (!projectile.in_ground)
    projectile.vy -= kArrowGravity;

  ++projectile.age;
  // `checkLeftOwner` runs after the movement, so the flag a tick's hit test
  // reads was computed from the previous tick's *new* box.
  if (!projectile.left_owner)
    projectile.left_owner =
        outside_owner_collision_range(env, projectile, projectile_box(projectile), dx, dy, dz);
  // `stepMoveAndHit` sets `needsSync` on the tick it resolves a hit, and `move`
  // does on every other moving tick, so a tick that ran the movement step is
  // always a sync — including the one it buried itself on.
  report->moved.push_back(projectile.entity_id);
  return false;
}

bool tick_throwable(Projectile& projectile, const ProjectileEnvironment& env,
                    ProjectileTickReport* report) {
  // `ThrowableProjectile.tick`: `applyGravity()`, `applyInertia()`, then the
  // move with a single resolved hit.
  projectile.vy -= kThrowableGravity;
  projectile.vx *= kThrowableAirDrag;
  projectile.vy *= kThrowableAirDrag;
  projectile.vz *= kThrowableAirDrag;

  const double dx = projectile.vx;
  const double dy = projectile.vy;
  const double dz = projectile.vz;
  const float margin = projectile_entity_margin(projectile.age);

  // `ProjectileUtil.getHitResultOnMoveVector`: the block clip shortens the
  // segment and the nearest entity along it replaces the block outright.
  const SolidHit block =
      clip_solids(env.is_solid, projectile.x, projectile.y, projectile.z, dx, dy, dz);
  const double to_x = block.hit ? block.x : projectile.x + dx;
  const double to_y = block.hit ? block.y : projectile.y + dy;
  const double to_z = block.hit ? block.z : projectile.z + dz;
  const NearestHit entity = clip_nearest_entity(env, projectile, projectile.x, projectile.y,
                                                projectile.z, to_x, to_y, to_z, dx, dy, dz, margin);

  if (entity.hit || block.hit) {
    const double hit_x = entity.hit ? entity.x : block.x;
    const double hit_y = entity.hit ? entity.y : block.y;
    const double hit_z = entity.hit ? entity.z : block.z;
    ProjectileImpact impact;
    impact.projectile_entity_id = projectile.entity_id;
    impact.kind = projectile.kind;
    impact.owner_entity_id = projectile.owner_entity_id;
    impact.x = hit_x;
    impact.y = hit_y;
    impact.z = hit_z;
    if (block.hit)
      fill_block_half(&impact, block, hit_x, hit_y, hit_z);
    if (entity.hit) {
      impact.hit_entity = true;
      impact.hit_block = block.hit;
      impact.target_entity_id = entity.target->entity_id;
      impact.target_is_player = entity.target->is_player;
      // `Snowball.onHitEntity`: three damage to a blaze and nothing to
      // everything else. An egg and a pearl hurt for 0, and all three still run
      // through `hurt` (which is what lets a caller's `hurt` be the same one the
      // arrows use).
      impact.damage = (projectile.kind == ProjectileKind::kSnowball && entity.target->is_blaze)
                          ? kSnowballBlazeDamage
                          : 0.0f;
      impact.accepted = env.hurt ? env.hurt(impact) : false;
    }
    finish_throwable_hit(projectile, impact, env);
    // The position is the impact point, as it is for a potion.
    projectile.x = hit_x;
    projectile.y = hit_y;
    projectile.z = hit_z;
    report->removed.push_back(projectile.entity_id);
    report->impacts.push_back(std::move(impact));
    return true;
  }

  projectile.x = to_x;
  projectile.y = to_y;
  projectile.z = to_z;
  ++projectile.age;
  if (!projectile.left_owner)
    projectile.left_owner =
        outside_owner_collision_range(env, projectile, projectile_box(projectile), dx, dy, dz);
  report->moved.push_back(projectile.entity_id);
  return false;
}

// `WitherSkull.tick`: `AbstractHurtingProjectile`'s move-then-inertia order,
// with no gravity, and a first hit that spends the skull whatever it struck. The
// damage is the class's flat 8 through `minecraft:wither_skull`; a charged (blue)
// skull is a different entity and is not modelled.
bool tick_wither_skull(Projectile& projectile, const ProjectileEnvironment& env,
                       ProjectileTickReport* report) {
  const double dx = projectile.vx;
  const double dy = projectile.vy;
  const double dz = projectile.vz;
  const float margin = projectile_entity_margin(projectile.age);
  const SolidHit block =
      clip_solids(env.is_solid, projectile.x, projectile.y, projectile.z, dx, dy, dz);
  const double to_x = block.hit ? block.x : projectile.x + dx;
  const double to_y = block.hit ? block.y : projectile.y + dy;
  const double to_z = block.hit ? block.z : projectile.z + dz;
  const NearestHit entity = clip_nearest_entity(env, projectile, projectile.x, projectile.y,
                                                projectile.z, to_x, to_y, to_z, dx, dy, dz, margin);
  if (entity.hit || block.hit) {
    const double hit_x = entity.hit ? entity.x : block.x;
    const double hit_y = entity.hit ? entity.y : block.y;
    const double hit_z = entity.hit ? entity.z : block.z;
    ProjectileImpact impact;
    impact.projectile_entity_id = projectile.entity_id;
    impact.kind = projectile.kind;
    impact.owner_entity_id = projectile.owner_entity_id;
    impact.x = hit_x;
    impact.y = hit_y;
    impact.z = hit_z;
    if (block.hit)
      fill_block_half(&impact, block, hit_x, hit_y, hit_z);
    if (entity.hit) {
      impact.hit_entity = true;
      impact.hit_block = block.hit;
      impact.target_entity_id = entity.target->entity_id;
      impact.target_is_player = entity.target->is_player;
      impact.damage = kWitherSkullDamage;
      impact.accepted = env.hurt ? env.hurt(impact) : false;
    }
    projectile.x = hit_x;
    projectile.y = hit_y;
    projectile.z = hit_z;
    impact.discarded = true;
    projectile.removed = true;
    report->removed.push_back(projectile.entity_id);
    report->impacts.push_back(std::move(impact));
    return true;
  }
  projectile.x = to_x;
  projectile.y = to_y;
  projectile.z = to_z;
  projectile.vx *= kWitherSkullDrag;
  projectile.vy *= kWitherSkullDrag;
  projectile.vz *= kWitherSkullDrag;
  ++projectile.age;
  if (!projectile.left_owner)
    projectile.left_owner =
        outside_owner_collision_range(env, projectile, projectile_box(projectile), dx, dy, dz);
  report->moved.push_back(projectile.entity_id);
  return false;
}

} // namespace

ProjectileTickReport tick_projectiles(std::vector<Projectile>& projectiles,
                                      const ProjectileEnvironment& env) {
  ProjectileTickReport report;
  for (size_t index = 0; index < projectiles.size();) {
    Projectile& projectile = projectiles[index];
    if (projectile.removed) {
      report.removed.push_back(projectile.entity_id);
      projectiles.erase(projectiles.begin() + static_cast<ptrdiff_t>(index));
      continue;
    }
    // `ServerLevel.tickNonPassenger` calls `setOldPosAndRot` before `tick()`, so
    // an ender pearl's `oldPosition()` is where it stood when the tick started.
    projectile.old_x = projectile.x;
    projectile.old_y = projectile.y;
    projectile.old_z = projectile.z;
    // `in_ground_changed` means "this tick's transition", so it is cleared here
    // and set only by the code above that makes the transition (see the field).
    projectile.in_ground_changed = false;
    const bool erase =
        projectile.kind == ProjectileKind::kWitherSkull
            ? tick_wither_skull(projectile, env, &report)
            : (projectile_is_arrow(projectile.kind) ? tick_arrow(projectile, env, &report)
                                                    : tick_throwable(projectile, env, &report));
    if (erase) {
      projectiles.erase(projectiles.begin() + static_cast<ptrdiff_t>(index));
      continue;
    }
    ++index;
  }
  return report;
}

} // namespace mc::world
