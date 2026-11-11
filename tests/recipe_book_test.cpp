// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the recipe book (Phase 3 milestone D2):
//   * the display list over the whole generated table,
//   * the `update_recipes`, `recipe_book_add`, `recipe_book_settings` and
//     `place_ghost_recipe` bodies, decoded back the way a client reads them —
//     including the two *orders* that have to agree across the wire: the recipe
//     display ids a client echoes in `place_recipe`, and the stonecutter list
//     index it filters itself,
//   * `place_recipe`: PlaceRecipeHelper's centring, the max-items count, the
//     ghost path, and the refusal that has to leave a grid standing.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "net/byte_buffer.h"
#include "world/inventory.h"
#include "world/item.h"
#include "world/recipe.h"
#include "world/recipe_book.h"
#include "world/trim_table.h"

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

// ---- lookups ---------------------------------------------------------------

int32_t item(const char* name) {
  const std::optional<int32_t> id = ItemRegistry::instance().id_of(name);
  if (!id) {
    std::fprintf(stderr, "FAIL: no such item %s\n", name);
    ++failures;
    return -1;
  }
  return *id;
}

const RecipeBookDisplay* display_named(const char* name) {
  for (const RecipeBookDisplay& display : recipe_book_displays())
    if (std::strcmp(display.recipe->name, name) == 0)
      return &display;
  std::fprintf(stderr, "FAIL: no display for recipe %s\n", name);
  ++failures;
  return nullptr;
}

int32_t display_id_named(const char* name) {
  const std::vector<RecipeBookDisplay>& all = recipe_book_displays();
  for (size_t i = 0; i < all.size(); ++i)
    if (std::strcmp(all[i].recipe->name, name) == 0)
      return static_cast<int32_t>(i);
  std::fprintf(stderr, "FAIL: no display id for recipe %s\n", name);
  ++failures;
  return -1;
}

bool contains(const std::vector<int32_t>& ids, int32_t id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// ---- a decoder for the pieces the assertions look at -----------------------

struct SlotView {
  int kind = -1;
  std::vector<int32_t> items;    // Item / Composite members
  std::vector<SlotView> members; // Composite members, with their own payload
  int32_t stack_item = 0;
  int stack_count = 0;
  // A SmithingTrim demo (kind 8): the base and material it trims, as the item
  // sets their own displays name, and the pattern holder id the wire carries
  // (the pattern registry id + 1, since a Holder is written as id + 1).
  std::vector<int32_t> demo_base;
  std::vector<int32_t> demo_material;
  int pattern = -1;
};

bool read_slot_display(mc::net::Reader& r, SlotView* out) {
  uint32_t kind = 0;
  if (!r.varint(&kind))
    return false;
  out->kind = static_cast<int>(kind);
  switch (out->kind) {
  case 0: // Empty
  case 1: // AnyFuel
    return true;
  case 4: { // Item
    uint32_t id = 0;
    if (!r.varint(&id))
      return false;
    out->items.push_back(static_cast<int32_t>(id));
    return true;
  }
  case 5: { // ItemStack
    uint32_t id = 0, count = 0, added = 0, removed = 0;
    if (!r.varint(&id) || !r.varint(&count) || !r.varint(&added) || !r.varint(&removed))
      return false;
    out->stack_item = static_cast<int32_t>(id);
    out->stack_count = static_cast<int>(count);
    // ItemStackTemplate's constructor throws "Item must be non-empty" for air
    // or a zero count, so either one would fail the client's decode.
    CHECK(id != 0);
    CHECK(count > 0);
    return added == 0 && removed == 0; // every result we send carries no components
  }
  case 8: { // SmithingTrim (SlotDisplay.SmithingTrimDemoSlotDisplay)
    SlotView base, material;
    if (!read_slot_display(r, &base) || !read_slot_display(r, &material))
      return false;
    uint32_t pattern = 0;
    if (!r.varint(&pattern))
      return false;
    out->demo_base = base.items;
    out->demo_material = material.items;
    out->pattern = static_cast<int>(pattern);
    return true;
  }
  case 10: { // Composite
    uint32_t n = 0;
    if (!r.varint(&n))
      return false;
    for (uint32_t i = 0; i < n; ++i) {
      SlotView inner;
      if (!read_slot_display(r, &inner))
        return false;
      out->items.push_back(inner.items.empty() ? inner.stack_item : inner.items[0]);
      out->members.push_back(inner);
    }
    return true;
  }
  default:
    return false;
  }
}

struct DisplayView {
  int kind = -1;
  int width = 0, height = 0;
  std::vector<SlotView> ingredients;
  SlotView result;
  int32_t station = 0;
  int cook_time = 0;
  float experience = 0.0f;
};

bool read_recipe_display(mc::net::Reader& r, DisplayView* out) {
  uint32_t kind = 0;
  if (!r.varint(&kind))
    return false;
  out->kind = static_cast<int>(kind);
  SlotView slot;
  switch (out->kind) {
  case 1: { // crafting_shaped
    uint32_t w = 0, h = 0, cells = 0;
    if (!r.varint(&w) || !r.varint(&h) || !r.varint(&cells))
      return false;
    out->width = static_cast<int>(w);
    out->height = static_cast<int>(h);
    if (cells != w * h)
      return false; // the record's own invariant
    for (uint32_t i = 0; i < cells; ++i) {
      if (!read_slot_display(r, &slot))
        return false;
      out->ingredients.push_back(slot);
      slot = SlotView{};
    }
    break;
  }
  case 0: { // crafting_shapeless
    uint32_t n = 0;
    if (!r.varint(&n))
      return false;
    for (uint32_t i = 0; i < n; ++i) {
      if (!read_slot_display(r, &slot))
        return false;
      out->ingredients.push_back(slot);
      slot = SlotView{};
    }
    break;
  }
  case 2: // furnace
    if (!read_slot_display(r, &slot))
      return false;
    out->ingredients.push_back(slot);
    if (!read_slot_display(r, &slot)) // the fuel slot
      return false;
    break;
  case 3: // stonecutter
    if (!read_slot_display(r, &slot))
      return false;
    out->ingredients.push_back(slot);
    break;
  case 4: // smithing
    for (int i = 0; i < 3; ++i) {
      if (!read_slot_display(r, &slot))
        return false;
      out->ingredients.push_back(slot);
      slot = SlotView{};
    }
    break;
  default:
    return false; // only the five registered kinds exist
  }
  if (!read_slot_display(r, &out->result))
    return false;
  // The crafting station is a SlotDisplay too (an item one, e.g. the crafting
  // table), not a bare item id.
  SlotView station;
  if (!read_slot_display(r, &station))
    return false;
  out->station = station.items.empty() ? station.stack_item : station.items[0];
  if (out->kind == 2) {
    uint32_t cook_time = 0;
    if (!r.varint(&cook_time) || !r.f32(&out->experience))
      return false;
    out->cook_time = static_cast<int>(cook_time);
  }
  return true;
}

struct EntryView {
  int32_t id = -1;
  DisplayView display;
  int group = -1;
  int category = -1;
  bool has_requirements = false;
  std::vector<std::vector<int32_t>> requirements; // one item set per Ingredient
  int flags = -1;
};

bool read_entry(mc::net::Reader& r, EntryView* out) {
  uint32_t id = 0;
  if (!r.varint(&id))
    return false;
  out->id = static_cast<int32_t>(id);
  if (!read_recipe_display(r, &out->display))
    return false;
  uint32_t group = 0;
  if (!r.varint(&group))
    return false;
  out->group = static_cast<int>(group) - 1; // OptionalInt: 0 is "empty"
  uint32_t category = 0;
  if (!r.varint(&category))
    return false;
  out->category = static_cast<int>(category);
  uint8_t present = 0;
  if (!r.u8(&present))
    return false;
  out->has_requirements = present != 0;
  if (out->has_requirements) {
    uint32_t count = 0;
    if (!r.varint(&count))
      return false;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t n = 0;
      if (!r.varint(&n) || n == 0) // 0 would be a tag, which the table never emits
        return false;
      std::vector<int32_t> ids;
      for (uint32_t j = 0; j + 1 < n; ++j) {
        uint32_t value = 0;
        if (!r.varint(&value))
          return false;
        ids.push_back(static_cast<int32_t>(value));
      }
      out->requirements.push_back(ids);
    }
  }
  uint8_t flags = 0;
  if (!r.u8(&flags))
    return false;
  out->flags = flags;
  return true;
}

// ---- the display list ------------------------------------------------------

void test_display_list() {
  const std::vector<RecipeBookDisplay>& all = recipe_book_displays();
  int shaped = 0, shapeless = 0, transmute = 0, furnace = 0, stonecutter = 0, smithing = 0;
  int smithing_trim = 0;
  for (size_t i = 0; i < all.size(); ++i) {
    const RecipeBookDisplay& display = all[i];
    CHECK(display.recipe != nullptr);
    if (display.recipe == nullptr)
      continue;
    // The id *is* the index, which is the whole contract: `place_recipe` names
    // an id and the server looks it up here.
    CHECK(recipe_book_display(static_cast<int32_t>(i)) == &display);
    const GeneratedRecipe& recipe = *display.recipe;
    CHECK(static_cast<int>(recipe.book_category) <= static_cast<int>(BookCategory::Campfire));
    if (recipe.kind == RecipeKind::SmithingTrim ||
        (recipe.kind == RecipeKind::Transmute && recipe.result_item == kAirItemId))
      // Trim and 26.3's resultless transmute recipes derive the result from the
      // player's input; they have no static result stack to check.
      CHECK(recipe.result_item == kAirItemId);
    else
      CHECK(recipe.result_item != kAirItemId && recipe.result_count >= 1);
    switch (recipe.kind) {
    case RecipeKind::Shaped:
      ++shaped;
      CHECK(display.kind == DisplayKind::CraftingShaped);
      CHECK(recipe.width >= 1 && recipe.width <= 3 && recipe.height >= 1 && recipe.height <= 3);
      CHECK(recipe.ing_count == recipe.width * recipe.height);
      break;
    case RecipeKind::Shapeless:
      ++shapeless;
      CHECK(display.kind == DisplayKind::CraftingShapeless);
      CHECK(display.material_count == 0);
      break;
    case RecipeKind::Transmute:
      ++transmute;
      CHECK(display.kind == DisplayKind::CraftingShapeless);
      CHECK(display.material_count >= recipe.material_min);
      CHECK(display.material_count <= (recipe.material_max & 0x7F));
      break;
    case RecipeKind::Smelting:
    case RecipeKind::Blasting:
    case RecipeKind::Smoking:
    case RecipeKind::Campfire:
      ++furnace;
      CHECK(display.kind == DisplayKind::Furnace);
      break;
    case RecipeKind::Stonecutting:
      ++stonecutter;
      CHECK(display.kind == DisplayKind::Stonecutter);
      break;
    case RecipeKind::SmithingTransform:
      ++smithing;
      CHECK(display.kind == DisplayKind::Smithing);
      break;
    case RecipeKind::SmithingTrim:
      ++smithing_trim;
      CHECK(display.kind == DisplayKind::Smithing);
      CHECK(display.material_count == 0);
      break;
    case RecipeKind::Special:
      CHECK(false); // never given a display (recipe_book.h)
      break;
    }
  }
  // Pinned to the table, so a generator change cannot quietly empty the book:
  // 827 shaped, 375 shapeless, 33 transmute recipes (each expanding to one
  // display per material count), 73 + 25 + 9 + 9 cooking, 351 stonecutting,
  // 12 smithing transforms and 18 smithing trims. The 31 code-driven specials
  // have no display in vanilla either.
  CHECK(shaped == 827);
  CHECK(shapeless == 375);
  CHECK(transmute == 264);
  CHECK(furnace == 116);
  CHECK(stonecutter == 351);
  CHECK(smithing == 12);
  CHECK(smithing_trim == 18);
  CHECK(all.size() == 1963);
  CHECK(kRecipeCount == 1763);

  // A transmute recipe gets exactly one display per legal material count.
  for (uint32_t i = 0; i < kRecipeCount; ++i) {
    const GeneratedRecipe& recipe = kRecipes[i];
    if (recipe.kind != RecipeKind::Transmute)
      continue;
    const int lo = recipe.material_min, hi = recipe.material_max & 0x7F;
    CHECK(lo >= 1 && hi <= 8);
    int seen = 0;
    for (const RecipeBookDisplay& display : all) {
      if (display.recipe != &recipe)
        continue;
      ++seen;
      CHECK(static_cast<int>(display.material_count) >= lo);
      CHECK(static_cast<int>(display.material_count) <= hi);
    }
    CHECK(seen == hi - lo + 1);
  }

  CHECK(recipe_book_display(-1) == nullptr);
  CHECK(recipe_book_display(static_cast<int32_t>(all.size())) == nullptr);
  std::printf("  displays: %zu (shaped %d, shapeless %d, transmute %d, furnace %d, stonecutter %d, "
              "smithing %d + %d trims)\n",
              all.size(), shaped, shapeless, transmute, furnace, stonecutter, smithing,
              smithing_trim);
}

// ---- update_recipes --------------------------------------------------------

void test_update_recipes() {
  mc::net::Writer w;
  write_update_recipes(w);
  const std::vector<uint8_t> bytes = std::move(w).take();
  mc::net::Reader r(bytes);

  uint32_t sets = 0;
  CHECK(r.varint(&sets));
  CHECK(sets == 7);
  static const char* kExpectedKeys[] = {
      "minecraft:smithing_base",  "minecraft:smithing_template",   "minecraft:smithing_addition",
      "minecraft:furnace_input",  "minecraft:blast_furnace_input", "minecraft:smoker_input",
      "minecraft:campfire_input",
  };
  std::vector<int32_t> furnace_input;
  for (uint32_t i = 0; i < sets && i < 7; ++i) {
    std::string key;
    CHECK(r.string(&key));
    CHECK(key == kExpectedKeys[i]);
    uint32_t count = 0;
    CHECK(r.varint(&count));
    CHECK(count > 0);
    std::vector<int32_t> ids;
    for (uint32_t j = 0; j < count; ++j) {
      uint32_t id = 0;
      CHECK(r.varint(&id));
      CHECK(id < static_cast<uint32_t>(kItemCount));
      ids.push_back(static_cast<int32_t>(id));
    }
    if (key == "minecraft:furnace_input")
      furnace_input = ids;
  }
  // The sets gate the menus' input slots, so their contents are observable:
  // raw iron smelts, a diamond does not (a log does — it is charcoal).
  CHECK(contains(furnace_input, item("minecraft:raw_iron")));
  CHECK(!contains(furnace_input, item("minecraft:diamond")));

  // The stonecutter list: every stonecutting recipe, in table order, each an
  // Ingredient plus the result display the menu shows for it.
  const std::vector<const GeneratedRecipe*> cutters = stonecutting_recipes();
  uint32_t decoded = 0;
  CHECK(r.varint(&decoded));
  CHECK(decoded == cutters.size());
  CHECK(decoded > 0);
  std::vector<int32_t> stone_results;
  for (uint32_t i = 0; i < decoded; ++i) {
    uint32_t n = 0;
    CHECK(r.varint(&n));
    std::vector<int32_t> input;
    for (uint32_t j = 0; j + 1 < n; ++j) {
      uint32_t id = 0;
      CHECK(r.varint(&id));
      input.push_back(static_cast<int32_t>(id));
    }
    SlotView result;
    CHECK(read_slot_display(r, &result));
    CHECK(result.kind == static_cast<int>(SlotKind::ItemStack));
    if (i < cutters.size()) {
      const GeneratedRecipe& recipe = *cutters[i];
      CHECK(input == ingredient_items(recipe_cells(recipe)[0]));
      CHECK(result.stack_item == recipe.result_item);
      CHECK(result.stack_count == recipe.result_count);
      if (contains(input, item("minecraft:stone")))
        stone_results.push_back(recipe.result_item);
    }
  }
  CHECK(r.at_end());

  // Filtering the received list by an input has to reproduce
  // stonecutting_recipes_for in the same order: that is the contract the
  // stonecutter menu's selected index depends on.
  std::vector<int32_t> server_side;
  for (const GeneratedRecipe* recipe : stonecutting_recipes_for(item("minecraft:stone")))
    server_side.push_back(recipe->result_item);
  CHECK(!server_side.empty());
  CHECK(stone_results == server_side);
  std::printf("  update_recipes: 7 property sets, %u stonecutter recipes (%zu for stone)\n",
              decoded, server_side.size());
}

// ---- recipe_book_add -------------------------------------------------------

void test_recipe_book_add() {
  // The whole list, the way the join sends it. Every entry is checked against
  // the display it came from, including "the requirements are the placement's
  // ingredients" — a shaped pattern's gaps are not ingredients.
  mc::net::Writer w;
  write_recipe_book_add(w, {}, 0, true);
  const std::vector<uint8_t> bytes = std::move(w).take();
  mc::net::Reader r(bytes);
  const std::vector<RecipeBookDisplay>& all = recipe_book_displays();
  uint32_t count = 0;
  CHECK(r.varint(&count));
  CHECK(count == all.size());
  int per_category[13] = {0};
  for (uint32_t i = 0; i < count && i < all.size(); ++i) {
    EntryView entry;
    CHECK(read_entry(r, &entry));
    CHECK(entry.id == static_cast<int32_t>(i));
    CHECK(entry.group == -1); // no JSON `group` in the table: one entry each
    CHECK(entry.category == static_cast<int>(all[i].recipe->book_category));
    CHECK(entry.category >= 0 && entry.category <= 12);
    CHECK(entry.flags == 0);
    if (entry.category >= 0 && entry.category <= 12)
      ++per_category[entry.category];
    CHECK(entry.has_requirements);
    CHECK(!entry.requirements.empty());
    if (entry.display.kind == static_cast<int>(DisplayKind::CraftingShaped)) {
      CHECK(static_cast<int>(entry.display.ingredients.size()) ==
            entry.display.width * entry.display.height);
      int present = 0;
      for (const SlotView& slot : entry.display.ingredients)
        if (slot.kind != static_cast<int>(SlotKind::Empty))
          ++present;
      CHECK(entry.requirements.size() == static_cast<size_t>(present));
    } else if (entry.display.kind == static_cast<int>(DisplayKind::CraftingShapeless)) {
      if (all[i].recipe->kind == RecipeKind::Transmute) {
        // TransmuteRecipe.createPlacementInfo uses the *maximum* material count
        // for every one of its displays.
        CHECK(entry.requirements.size() ==
              static_cast<size_t>(1 + (all[i].recipe->material_max & 0x7F)));
        CHECK(entry.display.ingredients.size() == static_cast<size_t>(1 + all[i].material_count));
      } else {
        CHECK(entry.requirements.size() == entry.display.ingredients.size());
      }
    } else {
      CHECK(entry.requirements.size() == 1 || entry.requirements.size() == 3);
    }
  }
  uint8_t replace = 0;
  CHECK(r.u8(&replace));
  CHECK(replace == 1);
  CHECK(r.at_end());
  // Every smithing_trim entry, decoded: the result slot is a demo (the base and
  // the material it trims, plus the pattern holder), and the pattern each entry
  // writes is the one whose template item its recipe's template cell names. The
  // 18 patterns have to come out exactly once each — that is what makes the
  // book's trim page the trim table rather than 18 copies of one pattern.
  {
    int trims = 0;
    bool seen_pattern[18] = {false};
    for (uint32_t i = 0; i < all.size(); ++i) {
      const GeneratedRecipe& recipe = *all[i].recipe;
      if (recipe.kind != RecipeKind::SmithingTrim)
        continue;
      ++trims;
      mc::net::Writer one;
      write_recipe_book_add(one, {static_cast<int32_t>(i)}, 0, false);
      const std::vector<uint8_t> entry_bytes = std::move(one).take();
      mc::net::Reader er(entry_bytes);
      uint32_t n = 0;
      CHECK(er.varint(&n));
      CHECK(n == 1);
      EntryView entry;
      CHECK(read_entry(er, &entry));
      CHECK(entry.category == static_cast<int>(BookCategory::Smithing));
      const DisplayView& d = entry.display;
      CHECK(d.kind == static_cast<int>(DisplayKind::Smithing));
      CHECK(d.station == item("minecraft:smithing_table"));
      const GeneratedIngredient* cells = recipe_cells(recipe);
      CHECK(d.ingredients.size() == 3);
      if (d.ingredients.size() == 3) {
        CHECK(d.ingredients[0].items == ingredient_items(cells[0])); // template
        CHECK(d.ingredients[1].items == ingredient_items(cells[1])); // base
        CHECK(d.ingredients[2].items == ingredient_items(cells[2])); // addition
      }
      // The demo: this recipe's base and addition ingredients again, and a
      // pattern holder that resolves to the pattern whose template item the
      // recipe's template cell holds.
      CHECK(d.result.kind == static_cast<int>(SlotKind::SmithingTrim));
      CHECK(d.result.demo_base == ingredient_items(cells[1]));
      CHECK(d.result.demo_material == ingredient_items(cells[2]));
      CHECK(contains(d.result.demo_base, item("minecraft:iron_chestplate")));
      CHECK(contains(d.result.demo_material, item("minecraft:gold_ingot")));
      const std::vector<int32_t> template_items = ingredient_items(cells[0]);
      CHECK(template_items.size() == 1); // a pattern has exactly one template item
      const int32_t template_item = template_items.empty() ? -1 : template_items.front();
      int32_t expected = -1;
      for (uint32_t p = 0; p < kTrimPatternCount; ++p)
        if (kTrimPatterns[p].template_item == template_item)
          expected = kTrimPatterns[p].id;
      CHECK(expected >= 0);
      CHECK(d.result.pattern == expected + 1); // a Holder is written id + 1
      if (expected >= 0 && expected < 18)
        seen_pattern[static_cast<size_t>(expected)] = true;
      // Three requirement ingredients, the placement's own order.
      CHECK(entry.requirements.size() == 3);
      if (entry.requirements.size() == 3) {
        CHECK(entry.requirements[0] == ingredient_items(cells[0]));
        CHECK(entry.requirements[1] == ingredient_items(cells[1]));
        CHECK(entry.requirements[2] == ingredient_items(cells[2]));
      }
      uint8_t trim_replace = 1;
      CHECK(er.u8(&trim_replace));
      CHECK(trim_replace == 0);
      CHECK(er.at_end());
    }
    CHECK(trims == 18);
    for (int p = 0; p < 18; ++p)
      CHECK(seen_pattern[p]);
  }

  // A transform and a trim share the display kind but not the result slot: the
  // transform names an item, the trim can only name a demo.
  {
    int transforms = 0, demos = 0;
    for (uint32_t i = 0; i < all.size(); ++i) {
      if (all[i].recipe->kind != RecipeKind::SmithingTransform &&
          all[i].recipe->kind != RecipeKind::SmithingTrim)
        continue;
      mc::net::Writer one;
      write_recipe_book_add(one, {static_cast<int32_t>(i)}, 0, false);
      const std::vector<uint8_t> entry_bytes = std::move(one).take();
      mc::net::Reader er(entry_bytes);
      uint32_t n = 0;
      CHECK(er.varint(&n));
      CHECK(n == 1);
      EntryView entry;
      CHECK(read_entry(er, &entry));
      if (all[i].recipe->kind == RecipeKind::SmithingTransform) {
        ++transforms;
        CHECK(entry.display.result.kind == static_cast<int>(SlotKind::ItemStack));
        CHECK(entry.display.result.stack_item == all[i].recipe->result_item);
        CHECK(entry.display.result.stack_count == all[i].recipe->result_count);
      } else {
        ++demos;
        CHECK(entry.display.result.kind == static_cast<int>(SlotKind::SmithingTrim));
      }
    }
    CHECK(transforms == 12);
    CHECK(demos == 18);
  }

  // A transmute whose result has no fixed item — map cloning's `result` is
  // `{}` — cannot be an ItemStack display: the client's ItemStackTemplate
  // constructor throws "Item must be non-empty" on air. Vanilla draws it as a
  // Composite of the input's items, one stack per item, as tall as the
  // material count; that is what has to go out.
  {
    const RecipeBookDisplay* cloning = display_named("map_cloning");
    CHECK(cloning != nullptr);
    if (cloning) {
      const std::vector<int32_t> input_items = ingredient_items(recipe_cells(*cloning->recipe)[0]);
      int shown = 0;
      for (uint32_t i = 0; i < all.size(); ++i) {
        if (all[i].recipe != cloning->recipe)
          continue;
        ++shown;
        mc::net::Writer one;
        write_recipe_book_add(one, {static_cast<int32_t>(i)}, 0, false);
        const std::vector<uint8_t> entry_bytes = std::move(one).take();
        mc::net::Reader er(entry_bytes);
        uint32_t n = 0;
        CHECK(er.varint(&n));
        CHECK(n == 1);
        EntryView entry;
        CHECK(read_entry(er, &entry));
        CHECK(entry.display.kind == static_cast<int>(DisplayKind::CraftingShapeless));
        CHECK(entry.display.result.kind == static_cast<int>(SlotKind::Composite));
        CHECK(entry.display.result.members.size() == input_items.size());
        for (const SlotView& member : entry.display.result.members) {
          CHECK(member.kind == static_cast<int>(SlotKind::ItemStack));
          CHECK(contains(input_items, member.stack_item));
          CHECK(member.stack_count == static_cast<int>(all[i].material_count));
        }
      }
      CHECK(shown == 8); // one display per material count, 1..8
    }
  }

  // The categories have to land in the tabs the crafting book lists, or the
  // client drops the entries without a word: 827 + 375 + 264 crafting, 116
  // furnace, 25 blasting, 9 smoker, 351 stonecutter, 30 smithing, 9 campfire.
  CHECK(per_category[0] + per_category[1] + per_category[2] + per_category[3] == 1466);
  // Furnace tabs are smelting only: each cooking family has its own tab.
  CHECK(per_category[4] + per_category[5] + per_category[6] == 73);
  CHECK(per_category[7] + per_category[8] == 25);
  CHECK(per_category[9] == 9);
  CHECK(per_category[10] == 351);
  CHECK(per_category[11] == 30); // 12 transforms + 18 trims
  CHECK(per_category[12] == 9);

  // One recipe, the way a later (newly unlocked) entry would arrive.
  {
    const int32_t id = display_id_named("oak_planks");
    mc::net::Writer one;
    write_recipe_book_add(one, {id}, 0, false);
    const std::vector<uint8_t> entry_bytes = std::move(one).take();
    mc::net::Reader er(entry_bytes);
    uint32_t n = 0;
    CHECK(er.varint(&n));
    CHECK(n == 1);
    EntryView entry;
    CHECK(read_entry(er, &entry));
    CHECK(entry.id == id);
    CHECK(entry.display.kind == static_cast<int>(DisplayKind::CraftingShapeless));
    CHECK(entry.display.ingredients.size() == 1);
    CHECK(contains(entry.display.ingredients[0].items, item("minecraft:oak_log")));
    CHECK(entry.display.result.stack_item == item("minecraft:oak_planks"));
    CHECK(entry.display.result.stack_count == 4);
    CHECK(entry.display.station == item("minecraft:crafting_table"));
    CHECK(entry.category == static_cast<int>(BookCategory::CraftingBuildingBlocks));
    CHECK(entry.requirements.size() == 1);
    CHECK(contains(entry.requirements[0], item("minecraft:oak_log")));
    uint8_t one_replace = 1;
    CHECK(er.u8(&one_replace));
    CHECK(one_replace == 0);
    CHECK(er.at_end());
  }

  // An id outside the list must be dropped *without* being counted: the count
  // is written before the entries, so announcing two and writing one would make
  // the client read the next field out of the first entry's tail.
  {
    const int32_t id = display_id_named("oak_planks");
    const int32_t unknown = static_cast<int32_t>(recipe_book_displays().size()) + 5;
    mc::net::Writer mixed;
    write_recipe_book_add(mixed, {id, unknown}, 0, false);
    const std::vector<uint8_t> bytes = std::move(mixed).take();
    mc::net::Reader mr(bytes);
    uint32_t n = 0;
    CHECK(mr.varint(&n));
    CHECK(n == 1);
    EntryView entry;
    CHECK(read_entry(mr, &entry));
    CHECK(entry.id == id);
    uint8_t mixed_replace = 1;
    CHECK(mr.u8(&mixed_replace));
    CHECK(mixed_replace == 0);
    CHECK(mr.at_end());
  }

  // A shaped recipe: the box, the empty-cell rule and the result count.
  {
    const int32_t id = display_id_named("stick");
    mc::net::Writer one;
    write_recipe_book_add(one, {id}, 0x02, false);
    const std::vector<uint8_t> entry_bytes = std::move(one).take();
    mc::net::Reader er(entry_bytes);
    uint32_t n = 0;
    CHECK(er.varint(&n));
    EntryView entry;
    CHECK(read_entry(er, &entry));
    CHECK(entry.id == id);
    CHECK(entry.display.kind == static_cast<int>(DisplayKind::CraftingShaped));
    CHECK(entry.display.width == 1 && entry.display.height == 2);
    CHECK(entry.display.ingredients.size() == 2);
    CHECK(entry.display.ingredients[0].kind == static_cast<int>(SlotKind::Composite));
    CHECK(contains(entry.display.ingredients[0].items, item("minecraft:oak_planks")));
    CHECK(entry.display.ingredients[0].items == entry.display.ingredients[1].items);
    CHECK(entry.display.result.stack_item == item("minecraft:stick"));
    CHECK(entry.display.result.stack_count == 4);
    CHECK(entry.display.station == item("minecraft:crafting_table"));
    CHECK(entry.category == static_cast<int>(BookCategory::CraftingMisc)); // no `category` in JSON
    CHECK(entry.requirements.size() == 2);
    CHECK(entry.flags == 0x02);
  }
  std::printf("  recipe_book_add: %u entries over 13 categories\n", count);
}

void test_settings_and_ghost() {
  {
    mc::net::Writer w;
    write_recipe_book_settings(w, true, false);
    const std::vector<uint8_t> bytes = std::move(w).take();
    // Four TypeSettings pairs: crafting, furnace, blast furnace, smoker.
    CHECK(bytes.size() == 8);
    CHECK(bytes[0] == 1 && bytes[1] == 0);
    for (size_t i = 2; i < bytes.size(); ++i)
      CHECK(bytes[i] == 0);
  }
  {
    const RecipeBookDisplay* display = display_named("oak_planks");
    if (display == nullptr)
      return;
    mc::net::Writer w;
    write_place_ghost_recipe(w, 7, *display);
    const std::vector<uint8_t> bytes = std::move(w).take();
    mc::net::Reader r(bytes);
    uint32_t container = 0;
    CHECK(r.varint(&container));
    CHECK(container == 7);
    DisplayView view;
    CHECK(read_recipe_display(r, &view));
    CHECK(view.kind == static_cast<int>(DisplayKind::CraftingShapeless));
    CHECK(view.result.stack_item == item("minecraft:oak_planks"));
    CHECK(r.at_end());
  }
}

// ---- place_recipe ----------------------------------------------------------

PlaceOutcome place(const RecipeBookDisplay& display, int side, Inventory& grid, Inventory& inv,
                   bool use_max, bool allow_drop, std::vector<ItemStack>* spilled) {
  return place_recipe_from_book(display, side, side, grid, inv, 0, use_max, allow_drop, spilled);
}

CraftingGrid view_of(const Inventory& grid, int side) {
  CraftingGrid view = CraftingGrid::of(side, side);
  for (int i = 0; i < side * side && i < grid.size(); ++i)
    view.items[static_cast<size_t>(i)] = grid.get(i);
  return view;
}

void test_place_recipe() {
  const RecipeBookDisplay* planks = display_named("oak_planks");
  const RecipeBookDisplay* sticks = display_named("stick");
  if (planks == nullptr || sticks == nullptr)
    return;
  const int32_t log = item("minecraft:oak_log");
  const int32_t plank = item("minecraft:oak_planks");
  const int32_t cobble = item("minecraft:cobblestone");

  // 1. A shapeless recipe in the player's 2x2: one craft, filled from cell 0.
  {
    Inventory inv(41);
    CHECK(inv.add(ItemStack::of_id(log, 3), 0) == 0);
    Inventory grid(4);
    std::vector<ItemStack> spilled;
    CHECK(place(*planks, 2, grid, inv, false, false, &spilled) == PlaceOutcome::Placed);
    CHECK(spilled.empty());
    CHECK(grid.get(0).item == log && grid.get(0).count == 1);
    CHECK(grid.get(1).empty() && grid.get(2).empty() && grid.get(3).empty());
    CHECK(inv.get(0).item == log && inv.get(0).count == 2);
    const std::optional<CraftingMatch> match = find_crafting_recipe(view_of(grid, 2));
    CHECK(match && match->recipe == planks->recipe && match->result_count == 4);
  }

  // 2. A shaped recipe on the 3x3: PlaceRecipeHelper centres a box narrower
  // than half the grid (x = 3/2 - 1/2 = 1) and `use max items` prepares as many
  // crafts as the ingredients cover.
  {
    Inventory inv(41);
    CHECK(inv.add(ItemStack::of_id(plank, 4), 0) == 0);
    Inventory grid(9);
    std::vector<ItemStack> spilled;
    CHECK(place(*sticks, 3, grid, inv, true, false, &spilled) == PlaceOutcome::Placed);
    CHECK(grid.get(1).item == plank && grid.get(1).count == 2);
    CHECK(grid.get(4).item == plank && grid.get(4).count == 2);
    CHECK(grid.get(0).empty() && grid.get(2).empty());
    CHECK(inv.get(0).empty());
    const std::optional<CraftingMatch> match = find_crafting_recipe(view_of(grid, 3));
    CHECK(match && match->recipe == sticks->recipe && match->result_count == 4);
  }

  // 3. A 3x3 recipe does not fit the player's 2x2: refused before anything
  // moves. (A client is free to send any display id, so the menu's grid size
  // has to be checked, not assumed from what the book shows.)
  {
    Inventory inv(41);
    CHECK(inv.add(ItemStack::of_id(cobble, 8), 0) == 0);
    Inventory grid(4);
    std::vector<ItemStack> spilled;
    const RecipeBookDisplay* furnace = display_named("furnace");
    CHECK(furnace != nullptr && furnace->recipe->width == 3);
    if (furnace != nullptr) {
      CHECK(place(*furnace, 2, grid, inv, false, false, &spilled) == PlaceOutcome::Refused);
      CHECK(grid.empty());
      CHECK(inv.get(0).count == 8);
    }
  }

  // 4. Nothing to craft: the grid is emptied and the caller is told to ghost
  // the recipe (ServerPlaceRecipe.tryPlaceRecipe).
  {
    Inventory inv(41);
    Inventory grid(4);
    std::vector<ItemStack> spilled;
    CHECK(place(*planks, 2, grid, inv, false, false, &spilled) == PlaceOutcome::Ghost);
    CHECK(grid.empty());
    CHECK(spilled.empty());
  }

  // 5. A grid that cannot be emptied in survival: nothing moves at all
  // (testClearGrid), so a client's optimistic view is not half-corrected. The
  // grid holds something the recipe does not want, so this is the clear path
  // and not the "already matching, one more craft" guard.
  {
    Inventory inv(41);
    for (int i = 0; i < inv.size(); ++i)
      inv.set(i, ItemStack::of_id(cobble, 64));
    Inventory grid(4);
    grid.set(0, ItemStack::of_id(cobble, 1));
    std::vector<ItemStack> spilled;
    CHECK(place(*planks, 2, grid, inv, false, false, &spilled) == PlaceOutcome::Refused);
    CHECK(grid.get(0).item == cobble && grid.get(0).count == 1);
    CHECK(spilled.empty());
    CHECK(inv.get(0).item == cobble && inv.get(0).count == 64);
  }

  // 6. In creative a grid that will not fit back spills instead, and the spill
  // is reported so the caller can drop it at the player's feet. The craft count
  // is then decided by what is actually left in the inventory.
  {
    Inventory inv(41);
    for (int i = 0; i < inv.size(); ++i)
      inv.set(i, ItemStack::of_id(cobble, 64));
    inv.set(0, ItemStack::of_id(log, 64)); // full, so the grid's logs cannot merge in
    Inventory grid(4);
    grid.set(0, ItemStack::of_id(log, 2));
    std::vector<ItemStack> spilled;
    CHECK(place(*planks, 2, grid, inv, false, true, &spilled) == PlaceOutcome::Placed);
    CHECK(spilled.size() == 1 && spilled[0].item == log && spilled[0].count == 2);
    // One more than the grid held (the `matches` branch), taken from the stack.
    CHECK(grid.get(0).item == log && grid.get(0).count == 3);
    CHECK(inv.get(0).item == log && inv.get(0).count == 61);
  }

  // 7. A grid already holding the recipe adds one craft per click — the
  // `smallestStackSize + 1` branch — taking the extra from the inventory.
  {
    Inventory inv(41);
    CHECK(inv.add(ItemStack::of_id(log, 4), 0) == 0);
    Inventory grid(4);
    grid.set(0, ItemStack::of_id(log, 2));
    std::vector<ItemStack> spilled;
    CHECK(place(*planks, 2, grid, inv, false, false, &spilled) == PlaceOutcome::Placed);
    CHECK(grid.get(0).item == log && grid.get(0).count == 3);
    CHECK(inv.get(0).item == log && inv.get(0).count == 3);
  }

  // 8. The same click is refused once the craft would overflow a cell, without
  // disturbing what is on the grid (ServerPlaceRecipe.placeRecipe's guard).
  {
    Inventory inv(41);
    Inventory grid(4);
    grid.set(0, ItemStack::of_id(log, 4));
    std::vector<ItemStack> spilled;
    CHECK(place(*planks, 2, grid, inv, false, false, &spilled) == PlaceOutcome::Refused);
    CHECK(grid.get(0).count == 4);
  }
  std::printf("  place_recipe: placement, centring, ghost, spill and refusal\n");
}

} // namespace

int main() {
  test_display_list();
  test_update_recipes();
  test_recipe_book_add();
  test_settings_and_ghost();
  test_place_recipe();
  if (failures == 0) {
    std::printf("recipe-book: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "recipe-book: %d check(s) failed\n", failures);
  return 1;
}
