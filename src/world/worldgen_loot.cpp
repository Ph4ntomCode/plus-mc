// SPDX-License-Identifier: GPL-3.0-or-later
//
// The deterministic resolver behind `worldgen_loot.h`. The generated table is
// the pack's own chest/dispenser data; this file is the small interpreter over
// it, not a second copy of the tables.
#include "world/worldgen_loot.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "world/effect.h"
#include "world/enchantment.h"
#include "world/enchantment_table.h"
#include "world/item.h"
#include "world/item_registry.h"
#include "world/potion.h"
#include "world/worldgen_loot_table.h"
#include "world/worldgen_random.h"

namespace mc::world {

namespace {

constexpr int kMaxLootDepth = 16;

const WgLootTableRow* find_table(std::string_view name) {
  size_t low = 0;
  size_t high = sizeof(kWgLootTables) / sizeof(kWgLootTables[0]);
  while (low < high) {
    const size_t mid = low + (high - low) / 2;
    // Compare the table's literal against the view directly: the old form built
    // a `std::string` copy of the query on every probe of the binary search.
    // The literal stays the left operand, so the ordering the branches below
    // read is the one `strcmp(table.name, query)` had.
    const int order = std::string_view(kWgLootTables[mid].name).compare(name);
    if (order == 0)
      return &kWgLootTables[mid];
    if (order < 0)
      low = mid + 1;
    else
      high = mid;
  }
  return nullptr;
}

int32_t uniform_int(WgLegacyRandom& rng, int32_t low, int32_t high) {
  if (high <= low)
    return low;
  return low + rng.next_int(high - low + 1);
}

float uniform_float(WgLegacyRandom& rng, float low, float high) {
  if (high <= low)
    return low;
  return low + rng.next_float() * (high - low);
}

bool chance_passes(WgLegacyRandom& rng, uint8_t has_chance, float chance) {
  if (has_chance == 0)
    return true;
  return rng.next_float() < chance;
}

bool item_supports_enchantment(const GeneratedEnchantment& enchantment, int32_t item_id) {
  for (int i = 0; i < enchantment.item_count; ++i)
    if (static_cast<int32_t>(enchantment.items[i]) == item_id)
      return true;
  return false;
}

bool enchantment_in_options(const GeneratedEnchantment& enchantment, uint16_t offset,
                            uint8_t count) {
  if (count == 0)
    return true;
  for (uint8_t i = 0; i < count; ++i)
    if (std::strcmp(kWgLootEnchantments[offset + i], enchantment.name) == 0)
      return true;
  return false;
}

const GeneratedEnchantment* pick_enchantment(WgLegacyRandom& rng, const std::string& item,
                                             uint16_t offset, uint8_t count) {
  const auto item_id = ItemRegistry::instance().id_of(item);
  const bool book = item == "minecraft:book" || item == "minecraft:enchanted_book";
  std::vector<const GeneratedEnchantment*> eligible;
  eligible.reserve(static_cast<size_t>(kEnchantmentCount));
  for (const GeneratedEnchantment& enchantment : kEnchantments) {
    if (!enchantment_in_options(enchantment, offset, count))
      continue;
    if (book || (item_id && item_supports_enchantment(enchantment, *item_id)))
      eligible.push_back(&enchantment);
  }
  if (eligible.empty())
    return nullptr;
  return eligible[static_cast<size_t>(rng.next_int(static_cast<int32_t>(eligible.size())))];
}

void add_enchantment(WgLootStack* stack, const GeneratedEnchantment& enchantment, int32_t level) {
  if (stack->item == "minecraft:book")
    stack->item = "minecraft:enchanted_book";
  const int32_t id = static_cast<int32_t>(&enchantment - kEnchantments);
  for (ItemEnchantment& entry : stack->enchantments) {
    if (entry.enchantment == id) {
      entry.level = std::max(entry.level, level);
      return;
    }
  }
  stack->enchantments.push_back(ItemEnchantment{id, level});
}

void apply_functions(WgLegacyRandom& rng, const WgLootEntryRow& entry, WgLootStack* stack) {
  if (entry.function_count == 0)
    return;
  for (uint32_t i = 0; i < entry.function_count; ++i) {
    const WgLootFunctionRow& function = kWgLootFunctions[entry.function_offset + i];
    const auto kind = static_cast<WgLootFunctionKind>(function.kind);
    switch (kind) {
    case WgLootFunctionKind::kSetCount: {
      const int32_t low = static_cast<int32_t>(function.min);
      const int32_t high = static_cast<int32_t>(function.max);
      stack->count = function.range ? uniform_int(rng, low, high) : low;
      break;
    }
    case WgLootFunctionKind::kSetDamage: {
      const float value =
          function.range ? uniform_float(rng, function.min, function.max) : function.min;
      const auto item_id = ItemRegistry::instance().id_of(stack->item);
      if (!item_id)
        break;
      const GeneratedItem* def = ItemRegistry::instance().by_id(*item_id);
      if (def == nullptr || def->max_damage == 0)
        break; // `SetItemDamageFunction`: `floor((1 - clamp(value)) * maxDamage)`.
      stack->damage = static_cast<int32_t>(
          std::floor((1.0f - std::clamp(value, 0.0f, 1.0f)) * static_cast<float>(def->max_damage)));
      stack->damage = std::clamp(stack->damage, 0, static_cast<int32_t>(def->max_damage));
      break;
    }
    case WgLootFunctionKind::kEnchantRandomly: {
      const GeneratedEnchantment* enchantment = pick_enchantment(
          rng, stack->item, function.enchantment_offset, function.enchantment_count);
      if (enchantment == nullptr)
        break;
      const int32_t level = uniform_int(rng, 1, static_cast<int32_t>(enchantment->max_level));
      add_enchantment(stack, *enchantment, level);
      break;
    }
    case WgLootFunctionKind::kEnchantWithLevels: {
      const GeneratedEnchantment* enchantment = pick_enchantment(
          rng, stack->item, function.enchantment_offset, function.enchantment_count);
      if (enchantment == nullptr)
        break;
      const int32_t cost = function.range ? uniform_int(rng, static_cast<int32_t>(function.min),
                                                        static_cast<int32_t>(function.max))
                                          : static_cast<int32_t>(function.min);
      // Vanilla's table selection needs the enchantment's min/max cost and the
      // item's enchantability, neither of which the generated enchantment table
      // carries. One eligible pick with a cost-scaled level is the honest
      // approximation named in the header.
      const int32_t level =
          std::clamp(1 + cost / 12, 1, static_cast<int32_t>(enchantment->max_level));
      add_enchantment(stack, *enchantment, level);
      break;
    }
    case WgLootFunctionKind::kSetPotion: {
      if (const GeneratedPotion* potion = potion_by_name(function.text ? function.text : ""))
        stack->potion = potion->id;
      break;
    }
    case WgLootFunctionKind::kSetStewEffect: {
      for (uint32_t e = 0; e < function.effect_count; ++e) {
        const WgLootEffectRow& effect = kWgLootEffects[function.effect_offset + e];
        const GeneratedMobEffect* def = mob_effect_by_name(effect.effect ? effect.effect : "");
        if (def == nullptr)
          continue;
        MobEffectInstance instance;
        instance.effect = mob_effect_id_of(*def);
        instance.duration = static_cast<int>(
            std::lround(effect.duration_max > effect.duration_min
                            ? uniform_float(rng, effect.duration_min, effect.duration_max)
                            : effect.duration_min));
        instance.amplifier = 0;
        stack->custom_effects.push_back(instance);
      }
      break;
    }
    case WgLootFunctionKind::kSetEnchantments: {
      if (function.text == nullptr)
        break;
      if (const GeneratedEnchantment* enchantment = enchantment_by_name(function.text)) {
        const int32_t id = static_cast<int32_t>(enchantment - kEnchantments);
        stack->enchantments.push_back(ItemEnchantment{id, std::max<int32_t>(1, function.data)});
        if (stack->item == "minecraft:book")
          stack->item = "minecraft:enchanted_book";
      }
      break;
    }
    case WgLootFunctionKind::kExplorationMap:
      if (stack->item == "minecraft:map")
        stack->item = "minecraft:filled_map";
      break;
    case WgLootFunctionKind::kSetName:
    case WgLootFunctionKind::kSetOminousBottleAmplifier:
      // No custom-name/ominous component is modelled; see the header.
      break;
    }
  }
}

class Roller {
public:
  Roller(WgLegacyRandom& rng_in, std::vector<WgLootStack>* out_in) : rng(rng_in), out(out_in) {}

  void table(const WgLootTableRow& table, int depth) {
    if (depth > kMaxLootDepth || table.pool_count == 0)
      return;
    for (uint32_t p = 0; p < table.pool_count; ++p) {
      const WgLootPoolRow& pool = kWgLootPools[table.pool_offset + p];
      if (!chance_passes(rng, pool.has_chance, pool.chance))
        continue;
      const int32_t rolls = uniform_int(
          rng, pool.rolls_min, pool.rolls_max > pool.rolls_min ? pool.rolls_max : pool.rolls_min);
      for (int32_t roll = 0; roll < std::max<int32_t>(0, rolls); ++roll)
        entry(pool, depth);
    }
  }

private:
  void entry(const WgLootPoolRow& pool, int depth) {
    // Conditions decide whether an entry is in the pool *before* the weighted
    // pick, exactly as vanilla filters `LootPoolEntryContainer`. Roll each
    // condition once so a failed entry cannot consume the pick's range.
    std::vector<const WgLootEntryRow*> available;
    available.reserve(pool.entry_count);
    int64_t total = 0;
    for (uint16_t i = 0; i < pool.entry_count; ++i) {
      const WgLootEntryRow& row = kWgLootEntries[pool.entry_offset + i];
      if (!chance_passes(rng, row.has_chance, row.chance))
        continue;
      // Luck is zero for a generated chest; quality would scale the weight by
      // `1 + quality * luck`, which is the identity here.
      const int32_t weight = std::max<int32_t>(0, row.weight);
      if (weight == 0)
        continue;
      available.push_back(&row);
      total += weight;
    }
    if (total <= 0)
      return;
    int32_t pick = rng.next_int(static_cast<int32_t>(total));
    for (const WgLootEntryRow* row : available) {
      const int32_t weight = std::max<int32_t>(0, row->weight);
      if (pick >= weight) {
        pick -= weight;
        continue;
      }
      emit(*row, depth);
      return;
    }
  }

  void emit(const WgLootEntryRow& row, int depth) {
    switch (static_cast<WgLootEntryKind>(row.kind)) {
    case WgLootEntryKind::kEmpty:
      return;
    case WgLootEntryKind::kItem: {
      if (row.item == nullptr)
        return;
      WgLootStack stack;
      stack.item = row.item;
      apply_functions(rng, row, &stack);
      if (!stack.empty())
        out->push_back(std::move(stack));
      return;
    }
    case WgLootEntryKind::kTable:
      if (row.table_index < sizeof(kWgLootTables) / sizeof(kWgLootTables[0]))
        table(kWgLootTables[row.table_index], depth + 1);
      return;
    case WgLootEntryKind::kAlternatives:
      // The first child whose own conditions pass. The generated child rows
      // carry those conditions, so this is a scan rather than a second check.
      for (uint16_t i = 0; i < row.child_count; ++i) {
        const WgLootEntryRow& child = kWgLootEntries[row.child_offset + i];
        if (child.has_chance && !chance_passes(rng, child.has_chance, child.chance))
          continue;
        emit(child, depth + 1);
        return;
      }
      return;
    case WgLootEntryKind::kGroup:
    case WgLootEntryKind::kSequence:
      for (uint16_t i = 0; i < row.child_count; ++i)
        emit(kWgLootEntries[row.child_offset + i], depth + 1);
      return;
    }
  }

  WgLegacyRandom& rng;
  std::vector<WgLootStack>* out;
};

} // namespace

const WgLootTableRow* wg_loot_table_by_name(std::string_view name) {
  return find_table(name);
}

std::vector<WgLootStack> wg_roll_loot_table(std::string_view table_name, int64_t seed) {
  std::vector<WgLootStack> out;
  const WgLootTableRow* table = find_table(table_name);
  if (table == nullptr)
    return out;
  WgLegacyRandom rng(seed);
  Roller roller(rng, &out);
  roller.table(*table, 0);
  return out;
}

int64_t wg_structure_loot_seed(int64_t world_seed, int32_t x, int32_t y, int32_t z,
                               std::string_view table) {
  // FNV-1a over the name, mixed with the position's vanilla seed and the world
  // seed. The result is a stable long, not a draw from any other stream.
  uint64_t hash = 1469598103934665603ull;
  for (const char c : table) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  const uint64_t positional = static_cast<uint64_t>(wg_mth_get_seed(x, y, z));
  const uint64_t world = static_cast<uint64_t>(world_seed);
  return static_cast<int64_t>(hash ^ positional ^ (world * 0x9E3779B97F4A7C15ull));
}

std::vector<ItemStack> wg_structure_loot_items(std::string_view table, int64_t seed) {
  std::vector<ItemStack> out;
  const auto& registry = ItemRegistry::instance();
  for (const WgLootStack& rolled : wg_roll_loot_table(table, seed)) {
    const auto item_id = registry.id_of(rolled.item);
    if (!item_id || *item_id == kAirItemId || rolled.count <= 0)
      continue;
    const int32_t stack_size = std::max<int32_t>(1, registry.max_stack_size(*item_id));
    // `LootTable.createStackSplitter`: a rolled count larger than the stack
    // size becomes several stacks, not one illegally large one.
    for (int32_t left = rolled.count; left > 0; left -= stack_size) {
      ItemStack stack = ItemStack::of_id(*item_id, std::min(left, stack_size));
      stack.damage = rolled.damage;
      if (rolled.potion >= 0) {
        StackPotionContents contents;
        contents.potion = rolled.potion;
        contents.custom_effects = rolled.custom_effects;
        stack.potion_contents = std::move(contents);
      } else if (!rolled.custom_effects.empty()) {
        StackPotionContents contents;
        contents.custom_effects = rolled.custom_effects;
        stack.potion_contents = std::move(contents);
      }
      stack.enchantments = rolled.enchantments;
      std::sort(stack.enchantments.begin(), stack.enchantments.end(),
                [](const ItemEnchantment& a, const ItemEnchantment& b) {
                  return a.enchantment < b.enchantment;
                });
      out.push_back(std::move(stack));
    }
  }
  return out;
}

} // namespace mc::world
