// SPDX-License-Identifier: GPL-3.0-or-later
//
// Combat: the player's half of a melee hit, as Player.attack computes it
// (Phase 4 milestone G). Everything here is a pure function over the numbers
// the session already has — the held weapon's attributes, the enchantment
// deltas world/enchantment.h resolved for *this* victim, the swing's charge and
// the attacker's state — so the arithmetic can be unit-tested with no session,
// no world and no client, and the session is left with the packets.
//
// The order is Player.attack's own, because two of its steps are only
// explicable by it:
//
//   scale       = clamp((attackStrengthTicker + 0.5) / delay)
//   baseDamage *= baseDamageScaleFactor() = 0.2 + scale^2 * 0.8
//   baseDamage += getAttackDamageBonus(...)  // only the mace's is not 0.0f
//   baseDamage *= 1.5                     // if the swing is critical
//   magicBoost  = scale * (enchanted(baseDamage) - baseDamage)
//   totalDamage = baseDamage + magicBoost
//
// so an enchantment's bonus is *not* multiplied by the critical 1.5 (the crit
// lands on the weapon's damage before the enchantment's add is scaled in) —
// though the mace's smash is, because it joins baseDamage before the
// multiplier — and a half-charged swing scales the enchantment down with
// everything else.
//
// One line of that sequence is a *weapon class* rather than a number:
//
//   baseDamage += attackingItemStack.getItem()
//       .getAttackDamageBonus(entity, baseDamage, damageSource)
//
// For every item but the mace that add is `Item.getAttackDamageBonus`'s 0.0f;
// for the mace it is the *smash*, defined only while `canSmashAttack` holds
// (`fallDistance > 1.5` and not fall-flying) and computed from the attacker's
// fall distance:
//
//   d     = fallDistance
//   bonus = d <= 3 ? 4d : (d <= 8 ? 12 + 2(d - 3) : 22 + d - 8)   // the curve
//         + Density * d            // minecraft:smash_damage_per_fallen_block
//
// It lands on baseDamage *before* the critical's 1.5, which is Player.attack's
// own order (the add at line 910, the multiplier at line 913) — and a smash is
// nearly always also a critical, because the gate that defines it
// (`fallDistance > 1.5`, airborne) already satisfies canCriticalAttack. The two
// are separate gates and are kept separate here: a blinded smasher still smashes.
//
// A smash is three more things the session owns, because they are not numbers in
// this sequence: `MaceItem.getItemDamageSource` swaps the *damage type* to
// `minecraft:mace_smash`, so the damage event the client draws — and the death
// message, `death.attack.mace_smash` — changes with it; `MaceItem.knockback`
// pushes every living entity within 3.5 blocks of the victim (the victim and the
// attacker themselves excluded), which is `smash_knockback_candidate` plus
// `smash_push`; and `MaceItem.postHurtEnemy` resets the attacker's fall
// distance, so the drop the smash was thrown from does not then break the
// smasher's legs. `CombatAttack` reports `smash` so the session can do all three.
//
// That push reaches two populations with one predicate and one geometry, and the
// session asks both of them the same way: the *candidate* is one call, the vector
// is one call, and the only thing the two branches keep to themselves is where
// the answer lands — a mob's velocity is the server's to write, a player's is its
// own client's, so that half is a packet.
//
// What a smash still does not do here, named rather than approximated: Breach's
// `minecraft:armor_effectiveness`, which lands inside CombatRules
// .getDamageAfterAbsorb and wants the armour step to see the weapon (world/
// damage.h's `damage_after_absorb` has no parameter for it); the sound and the
// particle (`MaceItem.hurtEnemy`'s level events — there is no particle packet in
// this engine); and the impulse pair `setDeltaMovement(y, 0.01f)` plus
// `setIgnoreFallDamageFromCurrentImpulse`, which need an impulse model this
// server has none of. The fall-distance reset that does the same job across a
// landing is the half of that pair that matters, and it is implemented.
//
// The sweep is `Player.doSweepAttack`, which is one flat constant away from the
// intuitive formula: a swept victim takes `1.0 + ratio * baseDamage` (not
// `ratio * baseDamage`), enchanted and strength-scaled exactly like the primary.
#pragma once

#include <cstdint>

#include "world/enchantment.h"

namespace mc::world {

// Player.attack's full charge gate: `attackStrengthScale > 0.9`.
constexpr float kFullChargeScale = 0.9f;
// Player.attack's critical multiplier.
constexpr float kCriticalMultiplier = 1.5f;
// Player.attack's sprint knockback, added to getKnockback.
constexpr double kSprintKnockback = 0.5;
// Player.doSweepAttack's knockback on every swept victim.
constexpr double kSweepKnockback = 0.4;

// MaceItem's smash constants.
constexpr double kSmashFallThreshold = 1.5;   // SMASH_ATTACK_FALL_THRESHOLD
constexpr double kSmashHeavyThreshold = 5.0;  // SMASH_ATTACK_HEAVY_THRESHOLD
constexpr double kSmashKnockbackRadius = 3.5; // SMASH_ATTACK_KNOCKBACK_RADIUS
constexpr double kSmashKnockbackPower = 0.7;  // SMASH_ATTACK_KNOCKBACK_POWER
// MaceItem.knockback's `nearby.push(x, 0.7f, z)`: the vertical half of the push
// is the same 0.7 for every entity the smash reaches, however far away it is.
constexpr double kSmashKnockbackLift = 0.7;

// One swing's inputs. `enchants` is world/enchantment.h's reading of the *held
// weapon* against the victim this swing is aimed at, because two of the three
// values depend on which entity the hit lands on (Smite's tag is the victim's
// own type).
//
// One input of Player.isSweepAttack's is deliberately absent: it also asks that
// the attacker's horizontal movement stay under 2.5x its own movement speed,
// and this engine's server has no client movement to ask (it applies the client's
// positions). What that leaves out is a sweep by an attacker already moving
// faster than it walks — a player on ice or with Speed and no sprint — and
// nothing else, because a sprinting attacker fails the gate earlier.
//
// Player.canCriticalAttack is eight clauses, and every one this engine can
// answer is a field here: `fallDistance > 0.0`, `!onGround()` and
// `!isSprinting()` above, `!isMobilityRestricted()` on `mobility_restricted`, and
// `entity instanceof LivingEntity` — every victim a melee swing reaches is a
// Mob, which is one. The other three cannot fire in play, so they are absent
// rather than dropped: `!onClimbable()` and `!isPassenger()` need climbing and
// riding, which this server has no model of, and `!isInWater()` needs the
// attacker's eye-in-fluid state, which has no fluid model behind it (the
// `eye_in_water` of world/survival.h is a test-only input).
struct MeleeSwing {
  float base_damage = 0.0f;         // Attributes.ATTACK_DAMAGE, effects folded in
  float attack_speed = 4.0f;        // Attributes.ATTACK_SPEED
  int charge_ticks = 0;             // LivingEntity.attackStrengthTicker
  bool sprinting = false;           // Player.canCriticalAttack's `!isSprinting()`
  bool falling = false;             // ...and its `fallDistance > 0.0`
  bool on_ground = true;            // ...and its `!onGround()`
  bool mobility_restricted = false; // ...and its `!isMobilityRestricted()`
  bool sword = false;               // mainhand in #minecraft:swords: the sweep gate
  // mainhand is `minecraft:mace` (MaceItem), the one item whose
  // getAttackDamageBonus is not 0.0f.
  bool mace = false;
  // The attacker's Entity.fallDistance and `isFallFlying()`: the smash's gate is
  // both of these, and its bonus is a function of the first.
  double fall_distance = 0.0;
  bool fall_flying = false;
  MeleeEnchantments enchants{};
};

struct CombatAttack {
  float strength = 0.0f; // attackStrengthScale
  float damage = 0.0f;   // totalDamage, the enchantment's share included
  // Player.attack's `baseDamage` at the moment it calls doSweepAttack: the
  // weapon's damage after the scale and the critical multiplier, and *without*
  // the enchantment's share, which is what the sweep formula multiplies.
  float base_damage = 0.0f;
  float sweep_ratio = 0.0f; // the weapon's Attributes.SWEEPING_DAMAGE_RATIO
  bool fully_charged = false;
  bool critical = false;
  bool sweeping = false;
  // MaceItem.canSmashAttack held for this swing: the damage type is
  // `minecraft:mace_smash`, the 3.5-radius push happens and the attacker's fall
  // distance is reset.
  bool smash = false;
  double knockback = 0.0; // the victim's resistance already applied
};

// Player.baseDamageScaleFactor and getCurrentItemAttackStrengthDelay.
float attack_strength(int ticks, float attack_speed);

// Player.attack.
CombatAttack player_attack(const MeleeSwing& swing);

// MaceItem.canSmashAttack: more than 1.5 blocks fallen, and not an elytra
// flight. It gates the bonus, the damage type, the knockback and the fall
// reset, and it is deliberately *not* the critical's `fallDistance > 0.0` — a
// mace swung a block into a fall does neither.
bool can_smash_attack(double fall_distance, bool fall_flying);

// MaceItem.getAttackDamageBonus' curve with Density folded in the way vanilla
// folds it: `curve(fallDistance) + smash_damage_per_fallen_block * fallDistance`.
// The curve is continuous at both joints (4 x 3 = 12 and 12 + 2 x 5 = 22), so
// the three branches meet rather than step. The caller supplies the gate — this
// is the arithmetic alone, and it is defined for every fall distance.
float mace_smash_bonus(double fall_distance, float smash_damage_per_fallen_block);

// MaceItem.getKnockbackPower for one entity `distance` blocks from the victim:
// `(3.5 - distance) * 0.7 * (fallDistance > 5.0 ? 2 : 1) * (1 - resistance)`,
// never negative (vanilla's `if (knockbackPower > 0.0)`).
//
// The radius is not a separate test: the factor reaches zero exactly at it (and
// `smash_push` is where that does the filtering), so `kSmashKnockbackRadius`
// documents the boundary rather than guarding it. Only the *heavy* smash — a
// fall of more than five blocks — doubles the push.
//
// `resistance` is the nearby entity's KNOCKBACK_RESISTANCE attribute. No mob
// this engine can spawn has one, so it is 0.0 on every live path and the factor
// is 1.0; it is a parameter because the value is the victim's, not a constant.
double smash_knockback_power(double distance, double fall_distance, double resistance);

// MaceItem.knockbackPredicate's clauses that this engine can answer, asked once
// per candidate by both sweeps so that the two populations cannot answer it
// differently: the candidate is not the victim, not the attacker, no spectator,
// and no creative player who is flying. Vanilla's other three clauses have
// nothing to test here — `isAlliedTo` needs teams, the tamable-ownership clause
// needs a tamed animal with an owner, and there are no armor stands — so they are
// named rather than approximated, the way the sweep's own gate names its
// unanswerable clause.
//
// `creative_flying` is `isCreative() && abilities.flying`. The second half is the
// serverbound abilities packet's own bit: the movement packets say where a
// creative player is, not whether it is holding itself there, which is why the
// session mirrors that packet into the registry.
bool smash_knockback_candidate(bool is_victim, bool is_attacker, bool spectator,
                               bool creative_flying);

// MaceItem.knockback's push for one candidate, given the vector from the victim
// to it: the power at that distance along the *unit* direction, with MaceItem's
// flat 0.7 vertical, which is the same 0.7 however far out the candidate is.
// Returns false when the power has reached zero — vanilla's `if (knockbackPower
// > 0.0)` — which is the radius test as well, because the factor is zero at
// exactly 3.5 blocks and negative outside.
//
// The zero-direction case is Vec3.normalize's own (`dist < 1.0e-5` returns
// ZERO): a candidate sitting on the victim is pushed straight up rather than in
// a direction that is pure noise.
bool smash_push(double dx, double dy, double dz, double fall_distance, double resistance,
                double* vx, double* vy, double* vz);

// Player.doSweepAttack's damage for one nearby victim: the flat 1.0 plus
// `sweep_ratio * base_damage`, plus *that victim's* enchantment delta (the sweep
// is enchanted per victim too, so a Smite sword sweeps a zombie harder than it
// sweeps a pig), all scaled by the swing's strength.
float sweeping_damage(float base_damage, float sweep_ratio, float enchant_damage, float strength);

// LivingEntity.getKnockback + Player.attack's sprint bonus + LivingEntity
// .knockback's resistance factor. `enchant_knockback` is the raw
// EnchantmentHelper.modifyKnockback delta; vanilla halves it, because
// ATTACK_KNOCKBACK itself is 0 for a player and getKnockback divides the sum.
double attack_knockback(double sprint_bonus, float enchant_knockback, double resistance);

// Shield blocking is refused only by the damage type's bypass_shield tag; the
// caller supplies that tag result so this module stays independent of registries.
bool shield_blocks_damage(bool blocking, bool bypasses_shield);

} // namespace mc::world
