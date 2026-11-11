// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/loot.h"

#include <cstring>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "world/block_drops.h"
#include "world/mob_loot_table.h"

namespace mc::world {

namespace {

// Requirement strings are canonical "k=v,k=v" (the same form BlockStateKey
// uses). Every pair in `requires` must be present with that value in the
// block's actual properties.
bool requirements_match(const char* required, const std::string& properties) {
  if (required == nullptr || *required == '\0')
    return true;
  const std::string_view req(required);
  size_t pos = 0;
  while (pos < req.size()) {
    size_t comma = req.find(',', pos);
    const std::string_view pair =
        comma == std::string_view::npos ? req.substr(pos) : req.substr(pos, comma - pos);
    // Look for the same "key=value" token inside the actual properties, at a
    // pair boundary (so "type=double" never matches "subtype=double").
    size_t search = 0;
    bool found = false;
    while (search <= properties.size()) {
      size_t hit = properties.find(pair, search);
      if (hit == std::string::npos)
        break;
      const bool at_start = hit == 0 || properties[hit - 1] == ',';
      const size_t end = hit + pair.size();
      const bool at_end = end == properties.size() || properties[end] == ',';
      if (at_start && at_end) {
        found = true;
        break;
      }
      search = hit + 1;
    }
    if (!found)
      return false;
    if (comma == std::string_view::npos)
      break;
    pos = comma + 1;
  }
  return true;
}

struct RuleRange {
  const GeneratedBlockDrop* begin = nullptr;
  const GeneratedBlockDrop* end = nullptr;
  int16_t first_pool = 0;
};

const std::unordered_map<std::string, RuleRange>& drop_index() {
  // kBlockDrops is sorted by (block, pool, sequence), so a block's rules are
  // one contiguous range.
  static const std::unordered_map<std::string, RuleRange> index = [] {
    std::unordered_map<std::string, RuleRange> map;
    map.reserve(2048);
    int i = 0;
    while (i < kBlockDropCount) {
      const std::string block(kBlockDrops[i].block);
      RuleRange range;
      range.begin = &kBlockDrops[i];
      range.first_pool = kBlockDrops[i].pool;
      while (i < kBlockDropCount && block == kBlockDrops[i].block)
        ++i;
      range.end = &kBlockDrops[i];
      map.emplace(block, range);
    }
    return map;
  }();
  return index;
}

const std::unordered_map<std::string, int>& tool_index() {
  static const std::unordered_map<std::string, int> index = [] {
    std::unordered_map<std::string, int> map;
    map.reserve(kBlockToolRequirementCount * 4);
    for (const GeneratedBlockToolRequirement& entry : kBlockToolRequirements)
      map.emplace(entry.block, entry.min_tier);
    return map;
  }();
  return index;
}

} // namespace

int held_pickaxe_tier(const std::string& held_item) {
  // Only pickaxes matter: every needs_* block is mineable/pickaxe (asserted by
  // the generator), so a non-pickaxe is tier 0.
  struct Tier {
    const char* prefix;
    int tier;
  };
  static constexpr Tier kTiers[] = {
      {"minecraft:wooden_pickaxe", 1},  {"minecraft:golden_pickaxe", 1},
      {"minecraft:stone_pickaxe", 2},   {"minecraft:iron_pickaxe", 3},
      {"minecraft:diamond_pickaxe", 4}, {"minecraft:netherite_pickaxe", 5},
  };
  for (const Tier& t : kTiers) {
    if (held_item == t.prefix)
      return t.tier;
  }
  return 0;
}

int block_min_pickaxe_tier(const std::string& block_name) {
  const auto it = tool_index().find(block_name);
  return it == tool_index().end() ? 0 : it->second;
}

std::vector<BlockDrop> resolve_block_drops(const BlockStateKey& state,
                                           const std::string& held_item) {
  if (state.name == "minecraft:air" || state.name == "minecraft:cave_air" ||
      state.name == "minecraft:void_air")
    return {};

  // Tool gate: vanilla only drops tool-gated blocks to the right pickaxe.
  const int required = block_min_pickaxe_tier(state.name);
  if (required > 0 && held_pickaxe_tier(held_item) < required)
    return {};

  const auto it = drop_index().find(state.name);
  if (it == drop_index().end())
    return {};
  const RuleRange& range = it->second;

  std::vector<BlockDrop> out;
  int16_t current_pool = -1;
  const GeneratedBlockDrop* current = nullptr;
  for (const GeneratedBlockDrop* rule = range.begin; rule != range.end; ++rule) {
    if (rule->pool != current_pool) {
      current_pool = rule->pool;
      current = nullptr;
    }
    if (current != nullptr)
      continue; // this pool already produced its drop
    // Chance-based drops are the "secondary" outcome (5% sapling, 10% flint,
    // fortune bonuses); only the block's first pool may use them, so the
    // guaranteed drops of later pools are not lost to a lucky roll.
    if ((rule->flags & kDropFlagChance) != 0 && rule->pool != range.first_pool)
      continue;
    if (!requirements_match(rule->requires_state, state.properties))
      continue;
    int32_t count = rule->count;
    if (rule->alt_count > 0 && requirements_match(rule->alt_requires, state.properties))
      count = rule->alt_count;
    if (count <= 0)
      count = 1;
    current = rule;
    out.push_back(BlockDrop{rule->item, count});
  }
  return out;
}

std::vector<BlockDrop> resolve_block_drops_rolled(const BlockStateKey& state,
                                                  const std::string& held_item,
                                                  const std::function<double()>& roll) {
  if (state.name == "minecraft:air" || state.name == "minecraft:cave_air" ||
      state.name == "minecraft:void_air")
    return {};

  const int required = block_min_pickaxe_tier(state.name);
  if (required > 0 && held_pickaxe_tier(held_item) < required)
    return {};

  const auto it = drop_index().find(state.name);
  if (it == drop_index().end())
    return {};
  const RuleRange& range = it->second;

  // Every pool rolls, and a chance rule rolls its own draw. Within a pool the
  // first rule whose state requirements match decides it (the generator emits
  // vanilla's alternatives order, certain entries before chance ones), and a
  // chance rule whose draw fails leaves that pool empty.
  std::vector<BlockDrop> out;
  int16_t current_pool = -1;
  const GeneratedBlockDrop* current = nullptr;
  for (const GeneratedBlockDrop* rule = range.begin; rule != range.end; ++rule) {
    if (rule->pool != current_pool) {
      current_pool = rule->pool;
      current = nullptr;
    }
    if (current != nullptr)
      continue;
    if (!requirements_match(rule->requires_state, state.properties))
      continue;
    if ((rule->flags & kDropFlagChance) != 0 &&
        (!roll || roll() >= static_cast<double>(rule->chance)))
      continue;
    int32_t count = rule->count;
    if (rule->alt_count > 0 && requirements_match(rule->alt_requires, state.properties))
      count = rule->alt_count;
    if (count <= 0)
      count = 1;
    current = rule;
    out.push_back(BlockDrop{rule->item, count});
  }
  return out;
}

std::vector<BlockDrop> resolve_mob_drops(const std::string& mob_name, bool killed_by_player,
                                         const std::function<double()>& roll) {
  std::vector<BlockDrop> out;
  for (int i = 0; i < kMobDropCount; ++i) {
    const GeneratedMobDrop& rule = kMobDrops[i];
    if (mob_name != rule.mob)
      continue;
    if (rule.player_kill && !killed_by_player)
      continue;
    // `rolls_min`/`rolls_max` repeat the whole draw: one for every entity pool
    // but the witch's 1..3 (see the generated table's header). Each repetition
    // re-rolls the pool's conditions, which is how vanilla's `LootPool` works.
    int rolls = static_cast<int>(rule.rolls_min);
    const int roll_span = static_cast<int>(rule.rolls_max) - rolls + 1;
    if (roll_span > 1) {
      const double unit = roll ? roll() : 0.0;
      int offset = static_cast<int>(unit * static_cast<double>(roll_span));
      if (offset >= roll_span)
        offset = roll_span - 1;
      if (offset < 0)
        offset = 0;
      rolls += offset;
    }
    // One of the pool's candidate items, chosen uniformly (every vanilla entity
    // entry has weight 1). Six is the widest pool in the game (the witch's
    // glowstone/sugar/redstone/stick/gunpowder/spider-eye draw).
    const char* candidates[6] = {rule.item,  rule.item2, rule.item3,
                                 rule.item4, rule.item5, rule.item6};
    int count = 0;
    for (const char* candidate : candidates)
      if (candidate != nullptr)
        ++count;
    if (count == 0)
      continue;
    for (int repetition = 0; repetition < rolls; ++repetition) {
      if (rule.chance < 1.0f && (!roll || roll() >= static_cast<double>(rule.chance)))
        continue;
      int pick = 0;
      if (count > 1) {
        const double unit = roll ? roll() : 0.0;
        pick = static_cast<int>(unit * static_cast<double>(count));
        if (pick >= count)
          pick = count - 1;
        if (pick < 0)
          pick = 0;
      }
      const int span = static_cast<int>(rule.max_count) - static_cast<int>(rule.min_count) + 1;
      int amount = static_cast<int>(rule.min_count);
      if (span > 1) {
        const double unit = roll ? roll() : 0.0;
        int offset = static_cast<int>(unit * static_cast<double>(span));
        if (offset >= span)
          offset = span - 1;
        if (offset < 0)
          offset = 0;
        amount += offset;
      }
      // A non-positive roll drops nothing (vanilla's set_count can produce 0,
      // and the spider's eye pool rolls a uniform -1..1).
      if (amount <= 0)
        continue;
      const char* chosen = nullptr;
      int seen = 0;
      for (const char* candidate : candidates) {
        if (candidate == nullptr)
          continue;
        if (seen++ == pick) {
          chosen = candidate;
          break;
        }
      }
      if (chosen != nullptr)
        out.push_back(BlockDrop{chosen, amount});
    }
  }
  return out;
}

} // namespace mc::world
