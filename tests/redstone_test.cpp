// SPDX-License-Identifier: GPL-3.0-or-later
//
// Keep the `assert`s live in every configuration: a Release build defines
// NDEBUG, and without this the checks below compile to a no-op in the gate that
// runs them.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "world/container.h"
#include "world/redstone.h"
#include <array>
#include <cassert>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace {
mc::world::BlockStateKey state(const char* name, const char* properties = "") {
  return {name, properties};
}
} // namespace

using namespace mc::world;
int main() {
  assert(recompute_redstone_component(0, 0, 0, {}, {}, {}, 10).empty());
  assert(clamp_redstone_power(-2) == 0);
  assert(clamp_redstone_power(19) == 15);
  std::array<int, 6> wires{0, 15, 0, 0, 0, 0};
  assert(redstone_wire_power(0, wires) == 14);
  assert(redstone_wire_power(15, wires) == 15);

  RedstoneRules rules;
  rules.side_signal = [](int, int, int, int face) { return face == 2 ? 7 : 0; };
  RedstoneNeighbours n;
  n.direct[0] = 15;
  n.wire[1] = 4;
  assert(redstone_received_signal(rules, 0, 0, 0, n) == 15);
  n.direct[0] = 0;
  assert(redstone_received_signal(rules, 0, 0, 0, n) == 7);

  using P = std::tuple<int, int, int>;
  std::map<P, int> powers{{P{0, 0, 0}, 0}, {P{1, 0, 0}, 0}, {P{2, 0, 0}, 0}};
  auto is_wire = [&](int x, int y, int z) { return powers.count({x, y, z}) != 0; };
  auto source = [&](int x, int, int) { return x == 0 ? 15 : 0; };
  auto current = [&](int x, int y, int z) { return powers[{x, y, z}]; };
  const auto changes = recompute_redstone_component(0, 0, 0, is_wire, source, current, 3);
  assert(changes.size() == 3);
  assert(changes[0].power == 15);
  assert(changes[1].power == 14);
  assert(changes[2].power == 13);

  // Source removal must relax all the way back through a chain; a one-pass
  // traversal leaves stale downstream power when it reads old neighbour values.
  powers[{0, 0, 0}] = 15;
  powers[{1, 0, 0}] = 14;
  powers[{2, 0, 0}] = 13;
  const auto removed =
      recompute_redstone_component(0, 0, 0, is_wire, [](int, int, int) { return 0; }, current, 3);
  assert(removed.size() == 3);
  assert(removed[0].power == 0);
  assert(removed[1].power == 0);
  assert(removed[2].power == 0);

  // The live state adapter must handle both source placement and source removal,
  // and must notify consumers after the wire graph settles.
  std::map<P, BlockStateKey> world{
      {P{0, 0, 0}, state("minecraft:redstone_block")},
      {P{1, 0, 0}, state("minecraft:redstone_wire", "power=0")},
      {P{2, 0, 0}, state("minecraft:redstone_wire", "power=0")},
      {P{3, 0, 0}, state("minecraft:redstone_lamp", "lit=false")},
  };
  auto get_state = [&](int x, int y, int z) {
    const auto it = world.find(P{x, y, z});
    return it == world.end() ? state("minecraft:air") : it->second;
  };
  auto write_state = [&](int x, int y, int z, const BlockStateKey& next) {
    world[P{x, y, z}] = next;
  };
  update_redstone_area(get_state, write_state, 0, 0, 0);
  assert(block_property(world[P{1, 0, 0}].properties, "power") == "15");
  assert(block_property(world[P{2, 0, 0}].properties, "power") == "14");
  assert(block_property(world[P{3, 0, 0}].properties, "lit") == "true");
  world.erase(P{0, 0, 0});
  update_redstone_area(get_state, write_state, 0, 0, 0);
  assert(block_property(world[P{1, 0, 0}].properties, "power") == "0");
  assert(block_property(world[P{2, 0, 0}].properties, "power") == "0");
  assert(block_property(world[P{3, 0, 0}].properties, "lit") == "false");

  // Attenuation through the live adapter, not just the pure engine: a wire run
  // beside a redstone block reads 15, 14, 13 ... — vanilla's
  // `DefaultRedstoneWireEvaluator.calculateTargetStrength` takes the strong
  // source's 15 undiminished and each further wire through
  // `getIncomingWireSignal`'s max(neighbour) - 1. Feeding a wire's own power
  // back in as its neighbour's *source* made every cell of the run read 15.
  std::map<P, BlockStateKey> run{{P{0, 0, 0}, state("minecraft:redstone_block")}};
  for (int i = 1; i <= 10; ++i)
    run[P{i, 0, 0}] = state("minecraft:redstone_wire", "power=0");
  auto run_get = [&](int x, int y, int z) {
    const auto it = run.find(P{x, y, z});
    return it == run.end() ? state("minecraft:air") : it->second;
  };
  auto run_write = [&](int x, int y, int z, const BlockStateKey& next) { run[P{x, y, z}] = next; };
  const auto changed = update_redstone_area(run_get, run_write, 0, 0, 0);
  assert(changed.size() == 10);
  for (int i = 1; i <= 10; ++i)
    assert(block_property(run[P{i, 0, 0}].properties, "power") == std::to_string(16 - i));
  // A source removed from one end relaxes the whole settled run back to 0.
  run.erase(P{0, 0, 0});
  update_redstone_area(run_get, run_write, 0, 0, 0);
  for (int i = 1; i <= 10; ++i)
    assert(block_property(run[P{i, 0, 0}].properties, "power") == "0");
  return 0;
}
