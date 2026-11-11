// SPDX-License-Identifier: GPL-3.0-or-later
//
// The recipe book (Phase 3 milestone D2): the `update_recipes` and
// `recipe_book_add` bodies the server pushes at join, and the `place_recipe`
// semantics that fill a crafting grid from one of those entries.
//
// Wire layouts are read off the decompiled 26.2 codecs rather than a wiki:
//
//   ClientboundUpdateRecipesPacket
//     [varint 7]  x([Identifier key][varint count][count x varint item])
//     [varint n]  x([Ingredient input][SlotDisplay result])       <- stonecutter
//
//   ClientboundRecipeBookAddPacket
//     [varint n] x Entry{ RecipeDisplayEntry, [u8 flags] } [u8 replace]
//
//   RecipeDisplayEntry
//     [varint display id][RecipeDisplay][varint group+1]
//     [varint RecipeBookCategory][optional crafting requirements]
//
//   RecipeDisplay = [varint type id][type payload]
//   SlotDisplay  = [varint type id][type payload]
//   Ingredient   = HolderSet<Item>: [varint count+1][count x varint item], or a
//                  lone 0 followed by a resource location for a tag (the table
//                  expands tags, so every ingredient here is the direct form)
//
// ClientboundPlaceGhostRecipePacket is [varint container id][RecipeDisplay].
//
// `update_recipes` is *not* the recipe list — it carries the seven
// RecipePropertySets (which items each menu slot accepts) and the stonecutter's
// selectable recipe list. The recipes themselves, and the ids `place_recipe`
// refers to, arrive in `recipe_book_add`: a RecipeDisplayId is only ever an
// index into the list this server sent.
//
// Display ids are ours: vanilla indexes its recipe-map order, which is not
// portable, and every id we hand out is looked up from the same table that
// produced it.
#pragma once

#include <cstdint>
#include <vector>

#include "net/byte_buffer.h"
#include "world/inventory.h"
#include "world/item.h"
#include "world/recipe.h"

namespace mc::world {

// minecraft:recipe_display registry ids (RecipeDisplays.bootstrap order).
enum class DisplayKind : uint8_t {
  CraftingShapeless = 0,
  CraftingShaped = 1,
  Furnace = 2,
  Stonecutter = 3,
  Smithing = 4,
};

// minecraft:slot_display registry ids (SlotDisplays.bootstrap order).
enum class SlotKind : uint8_t {
  Empty = 0,
  AnyFuel = 1,
  WithAnyPotion = 2,
  OnlyWithComponent = 3,
  Item = 4,
  ItemStack = 5,
  Tag = 6,
  Dyed = 7,
  SmithingTrim = 8,
  WithRemainder = 9,
  Composite = 10,
};

// minecraft:recipe_book_category registry ids (RecipeBookCategories), which are
// also the book's tabs. A tab list is per *menu type* (Crafting for the book of
// a crafting menu, and so on); an entry in a category the open menu does not
// list is dropped without a word, which is why the value is carried in the
// generated table instead of being guessed here.
enum class BookCategory : uint8_t {
  CraftingBuildingBlocks = 0,
  CraftingRedstone = 1,
  CraftingEquipment = 2,
  CraftingMisc = 3,
  FurnaceFood = 4,
  FurnaceBlocks = 5,
  FurnaceMisc = 6,
  BlastFurnaceBlocks = 7,
  BlastFurnaceMisc = 8,
  SmokerFood = 9,
  Stonecutter = 10,
  Smithing = 11,
  Campfire = 12,
};

// One display the server can send: the recipe behind it and the shape of the
// RecipeDisplay it becomes.
struct RecipeBookDisplay {
  const GeneratedRecipe* recipe = nullptr;
  DisplayKind kind = DisplayKind::CraftingShaped;
  // A transmute recipe has one display per legal material count (vanilla's
  // TransmuteRecipe.display loops min..max and grows the result with them), so
  // `material_count` says which one this is. 0 for every other family, whose
  // displays are one per recipe.
  uint8_t material_count = 0;
};

// Every display the server sends, in id order. Everything the table can
// describe except `crafting_special_*` (vanilla gives those no display at all —
// `Recipe.display()` defaults to empty). A `smithing_trim` display is a
// smithing one whose *result* slot is a base + material + pattern demo rather
// than an item stack (`SlotDisplay.SmithingTrimDemoSlotDisplay`), the way
// `SmithingTrimRecipe.display()` builds it: the trimmed piece is whatever base
// the player supplies, so no single item can stand for the result.
const std::vector<RecipeBookDisplay>& recipe_book_displays();

// The display behind a RecipeDisplayId, or nullptr when the id is not ours.
const RecipeBookDisplay* recipe_book_display(int32_t id);

// --- packet bodies (the caller writes the packet id first) ------------------

// ClientboundUpdateRecipesPacket. The property sets are derived from the table
// the way RecipeManager.prepare derives them: the union of every recipe
// family's items for that slot.
void write_update_recipes(mc::net::Writer& w);

// ClientboundRecipeBookAddPacket: `ids` as entries with `flags`, or every
// display when `ids` is empty, then the replace flag.
void write_recipe_book_add(mc::net::Writer& w, const std::vector<int32_t>& ids, uint8_t flags,
                           bool replace);

// ClientboundRecipeBookSettingsPacket: CRAFTING, FURNACE, BLAST_FURNACE and
// SMOKER, each an (open, filtering) pair. Only the crafting book is modelled,
// so the other three go out at their default (closed, no filtering).
void write_recipe_book_settings(mc::net::Writer& w, bool crafting_open, bool crafting_filtering);

// ClientboundPlaceGhostRecipePacket: the window to ghost the recipe in and the
// display to draw there. The caller writes one only for an id it already
// resolved, so the display comes in rather than an index that could be stale.
void write_place_ghost_recipe(mc::net::Writer& w, int container_id,
                              const RecipeBookDisplay& display);

// --- place_recipe -----------------------------------------------------------

enum class PlaceOutcome {
  Placed,  // the grid now holds the recipe
  Ghost,   // the player cannot craft it: the grid was emptied, ghost it instead
  Refused, // nothing changed (the grid could not be emptied, or does not fit)
};

// ServerPlaceRecipe.placeRecipe: put one craft of `display` (or as many as the
// inventory allows when `use_max_items`, which is what right-clicking a recipe
// asks for) into `grid`, taking the ingredients out of `inv`. `grid` is
// grid_width x grid_height, row-major — 2x2 for the player menu, 3x3 for a
// crafting table.
//
// What the grid already held is returned to `inv`; whatever does not fit is
// appended to `spilled` for the caller to drop at the player's feet, but only
// when `allow_dropping_items` is set (vanilla passes the player's creative
// flag). In survival a grid that cannot be emptied is `Refused` and nothing
// moves, which is `ServerPlaceRecipe.testClearGrid`.
PlaceOutcome place_recipe_from_book(const RecipeBookDisplay& display, int grid_width,
                                    int grid_height, Inventory& grid, Inventory& inv,
                                    int selected_slot, bool use_max_items,
                                    bool allow_dropping_items, std::vector<ItemStack>* spilled);

// The same for a furnace family menu, whose "grid" is its input slot alone:
// AbstractFurnaceMenu.handlePlacement (grid 1x1 over slot 0, with slots 0 and 2
// cleared first). `slots` is the *container's* three slots — input, fuel,
// result — and the fuel slot is never touched, so a book click cannot eat the
// coal that is already burning. Returns `Refused` for a display that is not a
// cooking recipe.
PlaceOutcome place_furnace_recipe_from_book(const RecipeBookDisplay& display, Inventory& slots,
                                            Inventory& inv, int selected_slot, bool use_max_items,
                                            bool allow_dropping_items,
                                            std::vector<ItemStack>* spilled);

} // namespace mc::world
