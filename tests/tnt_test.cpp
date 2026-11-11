// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for TNT (Phase 3 follow-up):
//   * the generated blast-resistance table: the values a blast spends its power
//     on (stone 6, obsidian 1200, bedrock 3600000, dirt 0.5, TNT 0), its sorted
//     order (the binary search's precondition) and `block_has_explosion_resistance`
//     (air has none at all);
//   * `ServerExplosion.calculateExplodedPositions`: the 16^3 shell, the 0.3 step
//     with its 0.22500001 power decay, the `0.7 + 0.6r` per-ray jitter, the
//     resistance cost, the out-of-world drop and the canonical set order;
//   * `Util.shuffle`: the walk `interactWithBlocks` puts the target list through
//     before it reads it;
//   * `ServerExplosion.getSeenPercent`: the sample grid (45 samples on a player
//     box) and the hit ratio;
//   * `ExplosionDamageCalculator.getEntityDamageAmount` and the knockback;
//   * `PrimedTnt`: the launch, the gravity and drag, landing on a floor with the
//     ground friction, and the 80-tick fuse;
//   * `getRandomShortFuse`.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "world/blast_resistance_table.h"
#include "world/chunk.h"
#include "world/container.h"
#include "world/tnt.h"

using namespace mc::world;

static int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECK_EQ(a, b)                                                                             \
  do {                                                                                             \
    const long long va = static_cast<long long>(a), vb = static_cast<long long>(b);                \
    if (va != vb) {                                                                                \
      std::fprintf(stderr, "FAIL %s:%d: %s == %lld, want %lld\n", __FILE__, __LINE__, #a, va, vb); \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                                      \
  do {                                                                                             \
    const double va = (a), vb = (b);                                                               \
    if (std::fabs(va - vb) > (eps)) {                                                              \
      std::fprintf(stderr, "FAIL %s:%d: %s == %g, want %g\n", __FILE__, __LINE__, #a, va, vb);     \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

namespace {

// A block-name world, so the ray-cast can be handed an all-air void, a floor or
// a wall without a Chunk. Keys are world coordinates.
class NameWorld {
public:
  void set(int x, int y, int z, std::string name) {
    blocks_[{x, y, z}] = std::move(name);
  }
  BlockStateKey get(int x, int y, int z) const {
    auto it = blocks_.find({x, y, z});
    return BlockStateKey{it == blocks_.end() ? std::string("minecraft:air") : it->second, ""};
  }
  ExplosionLevel level(double next_float) const {
    ExplosionLevel level;
    level.get_block = [this](int x, int y, int z) { return get(x, y, z); };
    level.in_bounds = [](int, int, int) { return true; };
    level.next_float = [next_float]() { return next_float; };
    return level;
  }

private:
  std::map<std::tuple<int, int, int>, std::string> blocks_;
};

bool is_air(const BlockStateKey& state) {
  return state.name == "minecraft:air";
}

} // namespace

static void test_blast_resistance_table() {
  // The values `Blocks` registers: `strength(destroyTime, resistance)`'s second
  // argument, or `strength(t)`'s only one.
  CHECK_NEAR(block_blast_resistance("minecraft:stone"), 6.0f, 1e-6);
  CHECK_NEAR(block_blast_resistance("minecraft:obsidian"), 1200.0f, 1e-6);
  CHECK_NEAR(block_blast_resistance("minecraft:bedrock"), 3600000.0f, 1e-6);
  CHECK_NEAR(block_blast_resistance("minecraft:dirt"), 0.5f, 1e-6);
  CHECK_NEAR(block_blast_resistance("minecraft:glass"), 0.3f, 1e-6);
  CHECK_NEAR(block_blast_resistance("minecraft:oak_planks"), 3.0f, 1e-6);
  // A block with no `strength` call resists nothing, and neither does a name the
  // registry does not have (TNT itself is the interesting one: a chain of TNT
  // does not hold a blast back).
  CHECK_NEAR(block_blast_resistance("minecraft:tnt"), 0.0f, 1e-6);
  CHECK_NEAR(block_blast_resistance("minecraft:not_a_block"), 0.0f, 1e-6);

  // Sorted, which is what the binary search needs.
  for (int i = 1; i < kBlockBlastResistanceCount; ++i)
    CHECK(std::string(kBlockBlastResistance[i - 1].name) <
          std::string(kBlockBlastResistance[i].name));

  // `getBlockExplosionResistance`: air has no entry at all, anything else does.
  CHECK(!block_has_explosion_resistance(BlockStateKey{"minecraft:air", ""}));
  CHECK(!block_has_explosion_resistance(BlockStateKey{"minecraft:cave_air", ""}));
  CHECK(!block_has_explosion_resistance(BlockStateKey{"minecraft:void_air", ""}));
  CHECK(block_has_explosion_resistance(BlockStateKey{"minecraft:stone", ""}));
  CHECK(block_has_explosion_resistance(BlockStateKey{"minecraft:water", "level=0"}));
}

static void test_explosion_rays() {
  // An all-air void, no jitter: every ray walks 2.8 / 0.22500001 -> 13 steps of
  // 0.3, so the furthest cell it can reach is ~3.9 blocks out.
  NameWorld void_world;
  const std::vector<BlockPosKey> air = explosion_blocks(void_world.level(0.0), 0.5, 0.5, 0.5, 4.0f);
  CHECK(!air.empty());
  // The centre cell is always marked, and it is marked as *air* — vanilla counts
  // the air cells too, which is what the explosion packet's block count is.
  CHECK(is_air(void_world.get(0, 0, 0)));
  bool centre = false;
  for (const BlockPosKey& pos : air)
    centre = centre || (pos.x == 0 && pos.y == 0 && pos.z == 0);
  CHECK(centre);

  // 2.8 / 0.22500001 is 12.x, so each ray marks 13 positions at 0.3 apart — the
  // last of them 3.6 blocks out. The centre is at 0.5 in each axis, so that
  // point is at 4.1 and the cell that holds it is `floor(4.1) = 4`: the largest
  // coordinate in the whole blast.
  int max_coord = 0;
  for (const BlockPosKey& pos : air)
    max_coord = std::max({max_coord, std::abs(pos.x), std::abs(pos.y), std::abs(pos.z)});
  CHECK_EQ(max_coord, 4);

  // The jitter is per ray: at its maximum the same radius reaches further.
  const std::vector<BlockPosKey> jittered =
      explosion_blocks(void_world.level(1.0), 0.5, 0.5, 0.5, 4.0f);
  CHECK(jittered.size() > air.size());
  // `4 * (0.7 + 0.6) = 5.2`, so 5.2 / 0.22500001 = 23.x -> 24 positions, the
  // last 23 * 0.3 = 6.9 out: 0.5 + 6.9 = 7.4, whose cell is 7.
  int jittered_max = 0;
  for (const BlockPosKey& pos : jittered)
    jittered_max = std::max({jittered_max, std::abs(pos.x), std::abs(pos.y), std::abs(pos.z)});
  CHECK_EQ(jittered_max, 7);

  // Canonical order: `explosion_blocks` returns the set sorted, and the caller
  // is what puts it through `explosion_shuffle` (tested below).
  for (size_t i = 1; i < air.size(); ++i) {
    const BlockPosKey& a = air[i - 1];
    const BlockPosKey& b = air[i];
    CHECK(a < b || a == b);
  }

  // A stone slab under the centre: the first stone cell a ray enters is marked,
  // but the resistance it costs ((6 + 0.3) * 0.3 = 1.89) spends the rest of that
  // ray's power, so the cells below it are untouched.
  NameWorld floored;
  for (int x = -6; x <= 6; ++x)
    for (int z = -6; z <= 6; ++z)
      floored.set(x, -1, z, "minecraft:stone");
  const std::vector<BlockPosKey> blast = explosion_blocks(floored.level(0.0), 0.5, 0.5, 0.5, 4.0f);
  int floor_cells = 0, below = 0;
  for (const BlockPosKey& pos : blast) {
    if (pos.y == -1)
      ++floor_cells;
    if (pos.y < -1)
      ++below;
  }
  CHECK(floor_cells > 0);
  CHECK_EQ(below, 0);

  // Bedrock (3600000) stops everything: no cell of the floor is marked.
  NameWorld bedrock;
  for (int x = -6; x <= 6; ++x)
    for (int z = -6; z <= 6; ++z)
      bedrock.set(x, -1, z, "minecraft:bedrock");
  const std::vector<BlockPosKey> stopped =
      explosion_blocks(bedrock.level(0.0), 0.5, 0.5, 0.5, 4.0f);
  for (const BlockPosKey& pos : stopped)
    CHECK(pos.y >= 0);

  // A ray that leaves the world is dropped (`continue block2`).
  ExplosionLevel out_of_world = void_world.level(0.0);
  out_of_world.in_bounds = [](int, int, int) { return false; };
  CHECK(explosion_blocks(out_of_world, 0.5, 0.5, 0.5, 4.0f).empty());
}

static void test_seen_percent() {
  // A player box is 0.6 x 1.8 x 0.6, so the sample grid is 1/(0.6*2+1) = 1/2.2
  // on x and z and 1/(1.8*2+1) = 1/4.6 on y: floor(1/xs) = 2 (3 samples) and
  // floor(1/ys) = 4 (5 samples), i.e. 45 samples.
  int samples = 0;
  const float clear_all =
      explosion_seen_percent(-0.3, 0.0, -0.3, 0.3, 1.8, 0.3, 0.0, 0.9, 0.0,
                             [&samples](double, double, double, double, double, double) {
                               ++samples;
                               return true;
                             });
  CHECK_EQ(samples, 45);
  CHECK_NEAR(clear_all, 1.0f, 1e-6);

  const float blocked_all =
      explosion_seen_percent(-0.3, 0.0, -0.3, 0.3, 1.8, 0.3, 0.0, 0.9, 0.0,
                             [](double, double, double, double, double, double) { return false; });
  CHECK_NEAR(blocked_all, 0.0f, 1e-6);

  // A wall halfway up the box hides a share of the samples: 2 of the 5 y layers
  // see the centre, so the ratio is exactly the grid's own.
  const float half = explosion_seen_percent(
      -0.3, 0.0, -0.3, 0.3, 1.8, 0.3, 0.0, 0.9, 0.0,
      [](double, double from_y, double, double, double, double) { return from_y > 0.8; });
  CHECK_NEAR(half, 2.0f / 5.0f, 1e-6);
}

static void test_damage_and_knockback() {
  // `((p^2 + p) / 2) * 7 * 2r + 1` with `p = (1 - distance / 2r) * exposure`, at
  // radius 4 (2r = 8).
  CHECK_NEAR(explosion_entity_damage(4.0f, 0.0, 1.0f), 57.0f, 1e-3);
  CHECK_NEAR(explosion_entity_damage(4.0f, 16.0, 1.0f), 22.0f, 1e-3);
  CHECK_NEAR(explosion_entity_damage(4.0f, 64.0, 1.0f), 1.0f, 1e-3);
  // Exposure scales it.
  CHECK_NEAR(explosion_entity_damage(4.0f, 0.0, 0.0f), 1.0f, 1e-3);

  // Knockback: `normalize(origin - center) * (1 - distance / 2r) * exposure`.
  const ExplosionPush up = explosion_knockback(0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 4.0f, 1.0f, 0.0);
  CHECK_NEAR(up.x, 0.0, 1e-9);
  CHECK_NEAR(up.y, 0.875, 1e-9);
  CHECK_NEAR(up.z, 0.0, 1e-9);
  // Explosion knockback resistance is a plain multiplier on the push.
  const ExplosionPush resisted =
      explosion_knockback(0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 4.0f, 1.0f, 0.5);
  CHECK_NEAR(resisted.y, 0.4375, 1e-9);
  // An entity exactly at the centre gets no direction, hence no push.
  const ExplosionPush centred =
      explosion_knockback(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 4.0f, 1.0f, 0.0);
  CHECK_NEAR(centred.x, 0.0, 1e-12);
  CHECK_NEAR(centred.y, 0.0, 1e-12);
}

static void test_primed_tnt() {
  PrimedTnt tnt;
  CHECK_EQ(tnt.fuse, 80);
  // `PrimedTnt`'s constructor: an upward 0.2 and a small horizontal spin.
  primed_tnt_launch(tnt, 0.0);
  CHECK_NEAR(tnt.vx, -0.0, 1e-12);
  CHECK_NEAR(tnt.vy, 0.2, 1e-12);
  CHECK_NEAR(tnt.vz, -0.02, 1e-12);
  primed_tnt_launch(tnt, 3.14159265358979 / 2.0);
  CHECK_NEAR(tnt.vx, -0.02, 1e-12);
  CHECK_NEAR(tnt.vz, 0.0, 1e-12);

  // Free fall in the void: one tick of gravity, then the air drag.
  TntEnvironment void_env;
  void_env.is_solid = [](int, int, int) { return false; };
  PrimedTnt falling;
  falling.y = 10.0;
  CHECK(!tick_primed_tnt(falling, void_env));
  CHECK_NEAR(falling.y, 10.0 - 0.04, 1e-12);
  CHECK_NEAR(falling.vy, -0.04 * 0.98, 1e-12);
  CHECK_EQ(falling.fuse, 79);

  // A floor at y = 0 (the cell [0,1)): the cube lands with its bottom on top of
  // it, the vertical velocity is spent and the ground friction bites.
  TntEnvironment floored;
  floored.is_solid = [](int, int by, int) { return by == 0; };
  PrimedTnt landed;
  landed.x = 0.5;
  landed.z = 0.5;
  landed.y = 5.0;
  landed.vx = 1.0;
  landed.vz = -1.0;
  for (int i = 0; i < 40; ++i)
    CHECK(!tick_primed_tnt(landed, floored));
  CHECK_NEAR(landed.y, 1.0 + 1e-7, 1e-6);
  // One more tick, resting: gravity is clipped again and the friction applies.
  tick_primed_tnt(landed, floored);
  CHECK_NEAR(landed.y, 1.0 + 1e-7, 1e-6);
  CHECK_NEAR(landed.vy, 0.0, 1e-9);

  // The fuse: 80 ticks, then the tick that returns true.
  PrimedTnt fused;
  int ticks = 0;
  while (!tick_primed_tnt(fused, void_env)) {
    ++ticks;
    if (ticks > 200)
      break;
  }
  CHECK_EQ(ticks, 79); // 79 calls returned false, the 80th is the explosion

  // `getRandomShortFuse(80, nextInt(20))` is 10..29.
  CHECK_EQ(primed_tnt_short_fuse(80, 0), 10);
  CHECK_EQ(primed_tnt_short_fuse(80, 19), 29);
}

// `ApplyExplosionDecay.run`: each item of a dropped stack survives with
// probability 1/radius, so a power-4 blast keeps a quarter of what it broke.
static void test_explosion_drop_decay() {
  // No draws at all (roll == 0 <= 1/4) keeps everything: the boundary case that
  // pins the comparison as `<=`, which is vanilla's.
  CHECK_EQ(explosion_drop_count(3, 4.0f, [] { return 0.0; }), 3);
  // Every draw above the chance keeps nothing (a 0.3 draw loses to 0.25).
  CHECK_EQ(explosion_drop_count(3, 4.0f, [] { return 0.3; }), 0);
  // 1.0 is the largest draw and still loses: the chance is a quarter, not a half.
  CHECK_EQ(explosion_drop_count(1, 4.0f, [] { return 1.0; }), 0);
  // A scripted alternating draw keeps exactly the ones under the chance: for
  // radius 4 that is every fourth item of a twelve-stack.
  int drawn = 0;
  const int kept = explosion_drop_count(12, 4.0f, [&drawn] {
    // 0.0, 0.1, 0.2, 0.3, 0.0, ... -> one survivor per group of four.
    return static_cast<double>(drawn++ % 4) * 0.1;
  });
  CHECK_EQ(kept, 9);   // three quarters of the twelve survive at chance 0.25
  CHECK_EQ(drawn, 12); // one draw per item, exactly like vanilla's loop
  // A radius of 1 keeps everything (chance 1.0) and an absent radius leaves the
  // stack alone; an empty stack stays empty.
  CHECK_EQ(explosion_drop_count(5, 1.0f, [] { return 1.0; }), 5);
  CHECK_EQ(explosion_drop_count(5, 0.0f, nullptr), 5);
  CHECK_EQ(explosion_drop_count(0, 4.0f, [] { return 0.0; }), 0);
  // A larger radius thins harder: `1 / 14` is 0.0714, so a 0.07 draw survives
  // and a 0.08 draw does not.
  CHECK_EQ(explosion_drop_count(64, 14.0f, [] { return 0.07; }), 64);
  CHECK_EQ(explosion_drop_count(64, 14.0f, [] { return 0.08; }), 0);
}

// `Direction.axisStepOrder`: Y always first, then whichever horizontal axis the
// movement is larger on. `Entity.move` reads it from the delta before any of it
// is resolved, and every mover here (TNT, orbs, mobs) walks the axes in it.
static void test_axis_step_order() {
  int order[3] = {-1, -1, -1};
  // A dominant X walks x before z.
  axis_step_order(1.0, 0.5, order);
  CHECK_EQ(order[0], 1); // y
  CHECK_EQ(order[1], 0); // x
  CHECK_EQ(order[2], 2); // z
  // A dominant Z (and the tie, which falls to x) flips the horizontal pair.
  axis_step_order(0.5, 1.0, order);
  CHECK_EQ(order[0], 1);
  CHECK_EQ(order[1], 2);
  CHECK_EQ(order[2], 0);
  // Equal magnitudes are the `else` branch: `abs(dx) < abs(dz)` is false.
  axis_step_order(0.7, -0.7, order);
  CHECK_EQ(order[1], 0);
  // The sign never matters, only the magnitude.
  axis_step_order(-3.0, 1.0, order);
  CHECK_EQ(order[1], 0);
  axis_step_order(1.0, -3.0, order);
  CHECK_EQ(order[1], 2);
  // A pure vertical fall still resolves y first (and only y, for the movers).
  axis_step_order(0.0, 0.0, order);
  CHECK_EQ(order[0], 1);
  CHECK_EQ(order[1], 0);
}

// `Util.shuffle(list, random)`: from the end down, each entry swaps with
// `nextInt(i)` of the ones before it, i from size to 2. `interactWithBlocks`
// runs the target list through this before it walks it, so which cell a combined
// drop stack pops from is random rather than the lowest coordinate.
static void test_explosion_shuffle() {
  const auto make = [] {
    return std::vector<BlockPosKey>{BlockPosKey{0, 0, 0}, BlockPosKey{1, 0, 0},
                                    BlockPosKey{2, 0, 0}, BlockPosKey{3, 0, 0},
                                    BlockPosKey{4, 0, 0}};
  };
  // `nextInt(i) == i - 1` at every step is the identity: each entry swaps with
  // itself, so nothing moves.
  std::vector<BlockPosKey> identity = make();
  explosion_shuffle(identity, [](int i) { return i - 1; });
  CHECK(identity == make());

  // A scripted draw pins the walk. size 5, so the draws are for i = 5, 4, 3, 2:
  //   i=5, draw 2: [A B C D E] -> [A B E D C]
  //   i=4, draw 0:            -> [D B E A C]
  //   i=3, draw 2: swap 2,2   -> unchanged
  //   i=2, draw 1: swap 1,1   -> unchanged
  std::vector<BlockPosKey> scripted = make();
  std::vector<int> draws = {2, 0, 2, 1};
  size_t next = 0;
  std::vector<int> bounds;
  explosion_shuffle(scripted, [&](int i) {
    bounds.push_back(i);
    return draws[next++];
  });
  CHECK_EQ(next, 4u); // one draw per i, size-1 of them
  CHECK_EQ(bounds.size(), 4u);
  CHECK_EQ(bounds[0], 5);
  CHECK_EQ(bounds[1], 4);
  CHECK_EQ(bounds[2], 3);
  CHECK_EQ(bounds[3], 2);
  const std::vector<BlockPosKey> expected = {BlockPosKey{3, 0, 0}, BlockPosKey{1, 0, 0},
                                             BlockPosKey{4, 0, 0}, BlockPosKey{0, 0, 0},
                                             BlockPosKey{2, 0, 0}};
  CHECK(scripted == expected);

  // A real draw sequence is a permutation: same elements, and each one exactly
  // once. That is the property the drop positions depend on.
  std::vector<BlockPosKey> shuffled = make();
  unsigned rng = 0x9e3779b9u;
  explosion_shuffle(shuffled, [&rng](int i) {
    rng = rng * 1664525u + 1013904223u; // Numerical Recipes' LCG
    return static_cast<int>(rng % static_cast<unsigned>(i));
  });
  std::sort(shuffled.begin(), shuffled.end());
  CHECK(shuffled == make());
  // A one-entry (and empty) list is untouched: the loop needs i > 1.
  std::vector<BlockPosKey> one = {BlockPosKey{9, 9, 9}};
  bool drew = false;
  explosion_shuffle(one, [&drew](int) {
    drew = true;
    return 0;
  });
  CHECK(!drew);
  CHECK(one.size() == 1u);
  std::vector<BlockPosKey> empty;
  explosion_shuffle(empty, [](int) { return 0; });
  CHECK(empty.empty());
  // No draw function at all (a caller with no randomness) leaves the list as it
  // found it rather than crashing.
  std::vector<BlockPosKey> untouched = make();
  explosion_shuffle(untouched, nullptr);
  CHECK(untouched == make());
}

int main() {
  test_blast_resistance_table();
  test_explosion_rays();
  test_seen_percent();
  test_damage_and_knockback();
  test_primed_tnt();
  test_explosion_drop_decay();
  test_axis_step_order();
  test_explosion_shuffle();
  if (failures == 0)
    std::printf("tnt: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
