// SPDX-License-Identifier: GPL-3.0-or-later
//
// Containers: a fixed-size slot vector plus the vanilla player layout.
//
// There are two numbering systems and mixing them up is the classic
// container-protocol bug, so both live here as named constants:
//
//   player *inventory* (net.minecraft.world.entity.player.Inventory), 41 slots
//     0..8   hotbar
//     9..35  main
//     36..39 armour, ordered feet, legs, chest, head
//     40     offhand
//
//   player *menu* (InventoryMenu), 46 slots — this is the numbering the wire
//   uses, for set_creative_mode_slot, container_click and
//   container_set_content:
//     0      crafting result (derived)
//     1..4   2x2 crafting grid
//     5..8   armour, ordered head, chest, legs, feet (the reverse of the
//            inventory's order)
//     9..35  main
//     36..44 hotbar
//     45     offhand
#pragma once

#include <cstddef>
#include <vector>

#include "world/item.h"

namespace mc::world {

// --- player inventory slots -------------------------------------------------
constexpr int kHotbarSlots = 9;
constexpr int kMainSlots = 27;
constexpr int kArmorSlots = 4; // feet, legs, chest, head
constexpr int kOffhandSlots = 1;
constexpr int kPlayerInventorySize = kHotbarSlots + kMainSlots + kArmorSlots + kOffhandSlots;
constexpr int kHotbarFirstSlot = 0;
constexpr int kMainFirstSlot = kHotbarFirstSlot + kHotbarSlots; // 9
constexpr int kArmorFirstSlot = kMainFirstSlot + kMainSlots;    // 36
constexpr int kFeetSlot = kArmorFirstSlot;                      // 36
constexpr int kLegsSlot = kArmorFirstSlot + 1;                  // 37
constexpr int kChestSlot = kArmorFirstSlot + 2;                 // 38
constexpr int kHeadSlot = kArmorFirstSlot + 3;                  // 39
constexpr int kOffhandSlot = kArmorFirstSlot + kArmorSlots;     // 40

// --- player menu slots (what the wire speaks) -------------------------------
constexpr int kPlayerMenuSize = 46;
constexpr int kMenuResultSlot = 0;
constexpr int kMenuCraftFirstSlot = 1;
constexpr int kMenuCraftSlots = 4;
constexpr int kMenuArmorFirstSlot = kMenuCraftFirstSlot + kMenuCraftSlots; // 5
constexpr int kMenuMainFirstSlot = kMenuArmorFirstSlot + kArmorSlots;      // 9
constexpr int kMenuHotbarFirstSlot = kMenuMainFirstSlot + kMainSlots;      // 36
constexpr int kMenuOffhandSlot = kMenuHotbarFirstSlot + kHotbarSlots;      // 45

// --- crafting table menu (CraftingMenu) -------------------------------------
// Same 46 slots as the player menu, but a different layout: the result and the
// 3x3 grid are followed by main and hotbar with *no* armour, offhand or 2x2, so
// the player slots shift (main starts at 10, not 9). MenuType.CRAFTING is the
// 13th entry of the static menu registry, so its wire id is 12 — the same
// static-order rule the container menus use (the menu registry is not synced
// with the client; see world/container.h).
constexpr int kCraftingMenuSize = kMenuResultSlot + 1 + 9 + kMainSlots + kHotbarSlots; // 46
constexpr int kCraftingMenuGridSlots = 9;
constexpr int kCraftingMenuMainFirstSlot = kMenuCraftFirstSlot + kCraftingMenuGridSlots; // 10
constexpr int kCraftingMenuHotbarFirstSlot = kCraftingMenuMainFirstSlot + kMainSlots;    // 37
constexpr int kMenuCraftingTypeId = 12;

// --- smithing table menu (SmithingMenu via ItemCombinerMenu) ----------------
// Its four own slots come first, in index order: the template, the base, the
// addition and then the result (ItemCombinerMenuSlotDefinition requires the
// result index to follow the last input). 40 slots total. Unlike the crafting
// table the result is *not* slot 0, so the player slots shift again.
constexpr int kSmithingInputSlots = 3;
constexpr int kSmithingTemplateSlot = 0;
constexpr int kSmithingBaseSlot = 1;
constexpr int kSmithingAdditionSlot = 2;
constexpr int kSmithingResultSlot = 3;
constexpr int kSmithingMenuMainFirstSlot = kSmithingResultSlot + 1;                   // 4
constexpr int kSmithingMenuHotbarFirstSlot = kSmithingMenuMainFirstSlot + kMainSlots; // 31
constexpr int kSmithingMenuSize = kSmithingMenuHotbarFirstSlot + kHotbarSlots;        // 40
constexpr int kMenuSmithingTypeId = 21;

// --- stonecutter menu (StonecutterMenu) ------------------------------------
// Input, result, then the standard 36: 38 slots (INV_SLOT_START = 2).
constexpr int kStonecutterInputSlot = 0;
constexpr int kStonecutterResultSlot = 1;
constexpr int kStonecutterMenuMainFirstSlot = 2;                                            // 2
constexpr int kStonecutterMenuHotbarFirstSlot = kStonecutterMenuMainFirstSlot + kMainSlots; // 29
constexpr int kStonecutterMenuSize = kStonecutterMenuHotbarFirstSlot + kHotbarSlots;        // 38
constexpr int kMenuStonecutterTypeId = 24;
// StonecutterMenu's one data slot carries the selected recipe index.
constexpr int kStonecutterSelectedRecipeDataSlot = 0;

// --- merchant menu (MerchantMenu) -------------------------------------------
// The two payment slots and the *derived* result (a MerchantResultSlot, which
// `mayPlace` refuses), then the standard 36: INV_SLOT_START is 3, so the window
// is 39 slots. MenuType.MERCHANT is the 20th entry of the static menu registry
// — the same order three existing ids pin down (brewing_stand 11, crafting 12,
// smithing 21) — so its wire id is 19.
constexpr int kMerchantPaymentSlots = 2;
constexpr int kMerchantPaymentFirstSlot = 0;
constexpr int kMerchantResultSlot = 2;
constexpr int kMerchantMenuMainFirstSlot = kMerchantResultSlot + 1;                   // 3
constexpr int kMerchantMenuHotbarFirstSlot = kMerchantMenuMainFirstSlot + kMainSlots; // 30
constexpr int kMerchantMenuSize = kMerchantMenuHotbarFirstSlot + kHotbarSlots;        // 39
constexpr int kMenuMerchantTypeId = 19;

// --- enchanting table menu (EnchantmentMenu) --------------------------------
constexpr int kEnchantInputSlot = 0;
constexpr int kEnchantLapisSlot = 1;
constexpr int kEnchantMenuMainFirstSlot = 2;
constexpr int kEnchantMenuHotbarFirstSlot = kEnchantMenuMainFirstSlot + kMainSlots;
constexpr int kEnchantMenuSize = kEnchantMenuHotbarFirstSlot + kHotbarSlots;
constexpr int kMenuEnchantmentTypeId = 13;
constexpr int kEnchantDataCostFirst = 0;
constexpr int kEnchantDataCostCount = 3;
constexpr int kEnchantDataSeed = 3;
constexpr int kEnchantDataLapis = 4;

// --- anvil menu (AnvilMenu) -------------------------------------------------
constexpr int kAnvilLeftSlot = 0;
constexpr int kAnvilRightSlot = 1;
constexpr int kAnvilResultSlot = 2;
constexpr int kAnvilMenuMainFirstSlot = kAnvilResultSlot + 1;
constexpr int kAnvilMenuHotbarFirstSlot = kAnvilMenuMainFirstSlot + kMainSlots;
constexpr int kAnvilMenuSize = kAnvilMenuHotbarFirstSlot + kHotbarSlots;
constexpr int kMenuAnvilTypeId = 8;
constexpr int kAnvilDataRepairCost = 0;

// --- furnace family menu (AbstractFurnaceMenu) ------------------------------
// The three block slots first (ingredient, fuel, result), then the standard 36
// player slots, exactly like the crafting table's "own slots then player". The
// same 39-slot layout serves a furnace, a blast furnace and a smoker — only the
// MENU id and the recipe family differ, which is why `container_spec_for_block`
// carries the menu id per block.
constexpr int kFurnaceInputSlot = 0;
constexpr int kFurnaceFuelSlot = 1;
constexpr int kFurnaceResultSlot = 2;
constexpr int kFurnaceMenuMainFirstSlot = kFurnaceResultSlot + 1;                   // 3
constexpr int kFurnaceMenuHotbarFirstSlot = kFurnaceMenuMainFirstSlot + kMainSlots; // 30
constexpr int kFurnaceMenuSize = kFurnaceMenuHotbarFirstSlot + kHotbarSlots;        // 39
// AbstractFurnaceMenu.DATA_COUNT = 4, in the block entity's ContainerData order:
// flame ticks remaining, flame total, cooking ticks spent, cooking total. The
// last two are the progress-bar pair the client draws.
constexpr int kFurnaceDataCount = 4;
constexpr int kFurnaceDataLitRemaining = 0;
constexpr int kFurnaceDataLitTotal = 1;
constexpr int kFurnaceDataCookingSpent = 2;
constexpr int kFurnaceDataCookingTotal = 3;

// --- brewing stand menu (BrewingStandMenu) ----------------------------------
// Its five own slots first, in the block entity's index order (bottles 0..2,
// then the ingredient, then the fuel), then the standard 36: INV_SLOT_START is
// 5 and USE_ROW_SLOT_START 32, so the window is 41 slots. MenuType.BREWING_STAND
// is the 12th entry of the static menu registry, so its wire id is 11.
constexpr int kBrewingBottleSlots = 3;
constexpr int kBrewingIngredientSlot = 3;
constexpr int kBrewingFuelSlot = 4;
constexpr int kBrewingSlotCount = kBrewingFuelSlot + 1;                             // 5
constexpr int kBrewingMenuMainFirstSlot = kBrewingSlotCount;                        // 5
constexpr int kBrewingMenuHotbarFirstSlot = kBrewingMenuMainFirstSlot + kMainSlots; // 32
constexpr int kBrewingMenuSize = kBrewingMenuHotbarFirstSlot + kHotbarSlots;        // 41
// BrewingStandMenu.DATA_COUNT = 2, in the block entity's ContainerData order:
// the brew ticks remaining (0..400) and the fuel *uses* remaining (0..20).
constexpr int kBrewingDataCount = 2;
constexpr int kBrewingDataBrewTime = 0;
constexpr int kBrewingDataFuel = 1;

// Menu slot -> player inventory slot, or -1 for the crafting result/grid
// (which are backed by the menu's own 5-slot craft area, not the inventory).
int menu_slot_to_inventory_slot(int menu_slot);
// Player inventory slot -> menu slot, or -1 when out of range.
int inventory_slot_to_menu_slot(int inv_slot);

// Armour menu slot (5..8, head-first) -> inventory slot (39..36), or -1.
int armor_menu_slot_to_inventory_slot(int menu_slot);

// A fixed-size container of stacks. Out-of-range access is a no-op rather than
// undefined: every caller here works from network input.
class Inventory {
public:
  explicit Inventory(int size) : slots_(static_cast<size_t>(size > 0 ? size : 0)) {}

  int size() const {
    return static_cast<int>(slots_.size());
  }
  bool valid(int slot) const {
    return slot >= 0 && slot < size();
  }
  const ItemStack& get(int slot) const;
  void set(int slot, const ItemStack& stack);
  void set_empty(int slot) {
    set(slot, ItemStack{});
  }
  void clear_all();
  bool empty() const;

  // Vanilla's Inventory.add(): the *selected* slot is preferred when it already
  // holds this item and has room, then any stack with room, then the first
  // empty slot scanning from slot 0 (hotbar first). Returns how many items
  // could not be placed (0 when everything fit).
  int add(const ItemStack& stack, int selected = 0);
  // Merges into matching stacks inside [first, first + count), then fills empty
  // slots in that window. Used by container fills. Returns the remainder.
  int add_in_range(int first, int count, const ItemStack& stack);

  // Index of a slot holding this item with room for more, or -1.
  int slot_with_remaining_space(const ItemStack& stack) const;

  // Index of the first slot holding an identical stack, or -1.
  int find(const ItemStack& stack) const;
  // Index of the first empty slot in [first, first + count), or -1.
  int first_empty_in_range(int first, int count) const;

  std::vector<ItemStack>& slots() {
    return slots_;
  }
  const std::vector<ItemStack>& slots() const {
    return slots_;
  }

private:
  std::vector<ItemStack> slots_;
};

} // namespace mc::world
