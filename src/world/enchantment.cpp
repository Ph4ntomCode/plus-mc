// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/enchantment.h"

#include <algorithm>
#include <cmath>

#include "world/entity_type_tags.h"

namespace mc::world {

namespace {

bool contains_id(const uint32_t* ids, int count, int32_t value) {
  if (!ids || count <= 0 || value < 0)
    return false;
  const uint32_t want = static_cast<uint32_t>(value);
  return std::binary_search(ids, ids + count, want);
}

// Enchantment.applyEffects' LevelBasedValue.linear: `base + per_level_above_first
// * (level - 1)`, so Smite V is 2.5 + 2.5 * 4 = 12.5 and not 2.5 * 5.
float linear_value(const GeneratedLinear& value, int level) {
  return value.base + value.per_level_above_first * static_cast<float>(level - 1);
}

// A `minecraft:damage`/`minecraft:knockback` entry's requirement: the tag has to
// name whichever entity the data tests, and an entry with no requirement (or one
// this engine has no tag for) applies unconditionally. Only a requirement that
// names a tag this build does not carry is refused *by name*, which the generator
// cannot produce — it fails the build instead.
bool requirement_met(const char* tag, uint8_t which, std::string_view victim_type,
                     std::string_view attacker_type) {
  if (tag == nullptr || which == kDamageTagEntityNone)
    return true;
  return entity_type_has_tag(tag, which == kDamageTagEntityVictim ? victim_type : attacker_type);
}

// EnchantmentHelper.doPostAttackEffects' entry list, resolved for the level the
// attacker's item carries: ReqEval.applyEffects walks the `minecraft:post_attack`
// entries, each gated by its own requirements, and hands every one of them the
// item's level. The requirement is the same entity-properties shape the damage
// hooks use (`this` is the victim), and a melee swing always satisfies the
// data's `is_direct` clause, so it is the tag that decides.
void add_post_attack(MeleeEnchantments& out, const GeneratedEnchantment& enchantment, int level,
                     std::string_view victim_type, std::string_view attacker_type) {
  for (int i = 0; i < enchantment.post_attack_count; ++i) {
    const GeneratedPostAttack& entry = enchantment.post_attack[i];
    if (!requirement_met(entry.tag, entry.tag_entity, victim_type, attacker_type))
      continue;
    if (out.post_attack_count >= kMaxPostAttack)
      continue;
    PostAttackEffect& slot = out.post_attack[out.post_attack_count++];
    slot.kind = entry.kind;
    slot.effect = entry.effect;
    slot.min_ticks = static_cast<int>(std::floor(linear_value(entry.duration_min, level) * 20.0f));
    slot.max_ticks = static_cast<int>(std::floor(linear_value(entry.duration_max, level) * 20.0f));
    slot.min_amplifier = static_cast<int>(std::floor(linear_value(entry.amplifier_min, level)));
    slot.max_amplifier = static_cast<int>(std::floor(linear_value(entry.amplifier_max, level)));
  }
}

// Sweeping Edge's `minecraft:fraction` amount: numerator/denominator, each linear
// in the level, which is why level L is L / (L + 1) rather than a table. An
// enchantment that carries minecraft:attributes for something else (Depth
// Strider's movement efficiency, Soul Speed's speed) has an all-zero fraction
// here and contributes nothing.
float sweeping_ratio(const GeneratedEnchantment& enchantment, int level) {
  const float denominator = linear_value(enchantment.sweeping_den, level);
  if (denominator <= 0.0f)
    return 0.0f;
  return linear_value(enchantment.sweeping_num, level) / denominator;
}

} // namespace

const GeneratedEnchantment* enchantment_by_name(std::string_view name) {
  const std::string normalized = normalize_item_name(name);
  if (normalized.empty())
    return nullptr;
  for (int i = 0; i < kEnchantmentCount; ++i)
    if (normalized == kEnchantments[i].name)
      return &kEnchantments[i];
  return nullptr;
}

const GeneratedEnchantment* enchantment_by_id(int32_t id) {
  if (id < 0 || id >= kEnchantmentCount)
    return nullptr;
  return &kEnchantments[id];
}

int32_t enchantment_id_of(const GeneratedEnchantment& enchantment) {
  const auto index = &enchantment - kEnchantments;
  if (index < 0 || index >= kEnchantmentCount)
    return -1;
  return static_cast<int32_t>(index);
}

bool enchantment_supports_item(const GeneratedEnchantment& enchantment, int32_t item_id) {
  // `supportedItems().contains(...)` over a tag of items; the generator resolved
  // the tag to ascending item ids.
  return contains_id(enchantment.items, enchantment.item_count, item_id);
}

bool enchantment_applies_to_slot(const GeneratedEnchantment& enchantment, uint8_t slot) {
  return (enchantment.slots & slot) != 0;
}

bool enchantment_compatible_with(const ItemStack& stack, const GeneratedEnchantment& candidate) {
  const int32_t candidate_id = enchantment_id_of(candidate);
  for (const ItemEnchantment& entry : stack.enchantments) {
    const GeneratedEnchantment* existing = enchantment_by_id(entry.enchantment);
    if (!existing)
      continue;
    // Enchantment.areCompatible: `!a.equals(b) && !a.exclusiveSet.contains(b) &&
    // !b.exclusiveSet.contains(a)`.
    if (entry.enchantment == candidate_id)
      return false;
    if (contains_id(existing->exclusive_set, existing->exclusive_count, candidate_id))
      return false;
    if (contains_id(candidate.exclusive_set, candidate.exclusive_count, entry.enchantment))
      return false;
  }
  return true;
}

float item_damage_protection(const ItemStack& stack, uint8_t slot,
                             const GeneratedDamageType& type) {
  float total = 0.0f;
  for (const ItemEnchantment& entry : stack.enchantments) {
    const GeneratedEnchantment* def = enchantment_by_id(entry.enchantment);
    if (!def || !enchantment_applies_to_slot(*def, slot))
      continue;
    for (int i = 0; i < def->protection_count; ++i) {
      const GeneratedProtection& protection = def->protections[i];
      // Enchantment.damageContext + the ConditionalEffect's
      // damage_source_properties predicate: every `expected: true` tag has to be
      // set on the damage type and every `expected: false` one has to be clear.
      // That single pair is what separates Protection (excludes
      // bypasses_invulnerability and nothing else) from Blast Protection (must
      // be is_explosion as well).
      if ((type.tags & protection.required_tags) != protection.required_tags)
        continue;
      if ((type.tags & protection.excluded_tags) != 0)
        continue;
      // Enchantment.applyEffects with AddValue:
      // `value + LevelBasedValue.linear(base, per_level_above_first).calculate(level)`,
      // and a linear value is `base + per_level_above_first * (level - 1)`. So
      // Blast Protection IV is 2 + 2 * 3 = 8, not 2 * 4.
      total +=
          protection.base + protection.per_level_above_first * static_cast<float>(entry.level - 1);
    }
  }
  return total;
}

float damage_protection(const EquipmentEntry* equipment, int count,
                        const GeneratedDamageType& type) {
  float total = 0.0f;
  for (int i = 0; i < count; ++i) {
    if (equipment[i].stack.empty())
      continue;
    total += item_damage_protection(equipment[i].stack, equipment[i].slot, type);
  }
  return total;
}

int victim_post_attack_effects(const EquipmentEntry* equipment, int count, ThornsEffect* out,
                               int max_out) {
  int written = 0;
  // EnchantmentHelper.runIterationOnEquipment walks EquipmentSlot.VALUES in slot
  // order; the caller's list is the same slots in the same order, so the entries
  // land in the order a full set of Thorns IV pieces would fire in.
  for (int i = 0; i < count && written < max_out; ++i) {
    const EquipmentEntry& piece = equipment[i];
    if (piece.stack.empty())
      continue;
    for (const ItemEnchantment& entry : piece.stack.enchantments) {
      const GeneratedEnchantment* def = enchantment_by_id(entry.enchantment);
      if (!def || def->thorns_count <= 0)
        continue;
      // `slots` is the piece's own slot, the same test the protection hook
      // applies (a Thorns book in a shulker box is not worn, so it does not
      // reach here at all — the caller passes the *equipped* list).
      if (!enchantment_applies_to_slot(*def, piece.slot))
        continue;
      for (int t = 0; t < def->thorns_count && written < max_out; ++t) {
        const GeneratedThorns& thorns = def->thorns[t];
        ThornsEffect& slot = out[written++];
        slot.slot = piece.slot;
        slot.damage_type = thorns.damage_type;
        slot.chance = linear_value(thorns.chance, entry.level);
        slot.min_damage = linear_value(thorns.min_damage, entry.level);
        slot.max_damage = linear_value(thorns.max_damage, entry.level);
        slot.item_damage = static_cast<int>(linear_value(thorns.item_damage, entry.level));
      }
    }
  }
  return written;
}

MeleeEnchantments item_melee_enchantments(const ItemStack& stack, std::string_view victim_type,
                                          std::string_view attacker_type) {
  MeleeEnchantments out;
  // EnchantmentHelper.runIterationOnItem walks the stack's own enchantments;
  // the slot was already decided when the stack was equipped, so there is no
  // matchingSlot test here — the caller passes the weapon it swings.
  for (const ItemEnchantment& entry : stack.enchantments) {
    const GeneratedEnchantment* def = enchantment_by_id(entry.enchantment);
    if (!def)
      continue;
    if ((def->effects & (1u << kEffectComponentDamage)) != 0 &&
        requirement_met(def->damage_tag, def->damage_tag_entity, victim_type, attacker_type))
      out.damage += linear_value(def->damage_add, entry.level);
    if ((def->effects & (1u << kEffectComponentKnockback)) != 0 &&
        requirement_met(def->knockback_tag, def->knockback_tag_entity, victim_type, attacker_type))
      out.knockback += linear_value(def->knockback_add, entry.level);
    // EnchantmentHelper.modifyFallBasedDamage -> Enchantment.modifyFallBasedDamage
    // -> `modifyDamageFilteredValue(SMASH_DAMAGE_PER_FALLEN_BLOCK, ...)`, called by
    // MaceItem.getAttackDamageBonus with a *base* of 0.0 and then multiplied by the
    // fall distance there. Density is the only enchantment in the data that
    // carries the component, and it tests nothing — but the same predicate shape
    // is kept, because a future datapack entry could.
    if ((def->effects & (1u << kEffectComponentSmashDamagePerFallenBlock)) != 0 &&
        requirement_met(def->smash_damage_tag, def->smash_damage_tag_entity, victim_type,
                        attacker_type))
      out.smash_damage += linear_value(def->smash_damage_add, entry.level);
    if ((def->effects & (1u << kEffectComponentAttributes)) != 0)
      out.sweep_ratio += sweeping_ratio(*def, entry.level);
    // EnchantmentHelper.doPostAttackEffects -> Enchantment.applyEffects over
    // minecraft:post_attack. Fire Aspect and Bane of Arthropods are the only two
    // entries in the data that land on the victim for a melee swing, and Bane's
    // own tag is what keeps it off a pig.
    if ((def->effects & (1u << kEffectComponentPostAttack)) != 0)
      add_post_attack(out, *def, entry.level, victim_type, attacker_type);
  }
  return out;
}

} // namespace mc::world
