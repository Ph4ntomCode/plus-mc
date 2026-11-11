// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for armor trims (Phase 3 milestone D2, the smithing table):
//   * the generated registries: 11 trim materials and 18 trim patterns with
//     dense ids, one providing item each, and the pattern -> template item link
//     resolved from the recipe data;
//   * the matcher: a `smithing_trim` combination resolves to its material and
//     pattern, every trim recipe in the table is matched by its own three
//     ingredients and resolves to its own pattern, and every refusal (an empty
//     input, a non-material addition, a non-template template, an item the base
//     cell does not accept) is a *no match* rather than a wrong match;
//   * the two registry name lookups: the NBT form of the trim component is a
//     pair of resource locations (`ArmorTrim.CODEC`), so the tables have to
//     answer both ways and never confuse a pattern for a material;
//   * the `minecraft:trim` component on the wire, both directions, so a trimmed
//     piece reaches the client as trimmed and comes back from a client intact;
//   * `same_item`: a trimmed stack is not the same item as a plain one, nor as
//     one trimmed differently, which is what keeps them from merging in an
//     inventory.
#include <cstdio>
#include <string>
#include <vector>

#include "net/byte_buffer.h"
#include "world/item.h"
#include "world/item_registry.h"
#include "world/recipe.h"
#include "world/trim.h"

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

void append_varint(std::vector<uint8_t>* out, uint32_t value) {
  while (true) {
    uint8_t byte = static_cast<uint8_t>(value & 0x7F);
    value >>= 7;
    if (value != 0)
      byte |= 0x80;
    out->push_back(byte);
    if (value == 0)
      return;
  }
}

void test_registry_shape() {
  CHECK(kTrimMaterialCount == 11);
  CHECK(kTrimPatternCount == 18);
  for (uint32_t i = 0; i < kTrimMaterialCount; ++i) {
    CHECK(kTrimMaterials[i].id == static_cast<int32_t>(i));
    CHECK(kTrimMaterials[i].item > 0);
    CHECK(kTrimMaterials[i].name != nullptr && kTrimMaterials[i].name[0] != '\0');
    // The lookup is the table's own inverse.
    CHECK(trim_material_for_item(kTrimMaterials[i].item) == kTrimMaterials[i].id);
  }
  for (uint32_t i = 0; i < kTrimPatternCount; ++i) {
    CHECK(kTrimPatterns[i].id == static_cast<int32_t>(i));
    CHECK(kTrimPatterns[i].template_item > 0);
    CHECK(trim_pattern_for_template(kTrimPatterns[i].template_item) == kTrimPatterns[i].id);
  }
  // The two links are disjoint in both directions: an item provides at most one
  // material and is a template for at most one pattern (the generator refuses
  // otherwise, and a lookup that answered two ways would be a silent mis-trim).
  for (uint32_t i = 0; i < kTrimMaterialCount; ++i) {
    for (uint32_t j = i + 1; j < kTrimMaterialCount; ++j)
      CHECK(kTrimMaterials[i].item != kTrimMaterials[j].item);
    for (uint32_t j = 0; j < kTrimPatternCount; ++j)
      CHECK(kTrimMaterials[i].item != kTrimPatterns[j].template_item);
  }
  for (uint32_t i = 0; i < kTrimPatternCount; ++i)
    for (uint32_t j = i + 1; j < kTrimPatternCount; ++j)
      CHECK(kTrimPatterns[i].template_item != kTrimPatterns[j].template_item);

  // The items the tables name are real, and the materials are the ones that
  // vanilla's `Items.java` tags with the component: the items of the tag are
  // exactly the ones that answer here, which is what makes the addition slot's
  // gate and this lookup agree.
  const int32_t expected_materials[] = {
      item("minecraft:amethyst_shard"), item("minecraft:copper_ingot"),
      item("minecraft:diamond"),        item("minecraft:emerald"),
      item("minecraft:gold_ingot"),     item("minecraft:iron_ingot"),
      item("minecraft:lapis_lazuli"),   item("minecraft:netherite_ingot"),
      item("minecraft:quartz"),         item("minecraft:redstone"),
      item("minecraft:resin_brick")};
  for (int32_t id : expected_materials)
    CHECK(trim_material_for_item(id) >= 0);
  // ...and an item outside that set provides none.
  CHECK(trim_material_for_item(item("minecraft:oak_planks")) < 0);
  CHECK(trim_material_for_item(0) < 0);
  CHECK(trim_pattern_for_template(item("minecraft:oak_planks")) < 0);
  CHECK(trim_pattern_for_template(0) < 0);
}

// The NBT direction: a trimmed stack in a chest stores "minecraft:gold" and
// "minecraft:bolt", so both tables resolve a name to an id and an id back, and
// the two namespaces never collide.
void test_registry_names() {
  for (uint32_t i = 0; i < kTrimMaterialCount; ++i) {
    const char* name = trim_material_name(kTrimMaterials[i].id);
    CHECK(name != nullptr);
    if (name != nullptr) {
      CHECK(std::string(name) == kTrimMaterials[i].name);
      CHECK(trim_material_for_name(name) == kTrimMaterials[i].id);
    }
  }
  for (uint32_t i = 0; i < kTrimPatternCount; ++i) {
    const char* name = trim_pattern_name(kTrimPatterns[i].id);
    CHECK(name != nullptr);
    if (name != nullptr) {
      CHECK(std::string(name) == kTrimPatterns[i].name);
      CHECK(trim_pattern_for_name(name) == kTrimPatterns[i].id);
    }
  }
  // The item links resolve to the same names, which is what a trim applied at
  // the smithing table writes into a chest.
  const char* gold = trim_material_name(trim_material_for_item(item("minecraft:gold_ingot")));
  CHECK(gold != nullptr && std::string(gold) == "minecraft:gold");
  const char* ward = trim_pattern_name(
      trim_pattern_for_template(item("minecraft:ward_armor_trim_smithing_template")));
  CHECK(ward != nullptr && std::string(ward) == "minecraft:ward");

  // Unknown ids and names answer "nothing", not a neighbour's value.
  CHECK(trim_material_name(static_cast<int32_t>(kTrimMaterialCount)) == nullptr);
  CHECK(trim_pattern_name(static_cast<int32_t>(kTrimPatternCount)) == nullptr);
  CHECK(trim_material_name(-1) == nullptr);
  CHECK(trim_pattern_name(-1) == nullptr);
  CHECK(trim_material_for_name("minecraft:does_not_exist") < 0);
  CHECK(trim_pattern_for_name("minecraft:does_not_exist") < 0);
  CHECK(trim_material_for_name("") < 0);
  CHECK(trim_pattern_for_name("") < 0);
  // A name from the other registry is not a name here: the two are separate
  // namespaces even though both are `minecraft:*` resource locations.
  for (uint32_t i = 0; i < kTrimPatternCount; ++i)
    CHECK(trim_material_for_name(kTrimPatterns[i].name) < 0);
  for (uint32_t i = 0; i < kTrimMaterialCount; ++i)
    CHECK(trim_pattern_for_name(kTrimMaterials[i].name) < 0);
}

void test_matcher() {
  const int32_t template_item = item("minecraft:coast_armor_trim_smithing_template");
  const int32_t chestplate = item("minecraft:iron_chestplate");
  const int32_t ingot = item("minecraft:iron_ingot");

  auto good = find_smithing_trim_recipe(ItemStack::of_id(template_item),
                                        ItemStack::of_id(chestplate), ItemStack::of_id(ingot));
  CHECK(good.has_value());
  if (good) {
    CHECK(good->recipe->kind == RecipeKind::SmithingTrim);
    CHECK(good->pattern == trim_pattern_for_template(template_item));
    CHECK(good->material == trim_material_for_item(ingot));
    CHECK(good->material >= 0 && good->pattern >= 0);
  }

  // A transform stays a transform: the netherite upgrade is no trim.
  CHECK(!find_smithing_trim_recipe(stack("minecraft:netherite_upgrade_smithing_template"),
                                   stack("minecraft:diamond_sword"),
                                   stack("minecraft:netherite_ingot")));
  // Every input matters.
  CHECK(!find_smithing_trim_recipe(ItemStack{}, stack("minecraft:iron_chestplate"),
                                   stack("minecraft:iron_ingot")));
  CHECK(!find_smithing_trim_recipe(stack("minecraft:coast_armor_trim_smithing_template"),
                                   ItemStack{}, stack("minecraft:iron_ingot")));
  CHECK(!find_smithing_trim_recipe(stack("minecraft:coast_armor_trim_smithing_template"),
                                   stack("minecraft:iron_chestplate"), ItemStack{}));
  // The template must be *that* pattern's template, and the addition a material
  // with the component: a template with a wrong base, a non-material addition, a
  // non-template template and a non-trimmable base all refuse.
  CHECK(!find_smithing_trim_recipe(stack("minecraft:netherite_upgrade_smithing_template"),
                                   stack("minecraft:iron_chestplate"),
                                   stack("minecraft:iron_ingot")));
  CHECK(!find_smithing_trim_recipe(stack("minecraft:coast_armor_trim_smithing_template"),
                                   stack("minecraft:iron_chestplate"),
                                   stack("minecraft:oak_planks")));
  CHECK(!find_smithing_trim_recipe(stack("minecraft:oak_planks"),
                                   stack("minecraft:iron_chestplate"),
                                   stack("minecraft:iron_ingot")));
  CHECK(!find_smithing_trim_recipe(stack("minecraft:coast_armor_trim_smithing_template"),
                                   stack("minecraft:oak_planks"), stack("minecraft:iron_ingot")));

  // The data-derived sweep: every trim recipe in the table is matched by its own
  // three ingredients and must resolve to its own pattern. This is what catches
  // a generator or table that silently mangled one pattern.
  int trims = 0;
  for (uint32_t i = 0; i < kRecipeCount; ++i) {
    const GeneratedRecipe& recipe = kRecipes[i];
    if (recipe.kind != RecipeKind::SmithingTrim)
      continue;
    ++trims;
    CHECK(recipe.ing_count == 3);
    const GeneratedIngredient* cells = recipe_cells(recipe);
    const int32_t t = ingredient_items(cells[0]).front();
    const int32_t b = ingredient_items(cells[1]).front();
    const int32_t a = ingredient_items(cells[2]).front();
    auto match =
        find_smithing_trim_recipe(ItemStack::of_id(t), ItemStack::of_id(b), ItemStack::of_id(a));
    CHECK(match.has_value());
    if (match) {
      CHECK(match->recipe == &recipe);
      CHECK(match->pattern == trim_pattern_for_template(t));
      CHECK(match->material == trim_material_for_item(a));
    }
    // The template slot's gate accepts it too, or the client would refuse the
    // very item the recipe is built around.
    CHECK(is_smithing_slot_item(SmithingSlot::Template, t));
    CHECK(is_smithing_slot_item(SmithingSlot::Base, b));
    CHECK(is_smithing_slot_item(SmithingSlot::Addition, a));
  }
  CHECK(trims == 18);
}

// The component the client draws the trim from: write_item_stack must emit it
// (id 56, material and pattern as `id + 1`) and read_item_stack must read it
// back out of a client's delimited patch.
void test_component_wire() {
  const int32_t chestplate = item("minecraft:iron_chestplate");
  const int32_t gold = trim_material_for_item(item("minecraft:gold_ingot"));
  const int32_t bolt =
      trim_pattern_for_template(item("minecraft:bolt_armor_trim_smithing_template"));
  CHECK(gold >= 0 && bolt >= 0);

  ItemStack stack = ItemStack::of_id(chestplate, 1);
  stack.trim = StackArmorTrim{gold, bolt};
  mc::net::Writer w;
  write_item_stack(w, stack);
  const std::vector<uint8_t> bytes = std::move(w).take();

  std::vector<uint8_t> expected;
  append_varint(&expected, 1); // count
  append_varint(&expected, static_cast<uint32_t>(chestplate));
  append_varint(&expected, 1); // one added component
  append_varint(&expected, 0); // none removed
  append_varint(&expected, static_cast<uint32_t>(kComponentTrimId));
  append_varint(&expected, static_cast<uint32_t>(gold + 1));
  append_varint(&expected, static_cast<uint32_t>(bolt + 1));
  CHECK(bytes == expected);

  // The client's own form is the *delimited* patch (set_creative_mode_slot,
  // container_click): every component value is byte-length prefixed.
  std::vector<uint8_t> delimited;
  append_varint(&delimited, 1); // count
  append_varint(&delimited, static_cast<uint32_t>(chestplate));
  append_varint(&delimited, 1); // one added component
  append_varint(&delimited, 0); // none removed
  append_varint(&delimited, static_cast<uint32_t>(kComponentTrimId));
  std::vector<uint8_t> value;
  append_varint(&value, static_cast<uint32_t>(gold + 1));
  append_varint(&value, static_cast<uint32_t>(bolt + 1));
  append_varint(&delimited, static_cast<uint32_t>(value.size()));
  delimited.insert(delimited.end(), value.begin(), value.end());

  mc::net::Reader r(delimited);
  ItemStack read;
  CHECK(read_item_stack(r, &read));
  CHECK(read.item == chestplate && read.count == 1);
  CHECK(read.trim.has_value());
  if (read.trim) {
    CHECK(read.trim->material == gold);
    CHECK(read.trim->pattern == bolt);
  }

  // `same_item` is `isSameItemSameComponents`: the trimmed piece is not the
  // plain one, nor the same piece wearing another pattern, nor another material.
  const int32_t coast =
      trim_pattern_for_template(item("minecraft:coast_armor_trim_smithing_template"));
  CHECK(coast >= 0 && coast != bolt);
  ItemStack other_pattern = stack;
  other_pattern.trim = StackArmorTrim{gold, coast};
  CHECK(!stack.same_item(other_pattern));
  ItemStack other_material = stack;
  other_material.trim =
      StackArmorTrim{trim_material_for_item(item("minecraft:copper_ingot")), bolt};
  CHECK(!stack.same_item(other_material));
  CHECK(!stack.same_item(ItemStack::of_id(chestplate, 1)));
  CHECK(stack.same_item(stack));
}

void test_read_drops_a_direct_holder() {
  // A *direct* holder (id 0) carries the material's own definition instead of a
  // registry reference, which this engine does not model: the component is
  // dropped, and the stack's other fields survive.
  const int32_t chestplate = item("minecraft:iron_chestplate");
  std::vector<uint8_t> delimited;
  append_varint(&delimited, 1);
  append_varint(&delimited, static_cast<uint32_t>(chestplate));
  append_varint(&delimited, 1);
  append_varint(&delimited, 0);
  append_varint(&delimited, static_cast<uint32_t>(kComponentTrimId));
  std::vector<uint8_t> value;
  append_varint(&value, 0); // direct material
  append_varint(&value, 0); // direct pattern
  append_varint(&delimited, static_cast<uint32_t>(value.size()));
  delimited.insert(delimited.end(), value.begin(), value.end());

  mc::net::Reader r(delimited);
  ItemStack read;
  CHECK(read_item_stack(r, &read));
  CHECK(read.item == chestplate);
  CHECK(!read.trim.has_value());
}

} // namespace

int main() {
  test_registry_shape();
  test_registry_names();
  test_matcher();
  test_component_wire();
  test_read_drops_a_direct_holder();
  if (failures == 0)
    std::printf("trim: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
