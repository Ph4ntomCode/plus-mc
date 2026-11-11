// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/survival.h"

#include "world/combat.h"

#include <algorithm>

namespace mc::world {

namespace {

// FoodData.tick's constants.
constexpr float kExhaustionPerFoodPoint = 4.0f;
constexpr float kMaxExhaustion = 40.0f;
constexpr int kSaturationRegenTicks = 10;
constexpr float kSaturationRegenCap = 6.0f;
constexpr int kRegenTicks = 80;
constexpr int kStarveTicks = 80;

// LivingEntity.shouldTakeDrowningDamage: air has to run this far into the
// negative before the lungs give out, which is what makes the 15 s of air a
// full 15 s rather than 14.
constexpr int kAirSupplyDamageThreshold = -20;

} // namespace

void heal(SurvivalState& state, float amount, float max_health) {
  // LivingEntity.heal: `if (getHealth() > 0)` then setHealth, which itself
  // clamps to [0, maxHealth]. A negative amount is *not* damage — nothing in
  // vanilla calls heal with one.
  if (!state.is_alive())
    return;
  state.health = std::min(std::max(state.health + amount, 0.0f), max_health);
}

void add_exhaustion(SurvivalState& state, float amount) {
  state.exhaustion = std::min(state.exhaustion + amount, kMaxExhaustion);
}

int armor_durability_damage(float damage) {
  // `(int)Math.max(1.0f, damage / 4.0f)`: the `damage <= 0` case is the method's
  // own early return, the max is what floors a light hit at one point, and the
  // cast is Java's float -> int conversion, which *saturates* at Integer.MAX_VALUE
  // (a `/damage` of 3.4e38 is reachable, and a C++ cast of that float would be
  // undefined behaviour rather than a big number).
  if (!(damage > 0.0f))
    return 0;
  const double quarter = static_cast<double>(damage) / 4.0;
  if (quarter <= 1.0)
    return 1;
  return quarter >= 2147483647.0 ? 2147483647 : static_cast<int>(quarter);
}

bool can_eat(const SurvivalState& state, bool can_always_eat, bool invulnerable) {
  return invulnerable || can_always_eat || state.food < kDefaultFoodLevel;
}

void eat(SurvivalState& state, int nutrition, float saturation) {
  // FoodData.add.
  state.food = std::clamp(state.food + nutrition, 0, kDefaultFoodLevel);
  state.saturation =
      std::clamp(state.saturation + saturation, 0.0f, static_cast<float>(state.food));
}

SurvivalTick tick_food(SurvivalState& state, const SurvivalContext& ctx) {
  SurvivalTick out;
  if (!state.is_alive())
    return out;
  const float health_before = state.health;
  const int food_before = state.food;
  const float saturation_before = state.saturation;

  // An exhaustion point that has piled past 4 is spent here: saturation first,
  // then the food level (unless the world is peaceful — that is the whole
  // point of peaceful).
  if (state.exhaustion > kExhaustionPerFoodPoint) {
    state.exhaustion -= kExhaustionPerFoodPoint;
    if (state.saturation > 0.0f)
      state.saturation = std::max(state.saturation - 1.0f, 0.0f);
    else if (ctx.difficulty != kDifficultyPeaceful)
      state.food = std::max(state.food - 1, 0);
  }

  const bool hurt = state.health < ctx.max_health;
  if (state.saturation > 0.0f && hurt && state.food >= kDefaultFoodLevel) {
    // The fast bar: a full food level spends saturation to heal every 10 ticks.
    if (++state.food_tick_timer >= kSaturationRegenTicks) {
      const float spent = std::min(state.saturation, kSaturationRegenCap);
      heal(state, spent / kSaturationRegenCap, ctx.max_health);
      add_exhaustion(state, spent);
      state.food_tick_timer = 0;
      out.healed = true;
    }
  } else if (state.food >= 18 && hurt) {
    // The slow bar: one heart every 4 s for six exhaustion points.
    if (++state.food_tick_timer >= kRegenTicks) {
      heal(state, 1.0f, ctx.max_health);
      add_exhaustion(state, 6.0f);
      state.food_tick_timer = 0;
      out.healed = true;
    }
  } else if (state.food <= 0) {
    if (++state.food_tick_timer >= kStarveTicks) {
      // FoodData.tick's difficulty gate: on normal difficulty starvation only
      // bites above half a heart, on easy above five hearts, on hard always.
      if (state.health > 10.0f || ctx.difficulty == kDifficultyHard ||
          (state.health > 1.0f && ctx.difficulty == kDifficultyNormal))
        out.starve_due = true;
      state.food_tick_timer = 0;
    }
  } else {
    state.food_tick_timer = 0;
  }

  out.changed = state.health != health_before || state.food != food_before ||
                state.saturation != saturation_before;
  return out;
}

bool tick_air(SurvivalState& state, const SurvivalContext& ctx, double oxygen_roll) {
  if (!state.is_alive())
    return false;
  if (ctx.eye_in_water) {
    // canDrownInWater, minus the water-breathing effect (milestone G) and with
    // the creative case folded into `invulnerable`.
    const bool can_drown = !ctx.can_breathe_underwater && !ctx.invulnerable;
    // Note the asymmetry in LivingEntity.baseTick: under water the supply only
    // ever drains. The refill branch is gated on
    // MobEffectUtil.shouldEffectsRefillAirsupply, so a water-breathing player
    // (or a creative one) *holds* their air rather than refilling it, and the
    // +4 a tick below is for a head above the surface.
    if (!can_drown)
      return false;
    // decreaseAirSupply: oxygen_bonus is a chance to keep the air this tick.
    if (ctx.oxygen_bonus <= 0.0 || oxygen_roll < 1.0 / (ctx.oxygen_bonus + 1.0))
      --state.air;
    if (state.air <= kAirSupplyDamageThreshold) {
      state.air = 0;
      return true;
    }
    return false;
  }
  // increaseAirSupply: +4 a tick out of the water (300 ticks back from empty).
  if (state.air < kMaxAirSupply)
    state.air = std::min(state.air + 4, kMaxAirSupply);
  return false;
}

double spend_fall_distance(SurvivalState& state, double dy, bool on_ground, bool in_water) {
  // Entity.checkFallDamage: falling accumulates, landing spends. The caller
  // applies the damage with the returned distance before the next move.
  if (!in_water && dy < 0.0)
    state.fall_distance -= dy;
  if (!on_ground)
    return 0.0;
  const double spent = state.fall_distance;
  state.fall_distance = 0.0;
  return spent;
}

DamageResult apply_damage(SurvivalState& state, const DamageSource& source, float damage,
                          const SurvivalContext& ctx) {
  DamageResult out;
  if (!source.type || !state.is_alive() || damage <= 0.0f)
    return out;
  const GeneratedDamageType& type = *source.type;

  // Player.hurtServer / LivingEntity.hurtServer's gates. `invulnerable` is the
  // creative/spectator flag; only the types that bypass invulnerability
  // (minecraft:out_of_world, minecraft:generic_kill) get through it, which is
  // exactly why /kill uses generic_kill rather than a big number.
  if (ctx.invulnerable && !damage_has_tag(type, kDamageTagBypassesInvulnerability))
    return out;
  // A spectating player is invulnerable too; that is the same flag.
  if (shield_blocks_damage(ctx.shield_blocking, damage_has_tag(type, kDamageTagBypassesShield))) {
    out.accepted = true;
    return out;
  }

  // The invulnerability window (LivingEntity.hurt). The BYPASSES_COOLDOWN tag
  // is empty in vanilla, so a second hit inside the first ten ticks only lands
  // for the part that exceeds the previous hit's raw damage.
  float landed = damage;
  if (state.invulnerable_time > 10.0f) {
    if (landed <= state.last_hurt)
      return out;
    landed -= state.last_hurt;
    state.last_hurt = damage;
    out.reduced = true;
  } else {
    state.last_hurt = damage;
    state.invulnerable_time = 20.0f;
  }
  // The number the armour step sees — and, through `getDamageAfterArmorAbsorb`,
  // the one the worn pieces lose durability on.
  out.landed = landed;

  // actuallyHurt: armour, then effects and enchantment protection, then
  // absorption. The protection total is summed here rather than taken from the
  // context because it depends on the damage type.
  const float after_armor = damage_after_armor(type, landed, ctx.armor);
  const float protection = damage_protection(ctx.equipment, ctx.equipment_count, type);
  const float after_magic =
      damage_after_magic_absorb(type, after_armor, protection, ctx.resistance_multiplier);
  const float to_health = std::max(after_magic - state.absorption, 0.0f);
  out.absorbed = after_magic - to_health;
  // LivingEntity.setAbsorptionAmount clamps to [0, getMaxAbsorption()], not to
  // zero: with the base max_absorption of 0 nothing can be absorbed at all,
  // which is exactly why the Absorption effect's MAX_ABSORPTION modifier is what
  // makes absorption hearts exist.
  state.absorption = std::min(std::max(state.absorption - after_magic, 0.0f), ctx.max_absorption);
  out.accepted = true;
  if (to_health <= 0.0f)
    return out;

  // Player.actuallyHurt charges the food bar for the damage that lands.
  add_exhaustion(state, type.exhaustion);
  state.health = std::max(state.health - to_health, 0.0f);
  out.applied = to_health;
  if (!state.is_alive()) {
    state.health = 0.0f;
    state.dead = true;
    out.died = true;
  }
  return out;
}

} // namespace mc::world
