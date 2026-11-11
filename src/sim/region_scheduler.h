// SPDX-License-Identifier: GPL-3.0-or-later
//
// Region-based tick parallelism (docs/PLAN.md §6.2, docs/PHASE5.md ordered
// item 3). This header owns the *policy* — how the world is cut into tick
// regions, which lane a region runs on, and where a region's randomness comes
// from — and nothing about what a region actually ticks. The caller (the
// server's tick path) supplies the body.
//
// The contract the rest of the engine can rely on:
//
//   * **Regions are owned for the whole of one tick.** A region is a square
//     block of chunks (`RegionGrid`); a unit of work belongs to the region that
//     owns the chunk it touches. Two lanes never work on the same region in the
//     same tick.
//
//   * **A region's own sequence does not depend on the lane count.** Region `i`
//     of the sorted region set runs on lane `i % lanes`, and a region's
//     randomness seeds from `(region, tick)` rather than from a thread's own
//     RNG state. So the *order* of a region's work, and every draw it makes,
//     are the same for one lane and for eight — which is what makes an A/B
//     between `PLUSMC_TICK_THREADS` values a measurement of the same
//     simulation.
//
//   * **Cross-region effects are deferred and applied in a canonical order.**
//     A region may only schedule work (a block tick, a fluid tick) into the
//     next tick's queue, and the queue is drained in `(region, position)` order;
//     a result that has to cross a boundary is reported out and applied by the
//     serial merge, never by whichever lane finished first.
//
// What is explicitly *not* promised is bit-exactness against a run with a
// different `chunks_per_region`: regrouping the world moves which lane does
// which chunk, and a write that crosses a region boundary inside one tick lands
// under the world's own lock, so whether the neighbouring region sees it before
// or after its own sample is timing-dependent. That residue is bounded by the
// region size and is recorded in docs/PHASE5.md §5. See `RegionScheduler::run`.
#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace mc::sim {

// A square block of `chunks_per_region` x `chunks_per_region` chunks: the unit
// of tick ownership. The name mirrors vanilla's region files, which are the
// same idea one layer down (32x32 chunks on disk); a *tick* region is smaller
// so there are enough of them to keep every lane busy on a normal view
// distance.
struct RegionPos {
  int rx = 0;
  int rz = 0;
};

inline bool operator==(const RegionPos& a, const RegionPos& b) {
  return a.rx == b.rx && a.rz == b.rz;
}
inline bool operator!=(const RegionPos& a, const RegionPos& b) {
  return !(a == b);
}
// The canonical order: by rx, then rz. Every ordered walk in the scheduler (the
// lane assignment, the mailbox drain, the queue sort) uses this.
inline bool operator<(const RegionPos& a, const RegionPos& b) {
  return a.rx != b.rx ? a.rx < b.rx : a.rz < b.rz;
}

// The chunk -> region mapping. `floor(x / chunks_per_region)` on both axes,
// which is the division vanilla's own `SectionPos`/`ChunkPos` packing uses
// (floor, so chunk -1 is region -1 and not region 0).
class RegionGrid {
public:
  explicit RegionGrid(int chunks_per_region);

  int chunks_per_region() const {
    return chunks_per_region_;
  }

  // The region that owns chunk (chunk_x, chunk_z).
  RegionPos region_of(int chunk_x, int chunk_z) const;

  // The inclusive chunk range a region covers. `min_cx` and `min_cz` are the
  // region's own corner, so `region_of(min_cx, min_cz) == region`.
  void chunk_bounds(RegionPos region, int* min_cx, int* min_cz, int* max_cx, int* max_cz) const;

  // Whether this region owns that chunk — the test a dispatch loop makes for
  // every unit of work it drains.
  bool owns_chunk(RegionPos region, int chunk_x, int chunk_z) const;

  // The regions covering `chunks`, sorted and deduplicated. This is the region
  // set of one tick: the union of every player's view square, so a region with
  // no player and no loaded chunk in it is never scheduled.
  std::vector<RegionPos> regions_covering(const std::vector<std::pair<int, int>>& chunks) const;

private:
  int chunks_per_region_;
};

// A deterministic 64-bit seed for one region's tick: splitmix64 over the world
// seed, the region and the tick index, mixed so that neighbouring regions and
// consecutive ticks share no low bits. Two runs of the same build on the same
// tick draw the same numbers — which the engine's own RNGs (`thread_local
// std::mt19937` seeded from `std::random_device`) do not, and that difference
// is the point: a region's work has to be its own for the lane count not to be
// visible in the result.
uint64_t region_tick_seed(uint64_t world_seed, RegionPos region, int64_t tick);

// A small, self-contained splitmix64 stream. One per (lane, region): the caller
// seeds it from `region_tick_seed` before running a region, so the stream a
// region draws from is a function of the region and the tick alone.
class RegionRandom {
public:
  void seed(uint64_t state) {
    state_ = state;
  }
  uint64_t next_u64();
  // `[0, bound)`; a non-positive bound answers 0, matching `Random.nextInt`'s
  // contract as the engine's other RNG helpers spell it.
  int next_int(int bound);
  // `[0, 1)`, as a double and as a float: the two shapes the engine's tick code
  // asks for (`unit_random` and `next_float`).
  double next_double();
  float next_float();

private:
  uint64_t state_ = 0;
};

// A fixed pool of worker lanes. Constructed once (one per process) and kept
// alive across ticks: `run` wakes the lanes, hands each its share of the region
// list, and waits for all of them, so a tick's parallel phase is a barrier and
// not a thread creation.
//
// Lane assignment is `i % lanes` over the *sorted* region list, which is a
// static, balanced split: it is deterministic, it needs no work-stealing queue
// (so no region can be run twice or skipped by a scheduling accident), and it
// keeps a region's own work on one lane for the whole tick. A lane that draws
// more expensive regions than another simply finishes later — the barrier is
// per tick, not per region, so the cost is one straggler per tick, and the
// caller's regions are all the same size class.
class RegionScheduler {
public:
  // `lanes` is clamped to at least 1. One lane runs everything inline on the
  // calling thread and starts no workers at all — the degenerate case is the
  // same code path, so `PLUSMC_TICK_THREADS=1` is a real arm of the A/B rather
  // than a second implementation.
  explicit RegionScheduler(unsigned lanes);
  ~RegionScheduler();

  RegionScheduler(const RegionScheduler&) = delete;
  RegionScheduler& operator=(const RegionScheduler&) = delete;

  unsigned lanes() const {
    return lanes_;
  }

  // Runs `fn(region, lane)` for every region in `regions` (which must be sorted
  // and unique — `RegionGrid::regions_covering` produces that order) and
  // returns when every region is done. Region `i` runs on lane `i % lanes()`;
  // lane `lanes() - 1` is the *calling* thread's, so the tick thread works
  // through the phase instead of sitting in a join, and the process runs
  // `lanes()` chunks of the world's work at once without ever having more
  // threads awake than the caller asked for.
  //
  // An exception thrown by `fn` does not abandon the other lanes: it is caught,
  // every lane is still joined, and the first one is rethrown here. A tick that
  // fails is a bug, and a tick that half-runs is worse, so the caller sees the
  // failure as a failure either way.
  void run(const std::vector<RegionPos>& regions,
           const std::function<void(RegionPos, unsigned)>& fn);

private:
  struct Impl;
  // One lane's worker loop, defined in the .cpp beside `Impl` (a free function
  // could not name the private type).
  static void lane_loop(Impl* impl, unsigned lane, unsigned lanes);

  std::unique_ptr<Impl> impl_;
  unsigned lanes_;
};

} // namespace mc::sim
