// SPDX-License-Identifier: GPL-3.0-or-later
//
// `minecraft:use_remainder` and `minecraft:use_cooldown` (Phase 3 milestone G
// follow-up). See world/use_components.h for the vanilla shape both mirror.
#include "world/use_components.h"

#include <algorithm>

namespace mc::world {
namespace {

// The item's cooldown range, as `UseCooldown.ticks()` reads it. A range with no
// length is not "on cooldown": vanilla divides by the range's duration, so a
// zero-length one yields NaN and `NaN > 0.0f` is false. Returning 0 for it
// reproduces that outcome instead of the NaN.
bool on_cooldown(const int tick_count, const int start, const int end) {
  return end - start > 0 && end > tick_count;
}

} // namespace

RemainderResult convert_into_remainder(const ItemStack& used, int count_before,
                                       bool infinite_materials, int32_t remainder_item,
                                       int remainder_count) {
  RemainderResult result;
  result.hand = used;
  if (infinite_materials)
    return result; // creative: nothing was consumed, so nothing comes back
  if (used.count >= count_before)
    return result; // the stack did not shrink (an unconsumed use)
  const ItemStack remainder =
      ItemStack::of_id(remainder_item, remainder_count > 0 ? remainder_count : 1);
  if (used.empty()) {
    // The whole stack was used up: the remainder takes the hand slot, which is
    // why eating the last stew of a stack leaves you holding a bowl.
    result.hand = remainder;
    return result;
  }
  // There are more of them, so the remainder cannot take the hand slot; vanilla
  // hands it to `handleExtraItemsCreatedOnUse` (ServerPlayer puts it in the
  // inventory, or drops it when that is full).
  result.extra = remainder;
  return result;
}

bool ItemCooldowns::is_on_cooldown(const std::string& group) const {
  for (const auto& entry : cooldowns_)
    if (entry.first == group)
      return on_cooldown(tick_count_, entry.second.start, entry.second.end);
  return false;
}

float ItemCooldowns::percent(const std::string& group, float partial) const {
  for (const auto& entry : cooldowns_) {
    if (entry.first != group)
      continue;
    const float duration = static_cast<float>(entry.second.end - entry.second.start);
    if (duration <= 0.0f)
      return 0.0f;
    const float remaining =
        static_cast<float>(entry.second.end) - (static_cast<float>(tick_count_) + partial);
    return std::clamp(remaining / duration, 0.0f, 1.0f);
  }
  return 0.0f;
}

void ItemCooldowns::add(const std::string& group, int ticks) {
  Entry* existing = find(group);
  if (!existing) {
    cooldowns_.emplace_back(group, Entry{});
    existing = &cooldowns_.back().second;
  }
  existing->start = tick_count_;
  existing->end = tick_count_ + ticks;
}

void ItemCooldowns::remove(const std::string& group) {
  for (auto it = cooldowns_.begin(); it != cooldowns_.end(); ++it) {
    if (it->first == group) {
      cooldowns_.erase(it);
      return;
    }
  }
}

std::vector<std::string> ItemCooldowns::tick() {
  ++tick_count_;
  std::vector<std::string> ended;
  for (auto it = cooldowns_.begin(); it != cooldowns_.end();) {
    if (it->second.end > tick_count_) {
      ++it;
      continue;
    }
    ended.push_back(it->first);
    it = cooldowns_.erase(it);
  }
  return ended;
}

void ItemCooldowns::clear() {
  cooldowns_.clear();
  // The counter is Player's clock, not the map's: `ItemCooldowns.tick` keeps
  // advancing it after a respawn just as before one.
}

ItemCooldowns::Entry* ItemCooldowns::find(const std::string& group) {
  for (auto& entry : cooldowns_)
    if (entry.first == group)
      return &entry.second;
  return nullptr;
}

std::string cooldown_group_of(const GeneratedItem& item) {
  if (item.cooldown_group && *item.cooldown_group)
    return item.cooldown_group;
  return item.name;
}

} // namespace mc::world
