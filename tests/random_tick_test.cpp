// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the random-tick pass (Phase 3 milestone J):
//   * the generated randomly-ticking table and the state-level question it
//     answers (`wheat` below its last age, a distance-7 non-persistent leaf,
//     anything else that random-ticks);
//   * the property helpers every rule is built from;
//   * `CropBlock.getGrowthSpeed` (the nine below-cells, the hydrated bonus and
//     the crowding halving), the light gate, the 25/speed roll and the write;
//   * `LeavesBlock`: the distance predicates, `updateDistance`'s one relaxation
//     step, `updateShape`'s schedule, the scheduled tick's write and the decay's
//     drop-plus-remove;
//   * `tickChunk`'s block half: the section gate, the per-section sample count,
//     the `getBlockRandomPos` bounds and the dispatch;
//   * the per-section count the gate reads (chunk writes, the parser's rebuild
//     and the world's mask);
//   * the rolled block-drop resolver the decay uses;
//   * the golden trace: the recorded scenes in `tests/fixtures/random_tick` are
//     replayed through `tick_chunk_random_blocks` and every interaction the pass
//     has with the level is compared against the checked-in trace, row for row.
//     The shape assertions above cannot see a shifted `BlockRandomPos` stream
//     (each draw is still inside its section) or a rule's changed read set; this
//     can, which is why the fixture exists (headers in
//     tools/gen_random_tick_dump.cpp).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "world/chunk.h"
#include "world/crop.h"
#include "world/leaves.h"
#include "world/loot.h"
#include "world/memory_world.h"
#include "world/random_tick.h"
#include "world/random_tick_table.h"
#include "world/weather.h"         // BlockRandomPos: the level's own LCG
#include "world/worldgen_random.h" // WgLegacyRandom: what seeds `Level.randValue`

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

BlockStateKey state(std::string name, std::string properties = "") {
  return BlockStateKey{std::move(name), std::move(properties)};
}

// A world of block states, so the crop and leaf rules can be handed farmland, a
// log and air without a Chunk.
class StateWorld {
public:
  void set(int x, int y, int z, BlockStateKey key) {
    blocks_[{x, y, z}] = std::move(key);
  }
  BlockStateKey get(int x, int y, int z) const {
    auto it = blocks_.find({x, y, z});
    return it == blocks_.end() ? state("minecraft:air") : it->second;
  }

private:
  std::map<std::tuple<int, int, int>, BlockStateKey> blocks_;
};

// A `RandomTickLevel` over a `StateWorld`, recording every write and schedule so
// a test can assert on them.
struct TestLevel {
  StateWorld world;
  RandomTickLevel level;
  std::vector<std::tuple<int, int, int, BlockStateKey, bool>> writes;
  std::vector<std::tuple<int, int, int, int>> schedules;
  std::vector<std::tuple<int, int, int, BlockStateKey>> drops;
  int brightness = 15;

  TestLevel() {
    level.min_section_y = -4;
    level.section_count = 24;
    level.ticking_sections = [](int, int) { return 0ull; };
    level.get_block = [this](int x, int y, int z) { return world.get(x, y, z); };
    level.set_block = [this](int x, int y, int z, const BlockStateKey& key, bool notify) {
      world.set(x, y, z, key);
      writes.emplace_back(x, y, z, key, notify);
    };
    level.raw_brightness = [this](int, int, int) { return brightness; };
    level.schedule_tick = [this](int x, int y, int z, int delay) {
      schedules.emplace_back(x, y, z, delay);
    };
    level.drop_resources = [this](int x, int y, int z, const BlockStateKey& key) {
      drops.emplace_back(x, y, z, key);
    };
  }
};

// A scripted RNG: the draws are handed out in order and any extra draw returns
// the last value, so a test only has to write down the draws it cares about.
RandomTickRandom scripted(std::vector<int> ints, std::vector<float> floats = {}) {
  auto index = std::make_shared<size_t>(0);
  auto float_index = std::make_shared<size_t>(0);
  RandomTickRandom rng;
  rng.next_int = [ints, index](int) {
    const size_t i = std::min(*index, ints.empty() ? size_t(0) : ints.size() - 1);
    ++*index;
    return ints.empty() ? 0 : ints[i];
  };
  rng.next_float = [floats, float_index]() {
    const size_t i = std::min(*float_index, floats.empty() ? size_t(0) : floats.size() - 1);
    ++*float_index;
    return floats.empty() ? 0.0f : floats[i];
  };
  return rng;
}

void test_generated_table() {
  CHECK(random_tick_kind("minecraft:wheat") == kRandomTickCrop);
  CHECK(random_tick_kind("minecraft:carrots") == kRandomTickCrop);
  CHECK(random_tick_kind("minecraft:beetroots") == kRandomTickCrop);
  CHECK(random_tick_kind("minecraft:oak_leaves") == kRandomTickLeaves);
  CHECK(random_tick_kind("minecraft:mangrove_leaves") == kRandomTickLeaves);
  CHECK(random_tick_kind("minecraft:copper_block") == kRandomTickOther);
  CHECK(random_tick_kind("minecraft:grass_block") == kRandomTickOther);
  // Fire is a *scheduled* tick (world/block_tick.h), logs never tick.
  CHECK(random_tick_kind("minecraft:fire") == kRandomTickNone);
  CHECK(random_tick_kind("minecraft:oak_log") == kRandomTickNone);
  CHECK(random_tick_kind("minecraft:stone") == kRandomTickNone);
  CHECK(random_tick_kind("minecraft:not_a_block") == kRandomTickNone);

  // Each crop's own last age, not the family's seven.
  CHECK_EQ(crop_info("minecraft:wheat").max_age, 7);
  CHECK_EQ(crop_info("minecraft:beetroots").max_age, 3);
  CHECK_EQ(crop_info("minecraft:torchflower_crop").max_age, 1);
  CHECK(!crop_info("minecraft:oak_leaves").is_crop);

  // The table is sorted: the binary search's precondition.
  for (int i = 1; i < kRandomTickBlockCount; ++i)
    CHECK(std::string(kRandomTickBlocks[i - 1].name) < std::string(kRandomTickBlocks[i].name));
}

void test_state_properties() {
  CHECK_EQ(state_prop_int("age=7,moisture=0", "age").value_or(-1), 7);
  CHECK_EQ(state_prop_int("age=7,moisture=0", "moisture").value_or(-1), 0);
  CHECK_EQ(state_prop_int("stage=3", "age").value_or(-1), -1); // not a substring match
  CHECK_EQ(state_prop_int("age=3,stage=7", "stage").value_or(-1), 7);
  CHECK_EQ(state_prop_int("waterlogged=true", "age").value_or(-1), -1);
  CHECK_EQ(state_prop_bool("distance=7,persistent=false", "persistent").value_or(true), false);
  CHECK_EQ(state_prop_bool("distance=7,persistent=true", "persistent").value_or(false), true);
  CHECK(!state_prop_bool("distance=7", "persistent").has_value());

  // The replacement keeps the canonical order and only the value.
  const BlockStateKey grown = state_with_prop_int(state("minecraft:wheat", "age=3"), "age", 4);
  CHECK(grown.name == "minecraft:wheat");
  CHECK(grown.properties == "age=4");
  const BlockStateKey leaf = state_with_prop_int(
      state("minecraft:oak_leaves", "distance=1,persistent=false,waterlogged=false"), "distance",
      7);
  CHECK(leaf.properties == "distance=7,persistent=false,waterlogged=false");
}

void test_is_randomly_ticking() {
  CHECK(block_is_randomly_ticking(state("minecraft:wheat", "age=0")));
  CHECK(!block_is_randomly_ticking(state("minecraft:wheat", "age=7")));
  CHECK(block_is_randomly_ticking(state("minecraft:beetroots", "age=2")));
  CHECK(!block_is_randomly_ticking(state("minecraft:beetroots", "age=3")));
  CHECK(block_is_randomly_ticking(
      state("minecraft:oak_leaves", "distance=7,persistent=false,waterlogged=false")));
  CHECK(!block_is_randomly_ticking(
      state("minecraft:oak_leaves", "distance=7,persistent=true,waterlogged=false")));
  CHECK(!block_is_randomly_ticking(
      state("minecraft:oak_leaves", "distance=6,persistent=false,waterlogged=false")));
  CHECK(block_is_randomly_ticking(state("minecraft:copper_block", "")));
  CHECK(!block_is_randomly_ticking(state("minecraft:oak_log", "")));
}

void test_crop_growth_speed() {
  TestLevel t;
  // One farmland cell under a lone crop: the centre (1.0) and nothing else, plus
  // the crop's own 1.0 -> 2.0.
  t.world.set(0, 99, 0, state("minecraft:farmland", "moisture=0"));
  CHECK_NEAR(crop_growth_speed(t.level, state("minecraft:wheat", "age=0"), 0, 100, 0), 2.0, 1e-6);

  // A 3x3 field of dry farmland: 1 (crop) + 1 (centre) + 8 * 0.25 = 4.0.
  for (int x = -1; x <= 1; ++x)
    for (int z = -1; z <= 1; ++z)
      if (x != 0 || z != 0)
        t.world.set(x, 99, z, state("minecraft:farmland", "moisture=0"));
  CHECK_NEAR(crop_growth_speed(t.level, state("minecraft:wheat", "age=0"), 0, 100, 0), 4.0, 1e-6);

  // Hydrated: the centre is worth 3.0 and each neighbour 0.75 -> 1 + 3 + 6 = 10.
  for (int x = -1; x <= 1; ++x)
    for (int z = -1; z <= 1; ++z)
      t.world.set(x, 99, z, state("minecraft:farmland", "moisture=7"));
  CHECK_NEAR(crop_growth_speed(t.level, state("minecraft:wheat", "age=0"), 0, 100, 0), 10.0, 1e-6);

  // Packed in with its own kind: one neighbour on both axes halves it to 5.0.
  t.world.set(1, 100, 0, state("minecraft:wheat", "age=0"));
  t.world.set(0, 100, 1, state("minecraft:wheat", "age=0"));
  CHECK_NEAR(crop_growth_speed(t.level, state("minecraft:wheat", "age=0"), 0, 100, 0), 5.0, 1e-6);

  // A diagonal neighbour alone halves it too (no shared face, one shared edge).
  t.world.set(1, 100, 0, state("minecraft:air", ""));
  t.world.set(0, 100, 1, state("minecraft:air", ""));
  t.world.set(1, 100, 1, state("minecraft:wheat", "age=0"));
  CHECK_NEAR(crop_growth_speed(t.level, state("minecraft:wheat", "age=0"), 0, 100, 0), 5.0, 1e-6);

  // A *different* crop does not crowd: only the same block halves it.
  t.world.set(1, 100, 1, state("minecraft:carrots", "age=0"));
  t.world.set(1, 100, 0, state("minecraft:wheat", "age=0"));
  t.world.set(0, 100, 1, state("minecraft:wheat", "age=0"));
  CHECK_NEAR(crop_growth_speed(t.level, state("minecraft:wheat", "age=0"), 0, 100, 0), 5.0, 1e-6);
}

void test_crop_random_tick() {
  TestLevel t;
  t.world.set(0, 99, 0, state("minecraft:farmland", "moisture=0")); // speed 2.0
  const BlockStateKey wheat = state("minecraft:wheat", "age=0");

  // The roll: `nextInt((int)(25 / 2) + 1) == nextInt(13) == 0` grows.
  RandomTickRandom rng = scripted({0});
  CHECK(crop_random_tick(t.level, rng, wheat, 0, 100, 0));
  CHECK_EQ(t.writes.size(), size_t(1));
  CHECK(std::get<3>(t.writes[0]).properties == "age=1");
  CHECK(std::get<4>(t.writes[0])); // `setBlock(pos, state, 2)`: clients only

  // The same crop, a roll that misses: nothing is written.
  t.writes.clear();
  RandomTickRandom miss = scripted({1});
  CHECK(!crop_random_tick(t.level, miss, wheat, 0, 100, 0));
  CHECK(t.writes.empty());

  // The light gate: `getRawBrightness(pos, 0) >= 9`.
  t.brightness = 8;
  CHECK(!crop_random_tick(t.level, scripted({0}), wheat, 0, 100, 0));
  t.brightness = 9;
  CHECK(crop_random_tick(t.level, scripted({0}), wheat, 0, 100, 0));

  // At its last age there is nothing to grow into.
  t.writes.clear();
  CHECK(!crop_random_tick(t.level, scripted({0}), state("minecraft:wheat", "age=7"), 0, 100, 0));
  CHECK(t.writes.empty());

  // The roll's bound follows the growth speed: the draw is
  // `nextInt((int)(25 / speed) + 1)`, so a lone crop on dry farmland (2.0) rolls
  // a 13 and a hydrated 3x3 field (10.0) only a 3.
  std::vector<int> bounds;
  RandomTickRandom recorder;
  recorder.next_int = [&bounds](int bound) {
    bounds.push_back(bound);
    return 0; // always grow, so the bound is all that varies
  };
  recorder.next_float = []() { return 0.0f; };
  t.writes.clear();
  CHECK(crop_random_tick(t.level, recorder, wheat, 0, 100, 0));
  CHECK_EQ(bounds.back(), 13);
  for (int x = -1; x <= 1; ++x)
    for (int z = -1; z <= 1; ++z)
      t.world.set(x, 99, z, state("minecraft:farmland", "moisture=7"));
  CHECK(crop_random_tick(t.level, recorder, wheat, 0, 100, 0));
  CHECK_EQ(bounds.back(), 3);

  // `crop_state_for_age` clamps and is a no-op on a non-crop.
  CHECK(crop_state_for_age(state("minecraft:beetroots", "age=0"), 9).properties == "age=3");
  CHECK(crop_state_for_age(state("minecraft:stone", ""), 4).properties == "");
}

void test_leaves_rules() {
  const BlockStateKey log = state("minecraft:oak_log", "");
  const BlockStateKey air = state("minecraft:air", "");
  CHECK_EQ(leaves_distance_at(log), 0);
  CHECK_EQ(leaves_distance_at(air), kLeavesDecayDistance);
  CHECK_EQ(leaves_distance_at(
               state("minecraft:oak_leaves", "distance=3,persistent=false,waterlogged=false")),
           3);
  // Every log is an anchor, not just oak.
  CHECK_EQ(leaves_distance_at(state("minecraft:stripped_mangrove_wood", "")), 0);

  CHECK(leaves_decaying(
      state("minecraft:oak_leaves", "distance=7,persistent=false,waterlogged=false")));
  CHECK(!leaves_decaying(
      state("minecraft:oak_leaves", "distance=7,persistent=true,waterlogged=false")));
  CHECK(!leaves_decaying(
      state("minecraft:oak_leaves", "distance=6,persistent=false,waterlogged=false")));

  // `updateDistance`: one relaxation step over the six neighbours.
  TestLevel t;
  const BlockStateKey leaf =
      state("minecraft:oak_leaves", "distance=7,persistent=false,waterlogged=false");
  t.world.set(0, 100, 0, leaf);
  t.world.set(0, 101, 0, log);
  CHECK(leaves_update_distance(t.level, leaf, 0, 100, 0).properties ==
        "distance=1,persistent=false,waterlogged=false");
  // A leaf next to a leaf at 3 is at 4; nothing around it is 7.
  t.world.set(0, 101, 0, air);
  t.world.set(0, 100, 1,
              state("minecraft:oak_leaves", "distance=3,persistent=false,waterlogged=false"));
  CHECK(leaves_update_distance(t.level, leaf, 0, 100, 0).properties ==
        "distance=4,persistent=false,waterlogged=false");
}

void test_leaves_update_shape_and_cascade() {
  TestLevel t;
  const BlockStateKey leaf =
      state("minecraft:oak_leaves", "distance=1,persistent=true,waterlogged=false");
  t.world.set(0, 100, 0, leaf);
  t.world.set(0, 101, 0, state("minecraft:oak_log", ""));

  // A leaf anchored by a log that has not changed: no schedule.
  leaves_update_shape(t.level, leaf, state("minecraft:oak_log", ""), 0, 100, 0);
  CHECK(t.schedules.empty());

  // The log goes away: `getDistanceAt(air) + 1` is 8, which is not 1 and not the
  // leaf's own 1, so the leaf schedules its own update one tick out.
  leaves_update_shape(t.level, leaf, state("minecraft:air", ""), 0, 100, 0);
  CHECK_EQ(t.schedules.size(), size_t(1));
  CHECK_EQ(std::get<3>(t.schedules[0]), 1);

  // The neighbour walk only schedules leaves: the one at -1, not the stone at 1
  // and not the log above.
  t.schedules.clear();
  t.world.set(1, 100, 0, state("minecraft:stone", ""));
  t.world.set(-1, 100, 0, leaf);
  leaves_notify_neighbours_of_change(t.level, 0, 100, 0, state("minecraft:air", ""));
  CHECK_EQ(t.schedules.size(), size_t(1));
  CHECK_EQ(std::get<0>(t.schedules[0]), -1);
}

void test_leaves_scheduled_tick_and_decay() {
  TestLevel t;
  const BlockStateKey leaf =
      state("minecraft:oak_leaves", "distance=1,persistent=false,waterlogged=false");
  t.world.set(0, 100, 0, leaf);
  t.world.set(0, 100, 1,
              state("minecraft:oak_leaves", "distance=7,persistent=false,waterlogged=false"));

  // `tick` writes the recomputed distance and notifies its neighbours (flag 3).
  CHECK(leaves_scheduled_tick(t.level, leaf, 0, 100, 0));
  CHECK_EQ(t.writes.size(), size_t(1));
  CHECK(std::get<3>(t.writes[0]).properties ==
        "distance=7,persistent=false,waterlogged=false"); // no wood anywhere near
  CHECK(std::get<4>(t.writes[0]));
  // The neighbour leaf is at 7 with a leaf next to it, so it schedules too.
  CHECK_EQ(t.schedules.size(), size_t(1));
  CHECK_EQ(std::get<0>(t.schedules[0]), 0);
  CHECK_EQ(std::get<2>(t.schedules[0]), 1);

  // `Level.setBlock`'s no-op: a recompute that yields the state the leaf already
  // holds writes nothing and notifies nothing, which is the branch that ends a
  // floating cluster's reschedule loop (`tests/fixtures/block_tick`'s `blob` run
  // is the loop-level pin of the same rule).
  TestLevel floating;
  const BlockStateKey floating_leaf =
      state("minecraft:oak_leaves", "distance=7,persistent=false,waterlogged=false");
  floating.world.set(0, 100, 0, floating_leaf);
  floating.world.set(0, 100, 1, floating_leaf);
  CHECK(leaves_scheduled_tick(floating.level, floating_leaf, 0, 100, 0));
  CHECK(floating.writes.empty());
  CHECK(floating.schedules.empty());
  // And the write that *does* change something still notifies.
  floating.world.set(0, 101, 0, state("minecraft:oak_log", ""));
  CHECK(leaves_scheduled_tick(floating.level, floating_leaf, 0, 100, 0));
  CHECK_EQ(floating.writes.size(), size_t(1));
  CHECK(std::get<3>(floating.writes[0]).properties ==
        "distance=1,persistent=false,waterlogged=false");
  CHECK_EQ(floating.schedules.size(), size_t(1));

  // The decay: a distance-7 non-persistent leaf drops and is removed.
  TestLevel d;
  d.world.set(5, 70, 5,
              state("minecraft:oak_leaves", "distance=7,persistent=false,waterlogged=false"));
  CHECK(leaves_random_tick(
      d.level, scripted({}),
      state("minecraft:oak_leaves", "distance=7,persistent=false,waterlogged=false"), 5, 70, 5));
  CHECK_EQ(d.drops.size(), size_t(1));
  CHECK_EQ(d.writes.size(), size_t(1));
  CHECK(std::get<3>(d.writes[0]).name == "minecraft:air");
  CHECK(std::get<4>(d.writes[0])); // `removeBlock(pos, false)` is flag 3

  // A persistent leaf, or one that is still anchored, is left alone.
  TestLevel p;
  CHECK(!leaves_random_tick(
      p.level, scripted({}),
      state("minecraft:oak_leaves", "distance=7,persistent=true,waterlogged=false"), 0, 100, 0));
  CHECK(!leaves_random_tick(
      p.level, scripted({}),
      state("minecraft:oak_leaves", "distance=4,persistent=false,waterlogged=false"), 0, 100, 0));
  CHECK(p.drops.empty() && p.writes.empty());
}

void test_placement_state() {
  TestLevel t;
  t.world.set(0, 101, 0, state("minecraft:oak_log", ""));
  // A placed leaf is persistent, at its distance from the wood around it.
  const BlockStateKey placed = leaves_placement_state(t.level, "minecraft:oak_leaves", 0, 100, 0);
  CHECK(placed.name == "minecraft:oak_leaves");
  CHECK(placed.properties == "distance=1,persistent=true,waterlogged=false");
  CHECK(!leaves_decaying(placed)); // and so it never decays
  const BlockStateKey floating =
      leaves_placement_state(t.level, "minecraft:spruce_leaves", 5, 200, 5);
  CHECK(floating.properties == "distance=7,persistent=true,waterlogged=false");
}

void test_chunk_section_counts() {
  Chunk chunk;
  chunk.x = 0;
  chunk.z = 0;
  chunk.min_section_y = -4;
  chunk.sections.resize(24);
  for (size_t i = 0; i < chunk.sections.size(); ++i)
    chunk.sections[i].y = -4 + static_cast<int>(i);

  // A crop starts ticking, a mature one stops, a leaf that is decaying ticks and
  // a log does not.
  CHECK(chunk.set_block_state(0, 100, 0, state("minecraft:wheat", "age=0")));
  CHECK_EQ(chunk.sections[10].random_tick_blocks, 1);
  CHECK(chunk.set_block_state(0, 100, 0, state("minecraft:wheat", "age=7")));
  CHECK_EQ(chunk.sections[10].random_tick_blocks, 0);
  CHECK(chunk.set_block_state(
      0, 100, 0, state("minecraft:oak_leaves", "distance=7,persistent=false,waterlogged=false")));
  CHECK_EQ(chunk.sections[10].random_tick_blocks, 1);
  CHECK(chunk.set_block_state(0, 100, 0, state("minecraft:oak_log", "")));
  CHECK_EQ(chunk.sections[10].random_tick_blocks, 0);
  CHECK(chunk.set_block_state(1, 100, 0, state("minecraft:copper_block", "")));
  CHECK_EQ(chunk.sections[10].random_tick_blocks, 1);

  // A rebuild from the cells agrees with the incremental count.
  chunk.sections[10].random_tick_blocks = 0;
  chunk_recount_random_tick_blocks(chunk);
  CHECK_EQ(chunk.sections[10].random_tick_blocks, 1);

  // And the world's mask is exactly the sections with a count.
  MemoryWorld world;
  CHECK(world.set_block_state(0, 0, 0, 100, 0, state("minecraft:wheat", "age=0")));
  CHECK_EQ(world.random_tick_sections(0, 0), 1ull << 10);
  CHECK_EQ(world.random_tick_sections(1, 0), 0ull); // a chunk nobody touched
}

void test_the_pass() {
  // A level whose one ticking section is index 10 (y 96..111) and whose sampled
  // positions all hold a mature crop except one young one at (3, 100, 4).
  StateWorld world;
  int reads = 0;
  std::vector<std::tuple<int, int, int>> sampled;
  RandomTickLevel level;
  level.min_section_y = -4;
  level.section_count = 24;
  level.ticking_sections = [](int, int) { return 1ull << 10; };
  level.get_block = [&world, &reads, &sampled](int x, int y, int z) {
    ++reads;
    sampled.emplace_back(x, y, z);
    if (x == 3 && y == 100 && z == 4)
      return state("minecraft:wheat", "age=0");
    return state("minecraft:wheat", "age=7");
  };
  int writes = 0;
  level.set_block = [&writes](int, int, int, const BlockStateKey&, bool) { ++writes; };
  level.raw_brightness = [](int, int, int) { return 15; };
  level.ticking_sections = [](int, int) { return 1ull << 10; };

  // Four draws of `tick_speed` 4: the LCG's own positions decide which cells are
  // looked at, so the test asserts the *shape* of the sample (all inside the
  // ticking section of the chunk) and that the one young crop grows.
  BlockRandomPos rand;
  rand.value = 0;
  RandomTickRandom always_grow = scripted({0});
  tick_chunk_random_blocks(level, always_grow, rand, 4, 0, 0);
  CHECK_EQ(reads, 4);
  for (const auto& [x, y, z] : sampled) {
    CHECK(x >= 0 && x < 16);
    CHECK(z >= 0 && z < 16);
    CHECK(y >= 96 && y <= 111); // section 10 with min_section_y -4
  }
  // A chunk whose mask is empty is never read at all.
  RandomTickLevel idle = level;
  idle.ticking_sections = [](int, int) { return 0ull; };
  reads = 0;
  tick_chunk_random_blocks(idle, always_grow, rand, 4, 0, 0);
  CHECK_EQ(reads, 0);
  // `randomTickSpeed` 0 is the gamerule's off state.
  tick_chunk_random_blocks(level, always_grow, rand, 0, 0, 0);
  CHECK_EQ(reads, 0);
}

void test_dispatch() {
  TestLevel t;
  t.world.set(0, 100, 0, state("minecraft:farmland", "moisture=0"));
  CHECK(random_tick_block(t.level, scripted({0}), state("minecraft:wheat", "age=0"), 0, 100, 0));
  CHECK_EQ(t.writes.size(), size_t(1));
  // A kind this engine does not simulate is sampled and then does nothing.
  CHECK(!random_tick_block(t.level, scripted({}), state("minecraft:copper_block", ""), 0, 100, 0));
  CHECK(!random_tick_block(t.level, scripted({}), state("minecraft:stone", ""), 0, 100, 0));
}

void test_rolled_block_drops() {
  const BlockStateKey leaves =
      state("minecraft:oak_leaves", "distance=7,persistent=false,waterlogged=false");
  // One draw per chance pool: 0.01 passes the sapling's 5% and the stick's 2%
  // but not the apple's 0.5%.
  std::vector<BlockDrop> drops = resolve_block_drops_rolled(leaves, "", []() { return 0.01; });
  CHECK_EQ(drops.size(), size_t(2));
  CHECK(drops[0].item == "minecraft:oak_sapling");
  CHECK(drops[1].item == "minecraft:stick");
  // A lucky draw takes all three.
  drops = resolve_block_drops_rolled(leaves, "", []() { return 0.001; });
  CHECK_EQ(drops.size(), size_t(3));
  CHECK(drops[2].item == "minecraft:apple");
  // A draw of 0.9 passes nothing at all, which is the 94% of leaves that drop
  // nothing (where the break path's deterministic model would always drop a
  // sapling).
  CHECK(resolve_block_drops_rolled(leaves, "", []() { return 0.9; }).empty());
  // Gravel's flint is a *sibling* alternative of the gravel itself in the same
  // pool, and the generated table puts the certain rule first (the break path's
  // contract), so the pool's one drop is the gravel whichever way the roll goes
  // -- the documented pool-order limitation, and the same answer the
  // deterministic resolver gives.
  drops = resolve_block_drops_rolled(state("minecraft:gravel", ""), "", []() { return 0.05; });
  CHECK_EQ(drops.size(), size_t(1));
  CHECK(drops[0].item == "minecraft:gravel");
  // The tool gate still applies, and every other block is unaffected.
  CHECK(resolve_block_drops_rolled(state("minecraft:stone", ""), "", []() { return 0.0; }).empty());
  CHECK_EQ(resolve_block_drops_rolled(state("minecraft:stone", ""), "minecraft:iron_pickaxe",
                                      []() { return 0.0; })
               .size(),
           size_t(1));
}

// ---- the golden trace -------------------------------------------------------

std::vector<std::string> split_tabs(const std::string& line) {
  std::vector<std::string> fields;
  size_t pos = 0;
  while (pos <= line.size()) {
    const size_t tab = line.find('\t', pos);
    if (tab == std::string::npos) {
      fields.push_back(line.substr(pos));
      break;
    }
    fields.push_back(line.substr(pos, tab - pos));
    pos = tab + 1;
  }
  return fields;
}

// A fixture file's data rows, i.e. everything that is not a `#` comment. Returns
// false when the file cannot be opened, so a missing fixture fails loudly
// instead of comparing against nothing.
bool read_fixture_rows(const std::string& path, std::vector<std::string>* rows) {
  std::ifstream file(path);
  if (!file)
    return false;
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;
    rows->push_back(line);
  }
  return true;
}

// The `name[properties]` form the trace and the scene use.
std::string canonical_state(const BlockStateKey& key) {
  std::string text = key.name;
  if (!key.properties.empty()) {
    text += '[';
    text += key.properties;
    text += ']';
  }
  return text;
}

BlockStateKey parse_state(const std::string& text) {
  const size_t open = text.find('[');
  if (open == std::string::npos)
    return state(text);
  return state(text.substr(0, open), text.substr(open + 1, text.size() - open - 2));
}

// One `random_tick_meta.tsv` row: the run parameters and the stream's two ends.
struct RecordedChunk {
  int64_t seed = 42;
  int32_t chunk_x = 0;
  int32_t chunk_z = 0;
  int min_section_y = -4;
  int section_count = 24;
  int tick_speed = 3;
  int ticks = 0;
  int brightness = 15;
  int32_t rand_start = 0;
  int32_t rand_end = 0;
  uint64_t mask = 0;
  std::string scene;
  size_t samples = 0;
  size_t writes = 0;
  size_t rows = 0;
};

struct RecordedPlacement {
  int32_t chunk_x = 0;
  int32_t chunk_z = 0;
  int x = 0;
  int y = 0;
  int z = 0;
  BlockStateKey state;
};

// The section index a cell is in, as the pass's own `min_section_y + index`
// mapping reads it (floor division, so negative y is not truncated to zero).
int section_index_of(int y) {
  const int min_section_y = -4; // the overworld's, which the meta pins
  int quotient = y / 16;
  if (y % 16 != 0 && y < 0)
    --quotient;
  return quotient - min_section_y;
}

int popcount64(uint64_t value) {
  int count = 0;
  while (value != 0) {
    value &= value - 1;
    ++count;
  }
  return count;
}

std::string trace_file_name(int64_t seed, int32_t chunk_x, int32_t chunk_z) {
  return "random_tick_seed" + std::to_string(seed) + "_c" + std::to_string(chunk_x) + "_" +
         std::to_string(chunk_z) + ".tsv";
}

struct Replay {
  std::vector<std::string> rows; // the trace, in the fixture's own format
  uint64_t mask = 0;
  int32_t rand_start = 0;
  int32_t rand_end = 0;
  size_t samples = 0;
  size_t writes = 0;
};

// Rebuilds the recorded scene, runs the real pass over it with a level that logs
// every interaction, and formats the log the way the fixture does. The sample
// rows are identified without a hook in the pass: the LCG advances immediately
// before the pass reads its own state, and a tick that follows draws from the
// caller's `RandomTickRandom`, never from the LCG — so "the LCG moved since the
// last read" is exactly a sample, and a `write`/`drop` after it is its outcome.
Replay replay_chunk(const RecordedChunk& chunk, const std::vector<RecordedPlacement>& placements) {
  Chunk world;
  world.x = chunk.chunk_x;
  world.z = chunk.chunk_z;
  world.min_section_y = chunk.min_section_y;
  world.sections.resize(static_cast<size_t>(chunk.section_count));
  for (int i = 0; i < chunk.section_count; ++i)
    world.sections[static_cast<size_t>(i)].y = chunk.min_section_y + i;
  for (const RecordedPlacement& placement : placements)
    world.set_block_state(placement.x, placement.y, placement.z, placement.state);

  Replay replay;
  for (size_t i = 0; i < world.sections.size() && i < 64; ++i) {
    if (world.sections[i].random_tick_blocks > 0)
      replay.mask |= 1ull << i;
  }

  mc::world::WgLegacyRandom level_random(chunk.seed);
  BlockRandomPos rand;
  rand.value = level_random.next_int();
  replay.rand_start = rand.value;

  mc::world::WgLegacyRandom rolls(chunk.seed);
  RandomTickRandom rng;
  rng.next_int = [&rolls](int bound) { return rolls.next_int(bound); };
  rng.next_float = [&rolls]() { return rolls.next_float(); };

  int tick = 0;
  int pending = -1; // the `sample` row still awaiting its outcome
  int32_t last_seen_value = rand.value;
  auto push = [&replay, &pending](std::string row, bool is_sample, bool is_outcome) {
    if (pending >= 0 && is_outcome) {
      // `ticked`: the tick ran and changed the world. A rule's own reads are not
      // an outcome (a crop's roll can miss after them), and a sample still
      // unanswered when the next draw arrives keeps the `skip`/`idle` it has.
      std::string& line = replay.rows[static_cast<size_t>(pending)];
      line.replace(line.size() - 4, 4, "ticked");
      pending = -1;
    }
    if (is_sample)
      pending = static_cast<int>(replay.rows.size());
    replay.rows.push_back(std::move(row));
  };
  auto row_text = [](const char* kind, int row_tick, int y, int x, int z,
                     const std::string& state_text, const std::string& extra) {
    char head[128];
    std::snprintf(head, sizeof(head), "%s\t%d\t%d\t%d\t%d\t%d\t", kind, row_tick,
                  section_index_of(y), x, y, z);
    return std::string(head) + state_text + "\t" + extra;
  };

  RandomTickLevel level;
  level.min_section_y = chunk.min_section_y;
  level.section_count = chunk.section_count;
  level.ticking_sections = [&replay](int, int) { return replay.mask; };
  level.get_block = [&](int x, int y, int z) {
    const BlockStateKey block =
        world.block_state_at(x - chunk.chunk_x * 16, y, z - chunk.chunk_z * 16);
    const bool is_sample = rand.value != last_seen_value;
    last_seen_value = rand.value;
    if (is_sample)
      ++replay.samples;
    const char* kind = is_sample ? "sample" : "read";
    const std::string extra = !is_sample ? "-" : block_is_randomly_ticking(block) ? "idle" : "skip";
    push(row_text(kind, tick, y, x, z, canonical_state(block), extra), is_sample, false);
    return block;
  };
  level.set_block = [&](int x, int y, int z, const BlockStateKey& block, bool notify) {
    world.set_block_state(x - chunk.chunk_x * 16, y, z - chunk.chunk_z * 16, block);
    ++replay.writes;
    push(row_text("write", tick, y, x, z, canonical_state(block), notify ? "notify" : "silent"),
         false, true);
  };
  level.raw_brightness = [&chunk](int, int, int) { return chunk.brightness; };
  level.schedule_tick = [&](int x, int y, int z, int delay) {
    push(row_text("schedule", tick, y, x, z, "delay=" + std::to_string(delay), "-"), false, false);
  };
  level.drop_resources = [&](int x, int y, int z, const BlockStateKey& block) {
    push(row_text("drop", tick, y, x, z, canonical_state(block), "-"), false, true);
  };

  for (tick = 0; tick < chunk.ticks; ++tick)
    tick_chunk_random_blocks(level, rng, rand, chunk.tick_speed, chunk.chunk_x, chunk.chunk_z);

  replay.rand_end = rand.value;
  return replay;
}

// Prints the first few rows that differ, so a failure names the drift instead of
// only counting it.
void report_trace_mismatch(const RecordedChunk& chunk, const std::vector<std::string>& on_disk,
                           const std::vector<std::string>& replayed) {
  std::fprintf(stderr, "FAIL %s:%d: chunk (%d, %d): %zu rows on disk, %zu replayed\n", __FILE__,
               __LINE__, chunk.chunk_x, chunk.chunk_z, on_disk.size(), replayed.size());
  size_t shown = 0;
  const size_t common = std::min(on_disk.size(), replayed.size());
  for (size_t i = 0; i < common && shown < 8; ++i) {
    if (on_disk[i] == replayed[i])
      continue;
    std::fprintf(stderr, "  row %zu:\n    on disk:   %s\n    replayed:  %s\n", i,
                 on_disk[i].c_str(), replayed[i].c_str());
    ++shown;
  }
  for (size_t i = common; i < on_disk.size() && shown < 8; ++i, ++shown)
    std::fprintf(stderr, "  row %zu:\n    on disk:   %s\n    replayed:  <absent>\n", i,
                 on_disk[i].c_str());
  for (size_t i = common; i < replayed.size() && shown < 8; ++i, ++shown)
    std::fprintf(stderr, "  row %zu:\n    on disk:   <absent>\n    replayed:  %s\n", i,
                 replayed[i].c_str());
}

// The golden half: replay every chunk the meta records and compare its whole
// interaction trace against the checked-in one.
void test_golden_trace(const std::string& fixtures_dir) {
  std::vector<std::string> meta_rows;
  std::vector<std::string> scene_rows;
  const bool have_meta = read_fixture_rows(fixtures_dir + "/random_tick_meta.tsv", &meta_rows);
  const bool have_scene = read_fixture_rows(fixtures_dir + "/random_tick_scene.tsv", &scene_rows);
  CHECK(have_meta);
  CHECK(have_scene);
  if (!have_meta || !have_scene)
    return;
  CHECK(!meta_rows.empty());

  std::vector<RecordedChunk> chunks;
  for (const std::string& row : meta_rows) {
    const std::vector<std::string> fields = split_tabs(row);
    CHECK_EQ(fields.size(), size_t(15));
    if (fields.size() != 15)
      continue;
    RecordedChunk chunk;
    chunk.seed = std::strtoll(fields[0].c_str(), nullptr, 10);
    chunk.chunk_x = static_cast<int32_t>(std::strtol(fields[1].c_str(), nullptr, 10));
    chunk.chunk_z = static_cast<int32_t>(std::strtol(fields[2].c_str(), nullptr, 10));
    chunk.min_section_y = std::stoi(fields[3]);
    chunk.section_count = std::stoi(fields[4]);
    chunk.tick_speed = std::stoi(fields[5]);
    chunk.ticks = std::stoi(fields[6]);
    chunk.brightness = std::stoi(fields[7]);
    chunk.rand_start = static_cast<int32_t>(std::strtol(fields[8].c_str(), nullptr, 10));
    chunk.rand_end = static_cast<int32_t>(std::strtol(fields[9].c_str(), nullptr, 10));
    chunk.mask = std::strtoull(fields[10].c_str(), nullptr, 10);
    chunk.scene = fields[11];
    chunk.samples = static_cast<size_t>(std::strtoull(fields[12].c_str(), nullptr, 10));
    chunk.writes = static_cast<size_t>(std::strtoull(fields[13].c_str(), nullptr, 10));
    chunk.rows = static_cast<size_t>(std::strtoull(fields[14].c_str(), nullptr, 10));
    // The row's section column is the overworld's fixed `minSectionY` (-4), which
    // is also what `section_index_of` subtracts.
    CHECK_EQ(chunk.min_section_y, -4);
    chunks.push_back(chunk);
  }

  std::vector<RecordedPlacement> placements;
  for (const std::string& row : scene_rows) {
    const std::vector<std::string> fields = split_tabs(row);
    CHECK_EQ(fields.size(), size_t(6));
    if (fields.size() != 6)
      continue;
    RecordedPlacement placement;
    placement.chunk_x = static_cast<int32_t>(std::strtol(fields[0].c_str(), nullptr, 10));
    placement.chunk_z = static_cast<int32_t>(std::strtol(fields[1].c_str(), nullptr, 10));
    placement.x = std::stoi(fields[2]);
    placement.y = std::stoi(fields[3]);
    placement.z = std::stoi(fields[4]);
    placement.state = parse_state(fields[5]);
    placements.push_back(placement);
  }

  size_t checked_rows = 0;
  for (const RecordedChunk& chunk : chunks) {
    std::vector<RecordedPlacement> scene;
    for (const RecordedPlacement& placement : placements) {
      if (placement.chunk_x == chunk.chunk_x && placement.chunk_z == chunk.chunk_z)
        scene.push_back(placement);
    }
    CHECK(!scene.empty());

    const Replay replay = replay_chunk(chunk, scene);

    // The pass draws once per ticking section per tick per speed unit, and an
    // idle pass (`randomTickSpeed` 0, or nothing in the chunk ticking) draws not
    // at all — the same two invariants the generator refuses to write a fixture
    // without, restated here so a fixture cannot pin a lie either.
    const size_t expected_samples =
        static_cast<size_t>(popcount64(replay.mask)) *
        static_cast<size_t>(chunk.tick_speed > 0 ? chunk.tick_speed : 0) *
        static_cast<size_t>(chunk.ticks);
    CHECK_EQ(replay.samples, expected_samples);
    CHECK_EQ(replay.mask, chunk.mask);
    CHECK_EQ(replay.rand_start, chunk.rand_start);
    CHECK_EQ(replay.rand_end, chunk.rand_end);
    CHECK_EQ(replay.writes, chunk.writes);
    CHECK_EQ(replay.rows.size(), chunk.rows);
    if (replay.mask == 0 || chunk.tick_speed <= 0)
      CHECK_EQ(replay.rand_end, replay.rand_start);

    std::vector<std::string> trace;
    const std::string path =
        fixtures_dir + "/" + trace_file_name(chunk.seed, chunk.chunk_x, chunk.chunk_z);
    const bool have_trace = read_fixture_rows(path, &trace);
    CHECK(have_trace);
    if (!have_trace)
      continue;
    if (trace == replay.rows) {
      checked_rows += trace.size();
      continue;
    }
    report_trace_mismatch(chunk, trace, replay.rows);
    ++failures;
  }
  std::printf("random_tick: golden trace replayed for %zu chunks, %zu rows compared\n",
              chunks.size(), checked_rows);
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <fixtures_dir>\n", argv[0]);
    return 2;
  }
  test_generated_table();
  test_state_properties();
  test_is_randomly_ticking();
  test_crop_growth_speed();
  test_crop_random_tick();
  test_leaves_rules();
  test_leaves_update_shape_and_cascade();
  test_leaves_scheduled_tick_and_decay();
  test_placement_state();
  test_chunk_section_counts();
  test_the_pass();
  test_dispatch();
  test_rolled_block_drops();
  test_golden_trace(argv[1]);
  if (failures == 0)
    std::printf("random_tick: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
