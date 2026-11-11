// SPDX-License-Identifier: GPL-3.0-or-later
//
// The relight against the world: the exclusion that keeps a chunk-stream
// SIGSEGV from happening again, and the concurrent shape it happened in.
//
// The bug this pins (convo/request_All_chunk_stream_light_crash.json, and the
// two unreported crashes beside it): `relight_world_chunk` borrows a 3x3 region
// of *live* columns from the world and writes one of them back, doing the whole
// thing without a single world operation of its own — `compute_chunk_light`
// reads the borrowed columns directly, which is the whole point of
// `LightRegionRing`: a relight must not pay nine ~400 KiB column copies. Every
// other reader of a column reaches it through a store method, and both stores
// hold their own mutex for the whole call — so the relight was the one writer in
// the process holding no lock while it mutated a column another session thread
// was, at that moment, copying (`load_chunk`'s snapshot), reading
// (`light_level`, `get_block_state`) or writing (`set_block_state`). Two threads
// sharing a `std::vector`'s buffer with no ordering is a double free, and a
// double free surfaces as any of the three SIGSEGVs on record: a null off
// `compute_chunk_light`, a corrupt block-state hash table, and a string read out
// of freed memory in a `RegistryEntry` destructor — all on session threads, all
// under a wide-spread bot population.
//
// The fix is `WorldInterface::lock_relight`/`unlock_relight`
// (world/world_interface.h): the relight takes the store's own exclusion for the
// whole borrow-and-write, and the store's mutex became re-entrant so the
// relight's own borrows can re-enter it. This file pins that two ways:
//
//   1. **the contract, deterministically** — a `ProbeWorld` derived from
//      `MemoryWorld` records whether the exclusion was held at the moment the
//      relight borrowed a column. With the guard removed from
//      `relight_world_chunk` the record says "no" and this check fails.
//   2. **the shape, concurrently** — four threads relighting the same nine
//      chunks (so every chunk is a centre for one thread and a neighbour for
//      another at the same time) while two more copy and read those columns and
//      one of them mutates a column. Nothing here has to crash to be a failure:
//      a relight of a column whose blocks have not moved must report *no*
//      change, so a half-written or torn column is caught by that invariant even
//      when the race happens not to take the process down.
//   3. **the two-phase commit** — the relight now copies the region's light
//      inputs under the exclusion, computes with it released, and writes the
//      light back only if all nine columns still carry the versions it copied
//      (world/world_interface.h's `chunk_content_version`). Three checks: the
//      versioned arm produces byte-identical light *and* the identical diff to
//      the in-lock arm on the same scene; a write that lands between the copy
//      and the commit makes the relight retry and commit light for the *new*
//      blocks (not the stale ones); and the versioned arm really is taken (a
//      world that cannot version — `NoVersionWorld` — is the in-lock arm).
//
// Usage: light_relight_race_test

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "world/chunk.h"
#include "world/light.h"
#include "world/memory_world.h"
#include "world/world_interface.h"
#include "world/world_light.h"

namespace {

int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

mc::world::BlockStateKey block(const char* name) {
  return mc::world::BlockStateKey{name, ""};
}

// A MemoryWorld that records whether a relight held the store's exclusion while
// it borrowed. Only the two accessors the relight and its ring go through are
// overridden, so everything else (the store, the light engine) is the real
// thing.
class ProbeWorld : public mc::world::MemoryWorld {
public:
  void lock_relight() override {
    mc::world::MemoryWorld::lock_relight();
    owner_ = std::this_thread::get_id();
  }
  void unlock_relight() override {
    owner_ = std::thread::id{};
    mc::world::MemoryWorld::unlock_relight();
  }

  // Reached from inside `relight_world_chunk` only: the ring's borrows and the
  // relight's own centre borrow.
  bool has_chunk(int cx, int cz) const override {
    note_borrow();
    return mc::world::MemoryWorld::has_chunk(cx, cz);
  }
  std::optional<mc::world::ChunkLease> modify_chunk(int cx, int cz) override {
    note_borrow();
    return mc::world::MemoryWorld::modify_chunk(cx, cz);
  }

  void start_recording() {
    recording_ = true;
  }
  bool saw_unexcluded_borrow() const {
    return unexcluded_;
  }

private:
  void note_borrow() const {
    if (recording_ && owner_ != std::this_thread::get_id())
      unexcluded_ = true;
  }

  mutable std::thread::id owner_{};
  mutable bool recording_ = false;
  mutable bool unexcluded_ = false;
};

// A column that is already current must not move light when it is relit again.
// A relight that can see a column another thread is halfway through writing
// breaks that: it computes against a mixture of the old and new arrays.
void check_idempotent(mc::world::WorldInterface& world, int cx, int cz) {
  mc::world::relight_world_chunk(cx, cz, world); // bring it current (blocks may have moved)
  const mc::world::LightChange again = mc::world::relight_world_chunk(cx, cz, world);
  if (again.any()) {
    std::fprintf(stderr, "FAIL %s:%d: relight(%d,%d) of an unchanged column moved light\n",
                 __FILE__, __LINE__, cx, cz);
    ++failures;
  }
}

void contract_check() {
  ProbeWorld world; // void world: a stored column is generated air
  // Seed the nine columns the relight will borrow, before recording, so the
  // record covers the relight's own borrows and nothing else.
  for (int cx = 0; cx < 3; ++cx)
    for (int cz = 0; cz < 3; ++cz)
      CHECK(world.modify_chunk(cx, cz).has_value());
  world.start_recording();

  const mc::world::LightChange change = mc::world::relight_world_chunk(1, 1, world);
  CHECK(change.any()); // a void column's sky light is computed on the first relight
  CHECK(!world.saw_unexcluded_borrow());
  check_idempotent(world, 1, 1);
}

void concurrent_shape() {
  mc::world::MemoryWorld world;
  std::vector<std::pair<int, int>> coords;
  for (int cx = 0; cx < 3; ++cx)
    for (int cz = 0; cz < 3; ++cz)
      coords.emplace_back(cx, cz);

  constexpr int kRelightThreads = 4;
  constexpr int kRounds = 40;
  std::atomic<bool> stop{false};
  std::atomic<int> relights_done{0};
  std::atomic<int> reader_failures{0};

  // A hang here would be a store that serialized the relight with a
  // *non*-re-entrant mutex (the relight's borrows re-enter it). Make it a
  // failure with a bound rather than a stuck suite.
  //
  // The bound is a deadlock detector, not a performance assertion: this shape
  // runs 2-3x slower under ASan and more under TSan, so a sanitizer run raises
  // the bound through `LIGHT_RELIGHT_RACE_WATCHDOG_SEC` (seconds). The default
  // is unchanged at 30 s.
  int watchdog_seconds = 30;
  if (const char* env = std::getenv("LIGHT_RELIGHT_RACE_WATCHDOG_SEC")) {
    const int v = std::atoi(env);
    if (v > 0)
      watchdog_seconds = v;
  }
  std::thread watchdog([&relights_done, watchdog_seconds]() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(watchdog_seconds);
    while (std::chrono::steady_clock::now() < deadline) {
      if (relights_done.load() == kRelightThreads)
        return;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::fprintf(stderr, "FAIL light_relight_race_test: relights did not finish (deadlock?)\n");
    std::_Exit(1);
  });

  // Two readers: the chunk-send path's snapshot copy, and the random-tick
  // pass's light read.
  std::vector<std::thread> readers;
  for (int i = 0; i < 2; ++i) {
    readers.emplace_back([&world, &coords, &stop, &reader_failures]() {
      while (!stop.load()) {
        for (const auto& [cx, cz] : coords) {
          const auto copy = world.load_chunk(cx, cz);
          if (!copy || copy->sections.size() != 24)
            reader_failures.fetch_add(1);
          const int level =
              world.light_level(mc::world::LightLayer::kBlock, cx * 16 + 8, 100, cz * 16 + 8);
          if (level < 0 || level > 15)
            reader_failures.fetch_add(1);
        }
      }
    });
  }

  std::vector<std::thread> relighters;
  for (int i = 0; i < kRelightThreads; ++i) {
    relighters.emplace_back([&world, &coords, &relights_done, i]() {
      for (int round = 0; round < kRounds; ++round) {
        for (const auto& [cx, cz] : coords) {
          mc::world::relight_world_chunk(cx, cz, world);
          // A block write mid-flight from one of the relight threads: the store
          // serializes it against the relights, and what lands must still be the
          // light the blocks imply.
          if (i == 0 && round % 7 == 0)
            world.set_block_state(cx, cz, 8, 100 + round, 8, block("minecraft:stone"));
        }
      }
      relights_done.fetch_add(1);
    });
  }

  for (std::thread& t : relighters)
    t.join();
  stop.store(true);
  for (std::thread& t : readers)
    t.join();
  watchdog.join();
  CHECK(reader_failures.load() == 0);

  // Every column is now current and structurally intact, and re-relighting it
  // moves nothing.
  for (const auto& [cx, cz] : coords) {
    const auto chunk = world.load_chunk(cx, cz);
    CHECK(chunk.has_value());
    if (!chunk)
      continue;
    CHECK(chunk->sections.size() == 24);
    CHECK(mc::world::chunk_has_light(*chunk));
    check_idempotent(world, cx, cz);
  }
}

// ---- the two-phase commit -------------------------------------------------

// The same scene in every world these tests build, so the two arms are
// comparable: a stone floor across the whole 3x3 region, a low wall, and — when
// `glowstone` — one light source in the centre column. A flat air column would
// light identically under almost any bug, which is why the scene has a source.
void seed_scene(mc::world::WorldInterface& world, bool glowstone) {
  for (int x = 0; x < 48; ++x)
    for (int z = 0; z < 48; ++z)
      world.set_block_state(x / 16, z / 16, x % 16, 0, z % 16, block("minecraft:stone"));
  for (int y = 1; y <= 4; ++y)
    world.set_block_state(1, 1, 2, y, 3, block("minecraft:stone"));
  if (glowstone)
    world.set_block_state(1, 1, 8, 1, 8, block("minecraft:glowstone"));
}

bool same_light(const mc::world::Chunk& a, const mc::world::Chunk& b) {
  if (a.sections.size() != b.sections.size())
    return false;
  for (size_t i = 0; i < a.sections.size(); ++i)
    if (a.sections[i].sky_light != b.sections[i].sky_light ||
        a.sections[i].block_light != b.sections[i].block_light)
      return false;
  return true;
}

// A store that answers "cannot say": every relight takes the original in-lock
// arm, which is what the versioned arm is compared against.
class NoVersionWorld : public mc::world::MemoryWorld {
public:
  uint64_t chunk_content_version(int /*cx*/, int /*cz*/) const override {
    return 0;
  }
};

// Counts the version reads, which is how the equivalence check knows the
// two-phase arm ran rather than silently falling back. A clean relight reads the
// nine versions twice — once to copy, once to commit — and no more.
class CountingWorld : public mc::world::MemoryWorld {
public:
  uint64_t chunk_content_version(int cx, int cz) const override {
    ++reads_;
    return mc::world::MemoryWorld::chunk_content_version(cx, cz);
  }
  int reads() const {
    return reads_;
  }

private:
  mutable int reads_ = 0;
};

// Simulates a write landing in the region while the light is computed: the
// commit phase's version read (the second read of the centre) performs the write
// *before* answering, so the version it returns is honestly the new one and the
// relight must see the mismatch and retry. The write removes the glowstone, so a
// stale commit is visible: the new blocks carry no block light at that cell.
class MutateOnCommitWorld : public mc::world::MemoryWorld {
public:
  uint64_t chunk_content_version(int cx, int cz) const override {
    if (cx == 1 && cz == 1) {
      ++centre_reads_;
      if (centre_reads_ == 2)
        const_cast<MutateOnCommitWorld*>(this)->set_block_state(1, 1, 8, 1, 8,
                                                                block("minecraft:air"));
    }
    return mc::world::MemoryWorld::chunk_content_version(cx, cz);
  }
  int centre_reads() const {
    return centre_reads_;
  }

private:
  mutable int centre_reads_ = 0;
};

// The same scene, the same relight: the versioned arm and the in-lock arm must
// agree on the light arrays *and* on the diff, or the fast path has changed what
// the client is told.
void versioned_matches_locked_path() {
  CountingWorld fast;
  NoVersionWorld locked;
  seed_scene(fast, /*glowstone=*/true);
  seed_scene(locked, /*glowstone=*/true);

  const mc::world::LightChange a = mc::world::relight_world_chunk(1, 1, fast);
  const mc::world::LightChange b = mc::world::relight_world_chunk(1, 1, locked);
  CHECK(a.any());
  CHECK(b.any());
  // 9 versions to copy + 9 to commit. Anything less means the two-phase arm did
  // not run (a fallback would read nine, from the in-lock path's own check-free
  // borrows, zero), and anything more means a retry happened on a quiet scene.
  CHECK(fast.reads() == 18);
  CHECK(a.first_section_y == b.first_section_y);
  CHECK(a.slot_count == b.slot_count);
  CHECK(a.sky_changed == b.sky_changed);
  CHECK(a.block_changed == b.block_changed);

  const auto fast_chunk = fast.load_chunk(1, 1);
  const auto locked_chunk = locked.load_chunk(1, 1);
  CHECK(fast_chunk.has_value());
  CHECK(locked_chunk.has_value());
  if (fast_chunk && locked_chunk)
    CHECK(same_light(*fast_chunk, *locked_chunk));
}

// A write between the copy and the commit must cost a retry, and the light that
// lands must be the light of the blocks as they are *now* — not of the copy the
// compute read.
void commit_retries_after_a_write() {
  MutateOnCommitWorld world;
  seed_scene(world, /*glowstone=*/true);
  const mc::world::LightChange change = mc::world::relight_world_chunk(1, 1, world);
  // Four reads of the centre: two captures and two commits. Three would mean the
  // stale attempt committed anyway.
  CHECK(world.centre_reads() >= 4);
  CHECK(change.any());

  const auto chunk = world.load_chunk(1, 1);
  CHECK(chunk.has_value());
  if (!chunk)
    return;
  CHECK(chunk->block_state_at(8, 1, 8).name == "minecraft:air");
  // The glowstone is gone, so its own cell carries no block light. A stale
  // commit would leave 15 here.
  CHECK(mc::world::chunk_light_level(*chunk, 8, 1, 8, mc::world::LightLayer::kBlock) == 0);

  // The reference: the same scene with the write applied *before* the relight.
  NoVersionWorld reference;
  seed_scene(reference, /*glowstone=*/false);
  mc::world::relight_world_chunk(1, 1, reference);
  const auto expected = reference.load_chunk(1, 1);
  CHECK(expected.has_value());
  if (expected)
    CHECK(same_light(*chunk, *expected));
}

} // namespace

int main() {
  std::printf("light_relight_race_test\n");
  contract_check();
  versioned_matches_locked_path();
  commit_retries_after_a_write();
  concurrent_shape();
  if (failures != 0) {
    std::fprintf(stderr, "light_relight_race_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("light_relight_race_test: all checks passed\n");
  return 0;
}
