// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mobs: the first entities in this engine that *act* (Phase 3 milestone I).
// One `Mob` is a living entity with gravity, collision, a health pool, a goal
// and — for the hostile ones — a target it walks to and hits.
//
// The shape is the same as world/experience_orb.h: the simulation is pure, it
// takes the world as a predicate (no chunk, no session, no client), and it
// reports what happened so the *caller* turns that into packets. That is what
// lets path finding, the AI and the damage arithmetic be unit-tested directly.
//
// What is mirrored, and from where:
//   * gravity 0.08 and the vertical air drag 0.98 (Attributes.GRAVITY,
//     LivingEntity.travelInAir); the horizontal friction is the ground block's
//     0.6 times the air drag 0.91, or 0.91 in the air;
//   * jump velocity 0.42 (Attributes.JUMP_STRENGTH; a full block is a jump
//     because STEP_HEIGHT is 0.6 and blocks are 1.0 tall);
//   * the AI: `Monster`'s target selector in its vanilla priority order —
//     `HurtByTargetGoal` (whoever hit the mob becomes its target, and is
//     remembered for 300 unseen ticks) then `NearestAttackableTargetGoal` (the
//     nearest non-creative, non-spectator player inside FOLLOW_RANGE *that the
//     mob can see*, re-scanned every 10 ticks while no target is held) — over
//     `MeleeAttackGoal` / `ZombieAttackGoal` (walk to the target at the goal's
//     speed modifier, re-path only while it can be seen, attack when inside
//     `getAttackReachSqr` *and* in line of sight, a 20-tick cooldown between
//     hits); passive mobs get `WaterAvoidingRandomStrollGoal`'s behaviour — walk
//     to a random nearby spot, rest, repeat;
//   * the target *memory*: a mob keeps the target it has for sixty ticks of not
//     seeing it (TargetGoal.unseenMemoryTicks), three hundred when it was hit
//     (HurtByTargetGoal.start), and does not switch to a nearer player while the
//     running goal holds one (GoalSelector only consults `canUse` when nothing
//     is running); out of FOLLOW_RANGE is forgotten at once either way;
//   * `LivingEntity.hurtServer`'s gates (the invulnerability window, the
//     `BYPASSES_ARMOR` step) and `LivingEntity.knockback`;
//   * `Entity.checkFallDamage`: fall distance accumulates while falling and is
//     spent on landing as `minecraft:fall` (safe fall distance 3.0);
//   * `Mob.doHurtTarget`, whose damage source is `minecraft:mob_attack`.
//
// Deliberately NOT modelled, all of it documented rather than approximated:
//   * fluids: no swimming, floating, drowning, burning or lava. A mob that
//     walks into water keeps falling;
//   * collision *shapes*: a block is solid or it is not ("not air and not a
//     fluid"), so slabs, stairs, fences and doors are all full cubes here —
//     which is why a mob cannot walk through a doorway but also cannot be
//     trapped by a trapdoor;
//   * sunlight burning, spawn cycles (the *creation* half is world/mob_spawn.h),
//     breeding, babies, mob equipment, status effects, and every mob-specific
//     goal (door breaking, climbing, ranged attacks, explosions, riding);
//   * `Mob.checkDespawn`'s PEACEFUL branch: it is keyed on a server-wide
//     difficulty this engine fixes at normal (the same constant the world tick's
//     fire odds read — see world/mob_spawn.h's own note). The discard itself
//     *is* modelled; see `check_despawn`.
//   * the client's own prediction: this server owns the mob's position every
//     tick and sends it, instead of integrating the client-side step and only
//     correcting it (vanilla sends small `entity_move` deltas).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "world/animal_food_table.h"
#include "world/effect.h"
#include "world/fire.h"
#include "world/mob_table.h"
#include "world/pathfinding.h"
#include "world/trade.h"

namespace mc::world {

// ---- vanilla constants ------------------------------------------------------

constexpr double kMobGravity = 0.08;      // Attributes.GRAVITY
constexpr double kMobJumpVelocity = 0.42; // Attributes.JUMP_STRENGTH
constexpr double kMobStepHeight = 0.6;    // Attributes.STEP_HEIGHT
// LivingEntity.travelInAir: the horizontal multiplier is the friction of the
// block below (0.6 for every ordinary block) times the 0.91 air drag; in the air
// it is the air drag alone. The vertical drag is 0.98.
constexpr double kMobGroundFriction = 0.6 * 0.91;
constexpr double kMobAirFriction = 0.91;
constexpr double kMobVerticalDrag = 0.98;
// Entity.getEffectiveGravity clamps a falling entity's speed at 3 blocks/tick.
constexpr double kMobTerminalVelocity = 3.0;
// LivingEntity.getMaxAirSupply-ish fall rule: safe_fall_distance 3.0 with a
// fall_damage_multiplier of 1.0 (Player.causeFallDamage's arithmetic).
constexpr double kMobSafeFallDistance = 3.0;

// LivingEntity.hurtServer sets invulnerableTime to 20 and ignores a hit while it
// is still above 10 (the `BYPASSES_COOLDOWN` tag is empty in vanilla, so a
// second hit only lands for the part that exceeds the first).
constexpr float kMobInvulnerableTicks = 20.0f;
constexpr float kMobInvulnerableThreshold = 10.0f;

// MeleeAttackGoal.resetAttackCooldown: adjustedTickDelay(20). MeleeAttackGoal
// requiresUpdateEveryTick(), so the delay is *not* halved: twenty real ticks.
constexpr int kMobAttackInterval = 20;

// ---- ranged attacks (the skeleton family) ----------------------------------

// `RangedBowAttackGoal<>(this, 1.0, 20, 15.0F)`: the attack radius (a *3D*
// `distanceToSqr` in vanilla, kept here as the same squared bound the melee
// reach uses) and the twenty-tick interval between shots. The goal's `see`
// condition is the same line-of-sight answer the target selector already
// carries in `Mob::target_visible`.
constexpr double kMobBowRange = 15.0;
constexpr int kMobBowInterval = 20;
// `AbstractSkeleton.performRangedAttack`'s `shoot(dx, dy + horizontal * 0.2,
// dz, 1.6F, ...)`: the velocity every skeleton-family arrow leaves at, and the
// 20%-of-the-horizontal-distance lift that compensates the arrow's drop.
constexpr double kMobArrowSpeed = 1.6;
constexpr double kMobArrowArc = 0.2;

// ---- animal breeding (Phase 4 milestone I) ---------------------------------

// `Animal.setInLove`: the love countdown (`DEFAULT_IN_LOVE_TIME` is 0, so a
// freshly fed adult goes to 600).
constexpr int kAnimalInLoveTicks = 600;
// `Animal.PARENT_AGE_AFTER_BREEDING`: both parents take this age after a breed,
// which `AgeableMob.aiStep` then counts down one tick at a time.
constexpr int kAnimalParentAgeAfterBreeding = 6000;
// `AgeableMob.BABY_START_AGE` / `getBabyStartAge()`: the age a child starts at,
// negative for as long as it is a baby.
constexpr int kAnimalBabyStartAge = -24000;
// `BreedGoal.PARTNER_TARGETING`: `forNonCombat().range(8.0).ignoreLineOfSight()`,
// searched inside `getBoundingBox().inflate(8.0)`. The engine measures feet to
// feet, which is `distanceToSqr`'s own measure.
constexpr double kAnimalBreedRange = 8.0;
// `BreedGoal.tick`'s `animal.distanceToSqr(partner) < 9.0`: inside three blocks.
constexpr double kAnimalBreedDistanceSqr = 9.0;
// `BreedGoal.tick`'s `loveTime >= adjustedTickDelay(60)`. BreedGoal does not
// `requiresUpdateEveryTick`, so `adjustedTickDelay` is `reducedTickDelay(60)` =
// ceil(60 / 2) = 30 *goal* ticks — and `Mob.serverAiStep` runs the selector's
// full tick on `(tickCount + id) % 2 == 0`, so thirty goal ticks is sixty real
// ticks. `love_time` counts goal ticks; see `tick_mobs`' parity gate.
constexpr int kAnimalBreedLoveTime = 30;
// `BreedGoal.canContinueToUse`'s raw `loveTime < 60` — the *unadjusted* sixty
// the decomp writes beside the adjusted breed check, so the goal outlives the
// breed by another sixty goal ticks.
constexpr int kAnimalLoveContinueTime = 60;

// ---- `FollowParentGoal` ------------------------------------------------

// HORIZONTAL_SCAN_RANGE / VERTICAL_SCAN_RANGE: the goal asks for same-class
// adults whose bounding boxes touch `getBoundingBox().inflate(8.0, 4.0, 8.0)`,
// so the two numbers are slack around the *baby's own* box (which is its baby
// box — `AgeableMob.getDefaultDimensions`), not a centre distance.
constexpr double kAnimalFollowHorizontalRange = 8.0;
constexpr double kAnimalFollowVerticalRange = 4.0;
// DONT_FOLLOW_IF_CLOSER_THAN = 3: the nearest candidate closer than three blocks
// refuses the goal outright (`closestDistSqr < 9.0`), which is what keeps a
// newborn standing beside its parents still. The far end is `canContinueToUse`'s
// `distSqr > 256.0`: a parent past sixteen blocks is let go.
constexpr double kAnimalFollowMinDistanceSqr = 9.0;
constexpr double kAnimalFollowMaxDistanceSqr = 256.0;
// `FollowParentGoal.tick`'s `timeToRecalcPath = adjustedTickDelay(10)`. The goal
// does not `requiresUpdateEveryTick`, so `adjustedTickDelay` is
// `reducedTickDelay(10)` = 5 *goal* ticks — ten real ticks under the selector
// parity, the same cadence BreedGoal runs on. The countdown is what makes the
// goal ask the navigation for a new path only once every five of its own ticks;
// the single path follower walks the path it was given in between.
constexpr int kAnimalFollowRepathTicks = 5;
// NearestAttackableTargetGoal.randomInterval: 10, and its canUse runs every other
// tick (Mob.serverAiStep gates the whole goal selector on the tick parity), so the
// scan lands about every ten real ticks. TargetGoal.canUse's 1/5 chance per check
// is the same budget, and this keeps the single pass a tick model honest.
constexpr int kMobTargetScanInterval = 10;
// TargetGoal.unseenMemoryTicks (60) against the same every-other-tick cadence:
// reducedTickDelay(60) = 30 *checked* ticks, which is sixty real ticks of a mob
// having lost sight of a target it already had before the goal gives up.
constexpr int kMobUnseenMemoryTicks = 60;
// HurtByTargetGoal.start sets `unseenMemoryTicks = 300`, so a mob that was hit
// keeps coming after whoever hit it for fifteen seconds of not seeing them
// (HurtByTargetGoal.java:100). Its *acquisition* conditions ignore line of sight
// entirely (HURT_BY_TARGETING is `forCombat().ignoreLineOfSight()`), but the
// running goal is still a TargetGoal, so the same sight memory governs it.
constexpr int kMobProvokedMemoryTicks = 300;
// MeleeAttackGoal.tick: `ticksUntilNextPathRecalculation = 4 + random.nextInt(7)`,
// re-planned from `tick` only while the target is in sight (or the goal was built
// with followingTargetEvenIfNotSeen).
constexpr int kMobRepathMinInterval = 4;
constexpr int kMobRepathMaxInterval = 10;
// MeleeAttackGoal.canContinueToUse is `!navigation.isDone()` and canUse's
// `lastCanUseCheck` gate is twenty game ticks, so a mob that *cannot* see its
// target keeps walking the path it took and, once that path runs out, re-plans
// through the restart path instead: at most one fresh path every twenty ticks.
constexpr int kMobBlindRepathInterval = 20;
// MeleeAttackGoal.tick's `target.distanceToSqr(pathedTarget*) >= 1.0`: the target
// has moved a block from where the path was built, so the path is stale.
constexpr double kMobRepathDistance = 1.0;
// LivingEntity.hasLineOfSight refuses anything further than 128 blocks.
constexpr double kMobMaxSightDistance = 128.0;
// WaterAvoidingRandomStrollGoal's interval between strolls.
constexpr int kMobStrayMinDelay = 40;
constexpr int kMobStrayMaxDelay = 160;
// `Mob.checkDespawn`'s slow branch: a mob that has done nothing for 600 ticks
// and is further than its category's no-despawn distance away is discarded on a
// 1-in-800 roll. `nextInt(800) == 0` is a uniform 1/800, which is what the
// caller's unit random is compared against.
constexpr int kMobDespawnTicks = 600;
constexpr double kMobDespawnChance = 1.0 / 800.0;
// PathNavigation.followThePath's node reach: bbWidth * 0.75, floored at 0.5.
constexpr double kMobWaypointReach = 0.5;

// ---- the world a mob ticks in ----------------------------------------------

// One player, as a mob sees them. `can_be_targeted` is false for a dead,
// creative or spectator player (EntitySelector.NO_CREATIVE_OR_SPECTATOR, which
// Mob.asValidTarget applies to `getTarget()` as well as to the scan).
// `eye_height` is Player.getEyeHeight() (1.62 standing): LivingEntity
// .hasLineOfSight aims at `target.getX(), target.getEyeY(), target.getZ()`.
struct MobPlayer {
  int32_t entity_id = 0;
  double x = 0.0, y = 0.0, z = 0.0;
  float width = 0.6f;
  float height = 1.8f;
  float eye_height = 1.62f;
  bool can_be_targeted = true;
  // `Player.isSpectator()`, which is the *only* thing `Mob.checkDespawn`'s
  // `Level.getNearestPlayer(this, -1.0)` filters on (NO_SPECTATORS — a creative
  // player still keeps the mobs around them alive). `can_be_targeted` is a
  // different, narrower question (NO_CREATIVE_OR_SPECTATOR), so the two cannot
  // share a flag.
  bool spectator = false;
};

struct MobEnvironment {
  std::vector<MobPlayer> players;
  // Whether the block at those world coordinates stops a mob. May be empty (the
  // void world): then nothing is solid, no cell is walkable and mobs simply fall.
  std::function<bool(int, int, int)> is_solid;
  // The block at those coordinates, for the fire a mob is standing in
  // (world/fire.h). Empty means no block is fire, which is what a bare unit test
  // wants.
  std::function<mc::world::BlockStateKey(int, int, int)> get_block;
  // `Level.isRainingAt(pos)` (world/weather.h): rain puts a burning mob out via
  // `Entity.applyEffectsFromBlocks`'s `isInRain` -> `clearFire`. Empty means no
  // weather, which leaves a mob burning exactly as it did before there was any.
  std::function<bool(int, int, int)> is_raining_at;
  // One uniform draw in [0, 1): target scans, stroll directions and attack
  // timing. The caller owns the randomness so a test can pin it.
  std::function<double()> unit_random;
  // `Level.getMinY()`: the dimension's lowest build height. `Entity
  // .checkBelowWorld` discards an entity whose feet fall more than 64 blocks
  // under this, so the overworld (min_y -64) discards below y = -128. The
  // default is the overworld's floor; a caller that owns a world sets the real
  // one.
  int min_y = -64;
};

// ---- one mob ----------------------------------------------------------------

// The AI's current intent, mirroring the goal that is running in vanilla.
enum class MobGoal : uint8_t {
  Idle = 0,     // no goal: standing still
  Stroll,       // WaterAvoidingRandomStrollGoal: walking to a random spot
  FollowParent, // FollowParentGoal: a baby walking to the nearest adult
  Chase,        // MeleeAttackGoal, target out of reach
  Attack,       // MeleeAttackGoal, target inside reach
  RangedAttack, // RangedBowAttackGoal, a bow-armed mob with a visible target in range
};

// ---- the wither's encounter (Phase 4 boss fight) ----------------------------
//
// `WitherBoss.invulnerableTicks`: 220 ticks of spawn shield, during which the
// boss takes no damage and its health climbs to full; the shield ends in a
// power-7 blast. After it, the wither stands at range and looses one skull every
// 40 ticks at a target inside 40 blocks. The arrow armour is the half-health
// line `mob_wither_arrow_immune` answers.
constexpr int kWitherShieldTicks = 220;
constexpr float kWitherShieldExplosionPower = 7.0f;
constexpr int kWitherSkullIntervalTicks = 40;
constexpr double kWitherSkullRange = 40.0;

// `EnderDragon`'s fight, reduced to the phases this engine simulates: a long
// circling turn, a dive at the target, a landing at half health and a takeoff
// after the perch. The radius and altitude are relative to the perch column.
constexpr int kDragonCircleTicks = 240;
constexpr int kDragonChargeTicks = 40;
constexpr int kDragonPerchTicks = 120;
constexpr int kDragonTakeoffTicks = 60;
constexpr int kDragonPerchCooldownTicks = 200;
constexpr double kDragonCircleRadius = 12.0;
constexpr double kDragonCircleHeight = 10.0;

struct Mob {
  int32_t entity_id = 0;
  const GeneratedMob* type = nullptr;
  // Entity.remainingFireTicks (world/fire.h). A mob's `getFireImmuneTicks` is
  // 0, so this idles at 0 rather than at a player's -20.
  Burning burning;
  // `LivingEntity.activeEffects`: the lasting effects on this mob. The container
  // is the players' own (`world/effect.h`), reused whole, so a duration, an
  // amplifier, a merge and an expiry all behave the same for a mob as for a
  // player. One writer lands one effect today — a spectral arrow's 200 ticks of
  // Glowing, which is what `mob_add_effect` is called with — and the only thing
  // read out of the set is the glowing bit below (a mob has no attribute
  // modifiers and its effect *events* would need a damage sink).
  ActiveEffects effects;
  // `LivingEntity.updateGlowingStatus`'s answer: `hasEffect(MobEffects.GLOWING)`.
  // It is a *field* rather than a call because the client learns a mob's
  // glowing from the shared-flags byte (flag 6) rather than from an effect
  // packet, and that byte is only sent when it changes — so the tick has to be
  // able to compare this against what the last tick held.
  bool glowing = false;
  // Position of the feet centre, delta movement in blocks per tick, yaw in
  // degrees (Entity.getYRot; 0 faces +z, and the look vector is
  // (-sin yaw, 0, cos yaw)).
  double x = 0.0, y = 0.0, z = 0.0;
  double vx = 0.0, vy = 0.0, vz = 0.0;
  float yaw = 0.0f;
  float health = 0.0f;
  float invulnerable_time = 0.0f;
  float last_hurt = 0.0f;
  bool on_ground = false;
  bool removed = false;
  // Entity.horizontalCollision for the tick just simulated (the path follower
  // uses it to decide a jump, like PathNavigation.followThePath).
  bool horizontal_collision = false;
  int age = 0;
  // ---- `AgeableMob`/`Animal` state (Phase 4 milestone I) ----
  // `AgeableMob.age`: negative while this is a baby, 0 when grown and ready,
  // positive while a parent is on its post-breeding cooldown. It is deliberately
  // *not* `age` above — that one is `Entity.tickCount`, which vanilla keeps
  // separate and which `mob_tick_effects` already reads.
  int breed_age = 0;
  // `AgeableMob.forcedAge`: what forced feeding (a player's food) has pushed the
  // age toward 0. Its one use is the `setAge(forcedAge)` jump at the end of
  // `ageUp`, which the feeding path cannot reach — see `mob_age_up`.
  int forced_age = 0;
  // `Animal.inLove`: the countdown `setInLove` starts at 600. It is cleared by
  // `Animal.aiStep`/`customServerAiStep` whenever the age is not 0, so only an
  // adult at age 0 can hold one.
  int in_love = 0;
  // `BreedGoal.loveTime`: how long this mob's breed goal has been running
  // toward its partner, in *goal* ticks (one per full selector tick).
  int love_time = 0;
  // `BreedGoal.partner`, as an entity id (-1 = none).
  int32_t mate_entity_id = -1;
  // `Animal.loveCause`: the player whose feeding started the current love, as a
  // name because the player registry is the session's and this module owns
  // none. `setInLove(player)` writes it and `resetLove()` clears it; the three
  // things that read it are `finalizeSpawnChildFromBreeding`'s pair (the
  // `minecraft:bred_animals` advancement and the `animals_bred` stat, both
  // awarded to the cause, not to the mob) and nothing else.
  std::string love_cause;
  // `FollowParentGoal.parent`, as an entity id (-1 = the goal is not running).
  // Only a baby ever holds one (`canUse` wants an age below 0) and only an
  // adult can be one (the scan skips the babies of its own class).
  int32_t parent_entity_id = -1;
  // `FollowParentGoal.timeToRecalcPath`: counts *goal* ticks down from five
  // between the navigation re-plans (`adjustedTickDelay(10)`); zero means "ask
  // for a path on the next tick of the goal".
  int follow_repath = 0;
  // `LivingEntity.noActionTime`: bumped once a tick by `Mob.serverAiStep`, and
  // zeroed by `checkDespawn` itself whenever the mob is within its category's
  // no-despawn distance. It is what the slow despawn branch counts.
  int no_action_time = 0;
  // `Mob.isPersistenceRequired()`: a mob that must not despawn however far it
  // wanders. Nothing in the engine sets it yet (a name tag, a spawn egg, a mob
  // that picked something up all would), but the field is read by both
  // `check_despawn` and the natural spawner's population scan, so it has to
  // exist for either to be complete.
  bool persistence_required = false;
  // Entity.fallDistance, computed exactly: the fall starts at the last y the mob
  // was standing at (`fall_start_y`) and the distance is the difference when it
  // lands, so a four-block drop costs exactly one point of fall damage.
  double fall_distance = 0.0;
  double fall_start_y = 0.0;
  bool falling = false;
  // ---- the wither's encounter state ----
  // `WitherBoss.invulnerableTicks`: while positive `hurt_mob` refuses every hit
  // and the tick raises health along the boss bar; at zero the spawn blast is
  // reported. `wither_shot_cooldown` is the 40-tick skull cadence.
  int wither_shield_ticks = 0;
  int wither_shot_cooldown = 0;

  // ---- the ender dragon's encounter state ----
  // `EnderDragon`'s phase manager, reduced to four states: 0 circles, 1 charges
  // the target, 2 perches and 3 takes off. `dragon_phase_ticks` is the time in
  // the current phase and `dragon_perch_cooldown` delays the next landing after
  // a takeoff. The perch is the summon column: this engine has no End dimension
  // and no exit portal to anchor the vanilla perch to.
  int dragon_phase = 0;
  int dragon_phase_ticks = 0;
  int dragon_perch_cooldown = 0;
  double dragon_perch_x = 0.0, dragon_perch_y = 0.0, dragon_perch_z = 0.0;

  // ---- AI state ----
  MobGoal goal = MobGoal::Idle;
  int32_t target_entity_id = -1;
  int attack_cooldown = 0;
  int target_scan_cooldown = 0;
  int path_cooldown = 0;
  int stray_cooldown = 0;
  // `PathNavigation.speedModifier`: the multiplier the last `moveTo` set, which
  // the path follower scales `getSpeed()` by every tick. Every goal this engine
  // carries asks for 1.0 except `FollowParentGoal` (1.1 for the pig, sheep and
  // chicken, 1.25 for the cow, out of the type's `follow_parent_speed`), so the
  // field only ever leaves 1.0 while a baby is walking to a parent.
  double move_speed_scale = 1.0;
  // Whether the mob had line of sight to its target on this tick
  // (LivingEntity.hasLineOfSight, aimed eye to eye), and for how long it has not
  // (TargetGoal.unseenTicks, spent against kMobUnseenMemoryTicks).
  bool target_visible = false;
  int unseen_ticks = 0;
  // Where the target came from: the nearest-visible scan, or being *hurt*
  // (HurtByTargetGoal). A provoked target is picked up without line of sight —
  // its acquisition conditions ignore it — but it is still held by
  // TargetGoal.canContinueToUse's sight memory, which for this goal is the 300
  // ticks HurtByTargetGoal.start sets rather than the scan's 60.
  bool target_provoked = false;
  // HurtByTargetGoal's "last hurt by" record: who hit this mob and whether the
  // tick has already reacted to it (`LivingEntity.setLastHurtByMob` bumps the
  // timestamp HurtByTargetGoal.canUse watches).
  int32_t provoked_entity_id = -1;
  bool provoked_fresh = false;
  NavPath path;
  // The position the current path was built toward (PathNavigation's
  // "pathed target"), so a moved target forces a re-plan
  // (MeleeAttackGoal.tick's `distanceToSqr(pathedTarget*) >= 1.0`).
  double path_to_x = 0.0, path_to_y = 0.0, path_to_z = 0.0;
  bool has_path_goal = false;
  // ---- `PathNavigation`'s stuck detection ----
  // `PathNavigation.tick`'s own counter plus the fields `doStuckDetection`
  // keeps: the last movement check (`lastStuckCheck`, `lastStuckCheckPos`), the
  // per-node timeout (`timeoutCachedNode`, `timeoutTimer`, `timeoutLimit`) and
  // its own `isStuck` answer. Vanilla stops the navigation in either case, and
  // a stopped navigation is what ends the goal that owns it
  // (`RandomStrollGoal.canContinueToUse` is `!navigation.isDone()`), so a mob
  // wedged against geometry picks a fresh target instead of pushing into it for
  // the rest of its life. The check lives in `world/mob.cpp`'s `follow_path`.
  int nav_tick = 0;
  int last_stuck_check = 0;
  double last_stuck_check_x = 0.0, last_stuck_check_y = 0.0, last_stuck_check_z = 0.0;
  bool is_stuck = false;
  NavCell timeout_node;
  bool has_timeout_node = false;
  int timeout_timer = 0;
  int timeout_last_check = 0;
  double timeout_limit = 0.0;

  // ---- villager state (Phase 4 milestone L) ----
  // `Villager`'s merchant half: the data-pack profession and type (some trades
  // are gated on the type), the level and xp, the lazily built offers, and the
  // trade screen's own counters. Only a `minecraft:villager` ever reads it;
  // every other type carries the default and never touches it.
  VillagerState villager;
  bool is_villager() const {
    return type != nullptr && std::string_view(type->name) == "minecraft:villager";
  }

  // ---- damage attribution (for the death message, drops and XP) ----
  int32_t last_attacker_entity_id = -1;
  std::string last_attacker_name;
  bool killed_by_player = false;

  bool is_alive() const {
    return health > 0.0f;
  }
  bool hostile() const {
    return type != nullptr && (type->flags & kMobHostile) != 0;
  }
  // `HurtByTargetGoal`: whoever damages this mob becomes its target, held with
  // the 300-tick memory that goal sets. It is the *only* targeting goal a
  // non-hostile with this flag has — a wolf, a polar bear, an iron golem — so
  // `update_target` reaches the nearest-target scan for a hostile mob alone.
  bool retaliates() const {
    return type != nullptr && (type->flags & kMobRetaliates) != 0;
  }
  // `AgeableMob.isBaby()`: the age is the whole answer (`canBeABaby` is true for
  // every mob this table carries).
  bool baby() const {
    return breed_age < 0;
  }
  // `Animal.canFallInLove()`.
  bool can_fall_in_love() const {
    return in_love <= 0;
  }
  // `AgeableMob.canAgeUp()`: `isBaby() && !isAgeLocked()`. Age locking (the
  // golden dandelion) is not modelled, so it is the baby test alone — the same
  // shape the engine documents for the other mob features it does not carry.
  bool can_age_up() const {
    return baby();
  }
  bool is_in_love() const {
    return in_love > 0;
  }
  // `Animal.canMate`: the same class (one generated type per class here), both
  // in love, and never itself.
  bool can_mate_with(const Mob& other) const {
    return &other != this && other.type == type && is_in_love() && other.is_in_love();
  }
  // Whether this mob is an `Animal`: an `AgeableMob` with a food tag.
  bool animal() const {
    return type != nullptr && (type->flags & kMobAnimal) != 0;
  }
  // NearestAttackableTargetGoal was built with mustSee for this type, so it can
  // only *acquire* a target it can see (TargetingConditions.forCombat's
  // checkLineOfSight).
  bool must_see() const {
    return type != nullptr && (type->flags & kMobMustSee) != 0;
  }
  // MeleeAttackGoal's followingTargetEvenIfNotSeen, which is the *tick* re-plan
  // gate: without it the mob still walks the path it had (and re-plans through
  // the goal's restart, twenty ticks at a time).
  bool follows_target_unseen() const {
    return type != nullptr && (type->flags & kMobFollowsUnseen) != 0;
  }
  // The sight memory the running target goal holds its target with: the nearest
  // scan's 60 ticks, or the 300 HurtByTargetGoal gives a target that hit back.
  int unseen_memory_ticks() const {
    return target_provoked ? kMobProvokedMemoryTicks : kMobUnseenMemoryTicks;
  }
  // Mob.getBbWidth / getBbHeight, from the type's dimensions — and
  // `getDefaultDimensions`' `isBaby() ? BABY_DIMENSIONS : super` for the four
  // animals, whose baby boxes are the table's own baby columns.
  double half_width() const {
    if (!type)
      return 0.3;
    return (baby() ? static_cast<double>(type->baby_width) : static_cast<double>(type->width)) *
           0.5;
  }
  double body_height() const {
    if (!type)
      return 1.8;
    return baby() ? static_cast<double>(type->baby_height) : static_cast<double>(type->height);
  }
  const char* display_name() const {
    return type ? type->display_name : "?";
  }
};

// ---- line of sight ----------------------------------------------------------

// Entity.getEyeY for a mob: the position plus EntityDimensions' default eye
// height, which EntityType.Builder.sized() sets to `height * 0.85`.
double mob_eye_y(const Mob& mob);
double player_eye_y(const MobPlayer& player);

// The geometry of Entity.hasLineOfSight on its own: whether the segment between
// two eye positions reaches anything solid on the way (`Block.COLLIDER`, so a
// block that is not a collider never stops it), refused outright past 128 blocks.
// An empty predicate means "no world, nothing to hide behind".
//
// It is deliberately direction-free — vanilla's check does not care which end it
// is called from, and neither does this — because both sides of a fight want it:
// `mob_has_line_of_sight` is this with the mob's own eye at one end, and the
// session's player-attack path is this with the *player's*.
bool eye_line_of_sight(const std::function<bool(int, int, int)>& is_solid, double from_x,
                       double from_y, double from_z, double to_x, double to_y, double to_z);

// LivingEntity.hasLineOfSight(target): the segment from this mob's eye to the
// other entity's eye.
bool mob_has_line_of_sight(const Mob& mob, const MobEnvironment& env, double target_x,
                           double target_y, double target_z);

// Mob's movement speed in blocks per tick, from the vanilla attribute. The
// conversion is the measured one (m/s = attribute^2 * 43.1718; the linear
// * 43.1 only holds for the player and horses — Minecraft Wiki, Talk:Attribute),
// divided by the 20 ticks in a second.
double mob_speed_per_tick(const GeneratedMob& type);

// Gives `mob` a type, a position and full health, and moves it out of a block
// it spawned inside (the experience orb's unstuck pass: `/summon` at a block
// position would otherwise leave a mob buried for its whole life). The unstuck
// search prefers the nearest supported free cell on the summon's own level and
// only lifts when that level offers none — a lateral nudge keeps a mob summoned
// against a wall on the floor, where a lift would stand it on the wall's top.
void mob_place(Mob& mob, const GeneratedMob& type, const MobEnvironment& env, double x, double y,
               double z, float yaw);

// ---- what one tick produced -------------------------------------------------

// A mob hit a player: the caller raises `minecraft:mob_attack` for it.
struct MobAttack {
  int32_t mob_entity_id = 0;
  int32_t player_entity_id = 0;
  float damage = 0.0f;
  std::string mob_name;
};

// A mob died this tick. Everything the caller needs to drop the loot and spawn
// the orbs is copied out before the mob is erased from the population.
struct MobDeath {
  int32_t entity_id = 0;
  const GeneratedMob* type = nullptr;
  double x = 0.0, y = 0.0, z = 0.0;
  bool killed_by_player = false;
  int32_t killer_entity_id = -1;
  std::string killer_name;
};

// A breeding pair produced a child this tick (`Animal.spawnChildFromBreeding`).
// The module owns no population, so the caller creates the entity at this
// position (the *initiator's* own position — `snapTo`), places it and tells the
// clients; the parents are already on their 6000-tick cooldown in `mobs`.
struct MobBirth {
  int32_t parent_entity_id = 0;
  int32_t partner_entity_id = 0;
  const GeneratedMob* type = nullptr;
  double x = 0.0, y = 0.0, z = 0.0;
  float yaw = 0.0f; // snapTo(..., 0.0f, 0.0f)
  // `Optional.ofNullable(this.getLoveCause()).or(() ->
  // Optional.ofNullable(partner.getLoveCause()))`: whoever fed either parent,
  // or empty when neither was fed (a pair that bred from two love causes is
  // the normal case; one parent fed by hand and one not cannot happen, since
  // both must be in love). The caller awards `animals_bred` and the
  // `bred_animals` advancement to this player.
  std::string love_cause;
};

// A projectile a mob wants this tick (`RangedBowAttackGoal.performRangedAttack`,
// and `WitherBoss`'s skull volley). The module owns no projectile registry, so
// it reports the aim and the caller creates the entity: `yaw`/`pitch` are the
// same degrees `shoot_arrow` takes, computed from the shooter's eye to the
// target's own aim point (plus the arrow's arc, which a skull does not carry).
enum class MobShotKind : uint8_t {
  kArrow = 0,
  kWitherSkull = 1,
};

struct MobShot {
  int32_t mob_entity_id = 0;
  MobShotKind kind = MobShotKind::kArrow;
  double x = 0.0, y = 0.0, z = 0.0; // the shooter's eye position
  float yaw = 0.0f, pitch = 0.0f;
};

// The wither's spawn shield ending: `WitherBoss.customServerAiStep` calls
// `Level.explode(this, x, y, z, 7.0F, false, MOB)` on the tick the shield reaches
// zero. The module owns no world, so the blast is reported and the caller
// applies it (and excludes the wither from its own blast).
struct MobExplosion {
  int32_t entity_id = 0;
  double x = 0.0, y = 0.0, z = 0.0;
  float power = 0.0f;
};

struct MobTickReport {
  std::vector<int32_t> moved; // position and/or yaw changed: send a sync
  // Children a breeding pair produced this tick. The caller creates each one
  // with `AgeableMob.age` at `kAnimalBabyStartAge` (it is setBaby(true) in
  // vanilla), so the *caller* is what puts the baby bit on the spawn's pairing
  // data.
  std::vector<MobBirth> births;
  // Babies whose age crossed 0 this tick: `AgeableMob.setAge` flips
  // `DATA_BABY_ID` (index 16) there, so the client needs the bit cleared.
  std::vector<int32_t> grew_up;
  std::vector<MobAttack> attacks;
  // Arrows this tick's bow-armed mobs want to loose. The caller spawns them
  // through the same projectile registry a player's bow uses.
  std::vector<MobShot> shots;
  // Blasts a mob's own behaviour raised this tick: the wither's shield ending.
  std::vector<MobExplosion> explosions;
  std::vector<int32_t> hurt; // took damage: play the hurt animation
  // The shared-flags byte changed: send it. It carries the on-fire bit
  // (`Entity.setSharedFlagOnFire`) and the glowing bit
  // (`LivingEntity.updateGlowingStatus`), so either one changing puts the mob
  // here once.
  std::vector<int32_t> flags_changed;
  // Villagers whose merchant career rose this tick. `increaseMerchantCareer`
  // goes through `setVillagerData`, so the caller resends the villager's
  // synched word (and the offers the level appended are already in `mob`).
  std::vector<int32_t> villager_leveled;
  // Villagers that finished a trade at some earlier point: the tick is where
  // `Villager.customServerAiStep` spends `lastTradedPlayer`, and spending it
  // broadcasts entity event 14 (the happy-villager particles).
  std::vector<int32_t> villager_traded;
  std::vector<MobDeath> deaths;
  // Discarded without a death — `Entity.discard`, so the client is told to
  // forget the entity and *nothing else happens*: no death event, no loot, no
  // experience. Two paths discard this way: `Mob.checkDespawn` (far away, or
  // the 1-in-800 idle roll) and `Entity.checkBelowWorld` (feet below
  // `minY - 64`).
  std::vector<int32_t> despawned;
};

// Advances every mob by one tick: the despawn check, the target scan, the goal,
// the path, the movement, the fall damage and the melee attack. Mobs that died
// or were discarded (despawned or below the world) are erased from `mobs` (a
// death's data survives in the report; a discard's is a bare entity id,
// because nothing follows it).
MobTickReport tick_mobs(std::vector<Mob>& mobs, const MobEnvironment& env);

// Whether this population is large enough for `MobGrid` — the 8-block buckets
// behind `BreedGoal.getFreePartner` and `FollowParentGoal.canUse` — to be built,
// as this build decided. The switch is `MobGrid`'s own `kMinPopulation`;
// building with `PLUSMC_MOB_GRID_OFF` answers "never", which is the population
// scan the digest corpus pins. That arm is a compile-time define, not a setting:
// it exists only in the second digest binary `ctest -R mob-grid-ab` links, so a
// deployed server has no switch to turn off. Exported so either binary can say
// which arm it is instead of a harness assuming it.
bool mob_grid_enabled_for(size_t population);

// `Mob.checkDespawn`: whether this mob should be discarded right now. Called for
// every mob at the top of its tick, before the AI (vanilla's `ServerLevel` runs
// it in its entity forEach, ahead of `entity.tick()`). A mob further than its
// category's despawn distance from the nearest non-spectator player goes at
// once — unless the type is one of the animals that `removeWhenFarAway` keeps
// (`kMobNoFarDespawn`) — and one that has done nothing for 600 ticks and is
// beyond the 32-block no-despawn distance goes on a 1-in-800 roll. Nothing
// follows a discard: the caller is told the id and no more.
bool check_despawn(Mob& mob, const MobEnvironment& env);

// ---- animal feeding + breeding (Phase 4 milestone I) ------------------------

// `Animal.isFood(ItemStack)` over the generated table
// (world/animal_food_table.h): the item tag each class tests. False for a mob
// that is not an `Animal` at all.
bool mob_eats(const Mob& mob, std::string_view item_name);

// `Animal.getSpeedUpSecondsWhenFeeding(ticksUntilAdult)`: the whole seconds a
// feeding takes off — `(int)((ticksUntilAdult / 20) * 0.1f)`, the integer
// division *before* the tenth (a tenth of 24000 ticks is 120 seconds, and under
// twenty ticks to go it is nothing at all).
int animal_speed_up_seconds(int ticks_until_adult);

// `AgeableMob.ageUp(seconds, forced)`, exactly: the age moves toward 0 by
// `seconds * 20` ticks and stops there, a forced call banks the delta in
// `forced_age`, and — vanilla's own last line — an age that lands *exactly* on 0
// is replaced by `forced_age` (the amount forced feeding had banked). The
// feeding path cannot reach that line (a feeding is a floored tenth of the
// remainder, which is never the remainder), but it is here because it is the
// method, and `mob_test` pins why it cannot fire.
void mob_age_up(Mob& mob, int seconds, bool forced);

// `Animal.mobInteract`'s food branch and nothing else. An adult at age 0 that
// can fall in love starts its 600-tick love (`setInLove`); a baby that can age
// up is fed `Animal.getSpeedUpSecondsWhenFeeding` seconds of growth; anything
// else does neither. `consumed` is `usePlayerItem`: the caller spends one item
// (`ItemStack.consume(1, player)`, a no-op for an instabuild player). The heart
// burst a successful `setInLove` broadcasts is the caller's packet to send.
//
// `love_cause` is the feeding player's name — `setInLove(player)`'s own
// argument, which vanilla stores as `loveCause` for the eventual birth to read
// (see `Mob::love_cause`). A baby's feeding has no cause to record.
struct MobFeedResult {
  bool consumed = false;
  bool fell_in_love = false;
  bool aged_up = false;
};
MobFeedResult feed_mob(Mob& mob, bool is_food, std::string_view love_cause = {});

// `LivingEntity.addEffect` for a mob: the instance merges into the set (a
// stronger or longer one takes over, an equal or weaker one does not) and the
// glowing bit is refreshed from it (`world/effect.h`'s `effects_glow`, the same
// derivation the player's flags use). Returns whether the set changed. The
// effect events an instantaneous or ticking effect would produce are
// deliberately not returned: nothing that writes a lasting effect to a mob today
// produces one (Glowing's tick is a no-op), and a mob's effect damage would have
// to travel this module's own `hurt_mob` rather than a player's survival sink.
bool mob_add_effect(Mob& mob, const MobEffectInstance& instance);

// `LivingEntity.tickEffects` for a mob: ages every instance, drops the expired
// ones and refreshes the glowing bit. Returns whether the *bit* changed, which
// is what the caller resends the shared flags for.
bool mob_tick_effects(Mob& mob);

// ---- damage from outside (a player's attack, /damage, fall damage) ----------

struct MobHurtResult {
  bool accepted = false;
  float applied = 0.0f;
  bool died = false;
  // The invulnerability window clipped the hit (LivingEntity.hurt's
  // "only the part above the last hit lands" branch).
  bool reduced = false;
};

// `WitherBoss.isPowered`: the wither's second phase, which vanilla enters when
// its health reaches half or below (`WitherBoss.setHealth` derives the flag from
// `health <= maxHealth / 2`). It is derived here from the live health rather
// than stored, so every damage path — a sword, an arrow, fire, a fall, `/damage`
// — crosses the threshold without knowing the phase exists. Only the wither has
// one; every other row answers false.
bool mob_wither_powered(const Mob& mob);

// `LivingEntity.getArmorValue`: the type's generated ARMOR attribute, with the
// `WitherBoss` override on top — 4 points normally and 4 more while powered
// (8 at half health). `hurt_mob` reads this instead of `type->armor` so the
// armour step follows the phase; a `bypasses_armor` type (`minecraft:fall`)
// still skips the whole step.
float mob_armor_value(const Mob& mob);

// `WitherBoss.hurtServer`'s phase-two refusal: a powered wither ignores every
// projectile (`source.is(DamageTypeTags.IS_PROJECTILE)`). The arrow path spends
// the arrow through `ProjectileTarget::arrow_immune`; this predicate is the
// general refusal the session's projectile sink checks before `hurt_mob`, so a
// thrown potion or snowball does not land either.
bool mob_deflects_projectiles(const Mob& mob);

// LivingEntity.hurtServer + Mob.actuallyHurt for one mob. `bypasses_armor` is
// `damage_has_tag(type, kDamageTagBypassesArmor)` of the type the caller is
// applying — true for `minecraft:fall`, false for a player's attack — because
// the armour step is the one place the damage *type* changes the arithmetic
// (world/damage.h's `damage_after_armor` does the same test for a player).
// `knockback_x` / `knockback_z` is the direction the hit pushes
// (LivingEntity.knockback takes the attacker's look vector), `knockback` its
// strength; 0 leaves the mob's movement alone. `attacker_*` name the responsible
// player for the death message, the loot's "killed by player" gate and the XP.
MobHurtResult hurt_mob(Mob& mob, float damage, int32_t attacker_entity_id,
                       std::string_view attacker_name, double knockback, double knockback_x,
                       double knockback_z, bool bypasses_armor = false);

// `WitherBoss`'s two server-side predicates. The arrow armour is
// `getHealth() <= getMaxHealth() / 2.0F` — the same threshold the client's
// WitherArmorLayer draws — and the spawn shield is the 220-tick counter above.
// Both answer false for every other mob.
bool mob_is_wither(const Mob& mob);
bool mob_wither_arrow_immune(const Mob& mob);
bool mob_is_dragon(const Mob& mob);

// Player.isWithinAttackRange, for the serverbound attack packet's range gate:
// the distance from the attacker's eyes to the mob's box, inside
// `reach` blocks (ENTITY_INTERACTION_RANGE 3.0 plus the packet handler's 3.0
// lag buffer in vanilla's `handleAttack`).
bool mob_within_player_reach(const Mob& mob, double eye_x, double eye_y, double eye_z,
                             double reach);

// The two lines `MeleeAttackGoal` stops at, exported so a test can pin the
// *promise* rather than a calibrated distance: `attack_reach_sqr` is
// `(2 * mob width)^2 + the player's own width` — Mob.getAttackBoundingBox's
// inflate plus the target's width — and `perceived_distance_sqr` is the gap
// between the mob's position and the player's *box* (0 inside it), squared. The
// goal walks while the second is larger than the first and swings from inside
// it, so "it stopped at its reach" is exactly
// `perceived_distance_sqr(mob, p) <= attack_reach_sqr(mob, p)`.
double attack_reach_sqr(const Mob& mob, const MobPlayer& player);
double perceived_distance_sqr(const Mob& mob, const MobPlayer& player);

// Player.doSweepAttack's victim test, for the mob the swing is aimed at:
// `target.getBoundingBox().inflate(1.0, 0.25, 1.0)` around the target's own box,
// intersected by `other`'s, *and* the two centres within three blocks
// (`distanceToSqr(nearby) < 9.0`). `other` is never the target itself: the
// caller decides that, because vanilla also excludes the attacker and its allies.
bool mob_in_sweep_range(const Mob& target, const Mob& other);

} // namespace mc::world
