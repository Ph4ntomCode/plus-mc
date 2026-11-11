// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for experience orbs as entities (Phase 3 milestone E follow-up):
//   * ExperienceOrb.getExperienceValue and the award split,
//   * the constructor's random launch,
//   * ExperienceOrb.tick's gravity, air/ground drag, the landing bounce and the
//     resting state,
//   * followNearbyPlayer: the 8-block range, the follower lock, the pull's
//     (1 - d/8)^2 * 0.1 strength and the eye-height aim,
//   * scanForMerges / canMerge: the id residue class, the value match, the
//     counts adding and the younger age winning,
//   * the 6000-tick lifetime,
//   * playerTouch: the AABB test, one *stack* per touch, the count draining and
//     the entity going away with the last one.
#include <cmath>
#include <cstdio>
#include <vector>

#include "world/experience_orb.h"

using namespace mc::world;

static int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                                      \
  do {                                                                                             \
    const double va = (a), vb = (b);                                                               \
    if (std::fabs(va - vb) > (eps)) {                                                              \
      std::fprintf(stderr, "FAIL %s:%d: %s == %f, want %f\n", __FILE__, __LINE__, #a, va, vb);     \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

namespace {

// A floor: everything at y <= 0 is solid, the air above it is not. `is_solid`
// takes *block* coordinates, which is what the session's probe passes through.
std::function<bool(int, int, int)> floor_at_zero() {
  return [](int, int y, int) { return y <= 0; };
}

OrbEnvironment flat_world() {
  OrbEnvironment env;
  env.is_solid = floor_at_zero();
  return env;
}

OrbPlayer player_at(double x, double y, double z, int32_t id = 100) {
  return OrbPlayer{id, x, y, z, true};
}

ExperienceOrb make_orb(int32_t id, double x, double y, double z, int value = 1) {
  ExperienceOrb orb;
  orb.entity_id = id;
  orb.x = x;
  orb.y = y;
  orb.z = z;
  orb.value = value;
  return orb;
}

void test_value_ladder() {
  // ExperienceOrb.getExperienceValue: the largest rung that fits, floored at 1.
  CHECK(orb_value_for(1) == 1);
  CHECK(orb_value_for(2) == 1);
  CHECK(orb_value_for(3) == 3);
  CHECK(orb_value_for(6) == 3);
  CHECK(orb_value_for(7) == 7);
  CHECK(orb_value_for(16) == 7);
  CHECK(orb_value_for(17) == 17);
  CHECK(orb_value_for(2477) == 2477);
  CHECK(orb_value_for(100000) == 2477);

  // award: the payout is split largest-rung-first and sums back to the input.
  const std::vector<int> one = split_orb_award(1);
  CHECK(one.size() == 1 && one[0] == 1);
  std::vector<int> split = split_orb_award(10); // 7 + 3
  CHECK(split.size() == 2);
  CHECK(split[0] == 7 && split[1] == 3);
  split = split_orb_award(2477 + 5); // 2477 + 3 + 1 + 1
  CHECK(split.size() == 4);
  int total = 0;
  for (int v : split)
    total += v;
  CHECK(total == 2477 + 5);
  CHECK(split_orb_award(0).empty());
}

void test_launch_velocity() {
  // ExperienceOrb's constructor: (random * 0.2 - 0.1) * 2 for x and z (so
  // [-0.2, 0.2]) and random * 0.2 * 2 for y (so [0, 0.4]).
  ExperienceOrb orb;
  orb_launch(orb, 0.0, 0.0, 0.0);
  CHECK_NEAR(orb.vx, -0.2, 1e-12);
  CHECK_NEAR(orb.vy, 0.0, 1e-12);
  CHECK_NEAR(orb.vz, -0.2, 1e-12);
  orb_launch(orb, 1.0, 1.0, 1.0);
  CHECK_NEAR(orb.vx, 0.2, 1e-12);
  CHECK_NEAR(orb.vy, 0.4, 1e-12);
  CHECK_NEAR(orb.vz, 0.2, 1e-12);
}

void test_gravity_drag_and_landing() {
  OrbEnvironment env = flat_world();
  std::vector<ExperienceOrb> orbs{make_orb(1, 0.5, 5.0, 0.5)};
  orbs[0].vx = 0.2;
  orbs[0].vy = 0.0;
  orbs[0].vz = 0.0;

  // One tick in the air: gravity 0.03, then the 0.98 air drag on every axis.
  const OrbTickReport first = tick_orbs(orbs, env);
  CHECK(orbs.size() == 1);
  CHECK_NEAR(orbs[0].vy, (0.0 - kOrbGravity) * kOrbAirDrag, 1e-12);
  CHECK_NEAR(orbs[0].vx, 0.2 * kOrbAirDrag, 1e-12);
  CHECK_NEAR(orbs[0].y, 5.0 + (0.0 - kOrbGravity), 1e-12);
  CHECK(!orbs[0].on_ground);
  CHECK(first.moved.size() == 1 && first.moved[0] == 1);

  // Falling to the floor: the orb lands on the block's top face and stays
  // there. The floor is solid at y <= 0, so the surface is y = 1.
  std::vector<ExperienceOrb> falling{make_orb(2, 0.5, 1.2, 0.5)};
  for (int i = 0; i < 60; ++i)
    tick_orbs(falling, env);
  CHECK(falling.size() == 1);
  CHECK(falling[0].on_ground);
  CHECK_NEAR(falling[0].y, 1.0, 1e-9);

  // Once resting, the orb does not move at all: the session sends no position
  // sync for it, and no client sees it jitter.
  std::vector<ExperienceOrb> resting{falling[0]};
  const double rest_y = resting[0].y;
  const OrbTickReport idle = tick_orbs(resting, env);
  CHECK(idle.moved.empty());
  CHECK_NEAR(resting[0].y, rest_y, 1e-12);
  CHECK_NEAR(resting[0].vy, 0.0, 1e-12);

  // A world with no solidity probe (the void world) lets it fall forever.
  std::vector<ExperienceOrb> void_orbs{make_orb(3, 0.5, 100.0, 0.5)};
  const OrbEnvironment void_env;
  for (int i = 0; i < 20; ++i)
    tick_orbs(void_orbs, void_env);
  CHECK(void_orbs[0].y < 100.0);
  CHECK(!void_orbs[0].on_ground);
}

void test_pull() {
  // A player 4 blocks away: power = 1 - 4/8 = 0.5, so the pull adds
  // direction * 0.5^2 * 0.1 = 0.025 along the line to the middle of the eye box.
  OrbEnvironment env = flat_world();
  env.players.push_back(player_at(4.5, 0.0, 0.5, 100));
  std::vector<ExperienceOrb> orbs{make_orb(1, 0.5, 1.5, 0.5)};
  tick_orbs(orbs, env);
  CHECK(orbs[0].following == 100);
  // The aim point is the player's feet + eye_height/2 = y + 0.81, which is above
  // the orb (1.5), so the y component of the pull points up against gravity.
  CHECK(orbs[0].vx > 0.0);
  // dx = 4, dy = 0.0 + 0.81 - 1.5 = -0.69, dz = 0: length 4.059, power 0.4926,
  // scale = power^2 * 0.1 / length.
  const double dx = 4.0, dy = 0.81 - 1.5, dz = 0.0;
  const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
  const double power = 1.0 - length / kOrbMaxFollowDistance;
  const double vx = (0.0 + dx * power * power * 0.1 / length) * kOrbAirDrag;
  CHECK_NEAR(orbs[0].vx, vx, 1e-9);

  // Out of range (more than 8 blocks): no follower, no pull.
  OrbEnvironment far = flat_world();
  far.players.push_back(player_at(30.5, 0.0, 0.5, 100));
  std::vector<ExperienceOrb> far_orbs{make_orb(2, 0.5, 3.0, 0.5)};
  tick_orbs(far_orbs, far);
  CHECK(far_orbs[0].following == -1);
  CHECK_NEAR(far_orbs[0].vx, 0.0, 1e-12);

  // A spectator (or anyone who cannot collect) neither pulls nor collects.
  OrbEnvironment spectating = flat_world();
  spectating.players.push_back(OrbPlayer{100, 1.5, 1.0, 0.5, false});
  std::vector<ExperienceOrb> ignored{make_orb(3, 1.5, 3.0, 0.5)};
  tick_orbs(ignored, spectating);
  CHECK(ignored[0].following == -1);
  CHECK(ignored.size() == 1);

  // The follower lock: an orb keeps its target while it stays in range, even
  // when a second player comes closer (vanilla only re-scans when the follower
  // leaves the 8-block range or turns invalid).
  OrbEnvironment locked = flat_world();
  locked.players.push_back(player_at(4.5, 1.0, 0.5, 100));
  std::vector<ExperienceOrb> locked_orbs{make_orb(4, 0.5, 2.0, 0.5)};
  tick_orbs(locked_orbs, locked);
  CHECK(locked_orbs[0].following == 100);
  // Closer than the first player (1.8 blocks vs 4.1) but *not* overlapping the
  // orb: a player standing on it would collect it, which would empty the vector
  // and make the assertion below read out of bounds.
  locked.players.push_back(player_at(2.0, 1.0, 0.5, 101));
  tick_orbs(locked_orbs, locked);
  CHECK(locked_orbs.size() == 1);
  CHECK(locked_orbs[0].following == 100);
}

void test_collection() {
  // Touching a player takes one orb out of the stack and hands over its value.
  OrbEnvironment env = flat_world();
  env.players.push_back(player_at(0.5, 1.0, 0.5, 100));
  std::vector<ExperienceOrb> orbs{make_orb(1, 0.5, 1.0, 0.5, 7)};
  const OrbTickReport report = tick_orbs(orbs, env);
  CHECK(report.collections.size() == 1);
  CHECK(report.collections[0].player_entity_id == 100);
  CHECK(report.collections[0].value == 7);
  CHECK(report.collections[0].orb_entity_id == 1);
  CHECK(orbs.empty()); // count 1 -> the entity is gone, with no despawn packet

  // A merged stack (count 3) drains one orb per touch, and the entity goes away
  // with the last one.
  std::vector<ExperienceOrb> stack{make_orb(2, 0.5, 1.0, 0.5, 3)};
  stack[0].count = 3;
  const OrbTickReport first = tick_orbs(stack, env);
  CHECK(first.collections.size() == 1);
  CHECK(stack.size() == 1 && stack[0].count == 2);
  CHECK(first.removed.empty()); // still there: no despawn for a surviving orb
  tick_orbs(stack, env);
  CHECK(stack.size() == 1 && stack[0].count == 1);
  tick_orbs(stack, env);
  CHECK(stack.empty());

  // takeXpDelay is the caller's (can_collect): with it running, the orb is not
  // taken, which is what paces the stack out over ticks instead of one tick.
  OrbEnvironment delayed = flat_world();
  delayed.players.push_back(OrbPlayer{100, 0.5, 1.0, 0.5, false});
  std::vector<ExperienceOrb> waiting{make_orb(3, 0.5, 1.0, 0.5, 3)};
  waiting[0].count = 2;
  const OrbTickReport none = tick_orbs(waiting, delayed);
  CHECK(none.collections.empty());
  CHECK(waiting.size() == 1 && waiting[0].count == 2);
  CHECK(kOrbPickupDelay == 2);

  // The pickup box: the orb's own 0.5-wide box against the player's 0.6 x 1.8.
  ExperienceOrb wide = make_orb(4, 0.5, 1.0, 0.5, 1);
  CHECK(orb_box_overlaps_player(orb_box(wide), player_at(0.5, 1.0, 0.5)));
  CHECK(orb_box_overlaps_player(orb_box(wide), player_at(0.9, 1.0, 0.5)));
  CHECK(!orb_box_overlaps_player(orb_box(wide), player_at(1.2, 1.0, 0.5)));
  // Vertically the boxes are [y, y+1.8] against [1.0, 1.5]: a player standing
  // above with its feet past the orb's top is out of reach, and so is one whose
  // head (y + 1.8) is below the orb's feet.
  CHECK(!orb_box_overlaps_player(orb_box(wide), player_at(0.5, 1.6, 0.5)));
  CHECK(!orb_box_overlaps_player(orb_box(wide), player_at(0.5, -1.0, 0.5)));
  CHECK(orb_box_overlaps_player(orb_box(wide), player_at(0.5, -0.2, 0.5)));
}

void test_pull_collects() {
  // The end-to-end claim: an orb at rest next to a standing player is pulled in
  // and collected, without the player moving a muscle. The wire test asserts
  // the same thing against a real client.
  OrbEnvironment env = flat_world();
  env.players.push_back(player_at(2.5, 1.0, 0.5));
  std::vector<ExperienceOrb> orbs{make_orb(1, 0.5, 1.0, 0.5, 1)};
  int ticks = 0;
  while (!orbs.empty() && ticks < 1200) {
    tick_orbs(orbs, env);
    ++ticks;
  }
  std::printf("  orb at rest 2 blocks away collected after %d ticks\n", ticks);
  CHECK(orbs.empty());
  CHECK(ticks < 1200);

  // From 6 blocks away (still inside MAX_FOLLOW_DIST) it is slower, and from
  // 9 blocks away it is never pulled at all: the orb just falls to the floor
  // and waits.
  OrbEnvironment far = flat_world();
  far.players.push_back(player_at(6.5, 1.0, 0.5));
  std::vector<ExperienceOrb> far_orb{make_orb(2, 0.5, 1.0, 0.5, 1)};
  int far_ticks = 0;
  while (!far_orb.empty() && far_ticks < 2400) {
    tick_orbs(far_orb, far);
    ++far_ticks;
  }
  std::printf("  orb at rest 6 blocks away collected after %d ticks\n", far_ticks);
  CHECK(far_orb.empty());

  OrbEnvironment out_of_range = flat_world();
  out_of_range.players.push_back(player_at(9.0, 1.0, 0.5));
  std::vector<ExperienceOrb> ignored{make_orb(3, 0.5, 1.0, 0.5, 1)};
  for (int i = 0; i < 600; ++i)
    tick_orbs(ignored, out_of_range);
  CHECK(ignored.size() == 1);
  CHECK(ignored[0].following == -1);
}

void test_merge() {
  // canMerge: same value, and ids in the same residue class (mod 40).
  std::vector<ExperienceOrb> orbs{make_orb(1, 0.5, 1.0, 0.5, 3), make_orb(41, 0.7, 1.0, 0.5, 3)};
  CHECK(orbs_can_merge(orbs[0], orbs[1]));
  std::vector<ExperienceOrb> other_value{make_orb(2, 0.5, 1.0, 0.5, 3),
                                         make_orb(42, 0.7, 1.0, 0.5, 7)};
  CHECK(!orbs_can_merge(other_value[0], other_value[1]));
  std::vector<ExperienceOrb> other_group{make_orb(3, 0.5, 1.0, 0.5, 3),
                                         make_orb(44, 0.7, 1.0, 0.5, 3)};
  CHECK(!orbs_can_merge(other_group[0], other_group[1]));
  // `orb != this` is the instance-level guard, which orb_merge_into applies.
  ExperienceOrb itself = make_orb(5, 0.5, 1.0, 0.5, 1);
  CHECK(!orb_merge_into(itself, itself));
  CHECK(itself.count == 1 && !itself.removed);

  // The merge scan runs on the first tick (tickCount % 20 == 1) and again every
  // 20 ticks after it: the absorbing orb's count adds up and the other entity is
  // reported as despawned.
  OrbEnvironment env = flat_world();
  std::vector<ExperienceOrb> pair{make_orb(1, 0.5, 1.0, 0.5, 3), make_orb(41, 0.8, 1.0, 0.5, 3),
                                  make_orb(81, 0.5, 1.0, 0.9, 3)};
  pair[0].count = 2;
  pair[1].count = 5;
  const OrbTickReport report = tick_orbs(pair, env);
  CHECK(pair.size() == 1);
  CHECK(pair[0].entity_id == 1);
  CHECK(pair[0].count == 2 + 5 + 1);
  CHECK(report.removed.size() == 2);

  // Merging is not "everything nearby": a same-value orb outside the 0.5-block
  // gap survives.
  std::vector<ExperienceOrb> distant{make_orb(1, 0.5, 1.0, 0.5, 3), make_orb(41, 1.6, 1.0, 0.5, 3)};
  tick_orbs(distant, env);
  CHECK(distant.size() == 2);

  // orb_merge_into keeps the younger age, so a fresh orb's lifetime is not cut
  // short by an old neighbour's.
  ExperienceOrb a = make_orb(1, 0.5, 1.0, 0.5, 3);
  ExperienceOrb b = make_orb(41, 0.5, 1.0, 0.5, 3);
  a.age = 100;
  b.age = 5000;
  CHECK(orb_merge_into(a, b));
  CHECK(a.age == 100);
  CHECK(b.removed);
}

void test_lifetime() {
  OrbEnvironment env = flat_world();
  std::vector<ExperienceOrb> orbs{make_orb(1, 0.5, 1.0, 0.5)};
  orbs[0].age = kOrbLifetime - 1;
  const OrbTickReport report = tick_orbs(orbs, env);
  CHECK(orbs.empty());
  CHECK(report.removed.size() == 1 && report.removed[0] == 1);
  CHECK(report.collections.empty());
}

} // namespace

int main() {
  test_value_ladder();
  test_launch_velocity();
  test_gravity_drag_and_landing();
  test_pull();
  test_collection();
  test_pull_collects();
  test_merge();
  test_lifetime();
  if (failures == 0)
    std::printf("xp_orb: all checks passed\n");
  else
    std::printf("xp_orb: %d check(s) failed\n", failures);
  return failures == 0 ? 0 : 1;
}
