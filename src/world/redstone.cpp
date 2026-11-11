// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/redstone.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "world/container.h"

namespace mc::world {

int clamp_redstone_power(int value) {
  return std::clamp(value, 0, 15);
}

int redstone_wire_power(int source, const std::array<int, 6>& adjacent_wire_power) {
  int power = clamp_redstone_power(source);
  for (int value : adjacent_wire_power)
    power = std::max(power, clamp_redstone_power(value) - 1);
  return std::clamp(power, 0, 15);
}

int redstone_received_signal(const RedstoneRules& rules, int x, int y, int z,
                             const RedstoneNeighbours& neighbours) {
  int result = 0;
  for (int face = 0; face < 6; ++face) {
    result = std::max(result, clamp_redstone_power(neighbours.direct[face]));
    int side = clamp_redstone_power(neighbours.wire[face]);
    if (rules.side_signal)
      side = std::max(side, clamp_redstone_power(rules.side_signal(x, y, z, face)));
    result = std::max(result, side);
  }
  return result;
}

namespace {
using Pos = std::tuple<int, int, int>;
constexpr int kDx[6] = {0, 0, 0, 0, -1, 1};
constexpr int kDy[6] = {-1, 1, 0, 0, 0, 0};
constexpr int kDz[6] = {0, 0, -1, 1, 0, 0};

// A world cell packed into one key: 26 bits of x, 12 of y and 26 of z (the
// world border is at +-30M, so 26 bits covers x and z with room to spare).
uint64_t cell_key(int x, int y, int z) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(x) & 0x3FFFFFFu) << 38) |
         (static_cast<uint64_t>(static_cast<uint32_t>(y) & 0xFFFu) << 26) |
         (static_cast<uint64_t>(static_cast<uint32_t>(z) & 0x3FFFFFFu));
}
} // namespace

std::vector<RedstoneUpdate> recompute_redstone_component(
    int origin_x, int origin_y, int origin_z, const std::function<bool(int, int, int)>& is_wire,
    const std::function<int(int, int, int)>& source_signal,
    const std::function<int(int, int, int)>& current_power, int max_nodes) {
  std::vector<RedstoneUpdate> updates;
  if (!is_wire || !source_signal || !current_power || max_nodes <= 0 ||
      !is_wire(origin_x, origin_y, origin_z))
    return updates;

  // Values are relaxed until no wire can improve or lose power. A one-shot BFS
  // is insufficient when a source is removed: downstream wires must first fall,
  // then feed that lower value back toward the source.
  std::deque<Pos> queue;
  std::set<Pos> queued;
  std::set<Pos> discovered;
  std::map<Pos, int> values;
  const Pos origin{origin_x, origin_y, origin_z};
  queue.push_back(origin);
  queued.insert(origin);
  discovered.insert(origin);
  values[origin] = clamp_redstone_power(current_power(origin_x, origin_y, origin_z));

  while (!queue.empty()) {
    const Pos pos = queue.front();
    queue.pop_front();
    queued.erase(pos);
    const auto [x, y, z] = pos;
    std::array<int, 6> adjacent{};
    std::array<Pos, 6> neighbours{};
    int neighbour_count = 0;
    for (int face = 0; face < 6; ++face) {
      const int nx = x + kDx[face], ny = y + kDy[face], nz = z + kDz[face];
      if (!is_wire(nx, ny, nz))
        continue;
      const Pos neighbour{nx, ny, nz};
      if (!discovered.count(neighbour)) {
        if (static_cast<int>(discovered.size()) >= max_nodes)
          continue;
        discovered.insert(neighbour);
        values[neighbour] = clamp_redstone_power(current_power(nx, ny, nz));
        queue.push_back(neighbour);
        queued.insert(neighbour);
      }
      neighbours[neighbour_count++] = neighbour;
      adjacent[face] = values[neighbour];
    }
    const int next = redstone_wire_power(source_signal(x, y, z), adjacent);
    if (next == values[pos])
      continue;
    values[pos] = next;
    for (int i = 0; i < neighbour_count; ++i) {
      const Pos neighbour = neighbours[i];
      if (!queued.count(neighbour)) {
        queue.push_back(neighbour);
        queued.insert(neighbour);
      }
    }
  }

  for (const auto& [pos, power] : values) {
    const auto [x, y, z] = pos;
    const int old = clamp_redstone_power(current_power(x, y, z));
    if (power != old)
      updates.push_back({x, y, z, power});
  }
  return updates;
}

namespace {

bool is_wire_state(const BlockStateKey& state) {
  return state.name == "minecraft:redstone_wire";
}

bool is_power_source_state(const BlockStateKey& state) {
  if (state.name == "minecraft:redstone_block")
    return true;
  if (state.name == "minecraft:redstone_torch" || state.name == "minecraft:redstone_wall_torch")
    return block_property(state.properties, "lit") == "true";
  if (state.name == "minecraft:lever" || state.name.find("_button") != std::string_view::npos ||
      state.name.find("_pressure_plate") != std::string_view::npos)
    return block_property(state.properties, "powered") == "true";
  return false;
}

// The two redstone gates. Both are *directional*: a powered repeater hands 15
// only to the neighbour it faces (a repeater beside a wire on any other side is
// not a source for it), and a comparator hands out its computed output the same
// way. Before this, both fell through `is_power_source_state`'s omnidirectional
// 15 and lit every wire around them.
bool is_repeater_state(const BlockStateKey& state) {
  return state.name.find("repeater") != std::string_view::npos;
}

bool is_comparator_state(const BlockStateKey& state) {
  return state.name.find("comparator") != std::string_view::npos;
}

// `Direction`'s ordinal from the block state's `facing` property. A state with
// no facing (the pack always writes one on these two) reads north.
int gate_facing(const BlockStateKey& state) {
  const std::string value = block_property(state.properties, "facing");
  if (value == "down")
    return 0;
  if (value == "up")
    return 1;
  if (value == "north")
    return 2;
  if (value == "south")
    return 3;
  if (value == "west")
    return 4;
  if (value == "east")
    return 5;
  return 2;
}

int opposite_face(int face) {
  return face ^ 1;
}

int property_int(const BlockStateKey& state, std::string_view key) {
  const std::string value = block_property(state.properties, key);
  if (value.empty())
    return 0;
  try {
    return std::stoi(value);
  } catch (...) {
    return 0;
  }
}

} // namespace

std::vector<RedstoneUpdate>
update_redstone_area(const std::function<BlockStateKey(int, int, int)>& get,
                     const std::function<void(int, int, int, const BlockStateKey&)>& write, int x,
                     int y, int z, int max_nodes) {
  std::vector<RedstoneUpdate> changed;
  if (!get || !write || max_nodes <= 0)
    return changed;

  // THE wire engine does the wire graph: `recompute_redstone_component` relaxes
  // rising and falling edges over the connected component. Nothing here
  // re-derives attenuation, and the two signals stay on their own sides of the
  // callback boundary — a strong source feeds a wire, a neighbouring wire's
  // POWER enters only through `redstone_wire_power`'s decrement — which is the
  // one place the split can live without the halves drifting apart again.
  // One settlement asks about each cell several times over — whether it is a
  // wire, what power it holds, what signal it hands over — and `get` is the
  // world: at the server's call site a lock, a chunk lookup and a by-value
  // `BlockStateKey`. A 48-cell run settled to 492 reads that way. All three
  // answers are per *cell* facts, so derive them once per position and keep
  // them for the settlement; `write` is the only mutation this function makes,
  // so it is what drops an entry. Nothing here changes what a read *means* —
  // the same predicates answer from the cached state instead of a fresh one.
  struct Cell {
    bool wire = false;
    // A redstone gate: `RedstoneDiode` / `ComparatorBlock`. Both are directional
    // sources, which is what the `emitted_toward` walk below reads.
    bool repeater = false;
    bool comparator = false;
    bool powered = false;
    int facing = 2; // Direction's ordinal (north)
    int mode = 0;   // comparator: 0 compare, 1 subtract
    // `current_power`: the cell's own `power` property, clamped.
    int power = 0;
    // One cell's contribution as a *source*: a redstone block, a lit torch, an
    // engaged lever/button/plate/repeater hands over 15, and **a wire hands
    // over nothing**. A wire is no source for another wire; its POWER reaches a
    // wire neighbour only through `redstone_wire_power`'s decrement, and
    // reaches a *consumer* through `emitted()` below. Folding the wire term
    // into this one is what fed every wire's power back in as its neighbour's
    // source, so a whole run read 15.
    int source = 0;
  };
  std::unordered_map<uint64_t, Cell> cells;
  const auto cell_at = [&get, &cells](int wx, int wy, int wz) -> const Cell& {
    const uint64_t k = cell_key(wx, wy, wz);
    const auto it = cells.find(k);
    if (it != cells.end())
      return it->second;
    const BlockStateKey state = get(wx, wy, wz);
    Cell cell;
    cell.wire = is_wire_state(state);
    cell.repeater = is_repeater_state(state);
    cell.comparator = is_comparator_state(state);
    cell.powered = block_property(state.properties, "powered") == "true";
    cell.facing = gate_facing(state);
    cell.mode = block_property(state.properties, "mode") == "subtract" ? 1 : 0;
    cell.power = std::clamp(property_int(state, "power"), 0, 15);
    cell.source = (cell.wire || cell.repeater || cell.comparator)
                      ? 0
                      : (is_power_source_state(state) ? 15 : 0);
    return cells.emplace(k, cell).first->second;
  };
  // What a cell hands to the neighbour in direction `dir`. A wire hands over
  // its own POWER at full strength (the decrement belongs to wire-to-wire
  // feeding alone); an ordinary source hands over 15; a repeater hands 15 only
  // toward the face it points at; a comparator hands its computed output the
  // same way. `depth` bounds the comparator-to-comparator lookahead (vanilla
  // resolves it through scheduled ticks; a settlement needs an answer *now*, so
  // a chain longer than this reads as unpowered rather than recursing away).
  std::function<int(int, int, int, int, int)> emitted_toward;
  std::function<int(int, int, int, int)> comparator_output;
  comparator_output = [&](int px, int py, int pz, int depth) -> int {
    if (depth <= 0)
      return 0;
    const Cell& cell = cell_at(px, py, pz);
    const int facing = cell.facing;
    const int input =
        emitted_toward(px - kDx[facing], py - kDy[facing], pz - kDz[facing], facing, depth - 1);
    int side = 0;
    for (int f = 2; f < 6; ++f) {
      if (f == facing || f == opposite_face(facing))
        continue;
      side =
          std::max(side, emitted_toward(px + kDx[f], py, pz + kDz[f], opposite_face(f), depth - 1));
    }
    if (cell.mode == 1)
      return std::clamp(input - side, 0, 15);
    return input >= side ? input : 0;
  };
  emitted_toward = [&](int px, int py, int pz, int dir, int depth) -> int {
    const Cell& cell = cell_at(px, py, pz);
    if (cell.wire)
      return cell.power;
    if (cell.repeater)
      return cell.powered && dir == cell.facing ? 15 : 0;
    if (cell.comparator)
      return dir == cell.facing ? comparator_output(px, py, pz, depth) : 0;
    return cell.source;
  };
  static constexpr int kGateDepth = 8;
  const auto settle_write = [&write, &cells](int wx, int wy, int wz, const BlockStateKey& state) {
    cells.erase(cell_key(wx, wy, wz));
    write(wx, wy, wz, state);
  };
  const auto is_wire = [&cell_at](int wx, int wy, int wz) { return cell_at(wx, wy, wz).wire; };
  // `Level.getBestNeighborSignal(pos)`, which is what vanilla's `getBlockSignal`
  // asks for with `RedStoneWireBlock.shouldSignal` off: the strongest signal the
  // cell's six *neighbours* hand over. Only this half enters a wire's own
  // strength; the wire-to-wire term is `redstone_wire_power`'s adjacent array.
  const auto source_signal = [&cell_at, &emitted_toward](int wx, int wy, int wz) {
    int best = 0;
    for (int i = 0; i < 6; ++i) {
      const int nx = wx + kDx[i], ny = wy + kDy[i], nz = wz + kDz[i];
      // A wire is not a source for another wire: its power reaches a wire
      // neighbour only through `redstone_wire_power`'s decrement, which is the
      // `adjacent` array, never through this half. Folding it in here kept a
      // run at the strength it was about to lose when its source was removed
      // (the removed block's neighbours read each other's stale power back in).
      if (cell_at(nx, ny, nz).wire)
        continue;
      best = std::max(best, emitted_toward(nx, ny, nz, opposite_face(i), kGateDepth));
    }
    return best;
  };
  const auto current_power = [&cell_at](int wx, int wy, int wz) {
    return cell_at(wx, wy, wz).power;
  };

  // One seed per wire this change touches: the cell itself when it is a wire (a
  // wire placed, broken or re-typed) and its six neighbours (a source placed or
  // removed beside a run). A component reached through two seeds relaxes
  // idempotently — the second pass reads the settled values.
  std::vector<std::tuple<int, int, int>> seeds;
  if (is_wire(x, y, z))
    seeds.emplace_back(x, y, z);
  for (int i = 0; i < 6; ++i) {
    const int nx = x + kDx[i], ny = y + kDy[i], nz = z + kDz[i];
    if (is_wire(nx, ny, nz))
      seeds.emplace_back(nx, ny, nz);
  }

  for (const auto& [sx, sy, sz] : seeds) {
    for (const RedstoneUpdate& update : recompute_redstone_component(
             sx, sy, sz, is_wire, source_signal, current_power, max_nodes)) {
      const BlockStateKey state = get(update.x, update.y, update.z);
      if (!is_wire_state(state) || property_int(state, "power") == update.power)
        continue;
      settle_write(update.x, update.y, update.z,
                   BlockStateKey{state.name, with_property(state.properties, "power",
                                                           std::to_string(update.power))});
      changed.push_back(update);
    }
  }

  // Lamps are not wires, but are the observable consumer: `RedstoneLampBlock`
  // asks `hasNeighborSignal`, i.e. whether any of its six neighbours emits a
  // signal. Update them once the wire graph has settled.
  // A sorted, deduplicated vector rather than a `std::set`: the iteration order
  // (lexicographic by position, so the lamp updates land in the same order and
  // the same packets go out) is identical, without a tree node per candidate.
  std::vector<Pos> candidates;
  candidates.emplace_back(x, y, z);
  for (const RedstoneUpdate& update : changed) {
    candidates.emplace_back(update.x, update.y, update.z);
    for (int i = 0; i < 6; ++i)
      candidates.emplace_back(update.x + kDx[i], update.y + kDy[i], update.z + kDz[i]);
  }
  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
  for (const auto& [cx, cy, cz] : candidates) {
    const BlockStateKey state = get(cx, cy, cz);
    if (state.name != "minecraft:redstone_lamp")
      continue;
    int signal = 0;
    for (int i = 0; i < 6; ++i)
      signal = std::max(signal, emitted_toward(cx + kDx[i], cy + kDy[i], cz + kDz[i],
                                               opposite_face(i), kGateDepth));
    const bool lit = block_property(state.properties, "lit") == "true";
    if (lit != (signal > 0))
      settle_write(cx, cy, cz,
                   BlockStateKey{state.name, with_property(state.properties, "lit",
                                                           signal > 0 ? "true" : "false")});
  }
  return changed;
}

namespace {

// The same emission rules `update_redstone_area` settles with, read straight
// from the world for one cell. The settlement keeps a memoised copy (it asks the
// same cell four or five times per settle); the gate ticks below ask about a
// handful of cells once, so they read the world directly. Both answer from the
// same three predicates, which is what keeps them from drifting apart.
int emitted_from(const std::function<BlockStateKey(int, int, int)>& get, int x, int y, int z,
                 int dir, int depth) {
  const BlockStateKey state = get(x, y, z);
  if (is_wire_state(state))
    return std::clamp(property_int(state, "power"), 0, 15);
  if (is_repeater_state(state))
    return (block_property(state.properties, "powered") == "true" && dir == gate_facing(state)) ? 15
                                                                                                : 0;
  if (is_comparator_state(state)) {
    if (dir != gate_facing(state) || depth <= 0)
      return 0;
    const int facing = gate_facing(state);
    const int input =
        emitted_from(get, x - kDx[facing], y - kDy[facing], z - kDz[facing], facing, depth - 1);
    int side = 0;
    for (int f = 2; f < 6; ++f) {
      if (f == facing || f == opposite_face(facing))
        continue;
      side =
          std::max(side, emitted_from(get, x + kDx[f], y, z + kDz[f], opposite_face(f), depth - 1));
    }
    if (block_property(state.properties, "mode") == "subtract")
      return std::clamp(input - side, 0, 15);
    return input >= side ? input : 0;
  }
  return is_power_source_state(state) ? 15 : 0;
}

// The strongest signal reaching (x, y, z) from its six neighbours.
int best_neighbour_signal(const std::function<BlockStateKey(int, int, int)>& get, int x, int y,
                          int z) {
  int best = 0;
  for (int f = 0; f < 6; ++f)
    best =
        std::max(best, emitted_from(get, x + kDx[f], y + kDy[f], z + kDz[f], opposite_face(f), 8));
  return best;
}

// `RepeaterBlock.isLocked`: a powered repeater or comparator pointing into
// either side of this one holds it.
bool gate_locked(const std::function<BlockStateKey(int, int, int)>& get, const BlockStateKey& state,
                 int x, int y, int z) {
  const int facing = gate_facing(state);
  for (int f = 2; f < 6; ++f) {
    if (f == facing || f == opposite_face(facing))
      continue;
    const BlockStateKey side = get(x + kDx[f], y, z + kDz[f]);
    if (!is_repeater_state(side) && !is_comparator_state(side))
      continue;
    if (block_property(side.properties, "powered") != "true")
      continue;
    // It must point *into* this cell: its facing is the direction toward us.
    if (gate_facing(side) == opposite_face(f))
      return true;
  }
  return false;
}

} // namespace

int redstone_gate_input(const std::function<BlockStateKey(int, int, int)>& get, int x, int y,
                        int z) {
  const BlockStateKey state = get(x, y, z);
  const int facing = gate_facing(state);
  // `DiodeBlock.getInputSignal`: the block behind, read toward the gate.
  return emitted_from(get, x - kDx[facing], y - kDy[facing], z - kDz[facing], facing, 8);
}

int redstone_gate_tick_delay(const BlockStateKey& state) {
  if (is_repeater_state(state)) {
    const int delay = std::clamp(property_int(state, "delay"), 1, 4);
    return delay * 2; // RepeaterBlock.getDelay: two game ticks per delay step
  }
  return 2; // AbstractRedstoneGateBlock.getTickDelay
}

std::optional<BlockStateKey>
redstone_gate_next_state(const std::function<BlockStateKey(int, int, int)>& get, int x, int y,
                         int z) {
  if (!get)
    return std::nullopt;
  const BlockStateKey state = get(x, y, z);
  if (is_repeater_state(state)) {
    if (gate_locked(get, state, x, y, z))
      return std::nullopt;
    const bool want = redstone_gate_input(get, x, y, z) > 0;
    if ((block_property(state.properties, "powered") == "true") == want)
      return std::nullopt;
    return BlockStateKey{state.name,
                         with_property(state.properties, "powered", want ? "true" : "false")};
  }
  if (is_comparator_state(state)) {
    const int output = emitted_from(get, x, y, z, gate_facing(state), 8);
    const bool want = output > 0;
    if ((block_property(state.properties, "powered") == "true") == want)
      return std::nullopt;
    return BlockStateKey{state.name,
                         with_property(state.properties, "powered", want ? "true" : "false")};
  }
  return std::nullopt;
}

int redstone_comparator_output(const std::function<BlockStateKey(int, int, int)>& get, int x, int y,
                               int z) {
  if (!get)
    return 0;
  const BlockStateKey state = get(x, y, z);
  if (!is_comparator_state(state))
    return 0;
  return emitted_from(get, x, y, z, gate_facing(state), 8);
}

int redstone_signal_into(const std::function<BlockStateKey(int, int, int)>& get, int x, int y,
                         int z) {
  return best_neighbour_signal(get, x, y, z);
}

} // namespace mc::world
