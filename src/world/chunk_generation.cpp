// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pool behind `world/chunk_generation.h`. Two things are worth saying here
// that the header does not: why the work is a coordinate and not a `Chunk`, and
// why a worker calls the store instead of generating into a buffer of its own.
//
// The coordinate. A generated column is a pure function of (seed, cx, cz) —
// `fill_generated_carved_chunk` (the stage `--worldgen` serves) takes a const
// generator and writes a fresh `Chunk` — so a worker does not need to hand its
// result to anybody: it stores
// it and the world hands it out later, through the same `load_chunk`/borrow path
// every other reader uses. That also means the pool holds no chunk memory at
// all: the store's own budget and eviction policy stay the only account of what
// a generated world costs, and a column this pool built that nobody ever reads
// is evicted exactly like any other unmodified column.
//
// The store call. `ensure_chunk_stored` is *the* "build it if it is not there,
// and hand nothing out" entry point (world/world_interface.h), and on
// `MemoryWorld` it already builds outside the store's mutex and publishes under
// it (`store_or_find_locked`), which is the property that made this pool
// possible at all: N workers generate in parallel with nothing serialising them
// but the publish.
#include "world/chunk_generation.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace mc::world {

namespace {

int64_t column_key(int cx, int cz) {
  return (static_cast<int64_t>(cx) << 32) ^ static_cast<uint32_t>(cz);
}

} // namespace

ChunkGeneration::ChunkGeneration(WorldInterface& world, Options options)
    : world_(world), options_(options) {
  if (options_.threads < 1)
    options_.threads = 1;
  threads_.reserve(static_cast<std::size_t>(options_.threads));
  for (int i = 0; i < options_.threads; ++i)
    threads_.emplace_back([this] { run(); });
}

ChunkGeneration::~ChunkGeneration() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
    // Accepted-but-not-started is not a promise. Generation is a pure function
    // of the coordinate and nothing downstream is waiting for these columns at
    // shutdown, so draining the queue would turn a stop into tens of seconds of
    // generating columns nobody will ever read — a join storm leaves hundreds
    // queued, and a bounded test's 45 s cell would sit in its own teardown.
    // In-flight jobs do finish: a worker is inside the store and cannot be
    // interrupted there safely, but that is one column, not the backlog.
    queue_.clear();
  }
  cv_.notify_all();
  for (std::thread& thread : threads_)
    if (thread.joinable())
      thread.join();
}

void ChunkGeneration::request(int cx, int cz) {
  // The centre first: a caller that wants one column sent wants its own build
  // before its neighbours' (the ring is prefetch, and a queue with room for one
  // of the nine should be the one that matters).
  request_one(cx, cz);
  for (int dx = -1; dx <= 1; ++dx)
    for (int dz = -1; dz <= 1; ++dz)
      if (dx != 0 || dz != 0)
        request_one(cx + dx, cz + dz);
}

void ChunkGeneration::request_one(int cx, int cz) {
  // Already built: not a job. The lookup is the store's own map read (the same
  // one `stored()` does), and counting it is what makes the shutdown line's
  // "already stored" a real answer rather than a field nothing increments.
  if (world_.has_stored_column(cx, cz)) {
    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.already;
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stop_)
      return;
    if (queue_.size() >= options_.capacity) {
      ++stats_.dropped;
      return;
    }
    if (!known_.insert(column_key(cx, cz)).second)
      return; // queued or being built already
    queue_.emplace_back(cx, cz);
    // Accepted *here*, not at the dequeue: `requested` is the caller's ask, and
    // the queue's backlog is then visible as requested - generated while the
    // pool is behind.
    ++stats_.requested;
  }
  cv_.notify_one();
}

void ChunkGeneration::discard_pending() {
  std::lock_guard<std::mutex> lock(mu_);
  // Only the queued keys leave `known_`. A worker that has already popped its
  // job is no longer in `queue_` but is still building that column; dropping its
  // key would let the next request queue the same coordinate again, undoing the
  // one-build-per-column guarantee this class exists for. The worker erases its
  // own key when the build finishes, so the set stays exact.
  for (const auto& job : queue_)
    known_.erase(column_key(job.first, job.second));
  queue_.clear();
}

bool ChunkGeneration::stored(int cx, int cz) const {
  return world_.has_stored_column(cx, cz);
}

bool ChunkGeneration::ring_stored(int cx, int cz) const {
  for (int dx = -1; dx <= 1; ++dx)
    for (int dz = -1; dz <= 1; ++dz)
      if (!world_.has_stored_column(cx + dx, cz + dz))
        return false;
  return true;
}

ChunkGeneration::Stats ChunkGeneration::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stats_;
}

std::size_t ChunkGeneration::pending() const {
  std::lock_guard<std::mutex> lock(mu_);
  return queue_.size();
}

std::string ChunkGeneration::report() const {
  std::lock_guard<std::mutex> lock(mu_);
  char line[256];
  std::snprintf(line, sizeof(line),
                "chunk generation: %llu columns generated by %d thread(s), %llu requested, "
                "%llu already stored, %llu dropped, %llu failed, %llu queued",
                static_cast<unsigned long long>(stats_.generated), options_.threads,
                static_cast<unsigned long long>(stats_.requested),
                static_cast<unsigned long long>(stats_.already),
                static_cast<unsigned long long>(stats_.dropped),
                static_cast<unsigned long long>(stats_.failed),
                static_cast<unsigned long long>(queue_.size()));
  return std::string(line);
}

void ChunkGeneration::run() {
  for (;;) {
    std::pair<int, int> job{};
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (queue_.empty())
        return; // `stop_` was set and the queue is drained
      job = queue_.front();
      queue_.pop_front();
    }
    try {
      world_.ensure_chunk_stored(job.first, job.second);
      std::lock_guard<std::mutex> lock(mu_);
      ++stats_.generated;
    } catch (...) {
      // A build that throws (the allocator, in practice) kills nothing: the
      // column stays unbuilt, the next request for it tries again, and the
      // count says it happened rather than hiding it. Nothing here logs, because
      // a worker thread must not touch the log from inside a catch of unknown
      // shape; the shutdown line is where the number is read.
      std::lock_guard<std::mutex> lock(mu_);
      ++stats_.failed;
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      known_.erase(column_key(job.first, job.second));
    }
  }
}

} // namespace mc::world
