// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/item.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <utility>
#include <vector>

namespace mc::world {

namespace {

// A minecraft identifier: [a-z0-9_.-]+ with an optional namespace, both
// non-empty. Deliberately strict — the names come from /give and the wire.
bool is_valid_identifier(std::string_view s) {
  if (s.empty())
    return false;
  bool colon = false;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == ':') {
      if (colon || i == 0 || i + 1 == s.size())
        return false;
      colon = true;
      continue;
    }
    const bool ok =
        (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    if (!ok)
      return false;
  }
  return true;
}

} // namespace

std::string normalize_item_name(std::string_view raw) {
  std::string s;
  s.reserve(raw.size() + 10);
  for (char c : raw) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u == ' ')
      continue; // players type "minecraft:stone " all the time
    s.push_back(static_cast<char>(std::tolower(u)));
  }
  if (s.empty())
    return s;
  if (s.find(':') == std::string::npos)
    s.insert(0, "minecraft:");
  if (!is_valid_identifier(s))
    return {};
  return s;
}

ItemRegistry::ItemRegistry() {
  by_name_.reserve(static_cast<size_t>(kItemCount) * 2);
  for (int i = 0; i < kItemCount; ++i)
    by_name_.emplace(kItems[i].name, kItems[i].id);
}

const ItemRegistry& ItemRegistry::instance() {
  static const ItemRegistry registry;
  return registry;
}

std::optional<int32_t> ItemRegistry::id_of(std::string_view name) const {
  const std::string normalized = normalize_item_name(name);
  if (normalized.empty())
    return std::nullopt;
  const auto it = by_name_.find(std::string_view(normalized));
  if (it == by_name_.end())
    return std::nullopt;
  return it->second;
}

const GeneratedItem* ItemRegistry::by_id(int32_t id) const {
  if (id < 0 || id >= kItemCount)
    return nullptr;
  return &kItems[id];
}

std::string_view ItemRegistry::name_of(int32_t id) const {
  const GeneratedItem* item = by_id(id);
  return item ? std::string_view(item->name) : std::string_view();
}

int ItemRegistry::max_stack_size(int32_t id) const {
  const GeneratedItem* item = by_id(id);
  // An unknown item still has to behave: treat it as unstackable rather than
  // silently handing out a 64-stack of nothing.
  return item ? static_cast<int>(item->max_stack_size) : 1;
}

bool ItemRegistry::is_block_item(int32_t id) const {
  const GeneratedItem* item = by_id(id);
  return item && (item->flags & kItemIsBlock) != 0;
}

bool ItemRegistry::is_damageable(int32_t id) const {
  const GeneratedItem* item = by_id(id);
  return item && (item->flags & kItemIsDamageable) != 0;
}

int32_t crafting_remainder_item(int32_t item) {
  const ItemRegistry& registry = ItemRegistry::instance();
  const std::string_view name = registry.name_of(item);
  // The five items `Items.java` sets `craftRemainder` on, by name. `name_of` is
  // an array index, so this is cheaper than four id lookups per call.
  if (name == "minecraft:water_bucket" || name == "minecraft:lava_bucket" ||
      name == "minecraft:milk_bucket")
    return registry.id_of("minecraft:bucket").value_or(kAirItemId);
  if (name == "minecraft:dragon_breath" || name == "minecraft:honey_bottle")
    return registry.id_of("minecraft:glass_bottle").value_or(kAirItemId);
  return kAirItemId;
}

bool ItemStack::has_tool() const {
  const GeneratedItem* def = ItemRegistry::instance().by_id(item);
  return count > 0 && def && (def->flags & kItemHasTool) != 0;
}

bool ItemStack::is_sword() const {
  const GeneratedItem* def = ItemRegistry::instance().by_id(item);
  return count > 0 && def && (def->flags & kItemIsSword) != 0;
}

bool ItemStack::is_mace() const {
  // By name rather than by a generated flag: the mace is a Java *class* rather
  // than a tag, and the item table has no bit for it. Being the only member of
  // #minecraft:enchantable/mace in 26.2 is a coincidence of the data, not the
  // definition — MaceItem.getAttackDamageBonus is what makes the item special.
  const GeneratedItem* def = ItemRegistry::instance().by_id(item);
  return count > 0 && def != nullptr && std::string_view(def->name) == "minecraft:mace";
}

// The `minecraft:weapon` component's stand-in: the item's own attack-damage
// modifier. `ToolMaterial.applyToolProperties` installs both the component and
// the attributes in one call, so an item that grants attack damage is an item
// with the component, and the table's default of 0.0 leaves a non-weapon at
// zero. (`DataComponents.WEAPON` also carries the per-hit durability cost, which
// is what `postHurtEnemy` spends; that half is not asked for here.)
bool ItemStack::is_weapon() const {
  const GeneratedItem* def = ItemRegistry::instance().by_id(item);
  return count > 0 && def != nullptr && def->attack_damage > 0.0f;
}

float ItemStack::attack_damage() const {
  // Attributes.ATTACK_DAMAGE's default base value is 2.0 for a *mob*; a player's
  // is overridden to 1.0 in Player.createAttributes. Player.attack reads the
  // player's, so 1.0 is the base an item modifier adds to.
  const float base = 1.0f;
  const GeneratedItem* def = ItemRegistry::instance().by_id(item);
  if (empty() || def == nullptr)
    return base;
  return base + def->attack_damage;
}

float ItemStack::attack_speed() const {
  const float base = 4.0f; // Attributes.DEFAULT_ATTACK_SPEED
  const GeneratedItem* def = ItemRegistry::instance().by_id(item);
  if (empty() || def == nullptr)
    return base;
  return base + def->attack_speed;
}

int ItemStack::max_damage() const {
  const GeneratedItem* def = ItemRegistry::instance().by_id(item);
  return def ? static_cast<int>(def->max_damage) : 0;
}

bool ItemStack::damage_by(int amount) {
  if (!is_damageable() || amount <= 0)
    return false;
  damage += amount;
  if (damage < max_damage())
    return false;
  // Broken: vanilla destroys the stack outright rather than leaving it at 0
  // durability. The caller decides whether it also syncs or plays a sound.
  clear();
  return true;
}

int ItemStack::remaining_durability() const {
  if (!is_damageable())
    return -1;
  const int left = max_damage() - damage;
  return left > 0 ? left : 0;
}

bool ItemStack::max_damaged() const {
  const int left = remaining_durability();
  return left == 0;
}

int ItemStack::enchantment_level(int32_t enchantment) const {
  for (const ItemEnchantment& entry : enchantments)
    if (entry.enchantment == enchantment)
      return entry.level;
  return 0;
}

void ItemStack::set_enchantment(int32_t enchantment, int32_t level) {
  for (size_t i = 0; i < enchantments.size(); ++i) {
    if (enchantments[i].enchantment != enchantment)
      continue;
    if (level <= 0) {
      enchantments.erase(enchantments.begin() + static_cast<long>(i));
      return;
    }
    enchantments[i].level = level;
    return;
  }
  if (level <= 0)
    return;
  ItemEnchantment entry;
  entry.enchantment = enchantment;
  entry.level = level;
  // Sorted by id, which is the order the wire form and `same_item` both assume.
  enchantments.push_back(entry);
  std::sort(enchantments.begin(), enchantments.end(),
            [](const ItemEnchantment& a, const ItemEnchantment& b) {
              return a.enchantment < b.enchantment;
            });
}

int ItemStack::grow(int n) {
  if (n <= 0 || empty())
    return 0;
  const int room = max_stack_size() - count;
  const int taken = n < room ? n : room;
  count += taken;
  return taken;
}

int ItemStack::shrink(int n) {
  if (n <= 0)
    return 0;
  const int removed = n < count ? n : count;
  count -= removed;
  if (count == 0)
    clear();
  return removed;
}

ItemStack ItemStack::of_id(int32_t item, int32_t count) {
  ItemStack stack;
  if (item == kAirItemId || count <= 0)
    return stack;
  stack.item = item;
  stack.count = count;
  return stack;
}

ItemStack ItemStack::of(std::string_view name, int32_t count) {
  const auto id = ItemRegistry::instance().id_of(name);
  if (!id)
    return ItemStack{};
  return of_id(*id, count);
}

namespace {

// The number of components write_item_stack emits for a stack. Kept next to the
// writes themselves so a component added in one place cannot be forgotten in
// the other: the patch's leading count is the only length the client trusts.
int added_component_count(const ItemStack& stack) {
  int count = 0;
  if (stack.damage > 0)
    ++count;
  if (stack.has_enchantments())
    ++count;
  if (stack.potion_contents)
    ++count;
  if (stack.trim && stack.trim->complete())
    ++count;
  count += static_cast<int>(stack.raw_components.size());
  return count;
}

// ArmorTrim.STREAM_CODEC, whose two fields are `TrimMaterial.STREAM_CODEC` and
// `TrimPattern.STREAM_CODEC`. Both are `ByteBufCodecs.holder`: `Holder` writes
// `id + 1` as a varint, reserving 0 for a *direct* holder (the value inline
// instead of a registry reference). This server only ever writes references, so
// a trim of registry ids [material][pattern] goes out as [material + 1]
// [pattern + 1].
void write_armor_trim(mc::net::Writer& w, const StackArmorTrim& trim) {
  w.varint(static_cast<uint32_t>(trim.material + 1));
  w.varint(static_cast<uint32_t>(trim.pattern + 1));
}

// The same codec from one delimited component value. A 0 holder id is a direct
// holder: its payload is a `TrimMaterial`/`TrimPattern` *definition* (asset
// names and a text component), which this engine does not model, so such a
// component is dropped rather than half-read — the value's bytes are already
// consumed as a whole, so nothing downstream desyncs.
bool read_armor_trim(const std::vector<uint8_t>& value, StackArmorTrim* out) {
  mc::net::Reader r(value);
  uint32_t material = 0, pattern = 0;
  if (!r.varint(&material) || !r.varint(&pattern))
    return false;
  if (material == 0 || pattern == 0)
    return false;
  out->material = static_cast<int32_t>(material) - 1;
  out->pattern = static_cast<int32_t>(pattern) - 1;
  return true;
}

// MobEffectInstance.STREAM_CODEC: `MobEffect.STREAM_CODEC` (the effect's own
// registry id, a plain varint) and then `Details`.
void write_effect_instance(mc::net::Writer& w, const MobEffectInstance& instance);

// MobEffectInstance.Details.STREAM_CODEC: amplifier, duration, ambient,
// showParticles, showIcon, then an optional nested Details for the hidden
// effect. Two details of the order are easy to get wrong: the amplifier comes
// *before* the duration, and the instance's `visible` is Details' showParticles
// (the wire name is `show_particles`, the field this server calls visible). The
// nested layer carries no effect id at all, because a hidden effect is always
// the same effect as the instance hiding it.
//
// `showIcon` is written explicitly rather than left to the codec's fallback, so
// what a client reads back is what was written.
void write_effect_details(mc::net::Writer& w, const MobEffectInstance& instance) {
  w.varint(static_cast<uint32_t>(instance.amplifier));
  w.varint(static_cast<uint32_t>(instance.duration));
  w.u8(instance.ambient ? 1 : 0);
  w.u8(instance.visible ? 1 : 0);
  w.u8(instance.show_icon ? 1 : 0);
  const bool has_hidden = !instance.hidden.empty();
  w.u8(has_hidden ? 1 : 0);
  if (has_hidden)
    write_effect_details(w, instance.hidden.front());
}

void write_effect_instance(mc::net::Writer& w, const MobEffectInstance& instance) {
  w.varint(static_cast<uint32_t>(instance.effect));
  write_effect_details(w, instance);
}

// Details.STREAM_CODEC, read side. `depth` counts the hidden layers already
// descended, because the codec is recursive and a client's stack decides how
// deep it goes: vanilla would recurse as far as the packet nests, which is a
// stack overflow waiting to happen, so this refuses past
// kMaxHiddenEffectLayers rather than following it.
bool read_effect_details(mc::net::Reader& r, int depth, MobEffectInstance* out) {
  uint32_t amplifier = 0, duration = 0;
  uint8_t ambient = 0, visible = 0, show_icon = 0, has_hidden = 0;
  if (!r.varint(&amplifier) || !r.varint(&duration) || !r.u8(&ambient) || !r.u8(&visible) ||
      !r.u8(&show_icon) || !r.u8(&has_hidden))
    return false;
  out->amplifier = static_cast<int32_t>(amplifier);
  out->duration = static_cast<int32_t>(duration);
  out->ambient = ambient != 0;
  out->visible = visible != 0;
  out->show_icon = show_icon != 0;
  if (has_hidden == 0)
    return true;
  if (depth + 1 >= kMaxHiddenEffectLayers)
    return false;
  MobEffectInstance hidden;
  // The nested layer carries no effect of its own: a hidden effect is always
  // the same effect as the instance hiding it.
  hidden.effect = out->effect;
  if (!read_effect_details(r, depth + 1, &hidden))
    return false;
  out->hidden.push_back(std::move(hidden));
  return true;
}

bool read_effect_instance(mc::net::Reader& r, MobEffectInstance* out) {
  uint32_t effect = 0;
  if (!r.varint(&effect))
    return false;
  out->effect = static_cast<int32_t>(effect);
  return read_effect_details(r, 0, out);
}

// PotionContents.STREAM_CODEC from one delimited component value. The custom
// colour and the custom name are parsed rather than skipped, because the effects
// list sits between them: a value that stopped early would take the next
// component's bytes with it.
bool read_potion_contents(const std::vector<uint8_t>& value, StackPotionContents* out) {
  mc::net::Reader r(value);
  uint8_t has_potion = 0;
  if (!r.u8(&has_potion))
    return false;
  StackPotionContents contents;
  if (has_potion != 0) {
    uint32_t potion = 0;
    if (!r.varint(&potion))
      return false;
    contents.potion = static_cast<int32_t>(potion);
  }
  uint8_t has_color = 0;
  if (!r.u8(&has_color))
    return false;
  if (has_color != 0) {
    uint32_t color = 0;
    if (!r.u32(&color))
      return false;
    contents.custom_color = static_cast<int32_t>(color);
  }
  uint32_t count = 0;
  if (!r.varint(&count))
    return false;
  // The count is whatever the client claims; the slice bounds it, but not
  // before the vector would have reserved that much.
  if (count > kMaxPotionCustomEffects)
    return false;
  contents.custom_effects.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    MobEffectInstance instance;
    if (!read_effect_instance(r, &instance))
      return false;
    contents.custom_effects.push_back(std::move(instance));
  }
  uint8_t has_name = 0;
  if (!r.u8(&has_name))
    return false;
  if (has_name != 0) {
    std::string name;
    if (!r.string(&name))
      return false;
    contents.custom_name = std::move(name);
  }
  *out = std::move(contents);
  return true;
}

} // namespace

// PotionContents.STREAM_CODEC, which is the four fields in this order:
// `Potion.STREAM_CODEC.apply(optional)` (a presence flag, then the potion's
// registry id), `ByteBufCodecs.INT.apply(optional)` (custom_color),
// `MobEffectInstance.STREAM_CODEC.apply(list)` (custom_effects) and
// `ByteBufCodecs.STRING_UTF8.apply(optional)` (custom_name).
void write_potion_contents(mc::net::Writer& w, const StackPotionContents& contents) {
  const bool has_potion = contents.potion >= 0;
  w.u8(has_potion ? 1 : 0);
  if (has_potion)
    w.varint(static_cast<uint32_t>(contents.potion));
  w.u8(contents.custom_color ? 1 : 0);
  if (contents.custom_color)
    w.u32(static_cast<uint32_t>(*contents.custom_color));
  w.varint(static_cast<uint32_t>(contents.custom_effects.size()));
  for (const MobEffectInstance& instance : contents.custom_effects)
    write_effect_instance(w, instance);
  w.u8(contents.custom_name ? 1 : 0);
  if (contents.custom_name)
    w.string(*contents.custom_name);
}

void write_item_stack(mc::net::Writer& w, const ItemStack& stack) {
  if (stack.empty()) {
    w.varint(0); // count 0 == empty, nothing else
    return;
  }
  w.varint(static_cast<uint32_t>(stack.count));
  w.varint(static_cast<uint32_t>(stack.item));
  // DataComponentPatch: the added entries, none removed.
  w.varint(static_cast<uint32_t>(added_component_count(stack)));
  w.varint(0); // removed
  if (stack.damage > 0) {
    w.varint(static_cast<uint32_t>(kComponentDamageId));
    w.varint(static_cast<uint32_t>(stack.damage)); // DataComponents.DAMAGE: VAR_INT
  }
  if (stack.has_enchantments()) {
    // DataComponents.ENCHANTMENTS is an ItemEnchantments, whose STREAM_CODEC is
    // ByteBufCodecs.map(ENCHANTMENT holder, VAR_INT): a varint count, then one
    // [varint holder id][varint level] pair per entry. The map holds 1..255
    // levels, so a 0 here would be rejected by the client's own codec.
    w.varint(static_cast<uint32_t>(kComponentEnchantmentsId));
    w.varint(static_cast<uint32_t>(stack.enchantments.size()));
    for (const ItemEnchantment& entry : stack.enchantments) {
      w.varint(static_cast<uint32_t>(entry.enchantment));
      w.varint(static_cast<uint32_t>(entry.level));
    }
  }
  if (stack.potion_contents) {
    w.varint(static_cast<uint32_t>(kComponentPotionContentsId));
    write_potion_contents(w, *stack.potion_contents);
  }
  // A trim with either id unset is not a component *at all* rather than an
  // emitted `[0, 0]`: 0 is a direct holder to the client, so writing one would
  // desync the packet instead of dropping a malformed component.
  if (stack.trim && stack.trim->complete()) {
    w.varint(static_cast<uint32_t>(kComponentTrimId));
    write_armor_trim(w, *stack.trim);
  }
  // Values the engine carries without interpreting. The serverbound value was
  // length-delimited; the clientbound DataComponentPatch value is written raw,
  // which is exactly the same codec bytes without that outer length.
  for (const RawItemComponent& component : stack.raw_components) {
    w.varint(static_cast<uint32_t>(component.type));
    w.bytes(component.value);
  }
}

bool read_item_stack(mc::net::Reader& r, ItemStack* out) {
  uint32_t count = 0;
  if (!r.varint(&count))
    return false;
  if (count == 0) {
    out->clear();
    return true;
  }
  uint32_t item = 0;
  if (!r.varint(&item))
    return false;

  // DataComponentPatch, delimited: each added entry's value is byte-length
  // prefixed, so unknown components can be skipped rather than desyncing.
  uint32_t added = 0, removed = 0;
  if (!r.varint(&added) || !r.varint(&removed))
    return false;
  int32_t damage = 0;
  std::vector<ItemEnchantment> enchantments;
  std::optional<StackPotionContents> potion_contents;
  std::optional<StackArmorTrim> trim;
  std::vector<RawItemComponent> raw_components;
  for (uint32_t i = 0; i < added; ++i) {
    uint32_t type_id = 0;
    if (!r.varint(&type_id))
      return false;
    uint32_t len = 0;
    if (!r.varint(&len))
      return false;
    // Read exactly the delimited value bytes before looking inside them. This
    // keeps the reader aligned for every component, even a hostile one whose
    // inner varint claims more bytes than `len`: parsing from a slice can
    // never consume into the next component's region.
    std::vector<uint8_t> value;
    if (!r.bytes(len, &value))
      return false;
    if (type_id == static_cast<uint32_t>(kComponentDamageId)) {
      mc::net::Reader value_reader(value);
      uint32_t v = 0;
      if (value_reader.varint(&v))
        damage = static_cast<int32_t>(v);
    } else if (type_id == static_cast<uint32_t>(kComponentEnchantmentsId)) {
      // ItemEnchantments.STREAM_CODEC inside the delimited value. The count is
      // whatever the client claims, so it is capped before it is trusted: the
      // reader is already bounded by `len`, and stopping at the cap keeps a
      // hostile 2^31-entry map from turning into 2^31 vector growth.
      mc::net::Reader value_reader(value);
      uint32_t entries = 0;
      if (value_reader.varint(&entries)) {
        if (entries > kMaxEnchantmentsPerStack)
          return false;
        enchantments.reserve(entries);
        for (uint32_t i = 0; i < entries; ++i) {
          uint32_t id = 0, level = 0;
          if (!value_reader.varint(&id) || !value_reader.varint(&level))
            return false;
          ItemEnchantment entry;
          entry.enchantment = static_cast<int32_t>(id);
          entry.level = static_cast<int32_t>(level);
          enchantments.push_back(entry);
        }
        std::sort(enchantments.begin(), enchantments.end(),
                  [](const ItemEnchantment& a, const ItemEnchantment& b) {
                    return a.enchantment < b.enchantment;
                  });
      }
    } else if (type_id == static_cast<uint32_t>(kComponentPotionContentsId)) {
      StackPotionContents contents;
      if (!read_potion_contents(value, &contents))
        return false;
      potion_contents = std::move(contents);
    } else if (type_id == static_cast<uint32_t>(kComponentTrimId)) {
      StackArmorTrim arm_trim;
      if (read_armor_trim(value, &arm_trim))
        trim = arm_trim;
    } else {
      // Keep the delimited value verbatim. The clientbound codec wants exactly
      // these bytes, so a component this engine does not interpret still comes
      // back out on the echo instead of being silently dropped.
      RawItemComponent raw;
      raw.type = static_cast<int32_t>(type_id);
      raw.value = std::move(value);
      raw_components.push_back(std::move(raw));
    }
  }
  for (uint32_t i = 0; i < removed; ++i) {
    uint32_t type_id = 0;
    if (!r.varint(&type_id))
      return false;
  }

  out->item = static_cast<int32_t>(item);
  out->count = static_cast<int32_t>(count);
  out->damage = damage;
  out->enchantments = std::move(enchantments);
  out->potion_contents = std::move(potion_contents);
  out->trim = std::move(trim);
  out->raw_components = std::move(raw_components);
  return true;
}

} // namespace mc::world
