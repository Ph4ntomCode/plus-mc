// SPDX-License-Identifier: GPL-3.0-or-later
//
// The generation pool's contract (src/world/chunk_generation.h, the
// ahead-of-view streaming the join storm needs):
//
//   1. **a request is the column plus its 3x3 ring**, and `ring_stored` only
//      answers true once all nine are in the store;
//   2. **duplicate asks cost nothing**: a column already queued or in flight is
//      not queued twice (two workers must never build the same column), and one
//      already stored is counted as such and not queued at all;
//   3. **a full queue drops instead of blocking** — the cost of a dropped ask is
//      one inline build by the caller, the cost of a blocking ask is the stall
//      the pool exists to remove;
//   4. **shutdown joins without draining**: the destructor lets the columns a
//      worker is already inside finish, discards whatever was still queued, and
//      returns — a stop costs one column's time, not the backlog's;
//   5. **discarding keeps in-flight work known**: an idle-server discard clears
//      the queue, but a column a worker is already building stays deduped, so a
//      join in that window cannot start a second build of it.
//
// The world is a fake store rather than `MemoryWorld`: this test is about the
// pool's bookkeeping, and the store's own build cost belongs to the worldgen
// tests. The fake's `ensure_chunk_stored` takes a configurable delay (so a
// nine-job queue is still there when the next assertion looks at it) and keeps a
// per-column build count, which is how "never built twice" is checked.
//
// Usage: chunk_generation_test

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "world/chunk_generation.h"
#include "world/world_interface.h"

namespace {

int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

using mc::world::Chunk;
using mc::world::ChunkGeneration;
using mc::world::ChunkLease;
using mc::world::WorldInterface;

class FakeWorld : public WorldInterface {
public:
  explicit FakeWorld(int delay_ms) : delay_ms_(delay_ms) {}

  // Builds a worker has entered (before the delay), which is how the shutdown
  // test puts one *in flight* instead of racing the destructor against a wake.
  std::atomic<int> started{0};
  // A test can hold the first build at its start, then exercise the pool's
  // bookkeeping while exactly one column is in flight. Released explicitly so
  // the duplicate-build window is not a race.
  std::atomic<bool> hold{false};
  std::atomic<bool> release{false};

  std::optional<Chunk> load_chunk(int /*cx*/, int /*cz*/) const override {
    return std::nullopt;
  }
  std::optional<ChunkLease> modify_chunk(int /*cx*/, int /*cz*/) override {
    return std::nullopt;
  }
  int min_section_y() const override {
    return -4;
  }
  int section_count() const override {
    return 24;
  }

  void ensure_chunk_stored(int cx, int cz) override {
    started.fetch_add(1, std::memory_order_relaxed);
    while (hold.load(std::memory_order_relaxed) && !release.load(std::memory_order_relaxed))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (delay_ms_ > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
    std::lock_guard<std::mutex> lock(mu_);
    stored_.insert(key(cx, cz));
    ++builds_[key(cx, cz)];
  }

  bool has_stored_column(int cx, int cz) const override {
    std::lock_guard<std::mutex> lock(mu_);
    return stored_.count(key(cx, cz)) != 0;
  }

  int builds() const {
    std::lock_guard<std::mutex> lock(mu_);
    int total = 0;
    for (const auto& [k, n] : builds_)
      total += n;
    return total;
  }
  int builds_for(int cx, int cz) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = builds_.find(key(cx, cz));
    return it == builds_.end() ? 0 : it->second;
  }
  std::size_t resident_columns() const override {
    std::lock_guard<std::mutex> lock(mu_);
    return stored_.size();
  }

private:
  static int64_t key(int cx, int cz) {
    return (static_cast<int64_t>(cx) << 32) ^ static_cast<uint32_t>(cz);
  }

  int delay_ms_ = 0;
  mutable std::mutex mu_;
  std::unordered_set<int64_t> stored_;
  std::unordered_map<int64_t, int> builds_;
};

bool wait_until(const std::function<bool()>& done, int budget_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (done())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return done();
}

// ---- 1. a request is the column and its ring ---------------------------------

void check_request_builds_the_ring() {
  FakeWorld world(5);
  ChunkGeneration::Options options;
  options.threads = 2;
  ChunkGeneration pool(world, options);

  CHECK(!pool.stored(0, 0));
  CHECK(!pool.ring_stored(0, 0));
  pool.request(0, 0);

  const ChunkGeneration::Stats after_request = pool.stats();
  CHECK(after_request.requested == 9); // the centre plus eight neighbours
  CHECK(after_request.generated == 0);

  CHECK(wait_until([&] { return pool.ring_stored(0, 0); }, 5000));
  CHECK(pool.stored(0, 0));
  // `ring_stored` is the store's answer; the pool counts a build after the
  // store call returns, so wait for the counter to catch up rather than read a
  // number that raced the last worker.
  CHECK(wait_until([&] { return pool.stats().generated == 9; }, 5000));
  const ChunkGeneration::Stats done = pool.stats();
  CHECK(done.generated == 9);
  CHECK(done.already == 0);
  CHECK(done.dropped == 0);
  CHECK(done.failed == 0);
  CHECK(world.builds() == 9);
  CHECK(world.resident_columns() == 9);
  for (int dx = -1; dx <= 1; ++dx)
    for (int dz = -1; dz <= 1; ++dz)
      CHECK(world.builds_for(dx, dz) == 1);
}

// ---- 2. duplicate asks ------------------------------------------------------

void check_duplicate_asks_are_cheap() {
  // In flight: the second ask finds the nine columns in `known_` and adds no
  // work, which is the property that keeps two workers off one column.
  {
    FakeWorld world(60);
    ChunkGeneration::Options options;
    options.threads = 1;
    ChunkGeneration pool(world, options);
    pool.request(40, 40);
    pool.request(40, 40);
    const ChunkGeneration::Stats queued = pool.stats();
    CHECK(queued.requested == 9);
    CHECK(queued.dropped == 0);
    // A worker may already have finished the centre column between the two
    // asks (the first one is 60 ms); that ask is then "already stored", not a
    // duplicate queued. Either way there are nine jobs, never ten.
    CHECK(queued.already <= 1);
    CHECK(wait_until([&] { return pool.stats().generated == 9; }, 5000));
    CHECK(world.builds() == 9);
    for (int dx = -1; dx <= 1; ++dx)
      for (int dz = -1; dz <= 1; ++dz)
        CHECK(world.builds_for(40 + dx, 40 + dz) == 1);
  }
  // Already stored: the ask is answered by the store, counted, and never queued.
  {
    FakeWorld world(0);
    ChunkGeneration pool(world);
    pool.request(7, 7);
    CHECK(wait_until([&] { return pool.ring_stored(7, 7) && pool.stats().generated == 9; }, 5000));
    const ChunkGeneration::Stats before = pool.stats();
    pool.request(7, 7);
    const ChunkGeneration::Stats after = pool.stats();
    CHECK(after.already == before.already + 9);
    CHECK(after.requested == before.requested);
    CHECK(after.generated == before.generated);
    CHECK(after.dropped == before.dropped);
  }
  // A neighbour of a stored ring: six of (1,0)'s nine are in the store already
  // (its ring overlaps (0,0)'s in two columns of three), so the ask is three
  // jobs and six "already stored".
  {
    FakeWorld world(0);
    ChunkGeneration pool(world);
    pool.request(0, 0);
    CHECK(wait_until([&] { return pool.ring_stored(0, 0) && pool.stats().generated == 9; }, 5000));
    const ChunkGeneration::Stats before = pool.stats();
    pool.request(1, 0);
    const ChunkGeneration::Stats after = pool.stats();
    CHECK(after.already == before.already + 6);
    CHECK(after.requested == before.requested + 3);
    CHECK(wait_until([&] { return pool.ring_stored(1, 0); }, 5000));
  }
}

// ---- 3. a full queue drops instead of blocking ------------------------------

void check_full_queue_drops() {
  FakeWorld world(80);
  ChunkGeneration::Options options;
  options.threads = 1;
  options.capacity = 2;
  ChunkGeneration pool(world, options);
  pool.request(200, 200);
  const ChunkGeneration::Stats stats = pool.stats();
  // Every one of the nine asks is either queued/accepted or dropped; the
  // capacity is the ceiling on what is waiting (one may already be in flight).
  CHECK(stats.requested + stats.dropped == 9);
  CHECK(stats.requested <= 4);
  CHECK(stats.dropped >= 5);
  CHECK(wait_until([&] { return pool.pending() == 0 && pool.stats().generated == stats.requested; },
                   5000));
  CHECK(pool.stats().generated == stats.requested);
  CHECK(static_cast<uint64_t>(world.builds()) == stats.requested);
}

// ---- 4. discard keeps an in-flight column known ---------------------------

void check_discard_keeps_in_flight_known() {
  FakeWorld world(0);
  world.hold = true;
  ChunkGeneration::Options options;
  options.threads = 1;
  ChunkGeneration pool(world, options);
  pool.request(700, 700);
  CHECK(wait_until([&] { return world.started.load() >= 1; }, 2000));

  // The one worker is inside (700, 700); the other eight jobs are queued. A
  // discard gives the queue back, but the in-flight column must stay known.
  pool.discard_pending();
  pool.request(700, 700);
  CHECK(pool.pending() == 8);
  CHECK(pool.stats().requested == 17); // 9 first asks + the 8 requeued columns

  world.release = true;
  CHECK(wait_until([&] { return pool.ring_stored(700, 700); }, 5000));
  CHECK(world.builds() == 9);
  CHECK(world.builds_for(700, 700) == 1);
}

// ---- 5. shutdown joins -----------------------------------------------------

void check_shutdown_joins() {
  FakeWorld world(200);
  const auto started = std::chrono::steady_clock::now();
  {
    ChunkGeneration::Options options;
    options.threads = 2;
    ChunkGeneration pool(world, options);
    pool.request(300, 300);
    CHECK(pool.stats().requested == 9);
    // Nine jobs of 200 ms each: draining would take 900 ms of the two workers'
    // time. The destructor must finish the (at most two) columns already in
    // flight and drop the rest — a server's stop may not be a backlog's length.
    CHECK(wait_until([&] { return world.started.load() >= 1; }, 2000));
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  CHECK(ms < 900);
  CHECK(world.builds() >= 1);
  CHECK(world.builds() <= 9);
}

// ---- 6. the shutdown line --------------------------------------------------

void check_report() {
  FakeWorld world(0);
  ChunkGeneration::Options options;
  options.threads = 3;
  ChunkGeneration pool(world, options);
  pool.request(500, 500);
  CHECK(
      wait_until([&] { return pool.ring_stored(500, 500) && pool.stats().generated == 9; }, 5000));
  pool.request(500, 500);
  const std::string line = pool.report();
  CHECK(line.find("chunk generation: 9 columns generated by 3 thread(s)") != std::string::npos);
  CHECK(line.find("9 requested") != std::string::npos);
  CHECK(line.find("9 already stored") != std::string::npos);
  CHECK(line.find("0 dropped") != std::string::npos);
  CHECK(line.find("0 failed") != std::string::npos);
  CHECK(line.find("0 queued") != std::string::npos);
}

// ---- 7. the thread count clamps --------------------------------------------

void check_zero_threads_clamps() {
  FakeWorld world(0);
  ChunkGeneration::Options options;
  options.threads = 0;
  ChunkGeneration pool(world, options);
  CHECK(pool.thread_count() == 1);
  pool.request(600, 600);
  CHECK(
      wait_until([&] { return pool.ring_stored(600, 600) && pool.stats().generated == 9; }, 5000));
  CHECK(pool.stats().generated == 9);
}

} // namespace

int main() {
  check_request_builds_the_ring();
  check_duplicate_asks_are_cheap();
  check_full_queue_drops();
  check_discard_keeps_in_flight_known();
  check_shutdown_joins();
  check_report();
  check_zero_threads_clamps();
  if (failures != 0) {
    std::fprintf(stderr, "%d chunk-generation check(s) failed\n", failures);
    return 1;
  }
  std::printf("chunk_generation: all checks passed\n");
  return 0;
}
