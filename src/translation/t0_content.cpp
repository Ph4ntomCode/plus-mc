// SPDX-License-Identifier: GPL-3.0-or-later
#include "translation/t0_content.h"

#include <cstddef>
#include <utility>

#include "translation/t0_blocks.generated.h"
#include "translation/t0_registries.generated.h"
#include "world/block_states.h"

namespace mc::translation::t0 {

namespace {

static_assert(sizeof(kCanonicalTo2612) / sizeof(kCanonicalTo2612[0]) == kCanonicalBlockStateCount);
static_assert(sizeof(kCanonicalTo262) / sizeof(kCanonicalTo262[0]) == kCanonicalBlockStateCount);
static_assert(kCanonicalBlockStateCount == mc::world::kBlockStateCount);
static_assert(sizeof(kCanonicalDataComponentTo262) / sizeof(int32_t) ==
              kCanonicalDataComponentCount);
static_assert(sizeof(kCanonicalEnchantmentTo262) / sizeof(int32_t) == kCanonicalEnchantmentCount);
static_assert(sizeof(kCanonicalPotionTo262) / sizeof(int32_t) == kCanonicalPotionCount);
static_assert(sizeof(kCanonicalMobEffectTo262) / sizeof(int32_t) == kCanonicalMobEffectCount);
static_assert(sizeof(kCanonicalTrimMaterialTo262) / sizeof(int32_t) == kCanonicalTrimMaterialCount);
static_assert(sizeof(kCanonicalTrimPatternTo262) / sizeof(int32_t) == kCanonicalTrimPatternCount);

class RemappedBlockStateTable final : public mc::world::BlockStateTable {
public:
  explicit RemappedBlockStateTable(const int32_t* map) : map_(map) {}

  std::optional<int32_t> lookup(const mc::world::BlockStateKey& key) const override {
    const auto canonical = mc::world::full_block_states().lookup(key);
    if (!canonical || *canonical < 0 || *canonical >= kCanonicalBlockStateCount)
      return 0; // a state outside the generated registry is safer as air
    const int32_t target = map_[*canonical];
    return target < 0 ? 0 : target;
  }

private:
  const int32_t* map_;
};

// A registry map is indexed by the canonical id and stores -1 for an entry the
// target release does not have. Protocol 775 is 26.1.x and 776 is 26.2; any
// other protocol keeps the canonical id (the caller is already on the
// canonical path in that case).
int32_t map_id(int protocol, int32_t canonical_id, int32_t canonical_count, const int32_t* to_262,
               const int32_t* to_2612) {
  if (canonical_id < 0 || canonical_id >= canonical_count)
    return -1;
  if (protocol == 775)
    return to_2612[canonical_id];
  if (protocol == 776)
    return to_262[canonical_id];
  return canonical_id;
}

int32_t target_component_id(int protocol, int32_t id) {
  return map_id(protocol, id, kCanonicalDataComponentCount, kCanonicalDataComponentTo262,
                kCanonicalDataComponentTo2612);
}
int32_t target_enchantment_id(int protocol, int32_t id) {
  return map_id(protocol, id, kCanonicalEnchantmentCount, kCanonicalEnchantmentTo262,
                kCanonicalEnchantmentTo2612);
}
int32_t target_potion_id(int protocol, int32_t id) {
  return map_id(protocol, id, kCanonicalPotionCount, kCanonicalPotionTo262, kCanonicalPotionTo2612);
}
int32_t target_mob_effect_id(int protocol, int32_t id) {
  return map_id(protocol, id, kCanonicalMobEffectCount, kCanonicalMobEffectTo262,
                kCanonicalMobEffectTo2612);
}
int32_t target_trim_material_id(int protocol, int32_t id) {
  return map_id(protocol, id, kCanonicalTrimMaterialCount, kCanonicalTrimMaterialTo262,
                kCanonicalTrimMaterialTo2612);
}
int32_t target_trim_pattern_id(int protocol, int32_t id) {
  return map_id(protocol, id, kCanonicalTrimPatternCount, kCanonicalTrimPatternTo262,
                kCanonicalTrimPatternTo2612);
}

// MobEffectInstance.Details.STREAM_CODEC. A hidden layer carries no effect id
// of its own, so only the outer instance goes through the effect-id map.
void write_effect_details(mc::net::Writer& w, const mc::world::MobEffectInstance& instance) {
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

// PotionContents.STREAM_CODEC, projected: optional potion holder, optional
// custom colour, custom-effect list and optional custom name. Returns false
// when nothing in the component survives the target registry. `custom_effects`
// keeps only effects the target has; dropping an unknown effect is safer than
// naming a different one. The colour and name are registry-independent and are
// copied unchanged.
bool write_target_potion_contents(mc::net::Writer& w,
                                  const mc::world::StackPotionContents& contents, int protocol) {
  const int32_t potion = contents.potion >= 0 ? target_potion_id(protocol, contents.potion) : -1;
  std::vector<const mc::world::MobEffectInstance*> effects;
  effects.reserve(contents.custom_effects.size());
  for (const mc::world::MobEffectInstance& instance : contents.custom_effects)
    if (target_mob_effect_id(protocol, instance.effect) >= 0)
      effects.push_back(&instance);
  if (potion < 0 && effects.empty() && !contents.custom_color && !contents.custom_name)
    return false;
  w.u8(potion >= 0 ? 1 : 0);
  if (potion >= 0)
    w.varint(static_cast<uint32_t>(potion));
  w.u8(contents.custom_color ? 1 : 0);
  if (contents.custom_color)
    w.u32(static_cast<uint32_t>(*contents.custom_color));
  w.varint(static_cast<uint32_t>(effects.size()));
  for (const mc::world::MobEffectInstance* instance : effects) {
    w.varint(static_cast<uint32_t>(target_mob_effect_id(protocol, instance->effect)));
    write_effect_details(w, *instance);
  }
  w.u8(contents.custom_name ? 1 : 0);
  if (contents.custom_name)
    w.string(*contents.custom_name);
  return true;
}

} // namespace

int32_t target_item_id(int protocol, int32_t canonical_item_id) {
  if (canonical_item_id < 0 || canonical_item_id >= kCanonicalItemCount)
    return -1;
  if (protocol == 775)
    return kCanonicalItemTo2612[canonical_item_id];
  if (protocol == 776)
    return kCanonicalItemTo262[canonical_item_id];
  return canonical_item_id;
}

int32_t canonical_item_id(int protocol, int32_t target_item) {
  if (target_item < 0)
    return -1;
  const int32_t* map = nullptr;
  if (protocol == 775)
    map = kCanonicalItemTo2612;
  else if (protocol == 776)
    map = kCanonicalItemTo262;
  else
    return target_item;
  for (int32_t canonical = 0; canonical < kCanonicalItemCount; ++canonical)
    if (map[canonical] == target_item)
      return canonical;
  return -1;
}

int32_t target_entity_type_id(int protocol, int32_t canonical_entity_type_id) {
  if (canonical_entity_type_id < 0 || canonical_entity_type_id >= kCanonicalEntityTypeCount)
    return -1;
  if (protocol == 775)
    return kCanonicalEntityTypeTo2612[canonical_entity_type_id];
  if (protocol == 776)
    return kCanonicalEntityTypeTo262[canonical_entity_type_id];
  return canonical_entity_type_id;
}

void write_item_stack(mc::net::Writer& w, const mc::world::ItemStack& stack, int protocol) {
  const int32_t item = target_item_id(protocol, stack.item);
  if (stack.empty() || item < 0) {
    w.varint(0);
    return;
  }

  // The patch is assembled first because its first varint is the number of
  // entries that follow, and an entry can be dropped when its component type or
  // one of its registry ids is absent from the target release.
  mc::net::Writer patch;
  int32_t added = 0;

  const int32_t damage_component = target_component_id(protocol, mc::world::kComponentDamageId);
  if (stack.damage > 0 && damage_component >= 0) {
    patch.varint(static_cast<uint32_t>(damage_component));
    patch.varint(static_cast<uint32_t>(stack.damage));
    ++added;
  }

  const int32_t enchantments_component =
      target_component_id(protocol, mc::world::kComponentEnchantmentsId);
  if (stack.has_enchantments() && enchantments_component >= 0) {
    std::vector<mc::world::ItemEnchantment> projected;
    projected.reserve(stack.enchantments.size());
    for (const mc::world::ItemEnchantment& entry : stack.enchantments) {
      const int32_t id = target_enchantment_id(protocol, entry.enchantment);
      if (id >= 0)
        projected.push_back({id, entry.level});
    }
    if (!projected.empty()) {
      patch.varint(static_cast<uint32_t>(enchantments_component));
      patch.varint(static_cast<uint32_t>(projected.size()));
      for (const mc::world::ItemEnchantment& entry : projected) {
        patch.varint(static_cast<uint32_t>(entry.enchantment));
        patch.varint(static_cast<uint32_t>(entry.level));
      }
      ++added;
    }
  }

  const int32_t potion_component =
      target_component_id(protocol, mc::world::kComponentPotionContentsId);
  if (stack.potion_contents && potion_component >= 0) {
    mc::net::Writer potion;
    if (write_target_potion_contents(potion, *stack.potion_contents, protocol)) {
      patch.varint(static_cast<uint32_t>(potion_component));
      patch.bytes(std::move(potion).take());
      ++added;
    }
  }

  const int32_t trim_component = target_component_id(protocol, mc::world::kComponentTrimId);
  if (stack.trim && stack.trim->complete() && trim_component >= 0) {
    const int32_t material = target_trim_material_id(protocol, stack.trim->material);
    const int32_t pattern = target_trim_pattern_id(protocol, stack.trim->pattern);
    if (material >= 0 && pattern >= 0) {
      patch.varint(static_cast<uint32_t>(trim_component));
      patch.varint(static_cast<uint32_t>(material + 1));
      patch.varint(static_cast<uint32_t>(pattern + 1));
      ++added;
    }
  }

  // Components the engine carries opaquely: remap the type id and preserve the
  // value bytes. The four modelled components above are excluded from this list
  // by read_item_stack, so there is no duplicate entry. A component absent from
  // the target registry is dropped rather than sent under a different name.
  for (const mc::world::RawItemComponent& component : stack.raw_components) {
    const int32_t type = target_component_id(protocol, component.type);
    if (type < 0)
      continue;
    patch.varint(static_cast<uint32_t>(type));
    patch.bytes(component.value);
    ++added;
  }

  w.varint(static_cast<uint32_t>(stack.count));
  w.varint(static_cast<uint32_t>(item));
  w.varint(static_cast<uint32_t>(added));
  w.varint(0); // removed components
  w.bytes(std::move(patch).take());
}

const mc::world::BlockStateTable& block_states(int protocol) {
  static const RemappedBlockStateTable k2612(kCanonicalTo2612);
  static const RemappedBlockStateTable k262(kCanonicalTo262);
  if (protocol == 775)
    return k2612;
  if (protocol == 776)
    return k262;
  return mc::world::full_block_states();
}

TargetBiomeTable::TargetBiomeTable(std::vector<std::string> names, std::string fallback)
    : names_(std::move(names)) {
  fallback_ = names_.lookup(fallback).value_or(0);
}

std::optional<int32_t> TargetBiomeTable::lookup(const std::string& name) const {
  if (const auto id = names_.lookup(name))
    return id;
  return fallback_;
}

size_t TargetBiomeTable::registry_size() const {
  return names_.registry_size();
}

} // namespace mc::translation::t0
