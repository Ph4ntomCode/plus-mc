// SPDX-License-Identifier: GPL-3.0-or-later
//
// The region scheduler's implementation: the floor-division chunk -> region
// mapping, the splitmix64 seed derivation, and a barrier pool whose lane
// assignment is a static, deterministic split of the sorted region list.
#include "sim/region_scheduler.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace mc::sim {

namespace {

// `Math.floorDiv`: C++ integer division truncates toward zero, so a negative
// chunk or region would land one to the right of vanilla's own mapping (chunk
// -1 belongs to region -1, not region 0).
int floor_div(int value, int divisor) {
  return value >= 0 ? value / divisor : -(((-value) + divisor - 1) / divisor);
}

// splitmix64: the mixer, and the stream `RegionRandom` runs. It is not a
// cryptographic generator and does not need to be — it is here because it is
// short, has no state beyond one word, is trivially reproducible, and mixes
// neighbouring seeds (region keys one apart) well enough that two lanes never
// draw the same sequence.
uint64_t splitmix64(uint64_t& state) {
  state += 0x9E3779B97F4A7C15ULL;
  uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

} // namespace

// ---------------------------------------------------------------------------
// RegionGrid
// ---------------------------------------------------------------------------

RegionGrid::RegionGrid(int chunks_per_region)
    : chunks_per_region_(chunks_per_region > 0 ? chunks_per_region : 1) {}

RegionPos RegionGrid::region_of(int chunk_x, int chunk_z) const {
  return RegionPos{floor_div(chunk_x, chunks_per_region_), floor_div(chunk_z, chunks_per_region_)};
}

void RegionGrid::chunk_bounds(RegionPos region, int* min_cx, int* min_cz, int* max_cx,
                              int* max_cz) const {
  const int min_x = region.rx * chunks_per_region_;
  const int min_z = region.rz * chunks_per_region_;
  if (min_cx != nullptr)
    *min_cx = min_x;
  if (min_cz != nullptr)
    *min_cz = min_z;
  if (max_cx != nullptr)
    *max_cx = min_x + chunks_per_region_ - 1;
  if (max_cz != nullptr)
    *max_cz = min_z + chunks_per_region_ - 1;
}

bool RegionGrid::owns_chunk(RegionPos region, int chunk_x, int chunk_z) const {
  return region_of(chunk_x, chunk_z) == region;
}

std::vector<RegionPos>
RegionGrid::regions_covering(const std::vector<std::pair<int, int>>& chunks) const {
  std::vector<RegionPos> regions;
  regions.reserve(chunks.size());
  for (const auto& [cx, cz] : chunks)
    regions.push_back(region_of(cx, cz));
  // Sorted and deduplicated, which is the order every consumer of this list
  // needs: `RegionScheduler::run`'s lane assignment is `i % lanes`, and the
  // merge walks the same list, so "the tick's region order" is one order
  // everywhere rather than one per call site.
  std::sort(regions.begin(), regions.end());
  regions.erase(std::unique(regions.begin(), regions.end()), regions.end());
  return regions;
}

// ---------------------------------------------------------------------------
// seeds and the per-region stream
// ---------------------------------------------------------------------------

uint64_t region_tick_seed(uint64_t world_seed, RegionPos region, int64_t tick) {
  // Every input is folded in through its own odd multiplier before the mixer
  // runs, so (region 1, tick 2) and (region 2, tick 1) cannot cancel, and the
  // tick's high bits -- which barely move between two ticks of one run -- do
  // not leave the low bits of the seed almost constant.
  uint64_t state = world_seed ^ 0x51ED270B5B3A45F1ULL;
  state ^= static_cast<uint64_t>(static_cast<uint32_t>(region.rx)) * 0x9E3779B97F4A7C15ULL;
  state ^= static_cast<uint64_t>(static_cast<uint32_t>(region.rz)) * 0xC2B2AE3D27D4EB4FULL;
  state ^= static_cast<uint64_t>(tick) * 0xD6E8FEB86659FD93ULL;
  return splitmix64(state);
}

uint64_t RegionRandom::next_u64() {
  return splitmix64(state_);
}

int RegionRandom::next_int(int bound) {
  if (bound <= 0)
    return 0;
  // The modulo bias is irrelevant here and would cost a division loop: the
  // engine's own helpers use `uniform_int_distribution` over 2^32 states, and
  // nothing in the tick path depends on the exact distribution at the top end.
  return static_cast<int>(next_u64() % static_cast<uint64_t>(bound));
}

double RegionRandom::next_double() {
  // 53 significant bits in [0, 1), the same shape as the engine's other
  // `unit_random` producers.
  return static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0);
}

float RegionRandom::next_float() {
  return static_cast<float>(static_cast<double>(next_u64() >> 40) * (1.0 / 16777216.0));
}

// ---------------------------------------------------------------------------
// RegionScheduler
// ---------------------------------------------------------------------------

struct RegionScheduler::Impl {
  std::mutex mu;
  std::condition_variable work_cv; // run() -> lanes
  std::condition_variable done_cv; // lanes -> run()

  std::vector<std::thread> workers;

  // The phase in flight. `regions` and `body` point at the caller's objects,
  // which outlive the phase: `run` does not return until `pending` lanes have
  // finished with them.
  const std::vector<RegionPos>* regions = nullptr;
  const std::function<void(RegionPos, unsigned)>* body = nullptr;
  uint64_t generation = 0;
  unsigned pending = 0;
  bool shutdown = false;
  std::exception_ptr error;
};

// One lane's worker loop. `seen` is the generation this lane last ran, so the
// condition variable's predicate is per-thread and a lane that is slower than
// its siblings cannot miss a wakeup.
void RegionScheduler::lane_loop(Impl* impl, unsigned lane, unsigned lanes) {
  uint64_t seen = 0;
  for (;;) {
    const std::vector<RegionPos>* regions = nullptr;
    const std::function<void(RegionPos, unsigned)>* body = nullptr;
    {
      std::unique_lock<std::mutex> lock(impl->mu);
      impl->work_cv.wait(lock, [&] { return impl->shutdown || impl->generation != seen; });
      if (impl->shutdown)
        return;
      seen = impl->generation;
      regions = impl->regions;
      body = impl->body;
    }
    try {
      for (size_t i = lane; regions != nullptr && body != nullptr && i < regions->size();
           i += lanes)
        (*body)((*regions)[i], lane);
    } catch (...) {
      // Held outside the lane: the region loop must finish on every lane before
      // the failure surfaces, or the next phase would run over half a tick.
      std::lock_guard<std::mutex> lock(impl->mu);
      if (impl->error == nullptr)
        impl->error = std::current_exception();
    }
    {
      std::lock_guard<std::mutex> lock(impl->mu);
      if (--impl->pending == 0)
        impl->done_cv.notify_all();
    }
  }
}

RegionScheduler::RegionScheduler(unsigned lanes) : impl_(new Impl), lanes_(lanes > 0 ? lanes : 1) {
  if (lanes_ <= 1)
    return; // one lane runs inline on the caller; no workers exist
  impl_->workers.reserve(lanes_ - 1);
  for (unsigned lane = 0; lane + 1 < lanes_; ++lane)
    impl_->workers.emplace_back([this, lane] { lane_loop(impl_.get(), lane, lanes_); });
}

RegionScheduler::~RegionScheduler() {
  if (lanes_ <= 1)
    return;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->shutdown = true;
  }
  impl_->work_cv.notify_all();
  for (std::thread& worker : impl_->workers)
    if (worker.joinable())
      worker.join();
}

void RegionScheduler::run(const std::vector<RegionPos>& regions,
                          const std::function<void(RegionPos, unsigned)>& fn) {
  if (regions.empty())
    return;
  if (lanes_ <= 1) {
    for (const RegionPos& region : regions)
      fn(region, 0);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->regions = &regions;
    impl_->body = &fn;
    impl_->error = nullptr;
    // The workers take lanes 0..lanes-2; the caller takes the last one below.
    impl_->pending = lanes_ - 1;
    ++impl_->generation;
  }
  impl_->work_cv.notify_all();

  // The calling thread is the last lane, so a phase is `lanes_` threads of work
  // with `lanes_ - 1` created for it -- the same count as a build that spawned
  // one thread per lane, without the spawn.
  try {
    for (size_t i = lanes_ - 1; i < regions.size(); i += lanes_)
      fn(regions[i], lanes_ - 1);
  } catch (...) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->error == nullptr)
      impl_->error = std::current_exception();
  }

  std::unique_lock<std::mutex> lock(impl_->mu);
  impl_->done_cv.wait(lock, [&] { return impl_->pending == 0; });
  impl_->regions = nullptr;
  impl_->body = nullptr;
  if (impl_->error != nullptr) {
    std::exception_ptr error = impl_->error;
    impl_->error = nullptr;
    lock.unlock();
    std::rethrow_exception(error);
  }
}

} // namespace mc::sim
