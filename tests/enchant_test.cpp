// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for enchantments (Phase 3 milestone G):
//   * the generated registry: 43 enchantments, the table index *is* the wire id
//     the `minecraft:enchantments` component uses, the slot masks, and the
//     supported-item and exclusive-set tags the generator resolved,
//   * Enchantment.canEnchant / matchingSlot / areCompatible, including the sharp
//     edge that an item already carrying an enchantment counts as incompatible
//     with itself,
//   * EnchantmentHelper.getDamageProtection: the linear level value, the
//     damage_source_properties tag predicate (Protection covers everything that
//     does not bypass invulnerability, Blast Protection only explosions), the
//     slot gate, and each protection enchantment's real number,
//   * LivingEntity.getDamageAfterMagicAbsorb's gates: Resistance, and the
//     BYPASSES_EFFECTS / BYPASSES_ENCHANTMENTS / BYPASSES_RESISTANCE tags in
//     vanilla's order,
//   * the minecraft:enchantments item component: the value type, the sorting,
//     same_item's isSameItemSameComponents rule, and the wire bytes both ways.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "world/damage.h"
#include "world/enchantment.h"
#include "world/item.h"
#include "world/item_registry.h"

using namespace mc::world;

static int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                                      \
  do {                                                                                             \
    const double va = (a), vb = (b);                                                               \
    if (std::fabs(va - vb) > (eps)) {                                                              \
      std::fprintf(stderr, "FAIL %s:%d: %s == %f, want %f\n", __FILE__, __LINE__, #a, va, vb);     \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

namespace {

int32_t item_id(const char* name) {
  const auto id = ItemRegistry::instance().id_of(name);
  return id ? *id : -1;
}

ItemStack item(const char* name) {
  return ItemStack::of(name, 1);
}

// A stack of one named item with one enchantment at `level`, which is what
// `/enchant` produces.
ItemStack enchanted(const char* name, const char* enchantment, int level) {
  ItemStack stack = item(name);
  const GeneratedEnchantment* def = enchantment_by_name(enchantment);
  if (def)
    stack.set_enchantment(enchantment_id_of(*def), level);
  return stack;
}

// The four armour slots, in the inventory order the session fills them in.
struct Armour {
  ItemStack feet, legs, chest, head;
};

int armour_entries(const Armour& armour, uint8_t* slots, const ItemStack** stacks) {
  static constexpr uint8_t kBits[4] = {kEnchantSlotFeet, kEnchantSlotLegs, kEnchantSlotChest,
                                       kEnchantSlotHead};
  const ItemStack* list[4] = {&armour.feet, &armour.legs, &armour.chest, &armour.head};
  for (int i = 0; i < 4; ++i) {
    slots[i] = kBits[i];
    stacks[i] = list[i];
  }
  return 4;
}

float protection_of(const Armour& armour, const char* damage_type) {
  const GeneratedDamageType* type = damage_type_by_name(damage_type);
  uint8_t slots[4];
  const ItemStack* stacks[4];
  const int count = armour_entries(armour, slots, stacks);
  std::vector<EquipmentEntry> entries;
  for (int i = 0; i < count; ++i) {
    EquipmentEntry entry;
    entry.slot = slots[i];
    entry.stack = *stacks[i];
    entries.push_back(entry);
  }
  return damage_protection(entries.data(), static_cast<int>(entries.size()), *type);
}

void test_registry() {
  CHECK(kEnchantmentCount == 43);
  for (int i = 0; i < kEnchantmentCount; ++i) {
    const GeneratedEnchantment* def = enchantment_by_id(i);
    CHECK(def != nullptr);
    CHECK(enchantment_id_of(*def) == i);
  }
  CHECK(enchantment_by_id(-1) == nullptr);
  CHECK(enchantment_by_id(kEnchantmentCount) == nullptr);

  // minecraft:enchantment is a datapack registry, so the id is the captured
  // wire order's index: protection is 28th, and the table must agree.
  const GeneratedEnchantment* protection = enchantment_by_name("minecraft:protection");
  CHECK(protection != nullptr);
  CHECK(enchantment_id_of(*protection) == 28);
  CHECK(enchantment_by_name("PROTECTION") == protection);
  CHECK(enchantment_by_name("not an enchantment") == nullptr);

  CHECK(protection->max_level == 4);
  CHECK(protection->slots == kEnchantSlotArmor);
  CHECK(protection->protection_count == 1);
  // Protection's predicate excludes only bypasses_invulnerability, so it covers
  // everything that can actually hurt a living entity.
  CHECK(protection->protections[0].required_tags == 0);
  CHECK(protection->protections[0].excluded_tags == kDamageTagBypassesInvulnerability);
  CHECK_NEAR(protection->protections[0].base, 1.0f, 1e-6);
  CHECK_NEAR(protection->protections[0].per_level_above_first, 1.0f, 1e-6);
  // The components bitmask names the one component this server applies, and
  // records the ones it does not: Protection also carries no other component,
  // which is why it is fully modelled.
  CHECK((protection->effects & (1u << kEffectComponentDamageProtection)) != 0);

  // Blast Protection adds `is_explosion` to the predicate; Fire Protection wraps
  // its single term in a `minecraft:all_of`, and both come out the same way.
  const GeneratedEnchantment* blast = enchantment_by_name("blast_protection");
  CHECK(blast && blast->protection_count == 1);
  CHECK(blast->protections[0].required_tags == kDamageTagIsExplosion);
  CHECK(blast->protections[0].excluded_tags == kDamageTagBypassesInvulnerability);
  const GeneratedEnchantment* fire = enchantment_by_name("fire_protection");
  CHECK(fire && fire->protection_count == 1);
  CHECK(fire->protections[0].required_tags == kDamageTagIsFire);
  const GeneratedEnchantment* projectile = enchantment_by_name("projectile_protection");
  CHECK(projectile && projectile->protections[0].required_tags == kDamageTagIsProjectile);
  const GeneratedEnchantment* feather = enchantment_by_name("feather_falling");
  CHECK(feather && feather->protections[0].required_tags == kDamageTagIsFall);

  // Sharpness has no protection entry at all, and its effects bitmask says what
  // it *does* carry — `minecraft:damage`, which is not modelled.
  const GeneratedEnchantment* sharpness = enchantment_by_name("sharpness");
  CHECK(sharpness != nullptr);
  CHECK(sharpness->protection_count == 0);
  CHECK(sharpness->max_level == 5);
  CHECK(sharpness->slots == kEnchantSlotMainhand);
  CHECK((sharpness->effects & (1u << kEffectComponentDamageProtection)) == 0);
  CHECK(sharpness->effects != 0);

  // The exclusive sets are resolved from the enchantment tags, not hardcoded:
  // the four armour protections exclude each other.
  const uint32_t* exclusive = protection->exclusive_set;
  CHECK(protection->exclusive_count == 4);
  CHECK(std::binary_search(exclusive, exclusive + 4, 3u));  // blast_protection
  CHECK(std::binary_search(exclusive, exclusive + 4, 11u)); // fire_protection
  CHECK(std::binary_search(exclusive, exclusive + 4, 27u)); // projectile_protection
  CHECK(std::binary_search(exclusive, exclusive + 4, 28u)); // protection
  CHECK(!std::binary_search(exclusive, exclusive + 4, 9u)); // feather_falling is free
}

void test_applicability() {
  const GeneratedEnchantment* protection = enchantment_by_name("protection");
  const GeneratedEnchantment* sharpness = enchantment_by_name("sharpness");
  CHECK(protection && sharpness);

  // Enchantment.canEnchant is supported_items membership.
  CHECK(enchantment_supports_item(*protection, item_id("minecraft:diamond_chestplate")));
  CHECK(enchantment_supports_item(*protection, item_id("minecraft:leather_boots")));
  CHECK(!enchantment_supports_item(*protection, item_id("minecraft:diamond_sword")));
  CHECK(enchantment_supports_item(*sharpness, item_id("minecraft:diamond_sword")));
  CHECK(!enchantment_supports_item(*sharpness, item_id("minecraft:diamond_chestplate")));

  // matchingSlot: the armour protections only answer in an armour slot, which is
  // the gate EnchantmentHelper.runIterationOnEquipment tests.
  CHECK(enchantment_applies_to_slot(*protection, kEnchantSlotChest));
  CHECK(enchantment_applies_to_slot(*protection, kEnchantSlotHead));
  CHECK(!enchantment_applies_to_slot(*protection, kEnchantSlotMainhand));
  CHECK(enchantment_applies_to_slot(*sharpness, kEnchantSlotMainhand));
  CHECK(!enchantment_applies_to_slot(*sharpness, kEnchantSlotChest));
  // Mending's group is "any", so every slot answers.
  const GeneratedEnchantment* mending = enchantment_by_name("mending");
  CHECK(mending && enchantment_applies_to_slot(*mending, kEnchantSlotMainhand) &&
        enchantment_applies_to_slot(*mending, kEnchantSlotOffhand) &&
        enchantment_applies_to_slot(*mending, kEnchantSlotFeet));
}

void test_compatibility() {
  const GeneratedEnchantment* protection = enchantment_by_name("protection");
  const GeneratedEnchantment* blast = enchantment_by_name("blast_protection");
  const GeneratedEnchantment* feather = enchantment_by_name("feather_falling");

  // Two members of the same exclusive set cannot coexist, in either direction.
  ItemStack plate = enchanted("minecraft:diamond_chestplate", "protection", 4);
  CHECK(!enchantment_compatible_with(plate, *blast));
  ItemStack other = enchanted("minecraft:diamond_chestplate", "blast_protection", 4);
  CHECK(!enchantment_compatible_with(other, *protection));

  // ...but a different kind of protection is free to join.
  CHECK(enchantment_compatible_with(plate, *feather));

  // Enchantment.areCompatible is false for two *equal* holders, so an item that
  // already carries the enchantment is incompatible with itself: a second
  // `/enchant` at a higher level is refused, which is genuinely vanilla.
  CHECK(!enchantment_compatible_with(plate, *protection));
  CHECK(enchantment_compatible_with(item("minecraft:diamond_chestplate"), *protection));
}

void test_damage_protection() {
  const Armour bare{};
  CHECK_NEAR(protection_of(bare, "generic"), 0.0f, 1e-6);

  // A full Protection IV set: each piece is 1 + 1 * (4 - 1) = 4.
  Armour plated;
  plated.feet = enchanted("minecraft:diamond_boots", "protection", 4);
  plated.legs = enchanted("minecraft:diamond_leggings", "protection", 4);
  plated.chest = enchanted("minecraft:diamond_chestplate", "protection", 4);
  plated.head = enchanted("minecraft:diamond_helmet", "protection", 4);
  CHECK_NEAR(protection_of(plated, "generic"), 16.0f, 1e-4);
  CHECK_NEAR(protection_of(plated, "mob_attack"), 16.0f, 1e-4);
  // A type that bypasses invulnerability is excluded by Protection's own
  // predicate, so /kill is not reduced by it.
  CHECK_NEAR(protection_of(plated, "generic_kill"), 0.0f, 1e-4);
  CHECK_NEAR(protection_of(plated, "out_of_world"), 0.0f, 1e-4);
  // Protection is the *general* one: its predicate asks for no tag at all, so
  // a fall or an explosion is reduced just as much as a sword. What separates
  // it from Blast Protection is the extra requirement, not a narrower one.
  CHECK_NEAR(protection_of(plated, "fall"), 16.0f, 1e-4);
  CHECK_NEAR(protection_of(plated, "explosion"), 16.0f, 1e-4);

  // A single piece is a quarter of that, and the linear value is not base *
  // level: Protection I is 1, IV is 4.
  Armour one;
  one.chest = enchanted("minecraft:diamond_chestplate", "protection", 1);
  CHECK_NEAR(protection_of(one, "generic"), 1.0f, 1e-4);
  one.chest = enchanted("minecraft:diamond_chestplate", "protection", 2);
  CHECK_NEAR(protection_of(one, "generic"), 2.0f, 1e-4);

  // Feather Falling IV on the boots: 3 + 3 * 3 = 12, and only against is_fall.
  Armour boots;
  boots.feet = enchanted("minecraft:diamond_boots", "feather_falling", 4);
  CHECK_NEAR(protection_of(boots, "fall"), 12.0f, 1e-4);
  CHECK_NEAR(protection_of(boots, "generic"), 0.0f, 1e-4);

  // Blast Protection IV is 2 + 2 * 3 = 8 a piece, against explosions only.
  Armour blast;
  blast.chest = enchanted("minecraft:diamond_chestplate", "blast_protection", 4);
  CHECK_NEAR(protection_of(blast, "explosion"), 8.0f, 1e-4);
  CHECK_NEAR(protection_of(blast, "player_explosion"), 8.0f, 1e-4);
  CHECK_NEAR(protection_of(blast, "generic"), 0.0f, 1e-4);

  // The slot gate: the *same* enchanted piece in the mainhand contributes
  // nothing, because Protection's slots do not include it.
  {
    const GeneratedDamageType* type = damage_type_by_name("generic");
    ItemStack plate = enchanted("minecraft:diamond_chestplate", "protection", 4);
    CHECK_NEAR(item_damage_protection(plate, kEnchantSlotChest, *type), 4.0f, 1e-4);
    CHECK_NEAR(item_damage_protection(plate, kEnchantSlotMainhand, *type), 0.0f, 1e-6);
  }

  // An empty stack is skipped rather than counted.
  {
    EquipmentEntry empty_entry;
    empty_entry.slot = kEnchantSlotChest;
    const GeneratedDamageType* type = damage_type_by_name("generic");
    CHECK_NEAR(damage_protection(&empty_entry, 1, *type), 0.0f, 1e-6);
    CHECK_NEAR(damage_protection(nullptr, 0, *type), 0.0f, 1e-6);
  }
}

void test_magic_absorb_gates() {
  const GeneratedDamageType* generic = damage_type_by_name("generic");
  const GeneratedDamageType* starve = damage_type_by_name("starve");     // BYPASSES_EFFECTS
  const GeneratedDamageType* sonic = damage_type_by_name("sonic_boom");  // BYPASSES_ENCHANTMENTS
  const GeneratedDamageType* kill = damage_type_by_name("generic_kill"); // BYPASSES_RESISTANCE
  CHECK(generic && starve && sonic && kill);

  // Protection IV's 16 points, through CombatRules.getDamageAfterMagicAbsorb:
  // 10 * (1 - 16/25) == 3.6.
  CHECK_NEAR(damage_after_magic_absorb(*generic, 10.0f, 16.0f, 1.0f), 3.6f, 1e-4);
  // No protection is a straight pass-through, and so is a protection of 0.
  CHECK_NEAR(damage_after_magic_absorb(*generic, 10.0f, 0.0f, 1.0f), 10.0f, 1e-6);
  // Resistance I is 20% off, applied *before* the enchantment step: 10 -> 8,
  // then 8 * (1 - 16/25) == 2.88.
  CHECK_NEAR(damage_after_magic_absorb(*generic, 10.0f, 16.0f, 0.8f), 2.88f, 1e-4);
  CHECK_NEAR(damage_after_magic_absorb(*generic, 10.0f, 0.0f, 0.8f), 8.0f, 1e-4);

  // A type that bypasses effects skips Resistance *and* the enchantments.
  CHECK_NEAR(damage_after_magic_absorb(*starve, 10.0f, 16.0f, 0.8f), 10.0f, 1e-6);
  // A type that bypasses enchantments still gets to resist.
  CHECK_NEAR(damage_after_magic_absorb(*sonic, 10.0f, 16.0f, 0.8f), 8.0f, 1e-4);
  // A type that bypasses resistance takes the full hit and then the protection.
  CHECK_NEAR(damage_after_magic_absorb(*kill, 10.0f, 16.0f, 0.8f), 3.6f, 1e-4);

  // Resistance V nulls a hit outright, and the zero short-circuits the
  // protection step rather than running it on zero.
  CHECK_NEAR(damage_after_magic_absorb(*generic, 10.0f, 16.0f, 0.0f), 0.0f, 1e-6);

  // The protection total itself is clamped to [0, 20] inside CombatRules, so a
  // hypothetical 25 points only ever buys the same 20.
  CHECK_NEAR(damage_after_magic_absorb(*generic, 10.0f, 25.0f, 1.0f), 2.0f, 1e-4);
}

void test_item_component() {
  // Setting a level, and the `set` semantics level 0 relies on.
  ItemStack sword = item("minecraft:diamond_sword");
  CHECK(!sword.has_enchantments());
  CHECK(sword.enchantment_level(33) == 0);
  sword.set_enchantment(33, 5); // sharpness
  CHECK(sword.has_enchantments());
  CHECK(sword.enchantment_level(33) == 5);
  CHECK(sword.enchantments.size() == 1);

  // Entries are kept sorted by id so the wire form is deterministic.
  const GeneratedEnchantment* looting = enchantment_by_name("looting");
  const GeneratedEnchantment* unbreaking = enchantment_by_name("unbreaking");
  sword.set_enchantment(enchantment_id_of(*unbreaking), 3);
  sword.set_enchantment(enchantment_id_of(*looting), 3);
  CHECK(sword.enchantments.size() == 3);
  CHECK(sword.enchantments[0].enchantment == 18); // looting
  CHECK(sword.enchantments[1].enchantment == 33); // sharpness
  CHECK(sword.enchantments[2].enchantment == 40); // unbreaking

  // Level 0 removes — which is what `/enchant <target> <enchantment> 0` does.
  sword.set_enchantment(enchantment_id_of(*looting), 0);
  CHECK(sword.enchantments.size() == 2);
  CHECK(sword.enchantment_level(18) == 0);

  // isSameItemSameComponents: an enchanted sword does not stack with a plain
  // one, nor with itself at a different level.
  ItemStack plain = item("minecraft:diamond_sword");
  ItemStack sharp5 = enchanted("minecraft:diamond_sword", "sharpness", 5);
  ItemStack sharp3 = enchanted("minecraft:diamond_sword", "sharpness", 3);
  CHECK(!plain.same_item(sharp5));
  CHECK(!sharp5.same_item(sharp3));
  CHECK(sharp5.same_item(enchanted("minecraft:diamond_sword", "sharpness", 5)));
  CHECK(!sharp5.can_merge_with(plain));

  // clear() takes the component with it.
  ItemStack cleared = sharp5;
  cleared.clear();
  CHECK(!cleared.has_enchantments());

  // The wire form: [varint count][varint item][varint added][varint removed]
  // then the components, with the enchantments one carrying
  // [varint entries][varint id][varint level] each. The expectation is built
  // rather than hardcoded so it pins the *layout* and not the item registry's
  // numbering (which the item-table test owns).
  const auto varint = [](std::vector<uint8_t>* out, uint32_t value) {
    while (true) {
      if (value < 0x80) {
        out->push_back(static_cast<uint8_t>(value));
        return;
      }
      out->push_back(static_cast<uint8_t>((value & 0x7f) | 0x80));
      value >>= 7;
    }
  };
  mc::net::Writer w;
  write_item_stack(w, sharp5);
  const std::vector<uint8_t> bytes = std::move(w).take();
  std::vector<uint8_t> expected;
  varint(&expected, 1); // count
  varint(&expected, static_cast<uint32_t>(sharp5.item));
  varint(&expected, 1); // one component added
  varint(&expected, 0); // none removed
  varint(&expected, kComponentEnchantmentsId);
  varint(&expected, 1);  // one entry
  varint(&expected, 33); // sharpness
  varint(&expected, 5);  // level 5
  CHECK(bytes == expected);

  // Durability and enchantments travel together, and the damage component comes
  // first because its id is lower.
  ItemStack worn = sharp5;
  worn.damage = 7;
  mc::net::Writer w2;
  write_item_stack(w2, worn);
  const std::vector<uint8_t> both = std::move(w2).take();
  std::vector<uint8_t> both_expected;
  varint(&both_expected, 1);
  varint(&both_expected, static_cast<uint32_t>(worn.item));
  varint(&both_expected, 2); // durability *and* enchantments
  varint(&both_expected, 0);
  varint(&both_expected, kComponentDamageId);
  varint(&both_expected, 7);
  varint(&both_expected, kComponentEnchantmentsId);
  varint(&both_expected, 1);
  varint(&both_expected, 33);
  varint(&both_expected, 5);
  CHECK(both == both_expected);

  // Reading is *not* the mirror of writing: a client's stack arrives in the
  // delimited form, where every component value is wrapped in a byte length so
  // an unknown component can be skipped instead of desyncing the reader. A
  // hand-built delimited packet is the only honest way to exercise it.
  std::vector<uint8_t> wire;
  varint(&wire, 1); // count
  varint(&wire, static_cast<uint32_t>(worn.item));
  varint(&wire, 2); // two components added
  varint(&wire, 0); // none removed
  varint(&wire, kComponentDamageId);
  varint(&wire, 1); // the value is one byte
  varint(&wire, 7);
  varint(&wire, kComponentEnchantmentsId);
  varint(&wire, 3); // varint(1) + varint(33) + varint(5)
  varint(&wire, 1);
  varint(&wire, 33);
  varint(&wire, 5);
  mc::net::Reader r(wire);
  ItemStack read;
  CHECK(read_item_stack(r, &read));
  CHECK(r.at_end());
  CHECK(read.item == worn.item);
  CHECK(read.count == 1);
  CHECK(read.damage == 7);
  CHECK(read.enchantment_level(33) == 5);
  CHECK(read.same_item(worn));

  // An unknown component is skipped by its length, and the ones after it still
  // land: this is the property the delimited encoding exists for.
  std::vector<uint8_t> with_unknown;
  varint(&with_unknown, 1);
  varint(&with_unknown, static_cast<uint32_t>(worn.item));
  varint(&with_unknown, 2);
  varint(&with_unknown, 0);
  varint(&with_unknown, 999);   // a component this build has
  varint(&with_unknown, 3);     // never heard of
  with_unknown.push_back(0xaa); // its value is opaque bytes,
  with_unknown.push_back(0xbb); // so they go in raw rather
  with_unknown.push_back(0xcc); // than as varints
  varint(&with_unknown, kComponentEnchantmentsId);
  varint(&with_unknown, 3); // varint(1) + varint(40) + varint(1)
  varint(&with_unknown, 1);
  varint(&with_unknown, 40); // unbreaking
  varint(&with_unknown, 1);
  mc::net::Reader r3(with_unknown);
  ItemStack skipped;
  CHECK(read_item_stack(r3, &skipped));
  CHECK(skipped.enchantment_level(40) == 1);
  CHECK(skipped.enchantment_level(33) == 0);

  // A hostile entry count is refused rather than reserved.
  std::vector<uint8_t> hostile;
  varint(&hostile, 1);
  varint(&hostile, static_cast<uint32_t>(worn.item));
  varint(&hostile, 1);
  varint(&hostile, 0);
  varint(&hostile, kComponentEnchantmentsId);
  varint(&hostile, 5);
  varint(&hostile, 1000000); // more entries than the cap
  for (int i = 0; i < 5; ++i)
    varint(&hostile, 1);
  mc::net::Reader r4(hostile);
  ItemStack refused;
  CHECK(!read_item_stack(r4, &refused));
}

} // namespace

int main() {
  test_registry();
  test_applicability();
  test_compatibility();
  test_damage_protection();
  test_magic_absorb_gates();
  test_item_component();
  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("enchantment tests passed\n");
  return 0;
}
