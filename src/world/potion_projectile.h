// SPDX-License-Identifier: GPL-3.0-or-later
//
// Thrown potions: the splash and lingering potion *entities*, and the
// `minecraft:area_effect_cloud` a lingering one leaves behind (Phase 3
// milestone G follow-up).
//
// A splash potion is not a consumable — `SplashPotionItem.use` never starts a
// use, it throws: `Projectile.spawnProjectileFromRotation(createPotion, level,
// stack, player, -20.0f, 0.5f, 1.0f)` spawns the entity at the thrower's eyes
// minus 0.1 and shots it along their rotation, then the stack is consumed and
// the interaction is over. From there the entity flies under
// `ThrowableProjectile.tick` (gravity 0.05, air drag 0.99) and breaks on the
// first thing it touches, which is what `AbstractThrownPotion.onHit` turns into
// one of three outcomes:
//
//   * a water bottle douses what it lands on and damages water-sensitive
//     entities — no `potion_contents` effects at all;
//   * a splash potion applies its effects in a 4 x 2 x 4 box around itself,
//     scaled by `1 - sqrt(distance) / 4` and skipping an instance that ends
//     within 20 ticks;
//   * a lingering potion leaves an `AreaEffectCloud` behind (radius 3, 600
//     ticks, shrinking by `-radius / duration` a tick) which keeps applying the
//     same effects to whatever stands in it, once per victim every 20 ticks.
//
// The three share the `potion_contents` component on the *stack*, not a
// consumable effect list, which is why the effects are read here and not from
// world/consume.h.
//
// What is mirrored, and from where: the spawn (`Projectile
// .spawnProjectileFromRotation` -> `shootFromRotation` -> `shoot`, including the
// 0.0172275 * uncertainty triangle spread and the thrower's own movement), the
// movement, `ProjectileUtil.getHitResultOnMoveVector` (a block clip that
// truncates the segment, then the nearest entity hit along it, which *wins*
// over the block), `Projectile.leftOwner` (a potion cannot hit its thrower
// until it has left their box), `AbstractThrownPotion.onHitAsPotion` for both
// kinds, and `AreaEffectCloud.serverTick` line for line.
//
// What is NOT mirrored, all of it documented rather than approximated:
//   * collision *shapes*: a block is solid or it is not ("not air and not a
//     fluid", the same stand-in the orbs and mobs use), so a potion breaks on a
//     block vanilla would let it pass through (grass, a torch) and never on one
//     it would clip (nothing here has a partial box);
//   * fluids: a potion in water keeps its air drag instead of 0.8, and the
//     bubble-column particles are not spawned;
//   * the water branch's block half (dousing `minecraft:fire`, candles and
//     campfires) and its entity half (extinguishing, `isSensitiveToWater`) are
//     *implemented* but unfalsifiable here: the table carries no water-
//     sensitivity column, so no mob here is water-sensitive (vanilla's snow
//     golem is the one type in the table that overrides `isSensitiveToWater`),
//     and a water bottle splashes and does nothing to a mob. The predicates are
//     in the environment so a water model can turn them on;
//   * lasting effects on a **mob**: a splash potion's instantaneous effects land
//     on any living entity (the harm or the heal), but a mob has no effect set
//     to hold the others — the boundary world/effect.h already documents. The
//     split is `PotionApplication::is_player`;
//   * the `minecraft:potion_duration_scale` *component on the stack*: only the
//     item's prototype value is read (which is what every real potion uses —
//     0.25 for the lingering potion and 1.0 for the rest).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "world/effect.h"
#include "world/fire.h"
#include "world/item.h"

namespace mc::world {

// ---- vanilla constants ------------------------------------------------------

// AbstractThrownPotion.getDefaultGravity / ThrowableProjectile.getAirDrag.
constexpr double kPotionGravity = 0.05;
constexpr double kPotionAirDrag = 0.99;
// ThrowablePotionItem.PROJECTILE_SHOOT_POWER, and the `use` call's own offset
// and uncertainty: spawnProjectileFromRotation(..., -20.0f, 0.5f, 1.0f).
constexpr double kPotionThrowPower = 0.5;
constexpr float kPotionThrowPitchOffset = -20.0f;
constexpr float kPotionThrowUncertainty = 1.0f;
// Projectile.getMovementToShoot's spread: `random.triangle(0.0, 0.0172275 * u)`
// per axis, one per draw pair, then the whole vector normalized and scaled.
constexpr double kPotionSpread = 0.0172275;
// ThrowableItemProjectile spawns this far below its owner's eye height
// (`owner.getEyeY() - 0.1f`) — and, because EntityDimensions.makeBoundingBox
// treats a position's y as the box's *bottom*, that is the bottom of the potion.
constexpr double kPotionSpawnEyeOffset = 0.1;
// EntityTypes.SPLASH_POTION / LINGERING_POTION: EntityType.Builder sized
// 0.25 x 0.25.
constexpr double kPotionWidth = 0.25;
constexpr double kPotionHeight = 0.25;
// AbstractThrownPotion.SPLASH_RANGE / SPLASH_RANGE_SQ, and the y inflation that
// is half of it: `potionAabb.inflate(4.0, 2.0, 4.0)`.
constexpr double kPotionSplashRange = 4.0;
constexpr double kPotionSplashRangeSq = kPotionSplashRange * kPotionSplashRange;
constexpr double kPotionSplashInflateY = 2.0;
// AreaEffectCloud's own constants, and the values ThrownLingeringPotion
// sets on the cloud it spawns.
constexpr int kCloudTimeBetweenApplications = 5; // TIME_BETWEEN_APPLICATIONS
constexpr int kCloudDefaultReapplicationDelay = 20;
constexpr float kCloudMinimalRadius = 0.5f;  // MINIMAL_RADIUS
constexpr float kCloudMaxRadius = 32.0f;     // MAX_RADIUS
constexpr float kCloudDefaultRadius = 3.0f;  // DEFAULT_RADIUS
constexpr int kCloudLingeringDuration = 600; // DEFAULT_LINGERING_DURATION
constexpr int kCloudLingeringWaitTime = 10;  // setWaitTime(10)
constexpr float kCloudLingeringRadiusOnUse = -0.5f;
// What an *instantaneous* effect is scaled by when a cloud applies it
// (`applyInstantaneousEffect(..., 0.5)`); the splash path uses its own falloff.
constexpr float kCloudInstantScale = 0.5f;
// An instance the splash path would land with 20 ticks or less left is skipped
// (`newEffect.endsWithin(20)`).
constexpr int kSplashMinDuration = 20;

// ---- the thrown entity ------------------------------------------------------

// Which item was thrown. The two are identical in flight and differ only in
// what the impact does.
enum class PotionProjectileKind : uint8_t {
  kSplash = 0,
  kLingering = 1,
};

// One live thrown potion. `x/y/z` is the entity position — x and z the box's
// centre, y its *bottom* (EntityDimensions.makeBoundingBox) — and `vx/vy/vz`
// the delta movement.
struct PotionProjectile {
  int32_t entity_id = 0;
  PotionProjectileKind kind = PotionProjectileKind::kSplash;
  StackPotionContents contents;
  // minecraft:potion_duration_scale, the thrown stack's own (see the header
  // comment); 1.0 for a splash potion, 0.25 for a lingering one.
  float duration_scale = 1.0f;
  double x = 0.0, y = 0.0, z = 0.0;
  double vx = 0.0, vy = 0.0, vz = 0.0;
  int32_t owner_entity_id = -1;
  // Projectile.leftOwner: not updated until the potion has left its thrower's
  // box (plus one block), which is what stops a potion hitting the player who
  // threw it before it is even out of their hand.
  bool left_owner = false;
  int age = 0; // Entity.tickCount
  bool removed = false;
};

// An axis-aligned box, in the same shape world/experience_orb.h uses.
struct PotionBox {
  double min_x = 0.0, min_y = 0.0, min_z = 0.0;
  double max_x = 0.0, max_y = 0.0, max_z = 0.0;
};

// The entity's box for a position: EntityDimensions.makeBoundingBox, i.e.
// x/z centred on the position and y its bottom.
PotionBox potion_box(double x, double y, double z);
PotionBox potion_box(const PotionProjectile& projectile);
PotionBox potion_target_box(double x, double y, double z, double half_width, double height);

// AABB.distanceToSqr(AABB): the squared gap between two boxes, 0 when they
// overlap. The one geometry helper the splash's range test shares with vanilla.
double box_distance_to_sqr(const PotionBox& a, const PotionBox& b);
PotionBox box_inflate(const PotionBox& box, double dx, double dy, double dz);
bool box_intersects(const PotionBox& a, const PotionBox& b);
bool box_contains(const PotionBox& box, double x, double y, double z);

// ProjectileUtil.computeMargin: `max(0, min(0.3, (tickCount - 2) / 20))`, the
// widening margin an entity is hit within (it exists so a potion cannot thread a
// one-pixel gap between two mobs).
float potion_entity_margin(int tick_count);

// `ThrowablePotionItem.use`: the spawn position, then `shootFromRotation` with
// the -20 degree pitch offset, power 0.5 and uncertainty 1.0, then the thrower's
// own movement added (their vertical motion only while airborne). The six
// `unit_draws` are the `nextDouble()` calls the three triangle spreads consume,
// in x/y/z order of the two draws each; pass zeros for a straight throw.
void throw_potion(PotionProjectile& projectile, PotionProjectileKind kind,
                  const StackPotionContents& contents, float duration_scale,
                  int32_t owner_entity_id, double owner_x, double owner_y, double owner_z,
                  float owner_yaw, float owner_pitch, double owner_vx, double owner_vy,
                  double owner_vz, bool owner_on_ground, const double unit_draws[6]);

// The thrown kind an item is, or nothing for an item that is not thrown.
//
// `SplashPotionItem` and `LingeringPotionItem` are *classes*, not data: 26.2
// registers them in code (`Items.java`,
// `registerItem(ItemIds.SPLASH_POTION, SplashPotionItem::new, ...)`), so no data
// report names them and the two item names are transcribed from that line. The
// callers that need the *item* (the spawn's stack, the display) already have it;
// this is only the question "does `use` throw this, and which way"?
std::optional<PotionProjectileKind> throwable_potion_kind(std::string_view item_name);

// `Projectile.shoot`'s own rotation, which is what Add Entity carries: yRot from
// the velocity's x/z and xRot from its y against the horizontal distance.
// It is taken at spawn, which is the only rotation this server transmits.
// Vanilla re-derives it every tick (`ThrowableProjectile.tick` ->
// `Projectile.updateRotation`, smoothed by `Projectile.lerpRotation`) and pushes
// the change through the rotation-only move packets, which this server does not
// send — so the bottle keeps the angle it was thrown at while its *position*
// follows the arc.
void potion_spawn_rotation(const PotionProjectile& projectile, float* yaw, float* pitch);

// ---- the world it flies through ---------------------------------------------

// One living entity a potion can reach. `x/y/z` is the box origin (x/z centre,
// y the bottom) and `is_player` is the line between the two effect sinks: a
// player's effects go into `ActiveEffects`, a mob's only exist while they are
// instantaneous (see the header comment).
struct PotionTarget {
  int32_t entity_id = 0;
  bool is_player = false;
  double x = 0.0, y = 0.0, z = 0.0;
  double half_width = 0.3; // Player.PLAYER_DIMENSIONS is 0.6 x 1.8
  double height = 1.8;
  // LivingEntity.isAffectedByPotions: `!isDeadOrDying()`.
  bool alive = true;
  // LivingEntity.isSensitiveToWater / Entity.isOnFire. Both false for
  // everything this server can create (header comment).
  bool sensitive_to_water = false;
  bool on_fire = false;
  // LivingEntity.isInvertedHealAndHarm: instant health *damages* this entity and
  // instant damage heals it. The zombie is the one mob here in the
  // `minecraft:inverted_healing_and_harm` tag.
  bool inverted_heal_and_harm = false;
};

// The world a thrown potion (and a cloud) ticks in. `is_solid` answers
// `ClipContext.Block.COLLIDER` for those coordinates and may be empty — in the
// void world there is nothing to break on, so a potion flies until it leaves the
// world's bounds.
struct PotionEnvironment {
  std::vector<PotionTarget> targets;
  std::function<bool(int, int, int)> is_solid;
};

// One effect landing on one entity, as the caller should apply it.
//
// A *lasting* effect is `instance`, as it should land: the duration already
// scaled by the splash's falloff (which drops an instance that would end within
// 20 ticks) or by the stack's own duration scale for a cloud.
//
// An *instantaneous* one is not an instance at all — `MobEffect` applies it and
// it is gone — so it carries the number instead: `harm` says which way it goes
// (already resolved against the target's `isInvertedHealAndHarm`, which is what
// makes instant health damage a zombie), `amount` is
// `instant_effect_amount`'s answer, `effect` the id for attribution, and
// `damage_type` the type a harm carries.
struct PotionApplication {
  int32_t entity_id = 0;
  // The thrower the effect is attributed to, or -1. Vanilla's harm is
  // `damageSources().indirectMagic(source, owner)`, so the *owner* is the
  // causing entity of a splash or a cloud's damage — which is what the client
  // draws (a hit that names an attacker) and what the death message reads. The
  // mob and the player sinks both use it.
  int32_t owner_entity_id = -1;
  bool is_player = false;
  bool inverted_heal_and_harm = false;
  bool instantaneous = false;
  bool harm = false;
  float amount = 0.0f;
  int32_t effect = 0;
  const char* damage_type = nullptr;
  MobEffectInstance instance;
  // `AbstractThrownPotion.onHitAsWater`'s *other* half: a target that is on
  // fire is extinguished (which is not a damage at all, so this can be the only
  // thing an application carries). Vanilla hurts first and extinguishes second,
  // which is the order the water path emits them in.
  bool extinguish = false;
};

// One potion breaking. `level_event` is 2007 for a potion with an instantaneous
// effect and 2002 otherwise (`AbstractThrownPotion.onHit`'s own choice), `color`
// the potion's colour the event carries, and `applications` the splash's effects
// (empty for a water bottle and for a lingering potion, whose effects arrive
// through the cloud instead).
struct PotionImpact {
  int32_t projectile_entity_id = 0;
  PotionProjectileKind kind = PotionProjectileKind::kSplash;
  int32_t owner_entity_id = -1;
  // Where the bottle broke. `ThrowableProjectile.tick` calls
  // `setPos(hitResult.getLocation())` *before* `hitTargetOrDeflectSelf`, so this
  // is the potion's own position when `onHit` runs — and therefore both what the
  // splash's box is centred on and the exact spot a lingering potion's cloud is
  // born at (`new AreaEffectCloud(level, this.getX(), this.getY(), this.getZ())`).
  // For a block hit it is the face of the block the segment entered.
  double x = 0.0, y = 0.0, z = 0.0;
  // AbstractThrownPotion's blockPosition() for the level event: the floor of the
  // potion's own position, not the block it hit.
  int block_x = 0, block_y = 0, block_z = 0;
  // True when the bottle broke on a *block* (rather than on an entity), which
  // is the only case `onHitBlock` runs in.
  bool hit_block = false;
  // `BlockHitResult.getBlockPos()` and `getDirection()`: the cell the bottle
  // entered and the face it came through, as the Direction ordinal the wire
  // uses. `douse` below is those six positions; they are kept as well because
  // they are what a caller (and a test) reads the geometry from.
  int block_hit_x = 0, block_hit_y = 0, block_hit_z = 0;
  int hit_face = 1;
  int32_t level_event = 2002;
  uint32_t color = 0;
  bool as_water = false;
  bool spawns_cloud = false;
  // `AbstractThrownPotion.onHitBlock`'s four-way douse: the six positions a
  // water bottle puts fire out at, reported rather than applied because this
  // module has no world to write to. Only a block hit fills it.
  std::vector<FireBlockPos> douse;
  // Handed to a spawned cloud, so it applies exactly what a splash would have.
  StackPotionContents contents;
  float duration_scale = 1.0f;
  std::vector<PotionApplication> applications;
};

// What one tick of the whole population produced: `moved` need a position sync,
// `removed` a despawn (a potion that broke), `impacts` the work above.
struct PotionTickReport {
  std::vector<int32_t> moved;
  std::vector<int32_t> removed;
  std::vector<PotionImpact> impacts;
};

// Advances every live potion by one tick. Broken potions are erased from
// `projectiles` and reported in both `removed` and `impacts`.
PotionTickReport tick_potions(std::vector<PotionProjectile>& projectiles,
                              const PotionEnvironment& env);

// PotionContents.getColor: the component's explicit `custom_color` when set,
// otherwise the amplifier-weighted average of the *visible* effects' colours,
// or `kBasePotionColor` when there is nothing to average.
int32_t potion_color(const StackPotionContents& contents);
// Potion.hasInstantEffects, over the same list.
bool potion_has_instant_effects(const StackPotionContents& contents);
// Whether the contents name `minecraft:water` — `PotionContents.is(Potions
// .WATER)`, which also requires that the stack adds no custom effects of its own.
bool potion_contents_is_water(const StackPotionContents& contents);

// ---- the lingering cloud ----------------------------------------------------

// AreaEffectCloud, as ThrownLingeringPotion sets it up: radius 3 shrinking to
// nothing over 600 ticks, a 10-tick wait before it starts, -0.5 radius per
// victim, and the thrown stack's contents at the thrown stack's duration scale.
struct AreaEffectCloud {
  int32_t entity_id = 0;
  int32_t owner_entity_id = -1;
  double x = 0.0, y = 0.0, z = 0.0;
  float radius = kCloudDefaultRadius;
  bool waiting = false;
  int age = 0; // Entity.tickCount, which is already incremented for this tick
  int duration = kCloudLingeringDuration;
  int wait_time = kCloudLingeringWaitTime;
  int reapplication_delay = kCloudDefaultReapplicationDelay;
  int duration_on_use = 0;
  float radius_on_use = kCloudLingeringRadiusOnUse;
  float radius_per_tick = 0.0f;
  StackPotionContents contents;
  float duration_scale = 1.0f;
  bool removed = false;
  // AreaEffectCloud.victims: entity id -> the tick it may be affected again at.
  struct Victim {
    int32_t entity_id = 0;
    int next_tick = 0;
  };
  std::vector<Victim> victims;
};

// ThrownLingeringPotion.onHitAsPotion's cloud, with `radius_per_tick` already
// `-radius / duration`.
AreaEffectCloud make_lingering_cloud(int32_t entity_id, double x, double y, double z,
                                     const StackPotionContents& contents, float duration_scale,
                                     int32_t owner_entity_id);

// AreaEffectCloud.getBoundingBox: `radius * 2` wide and 0.5 tall (its
// EntityDimensions), which is the box an entity has to intersect to be a
// candidate at all.
PotionBox cloud_box(const AreaEffectCloud& cloud);
// The colour `ColorParticleOption.create(ParticleTypes.ENTITY_EFFECT, ...)`
// carries, i.e. ARGB.opaque(getColor()).
uint32_t cloud_particle_color(const StackPotionContents& contents);

// What one tick of the cloud population produced: `applications` the effects
// that landed this tick and `removed` the clouds that were discarded (their age
// ran out, or a victim shrank them under MINIMAL_RADIUS).
//
// The radius and the waiting flag are deliberately *not* reported here: both go
// through SynchedEntityData.set, which only fires when the value differs from
// what was last sent, and the caller is the one that remembers that — a single
// last-cloud pair in a report could not describe a population anyway.
struct CloudTickReport {
  std::vector<PotionApplication> applications;
  std::vector<int32_t> removed;
};

// Advances every live cloud by one tick (Age is incremented by the caller's
// `cloud.age++` before this runs, exactly as Entity.tick does before
// serverTick). Discarded clouds are erased from `clouds`.
CloudTickReport tick_clouds(std::vector<AreaEffectCloud>& clouds, const PotionEnvironment& env);

} // namespace mc::world
