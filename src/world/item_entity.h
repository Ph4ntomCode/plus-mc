// SPDX-License-Identifier: GPL-3.0-or-later
//
// Dropped items as server entities (Phase 4 follow-up: the block-break drop).
//
// Until this landed a block drop was *spawn-and-forget*: `spawn_item_drop` built
// an Add Entity and a Set Entity Data packet, broadcast them, and kept nothing.
// The client drew an item that did not exist — it never fell, never expired and,
// the one a player notices immediately, could never be picked up. A survival
// world was therefore unplayable past the first block: break stone and the
// cobblestone sat there forever.
//
// Vanilla's ItemEntity is a server entity, and this is a port of it (all line
// references from the decompiled 26.2 `ItemEntity.java` / `Entity.java`):
//
//   * gravity 0.04 (`getDefaultGravity`, line 123) and air drag 0.98
//     (`Entity.getAirDrag`, line 1468), the latter multiplied by the friction of
//     the block below (0.6 for every ordinary block) while on the ground;
//   * the move gate at line 149: an item only runs `move` when it is airborne,
//     still sliding (`horizontalDistanceSqr() > 1.0E-5`), or on every fourth
//     tick (`(tickCount + id) % 4 == 0`). A resting item therefore stops being
//     re-moved — the rule that keeps a field of drops from costing anything;
//   * the ground bounce at line 158: `if (movement.y < 0) multiply(1, -0.5, 1)`,
//     which is what makes a drop hop once instead of sinking;
//   * the merge scan (`mergeWithNeighbours`), due on `tickCount % rate == 0`
//     with `rate` 2 when this tick's move crossed a block boundary and 40
//     otherwise: neighbours inside the box inflated by (0.5, 0.0, 0.5), merged
//     when `areMergable` holds — same item *and* same components, and the two
//     counts must fit one stack — which `setItem` tells the clients about (the
//     surviving entity's DATA_ITEM is dirty);
//   * the 6000-tick lifetime (line 52 `LIFETIME`, discarded at line 187);
//   * the pickup (line 337 `playerTouch`): a zero pickup delay, the inventory
//     accepting the stack, and only then the entity shrinking and vanishing.
//
// **What a drop is picked up by is not the item's own box.** Vanilla's
// `Player.aiStep` (line 447) builds a pickup area — the player's box inflated by
// (1.0, 0.5, 1.0) — and calls `touch()` on every entity inside it. That is a
// generous reach (about one and a half blocks beyond the player's 0.6-wide box,
// half a block up), and it is the reason a player walking over a drop collects it
// without having to stand on the exact pixel. The test here is therefore
// *overlap against the inflated player box*, not overlap of two tight boxes.
//
// The one thing this engine cannot ask is `ItemStack.isSameItemSameComponents`:
// an `ItemStack` here carries the components it models (damage, enchantments,
// potion contents) and drops none of the rest, so "same components" is the
// `same_item` comparison on the stack itself. Two drops that differ only in a
// component this engine does not model merge here and would not in vanilla; that
// is a documented limit, not an approximation hidden inside the rule.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "world/item.h"

namespace mc::world {

// ItemEntity's constants.
constexpr double kItemGravity = 0.04; // getDefaultGravity
constexpr double kItemAirDrag = 0.98; // Entity.getAirDrag
// getAirDrag * the block friction of the block below. Vanilla reads the block's
// own `getFriction()`; 0.6 is the default every ordinary block uses and there is
// no per-block friction table here (the same stand-in the orbs make).
constexpr double kItemGroundDrag = 0.98 * 0.6;
constexpr int kItemLifetime = 6000; // LIFETIME
// EntityType.ITEM is `sized(0.25f, 0.25f)` (EntityTypes.java:236).
constexpr double kItemWidth = 0.25;
constexpr double kItemHeight = 0.25;
// The ground bounce: `movement.multiply(1.0, -0.5, 1.0)`.
constexpr double kItemBounce = -0.5;
// The merge scan's box: `getBoundingBox().inflate(0.5, 0.0, 0.5)`.
constexpr double kItemMergeInflateXZ = 0.5;
// Player.aiStep's pickup area: `getBoundingBox().inflate(1.0, 0.5, 1.0)`.
constexpr double kItemPickupInflateXZ = 1.0;
constexpr double kItemPickupInflateY = 0.5;
// The player's own box, for the pickup-area test.
constexpr double kPlayerWidth = 0.6;
constexpr double kPlayerHeight = 1.8;

// One live dropped item. `x/y/z` is the entity position (the feet-centred point
// `Entity.setPos` uses), `vx/vy/vz` the delta movement, `age` the tick count the
// lifetime and the move gate read.
struct ItemEntity {
  int32_t entity_id = 0;
  double x = 0.0, y = 0.0, z = 0.0;
  double vx = 0.0, vy = 0.0, vz = 0.0;
  ItemStack stack; // DATA_ITEM
  int age = 0;     // Entity.tickCount / ItemEntity.age
  // ItemEntity.pickupDelay: the spawn site's starting value — 10 for a block's
  // `popResource`, 40 for a player's `drop`, none for a container's spill.
  int pickup_delay = 0;
  bool on_ground = false;
  bool removed = false; // discarded: lifetime, merge or a full pickup
  // `ItemEntity.setItem` -> `entityData.set(DATA_ITEM, stack)` marks the synched
  // value dirty, so a merge's survivor is re-sent to its viewers. A partial
  // pickup (the inventory took some but not all) is the same write, and the
  // session thread cannot send it itself without reaching into this registry;
  // the next tick's `data_changed` carries both.
  bool data_dirty = false;
};

// One player, as a dropped item sees them. `x/y/z` is the feet position from the
// movement packets; `can_pick_up` is false for a spectator or a dead player
// (`Player.aiStep`'s `getHealth() > 0 && !isSpectator()` gate).
struct ItemPlayer {
  int32_t entity_id = 0;
  double x = 0.0, y = 0.0, z = 0.0;
  bool can_pick_up = true;
};

// The world a dropped item ticks in. `is_solid` reports whether the block at
// those world coordinates stops a falling item, and may be empty (the void world
// has nothing to land on, so drops fall until they expire).
struct ItemEnvironment {
  std::vector<ItemPlayer> players;
  std::function<bool(int, int, int)> is_solid;
};

// An axis-aligned box.
struct ItemBox {
  double min_x = 0.0, min_y = 0.0, min_z = 0.0;
  double max_x = 0.0, max_y = 0.0, max_z = 0.0;
};

// One pickup: `player_entity_id` is touching this item entity, which holds
// `stack`. The caller is the one that can decide whether the stack fits (the
// inventory lives on the session thread), so this is a *request*: it answers with
// how many items the player accepted, and only then does the entity shrink. A
// pickup of 0 leaves the entity untouched, which is exactly vanilla's
// `player.getInventory().add(itemStack)` returning false.
struct ItemPickup {
  int32_t item_entity_id = 0;
  int32_t player_entity_id = 0;
  ItemStack stack;
};

// What one tick of the whole item population produced. `moved` are the items
// whose position changed (they need a position sync), `data_changed` the items
// whose stack changed without moving (a merge's survivor, or a partial pickup
// resolved between ticks) and so need a Set Entity Data, `removed` the entities
// that went away without a pickup (lifetime or merge) and need a despawn, and
// `pickups` the touches that want the session's inventory to accept a stack.
struct ItemTickReport {
  std::vector<ItemPickup> pickups;
  std::vector<int32_t> moved;
  std::vector<int32_t> data_changed;
  std::vector<int32_t> removed;
};

// ItemEntity's constructor's launch: `random.nextDouble() * 0.2 - 0.1` on x and
// z, a flat 0.2 on y. Vanilla draws only two draws, so the caller owns the RNG.
void item_launch(ItemEntity& item, double x_draw, double z_draw);

// The item's own box (0.25 x 0.25) and the player's pickup area (the player's
// 0.6 x 1.8 box inflated by 1.0/0.5/1.0).
ItemBox item_box(const ItemEntity& item);
ItemBox item_pickup_area(const ItemPlayer& player);
bool item_box_overlaps(const ItemBox& a, const ItemBox& b);

// ItemEntity.areMergable: the two counts must fit one stack — `max_stack_size`
// is the item's own MAX_STACK_SIZE component, read off the stack, so a 16-max
// item stops merging at 16 — and the stacks must be the same item with the same
// components.
bool items_can_merge(const ItemEntity& a, const ItemEntity& b);

// ItemEntity.merge: the counts add into the larger stack (or `into` on a tie),
// the older age wins, the surviving stack is marked `data_dirty`, and the
// absorbed entity is discarded. Returns true when the merge happened.
bool item_merge_into(ItemEntity& into, ItemEntity& other);

// Advances `items` by one tick: gravity, the move gate, the move with collision,
// the drag and bounce, the merge scan, the pickup touches and the lifetime — the
// order of `ItemEntity.tick` itself. Items discarded during the tick are erased;
// the report says what to tell the clients.
//
// A `pickups` entry is *not* a completed pickup: the caller resolves it against
// the player's inventory and reports back how many items were taken (see
// `item_apply_pickup`), because only the session thread can touch an inventory.
ItemTickReport tick_item_entities(std::vector<ItemEntity>& items, const ItemEnvironment& env);

// Resolves one `ItemPickup` after the inventory has accepted `accepted` items:
// the entity shrinks by that many and is discarded when it empties; a stack that
// survives a partial pickup is marked `data_dirty` (its new count has to reach
// the clients, and `take_item_entity` alone only carries what was taken).
// Returns true when the entity was removed by this pickup.
bool item_apply_pickup(std::vector<ItemEntity>& items, int32_t entity_id, int accepted);

} // namespace mc::world
