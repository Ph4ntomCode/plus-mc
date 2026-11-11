// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for fire (Phase 3 milestone G follow-up):
//   * the generated flammability table: `FireBlock.getIgniteOdds` by name, the
//     175 rows in sorted order (so the binary search is sound), and
//     `FireBlock.canBurn`'s waterlogged refusal;
//   * `minecraft:fire` / `minecraft:soul_fire` as names, and `Level.isEmptyBlock`;
//   * `BaseFireBlock.getState` / `FireBlock.getStateForPlacement`: soul fire over
//     a soul-fire base block, the default age-0 state over something burnable or
//     sturdy, and the five connection properties when the block below does
//     neither;
//   * `FireBlock.canSurvive` and `BaseFireBlock.canBePlacedAt`;
//   * `AbstractThrownPotion.onHitBlock`'s douse: `Direction`'s six steps,
//     `fire_douse_positions`' six positions in vanilla's order, and `dowseFire`'s
//     three branches (destroy a fire, unlight a campfire, unlight a candle);
//   * `CampfireBlock.canLight` / `CandleBlock.canLight` / `CandleCakeBlock
//     .canLight` and the `lit=true` they set;
//   * `Entity.remainingFireTicks`: the signed countdown, `getFireImmuneTicks`'s
//     20 for a player and 0 for everything else, `baseTick`'s 1.0 damage every
//     20 ticks (suppressed in lava, cleared when fire-immune),
//     `applyEffectsFromBlocks`' reset, `clearFire`/`extinguishFire`, and
//     `BaseFireBlock.fireIgnite`'s one-tick bump below zero against a player's
//     `nextInt(1, 3)` above it;
//   * `BaseFireBlock.entityInside`: the box walk with its 1e-7 pull-in, the
//     per-block damage (1.0 fire, 2.0 soul fire) and the last block winning;
//   * the per-chunk block-tick queue (world/block_tick.h): the due clock, the
//     one-entry-per-position rule and the chunk bucketing;
//   * `FireBlock.tick` (world/fire.cpp's `fire_tick`): the self-reschedule, the
//     `nextInt(3) / 2` ageing, the removal when `canSurvive` fails, the
//     six-neighbour burn-out, the spread into a flammable neighbour and the
//     infiniburn reprieve. Every draw is scripted, so each branch is pinned.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "world/block_tick.h"
#include "world/chunk.h"
#include "world/container.h"
#include "world/fire.h"
#include "world/fire_table.h"
#include "world/survival.h"

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

#define CHECK_STR(a, b)                                                                            \
  do {                                                                                             \
    const std::string va = (a), vb = (b);                                                          \
    if (va != vb) {                                                                                \
      std::fprintf(stderr, "FAIL %s:%d: %s == \"%s\", want \"%s\"\n", __FILE__, __LINE__, #a,      \
                   va.c_str(), vb.c_str());                                                        \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

namespace {

BlockStateKey block(std::string name, std::string props = {}) {
  return BlockStateKey{std::move(name), std::move(props)};
}

// A FireWorld over a sparse cell map, with `is_sturdy` true for any cell holding
// a block other than air — this server has no collision shapes, so that is the
// same predicate the server passes (see world/fire.h).
struct Cell {
  BlockStateKey state;
  int x, y, z;
};

struct TestWorld {
  std::vector<Cell> cells;

  void put(const char* name, int x, int y, int z, const char* props = "") {
    cells.push_back(Cell{block(name, props), x, y, z});
  }

  FireWorld make() const {
    FireWorld world;
    const auto* cells_ptr = &cells;
    world.get_block = [cells_ptr](int x, int y, int z) {
      for (const Cell& cell : *cells_ptr)
        if (cell.x == x && cell.y == y && cell.z == z)
          return cell.state;
      return block("minecraft:air");
    };
    world.is_sturdy = [cells_ptr](int x, int y, int z) {
      for (const Cell& cell : *cells_ptr)
        if (cell.x == x && cell.y == y && cell.z == z)
          return cell.state.name != "minecraft:air";
      return false;
    };
    return world;
  }
};

// A *mutable* FireLevel over a sparse cell map, recording every write and every
// schedule. `is_sturdy` is the same "not air" predicate the server passes (see
// world/fire.h).
struct TestLevel {
  struct Write {
    int x, y, z;
    BlockStateKey state;
    bool notify;
  };

  std::map<std::array<int, 3>, BlockStateKey> cells;
  std::vector<Write> writes;
  std::vector<BlockTickPos> scheduled;
  std::vector<int> delays;
  // The weather the rain branches see. `rain_cells` empty + `raining` true means
  // it rains *everywhere* (the shape a real level hands over where the biome
  // precipitates and nothing roofs the cell); a cell list restricts it.
  bool raining = false;
  std::vector<std::array<int, 3>> rain_cells;

  void put(const char* name, int x, int y, int z, const char* props = "") {
    cells[{x, y, z}] = block(name, props);
  }

  bool rains_at(int x, int y, int z) const {
    if (!raining)
      return false;
    if (rain_cells.empty())
      return true;
    for (const auto& cell : rain_cells)
      if (cell[0] == x && cell[1] == y && cell[2] == z)
        return true;
    return false;
  }
  BlockStateKey at(int x, int y, int z) const {
    const auto it = cells.find({x, y, z});
    return it == cells.end() ? block("minecraft:air") : it->second;
  }

  FireLevel make() {
    FireLevel level;
    level.get_block = [this](int x, int y, int z) { return at(x, y, z); };
    level.is_sturdy = [this](int x, int y, int z) { return at(x, y, z).name != "minecraft:air"; };
    level.set_block = [this](int x, int y, int z, const BlockStateKey& state, bool notify) {
      writes.push_back(Write{x, y, z, state, notify});
      cells[{x, y, z}] = state;
    };
    level.schedule_tick = [this](int x, int y, int z, int delay) {
      scheduled.push_back(BlockTickPos{x, y, z});
      delays.push_back(delay);
    };
    level.is_raining = [this]() { return raining; };
    level.is_raining_at = [this](int x, int y, int z) { return rains_at(x, y, z); };
    return level;
  }

  size_t fire_cells() const {
    size_t n = 0;
    for (const auto& cell : cells)
      if (fire_is_fire(cell.second))
        ++n;
    return n;
  }
};

// A `FireRandom` whose int draws are all 0 and whose float draws are all 0.0f —
// the least-random streams there are, and the ones that take every
// `nextInt(bound) < odds` branch, every `> odds` skip, and the rain douse's
// `nextFloat() < 0.2f + age * 0.03f` in the direction that makes something
// happen.
FireRandom zero_rng() {
  FireRandom rng;
  rng.next_int = [](int) { return 0; };
  rng.next_float = []() { return 0.0f; };
  return rng;
}

// Every int draw comes back as `value`, clamped into [0, bound) so it stays a
// legal `nextInt` result. Floats are 0.0f.
FireRandom fixed_rng(int value) {
  FireRandom rng = zero_rng();
  rng.next_int = [value](int bound) { return bound > 0 ? std::min(value, bound - 1) : 0; };
  return rng;
}

// A scripted `FireRandom`: the first `head.size()` int draws return those values
// (each clamped into range), and every later int draw returns `tail`.
// `FireBlock.tick`'s draws are taken in a fixed order — the reschedule, the age
// roll, the six burn-out rolls, then the spread's — so a head long enough to
// cover the first three groups leaves the spread fully under the test's control.
// The float stream is scripted the same way through `floats` / `float_tail`.
FireRandom scripted_rng(std::vector<int> head, int tail, std::vector<float> floats = {},
                        float float_tail = 0.0f) {
  auto calls = std::make_shared<size_t>(0);
  auto values = std::make_shared<const std::vector<int>>(std::move(head));
  auto float_calls = std::make_shared<size_t>(0);
  auto float_values = std::make_shared<const std::vector<float>>(std::move(floats));
  FireRandom rng;
  rng.next_int = [calls, values, tail](int bound) {
    const size_t call = (*calls)++;
    const int raw = call < values->size() ? (*values)[call] : tail;
    return bound > 0 ? std::min(raw, bound - 1) : 0;
  };
  rng.next_float = [float_calls, float_values, float_tail]() {
    const size_t call = (*float_calls)++;
    return call < float_values->size() ? (*float_values)[call] : float_tail;
  };
  return rng;
}

void test_names() {
  CHECK(fire_is_soul_base(block("minecraft:soul_sand")));
  CHECK(fire_is_soul_base(block("minecraft:soul_soil")));
  CHECK(!fire_is_soul_base(block("minecraft:sand")));
  CHECK(fire_is_fire(block("minecraft:fire")));
  CHECK(fire_is_fire(block("minecraft:soul_fire")));
  CHECK(!fire_is_fire(block("minecraft:campfire")));
  CHECK(fire_is_air(block("minecraft:air")));
  CHECK(fire_is_air(block("minecraft:cave_air")));
  CHECK(fire_is_air(block("minecraft:void_air")));
  CHECK(!fire_is_air(block("minecraft:stone")));
}

void test_flammability_table() {
  CHECK_EQ(std::size(kFireFlammability), 175u);
  // Sorted by name, which is what makes the binary search valid.
  for (size_t i = 1; i < std::size(kFireFlammability); ++i)
    CHECK(std::string_view(kFireFlammability[i - 1].name) <
          std::string_view(kFireFlammability[i].name));
  // `FireBlock.bootStrap`'s own numbers for a few well-known blocks.
  CHECK_EQ(fire_ignite_odds("minecraft:oak_planks"), 5);
  CHECK_EQ(fire_ignite_odds("minecraft:oak_leaves"), 30);
  CHECK_EQ(fire_ignite_odds("minecraft:allium"), 60);
  CHECK_EQ(fire_ignite_odds("minecraft:coal_block"), 5);
  // Not in the table at all, and a prefix that must not match.
  CHECK_EQ(fire_ignite_odds("minecraft:stone"), 0);
  CHECK_EQ(fire_ignite_odds("minecraft:oak"), 0);
  CHECK_EQ(fire_ignite_odds(""), 0);
  // The burn odds the table carries for the spread that is not modelled.
  CHECK(kFireFlammability[0].burn_odds >= 0);

  CHECK(fire_block_can_burn(block("minecraft:oak_planks")));
  CHECK(fire_block_can_burn(block("minecraft:oak_planks", "waterlogged=false")));
  // `getIgniteOdds` is 0 for a waterlogged state, whatever its block says.
  CHECK(!fire_block_can_burn(block("minecraft:oak_planks", "waterlogged=true")));
  CHECK(!fire_block_can_burn(block("minecraft:stone")));
}

void test_placement_state() {
  // Soul fire over a soul-fire base block, whatever the rest of the world says.
  {
    TestWorld w;
    w.put("minecraft:soul_sand", 0, 63, 0);
    const BlockStateKey state = fire_placement_state(w.make(), 0, 64, 0);
    CHECK(state.name == "minecraft:soul_fire");
    CHECK(state.properties.empty());
  }
  // Something burnable below: the default age-0 state with no faces connected,
  // even though nothing around it burns.
  {
    TestWorld w;
    w.put("minecraft:oak_planks", 0, 63, 0);
    const BlockStateKey state = fire_placement_state(w.make(), 0, 64, 0);
    CHECK(state.name == "minecraft:fire");
    CHECK_STR(block_property(state.properties, "age"), "0");
    for (const char* face : {"up", "north", "south", "west", "east"})
      CHECK_STR(block_property(state.properties, face), "false");
  }
  // A sturdy (non-flammable) floor does the same: `canBurn || isSturdy`.
  {
    TestWorld w;
    w.put("minecraft:stone", 0, 63, 0);
    const BlockStateKey state = fire_placement_state(w.make(), 0, 64, 0);
    CHECK(state.name == "minecraft:fire");
    CHECK_STR(block_property(state.properties, "up"), "false");
  }
  // Neither: the five connection properties answer whether that neighbour burns.
  {
    TestWorld w;
    w.put("minecraft:oak_planks", 1, 64, 0); // east
    w.put("minecraft:oak_planks", 0, 64, 1); // south
    w.put("minecraft:oak_planks", 0, 65, 0); // up
    const BlockStateKey state = fire_placement_state(w.make(), 0, 64, 0);
    CHECK(state.name == "minecraft:fire");
    CHECK_STR(block_property(state.properties, "up"), "true");
    CHECK_STR(block_property(state.properties, "east"), "true");
    CHECK_STR(block_property(state.properties, "south"), "true");
    CHECK_STR(block_property(state.properties, "north"), "false");
    CHECK_STR(block_property(state.properties, "west"), "false");
  }
}

void test_survival_and_placement_check() {
  {
    TestWorld w;
    w.put("minecraft:stone", 0, 63, 0);
    CHECK(fire_can_survive(w.make(), 0, 64, 0));
  }
  {
    // Nothing below, but a burning neighbour is enough (`isValidFireLocation`).
    TestWorld w;
    w.put("minecraft:oak_planks", -1, 64, 0);
    CHECK(fire_can_survive(w.make(), 0, 64, 0));
  }
  {
    // Soul fire only wants its base block below.
    TestWorld w;
    w.put("minecraft:soul_soil", 0, 63, 0);
    CHECK(fire_can_survive(w.make(), 0, 64, 0));
  }
  {
    TestWorld w;
    CHECK(!fire_can_survive(w.make(), 0, 64, 0));
    // ...and an empty FireWorld (no get_block at all) can survive nowhere.
    CHECK(!fire_can_survive(FireWorld{}, 0, 64, 0));
  }
  {
    TestWorld w;
    w.put("minecraft:stone", 0, 63, 0);
    const FireWorld world = w.make();
    CHECK(fire_can_be_placed_at(world, 0, 64, 0)); // air above, survives
    w.put("minecraft:oak_planks", 0, 64, 0);       // now occupied
    const FireWorld occupied = w.make();
    CHECK(!fire_can_be_placed_at(occupied, 0, 64, 0));
    CHECK(!fire_can_be_placed_at(FireWorld{}, 0, 64, 0));
  }
}

void test_douse_geometry() {
  // `Direction`'s ordinals as the wire numbers them.
  static const int kExpected[6][3] = {{0, -1, 0}, {0, 1, 0},  {0, 0, -1},
                                      {0, 0, 1},  {-1, 0, 0}, {1, 0, 0}};
  for (int face = 0; face < 6; ++face) {
    int dx = 99, dy = 99, dz = 99;
    block_face_step(face, &dx, &dy, &dz);
    CHECK_EQ(dx, kExpected[face][0]);
    CHECK_EQ(dy, kExpected[face][1]);
    CHECK_EQ(dz, kExpected[face][2]);
  }
  // A bottle that broke on the *up* face of (10, 64, 20): the cell it was in is
  // (10, 65, 20), the block it hit is (10, 64, 20), then that cell's four
  // horizontals in `Direction.Plane.HORIZONTAL`'s own order (north, east,
  // south, west).
  const std::vector<FireBlockPos> positions = fire_douse_positions(10, 64, 20, 1);
  CHECK_EQ(positions.size(), 6u);
  const int expected[6][3] = {{10, 65, 20}, {10, 64, 20}, {10, 65, 19},
                              {11, 65, 20}, {10, 65, 21}, {9, 65, 20}};
  for (size_t i = 0; i < positions.size(); ++i) {
    CHECK_EQ(positions[i].x, expected[i][0]);
    CHECK_EQ(positions[i].y, expected[i][1]);
    CHECK_EQ(positions[i].z, expected[i][2]);
  }
  // A sideways hit: face 3 is south (+z), so the cell the bottle was in is the
  // one *past* the block it hit.
  const std::vector<FireBlockPos> south = fire_douse_positions(0, 5, 0, 3);
  CHECK_EQ(south[0].x, 0);
  CHECK_EQ(south[0].y, 5);
  CHECK_EQ(south[0].z, 1);
}

void test_douse_kinds() {
  // `dowseFire`'s order of tests: fire, then a lit campfire, then a lit candle.
  CHECK(fire_douse_kind(block("minecraft:fire")) == DouseKind::kFire);
  CHECK(fire_douse_kind(block("minecraft:soul_fire")) == DouseKind::kFire);
  CHECK(fire_douse_kind(block("minecraft:campfire", "facing=north,lit=true")) ==
        DouseKind::kCampfire);
  CHECK(fire_douse_kind(block("minecraft:soul_campfire", "lit=true")) == DouseKind::kCampfire);
  // An unlit campfire is *not* a douse: `CampfireBlock.isLitCampfire`.
  CHECK(fire_douse_kind(block("minecraft:campfire", "facing=north,lit=false")) == DouseKind::kNone);
  CHECK(fire_douse_kind(block("minecraft:candle", "candles=1,lit=true")) == DouseKind::kCandle);
  CHECK(fire_douse_kind(block("minecraft:white_candle_cake", "lit=true")) == DouseKind::kCandle);
  CHECK(fire_douse_kind(block("minecraft:candle", "candles=1,lit=false")) == DouseKind::kNone);
  CHECK(fire_douse_kind(block("minecraft:stone")) == DouseKind::kNone);
  // A *cake* named `_candle_cake` but unlit is nothing, and a plain cake is not
  // a candle at all.
  CHECK(fire_douse_kind(block("minecraft:white_candle_cake", "lit=false")) == DouseKind::kNone);
  CHECK(fire_douse_kind(block("minecraft:cake", "bites=0")) == DouseKind::kNone);

  // `state.setValue(LIT, false)` keeps every other property.
  const BlockStateKey doused = fire_doused_state(
      block("minecraft:campfire", "facing=north,lit=true,signal_fire=false,waterlogged=false"));
  CHECK(doused.name == "minecraft:campfire");
  CHECK_STR(block_property(doused.properties, "lit"), "false");
  CHECK_STR(block_property(doused.properties, "facing"), "north");
  CHECK_STR(block_property(doused.properties, "signal_fire"), "false");
  CHECK_STR(block_property(doused.properties, "waterlogged"), "false");
}

void test_can_light() {
  CHECK(fire_can_light(block("minecraft:campfire", "facing=north,lit=false,waterlogged=false")));
  // A waterlogged campfire cannot be lit (CampfireBlock.canLight).
  CHECK(!fire_can_light(block("minecraft:campfire", "facing=north,lit=false,waterlogged=true")));
  CHECK(!fire_can_light(block("minecraft:campfire", "facing=north,lit=true,waterlogged=false")));
  CHECK(fire_can_light(block("minecraft:candle", "candles=3,lit=false,waterlogged=false")));
  CHECK(!fire_can_light(block("minecraft:candle", "candles=3,lit=false,waterlogged=true")));
  CHECK(fire_can_light(block("minecraft:white_candle_cake", "lit=false")));
  CHECK(!fire_can_light(block("minecraft:white_candle_cake", "lit=true")));
  CHECK(!fire_can_light(block("minecraft:stone")));

  const BlockStateKey lit = fire_lit_state(block("minecraft:campfire", "facing=west,lit=false"));
  CHECK_STR(block_property(lit.properties, "lit"), "true");
  CHECK_STR(block_property(lit.properties, "facing"), "west");
}

void test_burning_countdown() {
  Burning b;
  CHECK_EQ(b.remaining_ticks, 0);
  CHECK(!b.on_fire());
  b.ignite_seconds(kFireSecondsOnFire);
  CHECK_EQ(b.remaining_ticks, 160);
  CHECK(b.on_fire());
  // `igniteForTicks` only ever raises.
  b.ignite_ticks(40);
  CHECK_EQ(b.remaining_ticks, 160);
  b.ignite_ticks(400);
  CHECK_EQ(b.remaining_ticks, 400);

  // `baseTick`: 400 % 20 == 0, so this tick is the 1.0 `on_fire` one.
  CHECK(b.tick(false));
  CHECK_EQ(b.remaining_ticks, 399);
  CHECK(!b.tick(false));
  CHECK_EQ(b.remaining_ticks, 398);
  // In lava the countdown still falls; only the damage is suppressed.
  {
    Burning lava;
    lava.ignite_ticks(20);
    CHECK(!lava.tick(false, /*in_lava=*/true));
    CHECK_EQ(lava.remaining_ticks, 19);
  }
  // A fire-immune entity clears instead of counting down.
  {
    Burning immune;
    immune.ignite_ticks(100);
    CHECK(!immune.tick(/*fire_immune=*/true));
    CHECK_EQ(immune.remaining_ticks, 0);
  }

  // `clearFire` keeps a negative countdown; a positive one goes to zero.
  {
    Burning clearing;
    clearing.remaining_ticks = -7;
    clearing.clear();
    CHECK_EQ(clearing.remaining_ticks, -7);
    clearing.remaining_ticks = 7;
    clearing.clear();
    CHECK_EQ(clearing.remaining_ticks, 0);
  }

  // A player idles at `-getFireImmuneTicks()`; a mob at 0.
  {
    Burning player = player_burning();
    CHECK_EQ(player.immune_ticks, kPlayerFireImmuneTicks);
    CHECK_EQ(player.remaining_ticks, 0);
    player.reset_if_not_burning(false);
    CHECK_EQ(player.remaining_ticks, -20);
    // ...unless this tick's block pass ignited it.
    player.reset_if_not_burning(true);
    CHECK_EQ(player.remaining_ticks, -20);
  }
  {
    Burning mob;
    CHECK_EQ(mob.immune_ticks, 0);
    mob.reset_if_not_burning(false);
    CHECK_EQ(mob.remaining_ticks, 0);
  }
}

void test_fire_ignite() {
  // A player standing in fire: the first tick only nudges the countdown from
  // -20 to -19, and it takes all twenty of them to reach zero and catch.
  {
    Burning b = player_burning();
    b.reset_if_not_burning(false);
    for (int i = 0; i < 19; ++i) {
      CHECK(fire_ignite(b, /*fire_immune=*/false, /*is_player=*/true, 1));
      CHECK_EQ(b.remaining_ticks, -20 + (i + 1));
      CHECK(!b.on_fire());
    }
    CHECK(fire_ignite(b, false, true, 1));
    CHECK_EQ(b.remaining_ticks, 160);
    CHECK(b.on_fire());
  }
  // A player *already* at or above zero has `nextInt(1, 3)` added: the draw's
  // value moves the countdown, then the eight-second ignite lands either way.
  {
    Burning b = player_burning();
    b.remaining_ticks = 0;
    CHECK(fire_ignite(b, false, true, 2));
    CHECK_EQ(b.remaining_ticks, 160);
  }
  // A mob has no grace period: idling at 0, the first tick catches and ignites
  // for the full eight seconds.
  {
    Burning b;
    CHECK(fire_ignite(b, /*fire_immune=*/false, /*is_player=*/false, 0));
    CHECK_EQ(b.remaining_ticks, 160);
  }
  // A fire-immune entity is untouched and reports no ignition.
  {
    Burning b;
    b.remaining_ticks = -5;
    CHECK(!fire_ignite(b, /*fire_immune=*/true, true, 2));
    CHECK_EQ(b.remaining_ticks, -5);
  }
}

void test_entity_inside() {
  {
    TestWorld w;
    w.put("minecraft:fire", 0, 64, 0, "age=0");
    const FireWorld world = w.make();
    float damage = -1.0f;
    // A player's box standing in the cell.
    CHECK(fire_entity_inside(world, -0.3, 64.0, -0.3, 0.3, 65.8, 0.3, &damage));
    CHECK_EQ(damage, 1.0f);
    // Soul fire is worth 2.0.
    TestWorld soul;
    soul.put("minecraft:soul_fire", 0, 64, 0);
    float soul_damage = -1.0f;
    CHECK(fire_entity_inside(soul.make(), -0.3, 64.0, -0.3, 0.3, 65.8, 0.3, &soul_damage));
    CHECK_EQ(soul_damage, 2.0f);
    // The "last block wins" rule: a box spanning fire then soul fire reports 2.0.
    TestWorld both;
    both.put("minecraft:fire", 0, 63, 0);
    both.put("minecraft:soul_fire", 0, 64, 0);
    float top = -1.0f;
    CHECK(fire_entity_inside(both.make(), -0.3, 63.0, -0.3, 0.3, 65.0, 0.3, &top));
    CHECK_EQ(top, 2.0f);
  }
  {
    // A box resting *exactly* on the fire cell's boundary does not reach into
    // it: `BlockPos.containing(box.max - 1.0E-7)`.
    TestWorld w;
    w.put("minecraft:fire", 0, 64, 0);
    float damage = 0.0f;
    CHECK(!fire_entity_inside(w.make(), -0.3, 63.0, -0.3, 0.3, 64.0, 0.3, &damage));
    // ...and a box one cell over never sees it.
    CHECK(!fire_entity_inside(w.make(), -0.3, 64.0, 5.0, 0.3, 65.8, 5.6, &damage));
  }
  {
    // An empty FireWorld has no blocks at all.
    float damage = 0.0f;
    CHECK(!fire_entity_inside(FireWorld{}, -0.3, 64.0, -0.3, 0.3, 65.8, 0.3, &damage));
    // A non-fire block is not fire.
    TestWorld w;
    w.put("minecraft:stone", 0, 64, 0);
    CHECK(!fire_entity_inside(w.make(), -0.3, 64.0, -0.3, 0.3, 65.8, 0.3, &damage));
  }
}

void test_flammability_burn_odds() {
  // The burn column `FireBlock.getBurnOdds` reads, waterlogged refusal included.
  CHECK_EQ(fire_burn_odds(block("minecraft:oak_planks")), 20);
  CHECK_EQ(fire_burn_odds(block("minecraft:oak_log")), 5);
  CHECK_EQ(fire_burn_odds(block("minecraft:oak_leaves")), 60);
  CHECK_EQ(fire_burn_odds(block("minecraft:hay_block")), 20);
  CHECK_EQ(fire_burn_odds(block("minecraft:tnt")), 100);
  CHECK_EQ(fire_burn_odds(block("minecraft:oak_planks", "waterlogged=true")), 0);
  CHECK_EQ(fire_burn_odds(block("minecraft:stone")), 0);
  // `#minecraft:infiniburn_overworld`.
  CHECK(fire_is_infiniburn(block("minecraft:netherrack")));
  CHECK(fire_is_infiniburn(block("minecraft:magma_block")));
  CHECK(!fire_is_infiniburn(block("minecraft:stone")));
  // `state.getValue(FireBlock.AGE)`.
  CHECK_EQ(fire_age_of(block("minecraft:fire", "age=7")), 7);
  CHECK_EQ(fire_age_of(block("minecraft:fire", "age=15")), 15);
  CHECK_EQ(fire_age_of(block("minecraft:fire")), 0);
  CHECK_EQ(fire_age_of(block("minecraft:soul_fire")), 0);
}

void test_block_tick_queue() {
  BlockTickQueue queue;
  CHECK(queue.empty());
  CHECK_EQ(queue.now(), 0);
  queue.schedule(0, 64, 0, 3);
  CHECK_EQ(queue.size(), 1u);
  CHECK_EQ(queue.chunk_count(), 1u);
  CHECK(queue.advance().empty());                  // now = 1
  CHECK(queue.advance().empty());                  // now = 2
  std::vector<BlockTickPos> due = queue.advance(); // now = 3
  CHECK_EQ(due.size(), 1u);
  CHECK((due[0] == BlockTickPos{0, 64, 0}));
  CHECK(queue.empty());
  CHECK_EQ(queue.chunk_count(), 0u);

  // One pending entry per position: a re-schedule rewrites the due time rather
  // than stacking a second tick (`LevelChunkTicks.schedule`).
  queue.schedule(0, 64, 0, 5); // due 8
  queue.schedule(0, 64, 0, 2); // due 5 wins
  CHECK_EQ(queue.size(), 1u);
  CHECK(queue.advance().empty());       // now = 4
  CHECK_EQ(queue.advance().size(), 1u); // now = 5

  // ...and the buckets really are per chunk: two positions 20 apart in x land in
  // different ones (0 and 1) and are drained without walking either.
  queue.schedule(0, 64, 0, 1);
  queue.schedule(20, 64, 0, 1);
  CHECK_EQ(queue.size(), 2u);
  CHECK_EQ(queue.chunk_count(), 2u);
  CHECK_EQ(queue.advance().size(), 2u);
  CHECK(queue.empty());

  // A negative coordinate buckets by floor division, not truncation: (-1, -17)
  // lands in chunk (-1, -2), which is its own bucket.
  queue.schedule(-1, 64, -17, 1);
  CHECK_EQ(queue.chunk_count(), 1u);
  CHECK_EQ(queue.advance().size(), 1u);
}

void test_fire_tick_schedules_itself() {
  TestLevel w;
  w.put("minecraft:stone", 0, 63, 0);
  w.put("minecraft:fire", 0, 64, 0);
  FireLevel level = w.make();
  fire_tick(level, zero_rng(), kDifficultyNormal, 0, 64, 0);
  // `scheduleTick(pos, this, 30 + nextInt(10))`, first thing, unconditionally.
  CHECK_EQ(w.scheduled.size(), 1u);
  CHECK((w.scheduled[0] == BlockTickPos{0, 64, 0}));
  CHECK_EQ(w.delays[0], kFireTickMinDelay + 0);
  // A fixed draw of 9 lands the delay at the top of the range.
  TestLevel w2;
  w2.put("minecraft:stone", 0, 63, 0);
  w2.put("minecraft:fire", 0, 64, 0);
  FireLevel level2 = w2.make();
  fire_tick(level2, fixed_rng(9), kDifficultyNormal, 0, 64, 0);
  CHECK_EQ(w2.delays[0], kFireTickMinDelay + kFireTickDelaySpread - 1);
}

void test_fire_tick_ignores_non_fire() {
  // A stale queue entry: the block that scheduled the tick is gone.
  TestLevel w;
  w.put("minecraft:stone", 0, 64, 0);
  FireLevel level = w.make();
  fire_tick(level, zero_rng(), kDifficultyNormal, 0, 64, 0);
  CHECK(w.scheduled.empty());
  CHECK(w.writes.empty());
  // Soul fire has no tick of its own and never reschedules either.
  TestLevel soul;
  soul.put("minecraft:soul_fire", 0, 64, 0);
  soul.put("minecraft:soul_soil", 0, 63, 0);
  FireLevel soul_level = soul.make();
  fire_tick(soul_level, zero_rng(), kDifficultyNormal, 0, 64, 0);
  CHECK(soul.scheduled.empty());
  CHECK(soul.writes.empty());
}

void test_fire_tick_ages() {
  TestLevel w;
  w.put("minecraft:stone", 0, 63, 0);
  w.put("minecraft:fire", 0, 64, 0, "age=2");
  FireLevel level = w.make();
  // `nextInt(3) == 2`, so `2 / 2 == 1` and the age climbs to 3.
  fire_tick(level, fixed_rng(2), kDifficultyNormal, 0, 64, 0);
  CHECK_STR(w.at(0, 64, 0).properties, "age=3");
  // That write is vanilla's flags-260 one: the client is *not* told.
  CHECK_EQ(w.writes.size(), 1u);
  CHECK(!w.writes[0].notify);
  // A draw of 0 or 1 leaves the age alone (`nextInt(3) / 2` is 0 either way).
  for (int draw : {0, 1}) {
    TestLevel still;
    still.put("minecraft:stone", 0, 63, 0);
    still.put("minecraft:fire", 0, 64, 0, "age=2");
    FireLevel still_level = still.make();
    fire_tick(still_level, fixed_rng(draw), kDifficultyNormal, 0, 64, 0);
    CHECK_STR(still.at(0, 64, 0).properties, "age=2");
    CHECK(still.writes.empty());
  }
  // `MAX_AGE` is the ceiling: 15 + nextInt(3)/2 is clamped back to 15, so there
  // is not even a write. Netherrack below keeps the validity and burn-out checks
  // from removing the fire (a MAX_AGE fire over a sturdy, unflammable floor would
  // die on the `age > 3` half of `canSurvive`).
  TestLevel top;
  top.put("minecraft:netherrack", 0, 63, 0);
  top.put("minecraft:fire", 0, 64, 0, "age=15");
  FireLevel top_level = top.make();
  fire_tick(top_level, fixed_rng(2), kDifficultyNormal, 0, 64, 0);
  CHECK_STR(top.at(0, 64, 0).properties, "age=15");
  CHECK(top.writes.empty());
}

void test_fire_tick_removes_without_support() {
  // Nothing below and nothing burning beside it: `canSurvive` fails and the
  // fire is gone (twice over — vanilla keeps going after the removal).
  TestLevel w;
  w.put("minecraft:fire", 0, 64, 0);
  FireLevel level = w.make();
  fire_tick(level, zero_rng(), kDifficultyNormal, 0, 64, 0);
  CHECK(w.at(0, 64, 0).name == "minecraft:air");
  CHECK(!w.writes.empty());
  // Every removal tells the client: flags 3, not 260.
  for (const TestLevel::Write& write : w.writes)
    CHECK(write.notify);
  CHECK_EQ(w.fire_cells(), 0u);
}

void test_fire_tick_burns_out_the_block_below() {
  // A fire sitting on oak planks: the below-neighbour burn-out roll (chance 250
  // against the planks' burn odds of 20) succeeds and the planks become fire.
  TestLevel w;
  w.put("minecraft:oak_planks", 0, 63, 0);
  w.put("minecraft:fire", 0, 64, 0);
  w.put("minecraft:stone", 0, 62, 0);
  FireLevel level = w.make();
  fire_tick(level, zero_rng(), kDifficultyNormal, 0, 64, 0);
  CHECK_STR(w.at(0, 63, 0).name, std::string("minecraft:fire"));
  // The block that burned away is gone from the world as a plank.
  CHECK(!fire_block_can_burn(w.at(0, 63, 0)));
  // The new fire is scheduled by the same onPlace rule a placement would use.
  bool scheduled_below = false;
  for (const BlockTickPos& pos : w.scheduled)
    if (pos == BlockTickPos{0, 63, 0})
      scheduled_below = true;
  CHECK(scheduled_below);
  // Stone has burn odds 0, so the roll against the stone it sits on never fires
  // *that* branch: the only change is the one the planks gave up.
  CHECK(w.at(0, 62, 0).name == "minecraft:stone");
}

void test_fire_tick_burns_a_block_away() {
  // The other half of `checkBurnOut`: the roll passes but the second draw says
  // "remove". A draw of 6 is >= the 5 threshold, so the block is destroyed.
  TestLevel w;
  w.put("minecraft:oak_planks", 0, 63, 0);
  w.put("minecraft:fire", 0, 64, 0);
  FireLevel level = w.make();
  fire_tick(level, fixed_rng(6), kDifficultyNormal, 0, 64, 0);
  CHECK(w.at(0, 63, 0).name == "minecraft:air");
}

void test_fire_tick_spreads_into_a_flammable_neighbour() {
  // Fire directly beside an oak plank. The plank is what makes
  // `isValidFireLocation` true — without a burning *neighbour* `tick` returns
  // before the spread loop — and it is also what gives the cells around it the
  // ignite odds `getIgniteOdds(LevelReader, BlockPos)` reads.
  TestLevel w;
  w.put("minecraft:stone", 0, 63, 0);
  w.put("minecraft:oak_planks", 1, 64, 0);
  w.put("minecraft:fire", 0, 64, 0);
  FireLevel level = w.make();
  // The first eight draws are the reschedule, the age roll and the six burn-out
  // rolls: all high, so every one of them fails and the plank survives. Every
  // draw after that is 0, which clears the spread odds.
  fire_tick(level, scripted_rng({99, 99, 99, 99, 99, 99, 99, 99}, 0), kDifficultyNormal, 0, 64, 0);
  // The plank was *not* consumed...
  CHECK_STR(w.at(1, 64, 0).name, std::string("minecraft:oak_planks"));
  // ...and the fire jumped into one of the empty cells beside the plank (all of
  // which are inside the tick's -1..1 x -1..1 z -1..4 y box).
  const bool spread =
      w.at(1, 63, 0).name == "minecraft:fire" || w.at(1, 65, 0).name == "minecraft:fire" ||
      w.at(1, 64, -1).name == "minecraft:fire" || w.at(1, 64, 1).name == "minecraft:fire";
  CHECK(spread);
  CHECK(w.fire_cells() > 1);
  // Nothing spreads when nothing nearby is flammable: the fire survives (a
  // sturdy floor below it) but has nowhere to go.
  TestLevel bare;
  bare.put("minecraft:stone", 0, 63, 0);
  bare.put("minecraft:fire", 0, 64, 0);
  FireLevel bare_level = bare.make();
  fire_tick(bare_level, zero_rng(), kDifficultyNormal, 0, 64, 0);
  CHECK_EQ(bare.fire_cells(), 1u);
}

void test_fire_tick_infiniburn() {
  // Netherrack below: neither the validity check nor the burn-out runs, so a
  // fire with nothing but netherrack to feed on simply keeps burning.
  TestLevel w;
  w.put("minecraft:netherrack", 0, 63, 0);
  w.put("minecraft:fire", 0, 64, 0, "age=0");
  FireLevel level = w.make();
  fire_tick(level, zero_rng(), kDifficultyNormal, 0, 64, 0);
  CHECK(w.at(0, 64, 0).name == "minecraft:fire");
  CHECK_STR(w.at(0, 63, 0).name, std::string("minecraft:netherrack"));
  CHECK_EQ(w.fire_cells(), 1u);

  // The contrast: the same shape without the infiniburn block below dies.
  TestLevel plain;
  plain.put("minecraft:fire", 0, 64, 0);
  FireLevel plain_level = plain.make();
  fire_tick(plain_level, zero_rng(), kDifficultyNormal, 0, 64, 0);
  CHECK_EQ(plain.fire_cells(), 0u);
}

void test_fire_tick_difficulty_scales_the_spread() {
  // `(igniteOdds + 40 + difficulty * 7) / (age + 30)` at age 0, against an oak
  // plank's ignite odds of 5: normal (2) gives 59 / 30 = 1 and hard (3) gives
  // 66 / 30 = 2. A spread roll of exactly 2 is refused by the first and clears the
  // second — the difficulty is the only thing that decides it.
  for (int difficulty = kDifficultyNormal; difficulty <= kDifficultyHard; ++difficulty) {
    TestLevel w;
    w.put("minecraft:stone", 0, 63, 0);
    w.put("minecraft:oak_planks", 1, 64, 0);
    w.put("minecraft:fire", 0, 64, 0);
    FireLevel level = w.make();
    fire_tick(level, scripted_rng({99, 99, 99, 99, 99, 99, 99, 99}, 2), difficulty, 0, 64, 0);
    if (difficulty == kDifficultyNormal)
      CHECK_EQ(w.fire_cells(), 1u); // 2 > 1: no spread at all
    else
      CHECK(w.fire_cells() > 1); // 2 <= 2: the spread lands
  }
}

// `FireBlock.tick`'s rain removal: `!infiniBurn && isRaining() &&
// isNearRain(pos) && nextFloat() < 0.2f + age * 0.03f` removes the block and
// returns *before* the ageing, so a fire that is rained out does not also age.
void test_fire_tick_rain_douses() {
  // Age 5: the chance is 0.2 + 0.15 = 0.35, so a 0.1 draw is a douse and a 0.9 is
  // not. The int stream is all 99s so nothing else can happen first.
  {
    TestLevel w;
    w.put("minecraft:stone", 0, 63, 0);
    w.put("minecraft:fire", 0, 64, 0, "age=5");
    w.raining = true;
    FireLevel level = w.make();
    fire_tick(level, scripted_rng({99, 99, 99, 99, 99, 99, 99, 99}, 99, {0.1f}, 0.0f),
              kDifficultyNormal, 0, 64, 0);
    CHECK_EQ(w.fire_cells(), 0u);
    // The removal is the tick's last word: the age write never happened, so the
    // only write is the removal itself.
    CHECK_EQ(w.writes.size(), 1u);
    CHECK_STR(w.writes[0].state.name, "minecraft:air");
    CHECK(w.writes[0].notify);
    // ...and it still rescheduled itself first, like every tick does.
    CHECK_EQ(w.scheduled.size(), 1u);
  }
  // A draw above the line is a miss: at age 0 the line is 0.2, so 0.9 leaves the
  // fire burning (and young enough to survive on its own — a fire past age 3
  // with nothing to feed on dies by the survival rule, not by the rain).
  {
    TestLevel w;
    w.put("minecraft:stone", 0, 63, 0);
    w.put("minecraft:fire", 0, 64, 0, "age=0");
    w.raining = true;
    FireLevel level = w.make();
    fire_tick(level, scripted_rng({99, 99, 99, 99, 99, 99, 99, 99}, 99, {0.9f}, 0.0f),
              kDifficultyNormal, 0, 64, 0);
    CHECK_EQ(w.fire_cells(), 1u); // 0.9 > 0.2: not doused
  }
  // ...and the line really moves with the age: at age 14 it is 0.2 + 0.42 = 0.62,
  // so the same 0.5 draw a young fire shrugs off puts this one out (this fire has
  // a plank to feed on, so only the rain can end it).
  {
    TestLevel w;
    w.put("minecraft:stone", 0, 63, 0);
    w.put("minecraft:oak_planks", 1, 64, 0);
    w.put("minecraft:fire", 0, 64, 0, "age=14");
    w.raining = true;
    FireLevel level = w.make();
    fire_tick(level, scripted_rng({99, 99, 99, 99, 99, 99, 99, 99}, 99, {0.5f}, 0.0f),
              kDifficultyNormal, 0, 64, 0);
    CHECK_EQ(w.fire_cells(), 0u); // 0.5 < 0.62: doused
  }
  // Rain one cell away (a horizontal neighbour) counts: `isNearRain` is the cell
  // or its four sides, so a one-wide gap does not save a fire.
  {
    TestLevel w;
    w.put("minecraft:stone", 0, 63, 0);
    w.put("minecraft:fire", 0, 64, 0, "age=0");
    w.raining = true;
    w.rain_cells = {{1, 64, 0}};
    FireLevel level = w.make();
    fire_tick(level, scripted_rng({99, 99, 99, 99, 99, 99, 99, 99}, 99, {0.0f}, 0.0f),
              kDifficultyNormal, 0, 64, 0);
    CHECK_EQ(w.fire_cells(), 0u);
  }
  // And rain out of reach does nothing: the two cells east of the fire are not
  // near it.
  {
    TestLevel w;
    w.put("minecraft:stone", 0, 63, 0);
    w.put("minecraft:fire", 0, 64, 0, "age=0");
    w.raining = true;
    w.rain_cells = {{4, 64, 4}};
    FireLevel level = w.make();
    fire_tick(level, scripted_rng({99, 99, 99, 99, 99, 99, 99, 99}, 99, {0.0f}, 0.0f),
              kDifficultyNormal, 0, 64, 0);
    CHECK_EQ(w.fire_cells(), 1u);
  }
}

// The same gate on the spread: `level.isRaining() && isNearRain(testPos)` keeps a
// fire from *starting* in a cell it is raining on. The odds roll is drawn either
// way, so the weather changes no draw count — only the block that results.
void test_fire_tick_rain_blocks_the_spread() {
  // The spread recipe from test_fire_tick_spreads_into_a_flammable_neighbour: a
  // head long enough to cover the reschedule, the age roll and the six burn-out
  // rolls, then a 0 tail that clears every odds roll.
  const std::vector<int> head(8, 99);
  auto spread_cells = [&head](bool raining) {
    TestLevel w;
    w.put("minecraft:stone", 0, 63, 0);
    w.put("minecraft:oak_planks", 1, 64, 0);
    w.put("minecraft:fire", 0, 64, 0);
    w.raining = raining;
    FireLevel level = w.make();
    fire_tick(level, scripted_rng(head, 0, {0.9f}, 0.9f), kDifficultyNormal, 0, 64, 0);
    return w.fire_cells();
  };
  CHECK(spread_cells(false) > 1u);
  CHECK_EQ(spread_cells(true), 1u);
}

// `checkBurnOut`'s rain half: a cell it is raining on is consumed but not left
// alight — the fire takes the removal branch instead of re-ageing.
void test_check_burn_out_rain_removes() {
  auto neighbour_state = [](bool raining) {
    TestLevel w;
    w.put("minecraft:stone", 0, 63, 0);
    w.put("minecraft:oak_planks", 1, 64, 0); // burn odds 20
    w.put("minecraft:fire", 0, 64, 0, "age=0");
    w.raining = raining;
    w.rain_cells = {{1, 64, 0}};
    FireLevel level = w.make();
    // Ints all 0: `nextInt(300) < 20` consumes the plank and `nextInt(10) < 5`
    // would re-age it. The 0.9 float keeps the fire itself from being doused
    // (its chance at age 0 is 0.2).
    fire_tick(level, scripted_rng({}, 0, {0.9f}, 0.9f), kDifficultyNormal, 0, 64, 0);
    return w.at(1, 64, 0);
  };
  CHECK_STR(neighbour_state(false).name, "minecraft:fire");
  CHECK_STR(neighbour_state(true).name, "minecraft:air");
}

// `Entity.applyEffectsFromBlocks`: standing in rain puts an entity out, and then
// the countdown's tail resets it to its idle depth.
void test_burning_rain_clear() {
  Burning player = player_burning();
  player.ignite_seconds(kFireSecondsOnFire);
  CHECK(player.on_fire());
  player.reset_if_not_burning(/*ignited_this_tick=*/false, /*in_rain=*/true);
  CHECK(!player.on_fire());
  CHECK_EQ(player.remaining_ticks, -kPlayerFireImmuneTicks); // -getFireImmuneTicks()
  // A mob idles at 0 rather than at a player's -20, so being rained out leaves it
  // exactly where it started.
  Burning mob;
  mob.ignite_seconds(kFireSecondsOnFire);
  mob.reset_if_not_burning(false, true);
  CHECK_EQ(mob.remaining_ticks, 0);
  // A cell the rain does not reach leaves the fire alone.
  Burning dry = player_burning();
  dry.ignite_seconds(kFireSecondsOnFire);
  dry.reset_if_not_burning(false, /*in_rain=*/false);
  CHECK(dry.on_fire());
}

// A `with_property` sanity check, since `fire_doused_state` and `fire_lit_state`
// are built on it and a key-sorted result is what makes the state look up.
void test_property_helper() {
  const std::string props = with_property("facing=north,lit=true", "lit", "false");
  CHECK_STR(block_property(props, "lit"), "false");
  CHECK_STR(block_property(props, "facing"), "north");
}

} // namespace

int main() {
  test_names();
  test_flammability_table();
  test_flammability_burn_odds();
  test_placement_state();
  test_survival_and_placement_check();
  test_block_tick_queue();
  test_fire_tick_schedules_itself();
  test_fire_tick_ignores_non_fire();
  test_fire_tick_ages();
  test_fire_tick_removes_without_support();
  test_fire_tick_burns_out_the_block_below();
  test_fire_tick_burns_a_block_away();
  test_fire_tick_spreads_into_a_flammable_neighbour();
  test_fire_tick_infiniburn();
  test_fire_tick_difficulty_scales_the_spread();
  test_fire_tick_rain_douses();
  test_fire_tick_rain_blocks_the_spread();
  test_check_burn_out_rain_removes();
  test_burning_rain_clear();
  test_douse_geometry();
  test_douse_kinds();
  test_can_light();
  test_burning_countdown();
  test_fire_ignite();
  test_entity_inside();
  test_property_helper();
  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("fire: all checks passed\n");
  return 0;
}
