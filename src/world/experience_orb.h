// SPDX-License-Identifier: GPL-3.0-or-later
//
// Experience orbs as entities (Phase 3 milestone E follow-up: the furnace's
// payout — and anything else that awards XP — is dropped as orbs that fly to
// the player and are *collected*, instead of the bar being credited by hand).
//
// Vanilla's ExperienceOrb is a server entity: it falls, is pulled toward the
// nearest non-spectator player within 8 blocks, merges with its neighbours,
// expires after 6000 ticks, and is collected by touching a player
// (ExperienceOrb.playerTouch -> Player.giveExperiencePoints). The XP a furnace
// owes therefore arrives through an orb, never straight from the block entity.
//
// What is mirrored here, from ExperienceOrb.tick and its helpers:
//   * gravity 0.03 while not inside a block, air drag 0.98 (x0.6 block friction
//     when resting on a block), the -0.4 bounce below a surface,
//   * the pull: power = (1 - distance/8)^2, added to the velocity as
//     direction * power * 0.1, with the follower remembered until it leaves the
//     8-block range,
//   * merging: orbs of the same value sharing an id residue class (mod 40), and
//     the award-time merge into an existing orb inside a 1x1x1 box,
//   * the 6000-tick lifetime and the `Value`/`Count` split (one orb holds at
//     most the largest rung of the 1..2477 ladder that fits),
//   * collection: an AABB overlap with a player, one *stack* per touch, and the
//     two-tick `takeXpDelay` that paces a stack out.
//
// What is NOT mirrored, because this server has no model for it: fluid and lava
// movement, buoyancy, the `noCollision` unstuck sweep, the mending repair
// (`repairPlayerItems`) and the pickup animation. Collision itself is
// approximated: the block registry carries names and states, not collision
// shapes, so "solid" is "not air and not a fluid" — see `OrbEnvironment`.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace mc::world {

// ExperienceOrb's constants.
constexpr double kOrbGravity = 0.03;
constexpr double kOrbAirDrag = 0.98;
// getAirDrag * the block friction of the block below (vanilla reads the block's
// friction; 0.6 is the default every ordinary block uses, and there is no
// per-block friction table here).
constexpr double kOrbGroundDrag = 0.98 * 0.6;
constexpr int kOrbLifetime = 6000;            // LIFETIME
constexpr int kOrbScanPeriod = 20;            // ENTITY_SCAN_PERIOD
constexpr double kOrbMaxFollowDistance = 8.0; // MAX_FOLLOW_DIST
constexpr double kOrbMergeDistance = 0.5;     // ORB_MERGE_DISTANCE
constexpr int kOrbGroupsPerArea = 40;         // ORB_GROUPS_PER_AREA
// The orb's bounding box (EntityType.EXPERIENCE_ORB is 0.5 x 0.5).
constexpr double kOrbWidth = 0.5;
constexpr double kOrbHeight = 0.5;
// Player.takeXpDelay is set to this on a pickup: two ticks before the next orb
// is taken, which is what paces a merged stack out one orb at a time.
constexpr int kOrbPickupDelay = 2;

// ExperienceOrb.getExperienceValue: the largest rung of the ladder that fits
// `max_value`, floored at 1.
int orb_value_for(int max_value);

// ExperienceOrb.award's split: `amount` broken into orb values, largest rung
// first. Sums to `amount`; empty for a non-positive amount.
std::vector<int> split_orb_award(int amount);

// One live orb. `x/y/z` is the entity position (the feet-centred point
// ExperienceOrb.setPos uses), `vx/vy/vz` the delta movement.
struct ExperienceOrb {
  int32_t entity_id = 0;
  double x = 0.0, y = 0.0, z = 0.0;
  double vx = 0.0, vy = 0.0, vz = 0.0;
  int value = 1; // DATA_VALUE: XP one orb of this entity is worth
  int count = 1; // how many orbs are stacked in this entity
  int age = 0;   // tickCount, capped by kOrbLifetime
  bool on_ground = false;
  int32_t following = -1; // entity id of the player being pulled toward, or -1
  bool removed = false;   // discarded: lifetime, merge or collection
};

// One player, as an orb sees them. `x/y/z` is the feet position from the
// movement packets, `can_collect` false for a spectator or a dead player, and
// `take_xp_delay` is Player.takeXpDelay: the two ticks a player is barred from
// taking another orb for after a pickup. The delay is part of the player, not
// of the transaction, so the caller counts it down (one per tick it advances)
// and writes it back; an orb is taken only when it has reached zero.
struct OrbPlayer {
  int32_t entity_id = 0;
  double x = 0.0, y = 0.0, z = 0.0;
  bool can_collect = true;
  int take_xp_delay = 0;
};

// The world an orb ticks in. `players` is everyone in play; `is_solid` reports
// whether the *block* at those world coordinates stops a falling orb, and may
// be empty (the void world this server defaults to has nothing to land on, so
// orbs fall until they expire — exactly what vanilla does there).
struct OrbEnvironment {
  std::vector<OrbPlayer> players;
  std::function<bool(int, int, int)> is_solid;
};

// An axis-aligned box, for the collision and pickup tests.
struct OrbBox {
  double min_x = 0.0, min_y = 0.0, min_z = 0.0;
  double max_x = 0.0, max_y = 0.0, max_z = 0.0;
};

// One collection: `player_entity_id` took one orb (worth `value` XP) out of
// this orb entity, which held `count` of them before the touch.
struct OrbCollection {
  int32_t orb_entity_id = 0;
  int32_t player_entity_id = 0;
  int value = 0;
};

// What one tick of the whole orb population produced. `moved` are the orbs
// whose position changed (they need a position sync), `removed` the entities
// that went away without a collection (lifetime or merge) and need a despawn.
struct OrbTickReport {
  std::vector<OrbCollection> collections;
  std::vector<int32_t> moved;
  std::vector<int32_t> removed;
};

// ExperienceOrb's constructor, in two parts: the launch velocity from three
// `random.nextDouble()` draws (x, z in [-0.2, 0.2], y in [0, 0.4] — the
// `(random * 0.2 - 0.1) * 2` / `random * 0.2 * 2` expressions), then the value
// and the id.
void orb_launch(ExperienceOrb& orb, double x_draw, double y_draw, double z_draw);

// Advances `orbs` by one tick: the merge scan, gravity, the pull, the move with
// collision, the drag, collection and the lifetime. Orbs discarded during the
// tick are erased from `orbs`; the report says what to tell the clients.
OrbTickReport tick_orbs(std::vector<ExperienceOrb>& orbs, const OrbEnvironment& env);

// The orb's collision/pickup box, and the AABB test that stands in for
// `Player.push` -> `Entity.playerTouch`.
OrbBox orb_box(const ExperienceOrb& orb);
bool orb_box_overlaps_player(const OrbBox& box, const OrbPlayer& player);

// ExperienceOrb.canMerge: same value, same id residue class (mod 40) — vanilla
// compares `(orb.getId() - this.getId()) % 40 == 0`, which is what makes which
// orbs stack depend on their ids, not their positions.
bool orbs_can_merge(const ExperienceOrb& a, const ExperienceOrb& b);

// ExperienceOrb.merge: the counts add, the younger age wins and `other` is
// discarded. Returns true when the merge happened.
bool orb_merge_into(ExperienceOrb& into, ExperienceOrb& other);

} // namespace mc::world
