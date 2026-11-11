// SPDX-License-Identifier: GPL-3.0-or-later
//
// TNT: the block that is primed, the primed entity and its fuse, and the
// explosion that closes it out (Phase 3 follow-up).
//
// Three vanilla pieces, kept here as rules so a unit test can pin them without
// a session or a socket:
//
//   * `PrimedTnt` — the entity the primed block becomes. Its fuse is 80 ticks
//     (`DEFAULT_FUSE_TIME`), it falls under its own gravity, and at zero it is
//     discarded and the level explodes. An explosion that catches *another* TNT
//     block gives it `getRandomShortFuse` instead of the full 80.
//   * `ServerExplosion` — the blast itself: 16^3 shell rays marched outward
//     through the blocks, an entity visibility sample, and the damage and
//     knockback that fall off with distance.
//   * `TntBlock` — the priming rules: flint and steel (or a fire charge) on the
//     block, a redstone signal, a burning projectile, another explosion, and —
//     the path that makes a *fire* dangerous — `FireBlock.checkBurnOut`, which
//     primes a TNT neighbour it would otherwise have burned away.
//
// What is deliberately not modelled is listed in docs/PHASE3.md: the blast's
// own particles and sound have no ids in this server's registry set, and the
// `TNT_EXPLODES` / `BLOCK_EXPLOSION_DROP_DECAY` gamerules have their defaults
// hard-wired (always on, drops always decayed).
#pragma once

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include "world/axis_step.h"
#include "world/chunk.h"
#include "world/container.h"
#include "world/fire.h"

namespace mc::world {

// ---- the primed entity ------------------------------------------------------

// `PrimedTnt.DEFAULT_FUSE_TIME`, `DEFAULT_EXPLOSION_POWER` and the entity's own
// constants (`getDefaultGravity`, `Entity.getAirDrag`, the ground friction).
constexpr int kPrimedTntFuse = 80;
constexpr float kTntExplosionPower = 4.0f;
constexpr double kPrimedTntGravity = 0.04;
constexpr double kPrimedTntAirDrag = 0.98;
constexpr double kPrimedTntGroundFriction = 0.7;
constexpr double kPrimedTntGroundBounce = -0.5;
// `sized(0.98f, 0.98f)`: a 0.98-block cube centred on its position.
constexpr double kPrimedTntHalfWidth = 0.49;
constexpr double kPrimedTntHeight = 0.98;

// `PrimedTnt.getRandomShortFuse(fuse, random)`: an explosion that primes a
// block, or a TNT block destroyed by one, gets `nextInt(max(1, fuse/4)) +
// fuse/8` ticks instead of 80. `draw` is that `nextInt` result.
int primed_tnt_short_fuse(int fuse, int draw);

// One primed TNT. `x`/`y`/`z` are the entity's position (its feet-less centre:
// `Entity.getX()` on a 0.98 cube) and `vx`/`vy`/`vz` its delta movement.
struct PrimedTnt {
  int32_t entity_id = 0;
  // The player whose flint and steel lit it, or 0. `PrimedTnt.getOwner` is what
  // the explosion's damage source names as the *indirect* cause.
  int32_t owner_entity_id = 0;
  double x = 0.0, y = 0.0, z = 0.0;
  double vx = 0.0, vy = 0.0, vz = 0.0;
  int fuse = kPrimedTntFuse;
  // The block state the client draws (`DATA_BLOCK_STATE_ID`): always
  // `minecraft:tnt` here, since nothing else can be primed in this server.
  std::string_view block = "minecraft:tnt";
};

// What one primed-TNT tick needs from the world. `is_solid` is the same
// predicate a mob's movement asks (a block that stops the entity);
// `unit_random` owns the randomness so a test can pin the fuse.
struct TntEnvironment {
  std::function<bool(int, int, int)> is_solid;
  std::function<double()> unit_random;
};

// `PrimedTnt.tick()`'s movement and fuse: gravity, a collide-and-slide move
// (`Entity.move`'s `Direction.axisStepOrder` axis order, world/axis_step.h), the
// air drag, the ground friction, then `fuse - 1`. Returns true on the tick the
// fuse reaches zero — the caller discards the entity and explodes.
bool tick_primed_tnt(PrimedTnt& tnt, const TntEnvironment& env);

// The launch `PrimedTnt`'s constructor gives a fresh entity: a random upward
// 0.2 with a small horizontal spin, from a draw in [0, 2pi).
void primed_tnt_launch(PrimedTnt& tnt, double angle_radians);

// ---- the explosion ----------------------------------------------------------

// `Explosion.EXPLOSION_STEP` and the per-step power decay: the ray loop advances
// 0.3 blocks and spends `0.22500001` of remaining power each step.
constexpr float kExplosionStep = 0.3f;
constexpr float kExplosionStepDecay = 0.22500001f;
// `(resistance + 0.3) * 0.3`: what a cell's blast resistance costs a ray.
constexpr float kExplosionResistanceBias = 0.3f;
// The ray fan: 16 samples per axis, and the boundary half of each is skipped,
// so 16^3 - 14^3 = 1352 rays leave the centre.
constexpr int kExplosionRaySamples = 16;

// The world `ServerExplosion.calculateExplodedPositions` reads. `get_block` is
// the cell's state (its *name* is looked up in the generated blast-resistance
// table); `in_bounds` is `Level.isInWorldBounds` — a ray that leaves the world
// is dropped, which is how a blast at the build limit is cut off.
struct ExplosionLevel {
  std::function<BlockStateKey(int, int, int)> get_block;
  std::function<bool(int, int, int)> in_bounds;
  // `Level.random.nextFloat()`: one draw per ray, for the `0.7 + 0.6r` power
  // jitter. Empty leaves every ray at the 1.0 multiplier.
  std::function<double()> next_float;
}; // `ServerExplosion.calculateExplodedPositions`: the blocks the blast destroys,
// as a deterministic (sorted) canonical set. Vanilla hands this list on through
// `Util.shuffle` before reading it, which is `explosion_shuffle` below — the
// sort is only the seed order the shuffle walks, not the order the blast uses.
std::vector<BlockPosKey> explosion_blocks(const ExplosionLevel& level, double center_x,
                                          double center_y, double center_z, float radius);

// `Util.shuffle(list, random)`: from the end down, swap each entry with
// `next_int(i)` among the ones before it. `ServerExplosion.interactWithBlocks`
// shuffles the target list first, so no cell has priority when the drops
// combine — `StackCollector` keeps the position of the *first* cell that opened
// a stack, so the shuffle is what decides where a merged stack of up to 16
// pops, and therefore where a blast's loot lands. `next_int(bound)` is
// `RandomSource.nextInt(bound)`, i.e. uniform over [0, bound); an empty function
// leaves the list alone.
void explosion_shuffle(std::vector<BlockPosKey>& blocks, const std::function<int(int)>& next_int);

// `ServerExplosion.getSeenPercent`: how much of an entity's box the explosion
// centre can see. `clip_miss` is `level.clip(from, to).isMiss()`, i.e. nothing
// solid on the segment; the sample grid is
// `1 / ((extent * 2) + 1)` per axis over the box.
float explosion_seen_percent(
    double min_x, double min_y, double min_z, double max_x, double max_y, double max_z,
    double center_x, double center_y, double center_z,
    const std::function<bool(double, double, double, double, double, double)>& clip_miss);

// `ExplosionDamageCalculator.getEntityDamageAmount`: `((p^2 + p) / 2) * 7 *
// 2r + 1` for `p = (1 - distance / (2r)) * exposure`. One point of damage is
// the floor even for an entity on the very edge.
float explosion_entity_damage(float radius, double distance_sqr, float exposure);

// `ServerExplosion.hurtEntities`' knockback: `normalize(origin - center) *
// (1 - distance / 2r) * exposure * (1 - knockback_resistance)`. `origin` is the
// entity's eye position (or its position for a primed TNT).
struct ExplosionPush {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

ExplosionPush explosion_knockback(double center_x, double center_y, double center_z,
                                  double origin_x, double origin_y, double origin_z,
                                  double distance_sqr, float radius, float exposure,
                                  double knockback_resistance);

// `ApplyExplosionDecay.run`: a blast does not yield its loot whole. Each item in
// a dropped stack survives with probability `1 / radius` — the draw is compared
// against that survival chance, and the survivors are what the stack keeps, so a
// stack of three in a power-4 blast is expected to leave 0.75 of an item. `roll`
// is one `nextFloat()`; the caller owns the randomness.
int explosion_drop_count(int count, float radius, const std::function<double()>& roll);

// `BlockBehaviour.getExplosionResistance`: the generated per-block table
// (world/blast_resistance_table.h), 0 for a block that resists nothing.
float block_blast_resistance(std::string_view block_name);

// `ExplosionDamageCalculator.getBlockExplosionResistance`: an empty (air) cell
// has no resistance at all and lets a ray through untouched; anything else
// costs its table value.
bool block_has_explosion_resistance(const BlockStateKey& state);

} // namespace mc::world
