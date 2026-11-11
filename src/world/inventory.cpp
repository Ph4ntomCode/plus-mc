// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/inventory.h"

namespace mc::world {

namespace {
// Shared empty stack handed out for out-of-range reads.
const ItemStack kEmptyStack;
} // namespace

int armor_menu_slot_to_inventory_slot(int menu_slot) {
  // Menu order is head-first; the inventory stores feet-first.
  if (menu_slot < kMenuArmorFirstSlot || menu_slot >= kMenuArmorFirstSlot + kArmorSlots)
    return -1;
  const int index = menu_slot - kMenuArmorFirstSlot; // 0 = head
  return kHeadSlot - index;                          // 39, 38, 37, 36
}

int menu_slot_to_inventory_slot(int menu_slot) {
  if (menu_slot >= kMenuArmorFirstSlot && menu_slot < kMenuArmorFirstSlot + kArmorSlots)
    return armor_menu_slot_to_inventory_slot(menu_slot);
  if (menu_slot >= kMenuMainFirstSlot && menu_slot < kMenuHotbarFirstSlot)
    return kMainFirstSlot + (menu_slot - kMenuMainFirstSlot);
  if (menu_slot >= kMenuHotbarFirstSlot && menu_slot < kMenuHotbarFirstSlot + kHotbarSlots)
    return kHotbarFirstSlot + (menu_slot - kMenuHotbarFirstSlot);
  if (menu_slot == kMenuOffhandSlot)
    return kOffhandSlot;
  return -1; // 0 = crafting result, 1..4 = crafting grid
}

int inventory_slot_to_menu_slot(int inv_slot) {
  if (inv_slot >= kMainFirstSlot && inv_slot < kArmorFirstSlot)
    return kMenuMainFirstSlot + (inv_slot - kMainFirstSlot);
  if (inv_slot >= kHotbarFirstSlot && inv_slot < kHotbarFirstSlot + kHotbarSlots)
    return kMenuHotbarFirstSlot + (inv_slot - kHotbarFirstSlot);
  if (inv_slot >= kArmorFirstSlot && inv_slot < kArmorFirstSlot + kArmorSlots)
    return kMenuArmorFirstSlot + (kHeadSlot - inv_slot);
  if (inv_slot == kOffhandSlot)
    return kMenuOffhandSlot;
  return -1;
}

const ItemStack& Inventory::get(int slot) const {
  if (!valid(slot))
    return kEmptyStack;
  return slots_[static_cast<size_t>(slot)];
}

void Inventory::set(int slot, const ItemStack& stack) {
  if (!valid(slot))
    return;
  slots_[static_cast<size_t>(slot)] = stack;
}

void Inventory::clear_all() {
  for (ItemStack& slot : slots_)
    slot.clear();
}

bool Inventory::empty() const {
  for (const ItemStack& slot : slots_)
    if (!slot.empty())
      return false;
  return true;
}

int Inventory::add_in_range(int first, int count, const ItemStack& stack) {
  if (stack.empty() || count <= 0)
    return 0;
  int remaining = stack.count;

  // 1. Top up existing stacks of the same item.
  for (int i = first; i < first + count && remaining > 0; ++i) {
    if (!valid(i))
      continue;
    ItemStack& slot = slots_[static_cast<size_t>(i)];
    if (slot.empty() || !slot.can_merge_with(stack))
      continue;
    remaining -= slot.grow(remaining);
  }
  // 2. Then fill empty slots.
  for (int i = first; i < first + count && remaining > 0; ++i) {
    if (!valid(i))
      continue;
    ItemStack& slot = slots_[static_cast<size_t>(i)];
    if (!slot.empty())
      continue;
    ItemStack placed = stack;
    const int room = placed.max_stack_size();
    const int taken = remaining < room ? remaining : room;
    placed.count = taken;
    placed.damage = stack.damage;
    slot = placed;
    remaining -= taken;
  }
  return remaining;
}

int Inventory::add(const ItemStack& stack, int selected) {
  if (stack.empty())
    return 0;
  int remaining = stack.count;

  auto add_to_slot = [&](int slot) {
    if (!valid(slot) || remaining <= 0)
      return;
    ItemStack& target = slots_[static_cast<size_t>(slot)];
    if (target.empty()) {
      target = stack;
      target.count = 0;
      target.damage = stack.damage;
    }
    const int room = target.max_stack_size() - target.count;
    const int placed = remaining < room ? remaining : room;
    target.count += placed;
    remaining -= placed;
  };

  // 1. The selected slot, when it already holds this item with room to spare.
  if (valid(selected) && !slots_[static_cast<size_t>(selected)].empty() &&
      slots_[static_cast<size_t>(selected)].same_item(stack) &&
      slots_[static_cast<size_t>(selected)].count <
          slots_[static_cast<size_t>(selected)].max_stack_size())
    add_to_slot(selected);

  // 2. Any stack with room, then the first empty slot — repeating so a large
  //    add spreads over several slots.
  while (remaining > 0) {
    int slot = slot_with_remaining_space(stack);
    if (slot == -1)
      slot = first_empty_in_range(0, size());
    if (slot == -1)
      break; // full
    const int before = remaining;
    add_to_slot(slot);
    if (remaining == before)
      break; // no progress: belt-and-braces against a pathological loop
  }
  return remaining;
}

int Inventory::slot_with_remaining_space(const ItemStack& stack) const {
  for (int i = 0; i < size(); ++i) {
    const ItemStack& slot = slots_[static_cast<size_t>(i)];
    if (!slot.empty() && slot.same_item(stack) && slot.count < slot.max_stack_size())
      return i;
  }
  return -1;
}

int Inventory::find(const ItemStack& stack) const {
  for (int i = 0; i < size(); ++i)
    if (!slots_[static_cast<size_t>(i)].empty() && slots_[static_cast<size_t>(i)].same_item(stack))
      return i;
  return -1;
}

int Inventory::first_empty_in_range(int first, int count) const {
  for (int i = first; i < first + count; ++i)
    if (valid(i) && slots_[static_cast<size_t>(i)].empty())
      return i;
  return -1;
}

} // namespace mc::world
