// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cooking (Phase 3 milestone E): the furnace family's fuel table, its recipe
// lookup, and the smelting tick itself.
//
// Everything here is copied from vanilla rather than invented, because each
// piece is observable:
//
//  * **Fuel is code, not data.** `FuelValues.vanillaBurnTimes` builds the table
//    from items and item tags, all multiples of a 200-tick base unit;
//    `AbstractFurnaceBlockEntity.getBurnDuration` is a plain lookup in it. The
//    generated `fuel_table.h` is that method transcribed (see
//    tools/gen_fuel_table.py for why the order matters).
//  * **A cooking recipe is picked by its single input**, in table order
//    (`RecipeManager.getRecipeFor` -> the recipe map). Its own `cookingtime` is
//    the total cook time (200 for smelting, 100 for blasting/smoking), not a
//    per-family constant — which is why the total is derived from the matched
//    recipe.
//  * **The tick is `AbstractFurnaceBlockEntity.serverTick`**, including the
//    details that are easy to get wrong: the unlit branch decays progress by
//    *two* per tick rather than resetting it, the output slot has to accept the
//    whole result stack (`canBurn`), the total cook time is re-derived when the
//    input *item* changes (`setItem`), and a wet sponge burned over a bucket
//    becomes a water bucket (`burn`).
//
// The `lit` block state and the client syncs are the caller's job: the tick
// only touches one container's three slots and its four counters, and reports
// what moved.
#pragma once

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include "world/container.h"
#include "world/inventory.h"
#include "world/recipe.h"
#include "world/recipe_table.h"

namespace mc::world {

// ---- fuel ------------------------------------------------------------------

// Ticks one unit of `item` burns for, or 0 when it is not a fuel
// (FuelValues.burnDuration). The vanilla table has no zero-time fuels, so 0
// unambiguously means "not a fuel" and `is_fuel` is just `> 0`.
int fuel_burn_ticks(int32_t item);

// Whether the fuel slot accepts this item (FuelValues.isFuel).
bool is_fuel(int32_t item);

// The item left behind when one unit of `item` is consumed
// (Item.getCraftingRemainder), or kAirItemId for none.
//
// This is the general item-level lookup (world/item.h): a lava bucket burns
// down to a bucket, which is the one remainder the furnace family can observe,
// because it is the only fuel that has one.
int32_t fuel_remainder_item(int32_t item);

// The fuel slot's stack limit: 1 for a bucket or a lava bucket, the item's
// usual size otherwise (FurnaceFuelSlot.getMaxStackSize).
int fuel_slot_max_stack(int32_t item, int item_max_stack);

// Whether this is a bucket or a lava bucket — the two items vanilla lets into
// the fuel slot even when they are not fuel, so a lava-bucket cycle never
// destroys the bucket (FurnaceFuelSlot.isBucket plus the bucket branch of
// AbstractFurnaceBlockEntity.canPlaceItem).
bool is_bucket_item(int32_t item);

// ---- cooking recipes -------------------------------------------------------

// The RecipeKind a block cooks with (FurnaceBlockEntity -> SMELTING,
// BlastFurnaceBlockEntity -> BLASTING, SmokerBlockEntity -> SMOKING), or
// Special for a block that does not cook.
RecipeKind cooking_kind_for_block(std::string_view block);

// True for the three blocks that cook.
bool is_cooking_block(std::string_view block);

// The cooking recipe `item` makes in this family, or nullptr. First match in
// table order, which is also the order `update_recipes` and the recipe book
// list the family in.
const GeneratedRecipe* find_cooking_recipe(RecipeKind kind, int32_t item);

// The cook time for `item` in this family: the matched recipe's `cookingtime`,
// or 200 when nothing matches (AbstractFurnaceBlockEntity.getTotalCookTime's
// `orElse(200)`). Vanilla stores that into `cooking_total_time` as soon as the
// input slot changes, so the progress bar has a denominator before the first
// craft.
int cooking_total_time(RecipeKind kind, int32_t item);

// The experience one craft of this recipe pays out, in thousandths
// (AbstractCookingRecipe.experience(), which the table keeps as milli-xp).
int cooking_experience_milli(const GeneratedRecipe& recipe);

// ---- the smelting tick -----------------------------------------------------
//
// The state itself is `FurnaceState` in container.h, because it is part of a
// BlockContainer and has to persist with it.

// What one tick did, so the caller can sync the clients and persist.
struct FurnaceTickResult {
  bool changed = false;       // anything moved at all: mark the chunk dirty
  bool data_changed = false;  // one of the four counters moved: set_data
  bool items_changed = false; // the three slots moved: slot updates
  bool lit_changed = false;   // the `lit` block property must be rewritten
  bool lit = false;           // the new lit state (meaningful when changed)
};

// One tick of `AbstractFurnaceBlockEntity.serverTick` over a 3-slot furnace:
// slot 0 the input, 1 the fuel, 2 the output. `state` advances in place.
FurnaceTickResult tick_furnace(RecipeKind kind, FurnaceState& state, Inventory& slots);

// The experience a furnace owes for what it has cooked, paired with each
// recipe's craft count (vanilla's getRecipesToAwardAndPopExperience). The
// awards are also removed from the state, which is `recipesUsed.clear()`.
struct CookingAward {
  const GeneratedRecipe* recipe = nullptr;
  int crafts = 0;
  int experience = 0;
};
// `random_01` is drawn once per entry with a fractional part, matching
// `createExperience`'s single `level.getRandom().nextFloat()` per recipe.
std::vector<CookingAward> take_cooking_awards(FurnaceState& state,
                                              const std::function<double()>& random_01);

} // namespace mc::world
