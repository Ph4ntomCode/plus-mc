// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/container.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "world/item.h"

#include "world/effect.h"
#include "world/enchantment.h"
#include "world/potion.h"
#include "world/trim.h"

namespace mc::world {
namespace {

// Menu ids from the static `MenuType` registration order (see the header for
// why these are static ids).
constexpr int kMenuGeneric9x3 = 2;
constexpr int kMenuGeneric9x6 = 5;
constexpr int kMenuGeneric3x3 = 6;
constexpr int kMenuBlastFurnace = 10;
constexpr int kMenuBrewingStand = 11;
constexpr int kMenuFurnace = 14;
constexpr int kMenuHopper = 16;
constexpr int kMenuShulkerBox = 20;
constexpr int kMenuSmoker = 22;

struct Entry {
  std::string_view block;
  ContainerSpec spec;
};

constexpr Entry kContainers[] = {
    // Chests: one 9x3 menu, or a 9x6 one when two of them join
    // (ChestMenu.threeRows / ChestMenu.sixRows). Barrels do not join.
    {"minecraft:chest", {27, kMenuGeneric9x3, kMenuGeneric9x6, "Chest"}},
    {"minecraft:trapped_chest", {27, kMenuGeneric9x3, kMenuGeneric9x6, "Trapped Chest"}},
    {"minecraft:barrel", {27, kMenuGeneric9x3, 0, "Barrel"}},
    // Shulker boxes (all 17 colours share the menu and size).
    {"minecraft:shulker_box", {27, kMenuShulkerBox, 0, "Shulker Box"}},
    {"minecraft:white_shulker_box", {27, kMenuShulkerBox, 0, "Shulker Box"}},
    {"minecraft:orange_shulker_box", {27, kMenuShulkerBox, 0, "Shulker Box"}},
    {"minecraft:magenta_shulker_box", {27, kMenuShulkerBox, 0, "Shulker Box"}},
    {"minecraft:light_blue_shulker_box", {27, kMenuShulkerBox, 0, "Shulker Box"}},
    {"minecraft:yellow_shulker_box", {27, kMenuShulkerBox, 0, "Shulker Box"}},
    {"minecraft:lime_shulker_box", {27, kMenuShulkerBox, 0, "Shulker Box"}},
    {"minecraft:pink_shulker_box", {27, kMenuShulkerBox, 0, "Shulker Box"}},
    {"minecraft:gray_shulker_box", {27, kMenuShulkerBox, 0, "Shulker Box"}},
    {"minecraft:light_gray_shulker_box", {27, kMenuShulkerBox, 0, "Shulker Box"}},
    {"minecraft:cyan_shulker_box", {27, kMenuShulkerBox, 0, "Shulker Box"}},
    {"minecraft:purple_shulker_box", {27, kMenuShulkerBox, 0, "Shulker Box"}},
    {"minecraft:blue_shulker_box", {27, kMenuShulkerBox, 0, "Shulker Box"}},
    {"minecraft:brown_shulker_box", {27, kMenuShulkerBox, 0, "Shulker Box"}},
    {"minecraft:green_shulker_box", {27, kMenuShulkerBox, 0, "Shulker Box"}},
    {"minecraft:red_shulker_box", {27, kMenuShulkerBox, 0, "Shulker Box"}},
    {"minecraft:black_shulker_box", {27, kMenuShulkerBox, 0, "Shulker Box"}},
    // Dispensers and droppers are a 3x3.
    {"minecraft:dispenser", {9, kMenuGeneric3x3, 0, "Dispenser"}},
    {"minecraft:dropper", {9, kMenuGeneric3x3, 0, "Dropper"}},
    {"minecraft:hopper", {5, kMenuHopper, 0, "Hopper"}},
    // Furnace family: 3 slots (input 0, fuel 1, output 2) and a real smelting
    // tick (milestone E) — `cooking` is what makes the record carry the timers
    // and the menu carry its four data slots.
    {"minecraft:furnace", {3, kMenuFurnace, 0, "Furnace", true}},
    {"minecraft:blast_furnace", {3, kMenuBlastFurnace, 0, "Blast Furnace", true}},
    {"minecraft:smoker", {3, kMenuSmoker, 0, "Smoker", true}},
    // A brewing stand: three bottle slots, an ingredient and a fuel, with a
    // 400-tick brew timer (see brewing.h) — `brewing` is what makes the record
    // carry `BrewTime`/`Fuel` and the menu carry its two data slots.
    {"minecraft:brewing_stand", {5, kMenuBrewingStand, 0, "Brewing Stand", false, true}},
};

} // namespace

std::optional<ContainerSpec> container_spec_for_block(std::string_view block) {
  for (const Entry& e : kContainers)
    if (e.block == block)
      return e.spec;
  return std::nullopt;
}

std::vector<ItemStack> BlockContainer::part_items(int index) const {
  std::vector<ItemStack> out;
  if (index < 0 || index >= static_cast<int>(parts.size()))
    return out;
  const int base = index * spec.slots;
  out.reserve(static_cast<size_t>(spec.slots));
  for (int i = 0; i < spec.slots; ++i)
    out.push_back(inventory.get(base + i));
  return out;
}

bool is_double_block(std::string_view block) {
  auto spec = container_spec_for_block(block);
  return spec && spec->double_menu_id != 0;
}

namespace {
// The value of `key` in a canonical "a=1,b=2" properties string.
std::string prop_value(std::string_view props, std::string_view key) {
  size_t pos = 0;
  while (pos < props.size()) {
    size_t comma = props.find(',', pos);
    if (comma == std::string_view::npos)
      comma = props.size();
    std::string_view pair = props.substr(pos, comma - pos);
    size_t eq = pair.find('=');
    if (eq != std::string_view::npos && pair.substr(0, eq) == key)
      return std::string(pair.substr(eq + 1));
    pos = comma + 1;
  }
  return {};
}
} // namespace

std::string block_property(std::string_view props, std::string_view key) {
  return prop_value(props, key);
}

bool chest_type_is(std::string_view props, std::string_view type) {
  return prop_value(props, "type") == type;
}

std::string chest_facing(std::string_view props) {
  return prop_value(props, "facing");
}

bool chest_waterlogged(std::string_view props) {
  return prop_value(props, "waterlogged") == "true";
}

std::string with_property(std::string_view props, std::string_view key, std::string_view value) {
  std::vector<std::pair<std::string, std::string>> pairs;
  size_t pos = 0;
  while (!props.empty() && pos <= props.size()) {
    size_t comma = props.find(',', pos);
    if (comma == std::string_view::npos)
      comma = props.size();
    const std::string_view pair = props.substr(pos, comma - pos);
    if (!pair.empty()) {
      const size_t eq = pair.find('=');
      if (eq == std::string_view::npos)
        pairs.emplace_back(std::string(pair), std::string());
      else
        pairs.emplace_back(std::string(pair.substr(0, eq)), std::string(pair.substr(eq + 1)));
    }
    if (comma == props.size())
      break;
    pos = comma + 1;
  }
  bool replaced = false;
  for (auto& pair : pairs) {
    if (pair.first == key) {
      pair.second = std::string(value);
      replaced = true;
    }
  }
  if (!replaced)
    pairs.emplace_back(std::string(key), std::string(value));
  // The table's form is key-sorted (`facing=north,lit=false`) and the lookup is a
  // string compare, so the order has to match.
  std::sort(pairs.begin(), pairs.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  std::string out;
  for (const auto& pair : pairs) {
    if (!out.empty())
      out += ',';
    out += pair.first;
    out += '=';
    out += pair.second;
  }
  return out;
}

std::optional<BlockPosKey> chest_partner_pos(const BlockPosKey& pos, std::string_view props) {
  const std::string type = prop_value(props, "type");
  const std::string facing = prop_value(props, "facing");
  if (facing.empty() || (type != "left" && type != "right"))
    return std::nullopt;
  // Vanilla's ChestBlock.getConnectedDirection. Horizontal directions, so
  // clockwise is NORTH -> EAST -> SOUTH -> WEST.
  //   left  -> facing clockwise
  //   right -> facing counter-clockwise
  int dx = 0, dz = 0;
  if (facing == "north") {
    dx = (type == "left") ? 1 : -1;
  } else if (facing == "south") {
    dx = (type == "left") ? -1 : 1;
  } else if (facing == "west") {
    dz = (type == "left") ? -1 : 1;
  } else { // east
    dz = (type == "left") ? 1 : -1;
  }
  return BlockPosKey{pos.x + dx, pos.y, pos.z + dz};
}

std::string chest_props(std::string_view facing, std::string_view type, bool waterlogged) {
  std::string out = "facing=";
  out += facing;
  out += ",type=";
  out += type;
  out += waterlogged ? ",waterlogged=true" : ",waterlogged=false";
  return out;
}

std::string horizontal_direction(float yaw) {
  // Direction.fromYRot: floor(yaw / 90 + 0.5) & 3 into BY_2D_DATA
  // = {SOUTH, WEST, NORTH, EAST}.
  const int index = static_cast<int>(std::floor(yaw / 90.0f + 0.5f)) & 3;
  static constexpr const char* kBy2d[] = {"south", "west", "north", "east"};
  return kBy2d[index];
}

std::string placement_facing(float yaw) {
  // BlockPlaceContext.getHorizontalDirection().getOpposite(): a block faces the
  // player who placed it.
  const std::string dir = horizontal_direction(yaw);
  if (dir == "north")
    return "south";
  if (dir == "south")
    return "north";
  if (dir == "east")
    return "west";
  return "east";
}

std::string clockwise_direction(std::string_view facing) {
  if (facing == "north")
    return "east";
  if (facing == "east")
    return "south";
  if (facing == "south")
    return "west";
  return "north";
}

std::string counter_clockwise_direction(std::string_view facing) {
  if (facing == "north")
    return "west";
  if (facing == "west")
    return "south";
  if (facing == "south")
    return "east";
  return "north";
}

void horizontal_step(std::string_view facing, int* dx, int* dz) {
  *dx = 0;
  *dz = 0;
  if (facing == "north")
    *dz = -1;
  else if (facing == "south")
    *dz = 1;
  else if (facing == "west")
    *dx = -1;
  else if (facing == "east")
    *dx = 1;
}

bool block_entity_at_pos(const nbt::Value& be, int x, int y, int z) {
  if (be.type != nbt::Tag::Compound)
    return false;
  auto read = [&](const char* name, int fallback) -> int {
    const nbt::Value* v = nbt::child(be, name);
    if (v == nullptr)
      return fallback;
    auto iv = nbt::as_int(*v);
    return iv ? static_cast<int>(*iv) : fallback;
  };
  return read("x", 0) == x && read("y", 0) == y && read("z", 0) == z;
}

namespace {
// The chunk-local coordinate a legacy (pre-absolute-coordinate) record used for
// an absolute world coordinate. Plain `%` keeps the sign of a negative value,
// so normalize into 0..15 (matching floor division by 16).
int legacy_local_coord(int v) {
  const int m = v % 16;
  return m < 0 ? m + 16 : m;
}
} // namespace

bool block_entity_matches(const nbt::Value& be, int x, int y, int z) {
  if (be.type != nbt::Tag::Compound)
    return false;
  auto read = [&](const char* name) -> int {
    const nbt::Value* v = nbt::child(be, name);
    if (v == nullptr)
      return 0;
    auto iv = nbt::as_int(*v);
    return iv ? static_cast<int>(*iv) : 0;
  };
  if (read("y") != y)
    return false;
  const int rx = read("x");
  const int rz = read("z");
  // Vanilla (and this server now): absolute world coordinates.
  if (rx == x && rz == z)
    return true;
  // Legacy: x/z relative to the owning chunk (a value in 0..15). Inside one
  // chunk the two forms cannot collide: a nonzero chunk's absolute x/z are
  // outside 0..15, and chunk 0's absolute x/z equal the local ones.
  return rx == legacy_local_coord(x) && rz == legacy_local_coord(z);
}

namespace {

// ---- the `components` patch -------------------------------------------------
//
// `ContainerHelper.saveAllItems` writes `ItemStackWithSlot`, whose `components`
// field is the same `ItemStack.MAP_CODEC` patch the wire carries — in the
// *NBT* shapes of the component codecs. The four this engine models are
// written, each by resource location where vanilla names a registry entry, so a
// record reads back onto the same registry ids (and a record vanilla wrote
// reads back here). A component the engine does not model is dropped, which is
// the documented limit of the container codec (docs/PHASE3.md).

// The Details half of `MobEffectInstance.CODEC`, appended to the instance's own
// compound: the codec groups `MobEffect.CODEC.fieldOf("id")` with
// `Details.MAP_CODEC`, so `amplifier` (an unsigned byte), `duration` (an int),
// `ambient`, `show_particles`, `show_icon` and an optional nested
// `hidden_effect` sit beside `id`.
bool append_effect_details(const MobEffectInstance& instance, int depth,
                           std::vector<std::pair<std::string, nbt::Value>>* fields) {
  // `Details.MAP_CODEC`'s amplifier is `ExtraCodecs.UNSIGNED_BYTE`, so a value
  // it cannot carry is a component this codec cannot represent — refused rather
  // than truncated into a different amplifier.
  if (instance.amplifier < 0 || instance.amplifier > 255)
    return false;
  fields->emplace_back("amplifier", nbt::make_byte(static_cast<int8_t>(instance.amplifier)));
  fields->emplace_back("duration", nbt::make_int(instance.duration));
  fields->emplace_back("ambient", nbt::make_byte(instance.ambient ? 1 : 0));
  fields->emplace_back("show_particles", nbt::make_byte(instance.visible ? 1 : 0));
  fields->emplace_back("show_icon", nbt::make_byte(instance.show_icon ? 1 : 0));
  if (instance.hidden.empty())
    return true;
  // The same depth cap the wire reader enforces: a chain this server can only
  // have built one layer deep is not a state worth representing at any depth.
  if (depth + 1 >= kMaxHiddenEffectLayers)
    return false;
  std::vector<std::pair<std::string, nbt::Value>> hidden;
  if (!append_effect_details(instance.hidden.front(), depth + 1, &hidden))
    return false;
  fields->emplace_back("hidden_effect", nbt::make_compound(std::move(hidden)));
  return true;
}

// One `MobEffectInstance.CODEC` value, or an empty Value (Tag::End) when the
// effect is one this table cannot name — a hidden effect's id is the instance's
// own, so a nameless effect makes the whole instance unwritable.
nbt::Value effect_instance_to_nbt(const MobEffectInstance& instance) {
  const GeneratedMobEffect* def = mob_effect_by_id(instance.effect);
  if (def == nullptr)
    return nbt::Value{};
  std::vector<std::pair<std::string, nbt::Value>> fields;
  fields.emplace_back("id", nbt::make_string(def->name));
  if (!append_effect_details(instance, 0, &fields))
    return nbt::Value{};
  return nbt::make_compound(std::move(fields));
}

// One `ItemStackWithSlot` entry: Slot, id, count, and the component patch the
// stack actually carries (its damage, its enchantments, its potion contents and
// its trim).
nbt::Value item_to_nbt(const ItemStack& stack, int slot) {
  using namespace nbt;
  std::vector<std::pair<std::string, Value>> fields;
  fields.emplace_back("Slot", make_byte(static_cast<int8_t>(slot)));
  fields.emplace_back("id", make_string(std::string(ItemRegistry::instance().name_of(stack.item))));
  fields.emplace_back("count", make_int(stack.count));
  std::vector<std::pair<std::string, Value>> comps;
  if (stack.damage > 0)
    comps.emplace_back("minecraft:damage", make_int(stack.damage));
  // `ItemEnchantments.CODEC` is an unbounded map of `Enchantment.CODEC`
  // (a resource location) to a level in 1..255 — the component *is* the map,
  // there is no wrapper field around it.
  if (stack.has_enchantments()) {
    std::vector<std::pair<std::string, Value>> levels;
    levels.reserve(stack.enchantments.size());
    for (const ItemEnchantment& entry : stack.enchantments) {
      const GeneratedEnchantment* def = enchantment_by_id(entry.enchantment);
      if (def == nullptr)
        continue; // an id the table cannot name is left out of the map
      levels.emplace_back(def->name, make_int(entry.level));
    }
    if (!levels.empty())
      comps.emplace_back("minecraft:enchantments", make_compound(std::move(levels)));
  }
  // `PotionContents.CODEC`'s compound: `potion`, `custom_color`,
  // `custom_effects` and `custom_name`, all optional. The display-only colour
  // and name are kept on `StackPotionContents` so a container round-trip does
  // not lose the bottle's appearance.
  if (stack.potion_contents) {
    const StackPotionContents& contents = *stack.potion_contents;
    const GeneratedPotion* potion = contents.potion >= 0 ? potion_by_id(contents.potion) : nullptr;
    std::vector<Value> effects;
    effects.reserve(contents.custom_effects.size());
    bool representable = contents.potion < 0 || potion != nullptr;
    for (const MobEffectInstance& instance : contents.custom_effects) {
      Value value = effect_instance_to_nbt(instance);
      if (value.type != Tag::Compound) {
        representable = false;
        break;
      }
      effects.push_back(std::move(value));
    }
    if (representable) {
      std::vector<std::pair<std::string, Value>> potion_fields;
      if (potion != nullptr)
        potion_fields.emplace_back("potion", make_string(potion->name));
      if (contents.custom_color)
        potion_fields.emplace_back("custom_color", make_int(*contents.custom_color));
      if (!effects.empty())
        potion_fields.emplace_back("custom_effects", make_list(Tag::Compound, std::move(effects)));
      if (contents.custom_name)
        potion_fields.emplace_back("custom_name", make_string(*contents.custom_name));
      comps.emplace_back("minecraft:potion_contents", make_compound(std::move(potion_fields)));
    }
  }
  // `ArmorTrim.CODEC`: {material, pattern} as two resource locations (the parts
  // are Holders, and a RegistryFileCodec writes an id by name). Dropping this
  // is what made a trimmed piece go plain on its way through a chest.
  if (stack.trim && stack.trim->complete()) {
    const char* material = trim_material_name(stack.trim->material);
    const char* pattern = trim_pattern_name(stack.trim->pattern);
    if (material != nullptr && pattern != nullptr)
      comps.emplace_back("minecraft:trim", make_compound({{"material", make_string(material)},
                                                          {"pattern", make_string(pattern)}}));
  }
  if (!comps.empty())
    fields.emplace_back("components", make_compound(std::move(comps)));
  return make_compound(std::move(fields));
}

// ---- reading the patch back -------------------------------------------------

// `ItemEnchantments.CODEC`: the component *is* the map of names to levels. A
// name or a level this engine cannot hold makes the whole component
// unrepresentable, so it is dropped rather than read in part.
bool enchantments_from_nbt(const nbt::Value& value, std::vector<ItemEnchantment>* out) {
  if (value.type != nbt::Tag::Compound)
    return false;
  for (const auto& field : value.fields) {
    const GeneratedEnchantment* def = enchantment_by_name(field.first);
    if (def == nullptr)
      return false;
    const std::optional<int64_t> level = nbt::as_int(field.second);
    if (!level || *level < 1 || *level > 255)
      return false; // LEVEL_CODEC is intRange(1, 255)
    if (out->size() >= kMaxEnchantmentsPerStack)
      return false;
    out->push_back(ItemEnchantment{enchantment_id_of(*def), static_cast<int32_t>(*level)});
  }
  // An ItemStack keeps its enchantments sorted by id (world/item.h), which is
  // the order the wire form carries too, so a record and a packet agree.
  std::sort(out->begin(), out->end(), [](const ItemEnchantment& a, const ItemEnchantment& b) {
    return a.enchantment < b.enchantment;
  });
  return true;
}

// `MobEffectInstance.Details.MAP_CODEC` read back. `out->effect` is already
// set: a hidden layer carries no id of its own, because a hidden effect is
// always the same effect as the instance hiding it.
bool effect_details_from_nbt(const nbt::Value& value, int depth, MobEffectInstance* out) {
  if (value.type != nbt::Tag::Compound)
    return false;
  if (const nbt::Value* amplifier = nbt::child(value, "amplifier")) {
    const std::optional<int64_t> v = nbt::as_int(*amplifier);
    if (!v || *v < 0 || *v > 255)
      return false;
    out->amplifier = static_cast<int32_t>(*v);
  }
  if (const nbt::Value* duration = nbt::child(value, "duration")) {
    const std::optional<int64_t> v = nbt::as_int(*duration);
    if (!v)
      return false;
    out->duration = static_cast<int32_t>(*v);
  }
  if (const nbt::Value* ambient = nbt::child(value, "ambient")) {
    const std::optional<int64_t> v = nbt::as_int(*ambient);
    if (!v)
      return false;
    out->ambient = *v != 0;
  }
  if (const nbt::Value* particles = nbt::child(value, "show_particles")) {
    const std::optional<int64_t> v = nbt::as_int(*particles);
    if (!v)
      return false;
    out->visible = *v != 0;
  }
  if (const nbt::Value* icon = nbt::child(value, "show_icon")) {
    const std::optional<int64_t> v = nbt::as_int(*icon);
    if (!v)
      return false;
    out->show_icon = *v != 0;
  } else {
    // `show_icon` is optional and falls back to `show_particles`
    // (Details.create), not to true.
    out->show_icon = out->visible;
  }
  const nbt::Value* hidden = nbt::child(value, "hidden_effect");
  if (hidden == nullptr)
    return true;
  if (depth + 1 >= kMaxHiddenEffectLayers)
    return false; // the same cap the wire reader enforces
  MobEffectInstance nested;
  nested.effect = out->effect;
  if (!effect_details_from_nbt(*hidden, depth + 1, &nested))
    return false;
  out->hidden.push_back(std::move(nested));
  return true;
}

bool effect_instance_from_nbt(const nbt::Value& value, MobEffectInstance* out) {
  if (value.type != nbt::Tag::Compound)
    return false;
  const nbt::Value* id = nbt::child(value, "id");
  if (id == nullptr)
    return false;
  const std::optional<std::string_view> name = nbt::as_string(*id);
  if (!name)
    return false;
  const GeneratedMobEffect* def = mob_effect_by_name(*name);
  if (def == nullptr)
    return false;
  out->effect = mob_effect_id_of(*def);
  return effect_details_from_nbt(value, 0, out);
}

// `PotionContents.CODEC`: the compound, or the codec's alternative form — a
// bare potion id. The optional display colour and name round-trip too, so a
// renamed or tinted bottle survives a container save/load.
bool potion_contents_from_nbt(const nbt::Value& value, StackPotionContents* out) {
  StackPotionContents contents;
  if (const std::optional<std::string_view> name = nbt::as_string(value)) {
    const GeneratedPotion* def = potion_by_name(*name);
    if (def == nullptr)
      return false;
    contents.potion = potion_id_of(*def);
    *out = std::move(contents);
    return true;
  }
  if (value.type != nbt::Tag::Compound)
    return false;
  if (const nbt::Value* potion = nbt::child(value, "potion")) {
    const std::optional<std::string_view> name = nbt::as_string(*potion);
    if (!name)
      return false;
    const GeneratedPotion* def = potion_by_name(*name);
    if (def == nullptr)
      return false;
    contents.potion = potion_id_of(*def);
  }
  if (const nbt::Value* color = nbt::child(value, "custom_color")) {
    if (const std::optional<int64_t> v = nbt::as_int(*color))
      contents.custom_color = static_cast<int32_t>(*v);
  }
  if (const nbt::Value* effects = nbt::child(value, "custom_effects")) {
    if (effects->type != nbt::Tag::List)
      return false;
    for (const nbt::Value& item : effects->items) {
      if (contents.custom_effects.size() >= kMaxPotionCustomEffects)
        return false;
      MobEffectInstance instance;
      if (!effect_instance_from_nbt(item, &instance))
        return false;
      contents.custom_effects.push_back(std::move(instance));
    }
  }
  if (const nbt::Value* name = nbt::child(value, "custom_name")) {
    if (const std::optional<std::string_view> v = nbt::as_string(*name))
      contents.custom_name = std::string(*v);
  }
  *out = std::move(contents);
  return true;
}

// The `components` patch of one `ItemStackWithSlot` entry. Every component this
// engine models is read on its own; one it cannot represent is dropped, never
// half-read into the stack.
void apply_components(const nbt::Value& comps, ItemStack* stack) {
  if (comps.type != nbt::Tag::Compound)
    return;
  if (const nbt::Value* dmg = nbt::child(comps, "minecraft:damage"); dmg != nullptr) {
    if (const std::optional<int64_t> v = nbt::as_int(*dmg))
      stack->damage = static_cast<int32_t>(*v);
  }
  if (const nbt::Value* enchantments = nbt::child(comps, "minecraft:enchantments");
      enchantments != nullptr) {
    std::vector<ItemEnchantment> read;
    if (enchantments_from_nbt(*enchantments, &read))
      stack->enchantments = std::move(read);
  }
  if (const nbt::Value* contents = nbt::child(comps, "minecraft:potion_contents");
      contents != nullptr) {
    StackPotionContents read;
    if (potion_contents_from_nbt(*contents, &read))
      stack->potion_contents = std::move(read);
  }
  // A trim reads back by name so a record written by vanilla lands on the same
  // registry ids: an unknown name (a pattern this table does not carry) leaves
  // the stack untrimmed rather than half-trimmed.
  const nbt::Value* trim = nbt::child(comps, "minecraft:trim");
  const nbt::Value* material = trim != nullptr ? nbt::child(*trim, "material") : nullptr;
  const nbt::Value* pattern = trim != nullptr ? nbt::child(*trim, "pattern") : nullptr;
  if (trim != nullptr && trim->type == nbt::Tag::Compound && material != nullptr &&
      pattern != nullptr) {
    const std::optional<std::string_view> material_name = nbt::as_string(*material);
    const std::optional<std::string_view> pattern_name = nbt::as_string(*pattern);
    const int32_t material_id = material_name ? trim_material_for_name(*material_name) : -1;
    const int32_t pattern_id = pattern_name ? trim_pattern_for_name(*pattern_name) : -1;
    if (material_id >= 0 && pattern_id >= 0)
      stack->trim = StackArmorTrim{material_id, pattern_id};
  }
}

} // namespace

std::vector<ItemStack> read_block_entity_items(const nbt::Value& be, int size) {
  std::vector<ItemStack> out(static_cast<size_t>(size > 0 ? size : 0));
  const nbt::Value* items = nbt::child(be, "Items");
  if (items == nullptr || items->type != nbt::Tag::List)
    return out;
  const auto& registry = ItemRegistry::instance();
  for (const nbt::Value& entry : items->items) {
    if (entry.type != nbt::Tag::Compound)
      continue;
    const nbt::Value* slotv = nbt::child(entry, "Slot");
    const nbt::Value* idv = nbt::child(entry, "id");
    if (slotv == nullptr || idv == nullptr)
      continue;
    auto slot = nbt::as_int(*slotv);
    auto id_name = nbt::as_string(*idv);
    if (!slot || !id_name || *slot < 0 || *slot >= size)
      continue;
    auto item_id = registry.id_of(*id_name);
    if (!item_id)
      continue;
    ItemStack stack = ItemStack::of_id(*item_id, 1);
    if (const nbt::Value* cv = nbt::child(entry, "count"); cv != nullptr) {
      if (auto c = nbt::as_int(*cv); c && *c > 0)
        stack.count = static_cast<int32_t>(*c);
    }
    if (const nbt::Value* comps = nbt::child(entry, "components"))
      apply_components(*comps, &stack);
    out[static_cast<size_t>(*slot)] = stack;
  }
  return out;
}

void write_block_entity_items(nbt::Value* be, const std::vector<ItemStack>& items) {
  if (be == nullptr || be->type != nbt::Tag::Compound)
    return;
  std::vector<nbt::Value> list;
  for (size_t i = 0; i < items.size(); ++i) {
    if (!items[i].empty())
      list.push_back(item_to_nbt(items[i], static_cast<int>(i)));
  }
  nbt::Value encoded = nbt::make_list(nbt::Tag::Compound, std::move(list));
  for (auto& [name, value] : be->fields) {
    if (name == "Items") {
      value = std::move(encoded);
      return;
    }
  }
  be->fields.emplace_back("Items", std::move(encoded));
}

nbt::Value make_container_block_entity(std::string_view id, const BlockPosKey& pos,
                                       const std::vector<ItemStack>& items) {
  using namespace nbt;
  std::vector<std::pair<std::string, Value>> fields;
  fields.emplace_back("id", make_string(std::string(id)));
  fields.emplace_back("x", make_int(pos.x));
  fields.emplace_back("y", make_int(pos.y));
  fields.emplace_back("z", make_int(pos.z));
  Value be = make_compound(std::move(fields));
  write_block_entity_items(&be, items);
  return be;
}

namespace {

// Sets (or adds) one field, keeping the record's order stable for everything
// else it carries.
void put_or_replace(nbt::Value* be, const char* name, nbt::Value value) {
  for (auto& field : be->fields) {
    if (field.first == name) {
      field.second = std::move(value);
      return;
    }
  }
  be->fields.emplace_back(name, std::move(value));
}

} // namespace

void read_block_entity_furnace(const nbt::Value& be, FurnaceState* state) {
  if (state == nullptr || be.type != nbt::Tag::Compound)
    return;
  auto read_short = [&](const char* name, int fallback) -> int {
    const nbt::Value* v = nbt::child(be, name);
    if (v == nullptr)
      return fallback;
    auto iv = nbt::as_int(*v);
    if (!iv)
      return fallback;
    // Vanilla stores these as Shorts, so a value out of that range cannot have
    // come from a furnace; clamp rather than carry a bogus denominator.
    return static_cast<int>(std::max<int64_t>(-32768, std::min<int64_t>(32767, *iv)));
  };
  state->cooking_timer = read_short("cooking_time_spent", 0);
  state->cooking_total_time = read_short("cooking_total_time", 0);
  state->lit_time_remaining = read_short("lit_time_remaining", 0);
  state->lit_total_time = read_short("lit_total_time", 0);
  state->recipes_used.clear();
  const nbt::Value* used = nbt::child(be, "RecipesUsed");
  if (used != nullptr && used->type == nbt::Tag::Compound) {
    for (const auto& field : used->fields) {
      auto crafts = nbt::as_int(field.second);
      if (!crafts || *crafts <= 0)
        continue;
      // Vanilla keys the map by recipe resource *path* (the compound's key is
      // the id's path, e.g. "oak_planks"), which is exactly the table's name.
      state->recipes_used[field.first] = static_cast<int>(*crafts);
    }
  }
  // The tick re-derives the total cook time from this; seeding it from the
  // persisted input keeps a reloaded furnace from recomputing on the next tick.
  state->last_input_item = kAirItemId;
}

void write_block_entity_furnace(nbt::Value* be, const FurnaceState& state) {
  if (be == nullptr || be->type != nbt::Tag::Compound)
    return;
  using namespace nbt;
  put_or_replace(be, "cooking_time_spent", make_short(static_cast<int16_t>(state.cooking_timer)));
  put_or_replace(be, "cooking_total_time",
                 make_short(static_cast<int16_t>(state.cooking_total_time)));
  put_or_replace(be, "lit_time_remaining",
                 make_short(static_cast<int16_t>(state.lit_time_remaining)));
  put_or_replace(be, "lit_total_time", make_short(static_cast<int16_t>(state.lit_total_time)));
  std::vector<std::pair<std::string, Value>> used;
  for (const auto& entry : state.recipes_used)
    if (entry.second > 0)
      used.emplace_back(entry.first, make_int(entry.second));
  put_or_replace(be, "RecipesUsed", make_compound(std::move(used)));
}

void read_block_entity_brewing(const nbt::Value& be, BrewingState* state) {
  if (state == nullptr || be.type != nbt::Tag::Compound)
    return;
  auto read_number = [&](const char* name, int fallback) -> int {
    const nbt::Value* v = nbt::child(be, name);
    if (v == nullptr)
      return fallback;
    auto iv = nbt::as_int(*v);
    return iv ? static_cast<int>(*iv) : fallback;
  };
  // `BrewTime` is a Short and `Fuel` a Byte, so an out-of-range value is not
  // something this block entity could have written: clamp rather than carry a
  // countdown that outlasts any real brew. The timer is also *re-derived* from
  // the ingredient slot, exactly like loadAdditional: a running brew belongs to
  // the item sitting in slot 3, and an empty slot cancels it on the next tick.
  state->brew_time =
      static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(32767, read_number("BrewTime", 0))));
  state->fuel =
      static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(127, read_number("Fuel", 0))));
  state->ingredient_item = kAirItemId;
  state->bottles_known = false;
  state->bottles[0] = state->bottles[1] = state->bottles[2] = false;
}

void write_block_entity_brewing(nbt::Value* be, const BrewingState& state) {
  if (be == nullptr || be->type != nbt::Tag::Compound)
    return;
  using namespace nbt;
  put_or_replace(be, "BrewTime", make_short(static_cast<int16_t>(state.brew_time)));
  put_or_replace(be, "Fuel", make_byte(static_cast<int8_t>(state.fuel)));
}

} // namespace mc::world
