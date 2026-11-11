// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "world/chunk.h"

namespace mc::world {

// The six neighbouring block positions in vanilla Direction order.
struct RedstoneNeighbours {
  int direct[6] = {0, 0, 0, 0, 0, 0};
  int wire[6] = {0, 0, 0, 0, 0, 0};
};

struct RedstoneRules {
  // The ordinary side signal is separate from the direct values supplied in
  // RedstoneNeighbours because vanilla exposes getSignal and getDirectSignal
  // as distinct queries.
  std::function<int(int, int, int, int)> side_signal;
};

int clamp_redstone_power(int value);

// The signal a block receives from its six neighbours. Wire power loses one per
// edge and never powers through a wire's own strongest side twice.
int redstone_received_signal(const RedstoneRules& rules, int x, int y, int z,
                             const RedstoneNeighbours& neighbours);

// Vanilla's wire attenuation: a wire's next power is max(source, neighbouring
// wire - 1), capped to 15. The caller supplies a source signal and six adjacent
// wire powers in Direction order.
int redstone_wire_power(int source, const std::array<int, 6>& adjacent_wire_power);

// Recompute a connected wire component after a source or wire changes. The
// callback owns block-state writes and may schedule the next update; this pure
// traversal is bounded by max_nodes to avoid an adversarial redstone loop
// monopolising the server tick.
struct RedstoneUpdate {
  int x = 0, y = 0, z = 0, power = 0;
};
std::vector<RedstoneUpdate> recompute_redstone_component(
    int origin_x, int origin_y, int origin_z, const std::function<bool(int, int, int)>& is_wire,
    const std::function<int(int, int, int)>& source_signal,
    const std::function<int(int, int, int)>& current_power, int max_nodes = 4096);

// Applies the live block-state part of redstone after a block mutation, and
// returns the wire cells whose power it moved. The relaxation itself belongs to
// `recompute_redstone_component`: this adapter only seeds it from every wire the
// change touches, splits the two signals the way vanilla does (a strong source
// feeds a wire; a wire's own power reaches a *consumer* undiminished and reaches
// the next wire through `redstone_wire_power`'s decrement) and updates the
// powered lamps around the result. `write` owns the actual world mutation and
// client notification; the callback is injected so the graph stays testable
// without a server/session. The returned cells are the ones an after-write hook
// owes a neighbour pass.
std::vector<RedstoneUpdate>
update_redstone_area(const std::function<BlockStateKey(int, int, int)>& get,
                     const std::function<void(int, int, int, const BlockStateKey&)>& write, int x,
                     int y, int z, int max_nodes = 4096);

// ---- the two redstone gates -------------------------------------------------
//
// A repeater and a comparator are not wires: they read one input (the repeater)
// or three (the comparator's rear and two sides), latch it, and hand the result
// out **only toward the face they point at**. `update_redstone_area` reads that
// directionality through the same predicates these functions use
// (`redstone.cpp`'s `emitted_from`), which is what keeps a settlement and a gate
// tick from disagreeing.

// `DiodeBlock.getInputSignal`: the signal the block behind the gate hands it.
int redstone_gate_input(const std::function<BlockStateKey(int, int, int)>& get, int x, int y,
                        int z);

// How many game ticks a scheduled gate tick waits: `delay * 2` for a repeater
// (RepeaterBlock.getDelay), two for a comparator.
int redstone_gate_tick_delay(const BlockStateKey& state);

// The state a due gate tick writes, or nullopt when the gate already agrees with
// its inputs (or a repeater is locked).
std::optional<BlockStateKey>
redstone_gate_next_state(const std::function<BlockStateKey(int, int, int)>& get, int x, int y,
                         int z);

// `ComparatorBlock.getOutputSignal`: the comparator's 0..15 answer.
int redstone_comparator_output(const std::function<BlockStateKey(int, int, int)>& get, int x, int y,
                               int z);

// `Level.getBestNeighborSignal` for one cell: the strongest signal its six
// neighbours hand it.
int redstone_signal_into(const std::function<BlockStateKey(int, int, int)>& get, int x, int y,
                         int z);

} // namespace mc::world
