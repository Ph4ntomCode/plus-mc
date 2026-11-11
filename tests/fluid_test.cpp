// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cstdio>
#include <map>
#include <tuple>
#include <vector>

#include "world/container.h"
#include "world/fluid.h"

using namespace mc::world;
static int failures = 0;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                            \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)
#define CHECK_EQ(a, b)                                                                             \
  do {                                                                                             \
    if ((a) != (b)) {                                                                              \
      std::fprintf(stderr, "FAIL %s:%d\n", __FILE__, __LINE__);                                    \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

int main() {
  CHECK(fluid_state_of(BlockStateKey{"minecraft:water", "level=0"}).source());
  CHECK_EQ(fluid_state_of(BlockStateKey{"minecraft:water", "level=1"}).amount, 7);
  CHECK(fluid_state_of(BlockStateKey{"minecraft:lava", "level=8"}).falling);
  CHECK_EQ(fluid_block_state(FluidKind::Water, 9).properties, std::string("level=0"));
  CHECK_EQ(fluid_block_state(FluidKind::Water, 9, true).properties, std::string("level=8"));
  CHECK_EQ(
      fluid_shape(FluidState{FluidKind::Water, 9, false}, FluidState{FluidKind::Water, 7, false})
          .max_y,
      1.0f);
  CHECK_EQ(fluid_shape(FluidState{FluidKind::Water, 7, false}, {}).max_y, 7.0f / 9.0f);
  CHECK_EQ(fluid_drop_off(FluidKind::Water), 1);
  CHECK_EQ(fluid_drop_off(FluidKind::Lava), 2);

  std::map<std::tuple<int, int, int>, BlockStateKey> blocks;
  auto get = [&](int x, int y, int z) {
    auto it = blocks.find({x, y, z});
    return it == blocks.end() ? BlockStateKey{"minecraft:air", ""} : it->second;
  };
  FluidLevel level;
  level.get_block = get;
  level.can_hold_fluid = [](const BlockStateKey& s) {
    return s.name == "minecraft:air" || block_property(s.properties, "waterlogged") == "false";
  };
  std::vector<std::tuple<int, int, int>> scheduled;
  level.schedule_tick = [&](int x, int y, int z, int) { scheduled.emplace_back(x, y, z); };
  level.set_block = [&](int x, int y, int z, const BlockStateKey& s) { blocks[{x, y, z}] = s; };
  blocks[{0, 3, 0}] = {"minecraft:water", "level=0"};
  fluid_tick(level, FluidKind::Water, 0, 3, 0);
  CHECK(fluid_state_of(get(0, 2, 0)).falling);
  CHECK(!scheduled.empty());

  blocks.clear();
  scheduled.clear();
  blocks[{0, 0, 0}] = {"minecraft:water", "level=0"};
  blocks[{0, -1, 0}] = {"minecraft:stone", ""};
  blocks[{1, 0, 0}] = {"minecraft:air", ""};
  fluid_tick(level, FluidKind::Water, 0, 0, 0);
  CHECK(fluid_state_of(get(1, 0, 0)).kind == FluidKind::Water);

  blocks.clear();
  blocks[{0, 0, 0}] = {"minecraft:oak_slab", "waterlogged=false"};
  blocks[{0, 1, 0}] = {"minecraft:water", "level=0"};
  fluid_tick(level, FluidKind::Water, 0, 1, 0);
  CHECK(block_property(get(0, 0, 0).properties, "waterlogged") == "true");

  // One tick's read budget: it asks about its own cell plus the six around it,
  // and asks each of those exactly once. A tick used to read each side three
  // times over — the supply scan, the fan-out after a recomputed cell rewrites
  // itself, and the spread loop, whose own write path read its target again —
  // and each of those reads is a session `get_block` (a chunk lookup) plus a
  // `fluid_state_of` property parse. The budget is checked per cell rather than
  // in total so a tick that reads one cell twice and skips another still fails.
  blocks.clear();
  scheduled.clear();
  int reads = 0;
  std::map<std::tuple<int, int, int>, int> reads_per_cell;
  level.get_block = [&](int x, int y, int z) {
    ++reads;
    ++reads_per_cell[{x, y, z}];
    auto it = blocks.find({x, y, z});
    return it == blocks.end() ? BlockStateKey{"minecraft:air", ""} : it->second;
  };
  // A decaying droplet over a hole: no supply and no same-fluid neighbour, so
  // the tick pours its full falling state into the cell below and then retracts
  // to air — and the retract's neighbour fan-out has to see the cell this very
  // tick wrote. That below cell is therefore scheduled twice, once by the write
  // (`spread_to`) and once by the fan-out; the queue dedupes the pair (one entry
  // per cell and kind, earliest due kept), so the second is a lookup, but the
  // call itself is what proves the fan-out read the fresh state. Hand it a stale
  // answer and the second one disappears.
  blocks[{0, 0, 0}] = {"minecraft:water", "level=7"}; // amount 1, not a source
  fluid_tick(level, FluidKind::Water, 0, 0, 0);
  CHECK(fluid_state_of(get(0, 0, 0)).empty());
  CHECK(fluid_state_of(get(0, -1, 0)).falling);
  CHECK_EQ(std::count(scheduled.begin(), scheduled.end(), std::tuple<int, int, int>{0, -1, 0}), 2);
  CHECK(reads <= 7);
  for (const auto& [cell, count] : reads_per_cell)
    CHECK_EQ(count, 1);
  level.get_block = get;

  FluidTickQueue queue;
  queue.schedule(0, 0, 0, FluidKind::Water, 10);
  queue.schedule(0, 0, 0, FluidKind::Water, 2);
  CHECK_EQ(queue.size(), static_cast<size_t>(1));
  for (int i = 0; i < 1; ++i)
    CHECK(queue.advance().empty());
  CHECK_EQ(queue.advance().size(), static_cast<size_t>(1));
  return failures == 0 ? 0 : 1;
}
