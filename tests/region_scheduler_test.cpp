// SPDX-License-Identifier: GPL-3.0-or-later
//
// The region scheduler's contract (src/sim/region_scheduler.h, docs/PHASE5.md
// ordered item 3): the chunk -> region mapping, the deterministic seed streams,
// and the three properties the tick path is allowed to rely on —
//
//   1. **every region runs exactly once**, on lane `i % lanes` of the sorted
//      region list, and the assignment is the same on every run;
//   2. **the lane count is not visible in a region's result**: the same
//      per-region workload draws the same numbers for one lane and for four,
//      which is what makes `PLUSMC_TICK_THREADS` a measurement and not a second
//      simulation;
//   3. **a failure on one lane is a failure of the phase**: every lane is still
//      joined and the caller sees the first exception rather than a half-run
//      tick.
//
// A scheduler that silently dropped a region, ran one twice, or let the lane
// count reach the draws would pass a compile and fail here.
//
// Usage: region_scheduler_test

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "sim/region_scheduler.h"

namespace {

int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

using mc::sim::RegionGrid;
using mc::sim::RegionPos;
using mc::sim::RegionRandom;
using mc::sim::RegionScheduler;

// ---- the mapping ------------------------------------------------------------

void check_grid() {
  const RegionGrid grid(8);
  CHECK(grid.chunks_per_region() == 8);

  // Chunk 0 is region 0 and the region covers 0..7.
  CHECK(grid.region_of(0, 0) == (RegionPos{0, 0}));
  CHECK(grid.region_of(7, 7) == (RegionPos{0, 0}));
  CHECK(grid.region_of(8, 0) == (RegionPos{1, 0}));
  CHECK(grid.region_of(0, 8) == (RegionPos{0, 1}));
  // The floor-division half: chunk -1 belongs to region -1, not region 0. A
  // truncating division would put -1..-8 in region 0 and break the claim that a
  // region owns a contiguous square.
  CHECK(grid.region_of(-1, -1) == (RegionPos{-1, -1}));
  CHECK(grid.region_of(-8, 0) == (RegionPos{-1, 0}));
  CHECK(grid.region_of(-9, 0) == (RegionPos{-2, 0}));

  int min_cx = 0, min_cz = 0, max_cx = 0, max_cz = 0;
  grid.chunk_bounds(RegionPos{-1, 2}, &min_cx, &min_cz, &max_cx, &max_cz);
  CHECK(min_cx == -8);
  CHECK(min_cz == 16);
  CHECK(max_cx == -1);
  CHECK(max_cz == 23);
  // The bounds are self-consistent with the mapping at both corners.
  CHECK(grid.region_of(min_cx, min_cz) == (RegionPos{-1, 2}));
  CHECK(grid.region_of(max_cx, max_cz) == (RegionPos{-1, 2}));
  CHECK(grid.owns_chunk(RegionPos{-1, 2}, -8, 16));
  CHECK(!grid.owns_chunk(RegionPos{-1, 2}, -9, 16));

  // A degenerate size is clamped rather than dividing by zero.
  CHECK(RegionGrid(0).chunks_per_region() == 1);
  // One chunk per region: every chunk is its own region, negatives included.
  CHECK(RegionGrid(1).region_of(-3, 5) == (RegionPos{-3, 5}));

  // `regions_covering` is the tick's region set: sorted, deduplicated, and a
  // chunk list that crosses a boundary yields both regions.
  const std::vector<std::pair<int, int>> chunks{{0, 0}, {7, 7}, {8, 0},
                                                {0, 8}, {8, 8}, {7, 7}, // duplicate chunk
                                                {0, 0}};
  const std::vector<RegionPos> regions = grid.regions_covering(chunks);
  CHECK(regions.size() == 4);
  CHECK(regions[0] == (RegionPos{0, 0}));
  CHECK(regions[1] == (RegionPos{0, 1}));
  CHECK(regions[2] == (RegionPos{1, 0}));
  CHECK(regions[3] == (RegionPos{1, 1}));
  for (size_t i = 1; i < regions.size(); ++i)
    CHECK(regions[i - 1] < regions[i]);
  CHECK(grid.regions_covering({}).empty());
}

// ---- the streams ------------------------------------------------------------

void check_seeds_and_streams() {
  // The same (region, tick) is the same seed; a different region or tick is a
  // different one, including for neighbours that differ by one.
  const uint64_t a = mc::sim::region_tick_seed(0x1234, RegionPos{3, -2}, 77);
  CHECK(a == mc::sim::region_tick_seed(0x1234, RegionPos{3, -2}, 77));
  CHECK(a != mc::sim::region_tick_seed(0x1234, RegionPos{3, -1}, 77));
  CHECK(a != mc::sim::region_tick_seed(0x1234, RegionPos{4, -2}, 77));
  CHECK(a != mc::sim::region_tick_seed(0x1234, RegionPos{3, -2}, 78));
  CHECK(a != mc::sim::region_tick_seed(0x9999, RegionPos{3, -2}, 77));
  // And the salt the caller passes is honoured: two passes for one region and
  // tick must not share a stream.
  CHECK(mc::sim::region_tick_seed(1, RegionPos{0, 0}, 1) !=
        mc::sim::region_tick_seed(2, RegionPos{0, 0}, 1));

  // Stream reproducibility and range.
  RegionRandom one, two;
  one.seed(mc::sim::region_tick_seed(0, RegionPos{5, 5}, 9));
  two.seed(mc::sim::region_tick_seed(0, RegionPos{5, 5}, 9));
  // Both streams are advanced through the same calls in the same order, so
  // "the same seed draws the same numbers" is what the pairs compare.
  bool positive_int = false;
  for (int i = 0; i < 256; ++i) {
    CHECK(one.next_u64() == two.next_u64());
    const int bound = one.next_int(17);
    CHECK(bound == two.next_int(17));
    CHECK(bound >= 0 && bound < 17);
    if (bound > 0)
      positive_int = true;
    const double d = one.next_double();
    CHECK(d == two.next_double());
    CHECK(d >= 0.0 && d < 1.0);
    const float f = one.next_float();
    CHECK(f == two.next_float());
    CHECK(f >= 0.0f && f < 1.0f);
  }
  CHECK(positive_int);
  // A non-positive bound is 0 rather than a division.
  CHECK(one.next_int(0) == 0);
  CHECK(one.next_int(-5) == 0);

  // Two regions' streams diverge: the first draw differs for at least one of a
  // handful of neighbours (a mixer that folded region keys together would fail
  // this).
  RegionRandom left, right;
  int differing = 0;
  for (int rx = 0; rx < 4; ++rx) {
    left.seed(mc::sim::region_tick_seed(0, RegionPos{rx, 0}, 1));
    right.seed(mc::sim::region_tick_seed(0, RegionPos{rx + 1, 0}, 1));
    if (left.next_u64() != right.next_u64())
      ++differing;
  }
  CHECK(differing == 4);
}

// ---- the phase --------------------------------------------------------------

// The per-region workload both lane-count tests run: eight draws from the
// region's own stream, summed. It is deliberately a pure function of
// (region, tick), which is exactly what the tick path's seeds make its passes.
int region_work(RegionPos region, int64_t tick) {
  RegionRandom random;
  random.seed(mc::sim::region_tick_seed(0x5EED, region, tick));
  int value = 0;
  for (int i = 0; i < 8; ++i)
    value += random.next_int(1000);
  return value;
}

std::vector<RegionPos> sample_regions(size_t count) {
  // Deliberately not a rectangle and not zero-based on both axes: a scheduler
  // that assumed either would still pass a rectangular test.
  std::vector<RegionPos> regions;
  for (size_t i = 0; i < count; ++i)
    regions.push_back(RegionPos{static_cast<int>(i % 3) - 1, static_cast<int>(i / 3) - 2});
  return regions;
}

void check_every_region_once_on_its_lane() {
  const std::vector<RegionPos> regions = sample_regions(12);
  for (unsigned lanes : {1u, 2u, 3u, 4u}) {
    RegionScheduler scheduler(lanes);
    std::map<RegionPos, unsigned> lane_of;
    std::map<RegionPos, int> runs;
    std::mutex mu;
    std::atomic<int> concurrent{0};
    std::atomic<int> peak{0};
    std::set<std::thread::id> threads;
    std::mutex threads_mu;
    scheduler.run(regions, [&](RegionPos region, unsigned lane) {
      {
        std::lock_guard<std::mutex> lock(mu);
        ++runs[region];
        lane_of.emplace(region, lane);
      }
      {
        std::lock_guard<std::mutex> lock(threads_mu);
        threads.insert(std::this_thread::get_id());
      }
      const int now = ++concurrent;
      int seen = peak.load();
      while (now > seen && !peak.compare_exchange_weak(seen, now)) {
      }
      // Long enough that two lanes on a multi-core box overlap. Windows can
      // take milliseconds to wake a worker parked on the condition variable,
      // so 200 us let the caller's lane finish before a worker's first
      // callback (the Windows runner measured a peak of 1); 10 ms is longer
      // than that wake and still keeps the whole test under a second. The peak
      // is only asserted for the lanes > 1 case, and only as "at least two",
      // so a single-core box cannot make this flaky.
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      --concurrent;
    });
    CHECK(runs.size() == regions.size());
    for (const RegionPos& region : regions) {
      CHECK(runs[region] == 1);
      // The static split: region i of the *sorted* list runs on lane i % lanes.
      const auto found = std::find(regions.begin(), regions.end(), region);
      const size_t index = static_cast<size_t>(found - regions.begin());
      CHECK(lane_of[region] == index % lanes);
    }
    // Lane `lanes - 1` is the calling thread's, and the other lanes are distinct
    // worker threads, so with region 0 on lane 0 and lanes > 1 there is at least
    // one region on another thread.
    if (lanes == 1)
      CHECK(threads.size() == 1);
    else
      CHECK(threads.size() >= 2);
    if (lanes > 1)
      CHECK(peak.load() >= 2);
  }
}

void check_lane_count_is_invisible() {
  const std::vector<RegionPos> regions = sample_regions(15);
  std::map<RegionPos, int> reference;
  for (const RegionPos& region : regions)
    reference[region] = region_work(region, 41);

  for (unsigned lanes : {1u, 2u, 3u, 4u, 8u}) {
    RegionScheduler scheduler(lanes);
    std::map<RegionPos, int> results;
    std::mutex mu;
    scheduler.run(regions, [&](RegionPos region, unsigned /*lane*/) {
      const int value = region_work(region, 41);
      std::lock_guard<std::mutex> lock(mu);
      results[region] = value;
    });
    CHECK(results.size() == regions.size());
    CHECK(results == reference);
  }
  // Sanity: the workload really is region-specific, or the comparison above
  // would be vacuous.
  CHECK(reference.size() > 1);
  CHECK(reference.begin()->second != std::next(reference.begin())->second);
}

void check_empty_and_exception() {
  {
    RegionScheduler scheduler(3);
    std::atomic<int> calls{0};
    scheduler.run({}, [&](RegionPos, unsigned) { ++calls; });
    CHECK(calls.load() == 0);
  }
  {
    // A throw on one region must not skip the others, and must surface.
    const std::vector<RegionPos> regions = sample_regions(9);
    RegionScheduler scheduler(4);
    std::atomic<int> ran{0};
    bool threw = false;
    std::string what;
    try {
      scheduler.run(regions, [&](RegionPos region, unsigned) {
        ++ran;
        if (region.rx == 0 && region.rz == 0)
          throw std::runtime_error("region " + std::to_string(region.rz) + " failed");
      });
    } catch (const std::exception& e) {
      threw = true;
      what = e.what();
    }
    CHECK(threw);
    CHECK(what.find("failed") != std::string::npos);
    CHECK(ran.load() == static_cast<int>(regions.size()));
    // The pool is still usable after a failed phase: the next run is clean.
    std::atomic<int> again{0};
    scheduler.run(regions, [&](RegionPos, unsigned) { ++again; });
    CHECK(again.load() == static_cast<int>(regions.size()));
  }
  {
    // A one-lane scheduler runs inline on this thread, which is the case the
    // A/B's control arm uses.
    RegionScheduler scheduler(1);
    CHECK(scheduler.lanes() == 1);
    const std::thread::id here = std::this_thread::get_id();
    std::vector<std::thread::id> seen;
    scheduler.run(sample_regions(5), [&](RegionPos, unsigned lane) {
      CHECK(lane == 0);
      seen.push_back(std::this_thread::get_id());
    });
    CHECK(seen.size() == 5);
    for (const std::thread::id& id : seen)
      CHECK(id == here);
  }
}

} // namespace

int main() {
  std::printf("region_scheduler_test\n");
  check_grid();
  check_seeds_and_streams();
  check_every_region_once_on_its_lane();
  check_lane_count_is_invisible();
  check_empty_and_exception();
  if (failures != 0) {
    std::fprintf(stderr, "region_scheduler_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("region_scheduler_test: all checks passed\n");
  return 0;
}
