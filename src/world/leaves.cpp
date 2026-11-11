// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/leaves.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "world/block_tags.h"

namespace mc::world {
namespace {

// `LeavesBlock.TICK_DELAY`: `updateShape` schedules the distance update one
// tick out, so a chop walks the canopy one layer per tick.
constexpr int kLeavesTickDelay = 1;

// The block a removed leaf leaves behind: `FluidState.createLegacyBlock()` with
// no fluid modelled.
const BlockStateKey& air_state() {
  static const BlockStateKey air{"minecraft:air", ""};
  return air;
}

const GeneratedRandomTickBlock* find_block(std::string_view name) {
  const GeneratedRandomTickBlock* begin = kRandomTickBlocks;
  const GeneratedRandomTickBlock* end = kRandomTickBlocks + kRandomTickBlockCount;
  const GeneratedRandomTickBlock* it = std::lower_bound(
      begin, end, name, [](const GeneratedRandomTickBlock& entry, std::string_view key) {
        return std::string_view(entry.name) < key;
      });
  if (it == end || std::string_view(it->name) != name)
    return nullptr;
  return it;
}

// `Direction.values()`: DOWN, UP, NORTH, SOUTH, WEST, EAST.
constexpr std::array<std::array<int, 3>, 6> kNeighbours = {{
    {0, -1, 0},
    {0, 1, 0},
    {0, 0, -1},
    {0, 0, 1},
    {-1, 0, 0},
    {1, 0, 0},
}};

// `LeavesBlock.updateDistance` over a snapshot of the six neighbours, in
// `kNeighbours`' order. One implementation for both callers: the scheduled tick
// reads the neighbours from its level, the generation-time settle reads them
// from the column it owns (`settle_generated_leaf_distances`), and the two
// cannot drift apart.
BlockStateKey update_distance_from(const std::array<BlockStateKey, 6>& neighbours,
                                   const BlockStateKey& state) {
  int new_distance = kLeavesDecayDistance;
  for (const BlockStateKey& neighbour : neighbours) {
    new_distance = std::min(new_distance, leaves_distance_at(neighbour) + 1);
    if (new_distance == 1)
      break;
  }
  return state_with_prop_int(state, "distance", new_distance);
}

// The write half of `setBlock(pos, state, 3)`: the block, then the neighbour
// update. Vanilla calls `updateShape` on all six neighbours for any change;
// with no general neighbour-update pass, only the leaves react here.
void write_and_notify(const RandomTickLevel& level, int x, int y, int z,
                      const BlockStateKey& state) {
  level.set_block(x, y, z, state, /*notify=*/true);
  leaves_notify_neighbours_of_change(level, x, y, z, state);
}

} // namespace

bool leaves_is_leaves(std::string_view block_name) {
  const GeneratedRandomTickBlock* entry = find_block(block_name);
  return entry != nullptr && entry->kind == kRandomTickLeaves;
}

int leaves_distance(const BlockStateKey& state) {
  return state_prop_int(state.properties, "distance").value_or(kLeavesDecayDistance);
}

bool leaves_persistent(const BlockStateKey& state) {
  return state_prop_bool(state.properties, "persistent").value_or(false);
}

int leaves_distance_at(const BlockStateKey& state) {
  if (block_has_tag(kBlockTagPreventsNearbyLeafDecay, state.name))
    return 0;
  if (leaves_is_leaves(state.name))
    return leaves_distance(state);
  return kLeavesDecayDistance;
}

bool leaves_is_randomly_ticking(const BlockStateKey& state) {
  return leaves_decaying(state);
}

bool leaves_decaying(const BlockStateKey& state) {
  return !leaves_persistent(state) && leaves_distance(state) == kLeavesDecayDistance;
}

BlockStateKey leaves_update_distance(const RandomTickLevel& level, const BlockStateKey& state,
                                     int x, int y, int z) {
  std::array<BlockStateKey, 6> neighbours;
  for (std::size_t i = 0; i < kNeighbours.size(); ++i)
    neighbours[i] =
        level.get_block(x + kNeighbours[i][0], y + kNeighbours[i][1], z + kNeighbours[i][2]);
  return update_distance_from(neighbours, state);
}

void leaves_update_shape(const RandomTickLevel& level, const BlockStateKey& leaf,
                         const BlockStateKey& neighbour_state, int x, int y, int z) {
  // `updateShape`'s own condition: schedule unless the changed neighbour is
  // already the anchor this leaf sits at.
  const int distance_from_neighbour = leaves_distance_at(neighbour_state) + 1;
  if (distance_from_neighbour != 1 || leaves_distance(leaf) != distance_from_neighbour) {
    if (level.schedule_tick)
      level.schedule_tick(x, y, z, kLeavesTickDelay);
  }
}

void leaves_notify_neighbours_of_change(const RandomTickLevel& level, int x, int y, int z,
                                        const BlockStateKey& changed_state) {
  for (const std::array<int, 3>& offset : kNeighbours) {
    const int nx = x + offset[0], ny = y + offset[1], nz = z + offset[2];
    const BlockStateKey neighbour = level.get_block(nx, ny, nz);
    if (!leaves_is_leaves(neighbour.name))
      continue;
    leaves_update_shape(level, neighbour, changed_state, nx, ny, nz);
  }
}

bool leaves_scheduled_tick(const RandomTickLevel& level, const BlockStateKey& state, int x, int y,
                           int z) {
  if (!leaves_is_leaves(state.name))
    return false;
  // `Level.setBlock`'s no-op, which `write_and_notify` above does not model:
  // `LevelChunk.setBlockState` returns null when the value it is handed is the
  // one the cell already holds (`oldState == state`, a comparison of canonical
  // state objects), and `Level.setBlock` then skips the wire update *and* the
  // neighbour update. Without this, a recompute that yields the distance the
  // leaf already had still re-notifies every leaf around it, and two adjacent
  // leaves whose log is gone keep telling each other to re-derive the same
  // value forever — the standing load `docs/PHASE3.md` records for the blob run,
  // and the reason the forested square of `docs/PHASE5.md` §4 could not be
  // measured. This tick is the one leaf write path that can produce an
  // identical state; the drop path writes air.
  const BlockStateKey updated = leaves_update_distance(level, state, x, y, z);
  if (updated != state)
    write_and_notify(level, x, y, z, updated);
  return true;
}

bool leaves_random_tick(const RandomTickLevel& level, const RandomTickRandom& /*rng*/,
                        const BlockStateKey& state, int x, int y, int z) {
  if (!leaves_decaying(state))
    return false;
  // `LeavesBlock.dropResources(state, level, pos)` with no tool: the caller
  // resolves the loot (its own rolled pools) and spawns the item entities.
  if (level.drop_resources)
    level.drop_resources(x, y, z, state);
  // `level.removeBlock(pos, false)`: flag 3, so the neighbours are notified and
  // a leaf that was only alive thanks to this one starts its own countdown.
  write_and_notify(level, x, y, z, air_state());
  return true;
}

BlockStateKey leaves_placement_state(const RandomTickLevel& level, std::string_view block_name,
                                     int x, int y, int z) {
  // `getStateForPlacement`: the default state with PERSISTENT=true (and the
  // fluid at the position, which is always air here), then the distance from
  // what is around it. A placed leaf never decays.
  BlockStateKey state{std::string(block_name), "distance=7,persistent=true,waterlogged=false"};
  return leaves_update_distance(level, state, x, y, z);
}

long long settle_generated_leaf_distances(Chunk& chunk) {
  // Which sections can hold a leaf at all: a palette walk per section, so a
  // column of stone and air costs its palettes (24 of them) rather than 98k
  // cell reads. `Chunk` keeps its palettes per section, so this is exact — a
  // section with no leaf in its palette has no leaf in its cells.
  std::vector<uint8_t> work(chunk.sections.size(), 0);
  bool any = false;
  for (std::size_t i = 0; i < chunk.sections.size(); ++i) {
    for (const BlockStateKey& entry : chunk.sections[i].block_palette) {
      if (!leaves_is_leaves(entry.name))
        continue;
      work[i] = 1;
      any = true;
      break;
    }
  }
  if (!any)
    return 0;

  // Each pass relaxes every leaf the previous pass could have moved. The first
  // pass visits every leaf-bearing section; after that only the sections
  // touching a cell that changed, which is where the wave can still be (a leaf
  // takes its answer from a neighbour, so nothing else can move). A relaxation
  // can move a leaf by one layer per pass and no leaf is deeper than the decay
  // distance, so the fixed point is reached within those passes; the extra one
  // is the sweep that confirms nothing moved.
  long long written = 0;
  const int kPasses = kLeavesDecayDistance + 1;
  for (int pass = 0; pass < kPasses; ++pass) {
    std::vector<uint8_t> next(chunk.sections.size(), 0);
    long long changed = 0;
    for (std::size_t si = 0; si < chunk.sections.size(); ++si) {
      if (work[si] == 0)
        continue;
      const int base_y = (chunk.min_section_y + static_cast<int>(si)) * 16;
      for (int ly = 0; ly < 16; ++ly) {
        const int y = base_y + ly;
        for (int z = 0; z < 16; ++z) {
          for (int x = 0; x < 16; ++x) {
            const BlockStateKey state = chunk.block_state_at(x, y, z);
            if (!leaves_is_leaves(state.name))
              continue;
            std::array<BlockStateKey, 6> neighbours;
            for (std::size_t i = 0; i < kNeighbours.size(); ++i)
              neighbours[i] = chunk.block_state_at(x + kNeighbours[i][0], y + kNeighbours[i][1],
                                                   z + kNeighbours[i][2]);
            const BlockStateKey settled = update_distance_from(neighbours, state);
            if (settled.properties == state.properties)
              continue;
            chunk.set_block_state(x, y, z, settled);
            ++changed;
            // What can see this cell next: its own section and the two stacked
            // against it. The same-section case covers the column's own x/z
            // neighbours; a cell on the column's edge has no neighbour there.
            next[si] = 1;
            if (si > 0)
              next[si - 1] = 1;
            if (si + 1 < chunk.sections.size())
              next[si + 1] = 1;
          }
        }
      }
    }
    written += changed;
    if (changed == 0)
      break;
    work.swap(next);
  }
  return written;
}

} // namespace mc::world
