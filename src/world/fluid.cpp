// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/fluid.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

#include "world/container.h"

namespace mc::world {
namespace {

int floor_div16(int v) {
  return v >= 0 ? v / 16 : (v - 15) / 16;
}
int level_for_amount(int amount, bool falling) {
  if (amount >= 9 && !falling)
    return 0;
  if (falling)
    return 8;
  return std::clamp(8 - amount, 1, 7);
}
FluidKind kind_from_name(std::string_view name) {
  if (name == "minecraft:water" || name == "minecraft:bubble_column")
    return FluidKind::Water;
  if (name == "minecraft:lava")
    return FluidKind::Lava;
  return FluidKind::Empty;
}

// One cell's answer for one tick: the state the read named and the fluid it
// holds. Both halves are worth keeping for the tick — a read is a session
// `get_block` (a chunk lookup) and `fluid_state_of` parses the state's own
// properties — and a tick asks about the same neighbour as many as three times
// over (the supply scan, the recompute branch's fan-out, the spread loop, whose
// own write path asks the target again). A write through `spread_to` refreshes
// the entry it holds, because the retract's neighbour fan-out reads a cell the
// same tick may just have written below it.
struct CellRead {
  BlockStateKey key;
  FluidState fluid;
};

CellRead read_cell(const FluidLevel& level, int x, int y, int z) {
  CellRead out;
  out.key = level.get_block(x, y, z);
  out.fluid = fluid_state_of(out.key);
  return out;
}

// The fluid body's ONE write rule: may this fluid write `state`? Vanilla
// never lets a spread overwrite a source of the same fluid — sources are
// stable. Allowing it made a pool's backflow destroy its own sources, so both
// sides rewrote and re-scheduled each other every tick and the queue could
// never drain. A different fluid is not ours to touch; the level's hold
// predicate is the rest of the answer (it is also what rules out writing a
// fluid-empty cell of stone). Every spread site routes through here, and the
// caller passes the fluid state it already read, so the rule never re-reads a
// cell to ask its own question.
bool can_replace(const FluidLevel& level, const BlockStateKey& state, const FluidState& fluid,
                 FluidKind kind) {
  if (!fluid.empty()) {
    if (fluid.kind != kind || fluid.source())
      return false;
  }
  return !level.can_hold_fluid || level.can_hold_fluid(state);
}

// The fluid body's feed rule: the same fluid directly above means this cell is
// fed and must keep the state it was given — deleting it while the source
// above re-filled it on every heartbeat made an endless self-erasing waterfall
// (one write to fall, one write to erase), multiplied by every cave column.
// Spelled once because fluid_tick's delete guard and fluid_shape's surface
// height both need the identical question answered.
inline bool fed_from_above(const FluidState& above, FluidKind kind) {
  return above.kind == kind && !above.empty();
}

// A cell going to air owes its neighbours the tick vanilla's
// `Level.setBlock(..., 3)` gives them: cells fed through this one, and cells
// whose only "supply" was it, have to re-evaluate — that notification *is*
// what drains a stream one cell after another. Only same-fluid neighbours are
// scheduled: the queue dedupes, and air or stone beside a hole has nothing to
// re-evaluate. Its six neighbours are the ones the tick that decided to retract
// has already read (`sides` in the tick's own +x, -x, +z, -z order), so the
// fan-out takes them rather than reading the world a second time. The
// schedule order is the offsets' own — +x, -x, +y, +z, -y, -z — and it is part
// of the pin: the entry's bucket holds these in insertion order.
void retract_to_air(FluidLevel& level, FluidKind kind, int x, int y, int z, const CellRead& below,
                    const CellRead& above, const CellRead* sides) {
  level.set_block(x, y, z, BlockStateKey{"minecraft:air", ""});
  if (!level.schedule_tick)
    return;
  constexpr int rx[] = {1, -1, 0, 0, 0, 0};
  constexpr int ry[] = {0, 0, 1, 0, -1, 0};
  constexpr int rz[] = {0, 0, 0, 1, 0, -1};
  const FluidState around[6] = {sides[0].fluid, sides[1].fluid, above.fluid,
                                sides[2].fluid, below.fluid,    sides[3].fluid};
  const int delay = fluid_tick_delay(kind);
  for (int i = 0; i < 6; ++i) {
    if (around[i].kind == kind)
      level.schedule_tick(x + rx[i], y + ry[i], z + rz[i], delay);
  }
}

BlockStateKey placed_state(const BlockStateKey& old, FluidKind kind, int amount, bool falling) {
  if (block_property(old.properties, "waterlogged") == "false" && kind == FluidKind::Water)
    return BlockStateKey{old.name, with_property(old.properties, "waterlogged", "true")};
  return fluid_block_state(kind, amount, falling);
}

// Returns whether a block actually changed — the tick loop only re-queues a
// cell that moved, so a settled fluid stops ticking instead of heartbeating
// forever. The target is the caller's own read of that cell, and the write puts
// the new state back in it: the retract's fan-out reads the neighbours after a
// write, so a cache left stale here would silently drop a schedule.
bool spread_to(FluidLevel& level, FluidKind kind, int x, int y, int z, CellRead& target, int amount,
               bool falling) {
  if (!can_replace(level, target.key, target.fluid, kind))
    return false;
  const BlockStateKey next = placed_state(target.key, kind, amount, falling);
  if (next == target.key)
    return false;
  level.set_block(x, y, z, next);
  target.key = next;
  target.fluid = fluid_state_of(next);
  if (level.schedule_tick)
    level.schedule_tick(x, y, z, fluid_tick_delay(kind));
  return true;
}

} // namespace

FluidState fluid_state_of(const BlockStateKey& state) {
  if (state.name != "minecraft:water" && state.name != "minecraft:lava" &&
      state.name != "minecraft:bubble_column") {
    if (block_property(state.properties, "waterlogged") == "true")
      return FluidState{FluidKind::Water, 9, false};
    return {};
  }
  const FluidKind kind = kind_from_name(state.name);
  const std::string level_text = block_property(state.properties, "level");
  int level = 0;
  if (!level_text.empty()) {
    // State tables are trusted, but a malformed external world should not
    // terminate the server from a parser exception; vanilla treats unknown
    // fluid properties as the default source state while loading.
    try {
      level = std::stoi(level_text);
    } catch (...) {
      level = 0;
    }
  }
  if (level == 0)
    return FluidState{kind, 9, false};
  if (level >= 8)
    return FluidState{kind, 9, true};
  return FluidState{kind, std::clamp(8 - level, 1, 7), false};
}

BlockStateKey fluid_block_state(FluidKind kind, int amount, bool falling) {
  const char* name = kind == FluidKind::Lava ? "minecraft:lava" : "minecraft:water";
  return BlockStateKey{name, "level=" + std::to_string(level_for_amount(amount, falling))};
}

FluidShape fluid_shape(const FluidState& state, const FluidState& above) {
  if (state.empty())
    return {};
  return FluidShape{
      0.0f, state.amount == 9 && fed_from_above(above, state.kind) ? 1.0f : state.own_height()};
}

int fluid_drop_off(FluidKind kind) {
  return kind == FluidKind::Lava ? 2 : 1;
}
int fluid_tick_delay(FluidKind kind) {
  return kind == FluidKind::Lava ? 30 : 5;
}

void fluid_tick(FluidLevel& level, FluidKind kind, int x, int y, int z) {
  if (!level.get_block || !level.set_block)
    return;
  const BlockStateKey here_key = level.get_block(x, y, z);
  FluidState here = fluid_state_of(here_key);
  if (here.empty() || here.kind != kind)
    return;

  CellRead below = read_cell(level, x, y - 1, z);
  // The vertical branch asks the same write rule the lateral spread asks:
  // can_replace's source protection is exactly "not an own-kind source", so
  // its formula — (empty or own-kind-not-source) and the hold predicate — is
  // case-for-case this branch: a source below is never open, and a fluid-empty
  // cell of *stone* is ruled out by the hold predicate. It goes through the
  // helper now instead of being re-derived by hand here.
  const bool below_open = can_replace(level, below.key, below.fluid, kind);
  const CellRead above = read_cell(level, x, y + 1, z);
  // The four sides, read once for the whole tick: the supply scan below, the
  // recompute branch's own fan-out, the spread loop and the retract's neighbour
  // fan-out all ask about these same four cells, and nothing between them
  // writes one — the only writes in this tick go to this cell, the cell below,
  // or a side cell in the spread loop, whose new state `spread_to` puts back in
  // the cache.
  constexpr int dx[] = {1, -1, 0, 0};
  constexpr int dz[] = {0, 0, 1, -1};
  CellRead sides[4];
  for (int i = 0; i < 4; ++i)
    sides[i] = read_cell(level, x + dx[i], y, z + dz[i]);

  // FlowingFluid.getNewLiquid: two source neighbours create a source when the
  // fluid supports source conversion (water; lava does not in the overworld).
  int highest = 0, sources = 0;
  for (int i = 0; i < 4; ++i) {
    const FluidState& side = sides[i].fluid;
    if (side.kind != kind || side.empty())
      continue;
    highest = std::max(highest, side.amount);
    if (side.source())
      ++sources;
  }
  FluidState next = here;
  if (!here.source() && sources >= 2 && kind == FluidKind::Water &&
      (block_blocks_motion(below.key.name) || !below.fluid.empty()))
    next = FluidState{kind, 9, false};
  else if (highest > 0 && !here.source()) {
    // A source never downgrades because of a same-fluid neighbour — vanilla's
    // getNewLiquid returns the source unchanged — so a pool's interior stays
    // put instead of rewriting itself to flowing every tick.
    int amount = highest - fluid_drop_off(kind);
    // 8 is not representable in the level encoding (level 1 reads back as
    // amount 7), and a computed 8 against a stored 7 would make a cell rewrite
    // a no-op state and re-queue itself forever; clamp to the highest amount
    // the encoding can round-trip.
    if (amount > 7)
      amount = 7;
    next = amount > 0 ? FluidState{kind, amount, false} : FluidState{};
  } else if (!here.source()) {
    next = {};
  }

  // Whether any block (this one or a neighbour) was written this tick. Only a
  // cell that moved re-queues itself: unconditionally re-queueing kept every
  // settled fluid block on a permanent 5-tick heartbeat, so the queue never
  // drained even once the pool had been stable for minutes.
  bool changed = false;

  // `below_open` IS the write rule (can_replace), so no second clause is
  // owed here: a fluid-empty cell of stone fails the hold predicate inside it,
  // and any nonempty target it passes is own-kind by can_replace's own source
  // rule — which is exactly the "empty or own-kind" test this branch used to
  // restate by hand.
  if (below_open) {
    // A downward flow is the full falling state. Do not replace a same-fluid
    // source: vanilla's source block is stable and cannot be downgraded.
    if (below.fluid.empty() || !below.fluid.source())
      changed = spread_to(level, kind, x, y - 1, z, below, 9, true);
    if (here.source() && sources < 3 && changed) {
      // A source that successfully flowed downward returns before spreading
      // sideways. If the cell below is already a source, however, vanilla may
      // still fan the source out horizontally; returning on a no-op here made
      // a source sitting on solid ground fail the lateral-flow case entirely.
      if (level.schedule_tick)
        level.schedule_tick(x, y, z, fluid_tick_delay(kind));
      return;
    }
    if (!here.source() && next.empty() && !fed_from_above(above.fluid, kind)) {
      retract_to_air(level, kind, x, y, z, below, above, sides);
      return;
    }
  } else if (!next.empty() && next != here) {
    level.set_block(x, y, z, fluid_block_state(kind, next.amount, next.falling));
    // Vanilla schedules the cell after writing getNewLiquid's result, and
    // returns: the lateral spread belongs to a cell whose computed state *is*
    // its current one. Two things fell out of missing that return: the spread
    // below ran with the stale `here` amount, and — with no neighbour schedule
    // anywhere in this lambda — the cell that had just been recomputed against
    // never learned of the write, so a walled stream's cells could not decay
    // toward empty when their source was cut.
    if (level.schedule_tick)
      level.schedule_tick(x, y, z, fluid_tick_delay(kind));
    for (int i = 0; i < 4; ++i) {
      if (sides[i].fluid.kind == kind)
        level.schedule_tick(x + dx[i], y, z + dz[i], fluid_tick_delay(kind));
    }
    return;
  } else if (!here.source() && next.empty() && !fed_from_above(above.fluid, kind)) {
    // The floor-resting retract: the cell below is stone (closed), so the
    // `below_open` branch above is unreachable here, and without this arm an
    // unsupplied flowing cell whose feed was cut — a walled-in stream — had no
    // path to air at all and sat there forever. Vanilla sets air whenever
    // getNewLiquid comes back empty, whatever stands below; the
    // fed-from-above guard keeps a fed column's cell.
    retract_to_air(level, kind, x, y, z, below, above, sides);
    return;
  }

  const int side_amount = (here.falling ? 7 : here.amount) - fluid_drop_off(kind);
  if (side_amount <= 0) {
    if (changed && !next.empty() && level.schedule_tick)
      level.schedule_tick(x, y, z, fluid_tick_delay(kind));
    return;
  }
  // Vanilla's slope search keeps only directions at the lowest distance. In a
  // shape-less world every open side has equal distance, so all equally open
  // sides are selected without recursive scans.
  for (int i = 0; i < 4; ++i) {
    if (!can_replace(level, sides[i].key, sides[i].fluid, kind))
      continue;
    // The lateral spread advances into *empty* cells only. A same-fluid target
    // is levelled by the computed write above (supply-driven), never from
    // here: back-pumping a neighbour that then re-pumps you is the two-cell
    // oscillation that kept a settled channel broadcasting hundreds of times
    // instead of going quiet.
    if (!sides[i].fluid.empty())
      continue;
    if (spread_to(level, kind, x + dx[i], y, z + dz[i], sides[i], side_amount, false))
      changed = true;
  }
  if (changed && !next.empty() && level.schedule_tick)
    level.schedule_tick(x, y, z, fluid_tick_delay(kind));
}

int64_t FluidTickQueue::chunk_key(int x, int z) {
  const int64_t cx = floor_div16(x), cz = floor_div16(z);
  // Encode signed chunk coordinates through unsigned halves: shifting a
  // negative signed value left is undefined in C++, and this path is exercised
  // by every west/south chunk boundary.
  const uint64_t hi = static_cast<uint32_t>(cx);
  const uint64_t lo = static_cast<uint32_t>(cz);
  return static_cast<int64_t>((hi << 32) | lo);
}

void FluidTickQueue::schedule(int x, int y, int z, FluidKind kind, int delay) {
  if (kind == FluidKind::Empty)
    return;
  const int64_t due = now_ + std::max(1, delay);
  if (auto it = queued_.find(CellKey{x, y, z, kind}); it != queued_.end()) {
    it->second->due = std::min(it->second->due, due); // vanilla retains the earliest tick
    return;
  }
  Bucket& bucket = buckets_[chunk_key(x, z)];
  if (reclaim_.empty()) {
    bucket.push_back(Pending{due, kind, FluidTickPos{x, y, z}, {}});
  } else {
    // A node the last drain freed: splice it over instead of allocating.
    bucket.splice(bucket.end(), reclaim_, std::prev(reclaim_.end()));
    bucket.back() = Pending{due, kind, FluidTickPos{x, y, z}, {}};
  }
  const auto slot = queued_.emplace(CellKey{x, y, z, kind}, std::prev(bucket.end())).first;
  bucket.back().slot = slot;
}

std::vector<std::pair<FluidKind, FluidTickPos>> FluidTickQueue::advance() {
  ++now_;
  std::vector<std::pair<FluidKind, FluidTickPos>> due;
  for (auto it = buckets_.begin(); it != buckets_.end();) {
    Bucket& bucket = it->second;
    auto bit = bucket.begin();
    while (bit != bucket.end()) {
      if (bit->due <= now_) {
        due.emplace_back(bit->kind, bit->pos);
        queued_.erase(bit->slot); // by slot: no re-hash on the emission path
        auto dead = bit++;
        reclaim_.splice(reclaim_.end(), bucket, dead); // freed later, reused now
      } else {
        ++bit;
      }
    }
    if (bucket.empty())
      it = buckets_.erase(it);
    else
      ++it;
  }
  return due;
}

size_t FluidTickQueue::size() const {
  return queued_.size();
}

} // namespace mc::world
