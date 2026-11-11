// SPDX-License-Identifier: GPL-3.0-or-later
//
// Dropped items: the simulation behind world/item_entity.h. Every constant and
// every branch is drawn from the decompiled 26.2 `ItemEntity` and its parents;
// the comment on each block names the vanilla member it mirrors.
#include "world/item_entity.h"

#include <algorithm>
#include <cmath>

#include "world/axis_step.h"

namespace mc::world {
namespace {

// The player's bounding box, for the pickup area. `Player.aiStep` inflates this
// by (1.0, 0.5, 1.0) — see kItemPickupInflate*.
constexpr double kPlayerHalfWidth = 0.3;

// ItemEntity.getDefaultGravity / Entity.getAirDrag / the ground friction.
// ItemEntity also has a `noPhysics` unstuck sweep (`moveTowardsClosestSpace`,
// line 144) which this engine has no VoxelShape for; a drop spawns in the block
// it came from, whose centre is free space, so it is not needed here.

// The cells a box covers, so a box test is a handful of block lookups.
bool box_hits_block(const ItemBox& box, int bx, int by, int bz) {
  return box.min_x < static_cast<double>(bx) + 1.0 && box.max_x > static_cast<double>(bx) &&
         box.min_y < static_cast<double>(by) + 1.0 && box.max_y > static_cast<double>(by) &&
         box.min_z < static_cast<double>(bz) + 1.0 && box.max_z > static_cast<double>(bz);
}

// Substitutes for `!level.noCollision(box)`: does the box overlap a block the
// world calls solid? A world with no solidity probe reports "no collision", which
// is what lets a drop fall through the void world for its whole lifetime.
bool box_blocked(const ItemEnvironment& env, double x, double y, double z) {
  if (!env.is_solid)
    return false;
  const ItemBox box = ItemBox{.min_x = x - kItemWidth / 2.0,
                              .min_y = y,
                              .min_z = z - kItemWidth / 2.0,
                              .max_x = x + kItemWidth / 2.0,
                              .max_y = y + kItemHeight,
                              .max_z = z + kItemWidth / 2.0};
  const int x0 = static_cast<int>(std::floor(box.min_x));
  const int x1 = static_cast<int>(std::ceil(box.max_x)) - 1;
  const int y0 = static_cast<int>(std::floor(box.min_y));
  const int y1 = static_cast<int>(std::ceil(box.max_y)) - 1;
  const int z0 = static_cast<int>(std::floor(box.min_z));
  const int z1 = static_cast<int>(std::ceil(box.max_z)) - 1;
  for (int bx = x0; bx <= x1; ++bx)
    for (int by = y0; by <= y1; ++by)
      for (int bz = z0; bz <= z1; ++bz)
        if (box_hits_block(box, bx, by, bz) && env.is_solid(bx, by, bz))
          return true;
  return false;
}

// One axis of Entity.move's collision handling, block-granular: the item either
// completes the step, or stops at the face it hit.
void move_axis(ItemEntity& item, const ItemEnvironment& env, int axis) {
  const double before = axis == 0 ? item.x : (axis == 1 ? item.y : item.z);
  const double delta = axis == 0 ? item.vx : (axis == 1 ? item.vy : item.vz);
  const double after = before + delta;
  auto blocked_at = [&](double value) {
    return axis == 0 ? box_blocked(env, value, item.y, item.z)
                     : (axis == 1 ? box_blocked(env, item.x, value, item.z)
                                  : box_blocked(env, item.x, item.y, value));
  };
  if (!blocked_at(after)) {
    if (axis == 0)
      item.x = after;
    else if (axis == 1)
      item.y = after;
    else
      item.z = after;
    return;
  }
  // Blocked: stand the item on the face it reached for (a downward step lands on
  // the floor's top face) and lose the axis speed. ItemEntity.tick's bounce
  // (`movement.y < 0 -> multiply(1, -0.5, 1)`) is applied by the caller, which
  // can see that this was the vertical axis and the speed was downward.
  if (axis == 1) {
    if (delta < 0.0) {
      const double surface = std::floor(after) + 1.0;
      if (!blocked_at(surface)) {
        item.y = surface;
        item.on_ground = true;
        item.vy = 0.0;
        return;
      }
    } else if (delta > 0.0) {
      // The underside of a block: stop just below it rather than inside it.
      const double ceiling = std::ceil(after) - 1.0 - kItemHeight;
      if (!blocked_at(ceiling)) {
        item.y = ceiling;
        item.vy = 0.0;
        return;
      }
    }
  }
  if (axis == 0)
    item.vx = 0.0;
  else if (axis == 1)
    item.vy = 0.0;
  else
    item.vz = 0.0;
}

} // namespace

void item_launch(ItemEntity& item, double x_draw, double z_draw) {
  // ItemEntity's constructor: `random.nextDouble() * 0.2 - 0.1` horizontally, a
  // flat 0.2 up. (The orbs' is the same expression doubled and drawn per axis;
  // the item's is not — this is the item's own line, and it draws only twice.)
  item.vx = x_draw * 0.2 - 0.1;
  item.vy = 0.2;
  item.vz = z_draw * 0.2 - 0.1;
}

ItemBox item_box(const ItemEntity& item) {
  return ItemBox{.min_x = item.x - kItemWidth / 2.0,
                 .min_y = item.y,
                 .min_z = item.z - kItemWidth / 2.0,
                 .max_x = item.x + kItemWidth / 2.0,
                 .max_y = item.y + kItemHeight,
                 .max_z = item.z + kItemWidth / 2.0};
}

ItemBox item_pickup_area(const ItemPlayer& player) {
  return ItemBox{.min_x = player.x - kPlayerHalfWidth - kItemPickupInflateXZ,
                 .min_y = player.y - kItemPickupInflateY,
                 .min_z = player.z - kPlayerHalfWidth - kItemPickupInflateXZ,
                 .max_x = player.x + kPlayerHalfWidth + kItemPickupInflateXZ,
                 .max_y = player.y + kPlayerHeight + kItemPickupInflateY,
                 .max_z = player.z + kPlayerHalfWidth + kItemPickupInflateXZ};
}

bool item_box_overlaps(const ItemBox& a, const ItemBox& b) {
  return a.min_x < b.max_x && a.max_x > b.min_x && a.min_y < b.max_y && a.max_y > b.min_y &&
         a.min_z < b.max_z && a.max_z > b.min_z;
}

bool items_can_merge(const ItemEntity& a, const ItemEntity& b) {
  if (a.removed || b.removed)
    return false;
  if (a.stack.empty() || b.stack.empty())
    return false;
  // ItemEntity.areMergable: the two counts must fit one stack. The limit is the
  // stack's own MAX_STACK_SIZE component (`ItemStack.getMaxStackSize()`), not a
  // population-wide constant: a 16-max item (a snowball) must not merge past 16
  // just because a 64-max item can.
  if (a.stack.count + b.stack.count > b.stack.max_stack_size())
    return false;
  // ...and the stacks must be the same item with the same components.
  return a.stack.same_item(b.stack);
}

bool item_merge_into(ItemEntity& into, ItemEntity& other) {
  if (&into == &other || !items_can_merge(into, other))
    return false;
  into.stack.count += other.stack.count;
  // `toItem.setItem(newToStack)`: the survivor's DATA_ITEM is dirty, so every
  // viewer is sent the new count.
  into.data_dirty = true;
  // `toItem.age = Math.min(toItem.age, fromItem.age)` — the older age wins, so a
  // fresh drop merged into an old one does not extend the old one's life.
  into.age = std::min(into.age, other.age);
  into.pickup_delay = std::max(into.pickup_delay, other.pickup_delay);
  other.removed = true;
  return true;
}

namespace {

// ItemEntity.mergeWithNeighbours: neighbours inside the item's box inflated by
// (0.5, 0.0, 0.5), of which the *smaller* stack is absorbed by the larger (or, on
// a tie, the one being scanned absorbs the other).
void scan_for_merges(std::vector<ItemEntity>& items, size_t index) {
  ItemEntity& self = items[index];
  const ItemBox mine = item_box(self);
  for (size_t j = 0; j < items.size(); ++j) {
    if (j == index || items[j].removed || self.removed)
      continue;
    const ItemBox theirs = item_box(items[j]);
    const ItemBox inflated{.min_x = mine.min_x - kItemMergeInflateXZ,
                           .min_y = mine.min_y,
                           .min_z = mine.min_z - kItemMergeInflateXZ,
                           .max_x = mine.max_x + kItemMergeInflateXZ,
                           .max_y = mine.max_y,
                           .max_z = mine.max_z + kItemMergeInflateXZ};
    if (!item_box_overlaps(inflated, theirs))
      continue;
    // `otherItemStack.getCount() < thisItemStack.getCount()` picks the target:
    // the bigger stack wins, ties go to the entity being scanned.
    if (items[j].stack.count < self.stack.count)
      item_merge_into(self, items[j]);
    else
      item_merge_into(items[j], self);
  }
}

} // namespace

ItemTickReport tick_item_entities(std::vector<ItemEntity>& items, const ItemEnvironment& env) {
  ItemTickReport report;
  if (items.empty())
    return report;

  for (size_t i = 0; i < items.size(); ++i) {
    ItemEntity& item = items[i];
    if (item.removed)
      continue;

    // `super.tick()` (Entity.baseTick) runs before ItemEntity's own body and is
    // what advances tickCount; ItemEntity.age advances later in the body. The
    // two are therefore equal inside the body, and every gate below reads that
    // advanced count.
    ++item.age;
    if (item.pickup_delay > 0)
      --item.pickup_delay; // line 130
    // The lifetime is checked at the end of the body in vanilla; discarding here
    // is the same tick, because nothing may move an item that is about to
    // expire. `age >= 6000 -> discard()`.
    if (item.age >= kItemLifetime) {
      item.removed = true;
      continue;
    }

    // `xo/yo/zo`: the position the merge gate below compares against to decide
    // whether this tick's move crossed a block boundary.
    const double start_x = item.x, start_y = item.y, start_z = item.z;

    // `noPhysics = !level.noCollision(box)`: an item inside a block does not
    // accumulate gravity, which is what lets it rest on the floor.
    if (box_blocked(env, item.x, item.y, item.z))
      item.on_ground = true;
    else {
      item.on_ground = false;
      item.vy -= kItemGravity; // Entity.applyGravity -> getDefaultGravity() = 0.04
    }

    // The move gate (line 149): only an item that is airborne, still sliding, or
    // on its every-fourth tick runs `move` at all. A resting item is skipped
    // entirely — this is what keeps a field of drops cheap.
    const double horizontal_sqr = item.vx * item.vx + item.vz * item.vz;
    const bool move_due =
        !item.on_ground || horizontal_sqr > 1.0E-5 || (item.age + item.entity_id) % 4 == 0;
    if (move_due) {
      int step_order[3];
      axis_step_order(item.vx, item.vz, step_order);
      move_axis(item, env, step_order[0]);
      move_axis(item, env, step_order[1]);
      move_axis(item, env, step_order[2]);

      // `friction = getAirDrag()` (x the block friction when on the ground),
      // then `setDeltaMovement(getDeltaMovement().multiply(friction, airDrag,
      // friction))`.
      const double friction = item.on_ground ? kItemGroundDrag : kItemAirDrag;
      item.vx *= friction;
      item.vy *= kItemAirDrag;
      item.vz *= friction;

      // The ground bounce (line 158): a downward speed flips to half upward.
      if (item.on_ground && item.vy < 0.0)
        item.vy *= kItemBounce;
    }

    // ItemEntity.tick's merge scan, after the move and before the pickup:
    // `rate = moved ? 2 : 40` where `moved` is whether this tick's move crossed
    // a block boundary (the floors of xo/yo/zo against the new position), gated
    // on `tickCount % rate == 0` and on the item being mergable — a stack that
    // is already full is not (`isMergable`'s `count < getMaxStackSize()`).
    const bool moved_block = std::floor(start_x) != std::floor(item.x) ||
                             std::floor(start_y) != std::floor(item.y) ||
                             std::floor(start_z) != std::floor(item.z);
    const int rate = moved_block ? 2 : 40;
    if (item.age % rate == 0 && !item.stack.empty() &&
        item.stack.count < item.stack.max_stack_size())
      scan_for_merges(items, i);

    // Player.aiStep's pickup (line 447): every entity inside the player's box
    // inflated by (1.0, 0.5, 1.0) is touched. ItemEntity.playerTouch then checks
    // the pickup delay and whether the inventory can take the stack — which only
    // the session thread can answer, so the touch is *reported* here and resolved
    // by `item_apply_pickup`.
    if (!item.removed && item.pickup_delay == 0) {
      const ItemBox box = item_box(item);
      for (const ItemPlayer& player : env.players) {
        if (!player.can_pick_up)
          continue;
        if (!item_box_overlaps(box, item_pickup_area(player)))
          continue;
        report.pickups.push_back(ItemPickup{item.entity_id, player.entity_id, item.stack});
        break; // one touch per tick, like the first overlapping player vanilla finds
      }
    }

    if (!item.removed && (item.x != start_x || item.y != start_y || item.z != start_z))
      report.moved.push_back(item.entity_id);
  }

  // Everything the tick discarded is gone as far as the clients are concerned —
  // an item swallowed by a merge (the survivor carries its count) or one that
  // reached its lifetime — and every despawn of the batch leaves together.
  for (const ItemEntity& item : items)
    if (item.removed)
      report.removed.push_back(item.entity_id);

  // `setItem`'s dirty DATA_ITEM, collected after the whole loop so a merge into
  // an already-processed neighbour still leaves with this tick's packets.
  for (ItemEntity& item : items) {
    if (item.removed || !item.data_dirty)
      continue;
    item.data_dirty = false;
    report.data_changed.push_back(item.entity_id);
  }

  items.erase(std::remove_if(items.begin(), items.end(),
                             [](const ItemEntity& item) { return item.removed; }),
              items.end());
  return report;
}

bool item_apply_pickup(std::vector<ItemEntity>& items, int32_t entity_id, int accepted) {
  if (accepted <= 0)
    return false;
  for (size_t i = 0; i < items.size(); ++i) {
    if (items[i].entity_id != entity_id)
      continue;
    items[i].stack.count -= accepted;
    if (items[i].stack.count <= 0) {
      // `if (itemStack.isEmpty()) { this.discard(); }`
      items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
      return true;
    }
    // A partial pickup is `setItem` too: the survivors count is what the
    // clients draw, and take_item_entity carries the amount taken, not what is
    // left.
    items[i].data_dirty = true;
    return false;
  }
  return false;
}

} // namespace mc::world
