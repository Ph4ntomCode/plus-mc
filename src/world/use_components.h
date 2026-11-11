// SPDX-License-Identifier: GPL-3.0-or-later
//
// The two item components that run *after* a finished use rather than during
// it: `minecraft:use_remainder` and `minecraft:use_cooldown` (Phase 3 milestone
// G follow-up).
//
// Both sit in `ItemStack.applyAfterUseComponentSideEffects`, which
// `ItemStack.finishUsingItem` runs once `Consumable.onConsume` (the food, the
// `on_consume_effects` list, the stack shrinking) has returned:
//
//     ItemStack stackBeforeUse = this.copy();
//     ItemStack result = this.getItem().finishUsingItem(this, level, user);
//     return result.applyAfterUseComponentSideEffects(user, stackBeforeUse);
//
//     if (useRemainder != null) result = useRemainder.convertIntoRemainder(
//         result, stackCountBeforeUsing, user.hasInfiniteMaterials(),
//         user::handleExtraItemsCreatedOnUse);
//     if (useCooldown != null) useCooldown.apply(stackBeforeUsing, user);
//
// Two details of that shape are easy to get wrong and both are modelled here:
// the components are read from the stack as it was *before* the use (so a stack
// that shrank to nothing still knows it had a remainder), and the remainder
// compares the result's count against the pre-use count, which is what makes a
// creative mode use produce no remainder at all.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "world/item.h"
#include "world/item_registry.h"

namespace mc::world {

// --- minecraft:use_remainder -------------------------------------------------

// `UseRemainder.convertIntoRemainder`, as data: the two places the remainder can
// end up. `hand` is what the used hand slot holds afterwards — the used stack
// unchanged, or the remainder when the stack was emptied — and `extra` is a
// remainder that goes to the inventory because the used stack survived (there
// was more than one of it).
//
// `used` is the stack *after* `ItemStack.consume(1, user)`, `count_before` is
// its count before, and `infinite_materials` is `Player.hasInfiniteMaterials()`
// (creative's instabuild). The vanilla branch order matters: creative returns
// first, so a creative player's stew leaves no bowl behind even though the food
// was eaten.
struct RemainderResult {
  ItemStack hand;
  ItemStack extra;
};

RemainderResult convert_into_remainder(const ItemStack& used, int count_before,
                                       bool infinite_materials, int32_t remainder_item,
                                       int remainder_count);

// --- minecraft:use_cooldown --------------------------------------------------

// `ItemCooldowns`: a per-player map from cooldown group to the tick range it is
// locked for, plus the player's own tick counter. The key is the group's
// *identifier* rather than the item, because `ItemCooldowns.getCooldownGroup`
// only falls back to the item's own registry key when the component names no
// group — so two items sharing an explicit group share one cooldown, and the
// group is also what the client is sent (`ServerItemCooldowns.onCooldownStarted`
// -> ClientboundCooldownPacket(group, duration)).
//
// Vanilla keeps this in a HashMap and makes no promise about iteration order; a
// vector in insertion order is used here so the packets a tick produces are
// deterministic (nothing observable depends on it, but a test does).
class ItemCooldowns {
public:
  bool empty() const {
    return cooldowns_.empty();
  }
  // `ItemCooldowns.tickCount`, which the percent is measured against.
  int tick_count() const {
    return tick_count_;
  }

  // `isOnCooldown`: `getCooldownPercent(item, 0.0f) > 0.0f`.
  bool is_on_cooldown(const std::string& group) const;
  // `getCooldownPercent`: the fraction of the cooldown still to run, clamped to
  // [0, 1]; 0 for a group that is not on cooldown. `partial` is the fraction of
  // a tick the client is interpolating over.
  float percent(const std::string& group, float partial) const;

  // `addCooldown(group, time)`: the range is [now, now + time), and the entry
  // replaces any range the group already had.
  void add(const std::string& group, int ticks);
  void remove(const std::string& group);
  // `Player.tick`'s `cooldowns.tick()`: advances the counter and drops every
  // range that has run out, *returning* the groups it dropped so the caller can
  // send each one its zero-duration packet (`ServerItemCooldowns
  // .onCooldownEnded`). The returned order is the map's own, i.e. insertion
  // order (onCooldownEnded is called while the entry is being erased).
  std::vector<std::string> tick();
  // A fresh cooldown map, which is what respawning gives the player:
  // `ServerPlayer.restoreFrom` copies the attributes, the inventory and the
  // effects, and does not copy the cooldowns.
  void clear();

private:
  struct Entry {
    int start = 0;
    int end = 0;
  };
  Entry* find(const std::string& group);

  std::vector<std::pair<std::string, Entry>> cooldowns_;
  int tick_count_ = 0;
};

// `ItemCooldowns.getCooldownGroup`: the component's own group when it names one,
// otherwise the item's registry key — which is exactly the generated table's
// `name` (the same string the report's registry dump and the client agree on).
std::string cooldown_group_of(const GeneratedItem& item);

} // namespace mc::world
