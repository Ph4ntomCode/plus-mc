// SPDX-License-Identifier: GPL-3.0-or-later
//
// Survival: the player half of the living-entity state — health, absorption,
// the food data, the air supply, the fall distance and the damage cooldown —
// plus the vanilla steps that advance it (Phase 3 milestone F).
//
// Everything here is a pure function over SurvivalState so the arithmetic can
// be unit-tested without a session, a world or a client: the session owns the
// state and does the packet work, this module decides what the numbers become.
// Each function names the vanilla method it mirrors, and the constants above
// them are vanilla's own (FoodData, LivingEntity, CombatRules, Entity).
//
// Deliberately out of scope, and why:
//   * totems and freezing; shields are represented by the combat-state gate and
//     damage tag, while orientation and durability remain follow-up work;
//   * mob effects on anything but a player: the ActiveEffects container below is
//     the player's, and this server's mobs have their own damage model.
//
// Armour durability used to be on that list and is not any more: the rule is
// `armor_durability_damage` below, and the session applies it to the worn pieces
// (`wear_armor_on_hit` in server/session.cpp).
//
// Fire *is* modelled now, but not here: `SurvivalState::burning` is the
// countdown (world/fire.h) and the session drives it, because the tick that
// ignites an entity is the block it is standing in.
#pragma once

#include <cstdint>

#include "world/damage.h"
#include "world/damage_table.h"
#include "world/effect.h"
#include "world/enchantment.h"
#include "world/fire.h"

namespace mc::world {

// LivingEntity.getMaxAirSupply: 15 seconds of air, drained one tick at a time.
constexpr int kMaxAirSupply = 300;
// Player's max_health without modifiers, and the food data's starting values
// (FoodData: foodLevel 20, saturationLevel 5, exhaustion 0).
constexpr float kDefaultMaxHealth = 20.0f;
constexpr int kDefaultFoodLevel = 20;
constexpr float kDefaultSaturation = 5.0f;

// net.minecraft.world.Difficulty ordinals (what server.properties and the
// change_difficulty packet use).
enum Difficulty : uint8_t {
  kDifficultyPeaceful = 0,
  kDifficultyEasy = 1,
  kDifficultyNormal = 2,
  kDifficultyHard = 3,
};

// The survival half of a player, as one struct. Field defaults are the values a
// fresh ServerPlayer has.
struct SurvivalState {
  float health = kDefaultMaxHealth;
  float absorption = 0.0f;
  int food = kDefaultFoodLevel;
  float saturation = kDefaultSaturation;
  float exhaustion = 0.0f;
  int food_tick_timer = 0;
  int air = kMaxAirSupply;
  double fall_distance = 0.0;
  float invulnerable_time = 0.0f;
  float last_hurt = 0.0f;
  bool dead = false;
  // Entity.remainingFireTicks, owed to a different method than everything else
  // here (Entity.baseTick rather than LivingEntity), and started at a player's
  // idle depth of -20 rather than at zero.
  Burning burning = player_burning();
  // LivingEntity.activeEffects. The status effects on this player, which the
  // survival tick advances and the damage pipeline reads (Resistance). Reset
  // with the rest of the struct on respawn, which is what
  // `Player.restoreFrom(old, false)` does to a dead player's effects.
  ActiveEffects effects;

  // ServerPlayer's field defaults, which are deliberately absurd sentinels so
  // the first comparison always fails and a fresh join always sends one packet
  // (ServerPlayer: lastSentHealth = -1.0E8f, lastSentFood = -99999999,
  // lastFoodSaturationZero = true).
  float sent_health = -1.0e8f;
  int sent_food = -99999999;
  bool sent_saturation_zero = true;

  bool is_alive() const {
    return health > 0.0f;
  }
  bool is_hurt() const {
    return health < kDefaultMaxHealth;
  }

  // ServerPlayer.doTick's resume condition: the packet goes out when health or
  // food moved, or when saturation crossed zero in either direction. Saturation
  // itself is *not* compared — the client only draws whole haunches.
  bool health_packet_due() const {
    return health != sent_health || food != sent_food ||
           (saturation == 0.0f) != sent_saturation_zero;
  }
  void mark_health_sent() {
    sent_health = health;
    sent_food = food;
    sent_saturation_zero = saturation == 0.0f;
  }
};

// What the world knows about the player this tick. Everything the survival
// steps need from outside, so they stay pure.
struct SurvivalContext {
  float max_health = kDefaultMaxHealth;
  uint8_t difficulty = kDifficultyNormal;
  // Whether the player's eyes are in water (`isEyeInFluid(WATER)`). There is no
  // fluid model yet, so this is false in play and only unit tests set it.
  bool eye_in_water = false;
  // Respiration/water breathing would make this true.
  bool can_breathe_underwater = false;
  // Player.abilities.invulnerable (creative/spectator): no damage at all, and
  // `food` never drains.
  bool invulnerable = false;
  // Player.abilities.mayfly, which Player.causeFallDamage checks to skip fall
  // damage entirely.
  bool may_fly = false;
  // Attributes: safe_fall_distance (3.0) and fall_damage_multiplier (1.0).
  double safe_fall_distance = 3.0;
  double fall_damage_multiplier = 1.0;
  // oxygen_bonus; > 0 makes the air supply drain probabilistically.
  double oxygen_bonus = 0.0;
  ArmorValues armor{};
  // EnchantmentHelper.getDamageProtection's input: the equipment slots that can
  // carry an enchantment. It is a *per-hit* input rather than a per-tick one,
  // because the total depends on the damage *type* — Feather Falling protects
  // against a fall and Blast Protection against an explosion — so the sum is
  // taken inside apply_damage, where the type is known.
  EquipmentEntry equipment[6];
  int equipment_count = 0;
  // MobEffectUtil / Resistance: 1.0 with no Resistance effect. A per-hit input
  // for the same reason (the type's own tags decide whether it applies).
  float resistance_multiplier = 1.0f;
  // Whether the player is actively using a shield. The session owns the
  // hand/use packet state; damage types decide whether the shield is bypassed.
  bool shield_blocking = false;
  // Attributes whose value the effects change and the survival layer reads:
  // Health Boost's MAX_HEALTH, Absorption's MAX_ABSORPTION (which is the clamp
  // `LivingEntity.setAbsorptionAmount` applies — with a base of 0, absorption
  // hearts cannot exist at all without the effect's modifier), and Jump Boost's
  // SAFE_FALL_DISTANCE.
  float max_absorption = 0.0f;
};

// One tick's worth of decisions, so the session only has to raise packets for
// what actually happened.
struct SurvivalTick {
  bool starve_due = false; // apply minecraft:starve for 1.0
  bool drown_due = false;  // apply minecraft:drown for 2.0
  bool changed = false;    // health/food/saturation moved
  bool healed = false;
};

// The outcome of one damage application: LivingEntity.hurt + Player.hurtServer
// + actuallyHurt, in that order.
struct DamageResult {
  bool accepted = false; // LivingEntity.hurt's return value
  float applied = 0.0f;  // health actually lost
  float absorbed = 0.0f; // damage eaten by absorption hearts
  bool died = false;     // health reached zero on this hit
  bool reduced = false;  // the invulnerability window clipped the hit
  // The damage that landed *before* the armour step: the whole hit, or what was
  // left of it after the invulnerability window's subtraction. It is the number
  // `LivingEntity.getDamageAfterArmorAbsorb` hands `hurtArmor`, i.e. the one the
  // worn pieces lose durability on, so the session reads it rather than the
  // argument it passed in.
  float landed = 0.0f;
};

// Player.hurtServer + LivingEntity.hurt + actuallyHurt. `damage` is the raw
// amount (`/damage`'s argument, the fall damage, ...). Returns whether the hit
// landed, and mutates health/absorption/the cooldown window.
DamageResult apply_damage(SurvivalState& state, const DamageSource& source, float damage,
                          const SurvivalContext& ctx);

// `LivingEntity.doHurtEquipment`'s per-piece amount, which is what
// `Player.hurtArmor` (the four armour slots) and `Player.hurtHelmet` (the head
// alone) charge: `(int)Math.max(1.0f, damage / 4.0f)`, and 0 for a hit that is
// not positive — the method's own early return, which is why a fully shielded
// hit wears nothing.
//
// It is deliberately not the damage that reached the health bar: armour wears on
// the hit *before* the armour step, so a chestplate that absorbed everything
// still takes its point. The wear falls off quickly — one point for up to four
// damage, two for five to eight, five for the 20.0 of a point-blank TNT — and
// every worn piece that answers vanilla's three questions takes the same amount.
// Those three questions are the session's, because they are item data:
// `isDamageableItem()`, the equippable component's `damageOnHurt()` (the pack
// clears it on the elytra and the mount armours) and `canBeHurtBy(source)`
// (netherite's fire resistance). This engine also leaves out
// `EnchantmentHelper.processDurabilityChange` — Unbreaking's save — exactly as
// the tool path already does.
//
// The head-only `hurtHelmet` half of the same method is not driven from here: it
// runs for the three types in `#minecraft:damages_helmet`
// (`minecraft:falling_anvil`, `falling_block`, `falling_stalactite`), and this
// engine applies none of them — there is no falling-block entity, so nothing can
// reach the branch.
int armor_durability_damage(float damage);

// FoodData.tick. Mutates the food data (and health, through the natural
// regeneration that *heals*), and reports the starvation damage the caller must
// route through apply_damage.
SurvivalTick tick_food(SurvivalState& state, const SurvivalContext& ctx);

// LivingEntity.baseTick's air block. `oxygen_roll` is one `nextDouble()` call
// (0..1): with an oxygen bonus there is a chance the tick does not consume air.
// Returns true when the drowning damage (2.0, minecraft:drown) is due.
bool tick_air(SurvivalState& state, const SurvivalContext& ctx, double oxygen_roll);

// Entity.checkFallDamage: a downward move adds to the fall distance, landing
// spends it. Returns the distance the landing spent (0 when nothing was), which
// the caller feeds to fall_damage().
double spend_fall_distance(SurvivalState& state, double dy, bool on_ground, bool in_water);

// Player.causeFoodExhaustion, minus the caller's `abilities.invulnerable` gate.
void add_exhaustion(SurvivalState& state, float amount);

// Player.canEat: `abilities.invulnerable || canAlwaysEat || foodData.needsFood()`.
bool can_eat(const SurvivalState& state, bool can_always_eat, bool invulnerable);

// FoodData.eat(food, saturation).
void eat(SurvivalState& state, int nutrition, float saturation);

// LivingEntity.heal.
void heal(SurvivalState& state, float amount, float max_health);

} // namespace mc::world
