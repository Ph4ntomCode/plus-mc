// SPDX-License-Identifier: GPL-3.0-or-later
//
// Projectiles: the arrow a bow fires, and the three thrown items — snowball,
// egg and ender pearl (Phase 4 milestone H).
//
// The trajectory model is the one the thrown potions already use
// (world/potion_projectile.{h,cpp}): `Projectile.shootFromRotation` builds the
// launch, the entity integrates gravity and air drag a tick at a time, and the
// hit is resolved against a block clip that truncates the segment and the
// nearest entity along it. What this module adds is the *arrow*, which is not a
// `ThrowableProjectile` at all:
//
//   * `BowItem.releaseUsing` — `getPowerForTime(timeHeld)`'s quadratic, the
//     0.1 refusal, and `shoot(..., pow * 3.0f, 1.0f, pow == 1.0f)` where the
//     last argument is the critical flag;
//   * `AbstractArrow.tick` — its own order (the in-ground check first, inertia
//     and gravity *after* the movement), `stepMoveAndHit`'s piercing loop, the
//     1200-tick ground despawn, `shouldFall`'s "the block under a stuck arrow
//     was removed" branch, and the 0.05 push off the face it buried in;
//   * `AbstractArrow.onHitEntity` — `ceil(|v| * baseDamage)`, the critical's
//     `nextInt(damage / 2 + 2)`, the pierce budget (`pierce + 1` victims, and
//     the *next* one discards the arrow before it is ever damaged), and the
//     deflect branch `hurt` can take instead;
//   * the two enchantments Phase 4 milestone G modelled but left inert:
//     **Power**'s `modifyDamage` and **Punch**'s `modifyKnockback`, both passed
//     through world/enchantment.h's `item_melee_enchantments` with
//     `direct_attacker = "minecraft:arrow"` — which is exactly what vanilla's
//     `EnchantmentHelper.modifyDamage(level, this.getWeaponItem(), ...)` does
//     with the bow the arrow remembers as `firedFromWeapon`, and what
//     `#minecraft:arrows` (world/entity_type_tags.h) gates;
//
// and the three `ThrowableItemProjectile`s, each of which is a *different*
// launch from a potion's (`SnowballItem.use` and friends call
// `spawnProjectileFromRotation(..., 0.0f, 1.5f, 1.0f)`, not the potion's
// `-20.0f, 0.5f, 1.0f`) and a different impact:
//
//   * `Snowball` — 3 damage to a blaze and 0 to everything else, then it breaks
//     whatever it touched;
//   * `ThrownEgg` — 0 damage, and `nextInt(8) == 0` lays one chicken (`nextInt(32)
//     == 0` four) at the break, each a *baby* chicken (age -24000);
//   * `ThrownEnderpearl` — the owner is teleported to the pearl's own
//     `oldPosition()` (the position it held when the tick that hit started, not
//     the impact point) and takes 5.0 of `minecraft:ender_pearl` damage, whose
//     death message is the fall one.
//
// What is NOT mirrored, all of it documented rather than approximated:
//   * collision *shapes*: a block is solid or it is not ("not air and not a
//     fluid", the same stand-in the orbs, mobs and potions use), so an arrow
//     buries itself in a torch and never in tall grass;
//   * fluids: water's 0.6 water inertia, the bubble particles and
//     `applyEffectsFromBlocks` are not modelled — nothing here is ever in water;
//   * **arrow pickup**: a stuck arrow is never recovered (`AbstractArrow
//     .playerTouch` / `tryPickup` need a per-player overlap scan and an
//     inventory write on the player's own thread), and the creative-only pickup
//     of a `pickup = CREATIVE_ONLY` arrow falls out of the same gap;
//   * the *fire* half of an arrow hit: `igniteForSeconds(5)` needs an arrow that
//     is on fire, and nothing here can set one alight (no burning entity fires
//     arrows at this server);
//   * `doPostHurtEffects`: a spectral arrow's 200-tick Glowing lands in a mob's
//     no-effect-set gap (world/effect.h) and on a player it would need the
//     effect sink, so nothing applies it;
//   * the *shield* half of a hit: `hurtServer` may be refused by a blocking
//     player, and the arrow's answer to that (`deflect`, delta * 0.2, discard)
//     is modelled only through the environment's `hurt` returning false;
//   * `ThrownEnderpearl`'s 5% endermite and `EnderPearlsVanishOnDeath` — there
//     are no endermites in this server's mob table and no per-player pearl
//     registry;
//   * a *player* target's damage is deferred to that player's own session thread,
//     so this module cannot know whether it landed: the environment's `hurt`
//     answers for it (see `ProjectileEnvironment::hurt`).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "world/item.h"

namespace mc::world {

// ---- vanilla constants ------------------------------------------------------

// ThrowableProjectile.getDefaultGravity / getAirDrag. (The potions' own gravity
// is 0.05 — `AbstractThrownPotion` overrides it; a throwable item does not.)
constexpr double kThrowableGravity = 0.03;
constexpr double kThrowableAirDrag = 0.99;
// SnowballItem.PROJECTILE_SHOOT_POWER — and the same 1.5 the egg and the ender
// pearl throw with (`spawnProjectileFromRotation(..., 0.0f, 1.5f, 1.0f)`: no
// pitch offset at all, unlike the potions' -20 degrees).
constexpr double kThrowableShootPower = 1.5;
constexpr float kThrowableThrowPitchOffset = 0.0f;
constexpr float kThrowableThrowUncertainty = 1.0f;
// ThrowableItemProjectile spawns this far below its owner's eye height
// (`owner.getEyeY() - 0.1f`), which is the *bottom* of its box.
constexpr double kThrowableSpawnEyeOffset = 0.1;
// `Projectile.getMovementToShoot`'s per-axis triangle spread.
constexpr double kProjectileSpread = 0.0172275;

// AbstractArrow's own numbers: the gravity and drag, the base damage every
// arrow starts with (`ARROW_BASE_DAMAGE`; `ArrowItem.createArrow` does not touch
// it), the 1200-tick despawn a *buried* arrow counts, the seven-tick shake, and
// the 0.05 step back off the face it hit.
constexpr double kArrowGravity = 0.05;
constexpr double kArrowAirDrag = 0.99;
constexpr double kArrowBaseDamage = 2.0;
constexpr int kArrowDespawnTicks = 1200;
constexpr int kArrowShakeTicks = 7;
constexpr double kArrowGroundBackoff = 0.05;
// EntityTypes.ARROW is `sized(0.5f, 0.5f)`; the three throwables are all
// 0.25 x 0.25.
constexpr double kArrowWidth = 0.5;
constexpr double kArrowHeight = 0.5;
constexpr double kThrowableWidth = 0.25;
constexpr double kThrowableHeight = 0.25;

// BowItem: `MAX_DRAW_DURATION` (the ticks a full-power draw takes), the use
// duration the client's release is measured against, the power below which the
// release does not fire at all, and the 3.0 the velocity is scaled by.
constexpr int kBowUseDuration = 72000;
constexpr int kBowMaxDrawDuration = 20;
constexpr float kBowMinPower = 0.1f;
constexpr float kBowVelocityMultiplier = 3.0f;

// `SpectralArrow.DEFAULT_DURATION` (and its `doPostHurtEffects`): the Glowing a
// spectral arrow lands is 200 ticks, so `ProjectileImpact.glowing_ticks` is this
// for a `kSpectralArrow` and 0 for every other kind.
constexpr int kSpectralArrowGlowTicks = 200;

// Snowball's `entity instanceof Blaze ? 3 : 0`, and ThrownEgg's two chicken
// rolls (`nextInt(8) == 0`, then `nextInt(32) == 0` for a quad).
constexpr float kSnowballBlazeDamage = 3.0f;
constexpr int kEggChickenRoll = 8;
constexpr int kEggChickenQuadRoll = 32;
// ThrownEnderpearl's own damage on its owner, and the effect source of the
// teleport.
constexpr float kEnderPearlOwnerDamage = 5.0f;

// WitherSkull: `AbstractHurtingProjectile` (no gravity, 0.95 inertia) at the
// 0.3125 box `EntityType.WITHER_SKULL` is sized with, for its flat 8 damage.
constexpr double kWitherSkullDrag = 0.95;
constexpr float kWitherSkullDamage = 8.0f;
constexpr double kWitherSkullHalfWidth = 0.3125;
constexpr double kWitherSkullHeight = 0.3125;

// ---- the kinds --------------------------------------------------------------

enum class ProjectileKind : uint8_t {
  kArrow = 0,
  kSpectralArrow = 1,
  kSnowball = 2,
  kEgg = 3,
  kEnderPearl = 4,
  // The wither's volley: a `WitherSkull`, which is neither an arrow nor a
  // throwable — it flies flat, discards itself on the first hit and deals its
  // flat 8 through `minecraft:wither_skull`.
  kWitherSkull = 5,
};

// The entity type's registry name ("minecraft:arrow", "minecraft:snowball", ...).
// `minecraft:spectral_arrow` is its own entity type and its own item; the two
// arrows are identical in flight (and in this module) and differ only in what
// they drop.
const char* projectile_entity_type_name(ProjectileKind kind);
// The item a thrown projectile is (`Snowball`'s own `getDefaultItem`), which is
// what a caller puts in the synched-data stack and what an arrow's pickup stack
// would be. Empty for a kind this server cannot throw.
std::string_view projectile_item_name(ProjectileKind kind);
// AbstractArrow vs ThrowableProjectile: the two tick shapes (see the header
// comment) and the two collision boxes.
bool projectile_is_arrow(ProjectileKind kind);
// The thrown item kind an item name is, or nothing for an item this server does
// not throw (`SnowballItem`, `EggItem` and `EnderpearlItem` are registered in
// code, so their names are transcribed from `Items.java` exactly as the thrown
// potions' are).
std::optional<ProjectileKind> thrown_projectile_kind(std::string_view item_name);

// The `AbstractArrow` an arrow item creates: `SpectralArrowItem.createArrow`
// makes the `minecraft:spectral_arrow` entity, and `ArrowItem`'s own
// `createArrow` makes the plain arrow for everything else in the
// `minecraft:arrows` tag (`minecraft:arrow` and `minecraft:tipped_arrow`, whose
// potion contents ride on the *entity* rather than changing its type). The
// caller has already checked the name is an arrow item; a name that is not gets
// the plain arrow, which is what vanilla's default constructor returns.
ProjectileKind arrow_kind_for_item(std::string_view item_name);

// ---- the bow ----------------------------------------------------------------

// `BowItem.getPowerForTime(int timeHeld)`:
// `pow = timeHeld / 20; pow = (pow * pow + pow * 2) / 3; min(pow, 1)`.
float bow_power_for_time(int time_held_ticks);

// What a `releaseUsing` does: the power, whether it fires at all, and whether
// the arrow is a critical one (`BowItem.releaseUsing` passes `pow == 1.0f` as
// `isCrit`, and `ProjectileWeaponItem.createProjectile` turns that into
// `setCritArrow`).
struct BowShot {
  bool fires = false;
  float power = 0.0f;
  bool critical = false;
};
BowShot bow_shot(int time_held_ticks);

// ---- the entity -------------------------------------------------------------

// One live projectile. `x/y/z` is the entity position — x and z the box's
// centre, y its *bottom* (`EntityDimensions.makeBoundingBox`) — and `vx/vy/vz`
// the delta movement.
struct Projectile {
  int32_t entity_id = 0;
  ProjectileKind kind = ProjectileKind::kArrow;
  // The player (or mob) that fired it, or -1. Projectile.leftOwner keeps it from
  // being hit by its own shot until the shot has left its box.
  int32_t owner_entity_id = -1;
  double x = 0.0, y = 0.0, z = 0.0;
  // `Entity.oldPosition()`: the position held when *this* tick started, which is
  // what `setOldPosAndRot` (ServerLevel.tickNonPassenger, before `entity.tick()`)
  // strikes. An ender pearl teleports its owner here, not to the impact point.
  double old_x = 0.0, old_y = 0.0, old_z = 0.0;
  double vx = 0.0, vy = 0.0, vz = 0.0;
  bool left_owner = false;
  int age = 0; // Entity.tickCount
  bool removed = false;

  // ---- arrow state ----
  // The bow (or crossbow) the arrow remembers as `firedFromWeapon`: what Power's
  // and Punch's requirements are evaluated against on a hit. Empty when nothing
  // fired it (a dispenser, a mob's bow).
  ItemStack weapon;
  // `PIERCE_LEVEL` and the `piercingIgnoreEntityIds` set it fills: the ids
  // already pierced, in the order they were hit. The set holds at most
  // `pierce_level + 1` of them.
  int pierce_level = 0;
  std::vector<int32_t> pierced_entities;
  bool critical = false;
  double base_damage = kArrowBaseDamage;
  bool in_ground = false;
  // `inGroundTime` (the client's shake cadence) and `life` (the despawn counter,
  // which only runs while grounded).
  int in_ground_time = 0;
  int life = 0;
  // `IN_GROUND` is a *synched* datum — `AbstractArrow.setInGround` goes through
  // `SynchedEntityData.set`, so burying (`onHitBlock`) and the `startFalling`
  // that clears it are each a packet even on a tick the arrow does not move.
  // This bit means "the value changed on *this* tick": `tick_projectiles`
  // clears it at the start of each projectile's tick and the bury and un-stick
  // code sets it, which is why the two datum-only ticks (`startFalling`, and
  // by finding itself inside a block — vanilla's `setInGround(true)`) also
  // report the arrow as needing its sync slot.
  bool in_ground_changed = false;
};

// The entity's box for a position: x/z centred on it, y its bottom.
struct ProjectileBox {
  double min_x = 0.0, min_y = 0.0, min_z = 0.0;
  double max_x = 0.0, max_y = 0.0, max_z = 0.0;
};
ProjectileBox projectile_box(double x, double y, double z, double half_width, double height);
ProjectileBox projectile_box(const Projectile& projectile);
// The box of one living entity a projectile can reach.
ProjectileBox projectile_target_box(double x, double y, double z, double half_width, double height);

// `ProjectileUtil.computeMargin`: `max(0, min(0.3, (tickCount - 2) / 20))`.
float projectile_entity_margin(int tick_count);

// `Projectile.shoot`'s rotation, which is what Add Entity carries: yRot from the
// velocity's x/z and xRot from its y against the horizontal distance.
void projectile_spawn_rotation(const Projectile& projectile, float* yaw, float* pitch);

// `BowItem.releaseUsing` -> `ProjectileWeaponItem.shoot` ->
// `Projectile.shootFromRotation(shooter, xRot, yRot, 0.0f, power, 1.0f)` for one
// arrow. `shot` comes from `bow_shot`; `power` is the velocity and `critical`
// sets the flag. The six `unit_draws` are the `nextDouble()` calls the three
// triangle spreads consume (two per axis, x/y/z); pass zeros for a straight
// shot. `weapon` is the bow, kept for Power and Punch.
void shoot_arrow(Projectile& projectile, ProjectileKind kind, const ItemStack& weapon,
                 int32_t owner_entity_id, double owner_x, double owner_y, double owner_z,
                 float owner_yaw, float owner_pitch, double owner_vx, double owner_vy,
                 double owner_vz, bool owner_on_ground, const BowShot& shot,
                 const double unit_draws[6]);

// `SnowballItem.use` and its two siblings: the same spawn, with the item's own
// `0.0f, 1.5f, 1.0f`.
void throw_item_projectile(Projectile& projectile, ProjectileKind kind, int32_t owner_entity_id,
                           double owner_x, double owner_y, double owner_z, float owner_yaw,
                           float owner_pitch, double owner_vx, double owner_vy, double owner_vz,
                           bool owner_on_ground, const double unit_draws[6]);

// `WitherBoss.performRangedAttack`'s skull: `shoot(..., 0.6F, 0.5F)` from the
// wither's eye, with no pitch offset and the firer's own movement not added (the
// wither is stationary in the tick that fires). The draws are the launch's
// uncertainty triangle; a caller with no spread passes six 0.5s.
void launch_wither_skull(Projectile& projectile, int32_t owner_entity_id, double owner_x,
                         double owner_y, double owner_z, float owner_yaw, float owner_pitch,
                         const double unit_draws[6]);

// ---- the world it flies through ---------------------------------------------

// One living entity a projectile can reach. `entity_type` is the
// `minecraft:entity_type` name the weapon's requirements are evaluated against
// (Power's and Punch's are `direct_attacker` ones, so they read the *attacker*,
// but the name is what a caller had to resolve either way). `knockback_resistance`
// is `Attributes.KNOCKBACK_RESISTANCE` for Punch's factor: nothing this server
// can spawn has any, so it is 0.0 on every live path.
struct ProjectileTarget {
  int32_t entity_id = 0;
  bool is_player = false;
  std::string_view entity_type;
  double x = 0.0, y = 0.0, z = 0.0;
  double half_width = 0.3; // Player.PLAYER_DIMENSIONS is 0.6 x 1.8
  double height = 1.8;
  bool alive = true;
  // `entity instanceof Blaze`, the snowball's one damaging case.
  bool is_blaze = false;
  // `WitherBoss.isInvulnerableTo`: at or below half health the wither refuses
  // arrows outright. The caller derives it from the mob's own health
  // (`mob_wither_arrow_immune`).
  bool arrow_immune = false;
  double knockback_resistance = 0.0;
};

// One projectile striking one entity or one block, with everything the caller
// needs to make it real. The damage is already final — `|v| * baseDamage` with
// Power's add and the critical roll — and the push is already Punch's
// (`Entity.push`'s addition, scaled by 0.6 and by the victim's resistance), so a
// caller applies numbers, not rules.
struct ProjectileImpact {
  int32_t projectile_entity_id = 0;
  ProjectileKind kind = ProjectileKind::kArrow;
  int32_t owner_entity_id = -1;
  // Where the projectile was when the hit was resolved: `setPos(hitResult
  // .getLocation())` has already run, exactly as it has for a potion.
  double x = 0.0, y = 0.0, z = 0.0;

  // ---- the entity half ----
  bool hit_entity = false;
  int32_t target_entity_id = -1;
  bool target_is_player = false;
  // The final damage: a snowball's 3 against a blaze, an arrow's
  // `ceil(|v| * baseDamage)` with Power and the critical roll, and 0 for
  // everything else (an egg and a pearl hurt for nothing at all).
  float damage = 0.0f;
  bool critical = false;
  // `Entity.push`: an *addition* to the victim's delta movement, not
  // `LivingEntity.knockback`'s replace — which is why the caller adds it rather
  // than passing it to hurt_mob's knockback arguments.
  double push_x = 0.0, push_y = 0.0, push_z = 0.0;
  // Whether `ProjectileEnvironment::hurt` said the hit landed. An arrow that
  // missed its mark this way is deflected rather than discarded (see the tick).
  bool accepted = false;
  // The lasting effect this kind's `doPostHurtEffects` lands on the victim — 200
  // ticks of Glowing for a spectral arrow, 0 for everything else. It is stated
  // here rather than applied here because vanilla runs it *inside* the
  // `if (hurt)` branch: the caller applies it only when the hit it just accepted
  // landed (and a caller that refuses the hit leaves the flag alone).
  int glowing_ticks = 0;

  // ---- the block half ----
  bool hit_block = false;
  int block_x = 0, block_y = 0, block_z = 0;
  int face = 1;

  // ---- the projectile's own fate ----
  // An arrow has buried itself: it stays in the vector (no despawn) and stops
  // moving, and the caller should send the sync that says so.
  bool stuck = false;
  bool discarded = false;

  // ---- what the *owner* gets out of it ----
  // ThrownEnderpearl.onHit's teleport, to `oldPosition()`.
  bool teleports_owner = false;
  double teleport_x = 0.0, teleport_y = 0.0, teleport_z = 0.0;
  // ThrownEgg.onHit's chickens: 1 or 4, or 0 for no roll at all.
  int chickens = 0;
};

// The world a projectile flies through. `is_solid` answers
// `ClipContext.Block.COLLIDER` for those coordinates and may be empty — in the
// void world there is nothing to hit. `hurt` is `LivingEntity.hurtServer` for
// one target: it applies the damage and the push and returns whether the hit
// landed, which is the branch `AbstractArrow.onHitEntity` itself takes on
// (`if (entity.hurt(...))`). An empty `hurt` means nothing can be hit.
struct ProjectileEnvironment {
  std::vector<ProjectileTarget> targets;
  std::function<bool(int, int, int)> is_solid;
  std::function<bool(const ProjectileImpact&)> hurt;
  // One uniform draw in [0, 1) and one `nextInt(bound)`: the critical's bonus
  // roll, the egg's two chicken rolls, and `startFalling`'s three per-axis
  // scales. The caller owns the randomness so a test can pin it.
  std::function<double()> unit_random;
  std::function<int(int)> next_int;
};

// What one tick of the whole population produced: `moved` need a position sync
// (and, for an arrow whose `in_ground_changed` is set, a synched-data one too),
// `removed` a despawn, `impacts` the work above (already applied through `hurt`
// for an entity hit — the list is the record, so a caller that *also* applies an
// entity impact applies every hit twice).
struct ProjectileTickReport {
  std::vector<int32_t> moved;
  std::vector<int32_t> removed;
  std::vector<ProjectileImpact> impacts;
};

// Advances every live projectile by one tick. Discarded projectiles are erased
// from `projectiles` and reported in both `removed` and `impacts`.
ProjectileTickReport tick_projectiles(std::vector<Projectile>& projectiles,
                                      const ProjectileEnvironment& env);

} // namespace mc::world
