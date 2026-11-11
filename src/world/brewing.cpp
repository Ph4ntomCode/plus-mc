// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/brewing.h"

#include <array>
#include <cstddef>

#include "world/item.h"

namespace mc::world {
namespace {

// --- mixing -----------------------------------------------------------------

// PotionBrewing.hasContainerMix: the container itself changes (a potion plus
// gunpowder becomes a splash potion), so the mix names items.
const GeneratedContainerMix* find_container_mix(int32_t from_item, int32_t ingredient) {
  for (int i = 0; i < kBrewingContainerMixCount; ++i) {
    const GeneratedContainerMix& mix = kBrewingContainerMixes[i];
    if (mix.from == from_item && mix.ingredient == ingredient)
      return &mix;
  }
  return nullptr;
}

// PotionBrewing.hasPotionMix: the potion inside the bottle changes and the
// bottle itself does not.
const GeneratedPotionMix* find_potion_mix(int32_t from_potion, int32_t ingredient) {
  for (int i = 0; i < kBrewingPotionMixCount; ++i) {
    const GeneratedPotionMix& mix = kBrewingPotionMixes[i];
    if (mix.from == from_potion && mix.ingredient == ingredient)
      return &mix;
  }
  return nullptr;
}

// `source.getOrDefault(DataComponents.POTION_CONTENTS, EMPTY).potion()` — the
// potion registry id of a stack, or -1 when it carries no potion at all.
int32_t source_potion(const ItemStack& source) {
  if (!source.potion_contents)
    return -1;
  return source.potion_contents->potion;
}

void shrink_one(Inventory& slots, int slot) {
  ItemStack stack = slots.get(slot);
  if (stack.count <= 1)
    stack.clear();
  else
    stack.count -= 1;
  slots.set(slot, stack);
}

// PotionBrewing.isContainerIngredient / isPotionIngredient: the two halves of
// the mix list an ingredient can come from (the container changes, or the
// potion inside it does).
bool is_container_ingredient(int32_t item) {
  for (int i = 0; i < kBrewingContainerMixCount; ++i)
    if (kBrewingContainerMixes[i].ingredient == item)
      return true;
  return false;
}

bool is_potion_ingredient(int32_t item) {
  for (int i = 0; i < kBrewingPotionMixCount; ++i)
    if (kBrewingPotionMixes[i].ingredient == item)
      return true;
  return false;
}

// BrewingStandBlockEntity.getPotionBits: which bottle slots hold anything.
std::array<bool, kBrewingBottleSlots> potion_bits(const Inventory& slots) {
  std::array<bool, kBrewingBottleSlots> bits{};
  for (int i = 0; i < kBrewingBottleSlots; ++i)
    bits[static_cast<size_t>(i)] = !slots.get(i).empty();
  return bits;
}

// BrewingStandBlockEntity.isBrewable: the ingredient is something the stand
// accepts, and at least one bottle has a mix with it. Slots that hold something
// which is not a container (a glass bottle, an already-brewed potion with no
// match) simply do not count.
bool is_brewable(const Inventory& slots) {
  const ItemStack& ingredient = slots.get(kBrewingIngredientSlot);
  if (ingredient.empty() || !brewing_is_ingredient(ingredient.item))
    return false;
  for (int i = 0; i < kBrewingBottleSlots; ++i) {
    const ItemStack& bottle = slots.get(i);
    if (bottle.empty())
      continue;
    if (brewing_has_mix(bottle, ingredient.item))
      return true;
  }
  return false;
}

// BrewingStandBlockEntity.doBrew: every bottle slot is run through `mix` (the
// ones without a mix come back unchanged), the ingredient is consumed, and the
// ingredient's *crafting remainder* — Item.getCraftingRemainder, which for the
// one brewing ingredient that has one is dragon's breath -> glass bottle —
// either takes the emptied slot or is dropped on the ground.
//
// The drop is reported to the caller, which owns spawning item entities.
void do_brew(Inventory& slots, ItemStack* dropped_remainder) {
  const int32_t ingredient = slots.get(kBrewingIngredientSlot).item;
  for (int i = 0; i < kBrewingBottleSlots; ++i)
    slots.set(i, brewing_mix(ingredient, slots.get(i)));

  shrink_one(slots, kBrewingIngredientSlot);
  const int32_t remainder = crafting_remainder_item(ingredient);
  if (remainder != kAirItemId) {
    if (slots.get(kBrewingIngredientSlot).empty()) {
      ItemStack back;
      back.item = remainder;
      back.count = 1;
      slots.set(kBrewingIngredientSlot, back);
    } else {
      ItemStack drop;
      drop.item = remainder;
      drop.count = 1;
      *dropped_remainder = drop;
    }
  }
}

} // namespace

// --- lookups ----------------------------------------------------------------

bool brewing_is_container(int32_t item) {
  for (int i = 0; i < kBrewingContainerCount; ++i)
    if (kBrewingContainers[i] == item)
      return true;
  return false;
}

bool brewing_is_ingredient(int32_t item) {
  return is_container_ingredient(item) || is_potion_ingredient(item);
}

bool brewing_is_fuel(int32_t item) {
  for (int i = 0; i < kBrewingFuelCount; ++i)
    if (kBrewingFuels[i] == item)
      return true;
  return false;
}

bool brewing_is_bottle_slot_item(int32_t item) {
  return brewing_is_container(item) ||
         item == ItemRegistry::instance().id_of("minecraft:glass_bottle").value_or(kAirItemId);
}

bool brewing_has_mix(const ItemStack& source, int32_t ingredient) {
  if (source.empty() || !brewing_is_container(source.item))
    return false;
  if (find_container_mix(source.item, ingredient) != nullptr)
    return true;
  const int32_t potion = source_potion(source);
  return potion >= 0 && find_potion_mix(potion, ingredient) != nullptr;
}

ItemStack brewing_mix(int32_t ingredient, const ItemStack& source) {
  if (source.empty())
    return source;
  const int32_t potion = source_potion(source);
  if (potion < 0)
    return source; // no potion to carry over: a glass bottle comes back as-is
  // Container mixes first, in the order PotionBrewing.mix tries them: the item
  // changes and the potion rides along.
  if (const GeneratedContainerMix* mix = find_container_mix(source.item, ingredient)) {
    ItemStack out;
    out.item = mix->to;
    out.count = 1;
    out.potion_contents = StackPotionContents{potion, {}};
    return out;
  }
  // Then potion mixes: the same item, a new potion.
  if (const GeneratedPotionMix* mix = find_potion_mix(potion, ingredient)) {
    ItemStack out;
    out.item = source.item;
    out.count = 1;
    out.potion_contents = StackPotionContents{mix->to, {}};
    return out;
  }
  return source;
}

// --- the tick ---------------------------------------------------------------

BrewingTickResult tick_brewing(BrewingState& state, Inventory& slots) {
  BrewingTickResult result;

  // The fuel branch: only fires when the stand is *out* of uses, so a stand
  // that already has fuel does not burn a second unit for the same brews.
  if (state.fuel <= 0 && brewing_is_fuel(slots.get(kBrewingFuelSlot).item)) {
    state.fuel = kBrewingFuelUses;
    shrink_one(slots, kBrewingFuelSlot);
    result.changed = true;
    result.data_changed = true;
  }

  const bool brewable = is_brewable(slots);
  const int32_t ingredient_item = slots.get(kBrewingIngredientSlot).item;
  ItemStack dropped;

  if (state.brew_time > 0) {
    state.brew_time -= 1;
    if (state.brew_time == 0 && brewable) {
      do_brew(slots, &dropped);
      result.brewed = true;
    } else if (!brewable || ingredient_item != state.ingredient_item) {
      // `ingredient.is(entity.ingredient)`: a different item under a running
      // brew cancels it outright, and it does not restart on its own because
      // `isBrewing` was already true.
      state.brew_time = 0;
    }
    result.changed = true;
    result.data_changed = true;
  } else if (brewable && state.fuel > 0) {
    state.fuel -= 1;
    state.brew_time = kBrewingTimeTicks;
    state.ingredient_item = ingredient_item;
    result.changed = true;
    result.data_changed = true;
  }

  // The `has_bottle_0..2` block properties, which the client draws the bottle
  // models from. Vanilla compares against a field that starts null, so the very
  // first tick always writes the state.
  const std::array<bool, kBrewingBottleSlots> bits = potion_bits(slots);
  bool same = state.bottles_known;
  for (int i = 0; i < kBrewingBottleSlots && same; ++i)
    same = state.bottles[static_cast<size_t>(i)] == bits[static_cast<size_t>(i)];
  if (!same) {
    state.bottles_known = true;
    for (int i = 0; i < kBrewingBottleSlots; ++i)
      state.bottles[static_cast<size_t>(i)] = bits[static_cast<size_t>(i)];
    result.bottles[0] = bits[0];
    result.bottles[1] = bits[1];
    result.bottles[2] = bits[2];
    result.bottle_bits_changed = true;
    result.changed = true;
  }

  // `Containers.dropItemStack` for a remainder that had nowhere to go: the
  // caller runs the drop, because a tick cannot reach item entities.
  result.drop_remainder = dropped;
  return result;
}

} // namespace mc::world
