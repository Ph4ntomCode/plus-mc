// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the brewing stand (Phase 3 milestone G follow-up):
//   * the generated brewing table: the three containers, the two container
//     mixes, the 63 potion mixes (including the two `addStartMix` entries each
//     of the eleven start mixes contributes) and the one-item fuel tag;
//   * PotionBrewing's lookups — isContainer / isIngredient / isFuel /
//     hasMix — and `mix`, including the two things a brewed bottle always
//     loses: its count past one (createItemStack builds a fresh stack) and its
//     custom effects;
//   * BrewingStandBlockEntity.serverTick: the fuel branch burning one unit for
//     20 uses, the 400-tick timer, the ingredient-mismatch cancel, the two
//     counters, the `has_bottle_0..2` bits, the ingredient's crafting
//     remainder (dragon's breath -> a glass bottle, into the slot when it
//     empties and onto the floor when it does not) and how long a batch takes
//     to reach those states;
//   * the block entity's NBT round-trip (`BrewTime`, `Fuel`).
#include <cstdio>
#include <string>
#include <vector>

#include "world/brewing.h"
#include "world/container.h"
#include "world/cooking.h"
#include "world/inventory.h"
#include "world/item.h"
#include "world/item_registry.h"
#include "world/nbt.h"
#include "world/potion.h"

using namespace mc::world;

static int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECK_EQ(a, b)                                                                             \
  do {                                                                                             \
    const long long va = static_cast<long long>(a), vb = static_cast<long long>(b);                \
    if (va != vb) {                                                                                \
      std::fprintf(stderr, "FAIL %s:%d: %s == %lld, want %lld\n", __FILE__, __LINE__, #a, va, vb); \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

namespace {

int32_t item(const char* name) {
  auto id = ItemRegistry::instance().id_of(name);
  return id ? *id : -1;
}

int32_t potion(const char* name) {
  const GeneratedPotion* p = potion_by_name(name);
  return p ? potion_id_of(*p) : -1;
}

// A stack of one item carrying a potion, the way /give builds one: the component
// is what a brew's source needs (a plain bottle has no potion to carry over).
ItemStack potion_stack(const char* item_name, const char* potion_name) {
  ItemStack stack = ItemStack::of_id(item(item_name), 1);
  stack.potion_contents = StackPotionContents{potion(potion_name), {}};
  return stack;
}

// A brewing stand's five slots: three bottles, the ingredient, the fuel.
Inventory stand_slots(const char* bottle, const char* ingredient, int ingredient_n,
                      const char* fuel, int fuel_n) {
  Inventory slots{kBrewingSlotCount};
  if (bottle != nullptr)
    for (int i = 0; i < kBrewingBottleSlots; ++i)
      slots.set(i, potion_stack(bottle, "water"));
  if (ingredient != nullptr && ingredient_n > 0)
    slots.set(kBrewingIngredientSlot, ItemStack::of_id(item(ingredient), ingredient_n));
  if (fuel != nullptr && fuel_n > 0)
    slots.set(kBrewingFuelSlot, ItemStack::of_id(item(fuel), fuel_n));
  return slots;
}

// Runs up to `ticks` ticks, stopping at the first that changes nothing (the
// world tick's own early exit), and reports how many ran.
int tick_until_idle(BrewingState& state, Inventory& slots, int ticks) {
  for (int i = 0; i < ticks; ++i) {
    const BrewingTickResult r = tick_brewing(state, slots);
    if (!r.changed)
      return i;
  }
  return ticks;
}

void test_table() {
  // PotionBrewing: three containers, two container mixes, one fuel item.
  CHECK_EQ(kBrewingContainerCount, 3);
  CHECK_EQ(kBrewingContainerMixCount, 2);
  CHECK_EQ(kBrewingFuelCount, 1);
  CHECK_EQ(kBrewingContainerMixes[0].from, item("minecraft:potion"));
  CHECK_EQ(kBrewingContainerMixes[0].ingredient, item("minecraft:gunpowder"));
  CHECK_EQ(kBrewingContainerMixes[0].to, item("minecraft:splash_potion"));
  CHECK_EQ(kBrewingContainerMixes[1].from, item("minecraft:splash_potion"));
  CHECK_EQ(kBrewingContainerMixes[1].ingredient, item("minecraft:dragon_breath"));
  CHECK_EQ(kBrewingContainerMixes[1].to, item("minecraft:lingering_potion"));
  CHECK_EQ(kBrewingFuels[0], item("minecraft:blaze_powder"));

  // `addStartMix` contributes two entries each: water plus the ingredient is
  // mundane, awkward plus it is the named potion. All eleven are there, so the
  // count is 63 (22 from the start mixes plus 41 explicit ones).
  CHECK_EQ(kBrewingPotionMixCount, 63);
  int water_to_mundane = 0;
  bool sugar_water = false, sugar_awkward = false, wart_awkward = false;
  bool night_vision = false, long_night_vision = false, strong_healing = false;
  for (int i = 0; i < kBrewingPotionMixCount; ++i) {
    const GeneratedPotionMix& mix = kBrewingPotionMixes[i];
    if (mix.from == potion("minecraft:water") && mix.to == potion("minecraft:mundane"))
      ++water_to_mundane;
    if (mix.ingredient == item("minecraft:sugar") && mix.from == potion("minecraft:water"))
      sugar_water = mix.to == potion("minecraft:mundane");
    if (mix.ingredient == item("minecraft:sugar") && mix.from == potion("minecraft:awkward"))
      sugar_awkward = mix.to == potion("minecraft:swiftness");
    if (mix.ingredient == item("minecraft:nether_wart"))
      wart_awkward = mix.from == potion("minecraft:water") && mix.to == potion("minecraft:awkward");
    if (mix.ingredient == item("minecraft:golden_carrot"))
      night_vision =
          mix.from == potion("minecraft:awkward") && mix.to == potion("minecraft:night_vision");
    if (mix.ingredient == item("minecraft:redstone"))
      long_night_vision = long_night_vision || (mix.from == potion("minecraft:night_vision") &&
                                                mix.to == potion("minecraft:long_night_vision"));
    if (mix.ingredient == item("minecraft:glowstone_dust"))
      strong_healing = strong_healing || (mix.from == potion("minecraft:healing") &&
                                          mix.to == potion("minecraft:strong_healing"));
  }
  CHECK(sugar_water);
  CHECK(sugar_awkward);
  CHECK(wart_awkward);
  CHECK(night_vision);
  CHECK(long_night_vision);
  CHECK(strong_healing);
  // The eleven start mixes each register `water -> mundane`, and the explicit
  // `addMix(WATER, REDSTONE, MUNDANE)` is a twelfth water -> mundane row with a
  // different ingredient. They are not redundant: the lookup matches on the
  // *ingredient* too, so water plus any of those twelve becomes mundane and
  // water plus anything else does not.
  CHECK_EQ(water_to_mundane, 12);
}

void test_lookups() {
  CHECK(brewing_is_container(item("minecraft:potion")));
  CHECK(brewing_is_container(item("minecraft:splash_potion")));
  CHECK(brewing_is_container(item("minecraft:lingering_potion")));
  CHECK(!brewing_is_container(item("minecraft:glass_bottle")));

  // The ingredient slot takes either half of the mix list.
  CHECK(brewing_is_ingredient(item("minecraft:nether_wart")));
  CHECK(brewing_is_ingredient(item("minecraft:redstone")));
  CHECK(brewing_is_ingredient(item("minecraft:gunpowder")));
  CHECK(brewing_is_ingredient(item("minecraft:dragon_breath")));
  CHECK(!brewing_is_ingredient(item("minecraft:apple")));
  CHECK(!brewing_is_ingredient(item("minecraft:glass_bottle")));

  // The fuel is the whole `minecraft:brewing_fuel` tag: blaze powder and
  // nothing else. A blaze *rod* is a furnace fuel, not a brewing one.
  CHECK(brewing_is_fuel(item("minecraft:blaze_powder")));
  CHECK(!brewing_is_fuel(item("minecraft:blaze_rod")));
  CHECK(!brewing_is_fuel(kAirItemId));

  // BrewingStandMenu.PotionSlot.mayPlaceItem: the three potions and a glass
  // bottle, which is how an empty bottle goes in to be filled.
  CHECK(brewing_is_bottle_slot_item(item("minecraft:glass_bottle")));
  CHECK(brewing_is_bottle_slot_item(item("minecraft:potion")));
  CHECK(brewing_is_bottle_slot_item(item("minecraft:splash_potion")));
  CHECK(brewing_is_bottle_slot_item(item("minecraft:lingering_potion")));
  CHECK(!brewing_is_bottle_slot_item(item("minecraft:bowl")));

  // hasMix needs a *container* with a potion the mix names: a glass bottle has
  // no potion to carry over, and a splash potion has already been gunpowdered.
  CHECK(brewing_has_mix(potion_stack("minecraft:potion", "water"), item("minecraft:nether_wart")));
  CHECK(brewing_has_mix(potion_stack("minecraft:potion", "awkward"), item("minecraft:sugar")));
  // Water plus sugar *is* a mix — it makes a mundane potion, which is the whole
  // point of `addStartMix` registering `water -> mundane` for every ingredient
  // it also gives an awkward recipe. Sugar is what turns awkward into swiftness.
  CHECK(brewing_has_mix(potion_stack("minecraft:potion", "water"), item("minecraft:sugar")));
  // A container mix ignores which potion the bottle holds: gunpowder turns *any*
  // potion, water included, into its splash form; dragon's breath then makes it
  // lingering. A splash potion has no gunpowder recipe left.
  CHECK(brewing_has_mix(potion_stack("minecraft:potion", "water"), item("minecraft:gunpowder")));
  CHECK(brewing_has_mix(potion_stack("minecraft:potion", "healing"),
                        item("minecraft:dragon_breath")) == false);
  CHECK(brewing_has_mix(potion_stack("minecraft:splash_potion", "water"),
                        item("minecraft:dragon_breath")));
  CHECK(!brewing_has_mix(potion_stack("minecraft:splash_potion", "water"),
                         item("minecraft:gunpowder")));
  CHECK(!brewing_has_mix(ItemStack::of_id(item("minecraft:glass_bottle"), 1),
                         item("minecraft:nether_wart")));
}

void test_mix() {
  // A container mix changes the item and leaves the potion alone.
  const ItemStack splash =
      brewing_mix(item("minecraft:gunpowder"), potion_stack("minecraft:potion", "healing"));
  CHECK_EQ(splash.item, item("minecraft:splash_potion"));
  CHECK_EQ(splash.count, 1);
  CHECK(splash.potion_contents.has_value());
  CHECK_EQ(splash.potion_contents->potion, potion("minecraft:healing"));

  const ItemStack lingering = brewing_mix(item("minecraft:dragon_breath"),
                                          potion_stack("minecraft:splash_potion", "swiftness"));
  CHECK_EQ(lingering.item, item("minecraft:lingering_potion"));
  CHECK_EQ(lingering.potion_contents->potion, potion("minecraft:swiftness"));

  // A potion mix keeps the bottle and replaces the potion.
  const ItemStack awkward =
      brewing_mix(item("minecraft:nether_wart"), potion_stack("minecraft:potion", "water"));
  CHECK_EQ(awkward.item, item("minecraft:potion"));
  CHECK_EQ(awkward.potion_contents->potion, potion("minecraft:awkward"));

  // `PotionContents.createItemStack` builds a *fresh* stack, so a bottle's
  // custom effects are lost when it is brewed — and so is a count above one.
  ItemStack custom = potion_stack("minecraft:potion", "water");
  custom.count = 3;
  custom.potion_contents->custom_effects.push_back(MobEffectInstance{});
  const ItemStack brewed = brewing_mix(item("minecraft:nether_wart"), custom);
  CHECK_EQ(brewed.count, 1);
  CHECK(brewed.potion_contents->custom_effects.empty());

  // A source no mix matches, and a source with no potion at all, come back
  // unchanged: doBrew runs every bottle slot through `mix`.
  const ItemStack plain = ItemStack::of_id(item("minecraft:potion"), 1);
  CHECK(brewing_mix(item("minecraft:nether_wart"), plain).potion_contents.has_value() == false);
  const ItemStack wrong = potion_stack("minecraft:potion", "water");
  const ItemStack unchanged = brewing_mix(item("minecraft:apple"), wrong);
  CHECK_EQ(unchanged.item, wrong.item);
  CHECK_EQ(unchanged.potion_contents->potion, potion("minecraft:water"));
}

void test_tick_starts_a_brew() {
  BrewingState state;
  Inventory slots =
      stand_slots("minecraft:potion", "minecraft:nether_wart", 1, "minecraft:blaze_powder", 2);

  // First tick: the fuel branch loads 20 uses out of one blaze powder, then the
  // brew itself spends a use to start the 400-tick timer.
  const BrewingTickResult first = tick_brewing(state, slots);
  CHECK(first.changed);
  CHECK(first.data_changed);
  CHECK_EQ(state.fuel, kBrewingFuelUses - 1);
  CHECK_EQ(state.brew_time, kBrewingTimeTicks);
  CHECK_EQ(state.ingredient_item, item("minecraft:nether_wart"));
  CHECK(!first.brewed);
  CHECK_EQ(slots.get(kBrewingFuelSlot).count, 1); // one unit burned, not two
  CHECK_EQ(slots.get(kBrewingIngredientSlot).count, 1);

  // `has_bottle_0..2` were written on that first tick, because vanilla's
  // lastPotionCount starts null — and only then, since the bits match after.
  CHECK(first.bottle_bits_changed);
  CHECK(first.bottles[0] && first.bottles[1] && first.bottles[2]);
  const BrewingTickResult second = tick_brewing(state, slots);
  CHECK(!second.bottle_bits_changed);
  CHECK_EQ(state.brew_time, kBrewingTimeTicks - 1);

  // The fuel branch does not fire again while there are uses left, and each tick
  // only counts the timer down.
  CHECK_EQ(slots.get(kBrewingFuelSlot).count, 1);
  CHECK_EQ(state.fuel, kBrewingFuelUses - 1);
}

void test_tick_finishes_and_cancels() {
  BrewingState state;
  Inventory slots =
      stand_slots("minecraft:potion", "minecraft:nether_wart", 1, "minecraft:blaze_powder", 1);

  // The tick that starts a brew sets the timer and does *not* count it down, so
  // the brew finishes on the 401st tick (-1 because this counts from 0).
  int ticks = 0;
  for (; ticks <= kBrewingTimeTicks; ++ticks) {
    const BrewingTickResult r = tick_brewing(state, slots);
    if (r.brewed) {
      CHECK(r.changed);
      break;
    }
  }
  CHECK_EQ(ticks, kBrewingTimeTicks);
  CHECK_EQ(state.brew_time, 0);
  for (int i = 0; i < kBrewingBottleSlots; ++i) {
    CHECK_EQ(slots.get(i).item, item("minecraft:potion"));
    CHECK(slots.get(i).potion_contents.has_value());
    CHECK_EQ(slots.get(i).potion_contents->potion, potion("minecraft:awkward"));
  }
  // The one nether wart was consumed, and it has no crafting remainder, so the
  // slot is empty rather than holding a by-product.
  CHECK(slots.get(kBrewingIngredientSlot).empty());

  // Swapping the ingredient mid-brew cancels it instead of pausing it: the
  // stand goes idle with the bottles untouched and the fuel use already spent.
  BrewingState cancel;
  Inventory swap =
      stand_slots("minecraft:potion", "minecraft:nether_wart", 3, "minecraft:blaze_powder", 1);
  tick_brewing(cancel, swap);
  CHECK_EQ(cancel.brew_time, kBrewingTimeTicks);
  // An *ingredient* that has no mix with these bottles: the brew is dropped and
  // never restarts, because `is_brewable` is false from here on.
  swap.set(kBrewingIngredientSlot, ItemStack::of_id(item("minecraft:apple"), 3));
  tick_brewing(cancel, swap);
  CHECK_EQ(cancel.brew_time, 0);
  CHECK_EQ(swap.get(0).potion_contents->potion, potion("minecraft:water"));
  tick_brewing(cancel, swap);
  CHECK_EQ(cancel.brew_time, 0);
  CHECK_EQ(cancel.fuel, kBrewingFuelUses - 1); // one use, spent on the lost brew

  // A *brewable* replacement cancels the old brew and starts a new one on the
  // very next tick, with the new ingredient recorded — water plus sugar makes a
  // mundane potion, so this is a real restart rather than a continuation.
  BrewingState restart;
  Inventory swapped =
      stand_slots("minecraft:potion", "minecraft:nether_wart", 3, "minecraft:blaze_powder", 1);
  tick_brewing(restart, swapped);
  swapped.set(kBrewingIngredientSlot, ItemStack::of_id(item("minecraft:sugar"), 3));
  tick_brewing(restart, swapped);
  CHECK_EQ(restart.brew_time, 0);
  tick_brewing(restart, swapped);
  CHECK_EQ(restart.brew_time, kBrewingTimeTicks);
  CHECK_EQ(restart.ingredient_item, item("minecraft:sugar"));
  CHECK_EQ(restart.fuel, kBrewingFuelUses - 2);
}

void test_tick_needs_mix_and_fuel() {
  // No ingredient: nothing happens at all, not even the fuel branch (which only
  // cares about the fuel slot) — here there is no fuel either.
  BrewingState state;
  Inventory slots{kBrewingSlotCount};
  slots.set(0, potion_stack("minecraft:potion", "water"));
  const BrewingTickResult idle = tick_brewing(state, slots);
  CHECK_EQ(state.brew_time, 0);
  CHECK_EQ(state.fuel, 0);
  // The bottle bit still flips, though: the state is what the client draws.
  CHECK(idle.bottle_bits_changed);
  CHECK(idle.bottles[0] && !idle.bottles[1] && !idle.bottles[2]);

  // An ingredient no bottle has a mix with: the fuel is *not* spent (vanilla
  // only decrements it when a brew actually starts) and nothing changes.
  Inventory pointless{kBrewingSlotCount};
  pointless.set(0, potion_stack("minecraft:potion", "water"));
  pointless.set(kBrewingIngredientSlot, ItemStack::of_id(item("minecraft:apple"), 1));
  pointless.set(kBrewingFuelSlot, ItemStack::of_id(item("minecraft:blaze_powder"), 1));
  const BrewingTickResult no_mix = tick_brewing(state, pointless);
  CHECK(!no_mix.brewed);
  CHECK_EQ(state.brew_time, 0);
  CHECK_EQ(state.fuel, kBrewingFuelUses); // loaded from the powder, never spent
  CHECK_EQ(pointless.get(kBrewingFuelSlot).count, 0);

  // Brewable, no fuel: the timer stays at 0 and no use is spent.
  BrewingState dry;
  Inventory unfuelled = stand_slots("minecraft:potion", "minecraft:nether_wart", 1, nullptr, 0);
  tick_brewing(dry, unfuelled);
  CHECK_EQ(dry.brew_time, 0);
  CHECK_EQ(dry.fuel, 0);

  // A glass bottle is not a container, so it never starts a brew on its own:
  // there is no potion to carry the mix's result.
  BrewingState bottles_only;
  Inventory empty_bottles{kBrewingSlotCount};
  empty_bottles.set(0, ItemStack::of_id(item("minecraft:glass_bottle"), 1));
  empty_bottles.set(kBrewingIngredientSlot, ItemStack::of_id(item("minecraft:nether_wart"), 1));
  empty_bottles.set(kBrewingFuelSlot, ItemStack::of_id(item("minecraft:blaze_powder"), 1));
  tick_brewing(bottles_only, empty_bottles);
  CHECK_EQ(bottles_only.brew_time, 0);
}

void test_tick_container_mix_and_remainder() {
  // A container mix: three water *splash* potions plus one dragon's breath
  // become three lingering potions, and the ingredient's *crafting* remainder —
  // a glass bottle — takes the slot the breath emptied
  // (Item.getCraftingRemainder, then `items.set(3, remainder.create())` because
  // the slot was empty).
  BrewingState state;
  Inventory slots = stand_slots("minecraft:splash_potion", "minecraft:dragon_breath", 1,
                                "minecraft:blaze_powder", 1);
  int ticks = 0;
  for (; ticks <= kBrewingTimeTicks; ++ticks) {
    const BrewingTickResult r = tick_brewing(state, slots);
    if (r.brewed) {
      CHECK(r.drop_remainder.empty());
      break;
    }
  }
  CHECK_EQ(ticks, kBrewingTimeTicks);
  for (int i = 0; i < kBrewingBottleSlots; ++i) {
    CHECK_EQ(slots.get(i).item, item("minecraft:lingering_potion"));
    CHECK_EQ(slots.get(i).potion_contents->potion, potion("minecraft:water"));
  }
  CHECK_EQ(slots.get(kBrewingIngredientSlot).item, item("minecraft:glass_bottle"));
  CHECK_EQ(slots.get(kBrewingIngredientSlot).count, 1);

  // With two dragon's breath the slot is not emptied by the consume, so the
  // remainder has nowhere to go and is dropped instead (Containers.dropItemStack)
  // — reported to the caller, which owns item entities.
  BrewingState two_state;
  Inventory two = stand_slots("minecraft:splash_potion", "minecraft:dragon_breath", 2,
                              "minecraft:blaze_powder", 1);
  for (int i = 0; i <= kBrewingTimeTicks; ++i) {
    const BrewingTickResult r = tick_brewing(two_state, two);
    if (r.brewed) {
      CHECK_EQ(r.drop_remainder.item, item("minecraft:glass_bottle"));
      CHECK_EQ(r.drop_remainder.count, 1);
      break;
    }
  }
  CHECK_EQ(two.get(kBrewingIngredientSlot).item, item("minecraft:dragon_breath"));
  CHECK_EQ(two.get(kBrewingIngredientSlot).count, 1);
}

void test_tick_batch_is_cheap_when_idle() {
  // The world tick stops at the first tick that changes nothing, so an idle
  // stand costs one tick per batch rather than one per game tick.
  BrewingState state;
  state.bottles_known = true; // as if the stand had already ticked once
  Inventory slots{kBrewingSlotCount};
  CHECK_EQ(tick_until_idle(state, slots, 400), 0);
}

void test_crafting_remainder() {
  // Item.getCraftingRemainder is an Item *property* set on exactly five items in
  // 26.2, which is why it is not a column in the generated item table.
  CHECK_EQ(crafting_remainder_item(item("minecraft:dragon_breath")),
           item("minecraft:glass_bottle"));
  CHECK_EQ(crafting_remainder_item(item("minecraft:honey_bottle")), item("minecraft:glass_bottle"));
  CHECK_EQ(crafting_remainder_item(item("minecraft:lava_bucket")), item("minecraft:bucket"));
  CHECK_EQ(crafting_remainder_item(item("minecraft:water_bucket")), item("minecraft:bucket"));
  CHECK_EQ(crafting_remainder_item(item("minecraft:milk_bucket")), item("minecraft:bucket"));
  CHECK_EQ(crafting_remainder_item(item("minecraft:potion")), kAirItemId);
  CHECK_EQ(crafting_remainder_item(item("minecraft:nether_wart")), kAirItemId);
  CHECK_EQ(crafting_remainder_item(kAirItemId), kAirItemId);
  // The furnace family observes the same property, so its fuel remainder is
  // this lookup: the one fuel that has one is a lava bucket.
  CHECK_EQ(fuel_remainder_item(item("minecraft:lava_bucket")), item("minecraft:bucket"));
  CHECK_EQ(fuel_remainder_item(item("minecraft:coal")), kAirItemId);
}

void test_container_spec() {
  const auto spec = container_spec_for_block("minecraft:brewing_stand");
  CHECK(spec.has_value());
  CHECK_EQ(spec->slots, kBrewingSlotCount);
  CHECK_EQ(spec->menu_id, 11); // MenuType.BREWING_STAND's static index
  CHECK(spec->brewing);
  CHECK(!spec->cooking);
  CHECK_EQ(spec->double_menu_id, 0);

  BlockContainer c("minecraft:brewing_stand", *spec, {{0, 64, 0}});
  CHECK(c.is_brewing());
  CHECK(c.is_timed());
  CHECK(!c.is_cooking());
  CHECK_EQ(c.total_slots(), 5);
  CHECK_EQ(c.menu_id(), 11);
}

void test_nbt_round_trip() {
  BrewingState state;
  state.brew_time = 137;
  state.fuel = 7;
  nbt::Value be = nbt::make_compound({});
  write_block_entity_brewing(&be, state);

  BrewingState back;
  read_block_entity_brewing(be, &back);
  CHECK_EQ(back.brew_time, 137);
  CHECK_EQ(back.fuel, 7);
  // Neither of the two non-persisted fields comes back: vanilla re-derives the
  // running brew's ingredient from the slot, and the bottle bits start unknown
  // so the first tick writes the block state.
  CHECK_EQ(back.ingredient_item, kAirItemId);
  CHECK(!back.bottles_known);

  // A record from before this milestone (or a hand-written one) has no timers at
  // all, which reads as an idle stand.
  const nbt::Value empty_be = nbt::make_compound({});
  BrewingState fresh;
  fresh.brew_time = 400;
  fresh.fuel = 20;
  read_block_entity_brewing(empty_be, &fresh);
  CHECK_EQ(fresh.brew_time, 0);
  CHECK_EQ(fresh.fuel, 0);

  // A non-compound is ignored rather than read.
  const nbt::Value not_a_compound = nbt::make_int(3);
  BrewingState untouched;
  untouched.brew_time = 12;
  read_block_entity_brewing(not_a_compound, &untouched);
  CHECK_EQ(untouched.brew_time, 12);
}

} // namespace

int main() {
  test_table();
  test_lookups();
  test_mix();
  test_tick_starts_a_brew();
  test_tick_finishes_and_cancels();
  test_tick_needs_mix_and_fuel();
  test_tick_container_mix_and_remainder();
  test_tick_batch_is_cheap_when_idle();
  test_crafting_remainder();
  test_container_spec();
  test_nbt_round_trip();
  if (failures == 0)
    std::printf("brewing: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
