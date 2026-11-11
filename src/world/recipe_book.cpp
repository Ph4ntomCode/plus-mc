// SPDX-License-Identifier: GPL-3.0-or-later
//
// The recipe book. See recipe_book.h for the wire layouts and for why
// `update_recipes` is not the recipe list.
#include "world/recipe_book.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "world/trim.h"

namespace mc::world {
namespace {

// ---- small lookups ---------------------------------------------------------

int32_t item_by_name(std::string_view name) {
  return ItemRegistry::instance().id_of(name).value_or(kAirItemId);
}

// The crafting station a display page shows. Vanilla names one per family
// (`Items.CRAFTING_TABLE`, `furnaceIcon()`, ...).
int32_t crafting_station(RecipeKind kind) {
  switch (kind) {
  case RecipeKind::Shaped:
  case RecipeKind::Shapeless:
  case RecipeKind::Transmute:
    return item_by_name("minecraft:crafting_table");
  case RecipeKind::Smelting:
    return item_by_name("minecraft:furnace");
  case RecipeKind::Blasting:
    return item_by_name("minecraft:blast_furnace");
  case RecipeKind::Smoking:
    return item_by_name("minecraft:smoker");
  case RecipeKind::Campfire:
    return item_by_name("minecraft:campfire");
  case RecipeKind::Stonecutting:
    return item_by_name("minecraft:stonecutter");
  case RecipeKind::SmithingTransform:
  case RecipeKind::SmithingTrim:
    return item_by_name("minecraft:smithing_table");
  case RecipeKind::Special:
    break;
  }
  return kAirItemId;
}

// The top bit of material_max is `add_material_count_to_result`, not a bound
// (the generated header records it there).
int transmute_max_materials(const GeneratedRecipe& recipe) {
  return recipe.material_max & 0x7F;
}

int transmute_result_count(const GeneratedRecipe& recipe, int materials) {
  return (recipe.material_max & 0x80) != 0 ? materials + recipe.result_count : recipe.result_count;
}

// The pattern a `smithing_trim` recipe applies, or -1 when its template cell
// names no template item. The generated table pins the 18 pairs 1:1 (each
// pattern's template appears in exactly one trim recipe, world/trim.h), so this
// is the pattern the recipe is *about* — and the one number the display of such
// a recipe carries that its three ingredients do not.
int32_t trim_pattern_of(const GeneratedRecipe& recipe) {
  const GeneratedIngredient* cells = recipe_cells(recipe);
  if (recipe.ing_count < 1)
    return -1;
  for (int32_t item : ingredient_items(cells[0])) {
    const int32_t pattern = trim_pattern_for_template(item);
    if (pattern >= 0)
      return pattern;
  }
  return -1;
}

// ---- slot displays ---------------------------------------------------------

void write_slot_kind(mc::net::Writer& w, SlotKind kind) {
  w.varint(static_cast<uint32_t>(kind));
}

// SlotDisplay.ItemSlotDisplay: [varint item].
void write_item_slot(mc::net::Writer& w, int32_t item) {
  write_slot_kind(w, SlotKind::Item);
  w.varint(static_cast<uint32_t>(item));
}

// SlotDisplay.ItemStackSlotDisplay: [varint item][varint count][components].
// ItemStackTemplate.STREAM_CODEC writes the item *before* the count — the
// opposite order from the ItemStack codec — and every result here carries no
// components (an empty DataComponentPatch is two zero varints).
void write_stack_slot(mc::net::Writer& w, int32_t item, int count) {
  write_slot_kind(w, SlotKind::ItemStack);
  w.varint(static_cast<uint32_t>(item));
  w.varint(static_cast<uint32_t>(count));
  w.varint(0); // added components
  w.varint(0); // removed component types
}

// Ingredient.display(): a direct item set becomes a Composite of one item
// display per item. Vanilla wraps an item that has a crafting remainder in
// SlotDisplay.WithRemainder; remainders are not in the item table yet, so the
// plain item display is what goes out (docs/PHASE3.md).
void write_ingredient_slot(mc::net::Writer& w, const GeneratedIngredient& ingredient) {
  write_slot_kind(w, SlotKind::Composite);
  w.varint(ingredient.item_count);
  const uint16_t* items = kRecipeItems + ingredient.first_item;
  for (uint32_t i = 0; i < ingredient.item_count; ++i)
    write_item_slot(w, static_cast<int32_t>(items[i]));
}

// Ingredient.CONTENTS_STREAM_CODEC (ByteBufCodecs.holderSet): a *direct* set is
// `[varint count + 1]` then that many varint item ids; a lone 0 means a tag
// follows as a resource location. The generated table expands every tag into
// its item set, so this side is always the direct form.
void write_ingredient(mc::net::Writer& w, const GeneratedIngredient& ingredient) {
  w.varint(static_cast<uint32_t>(ingredient.item_count) + 1);
  const uint16_t* items = kRecipeItems + ingredient.first_item;
  for (uint32_t i = 0; i < ingredient.item_count; ++i)
    w.varint(items[i]);
}

// ---- recipe displays -------------------------------------------------------

// TransmuteRecipe.resultDisplay: a transmute whose result carries no item
// (map cloning's JSON `result` is `{}` — the input keeps its components) is
// drawn as a Composite of the input's items, one ItemStack per item, as tall
// as the result count. Sending an ItemStack of air instead is not just wrong,
// it fails the client's decode: ItemStackTemplate's constructor throws
// "Item must be non-empty".
void write_transmute_result(mc::net::Writer& w, const GeneratedRecipe& recipe, int materials) {
  const int count = transmute_result_count(recipe, materials);
  if (recipe.result_item != kAirItemId) {
    write_stack_slot(w, recipe.result_item, count);
    return;
  }
  const GeneratedIngredient& input = recipe_cells(recipe)[0];
  write_slot_kind(w, SlotKind::Composite);
  w.varint(input.item_count);
  const uint16_t* items = kRecipeItems + input.first_item;
  for (uint32_t i = 0; i < input.item_count; ++i)
    write_stack_slot(w, static_cast<int32_t>(items[i]), count);
}

void write_recipe_display(mc::net::Writer& w, const RecipeBookDisplay& display) {
  const GeneratedRecipe& recipe = *display.recipe;
  const GeneratedIngredient* cells = recipe_cells(recipe);
  const int32_t station = crafting_station(recipe.kind);

  w.varint(static_cast<uint32_t>(display.kind));
  switch (display.kind) {
  case DisplayKind::CraftingShaped:
    w.varint(recipe.width);
    w.varint(recipe.height);
    // Row-major, one slot display per pattern cell; a gap is SlotDisplay.Empty
    // (the pattern is stored shrunk, so the box has no empty border). The list
    // carries its own count, which the record checks against width * height.
    w.varint(recipe.width * recipe.height);
    for (int i = 0; i < recipe.width * recipe.height; ++i) {
      if (cells[i].item_count == 0)
        write_slot_kind(w, SlotKind::Empty);
      else
        write_ingredient_slot(w, cells[i]);
    }
    write_stack_slot(w, recipe.result_item, recipe.result_count);
    write_item_slot(w, station);
    break;
  case DisplayKind::CraftingShapeless:
    if (recipe.kind == RecipeKind::Transmute) {
      // TransmuteRecipe.display: one material stack per count in min..max, and
      // the result grows with the material count.
      w.varint(static_cast<uint32_t>(display.material_count) + 1);
      write_ingredient_slot(w, cells[0]);
      for (int i = 0; i < display.material_count; ++i)
        write_ingredient_slot(w, cells[1]);
      write_transmute_result(w, recipe, display.material_count);
    } else {
      w.varint(recipe.ing_count);
      for (int i = 0; i < recipe.ing_count; ++i)
        write_ingredient_slot(w, cells[i]);
      write_stack_slot(w, recipe.result_item, recipe.result_count);
    }
    write_item_slot(w, station);
    break;
  case DisplayKind::Furnace:
    write_ingredient_slot(w, cells[0]);
    write_slot_kind(w, SlotKind::AnyFuel);
    write_stack_slot(w, recipe.result_item, recipe.result_count);
    write_item_slot(w, station);
    w.varint(recipe.cook_time);
    // The generated table keeps experience as milli-xp, the JSON's float.
    w.f32(static_cast<float>(recipe.xp_milli) / 1000.0f);
    break;
  case DisplayKind::Stonecutter:
    write_ingredient_slot(w, cells[0]);
    write_stack_slot(w, recipe.result_item, recipe.result_count);
    write_item_slot(w, station);
    break;
  case DisplayKind::Smithing:
    write_ingredient_slot(w, cells[0]); // template
    write_ingredient_slot(w, cells[1]); // base
    write_ingredient_slot(w, cells[2]); // addition
    if (recipe.kind == RecipeKind::SmithingTrim) {
      // SmithingTrimRecipe.display(): the result of a trim is not one item but
      // a *demo* — SlotDisplay.SmithingTrimDemoSlotDisplay, the client applies
      // the pattern to every (base, material) pair — because the trimmed piece
      // is whatever base the player puts in, carrying the `minecraft:trim`
      // component (docs/PHASE3.md). The pattern goes out as a Holder, id + 1.
      const int32_t pattern = trim_pattern_of(recipe);
      if (pattern >= 0) {
        write_slot_kind(w, SlotKind::SmithingTrim);
        write_ingredient_slot(w, cells[1]); // base
        write_ingredient_slot(w, cells[2]); // material
        w.varint(static_cast<uint32_t>(pattern + 1));
      } else {
        // Unreachable for the generated table (build_displays drops such a
        // recipe): an empty slot still reads as a complete display, whereas
        // the id of -1 + 1 = 0 would announce a *direct* holder and desync.
        write_slot_kind(w, SlotKind::Empty);
      }
    } else {
      write_stack_slot(w, recipe.result_item, recipe.result_count);
    }
    write_item_slot(w, station);
    break;
  }
}

// Vanilla's PlacementInfo.ingredients(): the distinct *uses* of ingredients the
// recipe places, in cell order, which is also what a recipe book entry carries
// as its crafting requirements.
std::vector<const GeneratedIngredient*> placement_ingredients(const GeneratedRecipe& recipe) {
  const GeneratedIngredient* cells = recipe_cells(recipe);
  std::vector<const GeneratedIngredient*> out;
  switch (recipe.kind) {
  case RecipeKind::Shaped:
  case RecipeKind::SmithingTransform:
    // PlacementInfo.createFromOptionals: only the present cells.
    for (int i = 0; i < recipe.ing_count; ++i)
      if (cells[i].item_count != 0)
        out.push_back(&cells[i]);
    break;
  case RecipeKind::Shapeless:
  case RecipeKind::SmithingTrim:
    for (int i = 0; i < recipe.ing_count; ++i)
      out.push_back(&cells[i]);
    break;
  case RecipeKind::Transmute:
    // TransmuteRecipe.createPlacementInfo: the input plus the *maximum* number
    // of materials, whichever material count a display happens to show.
    out.push_back(&cells[0]);
    for (int i = 0; i < transmute_max_materials(recipe); ++i)
      out.push_back(&cells[1]);
    break;
  case RecipeKind::Smelting:
  case RecipeKind::Blasting:
  case RecipeKind::Smoking:
  case RecipeKind::Campfire:
  case RecipeKind::Stonecutting:
    // SingleItemRecipe.placementInfo = PlacementInfo.create(input).
    out.push_back(&cells[0]);
    break;
  case RecipeKind::Special:
    break;
  }
  return out;
}

void write_entry(mc::net::Writer& w, int32_t id, const RecipeBookDisplay& display, uint8_t flags) {
  w.varint(static_cast<uint32_t>(id));
  write_recipe_display(w, display);
  // OptionalInt group: vanilla groups recipes that share a JSON `group` so one
  // book entry cycles through them. The table does not carry the group string,
  // so every display is its own entry (docs/PHASE3.md).
  w.varint(0);
  // RecipeBookCategory registry id, from the recipe JSON's `category` through
  // the family's recipeBookCategory() (the generator records it in the table).
  w.varint(static_cast<uint32_t>(display.recipe->book_category));
  // Optional<List<Ingredient>> crafting requirements: what the client needs to
  // know whether the player can craft it. Vanilla leaves it empty for a special
  // recipe; every display here has one.
  const std::vector<const GeneratedIngredient*> requirements =
      placement_ingredients(*display.recipe);
  w.u8(requirements.empty() ? 0 : 1);
  if (!requirements.empty()) {
    w.varint(static_cast<uint32_t>(requirements.size()));
    for (const GeneratedIngredient* ingredient : requirements)
      write_ingredient(w, *ingredient);
  }
  w.u8(flags);
}

std::vector<RecipeBookDisplay> build_displays() {
  std::vector<RecipeBookDisplay> out;
  for (uint32_t i = 0; i < kRecipeCount; ++i) {
    const GeneratedRecipe& recipe = kRecipes[i];
    switch (recipe.kind) {
    case RecipeKind::Shaped:
      out.push_back({&recipe, DisplayKind::CraftingShaped, 0});
      break;
    case RecipeKind::Shapeless:
      out.push_back({&recipe, DisplayKind::CraftingShapeless, 0});
      break;
    case RecipeKind::Transmute:
      for (int materials = recipe.material_min; materials <= transmute_max_materials(recipe);
           ++materials)
        out.push_back({&recipe, DisplayKind::CraftingShapeless, static_cast<uint8_t>(materials)});
      break;
    case RecipeKind::Smelting:
    case RecipeKind::Blasting:
    case RecipeKind::Smoking:
    case RecipeKind::Campfire:
      out.push_back({&recipe, DisplayKind::Furnace, 0});
      break;
    case RecipeKind::Stonecutting:
      out.push_back({&recipe, DisplayKind::Stonecutter, 0});
      break;
    case RecipeKind::SmithingTransform:
      out.push_back({&recipe, DisplayKind::Smithing, 0});
      break;
    case RecipeKind::SmithingTrim:
      // A trim's display is the transform display with a base + material +
      // pattern demo in the result slot (recipe_book.h). A recipe whose
      // template cell names no template item cannot be drawn at all, and the
      // generated table has none.
      if (trim_pattern_of(recipe) >= 0)
        out.push_back({&recipe, DisplayKind::Smithing, 0});
      break;
    case RecipeKind::Special:
      // No display this server can draw honestly: the code-driven specials
      // have no display in vanilla either (`Recipe.display()` defaults to
      // empty).
      break;
    }
  }
  return out;
}

// ---- update_recipes --------------------------------------------------------

constexpr uint32_t kind_bit(RecipeKind kind) {
  return 1u << static_cast<uint32_t>(kind);
}

constexpr uint32_t kSmithingKinds =
    kind_bit(RecipeKind::SmithingTransform) | kind_bit(RecipeKind::SmithingTrim);

// RecipeManager.RECIPE_PROPERTY_SETS: which families fill each set and which of
// the recipe's cells is the slot it gates. Smithing's are the three input
// slots; each cooking family gates its single input.
struct PropertySet {
  const char* key;
  uint32_t kinds;
  int cell;
};

constexpr PropertySet kPropertySets[] = {
    {"minecraft:smithing_base", kSmithingKinds, 1},
    {"minecraft:smithing_template", kSmithingKinds, 0},
    {"minecraft:smithing_addition", kSmithingKinds, 2},
    {"minecraft:furnace_input", kind_bit(RecipeKind::Smelting), 0},
    {"minecraft:blast_furnace_input", kind_bit(RecipeKind::Blasting), 0},
    {"minecraft:smoker_input", kind_bit(RecipeKind::Smoking), 0},
    {"minecraft:campfire_input", kind_bit(RecipeKind::Campfire), 0},
};
constexpr uint32_t kPropertySetCount = sizeof(kPropertySets) / sizeof(kPropertySets[0]);

// RecipePropertySet.create(ingredients): the union of every ingredient's items,
// in table order (vanilla builds a Set here, so its order is not portable — the
// client only tests membership).
std::vector<int32_t> property_set_items(const PropertySet& set) {
  std::vector<int32_t> out;
  for (uint32_t i = 0; i < kRecipeCount; ++i) {
    const GeneratedRecipe& recipe = kRecipes[i];
    if ((set.kinds & kind_bit(recipe.kind)) == 0 || set.cell >= recipe.ing_count)
      continue;
    for (int32_t item : ingredient_items(recipe_cells(recipe)[set.cell]))
      if (std::find(out.begin(), out.end(), item) == out.end())
        out.push_back(item);
  }
  return out;
}

// ---- place_recipe ----------------------------------------------------------

// Vanilla's PlacementInfo + PlaceRecipeHelper in one step: for every grid cell
// that takes an ingredient, which ingredient. Equal ingredient *sets* share one
// entry, so their copies pool — which is what vanilla's bipartite assignment
// does for interchangeable ingredients.
struct GridLayout {
  std::vector<const GeneratedIngredient*> ingredients;
  std::vector<int> cells; // grid_width * grid_height, -1 = the cell stays empty
  std::vector<int> uses;  // how many cells each ingredient feeds
};

GridLayout grid_layout_for(const GeneratedRecipe& recipe, int grid_width, int grid_height) {
  GridLayout layout;
  if (grid_width <= 0 || grid_height <= 0)
    return layout;
  layout.cells.assign(static_cast<size_t>(grid_width) * static_cast<size_t>(grid_height), -1);

  auto index_of = [&layout](const GeneratedIngredient& ingredient) {
    for (size_t i = 0; i < layout.ingredients.size(); ++i) {
      const GeneratedIngredient* other = layout.ingredients[i];
      if (other->first_item == ingredient.first_item && other->item_count == ingredient.item_count)
        return static_cast<int>(i);
    }
    layout.ingredients.push_back(&ingredient);
    layout.uses.push_back(0);
    return static_cast<int>(layout.ingredients.size()) - 1;
  };
  auto place = [&layout, &index_of](int cell, const GeneratedIngredient& ingredient) {
    const int index = index_of(ingredient);
    layout.cells[static_cast<size_t>(cell)] = index;
    ++layout.uses[static_cast<size_t>(index)];
  };

  if (recipe.kind == RecipeKind::Shaped) {
    if (recipe.width > grid_width || recipe.height > grid_height)
      return {}; // does not fit this menu's grid
    // PlaceRecipeHelper only centres a pattern that is *smaller* than half the
    // grid (Mth.floor(gridWidth / 2.0f - recipeWidth / 2.0f)); otherwise it
    // starts at 0. That is why a 2x2 recipe sits in the top left of a 3x3 while
    // a single item is centred.
    const int x0 = recipe.width * 2 < grid_width ? grid_width / 2 - recipe.width / 2 : 0;
    const int y0 = recipe.height * 2 < grid_height ? grid_height / 2 - recipe.height / 2 : 0;
    const GeneratedIngredient* cells = recipe_cells(recipe);
    for (int y = 0; y < recipe.height; ++y)
      for (int x = 0; x < recipe.width; ++x)
        if (cells[x + y * recipe.width].item_count != 0)
          place((x0 + x) + (y0 + y) * grid_width, cells[x + y * recipe.width]);
    return layout;
  }

  // Everything else fills the grid row-major, one cell per ingredient:
  // PlaceRecipeHelper is handed the grid itself as the recipe box, so nothing
  // is centred. A transmute recipe places its input plus the *maximum* material
  // count, whatever a particular display shows.
  const GeneratedIngredient* cells = recipe_cells(recipe);
  const int count =
      recipe.kind == RecipeKind::Transmute ? 1 + transmute_max_materials(recipe) : recipe.ing_count;
  if (count > grid_width * grid_height)
    return {};
  for (int i = 0; i < count; ++i)
    place(i, recipe.kind == RecipeKind::Transmute && i > 0 ? cells[1] : cells[i]);
  return layout;
}

bool grid_holds_recipe(const Inventory& grid, int width, int height,
                       const GeneratedRecipe& recipe) {
  CraftingGrid view = CraftingGrid::of(width, height);
  for (int i = 0; i < width * height && i < grid.size(); ++i)
    view.items[static_cast<size_t>(i)] = grid.get(i);
  const std::optional<CraftingMatch> match = find_crafting_recipe(view);
  return match.has_value() && match->recipe == &recipe;
}

int smallest_grid_stack(const Inventory& grid) {
  int smallest = INT_MAX;
  for (const ItemStack& stack : grid.slots())
    if (!stack.empty() && stack.count < smallest)
      smallest = stack.count;
  return smallest == INT_MAX ? 0 : smallest;
}

// The number of `item` the player holds across the inventory and the craft
// grid — StackedItemContents over Inventory.fillStackedContents and
// menu.fillCraftSlotsStackedContents.
int available_of(const Inventory& inv, const Inventory& grid, int32_t item) {
  int total = 0;
  for (const ItemStack& stack : inv.slots())
    if (stack.item == item && !stack.empty())
      total += stack.count;
  for (const ItemStack& stack : grid.slots())
    if (stack.item == item && !stack.empty())
      total += stack.count;
  return total;
}

// Moves up to `count` of `item` out of the inventory and into a grid cell.
// ServerPlaceRecipe.moveItemToGrid picks one item per ingredient and takes it
// from the inventory slot that matches; which of several equal stacks is
// drained is not observable, so the fullest one goes first.
int move_into_cell(Inventory& inv, Inventory& grid, int cell, int32_t item, int count) {
  int best = -1;
  for (int slot = 0; slot < inv.size(); ++slot) {
    const ItemStack& stack = inv.get(slot);
    if (stack.empty() || stack.item != item)
      continue;
    if (best < 0 || stack.count > inv.get(best).count)
      best = slot;
  }
  if (best < 0)
    return 0;
  ItemStack from = inv.get(best);
  const int take = std::min(count, from.count);
  from.count -= take;
  if (from.count <= 0)
    from.clear();
  inv.set(best, from);

  ItemStack to = grid.get(cell);
  if (to.empty()) {
    // Built by hand rather than with of_id: a zero-count stack is *normalized
    // to air* there (ItemStack.of_id), and the count arrives just below.
    to = ItemStack{};
    to.item = item;
  }
  to.count += take;
  grid.set(cell, to);
  return take;
}

} // namespace

const std::vector<RecipeBookDisplay>& recipe_book_displays() {
  static const std::vector<RecipeBookDisplay> displays = build_displays();
  return displays;
}

const RecipeBookDisplay* recipe_book_display(int32_t id) {
  const std::vector<RecipeBookDisplay>& all = recipe_book_displays();
  if (id < 0 || static_cast<size_t>(id) >= all.size())
    return nullptr;
  return &all[static_cast<size_t>(id)];
}

void write_update_recipes(mc::net::Writer& w) {
  w.varint(kPropertySetCount);
  for (const PropertySet& set : kPropertySets) {
    w.string(set.key);
    const std::vector<int32_t> items = property_set_items(set);
    w.varint(static_cast<uint32_t>(items.size()));
    for (int32_t item : items)
      w.varint(static_cast<uint32_t>(item));
  }
  // The stonecutter's selectable list, in the order stonecutting_recipes_for
  // filters it by input: the client sends back an index into *its* filtered
  // copy, so the two orders have to agree.
  const std::vector<const GeneratedRecipe*> cutters = stonecutting_recipes();
  w.varint(static_cast<uint32_t>(cutters.size()));
  for (const GeneratedRecipe* recipe : cutters) {
    write_ingredient(w, recipe_cells(*recipe)[0]);
    write_stack_slot(w, recipe->result_item, recipe->result_count);
  }
}

void write_recipe_book_add(mc::net::Writer& w, const std::vector<int32_t>& ids, uint8_t flags,
                           bool replace) {
  const std::vector<RecipeBookDisplay>& all = recipe_book_displays();
  if (ids.empty()) {
    w.varint(static_cast<uint32_t>(all.size()));
    for (size_t i = 0; i < all.size(); ++i)
      write_entry(w, static_cast<int32_t>(i), all[i], flags);
  } else {
    // Resolve first: the count is written before the entries, so an id that is
    // not ours has to be dropped *before* the count is decided. Writing the
    // requested size and then skipping the id would announce more entries than
    // the body holds, and the client would read the next field out of a
    // neighbouring entry.
    std::vector<int32_t> resolved;
    resolved.reserve(ids.size());
    for (int32_t id : ids)
      if (recipe_book_display(id))
        resolved.push_back(id);
    w.varint(static_cast<uint32_t>(resolved.size()));
    for (int32_t id : resolved)
      write_entry(w, id, *recipe_book_display(id), flags);
  }
  w.u8(replace ? 1 : 0);
}

void write_recipe_book_settings(mc::net::Writer& w, bool crafting_open, bool crafting_filtering) {
  w.u8(crafting_open ? 1 : 0);
  w.u8(crafting_filtering ? 1 : 0);
  // FURNACE, BLAST_FURNACE and SMOKER keep RecipeBookSettings.TypeSettings
  // .DEFAULT (closed, no filtering) until those menus exist.
  for (int i = 0; i < 3; ++i) {
    w.u8(0);
    w.u8(0);
  }
}

void write_place_ghost_recipe(mc::net::Writer& w, int container_id,
                              const RecipeBookDisplay& display) {
  w.varint(static_cast<uint32_t>(container_id));
  write_recipe_display(w, display);
}

PlaceOutcome place_recipe_from_book(const RecipeBookDisplay& display, int grid_width,
                                    int grid_height, Inventory& grid, Inventory& inv,
                                    int selected_slot, bool use_max_items,
                                    bool allow_dropping_items, std::vector<ItemStack>* spilled) {
  const GeneratedRecipe& recipe = *display.recipe;
  const GridLayout layout = grid_layout_for(recipe, grid_width, grid_height);
  if (layout.ingredients.empty())
    return PlaceOutcome::Refused; // the recipe does not fit this grid

  const bool matches = grid_holds_recipe(grid, grid_width, grid_height, recipe);

  // One item per ingredient: the accepted one the player has most of. Vanilla
  // lets StackedItemContents assign items to ingredients (a bipartite matching
  // over the whole set); picking the most plentiful accepted item per
  // ingredient agrees with it whenever an ingredient's items are
  // interchangeable, and the clamp below keeps the rest honest.
  std::vector<int32_t> chosen(layout.ingredients.size(), kAirItemId);
  std::vector<int> chosen_available(layout.ingredients.size(), 0);
  int max_crafts = INT_MAX;
  for (size_t g = 0; g < layout.ingredients.size(); ++g) {
    for (int32_t item : ingredient_items(*layout.ingredients[g])) {
      const int total = available_of(inv, grid, item);
      if (total > chosen_available[g]) {
        chosen_available[g] = total;
        chosen[g] = item;
      }
    }
    max_crafts = std::min(max_crafts, chosen_available[g] / layout.uses[g]);
  }
  if (max_crafts < 1)
    max_crafts = 0;

  // ServerPlaceRecipe.placeRecipe's guard: when the grid already holds the
  // recipe and a cell has no room for one more craft, nothing happens at all.
  if (matches) {
    for (const ItemStack& stack : grid.slots()) {
      if (stack.empty())
        continue;
      if (std::min(max_crafts, stack.max_stack_size()) < stack.count + 1)
        return PlaceOutcome::Refused;
    }
  }

  int amount = use_max_items ? max_crafts : (matches ? smallest_grid_stack(grid) + 1 : 1);
  amount = std::max(amount, 1);
  // clampToMaxStackSize: never prepare more of an ingredient than it stacks to.
  for (size_t g = 0; g < layout.ingredients.size(); ++g)
    amount = std::min(amount, ItemRegistry::instance().max_stack_size(chosen[g]));

  // ServerPlaceRecipe.clearGrid: the grid belongs to the menu, so its contents
  // go back to the inventory. A grid that will not fit there is left standing
  // in survival (testClearGrid) and only spills in creative.
  Inventory trial = inv;
  std::vector<ItemStack> overflow;
  for (int i = 0; i < grid.size(); ++i) {
    const ItemStack held = grid.get(i);
    if (held.empty())
      continue;
    const int leftover = trial.add(held, selected_slot);
    if (leftover <= 0)
      continue;
    if (!allow_dropping_items)
      return PlaceOutcome::Refused; // nothing has moved: the grid still stands
    ItemStack rest = held;
    rest.count = leftover;
    overflow.push_back(rest);
  }
  inv = trial;
  grid.clear_all();
  if (spilled && !overflow.empty())
    spilled->insert(spilled->end(), overflow.begin(), overflow.end());

  // Clearing can spill items out of the world (creative only), and vanilla
  // counts the grid's own contents as available while it plans, so what is
  // *left* decides the final count. Nothing left means the ghost path.
  for (size_t g = 0; g < layout.ingredients.size(); ++g) {
    const int crafts = available_of(inv, grid, chosen[g]) / layout.uses[g];
    amount = std::min(amount, crafts);
  }
  if (amount < 1)
    return PlaceOutcome::Ghost;

  for (size_t cell = 0; cell < layout.cells.size(); ++cell) {
    const int ingredient = layout.cells[cell];
    if (ingredient < 0)
      continue;
    int remaining = amount;
    while (remaining > 0) {
      const int taken = move_into_cell(inv, grid, static_cast<int>(cell),
                                       chosen[static_cast<size_t>(ingredient)], remaining);
      if (taken <= 0)
        break;
      remaining -= taken;
    }
  }
  return PlaceOutcome::Placed;
}

PlaceOutcome place_furnace_recipe_from_book(const RecipeBookDisplay& display, Inventory& slots,
                                            Inventory& inv, int selected_slot, bool use_max_items,
                                            bool allow_dropping_items,
                                            std::vector<ItemStack>* spilled) {
  const GeneratedRecipe& recipe = *display.recipe;
  const bool cooking = recipe.kind == RecipeKind::Smelting || recipe.kind == RecipeKind::Blasting ||
                       recipe.kind == RecipeKind::Smoking;
  if (!cooking || recipe.ing_count != 1 || slots.size() < 3)
    return PlaceOutcome::Refused;
  // AbstractFurnaceMenu.handlePlacement: a 1x1 grid whose only cell is the
  // input slot, and the slots ServerPlaceRecipe empties first are the input and
  // the *result*. A cooking recipe's output is an ordinary slot rather than a
  // derived one, so a book click hands the finished goods back to the player —
  // and the fuel slot is never touched, or every book click would eat the coal
  // that is already burning.
  const GeneratedIngredient* ingredient = recipe_cells(recipe);
  Inventory input{1}; // the "grid": the container's slot 0, alone
  input.set(0, slots.get(0));

  // recipeMatches: the input already holds an item this recipe cooks.
  const bool matches = !input.get(0).empty() && ingredient_accepts(*ingredient, input.get(0).item);

  // StackedItemContents over `Inventory.fillStackedContents` plus
  // `menu.fillCraftSlotsStackedContents` (the block entity's own contents), and
  // the most plentiful accepted item per ingredient — the same simplification
  // the grid path makes. The fuel and result slots are *not* counted: the
  // placer can only move items out of the player's inventory, so counting them
  // would plan a craft it cannot carry out.
  int32_t chosen = kAirItemId;
  int max_crafts = 0;
  for (int32_t item : ingredient_items(*ingredient)) {
    const int total = available_of(inv, input, item);
    if (total > max_crafts) {
      max_crafts = total;
      chosen = item;
    }
  }

  // calculateAmountToCraft, then clampToMaxStackSize.
  int amount = use_max_items ? max_crafts : (matches ? input.get(0).count + 1 : 1);
  amount = std::max(amount, 1);
  amount = std::min(amount, ItemRegistry::instance().max_stack_size(chosen));

  // clearGrid: the input and the result go back to the player, and in survival
  // a result that will not fit there cancels the whole click (testClearGrid).
  Inventory trial = inv;
  std::vector<ItemStack> overflow;
  for (const int slot : {0, 2}) {
    const ItemStack held = slots.get(slot);
    if (held.empty())
      continue;
    const int leftover = trial.add(held, selected_slot);
    if (leftover <= 0)
      continue;
    if (!allow_dropping_items)
      return PlaceOutcome::Refused; // nothing has moved: the furnace still stands
    ItemStack rest = held;
    rest.count = leftover;
    overflow.push_back(rest);
  }
  inv = trial;
  input.set_empty(0); // the grid mirror too: what is placed below starts from
  slots.set_empty(0); // an empty cell, exactly like clearGrid
  slots.set_empty(2);
  if (spilled && !overflow.empty())
    spilled->insert(spilled->end(), overflow.begin(), overflow.end());

  // What is left after the clear decides the final count; nothing left is the
  // ghost path (ServerPlaceRecipe.tryPlaceRecipe).
  amount = std::min(amount, available_of(inv, input, chosen));
  if (amount < 1)
    return PlaceOutcome::Ghost;
  int remaining = amount;
  while (remaining > 0) {
    const int taken = move_into_cell(inv, input, 0, chosen, remaining);
    if (taken <= 0)
      break;
    remaining -= taken;
  }
  slots.set(0, input.get(0));
  return PlaceOutcome::Placed;
}

} // namespace mc::world
