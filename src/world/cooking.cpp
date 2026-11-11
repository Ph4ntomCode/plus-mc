// SPDX-License-Identifier: GPL-3.0-or-later
//
// The cooking module: see cooking.h for why each rule is what it is.
#include "world/cooking.h"

#include <algorithm>
#include <cmath>

#include "world/fuel_table.h"
#include "world/item.h"

namespace mc::world {
namespace {

int32_t item_named(const char* name) {
  return ItemRegistry::instance().id_of(name).value_or(kAirItemId);
}

int32_t bucket_item() {
  return item_named("minecraft:bucket");
}

int32_t lava_bucket_item() {
  return item_named("minecraft:lava_bucket");
}

} // namespace

int fuel_burn_ticks(int32_t item) {
  // The generated table is sorted by item id, so this is a plain binary search.
  int lo = 0, hi = kFuelCount - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    if (kFuels[mid].item == item)
      return kFuels[mid].ticks;
    if (kFuels[mid].item < item)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return 0;
}

bool is_fuel(int32_t item) {
  return fuel_burn_ticks(item) > 0;
}

int32_t fuel_remainder_item(int32_t item) {
  // The same `Item.getCraftingRemainder` a brew consults (world/item.h) — a
  // lava bucket is the one fuel that has one, and no other item with one is a
  // fuel, so the lookup is the whole answer here too.
  return crafting_remainder_item(item);
}

int fuel_slot_max_stack(int32_t item, int item_max_stack) {
  return is_bucket_item(item) ? 1 : item_max_stack;
}

bool is_bucket_item(int32_t item) {
  return item == bucket_item() || item == lava_bucket_item();
}

RecipeKind cooking_kind_for_block(std::string_view block) {
  if (block == "minecraft:furnace")
    return RecipeKind::Smelting;
  if (block == "minecraft:blast_furnace")
    return RecipeKind::Blasting;
  if (block == "minecraft:smoker")
    return RecipeKind::Smoking;
  return RecipeKind::Special;
}

bool is_cooking_block(std::string_view block) {
  return cooking_kind_for_block(block) != RecipeKind::Special;
}

const GeneratedRecipe* find_cooking_recipe(RecipeKind kind, int32_t item) {
  if (item == kAirItemId)
    return nullptr;
  for (uint32_t i = 0; i < kRecipeCount; ++i) {
    const GeneratedRecipe& recipe = kRecipes[i];
    if (recipe.kind != kind || recipe.ing_count != 1)
      continue;
    if (ingredient_accepts(recipe_cells(recipe)[0], item))
      return &recipe;
  }
  return nullptr;
}

int cooking_total_time(RecipeKind kind, int32_t item) {
  const GeneratedRecipe* recipe = find_cooking_recipe(kind, item);
  return recipe ? recipe->cook_time : 200;
}

int cooking_experience_milli(const GeneratedRecipe& recipe) {
  return recipe.xp_milli;
}

FurnaceTickResult tick_furnace(RecipeKind kind, FurnaceState& state, Inventory& slots) {
  FurnaceTickResult result;
  if (slots.size() < 3)
    return result;

  const int lit_before = state.lit_time_remaining;
  const int timer_before = state.cooking_timer;
  const int total_before = state.cooking_total_time;

  // setItem(0): a different input item re-derives the total cook time and
  // resets progress *before* anything cooks. Vanilla performs this on the write
  // (AbstractFurnaceBlockEntity.setItem), so by the time its tick runs the
  // denominator is already the new recipe's; doing it here first is the same
  // state at the same point in the tick.
  {
    const ItemStack& input = slots.get(0);
    if (input.item != state.last_input_item) {
      state.last_input_item = input.item;
      state.cooking_total_time = cooking_total_time(kind, input.item);
      state.cooking_timer = 0;
    }
  }

  // serverTick, in vanilla's order: age the fuel first, then decide from the
  // *new* lit state.
  const bool was_lit = state.lit_time_remaining > 0;
  bool is_lit = was_lit;
  if (was_lit) {
    --state.lit_time_remaining;
    is_lit = state.lit_time_remaining > 0;
  }

  const ItemStack& ingredient = slots.get(0);
  const ItemStack& fuel = slots.get(1);
  const bool has_ingredient = !ingredient.empty();
  const bool has_fuel = !fuel.empty();

  if (is_lit || (has_fuel && has_ingredient)) {
    if (has_ingredient) {
      const GeneratedRecipe* recipe = find_cooking_recipe(kind, ingredient.item);
      bool burned = false;
      if (recipe != nullptr && recipe->result_count > 0) {
        ItemStack burn_result;
        burn_result.item = recipe->result_item;
        burn_result.count = recipe->result_count;

        // canBurn: an empty output, or the same stack with room for the whole
        // result. The cap is min(the container's 64, the result's own max).
        const ItemStack& output = slots.get(2);
        const int cap = std::min(64, burn_result.max_stack_size());
        const bool can_burn = output.empty() || (output.item == burn_result.item &&
                                                 output.damage == burn_result.damage &&
                                                 output.count + burn_result.count <= cap);
        if (can_burn) {
          if (!is_lit) {
            const int burn = fuel_burn_ticks(fuel.item);
            if (burn > 0) {
              state.lit_time_remaining = burn;
              state.lit_total_time = burn;
              // consumeFuel: one unit of fuel, replaced by its remainder when
              // the slot empties.
              ItemStack left = fuel;
              left.count -= 1;
              if (left.count <= 0) {
                const int32_t remainder = fuel_remainder_item(fuel.item);
                left.clear();
                if (remainder != kAirItemId) {
                  left.item = remainder;
                  left.count = 1;
                }
              }
              slots.set(1, left);
              is_lit = true;
            } else {
              state.lit_total_time = 0;
            }
          }
          if (is_lit) {
            ++state.cooking_timer;
            if (state.cooking_timer == state.cooking_total_time) {
              state.cooking_timer = 0;
              state.cooking_total_time = recipe->cook_time;
              burned = true;
            }
          } else {
            state.cooking_timer = 0;
          }
        } else {
          state.cooking_timer = 0;
        }
      } else {
        state.cooking_timer = 0;
      }
      if (burned) {
        // burn(): grow the output, then the wet-sponge quirk, then consume one
        // input item.
        ItemStack out = slots.get(2);
        if (out.empty()) {
          out.item = recipe->result_item;
          out.count = recipe->result_count;
        } else {
          out.count += recipe->result_count;
        }
        slots.set(2, out);
        if (ingredient.item == item_named("minecraft:wet_sponge") && !slots.get(1).empty() &&
            is_bucket_item(slots.get(1).item)) {
          ItemStack water;
          water.item = item_named("minecraft:water_bucket");
          water.count = 1;
          slots.set(1, water);
        }
        ItemStack in = ingredient;
        in.count -= 1;
        if (in.count <= 0)
          in.clear();
        slots.set(0, in);
        // setRecipeUsed: one craft of this recipe, paid out when taken.
        if (recipe->name != nullptr && *recipe->name != '\0')
          state.recipes_used[recipe->name] += 1;
        // The input slot just changed item count; the item itself is the same,
        // so the total cook time stands (vanilla's `same` test is item+components
        // only, not the count).
      }
    } else {
      state.cooking_timer = 0;
    }
  } else if (state.cooking_timer > 0) {
    // Unlit with nothing to cook: progress decays by two per tick
    // (Mth.clamp(cookingTimer - 2, 0, cookingTotalTime)), it does not reset.
    state.cooking_timer = std::max(0, std::min(state.cooking_timer - 2, state.cooking_total_time));
  }

  result.lit = is_lit;
  result.lit_changed = was_lit != is_lit;
  result.data_changed = state.lit_time_remaining != lit_before ||
                        state.cooking_timer != timer_before ||
                        state.cooking_total_time != total_before;
  result.items_changed = false; // filled in by the caller's slot comparison
  result.changed = result.data_changed || result.lit_changed;
  return result;
}

std::vector<CookingAward> take_cooking_awards(FurnaceState& state,
                                              const std::function<double()>& random_01) {
  std::vector<CookingAward> awards;
  for (const auto& entry : state.recipes_used) {
    const std::string& name = entry.first;
    const int crafts = entry.second;
    const GeneratedRecipe* recipe = recipe_by_name(name);
    if (recipe == nullptr || crafts <= 0)
      continue;
    // createExperience: floor(crafts * xp), plus one when the fraction beats a
    // single random draw.
    const double exact =
        static_cast<double>(crafts) * (static_cast<double>(recipe->xp_milli) / 1000.0);
    int reward = static_cast<int>(std::floor(exact));
    const double fraction = exact - static_cast<double>(reward);
    if (fraction != 0.0 && random_01() < fraction)
      ++reward;
    awards.push_back(CookingAward{recipe, crafts, reward});
  }
  state.recipes_used.clear();
  return awards;
}

} // namespace mc::world
