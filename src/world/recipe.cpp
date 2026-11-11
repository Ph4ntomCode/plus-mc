// SPDX-License-Identifier: GPL-3.0-or-later
//
// The crafting recipe matcher. See recipe.h for why each rule is what it is.
#include "world/recipe.h"

#include <algorithm>
#include <cstring>

#include "world/trim.h"

namespace mc::world {
namespace {

// A cell of a shaped pattern: `item_count == 0` means the grid must leave it
// empty (Ingredient.testOptionalIngredient with an empty Optional).
bool cell_accepts(const GeneratedIngredient& cell, const ItemStack& stack) {
  if (cell.item_count == 0)
    return stack.empty();
  if (stack.empty())
    return false;
  return ingredient_accepts(cell, stack.item);
}

// The number of *filled* cells in a shaped pattern. The table's ing_count is
// the box area (width * height) because that is what indexes the cells; vanilla
// compares against `ingredientCount`, which is the number of non-empty
// ingredients (`ingredients.stream().flatMap(Optional::stream).count()`), so the
// gap in "# #"/"###" is not one of them.
int filled_cells(const GeneratedRecipe& recipe) {
  const GeneratedIngredient* cells = recipe_cells(recipe);
  int n = 0;
  for (int i = 0; i < recipe.ing_count; ++i)
    if (cells[i].item_count != 0)
      ++n;
  return n;
}

bool shaped_matches(const GeneratedRecipe& recipe, const CraftingGrid& grid) {
  if (grid.width != recipe.width || grid.height != recipe.height)
    return false;
  if (grid.non_empty_count() != filled_cells(recipe))
    return false;
  const GeneratedIngredient* cells = recipe_cells(recipe);
  // Mirrored first, then as written (ShapedRecipePattern.matches with xFlip).
  for (int mirror = 1; mirror >= 0; --mirror) {
    bool ok = true;
    for (int y = 0; y < recipe.height && ok; ++y) {
      for (int x = 0; x < recipe.width; ++x) {
        const int cx = mirror ? recipe.width - 1 - x : x;
        if (!cell_accepts(cells[cx + y * recipe.width], grid.at(x, y))) {
          ok = false;
          break;
        }
      }
    }
    if (ok)
      return true;
  }
  return false;
}

// Kuhn's algorithm over "ingredient -> non-empty cell". Vanilla solves the same
// assignment as a flow problem on item counts (StackedItemContents.tryPick),
// which is equivalent here: every cell contributes exactly one unit because
// CraftingInput accounts each stack with a cap of 1, and cells holding the same
// item are interchangeable.
bool augment(const GeneratedIngredient* cells, const int ingredients,
             const std::vector<int32_t>& slot_items, int ingredient, std::vector<char>* visited,
             std::vector<int>* owner) {
  for (size_t slot = 0; slot < slot_items.size(); ++slot) {
    if ((*visited)[slot] || !ingredient_accepts(cells[ingredient], slot_items[slot]))
      continue;
    (*visited)[slot] = 1;
    if ((*owner)[slot] < 0 ||
        augment(cells, ingredients, slot_items, (*owner)[slot], visited, owner)) {
      (*owner)[slot] = ingredient;
      return true;
    }
  }
  return false;
}

bool shapeless_matches(const GeneratedRecipe& recipe, const CraftingGrid& grid) {
  const int ingredients = static_cast<int>(recipe.ing_count);
  if (grid.non_empty_count() != ingredients)
    return false;
  const GeneratedIngredient* cells = recipe_cells(recipe);
  // Vanilla's fast path for a one-cell grid: a single ingredient tests the slot
  // directly, which also covers the "one ingredient, one stack" case without a
  // matching pass at all.
  if (grid.items.size() == 1 && ingredients == 1)
    return cell_accepts(cells[0], grid.items[0]);

  std::vector<int32_t> slot_items;
  for (const ItemStack& stack : grid.items)
    if (!stack.empty())
      slot_items.push_back(stack.item);

  std::vector<int> owner(slot_items.size(), -1);
  for (int i = 0; i < ingredients; ++i) {
    std::vector<char> visited(slot_items.size(), 0);
    if (!augment(cells, ingredients, slot_items, i, &visited, &owner))
      return false;
  }
  return true;
}

// The material bounds; the top bit of material_max is the
// `add_material_count_to_result` flag, not a bound.
int transmute_max_materials(const GeneratedRecipe& recipe) {
  return recipe.material_max & 0x7F;
}

// The whole grid must be one `input` stack plus lo..hi `material` stacks. Returns
// the result count, or nullopt.
std::optional<int> transmute_matches(const GeneratedRecipe& recipe, const CraftingGrid& grid) {
  const int lo = recipe.material_min;
  const int hi = transmute_max_materials(recipe);
  const int filled = grid.non_empty_count();
  if (filled < lo + 1 || filled > hi + 1)
    return std::nullopt;
  const GeneratedIngredient* cells = recipe_cells(recipe);
  int input_slot = -1;
  int materials = 0;
  for (size_t i = 0; i < grid.items.size(); ++i) {
    const ItemStack& stack = grid.items[i];
    if (stack.empty())
      continue;
    if (ingredient_accepts(cells[0], stack.item)) {
      if (input_slot >= 0)
        return std::nullopt; // two inputs is not a transmute
      input_slot = static_cast<int>(i);
      continue;
    }
    if (ingredient_accepts(cells[1], stack.item)) {
      if (++materials > hi)
        return std::nullopt;
      continue;
    }
    return std::nullopt; // something that is neither input nor material
  }
  if (input_slot < 0 || materials < lo)
    return std::nullopt;
  const bool count_from_materials = (recipe.material_max & 0x80) != 0;
  const int result_count =
      count_from_materials ? materials + recipe.result_count : recipe.result_count;
  // A single-item result identical to the input is a no-op, and vanilla refuses
  // it (TransmuteRecipe.matches ends with !isSameItemSameComponents(input, result)).
  if (result_count == 1 && grid.items[input_slot].item == recipe.result_item)
    return std::nullopt;
  return result_count;
}

// Smithing cells are always exactly three: template, base, addition.
bool smithing_recipe_cells(const GeneratedRecipe& recipe) {
  return (recipe.kind == RecipeKind::SmithingTransform ||
          recipe.kind == RecipeKind::SmithingTrim) &&
         recipe.ing_count == 3;
}

} // namespace

bool is_smithing_slot_item(SmithingSlot slot, int32_t item) {
  if (item <= 0)
    return false;
  const int cell = static_cast<int>(slot);
  for (uint32_t i = 0; i < kRecipeCount; ++i) {
    const GeneratedRecipe& recipe = kRecipes[i];
    if (!smithing_recipe_cells(recipe))
      continue;
    if (ingredient_accepts(recipe_cells(recipe)[cell], item))
      return true;
  }
  return false;
}

std::vector<int32_t> smithing_slot_items(SmithingSlot slot) {
  const int cell = static_cast<int>(slot);
  std::vector<int32_t> out;
  for (uint32_t i = 0; i < kRecipeCount; ++i) {
    const GeneratedRecipe& recipe = kRecipes[i];
    if (!smithing_recipe_cells(recipe))
      continue;
    for (int32_t id : ingredient_items(recipe_cells(recipe)[cell])) {
      if (std::find(out.begin(), out.end(), id) == out.end())
        out.push_back(id);
    }
  }
  return out;
}

std::vector<const GeneratedRecipe*> stonecutting_recipes() {
  std::vector<const GeneratedRecipe*> out;
  for (uint32_t i = 0; i < kRecipeCount; ++i) {
    const GeneratedRecipe& recipe = kRecipes[i];
    if (recipe.kind == RecipeKind::Stonecutting && recipe.ing_count == 1)
      out.push_back(&recipe);
  }
  return out;
}

std::vector<const GeneratedRecipe*> stonecutting_recipes_for(int32_t item) {
  std::vector<const GeneratedRecipe*> out;
  if (item <= 0)
    return out;
  for (const GeneratedRecipe* recipe : stonecutting_recipes()) {
    if (ingredient_accepts(recipe_cells(*recipe)[0], item))
      out.push_back(recipe);
  }
  return out;
}

bool is_stonecutter_input(int32_t item) {
  if (item <= 0)
    return false;
  for (uint32_t i = 0; i < kRecipeCount; ++i) {
    const GeneratedRecipe& recipe = kRecipes[i];
    if (recipe.kind == RecipeKind::Stonecutting && recipe.ing_count == 1 &&
        ingredient_accepts(recipe_cells(recipe)[0], item))
      return true;
  }
  return false;
}

std::optional<CraftingMatch> find_smithing_recipe(const ItemStack& template_stack,
                                                  const ItemStack& base,
                                                  const ItemStack& addition) {
  // SmithingRecipeInput requires all three, and a smithing_trim has no result we
  // can express, so both are refused up front.
  if (template_stack.empty() || base.empty() || addition.empty())
    return std::nullopt;
  for (uint32_t i = 0; i < kRecipeCount; ++i) {
    const GeneratedRecipe& recipe = kRecipes[i];
    if (recipe.kind != RecipeKind::SmithingTransform || recipe.ing_count != 3)
      continue;
    const GeneratedIngredient* cells = recipe_cells(recipe);
    if (cells[0].item_count == 0 || cells[1].item_count == 0 || cells[2].item_count == 0)
      continue;
    if (!ingredient_accepts(cells[0], template_stack.item) ||
        !ingredient_accepts(cells[1], base.item) || !ingredient_accepts(cells[2], addition.item))
      continue;
    return CraftingMatch{&recipe, recipe.result_count};
  }
  return std::nullopt;
}

std::optional<SmithingTrimMatch> find_smithing_trim_recipe(const ItemStack& template_stack,
                                                           const ItemStack& base,
                                                           const ItemStack& addition) {
  // `SmithingRecipe.matches` tests each ingredient against its stack, and an
  // empty stack is in no ingredient's item set — so an empty input is no match.
  if (template_stack.empty() || base.empty() || addition.empty())
    return std::nullopt;
  // `applyTrim` reads the material off the addition stack and returns an empty
  // result when it has none: resolving it here is what keeps a matched recipe
  // from claiming a material it does not provide.
  const int32_t material = trim_material_for_item(addition.item);
  if (material < 0)
    return std::nullopt;
  // The pattern rides the template item rather than the recipe table (see
  // world/trim.h): the generator pins the two to each other, so a recipe that
  // matches here is that pattern's recipe.
  const int32_t pattern = trim_pattern_for_template(template_stack.item);
  if (pattern < 0)
    return std::nullopt;
  for (uint32_t i = 0; i < kRecipeCount; ++i) {
    const GeneratedRecipe& recipe = kRecipes[i];
    if (recipe.kind != RecipeKind::SmithingTrim || recipe.ing_count != 3)
      continue;
    const GeneratedIngredient* cells = recipe_cells(recipe);
    if (cells[0].item_count == 0 || cells[1].item_count == 0 || cells[2].item_count == 0)
      continue;
    if (!ingredient_accepts(cells[0], template_stack.item) ||
        !ingredient_accepts(cells[1], base.item) || !ingredient_accepts(cells[2], addition.item))
      continue;
    return SmithingTrimMatch{&recipe, material, pattern};
  }
  return std::nullopt;
}

CraftingGrid CraftingGrid::of(int width, int height) {
  CraftingGrid grid;
  grid.width = width;
  grid.height = height;
  grid.items.assign(static_cast<size_t>(std::max(0, width * height)), ItemStack{});
  return grid;
}

int CraftingGrid::non_empty_count() const {
  int n = 0;
  for (const ItemStack& stack : items)
    if (!stack.empty())
      ++n;
  return n;
}

CraftingGrid trim_crafting_grid(const CraftingGrid& grid) {
  if (grid.width <= 0 || grid.height <= 0)
    return {};
  int left = grid.width - 1, right = 0, top = grid.height - 1, bottom = 0;
  bool any = false;
  for (int y = 0; y < grid.height; ++y) {
    for (int x = 0; x < grid.width; ++x) {
      if (grid.at(x, y).empty())
        continue;
      left = std::min(left, x);
      right = std::max(right, x);
      top = std::min(top, y);
      bottom = std::max(bottom, y);
      any = true;
    }
  }
  if (!any)
    return {};
  CraftingGrid trimmed = CraftingGrid::of(right - left + 1, bottom - top + 1);
  for (int y = 0; y < trimmed.height; ++y)
    for (int x = 0; x < trimmed.width; ++x)
      trimmed.items[static_cast<size_t>(x + y * trimmed.width)] = grid.at(x + left, y + top);
  return trimmed;
}

const GeneratedIngredient* recipe_cells(const GeneratedRecipe& recipe) {
  return &kRecipeIngredients[recipe.first_ing];
}

bool ingredient_accepts(const GeneratedIngredient& ingredient, int32_t item) {
  if (ingredient.item_count == 0 || item <= 0)
    return false;
  const uint16_t want = static_cast<uint16_t>(item);
  const uint16_t* items = &kRecipeItems[ingredient.first_item];
  for (uint16_t i = 0; i < ingredient.item_count; ++i)
    if (items[i] == want)
      return true;
  return false;
}

std::vector<int32_t> ingredient_items(const GeneratedIngredient& ingredient) {
  std::vector<int32_t> out;
  out.reserve(ingredient.item_count);
  for (uint16_t i = 0; i < ingredient.item_count; ++i)
    out.push_back(kRecipeItems[ingredient.first_item + i]);
  return out;
}

const GeneratedRecipe* recipe_by_name(std::string_view name) {
  for (uint32_t i = 0; i < kRecipeCount; ++i)
    if (name == kRecipes[i].name)
      return &kRecipes[i];
  return nullptr;
}

std::optional<CraftingMatch> find_crafting_recipe(const CraftingGrid& raw) {
  const CraftingGrid grid = trim_crafting_grid(raw);
  if (grid.empty())
    return std::nullopt;
  for (uint32_t i = 0; i < kRecipeCount; ++i) {
    const GeneratedRecipe& recipe = kRecipes[i];
    switch (recipe.kind) {
    case RecipeKind::Shaped:
      if (shaped_matches(recipe, grid))
        return CraftingMatch{&recipe, recipe.result_count};
      break;
    case RecipeKind::Shapeless:
      if (shapeless_matches(recipe, grid))
        return CraftingMatch{&recipe, recipe.result_count};
      break;
    case RecipeKind::Transmute:
      if (const auto count = transmute_matches(recipe, grid))
        return CraftingMatch{&recipe, *count};
      break;
    default:
      break; // unimplemented kinds never match
    }
  }
  return std::nullopt;
}

} // namespace mc::world
