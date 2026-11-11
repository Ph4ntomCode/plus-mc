// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 4's structure loot: the generated chest/dispenser tables, the resolver
// over them, and the two generated chest paths that consume it (a seed-42
// shipwreck and the two known seed-42 monster rooms). Deliberately small: it
// generates eleven surface/decorated columns and no ring lists, so it runs in
// about a second.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "world/chunk.h"
#include "world/container.h"
#include "world/effect.h"
#include "world/enchantment.h"
#include "world/enchantment_table.h"
#include "world/generated_chunk.h"
#include "world/item.h"
#include "world/item_registry.h"
#include "world/nbt.h"
#include "world/potion.h"
#include "world/worldgen_biome.h"
#include "world/worldgen_chunk.h"
#include "world/worldgen_feature.h"
#include "world/worldgen_loot.h"
#include "world/worldgen_loot_table.h"
#include "world/worldgen_structure.h"
#include "world/worldgen_structure_piece.h"

using namespace mc::world;

namespace {

int failures = 0;
int checks = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    ++checks;                                                                                      \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECK_EQ(a, b)                                                                             \
  do {                                                                                             \
    ++checks;                                                                                      \
    const long long va = static_cast<long long>(a), vb = static_cast<long long>(b);                \
    if (va != vb) {                                                                                \
      std::fprintf(stderr, "FAIL %s:%d: %s == %lld, want %lld\n", __FILE__, __LINE__, #a, va, vb); \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

uint64_t fnv(uint64_t hash, std::string_view text) {
  for (const char c : text) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

uint64_t digest(const std::vector<WgLootStack>& stacks) {
  uint64_t hash = 1469598103934665603ull;
  for (const WgLootStack& stack : stacks) {
    hash = fnv(hash, stack.item);
    hash = fnv(hash, std::to_string(stack.count));
    hash = fnv(hash, std::to_string(stack.damage));
    hash = fnv(hash, std::to_string(stack.potion));
    for (const ItemEnchantment& enchantment : stack.enchantments) {
      hash = fnv(hash, std::to_string(enchantment.enchantment));
      hash = fnv(hash, std::to_string(enchantment.level));
    }
    for (const MobEffectInstance& effect : stack.custom_effects) {
      hash = fnv(hash, std::to_string(effect.effect));
      hash = fnv(hash, std::to_string(effect.duration));
    }
    hash ^= '\n';
    hash *= 1099511628211ull;
  }
  return hash;
}

void test_generated_tables() {
  CHECK(wg_loot_table_by_name("minecraft:chests/shipwreck_treasure") != nullptr);
  CHECK(wg_loot_table_by_name("minecraft:chests/simple_dungeon") != nullptr);
  CHECK(wg_loot_table_by_name("minecraft:archaeology/desert_pyramid") != nullptr);
  CHECK(wg_loot_table_by_name("minecraft:spawners/trial_chamber/key") != nullptr);
  CHECK(wg_loot_table_by_name("minecraft:chests/not_a_table") == nullptr);
  CHECK_EQ(sizeof(kWgLootTables) / sizeof(kWgLootTables[0]), 71u);

  const auto& registry = ItemRegistry::instance();
  // Every name in the flattened data must be a real registry entry, including
  // the entries a particular seed never happens to pick.
  for (const WgLootEntryRow& entry : kWgLootEntries) {
    if (entry.item != nullptr)
      CHECK(registry.id_of(entry.item).has_value());
    CHECK(entry.table_index == 0xFFFFu ||
          entry.table_index < sizeof(kWgLootTables) / sizeof(kWgLootTables[0]));
  }
  for (const char* enchantment : kWgLootEnchantments) {
    if (enchantment != nullptr)
      CHECK(enchantment_by_name(enchantment) != nullptr);
  }
  for (const WgLootFunctionRow& function : kWgLootFunctions) {
    if (function.text == nullptr)
      continue;
    switch (static_cast<WgLootFunctionKind>(function.kind)) {
    case WgLootFunctionKind::kSetPotion:
      CHECK(potion_by_name(function.text) != nullptr);
      break;
    case WgLootFunctionKind::kSetEnchantments:
      CHECK(enchantment_by_name(function.text) != nullptr);
      break;
    case WgLootFunctionKind::kSetStewEffect:
      for (uint8_t e = 0; e < function.effect_count; ++e)
        CHECK(mob_effect_by_name(kWgLootEffects[function.effect_offset + e].effect) != nullptr);
      break;
    default:
      break;
    }
  }
  size_t non_empty_tables = 0;
  size_t total_stacks = 0;
  for (const WgLootTableRow& table : kWgLootTables) {
    if (table.pool_count == 0)
      continue;
    for (int64_t seed : {1ll, 2ll, 0x5DEECE66Dll}) {
      const std::vector<WgLootStack> stacks = wg_roll_loot_table(table.name, seed);
      // A nested table entry can add stacks beyond the top-level roll count;
      // 256 is comfortably above every table's generated shape.
      CHECK(stacks.size() <= 256u);
      for (const ItemStack& stack : wg_structure_loot_items(table.name, seed)) {
        CHECK(!stack.empty());
        CHECK(stack.count > 0 && stack.count <= registry.max_stack_size(stack.item));
      }
      if (!stacks.empty())
        ++non_empty_tables;
      total_stacks += stacks.size();
      for (const WgLootStack& stack : stacks) {
        CHECK(!stack.empty());
        CHECK(stack.count > 0);
        const auto item = registry.id_of(stack.item);
        CHECK(item.has_value());
        if (!item)
          continue;
        if (stack.damage > 0) {
          const GeneratedItem* def = registry.by_id(*item);
          CHECK(def != nullptr && stack.damage <= def->max_damage);
        }
        if (stack.potion >= 0)
          CHECK(potion_by_id(stack.potion) != nullptr);
        for (const ItemEnchantment& enchantment : stack.enchantments) {
          CHECK(enchantment.enchantment >= 0 && enchantment.enchantment < kEnchantmentCount);
          CHECK(enchantment.level >= 1);
        }
        for (const MobEffectInstance& effect : stack.custom_effects)
          CHECK(effect.effect >= 0 && mob_effect_by_id(effect.effect) != nullptr);
      }
    }
  }
  CHECK(non_empty_tables >= 120u); // 40+ tables x 3 seeds
  CHECK(total_stacks >= 300u);
}

// The name lookup is a binary search over the generated table; the paths above
// use four names, which is not enough to catch a probe that is ordered the wrong
// way (some names still land). Every row must find itself, and a name outside
// the table must not.
void test_table_lookup_all() {
  for (const WgLootTableRow& table : kWgLootTables)
    CHECK(wg_loot_table_by_name(table.name) == &table);
  CHECK(wg_loot_table_by_name("minecraft:chests/") == nullptr);
  CHECK(wg_loot_table_by_name("") == nullptr);
}

void test_determinism() {
  const std::vector<WgLootStack> first =
      wg_roll_loot_table("minecraft:chests/shipwreck_treasure", 0x1234);
  const std::vector<WgLootStack> second =
      wg_roll_loot_table("minecraft:chests/shipwreck_treasure", 0x1234);
  CHECK_EQ(digest(first), digest(second));
  CHECK(first.size() >= 6u); // rolls 3..6 + 2..5 + 1 + 1, minus empty entries
  CHECK(first.size() <= 13u);

  bool differs = false;
  for (int64_t seed = 1; seed <= 8; ++seed)
    if (digest(wg_roll_loot_table("minecraft:chests/shipwreck_treasure", seed)) != digest(first))
      differs = true;
  CHECK(differs);

  CHECK_EQ(wg_structure_loot_seed(42, 100, 60, 200, "minecraft:chests/shipwreck_treasure"),
           wg_structure_loot_seed(42, 100, 60, 200, "minecraft:chests/shipwreck_treasure"));
  CHECK(wg_structure_loot_seed(42, 100, 60, 200, "minecraft:chests/shipwreck_treasure") !=
        wg_structure_loot_seed(42, 101, 60, 200, "minecraft:chests/shipwreck_treasure"));
}

void test_shipwreck_chests() {
  // Seed 42's ocean ship is at (12, 28), and its pivoted box covers chunks
  // 11..13 x 28..29. Applying the pieces to each of those columns is exactly
  // the bridge's stitched path, so the chest records are produced the way the
  // served world produces them.
  WgOverworldGenerator generator(42);
  size_t chests = 0;
  size_t looted = 0;
  for (int32_t cz = 28; cz <= 29; ++cz) {
    for (int32_t cx = 11; cx <= 13; ++cx) {
      Chunk chunk;
      fill_generated_surface_chunk(generator, cx, cz, chunk);
      apply_generated_structures(generator, chunk);
      for (const nbt::Value& be : chunk.block_entities) {
        const nbt::Value* id = nbt::child(be, "id");
        if (id == nullptr)
          continue;
        const auto name = nbt::as_string(*id);
        if (!name || *name != "minecraft:chest")
          continue;
        const nbt::Value* x = nbt::child(be, "x");
        const nbt::Value* y = nbt::child(be, "y");
        const nbt::Value* z = nbt::child(be, "z");
        CHECK(x != nullptr && y != nullptr && z != nullptr);
        if (x == nullptr || y == nullptr || z == nullptr)
          continue;
        const auto wx = nbt::as_int(*x);
        const auto wy = nbt::as_int(*y);
        const auto wz = nbt::as_int(*z);
        CHECK(wx && wy && wz);
        if (!wx || !wy || !wz)
          continue;
        CHECK(chunk
                  .block_state_at(static_cast<int32_t>(*wx) - cx * 16, static_cast<int32_t>(*wy),
                                  static_cast<int32_t>(*wz) - cz * 16)
                  .name == "minecraft:chest");
        ++chests;
        const std::vector<ItemStack> items = read_block_entity_items(be, 27);
        const size_t non_empty = static_cast<size_t>(std::count_if(
            items.begin(), items.end(), [](const ItemStack& stack) { return !stack.empty(); }));
        CHECK(non_empty > 0);
        if (non_empty > 0)
          ++looted;
      }
    }
  }
  CHECK(chests >= 1u);
  CHECK_EQ(looted, chests);
}

void test_monster_room_block_entities() {
  // Two chunks the feature's own test pins as carrying a monster room at seed
  // 42: (-62, 39) and (-56, 32) each have a spawner, and the pair carries three
  // chests. The block-entity half is what this checks. The feature test pins
  // these on the noise column, before the served bridge's carvers, so this
  // mirrors that path rather than asking the carved bridge for the same room.
  WgOverworldGenerator generator(42);
  size_t spawners = 0;
  size_t chests = 0;
  for (const auto& [cx, cz] : {std::pair<int32_t, int32_t>{-62, 39}, {-56, 32}}) {
    Chunk chunk;
    fill_generated_chunk(generator, cx, cz, chunk);
    decorate_generated_chunk(generator, chunk, nullptr);
    for (const nbt::Value& be : chunk.block_entities) {
      const nbt::Value* id = nbt::child(be, "id");
      if (id == nullptr)
        continue;
      const auto name = nbt::as_string(*id);
      if (!name)
        continue;
      if (*name == "minecraft:spawner") {
        ++spawners;
        const nbt::Value* spawn = nbt::child(be, "SpawnData");
        CHECK(spawn != nullptr);
        if (spawn != nullptr) {
          const nbt::Value* entity = nbt::child(*spawn, "entity");
          CHECK(entity != nullptr);
          if (entity != nullptr) {
            const nbt::Value* entity_id = nbt::child(*entity, "id");
            CHECK(entity_id != nullptr);
            if (entity_id != nullptr) {
              const auto value = nbt::as_string(*entity_id);
              CHECK(value.has_value());
              if (value) {
                CHECK(*value == "minecraft:skeleton" || *value == "minecraft:zombie" ||
                      *value == "minecraft:spider");
              }
            }
          }
        }
      } else if (*name == "minecraft:chest") {
        ++chests;
        const std::vector<ItemStack> items = read_block_entity_items(be, 27);
        const size_t non_empty = static_cast<size_t>(std::count_if(
            items.begin(), items.end(), [](const ItemStack& stack) { return !stack.empty(); }));
        CHECK(non_empty > 0);
      }
    }
  }
  CHECK_EQ(spawners, 2u);
  CHECK_EQ(chests, 3u);
}

} // namespace

int main() {
  test_generated_tables();
  test_table_lookup_all();
  test_determinism();
  test_shipwreck_chests();
  test_monster_room_block_entities();
  std::printf("worldgen-loot: %d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
