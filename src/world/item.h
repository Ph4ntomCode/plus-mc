// SPDX-License-Identifier: GPL-3.0-or-later
//
// Items: the runtime view over the generated item table
// (world/item_registry.h) plus the ItemStack value type and its wire codec.
//
// ItemStack shape on the wire (ItemStack.OPTIONAL_STREAM_CODEC, confirmed from
// the decompiled 26.2 codecs — see docs/PHASE3.md):
//
//     [varint count]                     0 means "empty", and nothing follows
//     [varint item id]                   present only when count != 0
//     [DataComponentPatch]
//
// The patch is `[varint added][varint removed]` followed by the entries; each
// entry is `[varint component type id][value]`. The *untrusted* variant the
// client sends (DELIMITED_STREAM_CODEC, used by set_creative_mode_slot and
// container_click) wraps every value in a byte-length varint so a server can
// skip components it does not model — see read_item_stack.
//
// Components are modelled as far as the engine needs them for Phase 3: the
// damage (durability) component, the enchantments component, the
// potion_contents component and the trim component. Everything else on a stack
// is carried as opaque bytes in both directions, so a display-only component
// such as custom_name or custom_model_data survives a server echo even though
// gameplay code does not interpret it.
//
// The enchantments component matters on the wire rather than only internally:
// `ItemStack.isEnchanted()` is what makes the client draw the glint and list
// the enchantment in the tooltip, and it reads the component, so `/enchant`
// that only changed server-side state would be invisible to the player.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "net/byte_buffer.h"
#include "world/effect.h"
#include "world/item_registry.h"

namespace mc::world {

// minecraft:air. A stack with this item (or count 0) is empty.
constexpr int32_t kAirItemId = 0;

// The most enchantment entries a single stack will hold, whether they arrived
// from `/enchant` or from a client's item-stack patch. Vanilla has no such cap
// (`ItemEnchantments` is an unbounded map, and the data-driven enchantment set
// is finite), so this exists only so a hostile packet cannot ask for unbounded
// growth; 32 is far past the six an exclusive-set-grouped item can carry.
constexpr size_t kMaxEnchantmentsPerStack = 32;

// The most custom effects one stack's minecraft:potion_contents will hold, and
// the most hidden-effect layers one instance's Details will nest.
//
// Vanilla caps neither: custom_effects is an unbounded list and Details is a
// recursive codec, so a client decides both. 32 is past the 45 effect entries
// the whole potion registry holds (no potion plus its custom effects exceeds
// it), and the hidden chain is one layer deep in every state this server can
// produce — what the cap really stops is a packet that nests Details until the
// reader runs out of stack.
constexpr size_t kMaxPotionCustomEffects = 32;
constexpr int kMaxHiddenEffectLayers = 8;

// "stone" / "minecraft:stone" / "STONE" -> "minecraft:stone". Anything that is
// not a plain identifier is rejected by returning an empty string.
std::string normalize_item_name(std::string_view raw);

// `Item.getCraftingRemainder`: the item one unit of `item` turns into when a
// craft, a brew or a furnace consumes it, or kAirItemId for none.
//
// In 26.2 this is an Item *property* (`Item.Properties.craftRemainder`) rather
// than a data component, so it is code and lives here instead of in the
// generated item table: `Items.java` sets it on exactly five items — a water,
// lava or milk bucket leaves a bucket, and dragon's breath or a honey bottle
// leaves a glass bottle. (The `minecraft:use_remainder` component the item
// table does carry — `use_components.h` — is the *other* one: the item a
// consumable leaves behind when it is eaten, which is why a stew has a
// use_remainder and no crafting remainder.)
int32_t crafting_remainder_item(int32_t item);

// Lookups over the generated table. The table's ids are registry indices and
// are dense (pinned by the item-table test), so id -> entry is an array index.
class ItemRegistry {
public:
  static const ItemRegistry& instance();

  // Normalizes `name` first; returns nullopt for an unknown or malformed name.
  std::optional<int32_t> id_of(std::string_view name) const;
  // nullptr when the id is out of range (including negatives).
  const GeneratedItem* by_id(int32_t id) const;
  // "" for an out-of-range id.
  std::string_view name_of(int32_t id) const;

  int max_stack_size(int32_t id) const;
  bool is_empty_stack(int32_t id) const {
    return id == kAirItemId;
  }
  bool is_block_item(int32_t id) const;
  bool is_damageable(int32_t id) const;

private:
  ItemRegistry();
  std::unordered_map<std::string_view, int32_t> by_name_;
};

// One entry of the minecraft:enchantments component.
struct ItemEnchantment {
  int32_t enchantment = 0; // the enchantment registry id (its table index)
  int32_t level = 1;
};

// The minecraft:potion_contents component (id 53) of a stack: which potion it
// holds and any effects it adds on top of that potion's own.
//
// `potion` is `PotionContents.potion`, an `Optional<Holder<Potion>>` — absent
// for a component that names no potion at all (a plain bottle carries no
// component; a bottle with one and no potion is what a water bottle's contents
// look like), and otherwise the potion registry id of potion_table.h.
//
// `custom_effects` is `PotionContents.customEffects`, applied *after* the
// potion's own list (getAllEffects concatenates the two).
//
// `custom_color` tints the bottle and `custom_name` renames it. They are
// display-only, but are kept so a stack echoed to a client — including a
// translated 26.1/26.2 client — preserves the bottle's appearance.
struct StackPotionContents {
  int32_t potion = -1;
  std::vector<MobEffectInstance> custom_effects;
  std::optional<int32_t> custom_color;
  std::optional<std::string> custom_name;

  bool empty() const {
    return potion < 0 && custom_effects.empty() && !custom_color && !custom_name;
  }
};

// The minecraft:trim component (id 56, DataComponents.TRIM) of a stack: which
// trim material and pattern an armor piece wears, both as registry ids (see
// world/trim.h for the tables and the item -> material link).
//
// Vanilla's `ArmorTrim` is `(Holder<TrimMaterial>, Holder<TrimPattern>)` with no
// optionality inside the record, so a stack either carries no component at all
// or a complete one; `StackArmorTrim` is the complete one, and an ItemStack
// holds it in a std::optional for exactly that reason. The record has no other
// field: the tooltip and the armor layer texture are both derived from the two
// holders by the client, so this server needs nothing else to make a trimmed
// piece render trimmed.
struct StackArmorTrim {
  int32_t material = -1; // minecraft:trim_material registry id
  int32_t pattern = -1;  // minecraft:trim_pattern registry id

  // Both fields are always set on a component that exists (the vanilla record
  // holds no optional inside), so a negative id here is a bug in the producer,
  // not a stack shape to interpret: the writer emits `id + 1` and a 0 would be
  // a *direct* holder to the client, i.e. a different component entirely.
  bool complete() const {
    return material >= 0 && pattern >= 0;
  }
};

// A component the engine does not interpret. The delimited value is kept
// opaque and re-emitted under the canonical or translated type id, which
// preserves display components (custom_name, custom_model_data, food, tool,
// equippable, ...) without modelling their semantics server-side. The payload
// itself is not transcoded, so a component that embeds registry ids keeps the
// canonical ids inside it.
struct RawItemComponent {
  int32_t type = 0;
  std::vector<uint8_t> value;

  bool operator==(const RawItemComponent& other) const {
    return type == other.type && value == other.value;
  }
};

// A stack of items: the unit every inventory slot, container and recipe
// interaction works in.
struct ItemStack {
  int32_t item = kAirItemId;
  int32_t count = 0;
  // The minecraft:damage component (durability used); 0 for a fresh item.
  int32_t damage = 0;
  // The minecraft:enchantments component (DataComponents.ENCHANTMENTS, which is
  // what ItemEnchantments.STREAM_CODEC puts on the wire). Empty for an
  // unenchanted stack. Kept sorted by enchantment id, so the wire form is
  // deterministic and `same_item` is a straight comparison — vanilla's is an
  // Object2IntOpenHashMap with no order of its own, but nothing reads it in a
  // way that makes order observable.
  std::vector<ItemEnchantment> enchantments;
  // The minecraft:potion_contents component, absent for a stack that has none
  // (a /give'd potion is exactly that: a bottle with no contents).
  std::optional<StackPotionContents> potion_contents;
  // The minecraft:trim component, absent for an untrimmed armor piece.
  std::optional<StackArmorTrim> trim;
  // Components whose values are carried verbatim across the server echo. The
  // order is the order they arrived in; it is not observable to the client.
  std::vector<RawItemComponent> raw_components;

  bool empty() const {
    return count <= 0 || item == kAirItemId;
  }
  void clear() {
    item = kAirItemId;
    count = 0;
    damage = 0;
    enchantments.clear();
    potion_contents.reset();
    trim.reset();
    raw_components.clear();
  }
  // EnchantmentHelper.getItemEnchantmentLevel: 0 when the stack does not carry
  // that enchantment.
  int enchantment_level(int32_t enchantment) const;
  bool has_enchantments() const {
    return !enchantments.empty();
  }
  // ItemStack.enchant / EnchantmentHelper.updateEnchantments: a level of 0 or
  // less removes the entry (which is how the anvil clears one).
  void set_enchantment(int32_t enchantment, int32_t level);
  // The summed minecraft:damage_protection the *item's own* enchantments
  // contribute against `type`; see world/enchantment.h for the equipment-wide
  // lookup that calls this per slot.
  int max_stack_size() const {
    return ItemRegistry::instance().max_stack_size(item);
  }
  bool is_damageable() const {
    return count > 0 && ItemRegistry::instance().is_damageable(item);
  }
  // Whether the item carries a tool component (pickaxe/axe/shovel/hoe/shears/
  // sword). Vanilla damages these — and only these — when a block is broken.
  bool has_tool() const;
  // Whether the item is in `#minecraft:swords`. Player.isSweepAttack asks this
  // of the main-hand stack, which is what makes a sweep a sword's move alone.
  bool is_sword() const;
  // Whether the item is `minecraft:mace`. MaceItem is the one item whose
  // getAttackDamageBonus is not 0.0f — the smash — so this is what tells the
  // session that the fall distance it tracks is a weapon's damage.
  bool is_mace() const;
  // Whether the item carries the `minecraft:weapon` component —
  // `ItemStack.has(DataComponents.WEAPON)` in vanilla, and the gate
  // `ItemStack.hurtEnemy` awards `Stats.ITEM_USED` on. The component and the
  // item's `minecraft:attack_damage` modifiers are installed by the same
  // `Item.Properties` tool/sword call (ToolMaterial.applyToolProperties /
  // applySwordProperties), so this table's positive attack-damage modifier is
  // the component's stand-in: a shield or a fishing rod has neither, a sword,
  // an axe, a shovel, a trident and a mace all have both.
  bool is_weapon() const;

  // The player's `minecraft:attack_damage` attribute while holding this stack:
  // the base 1.0 plus the item's modifier (a diamond sword is 7.0, an iron axe
  // 9.0). Player.attack's base damage, before the attack-strength scale and the
  // critical multiplier.
  float attack_damage() const;
  // The player's `minecraft:attack_speed` attribute while holding this stack:
  // the base 4.0 plus the item's (negative) modifier — a diamond sword reaches
  // 1.6, which is what makes its swing take 12.5 ticks
  // (Player.getCurrentItemAttackStrengthDelay).
  float attack_speed() const;
  int max_damage() const;
  // The remaining durability, or -1 when the item is not damageable.
  int remaining_durability() const;
  bool max_damaged() const;
  // Applies `amount` of durability damage. Returns true when the item broke
  // (the caller should clear the slot); a non-damageable stack never changes.
  bool damage_by(int amount);

  // Two stacks merge only when they are the same item in the same state —
  // which in 1.20.5+ means `isSameItemSameComponents`, so an enchanted sword
  // does not stack with a plain one.
  bool same_item(const ItemStack& other) const {
    if (item != other.item || damage != other.damage)
      return false;
    if (enchantments.size() != other.enchantments.size())
      return false;
    for (size_t i = 0; i < enchantments.size(); ++i)
      if (enchantments[i].enchantment != other.enchantments[i].enchantment ||
          enchantments[i].level != other.enchantments[i].level)
        return false;
    // isSameItemSameComponents compares the components: a potion of healing is
    // not the same item as a water bottle, and a stack with no component at all
    // is not the same as one carrying an empty one. The custom effects are
    // compared field by field (MobEffectInstance has no equals of its own in
    // vanilla either, where the record's List comparison falls back to identity
    // — and two effect instances that differ in no field are the same effect).
    if (potion_contents.has_value() != other.potion_contents.has_value())
      return false;
    if (potion_contents) {
      if (potion_contents->potion != other.potion_contents->potion)
        return false;
      if (potion_contents->custom_color != other.potion_contents->custom_color)
        return false;
      if (potion_contents->custom_name != other.potion_contents->custom_name)
        return false;
      if (potion_contents->custom_effects.size() != other.potion_contents->custom_effects.size())
        return false;
      for (size_t i = 0; i < potion_contents->custom_effects.size(); ++i)
        if (!same_effect_instance(potion_contents->custom_effects[i],
                                  other.potion_contents->custom_effects[i]))
          return false;
    }
    // The trim is a component like any other: a gold-trimmed chestplate is not
    // the same item as the plain one (nor as a differently trimmed one).
    if (trim.has_value() != other.trim.has_value())
      return false;
    if (trim && (trim->material != other.trim->material || trim->pattern != other.trim->pattern))
      return false;
    // Opaque components are components too: a custom-named stack does not merge
    // with the plain item, and a differing model-data patch is a different stack.
    if (raw_components.size() != other.raw_components.size())
      return false;
    for (size_t i = 0; i < raw_components.size(); ++i)
      if (!(raw_components[i] == other.raw_components[i]))
        return false;
    return true;
  }
  bool can_merge_with(const ItemStack& other) const {
    return !empty() && !other.empty() && same_item(other) && count < max_stack_size();
  }

  // Moves up to `n` items in. Returns how many were accepted.
  int grow(int n);
  // Removes up to `n` items. Returns how many were actually removed.
  int shrink(int n);

  static ItemStack of_id(int32_t item, int32_t count = 1);
  // Returns an empty stack for an unknown name.
  static ItemStack of(std::string_view name, int32_t count = 1);
};

// Writes a stack the way the client expects (the trusted, non-delimited patch).
void write_item_stack(mc::net::Writer& w, const ItemStack& stack);

// Reads a stack from a client-sent packet (the delimited, untrusted patch).
// Returns false on truncation; `*out` is untouched on failure. The reader is
// left positioned after the stack, so callers can keep parsing.
bool read_item_stack(mc::net::Reader& r, ItemStack* out);

} // namespace mc::world
