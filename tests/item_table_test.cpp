// SPDX-License-Identifier: GPL-3.0-or-later
//
// Data-derived unit test for the generated item registry (PLAN §10.1): if we
// load the vanilla generator's own item data, these hold by construction — the
// test exists to catch generator and loader bugs, not to re-state the data.
//
// The values pinned here are the ones the engine actually branches on (stack
// merging, tool gating, armour, hunger), so a regeneration that silently shifts
// them fails loudly.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "world/item_registry.h"

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
  if (ok)
    return;
  ++g_failures;
  std::printf("FAIL: %s\n", what);
}

const mc::world::GeneratedItem* find(const char* name) {
  for (int i = 0; i < mc::world::kItemCount; ++i)
    if (std::strcmp(mc::world::kItems[i].name, name) == 0)
      return &mc::world::kItems[i];
  return nullptr;
}

// Asserts the whole row, so a change in any attribute is visible.
void expect_item(const char* name, int32_t id, uint16_t stack, uint16_t damage, uint8_t armor,
                 uint8_t nutrition, uint8_t flags) {
  const auto* it = find(name);
  if (!it) {
    ++g_failures;
    std::printf("FAIL: %s is missing from the item registry\n", name);
    return;
  }
  if (it->id != id || it->max_stack_size != stack || it->max_damage != damage ||
      it->armor != armor || it->nutrition != nutrition || it->flags != flags) {
    ++g_failures;
    std::printf("FAIL: %s = {id %d, stack %u, damage %u, armor %u, food %u, flags %u}\n"
                "      want {id %d, stack %u, damage %u, armor %u, food %u, flags %u}\n",
                name, it->id, it->max_stack_size, it->max_damage, it->armor, it->nutrition,
                it->flags, id, stack, damage, armor, nutrition, flags);
  }
}

} // namespace

int main() {
  using namespace mc::world;

  // --- Structural invariants ------------------------------------------------
  check(kItemCount == 1658, "kItemCount should be 1658 for 26.3");
  bool ids_dense = true, names_unique = true, names_prefixed = true, stacks_in_range = true;
  bool damageable_stacks_to_one = true;
  for (int i = 0; i < kItemCount; ++i) {
    const GeneratedItem& it = kItems[i];
    if (it.id != i)
      ids_dense = false;
    if (std::strncmp(it.name, "minecraft:", 10) != 0 || it.name[10] == '\0')
      names_prefixed = false;
    // MAX_STACK_SIZE's component codec is intRange(1, 99).
    if (it.max_stack_size < 1 || it.max_stack_size > 99)
      stacks_in_range = false;
    // Vanilla: a damageable item never stacks (tools, armour, shield, elytra).
    if ((it.flags & kItemIsDamageable) && it.max_stack_size != 1)
      damageable_stacks_to_one = false;
    // Nutrition and the food flag travel together.
    if (it.nutrition > 0 && !(it.flags & kItemIsFood))
      ++g_failures;
    for (int j = i + 1; j < kItemCount; ++j)
      if (std::strcmp(it.name, kItems[j].name) == 0)
        names_unique = false;
  }
  check(ids_dense, "item ids must be dense (the id IS the registry index)");
  check(names_unique, "item names must be unique");
  check(names_prefixed, "every name must be a minecraft: identifier");
  check(stacks_in_range, "max_stack_size outside MAX_STACK_SIZE's 1..99 range");
  check(damageable_stacks_to_one, "a damageable item must stack to 1");

  // --- Spot values the engine depends on ------------------------------------
  // Air is *not* a block item: `Items.AIR` is registered as an `AirItem`, which
  // extends `Item`, not `BlockItem` (the generator reads the registration, not
  // the name). It matters for a placement made with a block in the offhand.
  expect_item("minecraft:air", 0, 64, 0, 0, 0, 0);
  expect_item("minecraft:stone", 1, 64, 0, 0, 0, kItemIsBlock);
  // The seed/crop split: `minecraft:wheat_seeds` places `minecraft:wheat` and is
  // a block item, while the item named `minecraft:wheat` — the harvest, whose
  // name *is* a block — places nothing.
  expect_item("minecraft:wheat_seeds", 1065, 64, 0, 0, 0, kItemIsBlock);
  expect_item("minecraft:wheat", 1066, 64, 0, 0, 0, 0);
  // The item whose name is not the block it places, in the item table's own
  // terms: the redstone dust places `minecraft:redstone_wire`.
  expect_item("minecraft:redstone", 824, 64, 0, 0, 0, kItemIsBlock);
  // A plantable food keeps both bits.
  expect_item("minecraft:carrot", 1378, 64, 0, 0, 3, kItemIsFood | kItemIsBlock);
  // Tools: durability + the tool component, and never a block item.
  expect_item("minecraft:diamond_pickaxe", 1052, 1, 1561, 0, 0, kItemIsDamageable | kItemHasTool);
  expect_item("minecraft:netherite_pickaxe", 1057, 1, 2031, 0, 0,
              kItemIsDamageable | kItemHasTool | kItemFireResistant);
  // Armour: durability plus the minecraft:armor attribute modifiers, and the two
  // flags the wear step reads — `damage_on_hurt` (every worn piece) and the
  // netherite fire resistance of `ItemStack.canBeHurtBy`.
  expect_item("minecraft:netherite_chestplate", 1093, 1, 592, 8, 0,
              kItemIsDamageable | kItemDamageOnHurt | kItemFireResistant);
  expect_item("minecraft:diamond_leggings", 1086, 1, 495, 6, 0,
              kItemIsDamageable | kItemDamageOnHurt);
  // `minecraft:elytra` is the item that makes `kItemDamageOnHurt` necessary: it
  // is damageable and equippable in a player's chest slot, and the pack sets
  // `damage_on_hurt: false` on it (the other eleven are the mount and nautilus
  // armours, whose equippable slot is `body`).
  expect_item("minecraft:elytra", 974, 1, 432, 0, 0, kItemIsDamageable);
  // Food carries nutrition.
  expect_item("minecraft:apple", 1007, 64, 0, 0, 4, kItemIsFood);
  expect_item("minecraft:cooked_beef", 1261, 64, 0, 0, 8, kItemIsFood);
  // The 16-stack family and the 1-stack oddballs.
  expect_item("minecraft:ender_pearl", 1265, 16, 0, 0, 0, 0);
  expect_item("minecraft:snowball", 1132, 16, 0, 0, 0, 0);
  expect_item("minecraft:oak_sign", 1102, 16, 0, 0, 0, kItemIsBlock);
  expect_item("minecraft:water_bucket", 1129, 1, 0, 0, 0, 0);
  expect_item("minecraft:potion", 1271, 1, 0, 0, 0, 0);
  // Plain materials and their block counterparts.
  expect_item("minecraft:stick", 1060, 64, 0, 0, 0, 0);
  expect_item("minecraft:oak_planks", 63, 64, 0, 0, 0, kItemIsBlock);

  // --- The two armour flags, counted ----------------------------------------
  // The counts are the pack's own (84 equippable components, 12 of them
  // `damage_on_hurt: false`; 17 `damage_resistant`, 16 of them fire), so a
  // regeneration that changes either fails here rather than on the armour path.
  int damage_on_hurt = 0, fire_resistant = 0;
  for (int i = 0; i < kItemCount; ++i) {
    if (kItems[i].flags & kItemDamageOnHurt)
      ++damage_on_hurt;
    if (kItems[i].flags & kItemFireResistant)
      ++fire_resistant;
  }
  check(damage_on_hurt == 72,
        "the pack's 84 equippable items, less the 12 whose damage_on_hurt is false");
  check(fire_resistant == 16,
        "the pack's #minecraft:is_fire damage_resistant items (every netherite one)");

  if (g_failures == 0) {
    std::printf("PASS: item-table\t(%d items, ids dense, attributes pinned)\n", kItemCount);
    return 0;
  }
  std::printf("%d item-table check(s) failed\n", g_failures);
  return 1;
}
