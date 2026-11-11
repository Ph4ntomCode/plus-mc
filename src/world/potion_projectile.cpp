// SPDX-License-Identifier: GPL-3.0-or-later
//
// Thrown potions and lingering clouds: the launch, the movement, the impact and
// `AreaEffectCloud.serverTick`. See world/potion_projectile.h for what is
// modelled and where the boundary sits.
#include "world/potion_projectile.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "world/effect_table.h"
#include "world/potion.h"
#include "world/voxel_clip.h"

namespace mc::world {
namespace {

constexpr double kPi = 3.14159265358979323846;

double degrees_to_radians(double degrees) {
  return degrees * kPi / 180.0;
}

// The effect list a potion applies, in `getAllEffects` order.
std::vector<MobEffectInstance> resolved_effects(const StackPotionContents& contents) {
  return potion_all_effects(contents);
}

// The splash path's own mapping, `mapDuration(d -> (int)(scale * d *
// durationScale + 0.5))`: the falloff and the stack's duration scale collapse
// into one factor, and the `+ 0.5` rounds. Unlike the cloud's (below) it can
// reach zero, which is why the caller then drops anything that ends within 20
// ticks.
int splash_duration(int duration, double factor) {
  if (duration == kInfiniteEffectDuration || duration == 0)
    return duration;
  return static_cast<int>(factor * static_cast<double>(duration) + 0.5);
}

// `MobEffectInstance.withScaledDuration(scale)`, which is what
// `PotionContents.forEachEffect` applies (and so what a cloud uses):
// `mapDuration(d -> max(floor(d * scale), 1))` — never zero.
int scaled_duration(int duration, double scale) {
  if (duration == kInfiniteEffectDuration || duration == 0)
    return duration;
  return std::max(static_cast<int>(std::floor(static_cast<double>(duration) * scale)), 1);
}

// AABB.expandTowards: grow the box by the *positive* components of the vector
// (a negative component extends the minimum side instead).
PotionBox box_expand_towards(const PotionBox& box, double dx, double dy, double dz) {
  PotionBox out = box;
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

// AABB.clip: the first point at which the segment from -> to enters the box, as
// a parameter in [0, 1] of `to - from`. Returns false when the segment never
// enters (which includes every axis-parallel miss the slab test catches) — and
// importantly the *entry* point, so a segment that starts inside the box yields
// `from` itself rather than either face.
bool clip_box(const PotionBox& box, double from_x, double from_y, double from_z, double to_x,
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

// The block clip is the shared walk (world/voxel_clip.h): the point a potion
// broke on is what the level event needs (vanilla names `this.blockPosition()`,
// the floor of the potion's own position after `setPos(hitResult.getLocation())`,
// not the `BlockHitResult`'s own `blockPos`), and the cell and face are what the
// water branch's douse half needs — `blockHitPos` and `getDirection()`.
SolidHit clip_blocks(const PotionEnvironment& env, double from_x, double from_y, double from_z,
                     double dx, double dy, double dz) {
  return clip_solids(env.is_solid, from_x, from_y, from_z, dx, dy, dz);
}

// The nearest entity the segment from -> to passes through, within `margin`.
// `ProjectileUtil.getEntityHitResult`: the candidates are the targets whose box
// intersects the projectile's own box expanded by the movement and inflated by a
// block, and the winner is the closest clip point along the segment.
struct EntityHit {
  bool hit = false;
  const PotionTarget* target = nullptr;
  double x = 0.0, y = 0.0, z = 0.0;
};

EntityHit clip_entities(const PotionEnvironment& env, const PotionProjectile& projectile,
                        const PotionBox& start_box, double dx, double dy, double dz,
                        double margin) {
  EntityHit result;
  const PotionBox search = box_inflate(box_expand_towards(start_box, dx, dy, dz), 1.0, 1.0, 1.0);
  const double to_x = projectile.x + dx;
  const double to_y = projectile.y + dy;
  const double to_z = projectile.z + dz;
  double nearest = std::numeric_limits<double>::max();
  for (const PotionTarget& target : env.targets) {
    // Entity.canBeHitByProjectile / Projectile.canHitEntity: alive and pickable,
    // and not the thrower until the potion has left their box.
    if (!target.alive)
      continue;
    if (target.entity_id == projectile.owner_entity_id && !projectile.left_owner)
      continue;
    const PotionBox box =
        potion_target_box(target.x, target.y, target.z, target.half_width, target.height);
    if (!box_intersects(search, box))
      continue;
    const PotionBox inflated = box_inflate(box, margin, margin, margin);
    double hit_x = 0.0, hit_y = 0.0, hit_z = 0.0;
    if (!clip_box(inflated, projectile.x, projectile.y, projectile.z, to_x, to_y, to_z, &hit_x,
                  &hit_y, &hit_z))
      continue;
    const double hx = hit_x - projectile.x;
    const double hy = hit_y - projectile.y;
    const double hz = hit_z - projectile.z;
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

// `Projectile.isOutsideOwnerCollisionRange`: has the potion left its thrower's
// box, expanded by its own movement and inflated by a whole block?
bool outside_owner_collision_range(const PotionEnvironment& env, const PotionProjectile& projectile,
                                   const PotionBox& box, double dx, double dy, double dz) {
  if (projectile.owner_entity_id < 0)
    return true;
  const PotionBox swept = box_inflate(box_expand_towards(box, dx, dy, dz), 1.0, 1.0, 1.0);
  for (const PotionTarget& target : env.targets) {
    if (target.entity_id != projectile.owner_entity_id)
      continue;
    return !box_intersects(
        swept, potion_target_box(target.x, target.y, target.z, target.half_width, target.height));
  }
  // The thrower is not in the environment any more (they left, or they are not a
  // target at all): there is nothing left to stay inside of.
  return true;
}

// One application, for a target the potion has already been found to reach.
// `landing` is the instance as it should land, `instant_scale` the splash
// falloff or the cloud's fixed 0.5, and `drop_short` the splash's own
// `endsWithin(20)` skip (which the cloud, applying an already-scaled instance,
// does not have).
void apply_one(const GeneratedMobEffect& def, const MobEffectInstance& landing,
               const PotionTarget& target, int32_t owner_entity_id, double instant_scale,
               bool drop_short, std::vector<PotionApplication>* out) {
  PotionApplication application;
  application.entity_id = target.entity_id;
  application.owner_entity_id = owner_entity_id;
  application.is_player = target.is_player;
  application.inverted_heal_and_harm = target.inverted_heal_and_harm;
  if (def.instantaneous) {
    // `isHarm == mob.isInvertedHealAndHarm()` heals and everything else hurts,
    // which is what makes instant health damage a zombie.
    application.instantaneous = true;
    application.harm = (def.harm != target.inverted_heal_and_harm);
    application.amount = instant_effect_amount(application.harm, landing.amplifier, instant_scale);
    application.effect = landing.effect;
    application.damage_type = kPotionHarmDamageType;
    out->push_back(std::move(application));
    return;
  }
  if (drop_short && landing.duration > 0 && landing.duration <= kSplashMinDuration)
    return;
  application.instance = landing;
  out->push_back(std::move(application));
}

// `ThrownSplashPotion.onHitAsPotion`. The potion has already been moved to its
// impact point, so vanilla's `potionAabb.move(location.subtract(position()))` is
// the identity and the falloff is `1 - sqrt(distance) / 4`.
void splash_applications(const PotionEnvironment& env, const StackPotionContents& contents,
                         float duration_scale, int32_t owner_entity_id, double x, double y,
                         double z, double margin, std::vector<PotionApplication>* out) {
  const PotionBox potion_aabb = potion_box(x, y, z);
  const PotionBox effect_aabb =
      box_inflate(potion_aabb, kPotionSplashRange, kPotionSplashInflateY, kPotionSplashRange);
  for (const PotionTarget& target : env.targets) {
    if (!target.alive)
      continue;
    const PotionBox box =
        potion_target_box(target.x, target.y, target.z, target.half_width, target.height);
    if (!box_intersects(effect_aabb, box))
      continue;
    const double distance =
        box_distance_to_sqr(potion_aabb, box_inflate(box, margin, margin, margin));
    if (!(distance < kPotionSplashRangeSq))
      continue;
    const double scale = 1.0 - std::sqrt(distance) / kPotionSplashRange;
    for (const MobEffectInstance& instance : resolved_effects(contents)) {
      const GeneratedMobEffect* def = mob_effect_by_id(instance.effect);
      if (!def)
        continue;
      MobEffectInstance landing = instance;
      // `getAllEffects` order, with the *declared* duration put through
      // `scale * d * durationScale` — both factors at once, as vanilla does.
      landing.duration =
          splash_duration(instance.duration, scale * static_cast<double>(duration_scale));
      apply_one(*def, landing, target, owner_entity_id, scale, /*drop_short=*/true, out);
    }
  }
}

// `AbstractThrownPotion.onHitAsWater`'s entity half: everyone inside the 4 x 2
// x 4 box that is water-sensitive *or* on fire, within 16 squared blocks of the
// bottle. A water-sensitive one is hurt for 1.0 `indirect_magic`; a burning one
// is extinguished — and both can be true of the same entity, in that order
// (vanilla's `if (isSensitiveToWater) hurt; if (isOnFire) extinguishFire`).
std::vector<PotionApplication> water_applications(const PotionEnvironment& env,
                                                  int32_t owner_entity_id, double x, double y,
                                                  double z) {
  std::vector<PotionApplication> out;
  for (const PotionTarget& target : env.targets) {
    if (!target.alive)
      continue;
    if (!target.sensitive_to_water && !target.on_fire)
      continue;
    const PotionBox box =
        potion_target_box(target.x, target.y, target.z, target.half_width, target.height);
    // `WATER_SENSITIVE_OR_ON_FIRE` is the filter, but the range test is vanilla's
    // `distanceToSqr(entity) < 16.0`, against the potion's own *position*.
    const double dx = target.x - x;
    const double dy = box.min_y - y;
    const double dz = target.z - z;
    if (!(dx * dx + dy * dy + dz * dz < kPotionSplashRangeSq))
      continue;
    const auto base = [&](PotionApplication* application) {
      application->entity_id = target.entity_id;
      application->owner_entity_id = owner_entity_id;
      application->is_player = target.is_player;
      application->inverted_heal_and_harm = target.inverted_heal_and_harm;
    };
    if (target.sensitive_to_water) {
      PotionApplication application;
      base(&application);
      application.instantaneous = true;
      application.harm = true; // `hurtServer(..., indirectMagic, 1.0f)`
      application.amount = 1.0f;
      application.damage_type = kPotionHarmDamageType;
      out.push_back(std::move(application));
    }
    if (target.on_fire) {
      PotionApplication application;
      base(&application);
      application.extinguish = true;
      out.push_back(std::move(application));
    }
  }
  return out;
}

// `PotionContents.getColorOptional(getAllEffects()).orElse(defaultColor)`: the
// amplifier-weighted average of the *visible* effects' own colours.
int32_t averaged_effect_color(const StackPotionContents& contents, int32_t fallback) {
  int64_t red = 0;
  int64_t green = 0;
  int64_t blue = 0;
  int64_t weight = 0;
  for (const MobEffectInstance& instance : resolved_effects(contents)) {
    if (!instance.visible)
      continue;
    const GeneratedMobEffect* def = mob_effect_by_id(instance.effect);
    if (!def)
      continue;
    const int64_t amplifier = static_cast<int64_t>(instance.amplifier) + 1;
    red += amplifier * static_cast<int64_t>((def->color >> 16) & 0xFF);
    green += amplifier * static_cast<int64_t>((def->color >> 8) & 0xFF);
    blue += amplifier * static_cast<int64_t>(def->color & 0xFF);
    weight += amplifier;
  }
  if (weight == 0)
    return fallback;
  const uint32_t r = static_cast<uint32_t>(red / weight) & 0xFF;
  const uint32_t g = static_cast<uint32_t>(green / weight) & 0xFF;
  const uint32_t b = static_cast<uint32_t>(blue / weight) & 0xFF;
  return static_cast<int32_t>(0xFF000000u | (r << 16) | (g << 8) | b);
}

} // namespace

// ---- geometry ---------------------------------------------------------------

PotionBox potion_box(double x, double y, double z) {
  return potion_target_box(x, y, z, kPotionWidth * 0.5, kPotionHeight);
}

PotionBox potion_box(const PotionProjectile& projectile) {
  return potion_box(projectile.x, projectile.y, projectile.z);
}

PotionBox potion_target_box(double x, double y, double z, double half_width, double height) {
  PotionBox box;
  box.min_x = x - half_width;
  box.max_x = x + half_width;
  // EntityDimensions.makeBoundingBox: y is the *bottom* of the box.
  box.min_y = y;
  box.max_y = y + height;
  box.min_z = z - half_width;
  box.max_z = z + half_width;
  return box;
}

PotionBox box_inflate(const PotionBox& box, double dx, double dy, double dz) {
  PotionBox out = box;
  out.min_x -= dx;
  out.max_x += dx;
  out.min_y -= dy;
  out.max_y += dy;
  out.min_z -= dz;
  out.max_z += dz;
  return out;
}

bool box_intersects(const PotionBox& a, const PotionBox& b) {
  return a.min_x < b.max_x && a.max_x > b.min_x && a.min_y < b.max_y && a.max_y > b.min_y &&
         a.min_z < b.max_z && a.max_z > b.min_z;
}

bool box_contains(const PotionBox& box, double x, double y, double z) {
  return x >= box.min_x && x <= box.max_x && y >= box.min_y && y <= box.max_y && z >= box.min_z &&
         z <= box.max_z;
}

double box_distance_to_sqr(const PotionBox& a, const PotionBox& b) {
  const double dx = std::max(std::max(a.min_x, b.min_x) - std::min(a.max_x, b.max_x), 0.0);
  const double dy = std::max(std::max(a.min_y, b.min_y) - std::min(a.max_y, b.max_y), 0.0);
  const double dz = std::max(std::max(a.min_z, b.min_z) - std::min(a.max_z, b.max_z), 0.0);
  return dx * dx + dy * dy + dz * dz;
}

float potion_entity_margin(int tick_count) {
  return static_cast<float>(
      std::max(0.0, std::min(0.3, (static_cast<double>(tick_count) - 2.0) / 20.0)));
}

// ---- launching --------------------------------------------------------------

void throw_potion(PotionProjectile& projectile, PotionProjectileKind kind,
                  const StackPotionContents& contents, float duration_scale,
                  int32_t owner_entity_id, double owner_x, double owner_y, double owner_z,
                  float owner_yaw, float owner_pitch, double owner_vx, double owner_vy,
                  double owner_vz, bool owner_on_ground, const double unit_draws[6]) {
  projectile.kind = kind;
  projectile.contents = contents;
  projectile.duration_scale = duration_scale;
  projectile.owner_entity_id = owner_entity_id;
  // `ThrowableItemProjectile(type, owner, level, stack)`: the thrower's own x/z
  // and `owner.getEyeY() - 0.1f` for y — which is the *bottom* of the potion's
  // 0.25-tall box, because that is what EntityDimensions.makeBoundingBox means
  // by a position.
  projectile.x = owner_x;
  projectile.y = owner_y - kPotionSpawnEyeOffset;
  projectile.z = owner_z;
  projectile.left_owner = false;
  projectile.age = 0;

  // `shootFromRotation(source, source.getXRot(), source.getYRot(), -20.0f,
  // 0.5f, 1.0f)`: the horizontal aim comes from the *unmodified* pitch, and only
  // the vertical component takes the -20 degree offset (which is what makes a
  // throw arc rather than shoot flat).
  const double x_rot = degrees_to_radians(static_cast<double>(owner_pitch));
  const double y_rot = degrees_to_radians(static_cast<double>(owner_yaw));
  const double throw_pitch =
      degrees_to_radians(static_cast<double>(owner_pitch) + kPotionThrowPitchOffset);
  const double aim_x = -std::sin(y_rot) * std::cos(x_rot);
  const double aim_y = -std::sin(throw_pitch);
  const double aim_z = std::cos(y_rot) * std::cos(x_rot);

  // `Projectile.getMovementToShoot`: normalize, add one triangle spread per axis
  // (`mean + spread * (a - b)`, two draws each), then scale by the power.
  const double length = std::sqrt(aim_x * aim_x + aim_y * aim_y + aim_z * aim_z);
  const double spread = kPotionSpread * static_cast<double>(kPotionThrowUncertainty);
  const double jitter_x = spread * (unit_draws[0] - unit_draws[1]);
  const double jitter_y = spread * (unit_draws[2] - unit_draws[3]);
  const double jitter_z = spread * (unit_draws[4] - unit_draws[5]);
  const double scale = length > 0.0 ? kPotionThrowPower / length : 0.0;
  projectile.vx = aim_x * scale + jitter_x * kPotionThrowPower;
  projectile.vy = aim_y * scale + jitter_y * kPotionThrowPower;
  projectile.vz = aim_z * scale + jitter_z * kPotionThrowPower;

  // `this.setDeltaMovement(delta.add(sourceMovement.x, source.onGround() ? 0.0 :
  // sourceMovement.y, sourceMovement.z))`: a running throw carries the thrower's
  // own momentum, and a falling one their downward speed.
  projectile.vx += owner_vx;
  projectile.vy += owner_on_ground ? 0.0 : owner_vy;
  projectile.vz += owner_vz;
}

std::optional<PotionProjectileKind> throwable_potion_kind(std::string_view item_name) {
  // The registry keys of the two `ThrowablePotionItem` subclasses (see the
  // header). Anything else — a plain potion, a snowball, an ender pearl — is not
  // a thrown potion: `use` either drinks it or does nothing here.
  if (item_name == "minecraft:splash_potion")
    return PotionProjectileKind::kSplash;
  if (item_name == "minecraft:lingering_potion")
    return PotionProjectileKind::kLingering;
  return std::nullopt;
}

void potion_spawn_rotation(const PotionProjectile& projectile, float* yaw, float* pitch) {
  // `DEGREES_PER_RADIAN` is the constant `Mth.atan2(...) * 57.2957763671875`
  // spells out.
  constexpr double kDegreesPerRadian = 57.2957763671875;
  const double horizontal =
      std::sqrt(projectile.vx * projectile.vx + projectile.vz * projectile.vz);
  if (yaw != nullptr)
    *yaw = static_cast<float>(std::atan2(projectile.vx, projectile.vz) * kDegreesPerRadian);
  if (pitch != nullptr)
    *pitch = static_cast<float>(std::atan2(projectile.vy, horizontal) * kDegreesPerRadian);
}

// ---- colour -----------------------------------------------------------------

int32_t potion_color(const StackPotionContents& contents) {
  // PotionContents.getColor: an explicit custom colour wins; otherwise the
  // visible effects are averaged, falling back to the base potion colour.
  if (contents.custom_color)
    return *contents.custom_color;
  return averaged_effect_color(contents, kBasePotionColor);
}

bool potion_has_instant_effects(const StackPotionContents& contents) {
  // `Potion.hasInstantEffects`: the *named* potion's own list only. A stack's
  // custom effects do not count, which is why a water bottle with a custom
  // instant_damage one is a bottle that splashes as water.
  const GeneratedPotion* potion = potion_by_id(contents.potion);
  if (potion == nullptr)
    return false;
  for (int i = 0; i < potion->count; ++i) {
    const GeneratedMobEffect* def = mob_effect_by_id(kPotionEffects[potion->first + i].effect);
    if (def != nullptr && def->instantaneous)
      return true;
  }
  return false;
}

bool potion_contents_is_water(const StackPotionContents& contents) {
  // `PotionContents.is(Potions.WATER)`: the named potion is water *and* the
  // stack adds no effects of its own.
  const GeneratedPotion* water = potion_by_name("minecraft:water");
  return water != nullptr && contents.potion == potion_id_of(*water) &&
         contents.custom_effects.empty();
}

// ---- the projectile tick ----------------------------------------------------

PotionTickReport tick_potions(std::vector<PotionProjectile>& projectiles,
                              const PotionEnvironment& env) {
  PotionTickReport report;
  for (size_t index = 0; index < projectiles.size();) {
    PotionProjectile& projectile = projectiles[index];
    if (projectile.removed) {
      report.removed.push_back(projectile.entity_id);
      projectiles.erase(projectiles.begin() + static_cast<ptrdiff_t>(index));
      continue;
    }
    // ThrowableProjectile.tick: `applyGravity()`, then `applyInertia()`.
    projectile.vy -= kPotionGravity;
    projectile.vx *= kPotionAirDrag;
    projectile.vy *= kPotionAirDrag;
    projectile.vz *= kPotionAirDrag;

    const double dx = projectile.vx;
    const double dy = projectile.vy;
    const double dz = projectile.vz;
    const PotionBox start_box = potion_box(projectile);
    const float margin = potion_entity_margin(projectile.age);

    // `ProjectileUtil.getHitResultOnMoveVector`: the block clip shortens the
    // segment, and the nearest entity along the *shortened* segment replaces it
    // outright — which is why a potion can hit a mob standing against a wall and
    // can never hit one behind it.
    const SolidHit block = clip_blocks(env, projectile.x, projectile.y, projectile.z, dx, dy, dz);
    const double to_x = block.hit ? block.x : projectile.x + dx;
    const double to_y = block.hit ? block.y : projectile.y + dy;
    const double to_z = block.hit ? block.z : projectile.z + dz;
    const EntityHit entity = clip_entities(env, projectile, start_box, dx, dy, dz, margin);
    const bool broke = entity.hit || block.hit;

    if (broke) {
      // `this.setPos(hitResult.getLocation())` runs before the hit is resolved,
      // so the potion's position *is* the impact point.
      const double hit_x = entity.hit ? entity.x : block.x;
      const double hit_y = entity.hit ? entity.y : block.y;
      const double hit_z = entity.hit ? entity.z : block.z;
      PotionImpact impact;
      impact.projectile_entity_id = projectile.entity_id;
      impact.kind = projectile.kind;
      impact.owner_entity_id = projectile.owner_entity_id;
      impact.x = hit_x;
      impact.y = hit_y;
      impact.z = hit_z;
      impact.hit_block = block.hit;
      if (block.hit) {
        impact.block_hit_x = block.block_x;
        impact.block_hit_y = block.block_y;
        impact.block_hit_z = block.block_z;
        impact.hit_face = block.face;
      }
      // `AbstractThrownPotion.onHit`'s `this.blockPosition()`: the floor of the
      // potion's own position, not the block it hit.
      impact.block_x = static_cast<int>(std::floor(hit_x));
      impact.block_y = static_cast<int>(std::floor(hit_y));
      impact.block_z = static_cast<int>(std::floor(hit_z));
      impact.color = static_cast<uint32_t>(potion_color(projectile.contents));
      impact.contents = projectile.contents;
      impact.duration_scale = projectile.duration_scale;
      // `if (potion.is(WATER)) ... else if (potion.hasEffects()) ...`, and the
      // level event's type comes from the *named* potion either way.
      impact.level_event = potion_has_instant_effects(projectile.contents) ? 2007 : 2002;
      if (potion_contents_is_water(projectile.contents)) {
        impact.as_water = true;
        // `onHitBlock`'s douse only runs for a *block* hit, and reports the six
        // positions rather than writing them: this module has no world.
        if (impact.hit_block)
          impact.douse =
              fire_douse_positions(block.block_x, block.block_y, block.block_z, block.face);
        impact.applications =
            water_applications(env, projectile.owner_entity_id, hit_x, hit_y, hit_z);
      } else if (potion_has_effects(projectile.contents)) {
        if (projectile.kind == PotionProjectileKind::kLingering) {
          // `ThrownLingeringPotion.onHitAsPotion` spawns the cloud instead of
          // applying anything itself.
          impact.spawns_cloud = true;
        } else {
          splash_applications(env, projectile.contents, projectile.duration_scale,
                              projectile.owner_entity_id, hit_x, hit_y, hit_z, margin,
                              &impact.applications);
        }
      }
      // A potion with no effects at all (an awkward or a mundane one) breaks and
      // does nothing, which is exactly what `hasEffects()` being false means.
      report.removed.push_back(projectile.entity_id);
      report.impacts.push_back(std::move(impact));
      projectiles.erase(projectiles.begin() + static_cast<ptrdiff_t>(index));
      continue;
    }

    projectile.x = to_x;
    projectile.y = to_y;
    projectile.z = to_z;
    ++projectile.age;
    // `Projectile.tick` -> `checkLeftOwner` runs *after* the movement (it is
    // `ThrowableProjectile.tick`'s `super.tick()`), so the flag a tick's hit test
    // reads was computed from the previous tick's *new* box — which is why it is
    // measured against the moved box, expanded towards the same delta again.
    if (!projectile.left_owner)
      projectile.left_owner =
          outside_owner_collision_range(env, projectile, potion_box(projectile), dx, dy, dz);
    report.moved.push_back(projectile.entity_id);
    ++index;
  }
  return report;
}

// ---- the lingering cloud ----------------------------------------------------

AreaEffectCloud make_lingering_cloud(int32_t entity_id, double x, double y, double z,
                                     const StackPotionContents& contents, float duration_scale,
                                     int32_t owner_entity_id) {
  AreaEffectCloud cloud;
  cloud.entity_id = entity_id;
  cloud.owner_entity_id = owner_entity_id;
  cloud.x = x;
  cloud.y = y;
  cloud.z = z;
  cloud.contents = contents;
  cloud.duration_scale = duration_scale;
  // `setRadius(3.0f)` is the default, then `setRadiusOnUse(-0.5f)`,
  // `setDuration(600)`, `setWaitTime(10)` and finally
  // `setRadiusPerTick(-getRadius() / getDuration())` — which is what shrinks it
  // from three blocks toward nothing across the six hundred ticks it lives,
  // reaching the 0.5 discard floor around tick 510 rather than at 600.
  cloud.radius = kCloudDefaultRadius;
  cloud.radius_on_use = kCloudLingeringRadiusOnUse;
  cloud.duration = kCloudLingeringDuration;
  cloud.wait_time = kCloudLingeringWaitTime;
  cloud.radius_per_tick = -cloud.radius / static_cast<float>(cloud.duration);
  return cloud;
}

PotionBox cloud_box(const AreaEffectCloud& cloud) {
  // `EntityDimensions.scalable(getRadius() * 2.0f, 0.5f)`.
  return potion_target_box(cloud.x, cloud.y, cloud.z, cloud.radius, 0.5);
}

uint32_t cloud_particle_color(const StackPotionContents& contents) {
  // `ARGB.opaque(this.potionContents.getColor())`.
  return static_cast<uint32_t>(potion_color(contents)) | 0xFF000000u;
}

CloudTickReport tick_clouds(std::vector<AreaEffectCloud>& clouds, const PotionEnvironment& env) {
  CloudTickReport report;
  for (size_t index = 0; index < clouds.size();) {
    AreaEffectCloud& cloud = clouds[index];
    if (cloud.removed) {
      report.removed.push_back(cloud.entity_id);
      clouds.erase(clouds.begin() + static_cast<ptrdiff_t>(index));
      continue;
    }
    // `AreaEffectCloud.serverTick`. `age` is Entity.tickCount, which the caller
    // has already incremented for this tick (Entity.tick runs before serverTick).
    if (cloud.duration != -1 && cloud.age - cloud.wait_time >= cloud.duration) {
      report.removed.push_back(cloud.entity_id);
      clouds.erase(clouds.begin() + static_cast<ptrdiff_t>(index));
      continue;
    }
    const bool should_wait = cloud.age < cloud.wait_time;
    if (cloud.waiting != should_wait)
      cloud.waiting = should_wait;
    if (should_wait) {
      ++index;
      continue;
    }
    if (cloud.radius_per_tick != 0.0f) {
      cloud.radius += cloud.radius_per_tick;
      if (cloud.radius < kCloudMinimalRadius) {
        report.removed.push_back(cloud.entity_id);
        clouds.erase(clouds.begin() + static_cast<ptrdiff_t>(index));
        continue;
      }
    }
    if (cloud.age % kCloudTimeBetweenApplications == 0) {
      // `victims.entrySet().removeIf(entry -> tickCount >= entry.getValue())`,
      // and the whole map is dropped when the contents carry no effect at all.
      for (auto it = cloud.victims.begin(); it != cloud.victims.end();)
        it = cloud.age >= it->next_tick ? cloud.victims.erase(it) : it + 1;

      std::vector<MobEffectInstance> effects;
      for (const MobEffectInstance& instance : resolved_effects(cloud.contents)) {
        if (mob_effect_by_id(instance.effect) == nullptr)
          continue;
        // `forEachEffect(effects::add, potionDurationScale)`:
        // `withScaledDuration`, so the *declared* duration scaled by the stack's
        // own scale — 0.25 for a lingering potion, so its cloud grants a quarter
        // of what the bottle declares.
        MobEffectInstance scaled = instance;
        scaled.duration =
            scaled_duration(instance.duration, static_cast<double>(cloud.duration_scale));
        effects.push_back(std::move(scaled));
      }
      if (effects.empty()) {
        cloud.victims.clear();
      } else {
        const PotionBox box = cloud_box(cloud);
        bool discarded = false;
        for (const PotionTarget& target : env.targets) {
          if (!target.alive)
            continue;
          // The candidate set is `getEntitiesOfClass(LivingEntity.class,
          // getBoundingBox())` — the box overlap — and the real gate is the
          // *horizontal* distance against the radius.
          const PotionBox target_box =
              potion_target_box(target.x, target.y, target.z, target.half_width, target.height);
          if (!box_intersects(box, target_box))
            continue;
          bool already = false;
          for (const AreaEffectCloud::Victim& victim : cloud.victims)
            if (victim.entity_id == target.entity_id) {
              already = true;
              break;
            }
          if (already)
            continue;
          const double dx = target.x - cloud.x;
          const double dz = target.z - cloud.z;
          if (!(dx * dx + dz * dz <= static_cast<double>(cloud.radius) * cloud.radius))
            continue;

          cloud.victims.push_back({target.entity_id, cloud.age + cloud.reapplication_delay});
          for (const MobEffectInstance& instance : effects) {
            const GeneratedMobEffect* def = mob_effect_by_id(instance.effect);
            if (!def)
              continue;
            apply_one(*def, instance, target, cloud.owner_entity_id,
                      static_cast<double>(kCloudInstantScale), /*drop_short=*/false,
                      &report.applications);
          }
          if (cloud.radius_on_use != 0.0f) {
            cloud.radius += cloud.radius_on_use;
            if (cloud.radius < kCloudMinimalRadius) {
              discarded = true;
              break;
            }
          }
          if (cloud.duration_on_use == 0 || cloud.duration == -1)
            continue;
          cloud.duration += cloud.duration_on_use;
          if (cloud.duration <= 0) {
            discarded = true;
            break;
          }
        }
        if (discarded) {
          report.removed.push_back(cloud.entity_id);
          clouds.erase(clouds.begin() + static_cast<ptrdiff_t>(index));
          continue;
        }
      }
    }
    ++index;
  }
  return report;
}

} // namespace mc::world
