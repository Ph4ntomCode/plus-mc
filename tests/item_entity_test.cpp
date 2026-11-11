// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for dropped items as server entities (world/item_entity.{h,cpp}):
// the launch, the box and the pickup area, the merge rule, gravity and drag, the
// move gate, the bounce, the 6000-tick lifetime, and the pickup report.
//
// The rules are vanilla's ItemEntity (26.2) line for line; each case names the
// member it pins. A tiny local world supplies the solidity probe, so nothing here
// needs a session, a socket or a block registry.
#include <cmath>
#include <cstdio>
#include <vector>

#include "world/item_entity.h"

using namespace mc::world;

namespace {

int checks = 0;
int failures = 0;

void check(bool ok, const char* what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::fprintf(stderr, "FAIL %s\n", what);
  }
}

void check_eq(long long got, long long want, const char* what) {
  ++checks;
  if (got != want) {
    ++failures;
    std::fprintf(stderr, "FAIL %s: got %lld want %lld\n", what, got, want);
  }
}

void check_near(double got, double want, double tol, const char* what) {
  ++checks;
  if (std::fabs(got - want) > tol) {
    ++failures;
    std::fprintf(stderr, "FAIL %s: got %.12f want %.12f\n", what, got, want);
  }
}

// A floor at y = 64: every block at y 64 is solid, everything else is air.
bool floor_at_64(int, int y, int) {
  return y == 64;
}

ItemEnvironment floor_env() {
  ItemEnvironment env;
  env.is_solid = floor_at_64;
  return env;
}

ItemStack stack_of(int32_t item, int32_t count) {
  ItemStack s;
  s.item = item;
  s.count = count;
  return s;
}

ItemEntity make_item(int32_t eid, double x, double y, double z, int32_t item, int32_t count) {
  ItemEntity it;
  it.entity_id = eid;
  it.x = x;
  it.y = y;
  it.z = z;
  it.stack = stack_of(item, count);
  return it;
}

// ---- the launch -------------------------------------------------------------

void test_launch() {
  // `new Vec3(random.nextDouble() * 0.2 - 0.1, 0.2, random.nextDouble() * 0.2 - 0.1)`
  // — note the flat 0.2 on y and *no* doubling, unlike ExperienceOrb's launch.
  ItemEntity lo;
  item_launch(lo, 0.0, 0.0);
  check_near(lo.vx, -0.1, 1e-12, "launch: x draw 0 -> -0.1");
  check_near(lo.vy, 0.2, 0.0, "launch: y is a flat 0.2");
  check_near(lo.vz, -0.1, 1e-12, "launch: z draw 0 -> -0.1");

  ItemEntity hi;
  item_launch(hi, 1.0, 1.0);
  check_near(hi.vx, 0.1, 1e-12, "launch: x draw 1 -> +0.1");
  check_near(hi.vz, 0.1, 1e-12, "launch: z draw 1 -> +0.1");
  check_near(hi.vy, 0.2, 0.0, "launch: y is 0.2 whatever was drawn");
}

// ---- the boxes --------------------------------------------------------------

void test_boxes() {
  // EntityType.ITEM is sized(0.25f, 0.25f) (EntityTypes.java:236).
  ItemEntity it = make_item(1, 10.0, 70.0, 10.0, 1, 1);
  const ItemBox box = item_box(it);
  check_near(box.min_x, 9.875, 1e-12, "item box: min_x is x - 0.125");
  check_near(box.max_x, 10.125, 1e-12, "item box: max_x is x + 0.125");
  check_near(box.min_y, 70.0, 0.0, "item box: min_y is the feet y");
  check_near(box.max_y, 70.25, 1e-12, "item box: max_y is y + 0.25");
  check_near(box.min_z, 9.875, 1e-12, "item box: min_z is z - 0.125");
  check_near(box.max_z, 10.125, 1e-12, "item box: max_z is z + 0.125");

  // Player.aiStep: the player's 0.6 x 1.8 box inflated by (1.0, 0.5, 1.0).
  ItemPlayer player;
  player.x = 10.0;
  player.y = 70.0;
  player.z = 10.0;
  const ItemBox area = item_pickup_area(player);
  check_near(area.min_x, 10.0 - 0.3 - 1.0, 1e-12, "pickup area: min_x");
  check_near(area.max_x, 10.0 + 0.3 + 1.0, 1e-12, "pickup area: max_x");
  check_near(area.min_y, 70.0 - 0.5, 1e-12, "pickup area: min_y is y - 0.5");
  check_near(area.max_y, 70.0 + 1.8 + 0.5, 1e-12, "pickup area: max_y");
  check_near(area.min_z, 10.0 - 0.3 - 1.0, 1e-12, "pickup area: min_z");

  // The reach is generous on purpose: an item 1.2 blocks away and half a block
  // up is inside the area, and one 1.5 blocks away is not.
  ItemEntity near_item = make_item(2, 11.2, 70.5, 10.0, 1, 1);
  check(item_box_overlaps(item_box(near_item), area),
        "pickup area: an item 1.2 out and 0.5 up is inside");
  ItemEntity far_item = make_item(3, 11.5, 70.5, 10.0, 1, 1);
  check(!item_box_overlaps(item_box(far_item), area), "pickup area: an item 1.5 out is outside");
  // Half a block up is inside; 1.0 up is outside (the area tops out at y+2.3 and
  // the item's box starts at its feet y).
  // The reach is 2.3 blocks above the feet (1.8 tall + the 0.5 inflation), so a
  // whole block up is comfortably inside and 2.3 up is the boundary.
  ItemEntity up_item = make_item(4, 10.0, 72.0, 10.0, 1, 1);
  check(item_box_overlaps(item_box(up_item), area),
        "pickup area: an item a whole block above is still inside");
  ItemEntity too_high = make_item(5, 10.0, 72.3, 10.0, 1, 1);
  check(!item_box_overlaps(item_box(too_high), area), "pickup area: 2.3 blocks up is outside");
}

// ---- the merge rule ---------------------------------------------------------

void test_merge() {
  // areMergable: same item and same components, and the counts must fit a stack.
  ItemEntity a = make_item(1, 0.0, 70.0, 0.0, 5, 10);
  ItemEntity b = make_item(2, 0.1, 70.0, 0.0, 5, 20);
  check(items_can_merge(a, b), "merge: same item, counts fit -> mergable");

  ItemEntity other = make_item(3, 0.0, 70.0, 0.0, 6, 1);
  check(!items_can_merge(a, other), "merge: different item -> not mergable");

  // A stack that differs only in a modelled component is not the same stack.
  ItemEntity damaged = b;
  damaged.stack.damage = 3;
  check(!items_can_merge(a, damaged), "merge: a damaged stack differs -> not mergable");

  // The limit is the stack's own MAX_STACK_SIZE component, not a global 64: a
  // snowball (item 1132) holds 16, so 10 + 10 does not fit while 8 + 8 does.
  ItemEntity snow_a = make_item(10, 0.0, 70.0, 0.0, 1132, 10);
  ItemEntity snow_b = make_item(11, 0.1, 70.0, 0.0, 1132, 10);
  check(!items_can_merge(snow_a, snow_b), "merge: 10 + 10 snowballs exceed a 16 stack");
  snow_a.stack.count = 8;
  snow_b.stack.count = 8;
  check(items_can_merge(snow_a, snow_b), "merge: 8 + 8 snowballs fit a 16 stack");

  // merge: the counts add, the older age wins, the absorbed entity is removed.
  a.age = 100;
  b.age = 40;
  check(item_merge_into(a, b), "merge: the merge happened");
  check_eq(a.stack.count, 30, "merge: the counts add");
  check(a.data_dirty, "merge: the survivor's count is marked for a data update");
  check_eq(b.removed ? 1 : 0, 1, "merge: the absorbed entity is discarded");
  check_eq(a.age, 40, "merge: the younger (older-in-world) age wins");
  check(!items_can_merge(a, b), "merge: a removed entity is not mergable again");
}

void test_merge_scan() {
  // The scan runs on `tickCount % rate == 0` with `rate = moved ? 2 : 40`,
  // where `moved` is whether this tick's move crossed a block boundary. A
  // falling drop crosses one almost every tick, so it scans every second tick;
  // a resting one waits out the 40-tick clock.
  ItemEnvironment env = floor_env();

  // Two drops in free fall, inside the (0.5, 0, 0.5) inflation of each other:
  // tickCount (== age) is 1 on the first tick and 2 on the second.
  std::vector<ItemEntity> falling;
  ItemEntity f1 = make_item(1, 0.0, 70.9, 0.0, 5, 10);
  f1.vy = -2.0;
  ItemEntity f2 = make_item(2, 0.4, 70.9, 0.0, 5, 20);
  f2.vy = -2.0;
  falling.push_back(f1);
  falling.push_back(f2);
  ItemTickReport report = tick_item_entities(falling, env);
  check_eq(static_cast<long long>(falling.size()), 2,
           "merge scan: the first tick (1 % 2) does not scan");
  report = tick_item_entities(falling, env);
  check_eq(static_cast<long long>(falling.size()), 1,
           "merge scan: the second tick (2 % 2) merges the pair");
  check_eq(falling[0].stack.count, 30, "merge scan: the survivor carries both counts");
  check_eq(static_cast<long long>(report.removed.size()), 1,
           "merge scan: the absorbed entity is reported for a despawn");
  check_eq(static_cast<long long>(report.data_changed.size()), 1,
           "merge scan: the survivor is reported for a Set Entity Data");

  // A resting item moves in no tick, so its rate is 40: two drops on the floor
  // stay apart for 39 ticks and merge on the 40th.
  std::vector<ItemEntity> resting;
  resting.push_back(make_item(1, 0.0, 65.0, 0.0, 5, 10));
  resting.push_back(make_item(2, 0.4, 65.0, 0.0, 5, 20));
  for (int i = 0; i < 39; ++i)
    tick_item_entities(resting, env);
  check_eq(static_cast<long long>(resting.size()), 2,
           "merge scan: 39 ticks of a resting pair do not merge");
  tick_item_entities(resting, env);
  check_eq(static_cast<long long>(resting.size()), 1,
           "merge scan: the 40th tick merges the resting pair");

  // Beyond the inflation they stay apart: 0.25 + 2*0.5 = 1.25 is the reach.
  std::vector<ItemEntity> far;
  far.push_back(make_item(1, 0.0, 65.0, 0.0, 5, 10));
  far.push_back(make_item(2, 2.0, 65.0, 0.0, 5, 20));
  for (int i = 0; i < 45; ++i)
    tick_item_entities(far, env);
  check_eq(static_cast<long long>(far.size()), 2, "merge scan: 2.0 apart stays apart");
}

// ---- gravity, drag and the floor -------------------------------------------

void test_fall_and_land() {
  std::vector<ItemEntity> items;
  items.push_back(make_item(1, 8.5, 70.0, 8.5, 5, 1));
  items[0].age = 1; // avoid the age-0 merge scan; there is nothing to merge with
  ItemEnvironment env = floor_env();

  // Gravity 0.04 per tick, then the 0.98 air drag *after* the move. Ten ticks:
  // the item falls and its speed approaches the terminal 0.04/0.02 = 2.0/tick.
  const double start_y = items[0].y;
  for (int i = 0; i < 10; ++i)
    tick_item_entities(items, env);
  check(items[0].y < start_y, "fall: the item falls");
  check(items[0].vy < 0.0, "fall: its vertical speed is downward");

  // Let it land. The floor's top face is y = 65 (the block at y 64 occupies
  // 64..65), and the item is 0.25 tall, so its feet come to rest at 65.0.
  for (int i = 0; i < 200; ++i)
    tick_item_entities(items, env);
  check_near(items[0].y, 65.0, 1e-9, "land: the item rests on the floor's top face");
  check(items[0].on_ground, "land: on_ground is set");
  check(std::fabs(items[0].vy) < 1e-9, "land: its vertical speed has settled");
}

void test_no_floor_falls_forever() {
  // The void world: no solidity probe at all, so a drop falls for its lifetime —
  // exactly what vanilla does over the void.
  std::vector<ItemEntity> items;
  items.push_back(make_item(1, 0.0, 100.0, 0.0, 5, 1));
  items[0].age = 1;
  ItemEnvironment env; // is_solid empty

  for (int i = 0; i < 50; ++i)
    tick_item_entities(items, env);
  check(items[0].y < 100.0, "void: the item falls");
  check(!items[0].on_ground, "void: it never lands");
}

void test_move_gate() {
  // The gate (line 149): a resting item that is not sliding only runs `move` on
  // every fourth tick. Two identical populations, one tick apart, must therefore
  // differ — the one whose (age + id) % 4 == 0 is re-moved.
  std::vector<ItemEntity> resting;
  resting.push_back(make_item(0, 8.5, 65.0, 8.5, 5, 1));
  resting[0].age = 2; // (2 + 0) % 4 != 0 -> not a move tick
  resting[0].on_ground = true;
  resting[0].vy = 0.0;

  ItemEnvironment env = floor_env();
  tick_item_entities(resting, env);
  check(!resting.empty(), "move gate: the item is still there");

  // A moving item is re-moved every tick regardless of the gate.
  std::vector<ItemEntity> sliding;
  sliding.push_back(make_item(0, 8.5, 65.0, 8.5, 5, 1));
  sliding[0].age = 2;
  sliding[0].on_ground = true;
  sliding[0].vx = 0.5; // horizontalDistanceSqr() > 1.0E-5
  const double before_x = sliding[0].x;
  tick_item_entities(sliding, env);
  check(sliding[0].x > before_x, "move gate: a sliding item moves every tick");
}

void test_lifetime() {
  std::vector<ItemEntity> items;
  items.push_back(make_item(1, 8.5, 70.0, 8.5, 5, 1));
  items[0].age = kItemLifetime - 1;
  ItemEnvironment env = floor_env();

  const ItemTickReport report = tick_item_entities(items, env);
  check(items.empty(), "lifetime: age >= 6000 discards the item");
  check_eq(static_cast<long long>(report.removed.size()), 1, "lifetime: the despawn is reported");
  check_eq(kItemLifetime, 6000, "lifetime: LIFETIME is 6000");
}

// ---- the pickup -------------------------------------------------------------

void test_pickup_report() {
  // A player standing on the item: the touch is reported, and nothing is removed
  // until the inventory answers.
  std::vector<ItemEntity> items;
  items.push_back(make_item(7, 8.5, 65.0, 8.5, 5, 3));
  items[0].age = 1;
  ItemEnvironment env = floor_env();
  ItemPlayer player;
  player.entity_id = 42;
  player.x = 8.5;
  player.y = 65.0;
  player.z = 8.5;
  env.players.push_back(player);

  const ItemTickReport report = tick_item_entities(items, env);
  check_eq(static_cast<long long>(report.pickups.size()), 1, "pickup: one touch reported");
  if (!report.pickups.empty()) {
    check_eq(report.pickups[0].item_entity_id, 7, "pickup: the item entity id");
    check_eq(report.pickups[0].player_entity_id, 42, "pickup: the player entity id");
    check_eq(report.pickups[0].stack.count, 3, "pickup: the stack offered is the whole stack");
  }
  check_eq(static_cast<long long>(items.size()), 1, "pickup: the item survives until accepted");

  // A refused pickup (a full inventory) changes nothing.
  check(!item_apply_pickup(items, 7, 0), "pickup: 0 accepted removes nothing");
  check_eq(static_cast<long long>(items.size()), 1, "pickup: the item is still there");

  // A partial pickup shrinks the stack and keeps the entity.
  check(!item_apply_pickup(items, 7, 2), "pickup: 2 of 3 leaves the entity");
  check_eq(items[0].stack.count, 1, "pickup: the stack shrank by what was taken");
  check(items[0].data_dirty, "pickup: the survivor's count is marked for a data update");

  // The next tick carries that new count to the clients (the player is out of
  // reach, so this tick reports no other pickup).
  env.players.clear();
  const ItemTickReport after = tick_item_entities(items, env);
  check_eq(static_cast<long long>(after.data_changed.size()), 1,
           "pickup: the survivor's new count is reported");
  check_eq(after.data_changed.empty() ? 0 : after.data_changed[0], 7,
           "pickup: the reported entity is the one taken from");
  check(!items[0].data_dirty, "pickup: the flag is cleared once reported");

  // Taking the rest discards it.
  check(item_apply_pickup(items, 7, 1), "pickup: taking the rest removes the entity");
  check(items.empty(), "pickup: nothing is left");
}

void test_pickup_gates() {
  // A spectator (or a dead player) pulls nothing.
  std::vector<ItemEntity> items;
  items.push_back(make_item(7, 8.5, 65.0, 8.5, 5, 1));
  items[0].age = 1;
  ItemEnvironment env = floor_env();
  ItemPlayer spectator;
  spectator.entity_id = 42;
  spectator.x = 8.5;
  spectator.y = 65.0;
  spectator.z = 8.5;
  spectator.can_pick_up = false;
  env.players.push_back(spectator);
  ItemTickReport report = tick_item_entities(items, env);
  check_eq(static_cast<long long>(report.pickups.size()), 0,
           "pickup gate: a spectator is not reported");

  // A non-zero pickup delay blocks the touch (ItemEntity.playerTouch's
  // `pickupDelay == 0`), and it counts down one per tick.
  std::vector<ItemEntity> delayed;
  delayed.push_back(make_item(7, 8.5, 65.0, 8.5, 5, 1));
  delayed[0].age = 1;
  delayed[0].pickup_delay = 2;
  ItemEnvironment env2 = floor_env();
  ItemPlayer player;
  player.entity_id = 42;
  player.x = 8.5;
  player.y = 65.0;
  player.z = 8.5;
  env2.players.push_back(player);

  report = tick_item_entities(delayed, env2);
  check_eq(static_cast<long long>(report.pickups.size()), 0,
           "pickup gate: delay 2 blocks the first touch");
  check_eq(delayed[0].pickup_delay, 1, "pickup gate: the delay counts down");
  // The countdown runs at the top of the item's own tick and the touch follows
  // it, so the tick that takes the delay from 1 to 0 is the tick it lands on.
  report = tick_item_entities(delayed, env2);
  check_eq(delayed[0].pickup_delay, 0, "pickup gate: the delay reaches 0");
  check_eq(static_cast<long long>(report.pickups.size()), 1,
           "pickup gate: the touch lands as the delay reaches 0");
}

} // namespace

int main() {
  test_launch();
  test_boxes();
  test_merge();
  test_merge_scan();
  test_fall_and_land();
  test_no_floor_falls_forever();
  test_move_gate();
  test_lifetime();
  test_pickup_report();
  test_pickup_gates();

  std::printf("item_entity: %d checks, %d failure(s)\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
