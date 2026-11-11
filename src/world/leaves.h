// SPDX-License-Identifier: GPL-3.0-or-later
//
// `LeavesBlock`: the distance a leaf keeps from the wood that holds it up, and
// the decay that follows when there is none (Phase 3 milestone J).
//
// A leaf carries a `distance` property — its breadth-first distance to the
// nearest log, 1 for a leaf touching wood and 7 for one that might as well be
// floating:
//
//     getOptionalDistanceAt(state) = state.is(#minecraft:logs) ? 0
//                                  : state.hasProperty(DISTANCE) ? state.getValue(DISTANCE)
//                                  : EMPTY          // -> getDistanceAt's orElse(7)
//
// Two things read it. The random tick decays a leaf that is *already* at
// distance 7 and not persistent (`randomTick` -> `decaying` -> drop, remove),
// and the distance itself is recomputed by a scheduled tick: `updateShape` on a
// leaf whose neighbour changed schedules one, and the tick writes
// `updateDistance(state, level, pos)` — one relaxation step over the six
// neighbours, `min(getDistanceAt(neighbour) + 1)`.
//
// That scheduled tick is what makes a chopped tree fall apart. Breaking a log
// notifies its neighbours (flag 3), each leaf next to it schedules a tick, the
// tick writes a larger distance and *that* write notifies *its* neighbours, so
// the new distance walks outward one layer per tick and every leaf that reaches
// 7 is then decayed by its own random tick. This module owns the whole chain
// because the engine has no general neighbour-update pass: `leaves_notify_neighbours_of_change`
// is the leaf half of `Level.setBlock(..., 3)`, and the caller runs it after a
// block write (see the session's break path and `/setblock`).
//
// Modelled, and from where:
//   * `LeavesBlock.isRandomlyTicking` / `decaying` / `randomTick` /
//     `updateDistance` / `getDistanceAt` / `getOptionalDistanceAt` — all of it;
//   * `LeavesBlock.tick` — the 1-tick distance recompute;
//   * `LeavesBlock.updateShape` — the schedule-a-tick-on-a-changed-neighbour
//     rule, minus its waterlogged branch;
//   * `LeavesBlock.getStateForPlacement` — a *placed* leaf is persistent (it is
//     a decoration, not a canopy), so it is placed at its own distance from the
//     wood around it and never decays. The session's placement path calls
//     `leaves_placement_state` for it.
//
// What is NOT modelled, and why:
//   * **waterlogging.** `WATERLOGGED` is carried in the state (and round-trips
//     through the chunk), but there is no fluid tick, so a leaf in water neither
//     schedules the water nor drains it;
//   * **`dropResources`' tools.** A leaf broken by hand honours the *player's*
//     tool through the block-drop path; a decayed leaf drops what bare hands
//     would (`world/loot.h`'s rolled resolver, which is the sapling/stick/apple
//     chance pools the table carries — shears and silk touch are not modelled
//     anywhere in this engine);
//   * **the leaf particles** (`animateTick`: the dripping-water and falling-leaf
//     chances). Those are client-side effects the server has no packet for.
#pragma once

#include <string_view>

#include "world/chunk.h"
#include "world/random_tick.h"

namespace mc::world {

// `LeavesBlock.DECAY_DISTANCE`: the distance a leaf decays at.
constexpr int kLeavesDecayDistance = 7;

bool leaves_is_leaves(std::string_view block_name);

// `state.getValue(DISTANCE)` / `getValue(PERSISTENT)`. A state that carries
// neither (a block that is not a leaf) answers 7 / false, which is what the
// distance of a leaf that is not there at all would be.
int leaves_distance(const BlockStateKey& state);
bool leaves_persistent(const BlockStateKey& state);

// `getDistanceAt(state)`: 0 for `#minecraft:logs` (a leaf's anchor), the leaf's
// own distance for a leaf, and 7 for everything else — which is what makes a
// canopy with no wood under it decay.
int leaves_distance_at(const BlockStateKey& state);

// `LeavesBlock.isRandomlyTicking` and `decaying`: distance 7, not persistent.
bool leaves_is_randomly_ticking(const BlockStateKey& state);
bool leaves_decaying(const BlockStateKey& state);

// `LeavesBlock.updateDistance`: the state at one relaxation step.
BlockStateKey leaves_update_distance(const RandomTickLevel& level, const BlockStateKey& state,
                                     int x, int y, int z);

// `LeavesBlock.updateShape` for the leaf at (x, y, z) whose neighbour at
// `neighbour` now holds `neighbour_state`: schedules the leaf's own distance
// update unless the neighbour is already anchoring it at its current distance.
void leaves_update_shape(const RandomTickLevel& level, const BlockStateKey& leaf,
                         const BlockStateKey& neighbour_state, int x, int y, int z);

// The leaf half of `Level`'s neighbour update (flag 3) for a block that
// changed at (x, y, z): every *adjacent leaf* gets `leaves_update_shape`. Called
// by the module's own writes and by the caller after a block write or break.
void leaves_notify_neighbours_of_change(const RandomTickLevel& level, int x, int y, int z,
                                        const BlockStateKey& changed_state);

// `LeavesBlock.tick`: write `updateDistance` with flag 3 (clients *and*
// neighbours — this is the step that propagates a chop through the canopy).
bool leaves_scheduled_tick(const RandomTickLevel& level, const BlockStateKey& state, int x, int y,
                           int z);

// `LeavesBlock.randomTick`: `dropResources` then `removeBlock(pos, false)`.
bool leaves_random_tick(const RandomTickLevel& level, const RandomTickRandom& rng,
                        const BlockStateKey& state, int x, int y, int z);

// `LeavesBlock.getStateForPlacement`: `PERSISTENT=true` plus the distance from
// the surrounding wood, which is what a player-placed leaf is.
BlockStateKey leaves_placement_state(const RandomTickLevel& level, std::string_view block_name,
                                     int x, int y, int z);

// The fixed point of `updateDistance` over one column, for the generator: every
// leaf's distance brought to the value the scheduled-tick relaxation above
// would walk it to (1 next to wood, one more per leaf step, 7 for a leaf no wood
// reaches within the decay distance).
//
// A *generated* canopy owes this because vanilla gets it from the block updates
// a feature's `setBlock` runs: `updateShape` on each leaf whose neighbour
// changed schedules the recompute, the ticks walk the distance out from the
// trunk one layer at a time, and the chunk a server persists (and streams, and
// decays from) carries distances 1..7. This generator writes cells directly and
// never ticked them, so every leaf a tree placed kept the registry default —
// `distance=7`, `persistent=false` — which `leaves_decaying` answers true for:
// a freshly generated canopy was a canopy every random tick would eat, and its
// first drop woke the standing reschedule load `docs/PHASE3.md` records
// (`docs/PHASE5.md` measures what that did to a served world). The stage now
// settles the distances at generation, and this function is that step.
//
// The reads are the column's own cells: a feature writes nothing outside its
// column (`worldgen_feature.cpp`'s `try_set`), and the part of a canopy a
// column keeps is the part between it and its trunk, so an in-column fixed
// point is the canopy vanilla would have reached. The one shape it cannot
// settle is a leaf whose only path to wood crosses a chunk border — with the
// neighbour generated later, nothing here can see it — and such a leaf stays at
// 7 until a live-world recompute reads the neighbours it now has: 7 of seed
// 42's 25 tracked chunks' 13,057 leaves, against 123 the rule cannot anchor
// anywhere (`docs/PHASE5.md` §4, `tests/worldgen_feature_test.cpp`). Only
// `distance` is rewritten — the block, the persistence and every other property
// stay — and only on leaves, so nothing else in the column is touched.
//
// Returns how many cells it wrote. Costs one palette walk over the column plus
// a pass over the leaf-bearing sections per relaxation layer (see the .cpp).
long long settle_generated_leaf_distances(Chunk& chunk);

} // namespace mc::world
