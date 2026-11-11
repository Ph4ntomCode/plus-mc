// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the cooking layer (Phase 3 milestone E):
//   * the fuel table and the fuel slot's rules (FuelValues / FurnaceFuelSlot),
//   * cooking recipe lookup by input item (RecipeManager.getRecipeFor),
//   * AbstractFurnaceBlockEntity.serverTick: fuel consumption, the flame and
//     progress counters, the canBurn gate, the two-per-tick unlit decay, the
//     wet-sponge quirk and RecipesUsed,
//   * the furnace block entity's NBT round-trip,
//   * the experience a furnace owes (createExperience) and the recipe book's
//     placement into the input slot (AbstractFurnaceMenu.handlePlacement).
#include <cstdio>
#include <string>
#include <vector>

#include "world/container.h"
#include "world/cooking.h"
#include "world/inventory.h"
#include "world/item.h"
#include "world/item_registry.h"
#include "world/nbt.h"
#include "world/recipe_book.h"

using namespace mc::world;

static int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

namespace {

int32_t item(const char* name) {
  auto id = ItemRegistry::instance().id_of(name);
  return id ? *id : -1;
}

// A furnace's three slots, in vanilla's order: input, fuel, result.
Inventory furnace_slots(const char* in, int in_n, const char* fuel, int fuel_n) {
  Inventory slots{3};
  if (in != nullptr && in_n > 0)
    slots.set(0, ItemStack::of_id(item(in), in_n));
  if (fuel != nullptr && fuel_n > 0)
    slots.set(1, ItemStack::of_id(item(fuel), fuel_n));
  return slots;
}

void test_fuel_table() {
  // FuelValues.vanillaBurnTimes: all multiples of the 200-tick base unit,
  // except the two 100-tick items (a stick and a sapling's worth).
  CHECK(fuel_burn_ticks(item("minecraft:coal")) == 1600);
  CHECK(fuel_burn_ticks(item("minecraft:charcoal")) == 1600);
  CHECK(fuel_burn_ticks(item("minecraft:coal_block")) == 16000);
  CHECK(fuel_burn_ticks(item("minecraft:oak_planks")) == 300);
  CHECK(fuel_burn_ticks(item("minecraft:stick")) == 100);
  CHECK(fuel_burn_ticks(item("minecraft:blaze_rod")) == 2400);
  CHECK(fuel_burn_ticks(item("minecraft:lava_bucket")) == 20000);
  CHECK(fuel_burn_ticks(item("minecraft:iron_ingot")) == 0);
  CHECK(!is_fuel(item("minecraft:iron_ingot")));
  CHECK(is_fuel(item("minecraft:coal")));

  // FurnaceFuelSlot: a bucket (and a lava bucket) stacks to one, whatever the
  // item's own limit is.
  CHECK(fuel_slot_max_stack(item("minecraft:lava_bucket"), 64) == 1);
  CHECK(fuel_slot_max_stack(item("minecraft:bucket"), 64) == 1);
  CHECK(fuel_slot_max_stack(item("minecraft:coal"), 64) == 64);
  CHECK(is_bucket_item(item("minecraft:bucket")));
  CHECK(is_bucket_item(item("minecraft:lava_bucket")));
  // FurnaceFuelSlot.isBucket is exactly these two: a water bucket is *not* the
  // item the slot protects (a burned-out bucket cannot become one).
  CHECK(!is_bucket_item(item("minecraft:water_bucket")));
  CHECK(!is_bucket_item(item("minecraft:coal")));
  // The one remainder the furnace family can observe: burning a lava bucket
  // leaves the empty bucket behind (Item.getCraftingRemainder).
  CHECK(fuel_remainder_item(item("minecraft:lava_bucket")) == item("minecraft:bucket"));
  CHECK(fuel_remainder_item(item("minecraft:coal")) == kAirItemId);
}

void test_cooking_recipes() {
  CHECK(cooking_kind_for_block("minecraft:furnace") == RecipeKind::Smelting);
  CHECK(cooking_kind_for_block("minecraft:blast_furnace") == RecipeKind::Blasting);
  CHECK(cooking_kind_for_block("minecraft:smoker") == RecipeKind::Smoking);
  CHECK(cooking_kind_for_block("minecraft:chest") == RecipeKind::Special);
  CHECK(is_cooking_block("minecraft:furnace"));
  CHECK(!is_cooking_block("minecraft:chest"));

  const GeneratedRecipe* smelt =
      find_cooking_recipe(RecipeKind::Smelting, item("minecraft:raw_iron"));
  CHECK(smelt != nullptr);
  if (smelt != nullptr) {
    CHECK(smelt->result_item != 0);
    CHECK(cooking_total_time(RecipeKind::Smelting, item("minecraft:raw_iron")) == 200);
    CHECK(cooking_experience_milli(*smelt) == 700); // 0.7 xp
  }
  // 26.3's recipe data gives blasting and smoking an explicit 200-tick
  // `cookingtime`; the total still comes from the matched recipe, not the family.
  CHECK(cooking_total_time(RecipeKind::Blasting, item("minecraft:raw_iron")) == 200);
  CHECK(cooking_total_time(RecipeKind::Smoking, item("minecraft:beef")) == 200);
  // Nothing to cook: vanilla's `orElse(200)` keeps the bar's denominator sane.
  CHECK(find_cooking_recipe(RecipeKind::Smelting, item("minecraft:diamond")) == nullptr);
  CHECK(cooking_total_time(RecipeKind::Smelting, item("minecraft:diamond")) == 200);
  CHECK(find_cooking_recipe(RecipeKind::Smelting, kAirItemId) == nullptr);
}

void test_tick_burn() {
  // One cold furnace, one ore, one coal: the first tick lights it and consumes
  // the fuel unit (AbstractFurnaceBlockEntity.burn -> consumeFuel).
  Inventory slots = furnace_slots("minecraft:raw_iron", 1, "minecraft:coal", 1);
  FurnaceState state;
  const FurnaceTickResult r = tick_furnace(RecipeKind::Smelting, state, slots);
  CHECK(r.changed);
  CHECK(r.lit_changed);
  CHECK(r.lit);
  CHECK(state.lit_time_remaining == 1600);
  CHECK(state.lit_total_time == 1600);
  CHECK(state.cooking_timer == 1);
  CHECK(state.cooking_total_time == 200); // setItem(0) re-derived it
  CHECK(slots.get(1).empty());            // the coal was consumed
  CHECK(slots.get(0).count == 1);         // the ore is still there
  CHECK(slots.get(2).empty());

  // 199 more ticks finish the craft: one ingot out, the ore gone, the flame
  // still burning.
  for (int i = 0; i < 199; ++i)
    tick_furnace(RecipeKind::Smelting, state, slots);
  CHECK(slots.get(2).item == item("minecraft:iron_ingot"));
  CHECK(slots.get(2).count == 1);
  CHECK(slots.get(0).empty());
  CHECK(state.cooking_timer == 0);
  // One unit of coal is 1600 ticks; the tick that consumed the fuel did not
  // decrement, and the 199 after it did.
  CHECK(state.lit_time_remaining == 1600 - 199);
  CHECK(state.recipes_used.size() == 1);
  CHECK(state.recipes_used.count("iron_ingot_from_smelting_raw_iron") == 1);
  CHECK(state.recipes_used.begin()->second == 1);

  // The blast furnace's half-length cook time comes from the recipe.
  Inventory blast = furnace_slots("minecraft:raw_iron", 1, "minecraft:coal", 1);
  FurnaceState blast_state;
  for (int i = 0; i < 200; ++i)
    tick_furnace(RecipeKind::Blasting, blast_state, blast);
  CHECK(blast.get(2).count == 1);
  CHECK(blast_state.cooking_total_time == 200);
}

void test_tick_gates() {
  // canBurn: an output slot that cannot take the whole result stops the cook —
  // and the fuel is not consumed for it.
  Inventory full = furnace_slots("minecraft:raw_iron", 1, "minecraft:coal", 1);
  full.set(2, ItemStack::of_id(item("minecraft:iron_ingot"), 64));
  FurnaceState state;
  const FurnaceTickResult blocked = tick_furnace(RecipeKind::Smelting, state, full);
  CHECK(!blocked.lit_changed);
  CHECK(state.lit_time_remaining == 0);
  CHECK(state.cooking_timer == 0);
  CHECK(full.get(1).count == 1); // the coal is untouched

  // Smelting the same item into a full-ish output *is* allowed while the whole
  // result still fits (canBurn's `count + result <= cap`).
  full.set(2, ItemStack::of_id(item("minecraft:iron_ingot"), 63));
  const FurnaceTickResult ok = tick_furnace(RecipeKind::Smelting, state, full);
  CHECK(ok.lit_changed);

  // An input with nothing to cook resets progress rather than banking it.
  Inventory junk = furnace_slots("minecraft:diamond", 1, "minecraft:coal", 1);
  FurnaceState junk_state;
  junk_state.last_input_item = item("minecraft:diamond");
  junk_state.cooking_timer = 40;
  junk_state.cooking_total_time = 200;
  tick_furnace(RecipeKind::Smelting, junk_state, junk);
  CHECK(junk_state.cooking_timer == 0);
  CHECK(junk_state.lit_time_remaining == 0); // and no fuel is burned for it

  // Unlit with nothing to cook: progress decays by two per tick, it does not
  // reset (Mth.clamp(cookingTimer - 2, 0, cookingTotalTime)).
  Inventory idle = furnace_slots("minecraft:raw_iron", 1, nullptr, 0);
  FurnaceState idle_state;
  idle_state.last_input_item = item("minecraft:raw_iron"); // already in the input
  idle_state.cooking_timer = 5;
  idle_state.cooking_total_time = 200;
  tick_furnace(RecipeKind::Smelting, idle_state, idle);
  CHECK(idle_state.cooking_timer == 3);
  tick_furnace(RecipeKind::Smelting, idle_state, idle);
  CHECK(idle_state.cooking_timer == 1);
  tick_furnace(RecipeKind::Smelting, idle_state, idle);
  CHECK(idle_state.cooking_timer == 0); // clamped, never negative

  // A different input item re-derives the total cook time; the *count* does not.
  Inventory swap = furnace_slots("minecraft:stone", 1, nullptr, 0);
  FurnaceState swap_state;
  swap_state.cooking_total_time = 999;
  swap_state.last_input_item = item("minecraft:raw_iron");
  swap.set(0, ItemStack::of_id(item("minecraft:beef"), 3));
  tick_furnace(RecipeKind::Smoking, swap_state, swap);
  CHECK(swap_state.cooking_total_time == 200); // re-derived from smoked beef
}

void test_tick_sponge() {
  // burn()'s wet-sponge quirk: a wet sponge dried while the fuel slot holds a
  // bucket leaves a *water* bucket behind.
  Inventory slots = furnace_slots("minecraft:wet_sponge", 1, "minecraft:bucket", 1);
  FurnaceState state;
  // The input is already the sponge, so the tick does not re-derive the cook
  // time (which would reset the progress below) — the same state a burner that
  // has been running for 199 ticks is in.
  state.last_input_item = item("minecraft:wet_sponge");
  state.lit_time_remaining = 5;
  state.lit_total_time = 5;
  state.cooking_timer = 199;
  state.cooking_total_time = 200;
  for (int i = 0; i < 2; ++i)
    tick_furnace(RecipeKind::Smelting, state, slots);
  CHECK(slots.get(2).item == item("minecraft:sponge"));
  CHECK(slots.get(1).item == item("minecraft:water_bucket"));
}

void test_furnace_nbt() {
  FurnaceState state;
  state.lit_time_remaining = 123;
  state.lit_total_time = 1600;
  state.cooking_timer = 45;
  state.cooking_total_time = 200;
  state.recipes_used["iron_ingot_from_smelting_raw_iron"] = 3;
  state.recipes_used["bread"] = 2;

  nbt::Value be = make_container_block_entity("minecraft:furnace", BlockPosKey{4, 70, -9},
                                              std::vector<ItemStack>{});
  write_block_entity_items(&be,
                           std::vector<ItemStack>{ItemStack::of_id(item("minecraft:raw_iron"), 2)});
  write_block_entity_furnace(&be, state);

  FurnaceState read;
  read_block_entity_furnace(be, &read);
  CHECK(read.lit_time_remaining == 123);
  CHECK(read.lit_total_time == 1600);
  CHECK(read.cooking_timer == 45);
  CHECK(read.cooking_total_time == 200);
  CHECK(read.recipes_used.size() == 2);
  CHECK(read.recipes_used["iron_ingot_from_smelting_raw_iron"] == 3);
  CHECK(read.recipes_used["bread"] == 2);
  // The timers ride alongside the item list rather than replacing it.
  CHECK(read_block_entity_items(be, 3)[0].item == item("minecraft:raw_iron"));

  // A record with no timers at all reads as a cold furnace (what a hand-written
  // or vanilla furnace without them means too).
  nbt::Value bare = make_container_block_entity("minecraft:furnace", BlockPosKey{0, 0, 0},
                                                std::vector<ItemStack>{});
  FurnaceState fresh;
  fresh.lit_time_remaining = 999;
  fresh.recipes_used["bread"] = 5;
  read_block_entity_furnace(bare, &fresh);
  CHECK(fresh.lit_time_remaining == 0);
  CHECK(fresh.lit_total_time == 0);
  CHECK(fresh.cooking_timer == 0);
  CHECK(fresh.recipes_used.empty());
  CHECK(nbt::child(bare, "RecipesUsed") == nullptr);
}

void test_experience_awards() {
  FurnaceState state;
  // Three crafts of 0.7 xp: floor(2.1) = 2, plus one when the fraction (0.1)
  // beats a single draw — createExperience's one nextFloat per recipe.
  state.recipes_used["iron_ingot_from_smelting_raw_iron"] = 3;
  std::vector<CookingAward> lucky = take_cooking_awards(state, [] { return 0.05; });
  CHECK(lucky.size() == 1);
  CHECK(lucky[0].crafts == 3);
  CHECK(lucky[0].experience == 3);
  CHECK(state.recipes_used.empty()); // RecipesUsed.clear()

  state.recipes_used["iron_ingot_from_smelting_raw_iron"] = 3;
  std::vector<CookingAward> unlucky = take_cooking_awards(state, [] { return 0.5; });
  CHECK(unlucky.size() == 1);
  CHECK(unlucky[0].experience == 2);

  // Two smokes of 0.35 xp: floor(0.7) = 0, plus one when the draw beats 0.7.
  state.recipes_used["cooked_beef_from_smoking"] = 2;
  bool drew = false;
  std::vector<CookingAward> food = take_cooking_awards(state, [&] {
    drew = true;
    return 0.0;
  });
  CHECK(food.size() == 1);
  CHECK(drew);
  CHECK(food[0].experience == 1);

  // A recipe that pays a whole number of points never draws at all.
  state.recipes_used["cooked_beef_from_smoking"] = 20; // 7.0 xp exactly
  drew = false;
  std::vector<CookingAward> whole = take_cooking_awards(state, [&] {
    drew = true;
    return 0.0;
  });
  CHECK(whole.size() == 1);
  CHECK(whole[0].experience == 7);
  CHECK(!drew);
}

void test_book_placement() {
  // The raw-iron smelting display, found the way a client's id would be.
  const RecipeBookDisplay* display = nullptr;
  for (const RecipeBookDisplay& d : recipe_book_displays()) {
    if (d.kind != DisplayKind::Furnace || d.recipe == nullptr || d.recipe->name == nullptr)
      continue;
    if (std::string(d.recipe->name) == "iron_ingot_from_smelting_raw_iron") {
      display = &d;
      break;
    }
  }
  CHECK(display != nullptr);
  if (display == nullptr)
    return;

  // One craft: one ore into the input, the rest of the stack left behind.
  {
    Inventory slots{3};
    slots.set(1, ItemStack::of_id(item("minecraft:coal"), 5));
    Inventory inv{36};
    inv.set(9, ItemStack::of_id(item("minecraft:raw_iron"), 10));
    std::vector<ItemStack> spilled;
    const PlaceOutcome outcome =
        place_furnace_recipe_from_book(*display, slots, inv, 0, false, false, &spilled);
    CHECK(outcome == PlaceOutcome::Placed);
    CHECK(slots.get(0).item == item("minecraft:raw_iron"));
    CHECK(slots.get(0).count == 1);
    CHECK(inv.get(9).count == 9);
    CHECK(slots.get(1).count == 5); // the fuel is never touched
    CHECK(spilled.empty());
  }

  // use_max_items: the whole stack goes in the input.
  {
    Inventory slots{3};
    Inventory inv{36};
    inv.set(9, ItemStack::of_id(item("minecraft:raw_iron"), 10));
    const PlaceOutcome outcome =
        place_furnace_recipe_from_book(*display, slots, inv, 0, true, false, nullptr);
    CHECK(outcome == PlaceOutcome::Placed);
    CHECK(slots.get(0).count == 10);
    CHECK(inv.get(9).empty());
  }

  // A grid that already holds the recipe tops it up by one (smallest + 1).
  {
    Inventory slots{3};
    slots.set(0, ItemStack::of_id(item("minecraft:raw_iron"), 5));
    Inventory inv{36};
    inv.set(9, ItemStack::of_id(item("minecraft:raw_iron"), 10));
    const PlaceOutcome outcome =
        place_furnace_recipe_from_book(*display, slots, inv, 0, false, false, nullptr);
    CHECK(outcome == PlaceOutcome::Placed);
    CHECK(slots.get(0).count == 6);
    CHECK(inv.get(9).count == 9);
  }

  // Nothing to cook with: the input and the *result* are cleared and the click
  // becomes a ghost (ServerPlaceRecipe.tryPlaceRecipe).
  {
    Inventory slots{3};
    slots.set(0, ItemStack::of_id(item("minecraft:stone"), 4));
    slots.set(2, ItemStack::of_id(item("minecraft:iron_ingot"), 2));
    Inventory inv{36};
    const PlaceOutcome outcome =
        place_furnace_recipe_from_book(*display, slots, inv, 0, false, false, nullptr);
    CHECK(outcome == PlaceOutcome::Ghost);
    CHECK(slots.get(0).empty());
    CHECK(slots.get(2).empty());
    CHECK(inv.slots().size() == 36);
    // The two items that were in the furnace are back in the player's inventory.
    int recovered_iron = 0;
    for (const ItemStack& s : inv.slots())
      if (s.item == item("minecraft:iron_ingot"))
        recovered_iron += s.count;
    CHECK(recovered_iron == 2);
  }

  // A full inventory makes the clear impossible: nothing moves, the click is
  // refused (testClearGrid) — in creative it spills instead.
  {
    Inventory slots{3};
    slots.set(0, ItemStack::of_id(item("minecraft:stone"), 4));
    Inventory inv{36};
    for (int i = 0; i < 36; ++i)
      inv.set(i, ItemStack::of_id(item("minecraft:dirt"), 64));
    std::vector<ItemStack> spilled;
    CHECK(place_furnace_recipe_from_book(*display, slots, inv, 0, false, false, &spilled) ==
          PlaceOutcome::Refused);
    CHECK(slots.get(0).count == 4); // untouched
    CHECK(spilled.empty());
    CHECK(place_furnace_recipe_from_book(*display, slots, inv, 0, false, true, &spilled) !=
          PlaceOutcome::Refused);
    CHECK(!spilled.empty());
  }

  // A non-cooking display is not placeable in a furnace at all.
  {
    const RecipeBookDisplay* shaped = nullptr;
    for (const RecipeBookDisplay& d : recipe_book_displays())
      if (d.kind == DisplayKind::CraftingShaped) {
        shaped = &d;
        break;
      }
    if (shaped != nullptr) {
      Inventory slots{3};
      Inventory inv{36};
      CHECK(place_furnace_recipe_from_book(*shaped, slots, inv, 0, false, false, nullptr) ==
            PlaceOutcome::Refused);
    }
  }
}

} // namespace

int main() {
  test_fuel_table();
  test_cooking_recipes();
  test_tick_burn();
  test_tick_gates();
  test_tick_sponge();
  test_furnace_nbt();
  test_experience_awards();
  test_book_placement();
  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("cooking: all checks passed\n");
  return 0;
}
