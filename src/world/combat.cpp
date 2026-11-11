// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/combat.h"

#include <algorithm>
#include <cmath>

namespace mc::world {

float attack_strength(int ticks, float attack_speed) {
  const float delay = 20.0f / std::max(0.1f, attack_speed);
  return std::clamp((static_cast<float>(std::max(0, ticks)) + 0.5f) / delay, 0.0f, 1.0f);
}

CombatAttack player_attack(const MeleeSwing& swing) {
  CombatAttack out;
  out.strength = attack_strength(swing.charge_ticks, swing.attack_speed);
  out.fully_charged = out.strength > kFullChargeScale;
  // Player.canCriticalAttack: a fall the player is still in, not on the ground,
  // not sprinting and not mobility-restricted (Blindness). MeleeSwing names the
  // three clauses of its eight this engine cannot answer.
  out.critical = out.fully_charged && swing.falling && !swing.on_ground && !swing.sprinting &&
                 !swing.mobility_restricted;
  out.sweep_ratio = swing.enchants.sweep_ratio;
  // MaceItem.canSmashAttack: the *weapon's* gate, and only the mace has one. It
  // is read here rather than by the session so that the bonus, the damage type
  // the session raises and the knockback it applies are all one question with
  // one answer.
  out.smash = swing.mace && can_smash_attack(swing.fall_distance, swing.fall_flying);

  // `baseDamage *= baseDamageScaleFactor()`, then Player.attack's
  // `baseDamage += getAttackDamageBonus(...)` (the mace's smash, after the scale
  // and before the critical), then the critical multiplier.
  out.base_damage = std::max(0.0f, swing.base_damage) * (0.2f + out.strength * out.strength * 0.8f);
  if (out.smash)
    out.base_damage += mace_smash_bonus(swing.fall_distance, swing.enchants.smash_damage);
  if (out.critical)
    out.base_damage *= kCriticalMultiplier;

  // `magicBoost = attackStrengthScale * (getEnchantedDamage(...) - baseDamage)`:
  // the enchantment's add is scaled by the charge, and it rides *outside* the
  // critical multiplier that has already been applied to base_damage.
  out.damage = out.base_damage + out.strength * swing.enchants.damage;

  // Player.isSweepAttack: full charge, no critical, and no sprint knockback —
  // a sprinting full-charge swing is the knockback attack instead — on the
  // ground, with a sword in the main hand.
  const bool knockback_attack = swing.sprinting && out.fully_charged;
  out.sweeping =
      out.fully_charged && !out.critical && !knockback_attack && swing.on_ground && swing.sword;
  // No victim this engine can spawn resists a push (Attributes
  // .KNOCKBACK_RESISTANCE sits at vanilla's base 0.0 for every mob it has, and no
  // attribute column carries one), so the factor is 1.0 on every live path.
  out.knockback =
      attack_knockback(knockback_attack ? kSprintKnockback : 0.0, swing.enchants.knockback, 0.0);
  return out;
}

bool can_smash_attack(double fall_distance, bool fall_flying) {
  return fall_distance > kSmashFallThreshold && !fall_flying;
}

float mace_smash_bonus(double fall_distance, float smash_damage_per_fallen_block) {
  const double d = std::max(0.0, fall_distance);
  const double curve = d <= 3.0 ? 4.0 * d : (d <= 8.0 ? 12.0 + 2.0 * (d - 3.0) : 22.0 + d - 8.0);
  return static_cast<float>(curve + static_cast<double>(smash_damage_per_fallen_block) * d);
}

bool smash_knockback_candidate(bool is_victim, bool is_attacker, bool spectator,
                               bool creative_flying) {
  return !is_victim && !is_attacker && !spectator && !creative_flying;
}

bool smash_push(double dx, double dy, double dz, double fall_distance, double resistance,
                double* vx, double* vy, double* vz) {
  const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
  const double power = smash_knockback_power(distance, fall_distance, resistance);
  if (power <= 0.0)
    return false;
  const bool has_direction = distance >= 1.0e-5;
  *vx = has_direction ? (dx / distance) * power : 0.0;
  *vy = kSmashKnockbackLift;
  *vz = has_direction ? (dz / distance) * power : 0.0;
  return true;
}

double smash_knockback_power(double distance, double fall_distance, double resistance) {
  const double power = (kSmashKnockbackRadius - distance) * kSmashKnockbackPower *
                       (fall_distance > kSmashHeavyThreshold ? 2.0 : 1.0) *
                       (1.0 - std::clamp(resistance, 0.0, 1.0));
  return std::max(0.0, power);
}

float sweeping_damage(float base_damage, float sweep_ratio, float enchant_damage, float strength) {
  return (1.0f + sweep_ratio * std::max(0.0f, base_damage) + enchant_damage) * strength;
}

double attack_knockback(double sprint_bonus, float enchant_knockback, double resistance) {
  const double raw = sprint_bonus + static_cast<double>(enchant_knockback) / 2.0;
  return std::max(0.0, raw * (1.0 - std::clamp(resistance, 0.0, 1.0)));
}

bool shield_blocks_damage(bool blocking, bool bypasses_shield) {
  return blocking && !bypasses_shield;
}

} // namespace mc::world
