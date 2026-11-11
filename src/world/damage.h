// SPDX-License-Identifier: GPL-3.0-or-later
//
// Damage: the damage-type registry as the engine reads it, the armour lookup
// and the maths vanilla's damage pipeline runs (Phase 3 milestone F).
//
// The table itself is generated (world/damage_table.h) from
// data/minecraft/damage_type plus the captured wire order, because
// minecraft:damage_type is a *synchronized* registry: the id in a
// damage_event packet is that index and nothing else.
//
// What is modelled, and what is deliberately not:
//   * armour points/toughness come from the item table and the vanilla
//     CombatRules formulas, so a diamond chestplate really does turn 10 damage
//     into 5.6;
//   * the invulnerability window (LivingEntity.hurt's `invulnerableTime > 10`
//     branch, which is why a fast flurry of hits does not stack);
//   * absorption hearts;
//   * enchantment protection and the Resistance effect, both of which feed
//     getDamageAfterMagicAbsorb's magic-armour number (milestone G);
//   * knockback, shields and totems are not modelled.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "world/damage_table.h"
#include "world/item.h"

namespace mc::world {

// --- the registry -----------------------------------------------------------

// nullptr for an unknown or malformed name ("fall" is normalized first).
const GeneratedDamageType* damage_type_by_name(std::string_view name);
// nullptr when the id is out of range.
const GeneratedDamageType* damage_type_by_id(int32_t id);
// Registry index, or -1.
int32_t damage_type_id_of(const GeneratedDamageType& type);

inline bool damage_has_tag(const GeneratedDamageType& type, uint64_t tag) {
  return (type.tags & tag) != 0;
}

// --- sources ----------------------------------------------------------------

// Vanilla's DamageSource, minus the parts this server cannot produce yet. There
// are no mobs and no projectiles, so the only entity that can appear is a
// player; `causing_name` is carried directly rather than looked up so a death
// message still reads correctly after the attacker disconnects.
struct DamageSource {
  const GeneratedDamageType* type = nullptr;
  // The responsible entity (an attacker), or -1 for none.
  int32_t causing_entity_id = -1;
  // The entity that physically dealt the damage (an arrow's shooter is the
  // cause, the arrow the direct entity); -1 for none.
  int32_t direct_entity_id = -1;
  // The causing entity's display name (death message / "slain by" text).
  std::string causing_name;
  // A source position, which vanilla sends instead of entity ids when the
  // damage came from a block (bed respawn, an explosion).
  bool has_position = false;
  double x = 0.0, y = 0.0, z = 0.0;

  bool has_cause() const {
    return causing_entity_id >= 0 || !causing_name.empty();
  }
  bool is(const DamageSource& other) const {
    return type != nullptr && type == other.type;
  }
};

// A source of one registry type, with no attacker.
DamageSource source_of(const GeneratedDamageType& type);

// --- armour -----------------------------------------------------------------

// The two numbers CombatRules.getDamageAfterAbsorb reads off the victim: the
// summed `minecraft:armor` and `minecraft:armor_toughness` attribute values.
struct ArmorValues {
  float points = 0.0f;
  float toughness = 0.0f;
};

// `slots` holds kArmorSlots stacks in *inventory* order (feet, legs, chest,
// head). Empty stacks contribute nothing.
ArmorValues armor_values(const ItemStack* slots, int count);

// One update_attributes modifier. `id` is what lets the client *replace* the
// modifier for a slot rather than stack a new one on top of it, so it has to be
// stable per slot; vanilla takes the id from the item's own component
// ("minecraft:armor.chestplate") and this server derives it from the slot
// ("minecraft:armor.chest"). The client never interprets the string — it is an
// opaque key — so the slot form is equivalent.
struct ArmorModifier {
  std::string id;
  double amount = 0.0;
};

// The armour attribute modifiers the equipped slots contribute, in vanilla's
// shape: each piece is a modifier on `minecraft:armor` (or
// `minecraft:armor_toughness`), which is how the client's armour bar learns the
// number (AttributeInstance.getValue = base + modifiers).
void armor_modifiers(const ItemStack* slots, int count, std::vector<ArmorModifier>* points,
                     std::vector<ArmorModifier>* toughness);

// --- the pipeline -----------------------------------------------------------

// CombatRules.getDamageAfterAbsorb. `toughness` enters as `2 + toughness / 4`,
// which is what makes netherite shrug off big hits that iron does not; the
// armour that counts is never more than 20 and never less than a fifth of the
// raw armour value.
float damage_after_absorb(float damage, float armor, float toughness);

// CombatRules.getDamageAfterMagicAbsorb: the enchantment-protection step on
// its own, `damage * (1 - clamp(protection, 0, 20) / 25)`. A full set of
// Protection IV is 16 points, which is why protection alone caps out at 64%.
float combat_rules_magic_absorb(float damage, float protection);

// getDamageAfterArmorAbsorb: the whole armour step, including the
// BYPASSES_ARMOR gate (`minecraft:fall` and `minecraft:drown` skip it).
float damage_after_armor(const GeneratedDamageType& type, float damage, const ArmorValues& armor);

// LivingEntity.getDamageAfterMagicAbsorb: the *whole* post-armour step, in
// vanilla's order — nothing at all for a BYPASSES_EFFECTS type, then
// Resistance (skipped by BYPASSES_RESISTANCE), then nothing more for a
// BYPASSES_ENCHANTMENTS type, then the enchantment protection through
// combat_rules_magic_absorb. `resistance_multiplier` comes from
// effect.h's resistance_damage_multiplier and is 1.0 with no Resistance
// effect; EnchantmentHelper.getDamageProtection supplies `protection` (see
// world/enchantment.h).
float damage_after_magic_absorb(const GeneratedDamageType& type, float damage, float protection,
                                float resistance_multiplier);

// LivingEntity.calculateFallDamage: floor((fallDistance - safeFallDistance) *
// multiplier * fallDamageMultiplier), never negative. `may_fly` mirrors
// Player.causeFallDamage's `abilities.mayfly` early return (creative flight).
int fall_damage(double fall_distance, double safe_fall_distance, double fall_damage_multiplier,
                bool may_fly);

// DamageScaling: whether the difficulty multiplier applies. Vanilla scales by
// difficulty only for WHEN_CAUSED_BY_LIVING_NON_PLAYER (and, for ALWAYS, for
// all sources); with no difficulty model this server uses the *normal*
// factors, where both are 1.0, so this is a named no-op kept for the shape of
// the pipeline rather than a hidden constant.
inline float scale_damage(float damage, uint8_t /*scaling*/, bool /*is_player_attack*/) {
  return damage;
}

// The lang key + arguments of the death message, as
// CombatTracker.getDeathMessage / DamageSource.getLocalizedDeathMessage build
// it: `translate` plus the 1-3 components in `with`.
struct DeathMessage {
  std::string key;      // "death.attack.fall"
  std::string victim;   // the dying entity's display name
  std::string attacker; // the causing entity's name, when there is one
  bool has_attacker = false;
};

// `kill_credit` is `LivingEntity.getKillCredit()`: the name of whatever last hurt
// the victim (within its 100-tick window), which the message falls back on when
// the killing blow itself had no attacker — a fall right after a mob hit, say.
// Empty means there is none.
//
// The distinction matters: vanilla appends `.player` only in that *no cause*
// branch, so a zombie's killing blow is the bare `death.attack.mob` with
// `[victim, "Zombie"]`, and `death.attack.mob.player` does not exist as a lang
// key at all (DamageSource.getLocalizedDeathMessage).
DeathMessage death_message_for(const DamageSource& source, std::string_view victim_name,
                               std::string_view kill_credit = {});

} // namespace mc::world
