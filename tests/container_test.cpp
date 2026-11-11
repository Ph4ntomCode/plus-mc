// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the container layer (Phase 3 milestone C):
//   * the block -> container spec table (menu ids and slot counts),
//   * the block-entity item codec (ContainerHelper / ItemStackWithSlot layout),
//   * the chunk round-trip that carries block entities to disk and back,
//     including every component a stored stack can carry (damage,
//     enchantments, potion contents and trim) through the bytes the region
//     file holds.
#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "world/anvil.h"
#include "world/chunk.h"
#include "world/container.h"
#include "world/effect.h"
#include "world/enchantment.h"
#include "world/item.h"
#include "world/nbt.h"
#include "world/potion.h"
#include "world/registry.h"
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

int32_t item_id(const char* name) {
  auto id = ItemRegistry::instance().id_of(name);
  return id ? *id : -1;
}

// Registry ids by name, through the same lookups the session uses: -1 when the
// name is not in the table.
int32_t enchantment_id(const char* name) {
  const GeneratedEnchantment* def = enchantment_by_name(name);
  return def == nullptr ? -1 : enchantment_id_of(*def);
}

int32_t effect_id(const char* name) {
  const GeneratedMobEffect* def = mob_effect_by_name(name);
  return def == nullptr ? -1 : mob_effect_id_of(*def);
}

int32_t potion_id(const char* name) {
  const GeneratedPotion* def = potion_by_name(name);
  return def == nullptr ? -1 : potion_id_of(*def);
}

// Finds the "Items" list entry for `slot`, or nullptr.
const nbt::Value* items_entry(const nbt::Value& be, int slot) {
  const nbt::Value* items = nbt::child(be, "Items");
  if (items == nullptr || items->type != nbt::Tag::List)
    return nullptr;
  for (const nbt::Value& entry : items->items) {
    const nbt::Value* s = nbt::child(entry, "Slot");
    if (s != nullptr && nbt::as_int(*s) == slot)
      return &entry;
  }
  return nullptr;
}

// Replaces the whole `components` patch of a record's first item entry with one
// hand-written component, so a test can feed the reader a literal NBT value —
// the shape a real server writes — rather than what this engine's own writer
// happens to produce.
nbt::Value patch_components(const nbt::Value& record, std::string_view key, nbt::Value value) {
  nbt::Value patched = record;
  for (auto& [name, entry] : patched.fields) {
    if (name != "Items")
      continue;
    for (nbt::Value& slot : entry.items) {
      if (slot.type != nbt::Tag::Compound)
        continue;
      std::vector<std::pair<std::string, nbt::Value>> comps;
      comps.emplace_back(std::string(key), std::move(value));
      for (auto& [field, field_value] : slot.fields) {
        if (field == "components") {
          field_value = nbt::make_compound(std::move(comps));
          return patched;
        }
      }
      slot.fields.emplace_back("components", nbt::make_compound(std::move(comps)));
      return patched;
    }
  }
  return patched;
}

void test_specs() {
  auto chest = container_spec_for_block("minecraft:chest");
  CHECK(chest.has_value());
  CHECK(chest->slots == 27);
  CHECK(chest->menu_id == 2); // MenuType.generic_9x3

  auto hopper = container_spec_for_block("minecraft:hopper");
  CHECK(hopper.has_value());
  CHECK(hopper->slots == 5);
  CHECK(hopper->menu_id == 16);

  auto shulker = container_spec_for_block("minecraft:purple_shulker_box");
  CHECK(shulker.has_value());
  CHECK(shulker->slots == 27);
  CHECK(shulker->menu_id == 20);

  auto furnace = container_spec_for_block("minecraft:furnace");
  CHECK(furnace.has_value());
  CHECK(furnace->slots == 3);
  CHECK(furnace->menu_id == 14);

  CHECK(!container_spec_for_block("minecraft:stone").has_value());
  CHECK(!container_spec_for_block("minecraft:air").has_value());
  CHECK(!container_spec_for_block("").has_value());
}

// The default-state table and the chest geometry placement depends on. A chest
// placed on its default state must not be waterlogged (the id-table fallback
// used to pick the `waterlogged=true` variant, which renders full of water),
// and the pairing/connected-direction helpers must agree with vanilla's
// ChestBlock.getStateForPlacement / getConnectedDirection.
void test_placement_geometry() {
  auto chest = default_block_state("minecraft:chest");
  CHECK(chest.has_value());
  if (chest) {
    CHECK(chest->key.name == "minecraft:chest");
    CHECK(chest->key.properties == "facing=north,type=single,waterlogged=false");
    CHECK(chest->id == 5544); // not the waterlogged 5543
    // Later lookups only know the mapping itself, not the table entry.
    auto bare = default_block_state("chest");
    CHECK(bare.has_value() && bare->id == chest->id);
  }
  CHECK(!default_block_state("minecraft:not_a_block").has_value());
  // Grass must come back snowy=false, not the first snowy=true variant.
  auto grass = default_block_state("minecraft:grass_block");
  CHECK(grass.has_value() && grass->id == 9);

  // props round-trip + canonical (sorted) form.
  CHECK(chest_props("north", "single", false) == "facing=north,type=single,waterlogged=false");
  CHECK(chest_props("east", "left", true) == "facing=east,type=left,waterlogged=true");
  CHECK(chest_type_is("facing=north,type=left,waterlogged=false", "left"));
  CHECK(!chest_type_is("facing=north,type=left,waterlogged=false", "single"));
  CHECK(chest_facing("facing=west,type=right,waterlogged=true") == "west");
  CHECK(chest_waterlogged("facing=west,type=right,waterlogged=true"));
  CHECK(!chest_waterlogged("facing=west,type=right,waterlogged=false"));

  // Left joins at the facing's clockwise side, right at its counter-clockwise
  // side (Direction.getConnectedDirection). NORTH -> clockwise EAST.
  auto left = chest_partner_pos(BlockPosKey{0, 70, 0}, chest_props("north", "left", false));
  CHECK(left.has_value());
  if (left) {
    CHECK(left->x == 1 && left->y == 70 && left->z == 0);
  }
  auto right = chest_partner_pos(BlockPosKey{0, 70, 0}, chest_props("north", "right", false));
  CHECK(right.has_value());
  if (right) {
    CHECK(right->x == -1 && right->y == 70 && right->z == 0);
  }
  CHECK(
      !chest_partner_pos(BlockPosKey{0, 70, 0}, chest_props("north", "single", false)).has_value());
  CHECK(!chest_partner_pos(BlockPosKey{0, 70, 0}, "facing=north").has_value());

  // BlockPlaceContext: the block faces the player (the look's opposite). Yaw 0
  // looks south, so a chest faces north.
  CHECK(horizontal_direction(0.0f) == "south");
  CHECK(horizontal_direction(90.0f) == "west");
  CHECK(horizontal_direction(180.0f) == "north");
  CHECK(horizontal_direction(-90.0f) == "east");
  CHECK(placement_facing(0.0f) == "north");
  CHECK(placement_facing(90.0f) == "east");
  CHECK(placement_facing(180.0f) == "south");
  CHECK(placement_facing(-90.0f) == "west");
  CHECK(clockwise_direction("north") == "east");
  CHECK(clockwise_direction("east") == "south");
  CHECK(clockwise_direction("south") == "west");
  CHECK(clockwise_direction("west") == "north");
  CHECK(counter_clockwise_direction("north") == "west");
  CHECK(counter_clockwise_direction("west") == "south");
  int dx = 0, dz = 0;
  horizontal_step("north", &dx, &dz);
  CHECK(dx == 0 && dz == -1);
  horizontal_step("east", &dx, &dz);
  CHECK(dx == 1 && dz == 0);
  horizontal_step("", &dx, &dz);
  CHECK(dx == 0 && dz == 0);

  // Only the chest family joins (a barrel's double_menu_id is 0).
  CHECK(is_double_block("minecraft:chest"));
  CHECK(is_double_block("minecraft:trapped_chest"));
  CHECK(!is_double_block("minecraft:barrel"));
  CHECK(!is_double_block("minecraft:hopper"));
  CHECK(!is_double_block("minecraft:stone"));

  // A double container is one 54-slot inventory in two parts, right half
  // first (vanilla's FIRST = type right), and opens generic_9x6 = 5.
  auto spec = container_spec_for_block("minecraft:chest");
  CHECK(spec.has_value());
  if (spec) {
    CHECK(spec->double_menu_id == 5);
    const BlockPosKey right{1, 70, 0};
    const BlockPosKey left{0, 70, 0};
    BlockContainer dc("minecraft:chest", *spec, std::vector<BlockPosKey>{right, left});
    CHECK(dc.total_slots() == 54);
    CHECK(dc.menu_id() == 5);
    CHECK(dc.is_double());
    CHECK(dc.part_index(right) == 0);
    CHECK(dc.part_index(left) == 1);
    CHECK(dc.part_index(BlockPosKey{5, 70, 5}) == -1);
    // Every menu slot indexes into the inventory that backs it: the second
    // half's slot 0 is menu slot 27.
    dc.inventory.set(27, ItemStack::of_id(item_id("minecraft:stone"), 12));
    const std::vector<ItemStack> second = dc.part_items(1);
    CHECK(second.size() == 27);
    CHECK(second[0].count == 12);
    CHECK(dc.part_items(0)[0].empty());
  }
}

void test_item_codec() {
  const int32_t stone = item_id("minecraft:stone");
  const int32_t pickaxe = item_id("minecraft:diamond_pickaxe");
  const int32_t chestplate = item_id("minecraft:iron_chestplate");
  CHECK(stone > 0);
  CHECK(pickaxe > 0);
  CHECK(chestplate > 0);

  const int32_t gold = trim_material_for_item(item_id("minecraft:gold_ingot"));
  const int32_t ward =
      trim_pattern_for_template(item_id("minecraft:ward_armor_trim_smithing_template"));
  CHECK(gold >= 0);
  CHECK(ward >= 0);

  std::vector<ItemStack> items(27);
  items[3] = ItemStack::of_id(stone, 64);
  items[8] = ItemStack::of_id(pickaxe, 1);
  items[8].damage = 1561 - 3; // a nearly broken pickaxe
  items[10] = ItemStack::of_id(chestplate, 1);
  items[10].trim = StackArmorTrim{gold, ward}; // a gold-ward chestplate in a chest

  const nbt::Value be =
      make_container_block_entity("minecraft:chest", BlockPosKey{-31, 81, 0}, items);

  // Vanilla's field names, verbatim: id/x/y/z + the Items list.
  CHECK(nbt::child(be, "id") != nullptr);
  CHECK(nbt::child(be, "x") != nullptr);
  CHECK(nbt::child(be, "y") != nullptr);
  CHECK(nbt::child(be, "z") != nullptr);
  CHECK(block_entity_at_pos(be, -31, 81, 0));
  CHECK(!block_entity_at_pos(be, -31, 81, 1));
  CHECK(!block_entity_at_pos(be, 0, 0, 0));

  // Only the occupied slots are written, and each entry carries Slot/id.
  const nbt::Value* items_list = nbt::child(be, "Items");
  CHECK(items_list != nullptr && items_list->type == nbt::Tag::List);
  CHECK(items_list->items.size() == 3);
  const nbt::Value* e3 = items_entry(be, 3);
  CHECK(e3 != nullptr);
  if (e3 != nullptr) {
    auto n = nbt::as_string(*nbt::child(*e3, "id"));
    CHECK(n.has_value() && *n == "minecraft:stone");
    auto c = nbt::as_int(*nbt::child(*e3, "count"));
    CHECK(c.has_value() && *c == 64);
    CHECK(nbt::child(*e3, "components") == nullptr); // fresh item: no patch
  }
  const nbt::Value* e8 = items_entry(be, 8);
  CHECK(e8 != nullptr);
  if (e8 != nullptr) {
    auto n = nbt::as_string(*nbt::child(*e8, "id"));
    CHECK(n.has_value() && *n == "minecraft:diamond_pickaxe");
    const nbt::Value* comps = nbt::child(*e8, "components");
    CHECK(comps != nullptr && comps->type == nbt::Tag::Compound);
    if (comps != nullptr) {
      auto d = nbt::as_int(*nbt::child(*comps, "minecraft:damage"));
      CHECK(d.has_value() && *d == 1561 - 3);
    }
  }

  // A trimmed piece carries the trim as vanilla's `ArmorTrim.CODEC` does: a
  // `minecraft:trim` compound of two resource locations, or the piece would
  // come back plain after the world reloads.
  const nbt::Value* e10 = items_entry(be, 10);
  CHECK(e10 != nullptr);
  if (e10 != nullptr) {
    const nbt::Value* comps = nbt::child(*e10, "components");
    CHECK(comps != nullptr && comps->type == nbt::Tag::Compound);
    const nbt::Value* trim = comps != nullptr ? nbt::child(*comps, "minecraft:trim") : nullptr;
    CHECK(trim != nullptr && trim->type == nbt::Tag::Compound);
    if (trim != nullptr) {
      auto m = nbt::as_string(*nbt::child(*trim, "material"));
      auto p = nbt::as_string(*nbt::child(*trim, "pattern"));
      CHECK(m.has_value() && *m == "minecraft:gold");
      CHECK(p.has_value() && *p == "minecraft:ward");
    }
  }

  // Decoding gets the same stacks back, in the same slots.
  const std::vector<ItemStack> back = read_block_entity_items(be, 27);
  CHECK(back.size() == 27);
  CHECK(back[3].item == stone && back[3].count == 64);
  CHECK(back[8].item == pickaxe && back[8].count == 1);
  CHECK(back[8].damage == 1561 - 3);
  CHECK(back[10].item == chestplate && back[10].count == 1);
  CHECK(back[10].trim.has_value());
  if (back[10].trim) {
    CHECK(back[10].trim->material == gold);
    CHECK(back[10].trim->pattern == ward);
  }
  CHECK(back[10].same_item(items[10]));
  for (int i = 0; i < 27; ++i)
    if (i != 3 && i != 8 && i != 10)
      CHECK(back[static_cast<size_t>(i)].empty());
}

// ---- the enchantments component ---------------------------------------------
//
// `ItemEnchantments.CODEC` is a map of enchantment id -> level in 1..255, and
// the component *is* that map. It is written under `minecraft:enchantments`,
// read back onto the same registry ids, and a record whose names or levels this
// engine cannot hold is dropped whole rather than read in part.
void test_enchantments_nbt() {
  const int32_t sword = item_id("minecraft:diamond_sword");
  const int32_t sharpness = enchantment_id("minecraft:sharpness");
  const int32_t unbreaking = enchantment_id("minecraft:unbreaking");
  CHECK(sword > 0);
  CHECK(sharpness >= 0 && unbreaking >= 0 && sharpness != unbreaking);

  std::vector<ItemStack> items(27);
  items[0] = ItemStack::of_id(sword, 1);
  items[0].set_enchantment(sharpness, 3); // set_enchantment keeps them sorted by id
  items[0].set_enchantment(unbreaking, 2);

  const nbt::Value be =
      make_container_block_entity("minecraft:chest", BlockPosKey{0, 70, 0}, items);
  const nbt::Value* entry = items_entry(be, 0);
  CHECK(entry != nullptr);
  if (entry != nullptr) {
    const nbt::Value* comps = nbt::child(*entry, "components");
    CHECK(comps != nullptr && comps->type == nbt::Tag::Compound);
    const nbt::Value* ench =
        comps != nullptr ? nbt::child(*comps, "minecraft:enchantments") : nullptr;
    CHECK(ench != nullptr && ench->type == nbt::Tag::Compound);
    if (ench != nullptr) {
      // The map itself: the enchantment's name on the left, its level on the
      // right — no wrapper field around it.
      int32_t found_sharpness = -1, found_unbreaking = -1;
      for (const auto& field : ench->fields) {
        const std::optional<int64_t> level = nbt::as_int(field.second);
        CHECK(level.has_value());
        if (field.first == "minecraft:sharpness")
          found_sharpness = level ? static_cast<int32_t>(*level) : -1;
        if (field.first == "minecraft:unbreaking")
          found_unbreaking = level ? static_cast<int32_t>(*level) : -1;
      }
      CHECK(ench->fields.size() == 2);
      CHECK(found_sharpness == 3);
      CHECK(found_unbreaking == 2);
      // ...and in the stack's own order, which is ascending registry id.
      int32_t previous = -1;
      for (const auto& field : ench->fields) {
        const GeneratedEnchantment* def = enchantment_by_name(field.first);
        CHECK(def != nullptr);
        const int32_t id = def == nullptr ? -1 : enchantment_id_of(*def);
        CHECK(id > previous);
        previous = id;
      }
    }
  }

  const std::vector<ItemStack> back = read_block_entity_items(be, 27);
  CHECK(back[0].item == sword);
  CHECK(back[0].enchantments.size() == 2);
  CHECK(back[0].same_item(items[0]));
  CHECK(back[0].enchantment_level(sharpness) == 3);
  CHECK(back[0].enchantment_level(unbreaking) == 2);

  // A record whose map cannot be represented: an unknown name, a level outside
  // 1..255, a level of the wrong type, or more entries than a stack holds. Each
  // leaves the stack with *no* enchantments, never the entries that happened to
  // parse first.
  auto read_enchanted_map = [&](const nbt::Value& map) {
    std::vector<ItemStack> one(27);
    one[0] = ItemStack::of_id(sword, 1);
    const nbt::Value record =
        make_container_block_entity("minecraft:chest", BlockPosKey{0, 70, 0}, one);
    return read_block_entity_items(patch_components(record, "minecraft:enchantments", map), 27)[0];
  };
  CHECK(read_enchanted_map(nbt::make_compound({{"minecraft:does_not_exist", nbt::make_int(1)}}))
            .enchantments.empty());
  CHECK(read_enchanted_map(nbt::make_compound({{"minecraft:sharpness", nbt::make_int(0)}}))
            .enchantments.empty());
  CHECK(read_enchanted_map(nbt::make_compound({{"minecraft:sharpness", nbt::make_int(256)}}))
            .enchantments.empty());
  CHECK(read_enchanted_map(nbt::make_compound({{"minecraft:sharpness", nbt::make_string("3")}}))
            .enchantments.empty());
  {
    std::vector<std::pair<std::string, nbt::Value>> too_many;
    for (int i = 0; i < static_cast<int>(kMaxEnchantmentsPerStack) + 1 && i < kEnchantmentCount;
         ++i)
      too_many.emplace_back(kEnchantments[i].name, nbt::make_int(1));
    CHECK(too_many.size() == kMaxEnchantmentsPerStack + 1);
    CHECK(read_enchanted_map(nbt::make_compound(std::move(too_many))).enchantments.empty());
  }

  // An id the table cannot name is not written into the map, so a stack that
  // somehow holds one loses it on the way out instead of gaining a name.
  std::vector<ItemStack> partial(27);
  partial[0] = ItemStack::of_id(sword, 1);
  partial[0].enchantments.push_back(ItemEnchantment{-1, 4});
  const nbt::Value partial_be =
      make_container_block_entity("minecraft:chest", BlockPosKey{0, 70, 0}, partial);
  const nbt::Value* partial_entry = items_entry(partial_be, 0);
  CHECK(partial_entry == nullptr || nbt::child(*partial_entry, "components") == nullptr);
}

// ---- the potion contents component ------------------------------------------
//
// `PotionContents.CODEC` is a compound of optional fields (`potion`,
// `custom_color`, `custom_effects`, `custom_name`), or a bare potion id. All
// four are modelled, including each effect's Details and hidden-effect chain.
void test_potion_contents_nbt() {
  const int32_t bottle = item_id("minecraft:potion");
  const int32_t healing = potion_id("minecraft:healing");
  const int32_t poison = effect_id("minecraft:poison");
  CHECK(bottle > 0);
  CHECK(healing >= 0 && poison >= 0);

  // A hidden layer is always the *same* effect as the instance hiding it
  // (`Details` carries no id), so this is a poison hiding a shorter poison —
  // what `/effect` leaves behind when a stronger one is applied over it.
  MobEffectInstance hidden_effect;
  hidden_effect.effect = poison;
  hidden_effect.duration = 100;
  hidden_effect.visible = true;
  MobEffectInstance effect;
  effect.effect = poison;
  effect.duration = 200;
  effect.amplifier = 1;
  effect.ambient = true;
  effect.visible = false;  // show_particles
  effect.show_icon = true; // show_icon, explicitly different from show_particles
  effect.hidden.push_back(hidden_effect);

  std::vector<ItemStack> items(27);
  items[0] = ItemStack::of_id(bottle, 1);
  StackPotionContents contents;
  contents.potion = healing;
  contents.custom_effects.push_back(effect);
  contents.custom_color = 123456;
  contents.custom_name = "Bob";
  items[0].potion_contents = contents;

  const nbt::Value be =
      make_container_block_entity("minecraft:chest", BlockPosKey{0, 70, 0}, items);
  const nbt::Value* entry = items_entry(be, 0);
  CHECK(entry != nullptr);
  if (entry != nullptr) {
    const nbt::Value* comps = nbt::child(*entry, "components");
    const nbt::Value* potion =
        comps != nullptr ? nbt::child(*comps, "minecraft:potion_contents") : nullptr;
    CHECK(potion != nullptr && potion->type == nbt::Tag::Compound);
    if (potion != nullptr) {
      const auto name = nbt::as_string(*nbt::child(*potion, "potion"));
      CHECK(name.has_value() && *name == "minecraft:healing");
      CHECK(nbt::as_int(*nbt::child(*potion, "custom_color")) == 123456);
      const auto custom_name = nbt::as_string(*nbt::child(*potion, "custom_name"));
      CHECK(custom_name.has_value() && *custom_name == "Bob");
      const nbt::Value* effects = nbt::child(*potion, "custom_effects");
      CHECK(effects != nullptr && effects->type == nbt::Tag::List);
      CHECK(effects != nullptr && effects->items.size() == 1);
      if (effects != nullptr && !effects->items.empty()) {
        const nbt::Value& written = effects->items[0];
        CHECK(written.type == nbt::Tag::Compound);
        const auto id = nbt::as_string(*nbt::child(written, "id"));
        CHECK(id.has_value() && *id == "minecraft:poison");
        CHECK(nbt::as_int(*nbt::child(written, "amplifier")) == 1);
        CHECK(nbt::as_int(*nbt::child(written, "duration")) == 200);
        CHECK(nbt::as_int(*nbt::child(written, "ambient")) == 1);
        CHECK(nbt::as_int(*nbt::child(written, "show_particles")) == 0);
        CHECK(nbt::as_int(*nbt::child(written, "show_icon")) == 1);
        // A hidden layer is a Details: no id of its own, because it is always
        // the same effect as the instance hiding it.
        const nbt::Value* hidden = nbt::child(written, "hidden_effect");
        CHECK(hidden != nullptr && hidden->type == nbt::Tag::Compound);
        if (hidden != nullptr) {
          CHECK(nbt::child(*hidden, "id") == nullptr);
          CHECK(nbt::as_int(*nbt::child(*hidden, "duration")) == 100);
          CHECK(nbt::as_int(*nbt::child(*hidden, "show_particles")) == 1);
          CHECK(nbt::as_int(*nbt::child(*hidden, "show_icon")) == 1);
        }
      }
    }
  }

  const std::vector<ItemStack> back = read_block_entity_items(be, 27);
  CHECK(back[0].item == bottle);
  CHECK(back[0].potion_contents.has_value());
  if (back[0].potion_contents) {
    CHECK(back[0].potion_contents->potion == healing);
    CHECK(back[0].potion_contents->custom_color == std::optional<int32_t>(123456));
    CHECK(back[0].potion_contents->custom_name == std::optional<std::string>("Bob"));
    CHECK(back[0].potion_contents->custom_effects.size() == 1);
    if (!back[0].potion_contents->custom_effects.empty()) {
      const MobEffectInstance& read = back[0].potion_contents->custom_effects[0];
      CHECK(read.effect == poison && read.duration == 200 && read.amplifier == 1);
      CHECK(read.ambient && !read.visible && read.show_icon);
      CHECK(read.hidden.size() == 1);
      if (!read.hidden.empty()) {
        CHECK(read.hidden.front().effect == poison); // the hiding instance's own effect
        CHECK(read.hidden.front().duration == 100);
        CHECK(read.hidden.front().visible && read.hidden.front().show_icon);
      }
    }
  }
  CHECK(back[0].same_item(items[0]));
  CHECK(read_block_entity_items(be, 27)[0].same_item(items[0]));

  // The codec's alternative form: a bare potion id means just that potion.
  {
    std::vector<ItemStack> one(27);
    one[0] = ItemStack::of_id(bottle, 1);
    const nbt::Value record =
        make_container_block_entity("minecraft:chest", BlockPosKey{0, 70, 0}, one);
    const std::vector<ItemStack> read = read_block_entity_items(
        patch_components(record, "minecraft:potion_contents", nbt::make_string("minecraft:water")),
        27);
    CHECK(read[0].potion_contents.has_value());
    if (read[0].potion_contents) {
      CHECK(read[0].potion_contents->potion == potion_id("minecraft:water"));
      CHECK(read[0].potion_contents->custom_effects.empty());
    }
  }

  // A record from a real server may leave every field but `id` out: the codec's
  // own defaults are amplifier 0, duration 0, ambient false, show_particles
  // true, and show_icon *following show_particles* — not a fixed true.
  {
    std::vector<ItemStack> one(27);
    one[0] = ItemStack::of_id(bottle, 1);
    const nbt::Value record =
        make_container_block_entity("minecraft:chest", BlockPosKey{0, 70, 0}, one);
    nbt::Value bare_effect = nbt::make_compound(
        {{"id", nbt::make_string("minecraft:poison")}, {"show_particles", nbt::make_byte(0)}});
    const std::vector<ItemStack> read = read_block_entity_items(
        patch_components(record, "minecraft:potion_contents",
                         nbt::make_compound({{"custom_effects",
                                              nbt::make_list(nbt::Tag::Compound, {bare_effect})}})),
        27);
    CHECK(read[0].potion_contents.has_value());
    if (read[0].potion_contents && !read[0].potion_contents->custom_effects.empty()) {
      const MobEffectInstance& read_effect = read[0].potion_contents->custom_effects[0];
      CHECK(read_effect.effect == poison);
      CHECK(read_effect.amplifier == 0 && read_effect.duration == 0);
      CHECK(!read_effect.ambient);
      CHECK(!read_effect.visible);
      CHECK(!read_effect.show_icon); // Details.create's fallback to show_particles
      CHECK(read_effect.hidden.empty());
    }
  }

  // `custom_color` and `custom_name` are display-only but kept: a container
  // round-trip preserves the tint and the name as well as the potion.
  {
    std::vector<ItemStack> one(27);
    one[0] = ItemStack::of_id(bottle, 1);
    const nbt::Value record =
        make_container_block_entity("minecraft:chest", BlockPosKey{0, 70, 0}, one);
    const std::vector<ItemStack> read = read_block_entity_items(
        patch_components(record, "minecraft:potion_contents",
                         nbt::make_compound({{"potion", nbt::make_string("minecraft:healing")},
                                             {"custom_color", nbt::make_int(123456)},
                                             {"custom_name", nbt::make_string("Bob")}})),
        27);
    CHECK(read[0].potion_contents.has_value());
    if (read[0].potion_contents) {
      CHECK(read[0].potion_contents->potion == healing);
      CHECK(read[0].potion_contents->custom_color == std::optional<int32_t>(123456));
      CHECK(read[0].potion_contents->custom_name == std::optional<std::string>("Bob"));
    }
  }

  // A record that cannot be represented drops the whole component: an unknown
  // potion, an unknown effect, an effect without an id, an amplifier outside a
  // byte, a `custom_effects` value that is not a list, and a hidden chain
  // deeper than the wire reader's own cap.
  auto read_potion = [&](const nbt::Value& value) {
    std::vector<ItemStack> one(27);
    one[0] = ItemStack::of_id(bottle, 1);
    const nbt::Value record =
        make_container_block_entity("minecraft:chest", BlockPosKey{0, 70, 0}, one);
    return read_block_entity_items(patch_components(record, "minecraft:potion_contents", value),
                                   27)[0];
  };
  CHECK(!read_potion(nbt::make_compound({{"potion", nbt::make_string("minecraft:does_not_exist")}}))
             .potion_contents.has_value());
  CHECK(!read_potion(nbt::make_string("minecraft:does_not_exist")).potion_contents.has_value());
  CHECK(!read_potion(
             nbt::make_compound(
                 {{"custom_effects",
                   nbt::make_list(nbt::Tag::Compound,
                                  {nbt::make_compound(
                                      {{"id", nbt::make_string("minecraft:does_not_exist")}})})}}))
             .potion_contents.has_value());
  CHECK(
      !read_potion(nbt::make_compound(
                       {{"custom_effects",
                         nbt::make_list(nbt::Tag::Compound,
                                        {nbt::make_compound({{"duration", nbt::make_int(5)}})})}}))
           .potion_contents.has_value());
  CHECK(!read_potion(nbt::make_compound({{"custom_effects", nbt::make_string("minecraft:poison")}}))
             .potion_contents.has_value());
  CHECK(!read_potion(
             nbt::make_compound(
                 {{"custom_effects",
                   nbt::make_list(nbt::Tag::Compound,
                                  {nbt::make_compound({{"id", nbt::make_string("minecraft:poison")},
                                                       {"amplifier", nbt::make_int(256)}})})}}))
             .potion_contents.has_value());
  {
    // Nine nested hidden layers, one past kMaxHiddenEffectLayers.
    nbt::Value deepest = nbt::make_compound({{"duration", nbt::make_int(1)}});
    for (int i = 0; i < kMaxHiddenEffectLayers; ++i)
      deepest = nbt::make_compound({{"hidden_effect", deepest}});
    nbt::Value instance = nbt::make_compound(
        {{"id", nbt::make_string("minecraft:poison")}, {"hidden_effect", deepest}});
    CHECK(!read_potion(nbt::make_compound(
                           {{"custom_effects", nbt::make_list(nbt::Tag::Compound, {instance})}}))
               .potion_contents.has_value());
  }

  // An id or an amplifier the NBT cannot carry is not written: the component is
  // left out rather than written as a different potion.
  {
    std::vector<ItemStack> bad(27);
    bad[0] = ItemStack::of_id(bottle, 1);
    StackPotionContents unnameable;
    unnameable.potion = 9999;
    bad[0].potion_contents = unnameable;
    const nbt::Value record =
        make_container_block_entity("minecraft:chest", BlockPosKey{0, 70, 0}, bad);
    const nbt::Value* bad_entry = items_entry(record, 0);
    CHECK(bad_entry == nullptr || nbt::child(*bad_entry, "components") == nullptr);

    bad[0].potion_contents = StackPotionContents{};
    MobEffectInstance loud;
    loud.effect = poison;
    loud.amplifier = 300; // past UNSIGNED_BYTE
    bad[0].potion_contents->custom_effects.push_back(loud);
    const nbt::Value loud_record =
        make_container_block_entity("minecraft:chest", BlockPosKey{0, 70, 0}, bad);
    const nbt::Value* loud_entry = items_entry(loud_record, 0);
    CHECK(loud_entry == nullptr || nbt::child(*loud_entry, "components") == nullptr);
  }

  // A component that names no potion and adds no effect is a state of its own
  // (a bottle with nothing in it): it round-trips as a present-but-empty
  // component, not as an absent one.
  {
    std::vector<ItemStack> empty_contents(27);
    empty_contents[0] = ItemStack::of_id(bottle, 1);
    empty_contents[0].potion_contents = StackPotionContents{};
    const nbt::Value record =
        make_container_block_entity("minecraft:chest", BlockPosKey{0, 70, 0}, empty_contents);
    CHECK(items_entry(record, 0) != nullptr);
    const std::vector<ItemStack> read = read_block_entity_items(record, 27);
    CHECK(read[0].potion_contents.has_value());
    CHECK(read[0].potion_contents && read[0].potion_contents->empty());
    CHECK(read[0].same_item(empty_contents[0]));
  }
}

// A record written by a real server reads onto this engine's registry ids, and
// a name this table does not carry leaves the piece untrimmed rather than
// half-trimmed.
void test_trim_nbt_interop() {
  using namespace nbt;
  const int32_t chestplate = item_id("minecraft:iron_chestplate");
  const int32_t netherite = trim_material_for_name("minecraft:netherite");
  const int32_t eye = trim_pattern_for_name("minecraft:eye");
  CHECK(netherite >= 0);
  CHECK(eye >= 0);

  // One chest holding one chestplate whose `minecraft:trim` value is `trim`.
  auto read_trim = [&](const nbt::Value& trim) {
    return read_block_entity_items(
        make_compound(
            {{"Items", make_list(Tag::Compound,
                                 {make_compound({{"Slot", make_byte(0)},
                                                 {"id", make_string("minecraft:iron_chestplate")},
                                                 {"count", make_int(1)},
                                                 {"components",
                                                  make_compound({{"minecraft:trim", trim}})}})})}}),
        27)[0];
  };
  const ItemStack vanilla =
      read_trim(make_compound({{"material", make_string("minecraft:netherite")},
                               {"pattern", make_string("minecraft:eye")}}));
  CHECK(vanilla.item == chestplate);
  CHECK(vanilla.trim.has_value());
  if (vanilla.trim) {
    CHECK(vanilla.trim->material == netherite);
    CHECK(vanilla.trim->pattern == eye);
  }

  // An unknown material, an unknown pattern, or a malformed value: no trim at
  // all. The stack itself survives either way.
  CHECK(!read_trim(make_compound({{"material", make_string("minecraft:does_not_exist")},
                                  {"pattern", make_string("minecraft:eye")}}))
             .trim.has_value());
  CHECK(!read_trim(make_compound({{"material", make_string("minecraft:netherite")},
                                  {"pattern", make_string("minecraft:does_not_exist")}}))
             .trim.has_value());
  CHECK(!read_trim(make_compound({{"material", make_string("minecraft:netherite")}}))
             .trim.has_value());
  CHECK(!read_trim(make_string("minecraft:netherite")).trim.has_value());

  // An unset trim id is not a component (StackArmorTrim::complete), so it never
  // reaches the NBT at all.
  std::vector<ItemStack> partial(27);
  partial[0] = ItemStack::of_id(chestplate, 1);
  partial[0].trim = StackArmorTrim{-1, eye};
  const nbt::Value be =
      make_container_block_entity("minecraft:chest", BlockPosKey{0, 70, 0}, partial);
  const nbt::Value* e = items_entry(be, 0);
  CHECK(e == nullptr || nbt::child(*e, "components") == nullptr);
}

void test_codec_robustness() {
  using namespace nbt;
  // An out-of-range slot and an unknown item are skipped, not fatal.
  Value bad = make_compound({
      {"Slot", make_byte(static_cast<int8_t>(99))},
      {"id", make_string("minecraft:stone")},
  });
  Value unknown = make_compound({
      {"Slot", make_byte(2)},
      {"id", make_string("minecraft:does_not_exist")},
  });
  Value good = make_compound({
      {"Slot", make_byte(1)},
      {"id", make_string("minecraft:stone")},
      {"count", make_int(7)},
  });
  Value be = make_compound({
      {"id", make_string("minecraft:chest")},
      {"x", make_int(0)},
      {"y", make_int(0)},
      {"z", make_int(0)},
      {"Items", make_list(Tag::Compound, {bad, unknown, good})},
  });
  const std::vector<ItemStack> items = read_block_entity_items(be, 27);
  CHECK(items.size() == 27);
  CHECK(items[1].item == item_id("minecraft:stone") && items[1].count == 7);
  // count missing -> 1, matching the codec's default.
  Value no_count = make_compound({
      {"id", make_string("minecraft:chest")},
      {"x", make_int(0)},
      {"y", make_int(0)},
      {"z", make_int(0)},
      {"Items",
       make_list(Tag::Compound, {make_compound({{"Slot", make_byte(0)},
                                                {"id", make_string("minecraft:stone")}})})},
  });
  const std::vector<ItemStack> one = read_block_entity_items(no_count, 27);
  CHECK(one[0].count == 1);

  // Unknown fields (sign text, ...) survive a rewrite: only Items is touched.
  Value sign = make_compound({
      {"id", make_string("minecraft:chest")},
      {"x", make_int(1)},
      {"y", make_int(2)},
      {"z", make_int(3)},
      {"CustomName", make_string("keep me")},
  });
  std::vector<ItemStack> two(27);
  two[0] = ItemStack::of_id(item_id("minecraft:stone"), 2);
  write_block_entity_items(&sign, two);
  auto name = nbt::as_string(*nbt::child(sign, "CustomName"));
  CHECK(name.has_value() && *name == "keep me");
  const std::vector<ItemStack> reread = read_block_entity_items(sign, 27);
  CHECK(reread[0].count == 2);

  // An empty container keeps an empty Items list (vanilla writes it either way).
  write_block_entity_items(&sign, std::vector<ItemStack>(27));
  const nbt::Value* list = nbt::child(sign, "Items");
  CHECK(list != nullptr && list->type == nbt::Tag::List && list->items.empty());
}

// Block-entity records carry *absolute* world coordinates, like vanilla. This
// guards the bug where container records used chunk-local x/z: a record from an
// older build (local) must still be found, and a fresh save must use the
// absolute form so vanilla and other tools read the same position.
void test_block_entity_coordinates() {
  const int32_t stone = item_id("minecraft:stone");

  // A chest at absolute (-31, 70, 2) lives in chunk (-2, 0); its local x is 1.
  std::vector<ItemStack> items(27);
  items[4] = ItemStack::of_id(stone, 5);
  const BlockPosKey abs{-31, 70, 2};
  const nbt::Value fresh = make_container_block_entity("minecraft:chest", abs, items);

  // Fresh records are written in vanilla's absolute form.
  CHECK(nbt::as_int(*nbt::child(fresh, "x")) == -31);
  CHECK(nbt::as_int(*nbt::child(fresh, "y")) == 70);
  CHECK(nbt::as_int(*nbt::child(fresh, "z")) == 2);
  CHECK(block_entity_matches(fresh, -31, 70, 2));
  CHECK(!block_entity_matches(fresh, -30, 70, 2));

  // A legacy record for the same position stored chunk-local x/z = (1, 2); it
  // still resolves for an absolute query (records are scoped to their owning
  // chunk, where local and absolute forms cannot collide).
  nbt::Value legacy = fresh;
  for (auto& [name, value] : legacy.fields) {
    if (name == "x")
      value = nbt::make_int(1);
    else if (name == "z")
      value = nbt::make_int(2);
  }
  CHECK(block_entity_matches(legacy, -31, 70, 2));
  const std::vector<ItemStack> got = read_block_entity_items(legacy, 27);
  CHECK(got[4].item == stone && got[4].count == 5);
}

void test_chunk_round_trip() {
  Chunk chunk;
  chunk.x = -2;
  chunk.z = 0;
  chunk.min_section_y = kOverworldMinSectionY;
  chunk.sections.resize(static_cast<size_t>(kOverworldSectionCount));
  for (int i = 0; i < kOverworldSectionCount; ++i) {
    Section& s = chunk.sections[static_cast<size_t>(i)];
    s.y = chunk.min_section_y + i;
    s.has_blocks = true;
    s.block_palette = {{"minecraft:air", ""}};
    s.block_cells.assign(kBlockCellsPerSection, 0);
    s.has_biomes = true;
    s.biome_palette = {"minecraft:plains"};
    s.biome_cells.assign(kBiomeCellsPerSection, 0);
  }
  // A container record plus an unmodelled one that must survive verbatim. Both
  // sit in this chunk (-2, 0), whose x range is [-32, -17] — i.e. absolute
  // coordinates, not the chunk-local 0..15.
  //
  // The chest holds one stack carrying *every* component the engine models — its
  // damage, two enchantments, potion contents (a potion plus a custom effect
  // that hides another) and a trim — so this is the byte-level save/load the
  // region file uses, exercised for each of them. The in-memory readers in the
  // tests above never see the serializer, and a component that survives
  // `make_*`/`child` can still be mangled by an encoder that writes the wrong
  // tag type (a byte for an int, a list without its element type).
  std::vector<ItemStack> items(27);
  items[0] = ItemStack::of_id(item_id("minecraft:iron_chestplate"), 1);
  items[0].damage = 42;
  items[0].set_enchantment(enchantment_id("minecraft:sharpness"), 3);
  items[0].set_enchantment(enchantment_id("minecraft:unbreaking"), 2);
  {
    StackPotionContents contents;
    contents.potion = potion_id("minecraft:healing");
    MobEffectInstance effect;
    effect.effect = effect_id("minecraft:poison");
    effect.amplifier = 2;
    effect.duration = 120;
    effect.ambient = true;
    effect.visible = false; // show_particles
    effect.show_icon = true;
    // A hidden layer carries no effect of its own: it is always the effect of
    // the instance hiding it, which is what the writer relies on.
    MobEffectInstance hidden;
    hidden.effect = effect.effect;
    hidden.amplifier = 1;
    hidden.duration = 40;
    effect.hidden.push_back(hidden);
    contents.custom_effects.push_back(effect);
    items[0].potion_contents = contents;
  }
  items[0].trim = StackArmorTrim{trim_material_for_name("minecraft:netherite"),
                                 trim_pattern_for_name("minecraft:eye")};
  chunk.block_entities.push_back(
      make_container_block_entity("minecraft:chest", BlockPosKey{-31, 70, 2}, items));
  chunk.block_entities.push_back(nbt::make_compound({
      {"id", nbt::make_string("minecraft:sign")},
      {"x", nbt::make_int(-30)},
      {"y", nbt::make_int(71)},
      {"z", nbt::make_int(4)},
      {"Text1", nbt::make_string("{\"text\":\"hi\"}")},
  }));

  const std::vector<uint8_t> bytes = nbt::serialize(serialize_chunk_nbt(chunk, 4903), "");
  // ...and the components really are in the bytes: the record names each one by
  // resource location, so a reader that quietly dropped a component (or a
  // writer that never emitted it) cannot make these appear.
  auto contains = [&bytes](std::string_view needle) {
    return std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end()) != bytes.end();
  };
  CHECK(contains("minecraft:enchantments"));
  CHECK(contains("minecraft:sharpness"));
  CHECK(contains("minecraft:potion_contents"));
  CHECK(contains("hidden_effect"));
  CHECK(contains("minecraft:trim"));
  CHECK(contains("minecraft:netherite"));
  CHECK(contains("minecraft:eye"));
  auto root = nbt::parse(bytes.data(), bytes.size());
  CHECK(root.has_value());
  if (!root) {
    std::fprintf(stderr, "chunk NBT did not parse back\n");
    return;
  }
  Chunk back;
  CHECK(parse_chunk_nbt(*root, kOverworldMinSectionY, kOverworldSectionCount, &back));
  CHECK(back.x == -2 && back.z == 0);
  CHECK(back.block_entities.size() == 2);
  if (back.block_entities.size() == 2) {
    // The absolute position survives the save/load round trip verbatim.
    CHECK(nbt::as_int(*nbt::child(back.block_entities[0], "x")) == -31);
    CHECK(nbt::as_int(*nbt::child(back.block_entities[0], "z")) == 2);
    CHECK(block_entity_matches(back.block_entities[0], -31, 70, 2));
    const std::vector<ItemStack> got = read_block_entity_items(back.block_entities[0], 27);
    CHECK(got[0].item == item_id("minecraft:iron_chestplate"));
    // `same_item` is damage, enchantments, potion contents (with each effect's
    // hidden layer) and trim in one compare, so this is the whole component set
    // off the bytes. The parts are then named, so a failure says which one.
    CHECK(got[0].same_item(items[0]));
    CHECK(got[0].damage == 42);
    CHECK(got[0].enchantment_level(enchantment_id("minecraft:sharpness")) == 3);
    CHECK(got[0].enchantment_level(enchantment_id("minecraft:unbreaking")) == 2);
    CHECK(got[0].potion_contents.has_value());
    if (got[0].potion_contents) {
      CHECK(got[0].potion_contents->potion == potion_id("minecraft:healing"));
      CHECK(got[0].potion_contents->custom_effects.size() == 1);
      if (got[0].potion_contents->custom_effects.size() == 1) {
        const MobEffectInstance& effect = got[0].potion_contents->custom_effects[0];
        CHECK(effect.effect == effect_id("minecraft:poison"));
        CHECK(effect.amplifier == 2 && effect.duration == 120);
        CHECK(effect.ambient && !effect.visible && effect.show_icon);
        CHECK(effect.hidden.size() == 1);
        if (effect.hidden.size() == 1) {
          CHECK(effect.hidden[0].effect == effect_id("minecraft:poison"));
          CHECK(effect.hidden[0].amplifier == 1 && effect.hidden[0].duration == 40);
        }
      }
    }
    CHECK(got[0].trim.has_value());
    if (got[0].trim) {
      CHECK(got[0].trim->material == items[0].trim->material);
      CHECK(got[0].trim->pattern == items[0].trim->pattern);
    }
    // The sign's text survived: we only ever read/write the Items field.
    auto text = nbt::as_string(*nbt::child(back.block_entities[1], "Text1"));
    CHECK(text.has_value() && *text == "{\"text\":\"hi\"}");
    CHECK(block_entity_matches(back.block_entities[1], -30, 71, 4));
  }

  // A chunk with no block entities still writes an (empty) list, so vanilla
  // never sees a missing field.
  Chunk bare = back;
  bare.block_entities.clear();
  const std::vector<uint8_t> bare_bytes = nbt::serialize(serialize_chunk_nbt(bare, 4903), "");
  auto bare_root = nbt::parse(bare_bytes.data(), bare_bytes.size());
  CHECK(bare_root.has_value());
  if (bare_root) {
    const nbt::Value* bes = nbt::child(*bare_root, "block_entities");
    CHECK(bes != nullptr && bes->type == nbt::Tag::List && bes->items.empty());
  }
}

} // namespace

int main() {
  test_specs();
  test_placement_geometry();
  test_item_codec();
  test_enchantments_nbt();
  test_potion_contents_nbt();
  test_trim_nbt_interop();
  test_codec_robustness();
  test_block_entity_coordinates();
  test_chunk_round_trip();
  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("container: all checks passed\n");
  return 0;
}
