// SPDX-License-Identifier: GPL-3.0-or-later
//
// Chunk generation ahead of the view: the columns a player's view is about to
// need, built on their own threads instead of on the thread that streams to
// them.
//
// Why it exists. A generated column costs ~0.3-1.7 s of CPU on this engine
// (docs/PHASE5.md, the biome-memo section), and every path that *needs* one
// waits for it: the send path builds the column and the 3x3 ring its light pass
// borrows, inline, on the session thread that asked. A join storm therefore
// spends its session threads generating — one 30-bot cell advanced the stream at
// 0.6-1.7 columns/s while each step blocked the connection that needed it,
// keep-alives included — and the work ran only as fast as those blocked threads
// happened to overlap.
//
// What this is. A work queue and N threads. `request` is the whole interface a
// caller needs: a column that will be wanted soon, plus the ring around it.
// Workers call `WorldInterface::ensure_chunk_stored`, which is exactly "build
// this column's terrain and store it, hand nothing out" and already builds
// outside the store's lock (world/memory_world.cpp). Nothing here decides
// anything about the world: generation is a pure function of (seed, cx, cz), so
// a column built here is byte-identical to one built by the inline path, and a
// request for a column that is already stored costs a map lookup.
//
// What it deliberately is not. Not a scheduler: requests are built in the order
// they were asked for (the stream path asks nearest-first), and readiness is
// answered by the store rather than by a bookkeeping set of this class's own, so
// a caller can ask "is this ready?" without sharing state with the pool. Not
// required for correctness either: with no pool installed the stream path builds
// inline exactly as it always did, which is what the `PLUSMC_GEN_THREADS=0` arm
// measures against. Shutdown discards whatever was still queued — nothing is
// waiting for those columns — and joins the workers after the column each was
// already inside, so a server stops in a column's time and not a backlog's.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "world/world_interface.h"

namespace mc::world {

class ChunkGeneration {
public:
  struct Options {
    // Worker threads. 0 makes `ChunkGeneration` useless on purpose — the caller
    // is expected to have checked `enabled` and not built one at all — so the
    // constructor clamps to at least one.
    int threads = 4;
    // The queue's ceiling, in columns. A request that arrives with the queue
    // full is dropped rather than accepted-and-ignored: the cost of a dropped
    // request is one more inline build by the caller, the cost of a blocking
    // request is the stall this class exists to remove. A join storm queues
    // more than this (30 bots x 289 columns), so the ceiling is also what keeps
    // a wedged pool from holding a whole population's view in memory.
    std::size_t capacity = 8192;
  };

  // Counters for the shutdown line (and the soak records that parse it). Every
  // one of them is a count of *requests*, not of world state, so they are stable
  // under any interleaving a test cares about.
  struct Stats {
    uint64_t requested = 0; // columns accepted for generation
    uint64_t generated = 0; // columns a worker built and stored
    uint64_t already = 0;   // requests answered "already stored"
    uint64_t dropped = 0;   // requests refused because the queue was full
    uint64_t failed = 0;    // builds that threw; the worker survives and says so
  };

  // Two constructors rather than `Options = {}`: the nested type's default
  // member initializers cannot be used from a default argument inside the
  // enclosing class (clang rejects it outright, and rightly — the class is not
  // complete there yet).
  explicit ChunkGeneration(WorldInterface& world) : ChunkGeneration(world, Options{}) {}
  ChunkGeneration(WorldInterface& world, Options options);
  ~ChunkGeneration();
  ChunkGeneration(const ChunkGeneration&) = delete;
  ChunkGeneration& operator=(const ChunkGeneration&) = delete;
  ChunkGeneration(ChunkGeneration&&) = delete;
  ChunkGeneration& operator=(ChunkGeneration&&) = delete;

  // Ask for (cx, cz) to be built soon, with the 3x3 ring its light pass wants.
  // The column itself is queued before its ring, so it can never be ready later
  // than the neighbours that need it. Cheap and safe from any thread; an
  // already-stored column, or one already queued or being built, records nothing
  // but the `Stats::already` count.
  void request(int cx, int cz);

  // Give back everything accepted but not yet started. Called when nobody is
  // online: those columns were prefetch for a view that no longer exists, and a
  // worker that keeps building them stores terrain the idle release is about to
  // drop — the release and the backlog then take turns, which is what the
  // post-leave window measured (docs/PHASE5.md §4: the store refilled to its
  // ceiling twice inside one 45 s leave window, so a 192-column release read as
  // 576). In-flight jobs still finish (a worker inside the store cannot be
  // interrupted there); `Stats::requested` keeps the count, and a later request
  // for a discarded queued column queues it again. A column already being built
  // stays in the known set, so that later request cannot start a second build
  // of the same column.
  void discard_pending();

  // Whether a column is stored right now, i.e. reading it would build nothing.
  // For a generator world this is not `has_chunk`: that answers "terrain exists
  // here" (true for a column nobody has built yet), and the question the stream
  // path is really asking is whether the build has already happened.
  bool stored(int cx, int cz) const;
  // ...and whether the whole 3x3 is, which is the condition the send path's
  // light pass needs: `warm_light_region` builds any missing ring column inline,
  // on the asking thread.
  bool ring_stored(int cx, int cz) const;

  Stats stats() const;
  std::size_t pending() const;
  int thread_count() const {
    return options_.threads;
  }

  // One line, in the shape of the session's other shutdown reports
  // (`chunk_share_report`, session.h), so a soak can grep it out of the log.
  std::string report() const;

private:
  void run();
  void request_one(int cx, int cz);

  WorldInterface& world_;
  Options options_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::pair<int, int>> queue_;
  // Queued *and* in-flight coordinates: a request for a column a worker is
  // already building is a no-op, which is what keeps two workers from building
  // the same column (the store tolerates that race — the loser of
  // `store_or_find_locked` is dropped — but paying for one generation twice is
  // exactly the waste this class exists to remove).
  std::unordered_set<int64_t> known_;
  bool stop_ = false;
  Stats stats_;
  std::vector<std::thread> threads_;
};

} // namespace mc::world
