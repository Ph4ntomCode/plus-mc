// SPDX-License-Identifier: GPL-3.0-or-later
//
// `minecraft:consumable`'s `on_consume_effects` list (Phase 3 milestone G
// follow-up).
//
// In 26.2 eating runs `Item.finishUsingItem` -> `Consumable.onConsume`, which
// fires the effects in a fixed order:
//
//   1. the item's own `ConsumableListener` components — for food that is
//      `FoodProperties.onConsume`, i.e. `FoodData.eat` (which is why the
//      nutrition lands *before* a poisonous potato's poison);
//   2. `on_consume_effects`, the `ConsumeEffect` list this module models;
//   3. `ItemStack.consume(1, user)`, the stack shrinking by one.
//
// The list has five kinds and the engine's boundary is drawn per kind, not per
// item: three of them are operations on the effect set this server already has
// (`apply_effects`, `remove_effects`, `clear_all_effects`), one needs the world
// (`teleport_randomly`, the chorus fruit hop — `random_teleport` below) and one
// cannot be modelled at all (`play_sound`: this server has no sound registry
// and sends no sound packets, so an ominous bottle is silent by construction
// rather than by omission).
//
// The effects an `apply_effects` row grants are *copies* of the row's
// instances, exactly as `new MobEffectInstance(effect)` copies them, so eating
// two golden apples grants the same instance both times and a copy cannot mutate
// the table.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "world/consume_effect_table.h"
#include "world/effect.h"

namespace mc::world {

// The `on_consume_effects` rows for one item, in the order the item declares
// them. Returns the count and points `first` at the item's first row; `first` is
// set to nullptr and 0 is returned for an item with no list.
//
// The table is generated sorted by item id with an item's rows adjacent, so the
// rows for one item are `first[0 .. count)` — the unit test asserts that
// adjacency, because the walk here depends on it.
int consume_effects_for_item(int32_t item, const GeneratedConsumeEffect** first);

// ApplyStatusEffectsConsumeEffect.apply's first line: vanilla refuses the whole
// effect when `user.getRandom().nextFloat() >= probability`, so nothing lands at
// all — not a short-lived instance, not a partial list. `unit` is one float from
// the caller's RandomSource, in [0, 1). Only meaningful for an apply_effects
// row; every other kind's `value` is not a probability.
bool consume_effect_fires(const GeneratedConsumeEffect& effect, double unit);

// ApplyStatusEffectsConsumeEffect.apply: `unit` gates the list, then each entry
// lands through `LivingEntity.addEffect` (so an effect already running is
// merged rather than duplicated). Returns whether any of them landed, which is
// vanilla's `anyApplied`; the packets are the caller's business because the
// instances' side effects and the attribute refresh are session state.
bool apply_consume_status_effects(const GeneratedConsumeEffect& effect, double unit,
                                  ActiveEffects& effects, EffectOutcome& out);

// RemoveStatusEffectsConsumeEffect.apply: `removeEffect` per effect the row
// names, in the order it names them. `removed` collects the ids that were
// actually active — one remove_mob_effect each, which is what
// `LivingEntity.onEffectsRemoved` sends. Returns whether any of them landed.
bool remove_consume_status_effects(const GeneratedConsumeEffect& effect, ActiveEffects& effects,
                                   std::vector<int32_t>* removed);

// ClearAllStatusEffectsConsumeEffect.apply -> `LivingEntity.removeAllEffects`,
// which reports everything it takes away and *then* empties the set. `removed`
// is filled before the clear because the ids are what the packets carry.
bool clear_all_consume_effects(ActiveEffects& effects, std::vector<int32_t>* removed);

// --- TeleportRandomlyConsumeEffect (the chorus fruit hop) --------------------

// `LivingEntity.randomTeleport` reads the level three ways, and none of them
// lives in this module: the two predicates below are the caller's world, and
// `min_y` is `Level.getMinY()`.
struct RandomTeleportQuery {
  // `BlockState.blocksMotion()`: the block that ends the fall.
  std::function<bool(int, int, int)> blocks_motion;
  // `Level.containsAnyLiquid(box)`: fluid anywhere in the entity's box refuses
  // the landing even when nothing solid is in the way.
  std::function<bool(int, int, int)> liquid;
  int min_y = -64;
};

struct RandomTeleportTarget {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

// `LivingEntity.randomTeleport(xx, yy, zz, showParticles)` minus the entity and
// the particles: `x`/`y`/`z` are the candidate `TeleportRandomlyConsumeEffect`
// rolled from the player's random source, and on success `out` receives where
// the entity lands. Returns false when the candidate has no floor under it or
// the landing box is obstructed, in which case the caller leaves the entity
// exactly where it was — randomTeleport re-teleports to the old position, which
// is a no-op here because nothing moved yet.
//
// The search is vanilla's: start at the candidate's own block, walk down until
// the block below stops motion (`Level.getMinY()` is the floor of the search),
// then land *on* it. What vanilla additionally reads that this cannot is
// `Level.hasChunkAt(pos)`: this server's world answers every coordinate and
// generates terrain on demand, so there is no unloaded position to refuse.
bool random_teleport(const RandomTeleportQuery& query, double x, double y, double z,
                     double half_width, double height, RandomTeleportTarget* out);

} // namespace mc::world
