// SPDX-License-Identifier: GPL-3.0-or-later
//
// Brewing (Phase 3 milestone G follow-up): the brewing stand's mix lookups and
// its tick.
//
// Everything here is `PotionBrewing` and `BrewingStandBlockEntity` transcribed,
// because each piece is observable:
//
//  * **Brewing is code, not data.** `PotionBrewing.addVanillaMixes` builds the
//    allowed containers, the container mixes and the potion mixes from items
//    and `Potions` fields, and `BrewingStandBlockEntity` takes its fuel from
//    the `minecraft:brewing_fuel` item tag. The generated `brewing_table.h` is
//    those lists (see tools/gen_brewing_table.py), and a lookup takes the
//    *first* entry whose `from` and `ingredient` both match.
//  * **A mix replaces the whole stack.** `PotionContents.createItemStack` builds
//    a fresh single-item stack, so a bottle's custom effects are *lost* when it
//    is brewed — vanilla's own behaviour, and the reason a brewed potion can
//    only ever carry the named potion's effects.
//  * **The timer cancels.** A brew is 400 ticks, but it only finishes if the
//    stand is still brewable *and* the ingredient slot still holds the item the
//    brew started from (`ingredient.is(entity.ingredient)`); anything else
//    resets `brew_time` to 0 rather than pausing it.
//  * **The fuel is not a flame.** One unit of the tag is worth 20 *uses*, and
//    one use is spent to start a brew, so a stand that is out of fuel keeps its
//    progress bar at 0 rather than draining anything.
//
// The `has_bottle_0..2` block properties and the client syncs are the caller's
// job: the tick only touches one container's five slots and its two counters,
// and reports what moved.
#pragma once

#include <array>
#include <cstdint>

#include "world/brewing_table.h"
#include "world/container.h"
#include "world/inventory.h"
#include "world/item.h"

namespace mc::world {

// ---- lookups ---------------------------------------------------------------

// PotionBrewing.isContainer: one of the three potion items, the sources a mix
// can start from.
bool brewing_is_container(int32_t item);

// PotionBrewing.isIngredient: what the ingredient slot accepts — either half of
// the mix list, the container mixes (gunpowder, dragon's breath) or the potion
// mixes (nether wart, redstone, ...).
bool brewing_is_ingredient(int32_t item);

// `stack.is(ItemTags.BREWING_FUEL)` — what the fuel slot accepts.
bool brewing_is_fuel(int32_t item);

// BrewingStandMenu.PotionSlot.mayPlaceItem: the three potions *and* a plain
// glass bottle, which is how a bottle is put in to be filled.
bool brewing_is_bottle_slot_item(int32_t item);

// PotionBrewing.hasMix: `source` is a container and `ingredient` has a mix with
// it. This is the gate for both "is there anything to brew" and "is this slot
// write allowed".
bool brewing_has_mix(const ItemStack& source, int32_t ingredient);

// PotionBrewing.mix: `ingredient` plus `source` becomes the new stack.
//
// A source with no potion at all (a glass bottle, an empty slot) is returned
// unchanged, as is a source no mix matches — `doBrew` calls this on all three
// bottle slots and only the ones with a mix change. The result is a fresh
// single-item stack, so a brewed bottle loses its custom effects.
ItemStack brewing_mix(int32_t ingredient, const ItemStack& source);

// ---- the brewing tick ------------------------------------------------------

// What one tick did, so the caller can sync the clients and persist.
struct BrewingTickResult {
  bool changed = false;                    // anything at all: mark the chunk dirty
  bool data_changed = false;               // the two counters moved: set_data
  bool bottle_bits_changed = false;        // the `has_bottle_0..2` properties moved
  bool bottles[3] = {false, false, false}; // the new bits (when changed)
  bool brewed = false;                     // a brew just finished: level event 1035
  // The ingredient's crafting remainder when the ingredient slot was emptied by
  // *other* contents and the remainder had nowhere to go
  // (`Containers.dropItemStack`). Empty for every other tick; the caller spawns
  // it, because the tick cannot reach item entities.
  ItemStack drop_remainder;
};

// One tick of `BrewingStandBlockEntity.serverTick` over a brewing stand's five
// slots: 0..2 the bottles, 3 the ingredient, 4 the fuel. `state` advances in
// place.
BrewingTickResult tick_brewing(BrewingState& state, Inventory& slots);

} // namespace mc::world
