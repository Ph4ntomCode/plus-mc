// SPDX-License-Identifier: GPL-3.0-or-later
//
// The crafting recipe engine: which recipe a crafting grid makes, and what it
// costs. The data lives in the generated table (world/recipe_table.h); this is
// the matcher over it.
//
// The semantics are copied from vanilla rather than invented, because "which
// recipe wins" is observable:
//
//  * The grid is *trimmed to the bounding box of its non-empty cells* before
//    anything else (CraftingInput.ofPositioned). That is why a shaped recipe
//    matches wherever it is placed in the table, and why its stored width and
//    height are the trimmed ones.
//  * A shaped recipe needs the trimmed grid to have the pattern's dimensions
//    (ShapedRecipePattern.matches), the same number of non-empty cells as the
//    pattern has *filled* cells (the gaps in "# #"/"###" do not count), and
//    every cell to match — empty cells must be really empty. Vanilla tries the
//    mirrored pattern first and then the pattern as written; a symmetrical
//    pattern is identical either way, so trying both is equivalent.
//  * A shapeless recipe needs exactly as many non-empty cells as it has
//    ingredients and a perfect assignment of ingredients to cells
//    (ShapelessRecipe.matches -> StackedItemContents.canCraft, a bipartite
//    matching). Each cell counts as one unit, because CraftingInput accounts
//    every stack with a cap of 1.
//  * A transmute recipe takes exactly one `input` stack plus between
//    material_min and material_max `material` stacks and nothing else, and it
//    refuses to produce the input itself when the result would be a single item
//    (a no-op transmute).
//  * Recipes are searched in table order, which is name order, and the first
//    match wins. Vanilla's own order is its recipe-map order, which is not
//    portable; when two recipes can match the same grid they almost always
//    produce the same item, and the header records the rest.
//
// `crafting_special_*` (banner copying, fireworks, map extending, shield
// decoration, repair, ...) are code-driven in vanilla. They are in the table as
// RecipeKind::Special and are deliberately never matched here, so a grid they
// would have handled simply does not craft instead of crafting something wrong.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "world/item.h"
#include "world/recipe_table.h"

namespace mc::world {

// A crafting input the way vanilla sees it: a width x height grid of stacks,
// row-major. The player's 2x2 and the crafting table's 3x3 are both this.
struct CraftingGrid {
  int width = 0;
  int height = 0;
  std::vector<ItemStack> items;

  static CraftingGrid of(int width, int height);
  const ItemStack& at(int x, int y) const {
    return items[static_cast<size_t>(x + y * width)];
  }
  // Non-empty cells, i.e. vanilla's CraftingInput.ingredientCount().
  int non_empty_count() const;
  bool empty() const {
    return non_empty_count() == 0;
  }
};

// The grid shaved down to the bounding box of its non-empty cells, which is the
// space a shaped recipe is matched in. Returns a 0x0 grid when everything is
// empty.
CraftingGrid trim_crafting_grid(const CraftingGrid& grid);

// Which recipe a grid makes. `result_count` is usually the recipe's own count;
// a transmute recipe with `add_material_count_to_result` makes base + materials.
struct CraftingMatch {
  const GeneratedRecipe* recipe = nullptr;
  int result_count = 1;
};

// The recipe a grid crafts, or nullopt. Only Shaped, Shapeless and Transmute
// can match (see the header comment).
std::optional<CraftingMatch> find_crafting_recipe(const CraftingGrid& grid);

// True when the ingredient accepts this item id. An ingredient with item_count
// == 0 is a shaped pattern's *empty* cell and accepts nothing.
bool ingredient_accepts(const GeneratedIngredient& ingredient, int32_t item);

// The item ids an ingredient accepts, in table order. Empty for an empty cell.
std::vector<int32_t> ingredient_items(const GeneratedIngredient& ingredient);

// ---- smithing (Phase 3 milestone D2) ---------------------------------------
//
// A smithing recipe's three cells are the template, the base and the addition,
// in that order (SmithingRecipeInput), and the smithing menu's input slots are
// gated on being *any* recipe's item for that cell — vanilla builds those gates
// as the RecipePropertySets SMITHING_TEMPLATE / SMITHING_BASE / SMITHING_ADDITION.
// We derive them from the recipes themselves, which is what vanilla does too.
enum class SmithingSlot : int { Template = 0, Base = 1, Addition = 2 };

// Whether `item` may go in that smithing input slot at all.
bool is_smithing_slot_item(SmithingSlot slot, int32_t item);

// The smithing_transform recipe these three inputs make, or nullopt. A
// smithing_trim is not matched here — see find_smithing_trim_recipe — because
// its result is the base stack with a trim component rather than a fixed item,
// and only the caller holds the base stack.
std::optional<CraftingMatch> find_smithing_recipe(const ItemStack& template_stack,
                                                  const ItemStack& base, const ItemStack& addition);

// The smithing_trim recipe these three inputs make, or nullopt
// (`SmithingTrimRecipe.matches`). `material` is the material the addition item
// *provides* and `pattern` the pattern the template item names (world/trim.h);
// the result stack is the caller's to assemble, because the base's other
// components ride along into it (`applyTrim` copies the base and sets the trim).
//
// One triple can never match both families: a trim's template is its pattern's
// template item and a transform's is the netherite upgrade template, and the two
// sets are disjoint. So the order the caller asks in is unobservable.
struct SmithingTrimMatch {
  const GeneratedRecipe* recipe = nullptr;
  int32_t material = -1;
  int32_t pattern = -1;
};

std::optional<SmithingTrimMatch> find_smithing_trim_recipe(const ItemStack& template_stack,
                                                           const ItemStack& base,
                                                           const ItemStack& addition);

// ---- stonecutting ----------------------------------------------------------
//
// Every stonecutting recipe in table order, which is the order `update_recipes`
// carries and the order `stonecutting_recipes_for` filters, so the index a
// client sends back means the same recipe on both sides.
std::vector<const GeneratedRecipe*> stonecutting_recipes();

// A stonecutting recipe has one input and a fixed result. The *client* owns the
// stonecutter's recipe list: it filters the list `update_recipes` carried by the
// input item, and sends back an index into its filtered list. So the server must
// filter the same list the same way, in the same order, or the index selects a
// different recipe — that is what `stonecutting_recipes_for` is.
std::vector<const GeneratedRecipe*> stonecutting_recipes_for(int32_t item);

// Whether any stonecutter recipe accepts `item` (the input slot's gate).
bool is_stonecutter_input(int32_t item);

// The item ids a smithing input slot accepts, in table order. This is the
// RecipePropertySet the client is sent so its own slots can refuse a wrong item
// before the click is ever sent.
std::vector<int32_t> smithing_slot_items(SmithingSlot slot);

// The recipe with this file name ("oak_planks"), or nullptr.
const GeneratedRecipe* recipe_by_name(std::string_view name);

// The recipe's cells: ing_count entries, row-major for a shaped recipe.
const GeneratedIngredient* recipe_cells(const GeneratedRecipe& recipe);

} // namespace mc::world
