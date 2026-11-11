// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the crafting recipe engine (Phase 3 milestone D):
//   * the generated table's shape (every family present, every id valid),
//   * the matching rules — 2x2/3x3 placement, mirroring, shapeless assignment,
//     transmute bounds and the no-op rule,
//   * a data-derived sweep: every recipe the engine claims to support must
//     actually match a grid built from its own ingredients and produce its own
//     result. That is the test that catches a generator that silently mangles
//     a recipe, which is otherwise almost invisible.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "world/item.h"
#include "world/recipe.h"

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
  if (!id) {
    std::fprintf(stderr, "FAIL: no such item %s\n", name);
    ++failures;
    return -1;
  }
  return *id;
}

ItemStack stack(const char* name, int count = 1) {
  return ItemStack::of_id(item(name), count);
}

// Builds a 3x3 grid from a list of (x, y, item) placements.
CraftingGrid grid3(const std::vector<std::pair<std::pair<int, int>, ItemStack>>& cells) {
  CraftingGrid g = CraftingGrid::of(3, 3);
  for (const auto& entry : cells)
    g.items[static_cast<size_t>(entry.first.first + entry.first.second * 3)] = entry.second;
  return g;
}

const GeneratedRecipe* recipe(const char* name) {
  const GeneratedRecipe* r = recipe_by_name(name);
  if (r == nullptr) {
    std::fprintf(stderr, "FAIL: no such recipe %s\n", name);
    ++failures;
  }
  return r;
}

// The first item an ingredient accepts (the sweep's stand-in for a tag).
int32_t first_item_of(const GeneratedIngredient& ing) {
  auto ids = ingredient_items(ing);
  return ids.empty() ? -1 : ids.front();
}

void test_table_shape() {
  // Family counts, pinned. A regeneration that drops a family (or a change that
  // makes the parser skip a type) has to be a deliberate, visible edit.
  std::map<std::string, int> counts;
  for (uint32_t i = 0; i < kRecipeCount; ++i) {
    switch (kRecipes[i].kind) {
    case RecipeKind::Shaped:
      counts["shaped"]++;
      break;
    case RecipeKind::Shapeless:
      counts["shapeless"]++;
      break;
    case RecipeKind::Transmute:
      counts["transmute"]++;
      break;
    case RecipeKind::Smelting:
      counts["smelting"]++;
      break;
    case RecipeKind::Blasting:
      counts["blasting"]++;
      break;
    case RecipeKind::Smoking:
      counts["smoking"]++;
      break;
    case RecipeKind::Campfire:
      counts["campfire"]++;
      break;
    case RecipeKind::Stonecutting:
      counts["stonecutting"]++;
      break;
    case RecipeKind::SmithingTransform:
      counts["smithing_transform"]++;
      break;
    case RecipeKind::SmithingTrim:
      counts["smithing_trim"]++;
      break;
    case RecipeKind::Special:
      counts["special"]++;
      break;
    }
  }
  CHECK(kRecipeCount == 1763);
  CHECK(counts["shaped"] == 827);
  CHECK(counts["shapeless"] == 375);
  CHECK(counts["transmute"] == 33);
  CHECK(counts["smelting"] == 73);
  CHECK(counts["blasting"] == 25);
  CHECK(counts["smoking"] == 9);
  CHECK(counts["campfire"] == 9);
  CHECK(counts["stonecutting"] == 351);
  CHECK(counts["smithing_transform"] == 12);
  CHECK(counts["smithing_trim"] == 18);
  CHECK(counts["special"] == 31);

  const int32_t item_count = static_cast<int32_t>(ItemRegistry::instance().by_id(0) ? 1 : 0);
  (void)item_count;

  for (uint32_t i = 0; i < kRecipeCount; ++i) {
    const GeneratedRecipe& r = kRecipes[i];
    // Every recipe's cells are inside the pools.
    CHECK(static_cast<uint32_t>(r.first_ing) + r.ing_count <= kRecipeIngredientCount);

    if (r.kind == RecipeKind::Shaped) {
      // The stored box *is* the trimmed pattern, so it never exceeds the
      // crafting table and its cell count matches its area.
      CHECK(r.width >= 1 && r.width <= 3);
      CHECK(r.height >= 1 && r.height <= 3);
      CHECK(static_cast<int>(r.ing_count) == r.width * r.height);
      CHECK(r.result_item != 0);
      CHECK(r.result_count >= 1);
    }
    if (r.kind == RecipeKind::Shapeless) {
      CHECK(r.ing_count >= 1);
      CHECK(r.result_item != 0);
    }
    if (r.kind == RecipeKind::Transmute) {
      CHECK(r.ing_count == 2);
      CHECK(r.material_min >= 1);
      const int hi = r.material_max & 0x7F;
      CHECK(hi >= r.material_min);
    }
    if (r.kind == RecipeKind::SmithingTrim)
      CHECK(r.result_item == 0); // derived from the base, not in the JSON

    const GeneratedIngredient* cells = recipe_cells(r);
    for (int c = 0; c < r.ing_count; ++c) {
      for (int32_t id : ingredient_items(cells[c])) {
        CHECK(id > 0); // air can never be an ingredient
        CHECK(ItemRegistry::instance().by_id(id) != nullptr);
      }
    }
    if (r.result_item != 0)
      CHECK(ItemRegistry::instance().by_id(r.result_item) != nullptr);
  }
  std::printf("  table shape ok (%u recipes, %u cells, %u item ids)\n", kRecipeCount,
              kRecipeIngredientCount, kRecipeItemCount);
}

void test_tag_ingredients() {
  // #oak_logs expands to the four oak log/wood items, so all of them craft
  // planks and a non-tag item does not.
  const GeneratedRecipe* planks = recipe("oak_planks");
  CHECK(planks != nullptr && planks->kind == RecipeKind::Shapeless);
  for (const char* log : {"minecraft:oak_log", "minecraft:oak_wood", "minecraft:stripped_oak_log",
                          "minecraft:stripped_oak_wood"}) {
    CraftingGrid g = CraftingGrid::of(1, 1);
    g.items[0] = stack(log, 1);
    auto match = find_crafting_recipe(g);
    CHECK(match.has_value());
    if (match) {
      CHECK(match->recipe->result_item == item("minecraft:oak_planks"));
      CHECK(match->result_count == 4);
    }
  }
  CraftingGrid dirt = CraftingGrid::of(1, 1);
  dirt.items[0] = stack("minecraft:dirt");
  CHECK(!find_crafting_recipe(dirt).has_value());

  // The tag really is a set, not a single item.
  const GeneratedIngredient* cell = recipe_cells(*planks);
  CHECK(ingredient_items(cell[0]).size() == 4);
  CHECK(ingredient_accepts(cell[0], item("minecraft:oak_log")));
  CHECK(!ingredient_accepts(cell[0], item("minecraft:birch_log")));
  std::printf("  tags ok\n");
}

void test_shaped_placement_and_mirror() {
  // A 2x2 pattern matches in any corner of a 3x3: vanilla trims the grid to the
  // bounding box of the non-empty cells first.
  for (int ox = 0; ox <= 1; ++ox) {
    for (int oy = 0; oy <= 1; ++oy) {
      CraftingGrid g = CraftingGrid::of(3, 3);
      for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
          g.items[static_cast<size_t>((x + ox) + (y + oy) * 3)] = stack("minecraft:oak_planks");
      auto match = find_crafting_recipe(g);
      CHECK(match.has_value());
      if (match)
        CHECK(match->recipe->result_item == item("minecraft:crafting_table"));
    }
  }

  // carrot_on_a_stick is 2x2 with the rod top-left and the carrot bottom-right;
  // mirrored (rod top-right, carrot bottom-left) matches too, because vanilla
  // tries the mirrored pattern as well.
  const int32_t rod = item("minecraft:fishing_rod");
  const int32_t carrot = item("minecraft:carrot");
  CraftingGrid as_written =
      grid3({{{0, 0}, ItemStack::of_id(rod)}, {{1, 1}, ItemStack::of_id(carrot)}});
  CraftingGrid mirrored =
      grid3({{{1, 0}, ItemStack::of_id(rod)}, {{0, 1}, ItemStack::of_id(carrot)}});
  for (CraftingGrid* g : {&as_written, &mirrored}) {
    auto match = find_crafting_recipe(*g);
    CHECK(match.has_value());
    if (match)
      CHECK(match->recipe->result_item == item("minecraft:carrot_on_a_stick"));
  }

  // A gap in the middle is part of the pattern: a boat is "# #" over "###", so
  // filling the gap must not match it.
  CraftingGrid boat_filled = CraftingGrid::of(3, 3);
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 3; ++x)
      boat_filled.items[static_cast<size_t>(x + y * 3)] = stack("minecraft:acacia_planks");
  auto boat = find_crafting_recipe(boat_filled);
  CHECK(boat.has_value());
  CHECK(boat && boat->recipe->name != std::string("acacia_boat"));

  // Nothing to craft at all.
  CHECK(!find_crafting_recipe(CraftingGrid::of(3, 3)).has_value());
  CHECK(!find_crafting_recipe(CraftingGrid::of(0, 0)).has_value());
  std::printf("  shaped placement + mirror ok\n");
}

void test_shapeless_assignment() {
  // andesite is diorite + cobblestone, in either order.
  const int32_t diorite = item("minecraft:diorite");
  const int32_t cobble = item("minecraft:cobblestone");
  CraftingGrid forward =
      grid3({{{0, 0}, ItemStack::of_id(diorite)}, {{1, 0}, ItemStack::of_id(cobble)}});
  CraftingGrid reversed =
      grid3({{{0, 0}, ItemStack::of_id(cobble)}, {{1, 0}, ItemStack::of_id(diorite)}});
  for (CraftingGrid* g : {&forward, &reversed}) {
    auto match = find_crafting_recipe(*g);
    CHECK(match.has_value());
    if (match)
      CHECK(match->recipe->result_item == item("minecraft:andesite"));
  }

  // ...but not two of the same: the assignment has to be a perfect matching, so
  // one grid cell cannot satisfy both ingredients.
  CraftingGrid two_diorite =
      grid3({{{0, 0}, ItemStack::of_id(diorite)}, {{1, 0}, ItemStack::of_id(diorite)}});
  auto bad = find_crafting_recipe(two_diorite);
  CHECK(!bad.has_value() || bad->recipe->result_item != item("minecraft:andesite"));

  // One ingredient alone is not enough.
  CraftingGrid one = grid3({{{0, 0}, ItemStack::of_id(diorite)}});
  auto partial = find_crafting_recipe(one);
  CHECK(!partial.has_value() || partial->recipe->result_item != item("minecraft:andesite"));
  std::printf("  shapeless assignment ok\n");
}

void test_transmute() {
  // bundle + black_dye -> black_bundle (input first, but order-independent).
  const int32_t bundle = item("minecraft:bundle");
  const int32_t dye = item("minecraft:black_dye");
  CraftingGrid g = grid3({{{0, 0}, ItemStack::of_id(bundle)}, {{1, 0}, ItemStack::of_id(dye)}});
  auto match = find_crafting_recipe(g);
  CHECK(match.has_value());
  if (match) {
    CHECK(match->recipe->kind == RecipeKind::Transmute);
    CHECK(match->recipe->result_item == item("minecraft:black_bundle"));
  }

  // A transmute that would produce its own input is a no-op, and vanilla
  // refuses it — the black_bundle recipe takes the *whole* bundle tag as input,
  // so a black bundle plus black dye must not "transmute" into itself.
  CraftingGrid noop =
      grid3({{{0, 0}, stack("minecraft:black_bundle")}, {{1, 0}, ItemStack::of_id(dye)}});
  auto self = find_crafting_recipe(noop);
  CHECK(!self.has_value() || self->recipe->result_item != item("minecraft:black_bundle"));

  // Two materials is fine, an unknown third item is not.
  CraftingGrid two = grid3({{{0, 0}, ItemStack::of_id(bundle)},
                            {{1, 0}, ItemStack::of_id(dye)},
                            {{2, 0}, ItemStack::of_id(dye)}});
  auto more = find_crafting_recipe(two);
  CHECK(more.has_value() && more->recipe->kind == RecipeKind::Transmute);
  CraftingGrid extra = grid3({{{0, 0}, ItemStack::of_id(bundle)},
                              {{1, 0}, ItemStack::of_id(dye)},
                              {{2, 0}, stack("minecraft:dirt")}});
  auto junk = find_crafting_recipe(extra);
  CHECK(!junk.has_value() || junk->recipe->result_item != item("minecraft:black_bundle"));
  std::printf("  transmute ok\n");
}

void test_unimplemented_kinds_never_match() {
  // Smelting / stonecutting / smithing ingredients sitting in a crafting grid
  // must not craft anything: those recipes belong to other menus.
  const char* single[] = {"iron_ingot_from_smelting_raw_iron",
                          "andesite_slab_from_andesite_stonecutting"};
  for (const char* name : single) {
    const GeneratedRecipe* r = recipe(name);
    if (r == nullptr)
      continue;
    CraftingGrid g = CraftingGrid::of(3, 3);
    g.items[0] = ItemStack::of_id(first_item_of(recipe_cells(*r)[0]));
    CHECK(!find_crafting_recipe(g).has_value());
  }
  const GeneratedRecipe* smithing = recipe("netherite_axe_smithing");
  if (smithing != nullptr) {
    CraftingGrid g = CraftingGrid::of(3, 3);
    for (int c = 0; c < 3; ++c)
      g.items[static_cast<size_t>(c)] = ItemStack::of_id(first_item_of(recipe_cells(*smithing)[c]));
    CHECK(!find_crafting_recipe(g).has_value());
  }

  // The code-driven specials: two damaged iron pickaxes (the repair recipe) must
  // not craft, because the repair logic is not implemented yet — and crucially,
  // no *other* recipe may claim them either.
  const GeneratedRecipe* repair = recipe("repair_item");
  CHECK(repair != nullptr && repair->kind == RecipeKind::Special);
  CHECK(repair->ing_count == 0);
  CraftingGrid tools =
      grid3({{{0, 0}, stack("minecraft:iron_pickaxe")}, {{1, 0}, stack("minecraft:iron_pickaxe")}});
  CHECK(!find_crafting_recipe(tools).has_value());
  std::printf("  unimplemented kinds never match\n");
}

// Every supported recipe, rebuilt from its own ingredients, must match and
// produce its own result. Shapeless recipes are placed in row-major order and
// shaped ones in their exact pattern; both are the natural placement a player
// would use.
void test_every_supported_recipe_round_trips() {
  int checked = 0;
  for (uint32_t i = 0; i < kRecipeCount; ++i) {
    const GeneratedRecipe& r = kRecipes[i];
    const bool supported = r.kind == RecipeKind::Shaped || r.kind == RecipeKind::Shapeless ||
                           r.kind == RecipeKind::Transmute;
    if (!supported)
      continue;
    const GeneratedIngredient* cells = recipe_cells(r);
    CraftingGrid g = CraftingGrid::of(3, 3);
    if (r.kind == RecipeKind::Shaped) {
      for (int y = 0; y < r.height; ++y) {
        for (int x = 0; x < r.width; ++x) {
          const GeneratedIngredient& cell = cells[x + y * r.width];
          if (cell.item_count == 0)
            continue;
          g.items[static_cast<size_t>(x + y * 3)] = ItemStack::of_id(first_item_of(cell));
        }
      }
    } else {
      for (int c = 0; c < r.ing_count; ++c) {
        const int32_t id = first_item_of(cells[c]);
        if (id < 0)
          continue;
        g.items[static_cast<size_t>(c)] = ItemStack::of_id(id);
      }
    }
    auto match = find_crafting_recipe(g);
    if (!match) {
      std::fprintf(stderr, "FAIL %s: ingredients do not craft anything\n", r.name);
      ++failures;
      continue;
    }
    if (match->recipe->result_item != r.result_item) {
      // A different recipe winning is only acceptable when it produces the same
      // item; anything else is an ambiguity that would confuse players.
      std::fprintf(stderr, "FAIL %s: produced %s instead\n", r.name, match->recipe->name);
      ++failures;
    }
    ++checked;
  }
  CHECK(checked >= 1080);
  std::printf("  %d supported recipes round-trip from their own ingredients\n", checked);
}

// Smithing: the three input slots are gated on being any recipe's item for that
// cell, and the transform needs all three (a trim recipe yields nothing, since
// we cannot express a trimmed result).
void test_smithing() {
  const int32_t template_ = item("minecraft:netherite_upgrade_smithing_template");
  const int32_t base = item("minecraft:diamond_sword");
  const int32_t addition = item("minecraft:netherite_ingot");
  const int32_t result = item("minecraft:netherite_sword");

  CHECK(is_smithing_slot_item(SmithingSlot::Template, template_));
  CHECK(is_smithing_slot_item(SmithingSlot::Base, base));
  CHECK(is_smithing_slot_item(SmithingSlot::Addition, addition));
  // The gates are per-slot, not "any smithing input".
  CHECK(!is_smithing_slot_item(SmithingSlot::Base, template_));
  CHECK(!is_smithing_slot_item(SmithingSlot::Template, base));
  CHECK(!is_smithing_slot_item(SmithingSlot::Addition, base));
  CHECK(!is_smithing_slot_item(SmithingSlot::Base, 0));

  auto match =
      find_smithing_recipe(stack("minecraft:netherite_upgrade_smithing_template"),
                           stack("minecraft:diamond_sword"), stack("minecraft:netherite_ingot"));
  CHECK(match.has_value());
  if (match) {
    CHECK(match->recipe->kind == RecipeKind::SmithingTransform);
    CHECK(match->recipe->result_item == result);
    CHECK(match->result_count == 1);
  }

  // Every input is required, and a wrong item in any slot refuses.
  CHECK(!find_smithing_recipe(ItemStack{}, stack("minecraft:diamond_sword"),
                              stack("minecraft:netherite_ingot")));
  CHECK(!find_smithing_recipe(stack("minecraft:netherite_upgrade_smithing_template"), ItemStack{},
                              stack("minecraft:netherite_ingot")));
  CHECK(!find_smithing_recipe(stack("minecraft:netherite_upgrade_smithing_template"),
                              stack("minecraft:diamond_sword"), ItemStack{}));
  CHECK(!find_smithing_recipe(stack("minecraft:coal"), stack("minecraft:diamond_sword"),
                              stack("minecraft:netherite_ingot")));

  // A trim combination has no smithing_transform behind it: the transform
  // matcher refuses it, and `find_smithing_trim_recipe` is what reports it (its
  // own test, `trim`, sweeps all 18).
  const int32_t trim_template = item("minecraft:coast_armor_trim_smithing_template");
  CHECK(is_smithing_slot_item(SmithingSlot::Template, trim_template));
  CHECK(!find_smithing_recipe(stack("minecraft:coast_armor_trim_smithing_template"),
                              stack("minecraft:diamond_chestplate"),
                              stack("minecraft:iron_ingot")));
  CHECK(find_smithing_trim_recipe(stack("minecraft:coast_armor_trim_smithing_template"),
                                  stack("minecraft:diamond_chestplate"),
                                  stack("minecraft:iron_ingot"))
            .has_value());

  // The gate must accept every recipe's item for its cell, so a sweep over the
  // table's smithing recipes pins that nothing is silently unsupported.
  int transforms = 0;
  for (uint32_t i = 0; i < kRecipeCount; ++i) {
    const GeneratedRecipe& r = kRecipes[i];
    if (r.kind != RecipeKind::SmithingTransform)
      continue;
    ++transforms;
    const GeneratedIngredient* cells = recipe_cells(r);
    for (int c = 0; c < 3; ++c) {
      const int32_t id = first_item_of(cells[c]);
      CHECK(id > 0 && is_smithing_slot_item(static_cast<SmithingSlot>(c), id));
    }
    const int32_t t = first_item_of(cells[0]);
    const int32_t b = first_item_of(cells[1]);
    const int32_t a = first_item_of(cells[2]);
    auto m = find_smithing_recipe(ItemStack::of_id(t), ItemStack::of_id(b), ItemStack::of_id(a));
    CHECK(m.has_value());
    if (m)
      CHECK(m->recipe->result_item == r.result_item);
  }
  CHECK(transforms == 12);
  CHECK(!find_smithing_recipe(ItemStack{}, ItemStack{}, ItemStack{}));
  std::printf("  smithing: %d transforms gate and match\n", transforms);
}

} // namespace

int main() {
  test_table_shape();
  test_tag_ingredients();
  test_shaped_placement_and_mirror();
  test_shapeless_assignment();
  test_transmute();
  test_unimplemented_kinds_never_match();
  test_every_supported_recipe_round_trips();
  test_smithing();
  if (failures == 0) {
    std::printf("recipe: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "recipe: %d check(s) failed\n", failures);
  return 1;
}
