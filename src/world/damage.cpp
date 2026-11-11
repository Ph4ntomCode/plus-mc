// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/damage.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "world/item_registry.h"

namespace mc::world {

namespace {

// The registry name -> table index, built once. 51 entries, so a map is not
// worth it; the scan below is only on the `/damage <type>` path.
const GeneratedDamageType* find_by_name(std::string_view normalized) {
  for (int i = 0; i < kDamageTypeCount; ++i)
    if (normalized == kDamageTypes[i].name)
      return &kDamageTypes[i];
  return nullptr;
}

} // namespace

const GeneratedDamageType* damage_type_by_name(std::string_view name) {
  const std::string normalized = normalize_item_name(name);
  if (normalized.empty())
    return nullptr;
  return find_by_name(normalized);
}

const GeneratedDamageType* damage_type_by_id(int32_t id) {
  if (id < 0 || id >= kDamageTypeCount)
    return nullptr;
  return &kDamageTypes[id];
}

int32_t damage_type_id_of(const GeneratedDamageType& type) {
  const auto* base = kDamageTypes;
  const auto index = &type - base;
  if (index < 0 || index >= kDamageTypeCount)
    return -1;
  return static_cast<int32_t>(index);
}

DamageSource source_of(const GeneratedDamageType& type) {
  DamageSource source;
  source.type = &type;
  return source;
}

ArmorValues armor_values(const ItemStack* slots, int count) {
  ArmorValues out;
  if (!slots)
    return out;
  const ItemRegistry& items = ItemRegistry::instance();
  for (int i = 0; i < count; ++i) {
    if (slots[i].empty())
      continue;
    const GeneratedItem* item = items.by_id(slots[i].item);
    if (!item)
      continue;
    out.points += static_cast<float>(item->armor);
    out.toughness += static_cast<float>(item->armor_toughness);
  }
  return out;
}

void armor_modifiers(const ItemStack* slots, int count, std::vector<ArmorModifier>* points,
                     std::vector<ArmorModifier>* toughness) {
  // Inventory order, which is also the order the client's armour bar draws in.
  static constexpr const char* kSlotNames[] = {"feet", "legs", "chest", "head"};
  const ItemRegistry& items = ItemRegistry::instance();
  for (int i = 0; i < count && i < 4; ++i) {
    if (slots[i].empty())
      continue;
    const GeneratedItem* item = items.by_id(slots[i].item);
    if (!item)
      continue;
    if (item->armor > 0 && points)
      points->push_back(
          {std::string("minecraft:armor.") + kSlotNames[i], static_cast<double>(item->armor)});
    if (item->armor_toughness > 0 && toughness)
      toughness->push_back({std::string("minecraft:armor_toughness.") + kSlotNames[i],
                            static_cast<double>(item->armor_toughness)});
  }
}

float damage_after_absorb(float damage, float armor, float toughness) {
  // CombatRules.getDamageAfterAbsorb: `toughness` is 2 + armorToughness / 4,
  // the effective armour is clamped to [armor * 0.2, 20], and the fraction is
  // that over 25. The weapon's armor-effectiveness modifier
  // (EnchantmentHelper.modifyArmorEffectiveness) is 1 without enchants.
  const float toughness_term = 2.0f + toughness / 4.0f;
  const float real_armor = std::clamp(armor - damage / toughness_term, armor * 0.2f, 20.0f);
  const float fraction = real_armor / 25.0f;
  return damage * (1.0f - fraction);
}

float combat_rules_magic_absorb(float damage, float protection) {
  const float real_armor = std::clamp(protection, 0.0f, 20.0f);
  return damage * (1.0f - real_armor / 25.0f);
}

float damage_after_armor(const GeneratedDamageType& type, float damage, const ArmorValues& armor) {
  if (damage_has_tag(type, kDamageTagBypassesArmor))
    return damage;
  return damage_after_absorb(damage, armor.points, armor.toughness);
}

float damage_after_magic_absorb(const GeneratedDamageType& type, float damage, float protection,
                                float resistance_multiplier) {
  // LivingEntity.getDamageAfterMagicAbsorb, gate by gate. The order is what
  // matters: Resistance is an *effect*, so a type that bypasses effects skips
  // it as well as the enchantments, and a type that bypasses enchantments still
  // gets to resist.
  if (damage_has_tag(type, kDamageTagBypassesEffects))
    return damage;
  if (resistance_multiplier != 1.0f && !damage_has_tag(type, kDamageTagBypassesResistance))
    damage = std::max(damage * resistance_multiplier, 0.0f);
  if (damage <= 0.0f)
    return 0.0f;
  if (damage_has_tag(type, kDamageTagBypassesEnchantments))
    return damage;
  // `if (enchantmentArmor > 0.0f)`: a negative total (which no enchantment in
  // the data produces) is left alone rather than clamping the damage upward.
  if (protection > 0.0f)
    return combat_rules_magic_absorb(damage, protection);
  return damage;
}

int fall_damage(double fall_distance, double safe_fall_distance, double fall_damage_multiplier,
                bool may_fly) {
  // Player.causeFallDamage returns false (no damage at all) for a flying
  // creative player; LivingEntity.calculateFallPower is `fallDistance + 1e-6 -
  // safeFallDistance` (safe fall distance is 3 for everything).
  if (may_fly)
    return 0;
  const double power = fall_distance + 1.0e-6 - safe_fall_distance;
  if (power <= 0.0)
    return 0;
  return static_cast<int>(std::floor(power * fall_damage_multiplier));
}

DeathMessage death_message_for(const DamageSource& source, std::string_view victim_name,
                               std::string_view kill_credit) {
  DeathMessage message;
  message.victim.assign(victim_name);
  if (!source.type)
    return message;
  const std::string base = std::string("death.attack.") + source.type->message_id;

  // CombatTracker.getDeathMessage. The fall variants need the block that broke
  // the fall (ladder, vines, scaffolding, ...) and the point the player last
  // ascended from; without block tracking the honest generic form is the one
  // vanilla itself uses for a plain fall.
  if (source.type->death_message_type == kDeathMessageFallVariants) {
    if (!source.has_cause()) {
      message.key = "death.fell.accident.generic";
      return message;
    }
    message.key = "death.fell.finish";
    message.attacker = source.causing_name;
    message.has_attacker = true;
    return message;
  }
  if (source.type->death_message_type == kDeathMessageIntentionalGameDesign) {
    message.key = base + ".intentional_game_design";
    return message;
  }
  if (!source.has_cause()) {
    // DamageSource.getLocalizedDeathMessage's "no cause" branch: the victim's
    // kill credit, if the server has one, names the attacker in a `.player`
    // variant ("death.attack.fall.player" — "<victim> fell to their death while
    // fighting <attacker>"); with no credit the bare key carries only the victim.
    if (!kill_credit.empty()) {
      message.key = base + ".player";
      message.attacker.assign(kill_credit);
      message.has_attacker = true;
    } else {
      message.key = base;
    }
    return message;
  }
  // A cause entity: the *bare* key with the attacker as the second argument,
  // mobs and players alike. Vanilla only reaches for another suffix here when
  // the attacker's held item has a custom name (`.item`, naming the weapon),
  // which this server does not model — items here carry no custom names.
  message.key = base;
  message.attacker = source.causing_name;
  message.has_attacker = true;
  return message;
}

} // namespace mc::world
