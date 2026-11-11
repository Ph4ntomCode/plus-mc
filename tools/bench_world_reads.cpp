// SPDX-License-Identifier: GPL-3.0-or-later
//
// Scratch benchmark for the world read path — the cost of a state read, and the
// proof that the light write gate's kinds-only accessor answers exactly what the
// read answers. Not part of the build (no CMakeLists entry); a standalone file
// because the numbers it prints are the evidence for `light_kinds_at`:
//
//   * `reads` — what one `get_block_state` costs today, per state shape: ms per
//     million reads and the allocations each read makes (the two strings of the
//     returned `BlockStateKey` are copied out of the section's palette entry, so
//     a long property list lands on the heap). This is the cost the write gate
//     used to pay on every block write and no longer does.
//   * `gate` — both halves of the change side by side, in one process, on the
//     same cells: the *old* expression (`get_block_state` then `light_kinds`)
//     and the *new* one (`light_kinds_at`). Same function, same inline context,
//     so the delta is the copy alone. Needs `-DHAVE_KINDS_AT`.
//   * `gatepath` — the *whole* write gate (`LightWorld::set_block_state`, write
//     + state read + kinds compare + marking), timed over paired place/revert
//     writes, with the marked-chunk set hashed at the end. Run it twice, once
//     against a copy of `world/world_light.h` carrying the gate's old state
//     read (see the header comment's "old gate" build below), and the two runs
//     are the matched A/B: same library, same sequence, one header different.
//   * `writes` — a fixed edit sequence through the real write gate (`LightWorld`
//     + drain + relight), hashed: which chunks a write marks, and what the
//     relight moved. A reference: the unmodified build printed the same values
//     (the change cannot reach these answers — `light_kinds_at` is only asked
//     the kinds question the gate already asked).
//   * `solid` — the collision predicate's read, as `entity_block_is_solid`
//     (session.cpp) writes it: `block_blocks_motion(get_block_state(...).name)`.
//     This is the one *name-only* read on the session path — every entity that
//     moves, paths, falls or is clipped asks it — and the candidate a
//     `block_blocks_motion_at` accessor would shrink. Measured here so the two
//     candidates are compared as numbers.
//   * `loadchunk` — what asking "is there a column here?" costs when the answer
//     is read out of the value: `load_chunk(cx, cz).has_value()` builds a whole
//     overworld column (~400 KiB, ~100 allocations) and drops it, where the
//     store's `has_chunk` answers in nanoseconds. The natural-spawn pass asks
//     this once per candidate chunk per tick (`can_spawn_entities_in_chunk`),
//     and `can_load_chunk` is the accessor that answers it without the column.
//     Needs `-DHAVE_CAN_LOAD`; the equivalence walk over both worlds and the
//     `LightWorld` wrapper runs either way.
//   * `fluidtick` — the scheduled fluid tick's own reads, over a real
//     `MemoryWorld` column: a source settled into a pool (the steady state a
//     live server spends its ticks in) and a source pouring into air (the write
//     path). Prints the reads one tick makes, its timed cost, and the scene's
//     state hash. Those reads are the whole point: a tick asks about its own
//     cell, the two above/below it and its four sides, and it used to ask about
//     the sides three times over — the supply scan, the fan-out after a
//     recomputed cell rewrites itself, and the spread loop, whose own write
//     path read its target twice more. The matched A/B here is a *whole library*
//     pair rather than a header pair, since the rule lives in `fluid.cpp`: the
//     before side is built by swapping in the previous `world/fluid.cpp`,
//     rebuilding `plusmc-core` and compiling this file against it. The state
//     hash of the settled scene must not move between the two — the before side
//     ran the same ticks and left the same world.
//   * `probe` — walks the whole 32,366-state registry through one cell and
//     asserts `light_kinds_at(x,y,z) == light_kinds(get_block_state(x,y,z))` for
//     every state, hashing both sequences and counting allocations on each side.
//     This is the equivalence *proof*: the gate's decision is a function of the
//     kinds alone, so agreement over the full state space is agreement over
//     every write the server can ever make.
//
// Build (from the repo root, after the core library exists):
//
//   cmake --build build -j 8
//   /usr/bin/c++ -I src -isystem /opt/local/libexec/openssl3/include \
//     -O2 -g -DNDEBUG -std=c++20 -DHAVE_KINDS_AT -DHAVE_CAN_LOAD \
//     tools/bench_world_reads.cpp build/src/libplusmc-core.a \
//     -L/opt/local/libexec/openssl3/lib -lcrypto -L/opt/local/lib -lz \
//     -o /tmp/bench_world_reads
//
// The `fluidtick` A/B is a library pair, not a header pair — the rule is a
// .cpp — so the before side is its own build:
//
//   cp src/world/fluid.cpp /tmp/fluid_after.cpp
//   git show HEAD:src/world/fluid.cpp > src/world/fluid.cpp
//   cmake --build build -j 8 --target plusmc-core
//   /usr/bin/c++ ... -o /tmp/bench_before            # the same command as above
//   cp /tmp/fluid_after.cpp src/world/fluid.cpp
//   cmake --build build -j 8 --target plusmc-core
//   /usr/bin/c++ ... -o /tmp/bench_after
//
// then run `/tmp/bench_before fluidtick` and `/tmp/bench_after fluidtick` back
// to back: the reads per tick and the reads the pour path makes are exact, the
// timings are best-of-3 from the same session, and the scene state hash must be
// byte-identical between the two.
//
// The two `HAVE_*` guards only gate the *new* side of their A/B (the accessor
// that exists once its change is in); with them off the file still builds and
// prints the before side.
//
// Run: /tmp/bench_world_reads
// [reads|solid|loadchunk|relight|commit|reload|ring|scratch|fluidtick|gate|gatepath|writes|probe|all]
//      (default: all; `loadchunk` takes the fixtures dir, default tests/fixtures)
//
// The old-gate side of the A/B: the gate is defined inline in `world_light.h`,
// so reverting only that header's state read is enough to build the *before*
// binary against the same library —
//
//   mkdir -p /tmp/old_gate/world && cp src/world/world_light.h /tmp/old_gate/world/
//   # in the copy: `const int before = inner_->light_kinds_at(...)` becomes
//   #              `const BlockStateKey before = inner_->get_block_state(...)`
//   #              and `before != light_kinds(key)` becomes
//   #              `light_kinds(before) != light_kinds(key)`
//   /usr/bin/c++ -I /tmp/old_gate -I src ... -DHAVE_KINDS_AT -o /tmp/bench_old_gate
//
// then compare `gatepath` between /tmp/bench_old_gate and the plain build.
//
// Timing on a shared machine is noisy: every timed window is best-of-3 and the
// allocation counts are exact and deterministic. Compare CPU between runs only
// from the same session; compare allocations always.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "world/anvil.h"
#include "world/block_states.h"
#include "world/chunk.h"
#include "world/fluid.h"
#include "world/light.h"
#include "world/memory_world.h"
#include "world/mob_spawn.h"
#include "world/registry.h"
#include "world/world_interface.h"
#include "world/world_light.h"
#include "world/worldgen_chunk.h"

// ---------------------------------------------------------------------------
// Allocation counting: a global `operator new` that counts both the number of
// allocations and the bytes requested, so "allocations per read" and "bytes per
// read" are measured, not guessed. The byte counter is what makes a whole-column
// copy visible: one `load_chunk` of a 24-section overworld column asks for a few
// hundred KiB, where a palette entry's strings ask for a few dozen. Deleting
// goes straight to `free`.
// ---------------------------------------------------------------------------
namespace {
uint64_t g_allocs = 0;
uint64_t g_alloc_bytes = 0;

void* counted_alloc(std::size_t n) {
  ++g_allocs;
  g_alloc_bytes += n;
  void* p = std::malloc(n ? n : 1);
  if (!p)
    throw std::bad_alloc();
  return p;
}
} // namespace

void* operator new(std::size_t n) {
  return counted_alloc(n);
}
void* operator new[](std::size_t n) {
  return counted_alloc(n);
}
void* operator new(std::size_t n, std::align_val_t align) {
  ++g_allocs;
  g_alloc_bytes += n;
  void* p = nullptr;
  if (posix_memalign(&p, static_cast<std::size_t>(align), n ? n : 1) != 0)
    throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n, std::align_val_t align) {
  return ::operator new(n, align);
}
void operator delete(void* p) noexcept {
  std::free(p);
}
void operator delete[](void* p) noexcept {
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
  std::free(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
  std::free(p);
}

namespace {

using mc::world::BlockStateKey;
using mc::world::Chunk;
using mc::world::LightWorld;
using mc::world::MemoryWorld;

volatile int64_t g_sink = 0;

// Hands a buffer to code the compiler cannot see into. A buffer that never
// escapes lets the optimiser delete its zero-fill (nobody reads the bytes), so
// any timing of "allocate and zero" needs this to measure the real work.
__attribute__((noinline)) void opaque_read(const uint8_t* bytes, std::size_t n) {
  if (n == 0)
    return;
  uint64_t acc = 0;
  for (std::size_t i = 0; i < n; i += 64)
    acc = acc * 131 + bytes[i];
  acc = acc * 131 + bytes[n - 1];
  g_sink = g_sink + static_cast<int64_t>(acc);
}

uint64_t fnv1a(uint64_t h, const void* data, std::size_t n) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < n; ++i) {
    h ^= bytes[i];
    h *= 1099511628211ULL;
  }
  return h;
}
uint64_t fnv_int(uint64_t h, int64_t v) {
  return fnv1a(h, &v, sizeof(v));
}

struct Result {
  double ms_per_million = 0;
  double allocs_per_op = 0;
  double bytes_per_op = 0;
};

// Best-of-`reps` timing, with the allocation counts taken from the fastest pass
// (the counts are the same in every pass; they are properties of the code, not
// the machine's mood).
template <typename F> Result measure(const char* label, int iters, F&& body) {
  double best = 1e300;
  uint64_t allocs = 0;
  uint64_t bytes = 0;
  for (int rep = 0; rep < 3; ++rep) {
    g_allocs = 0;
    g_alloc_bytes = 0;
    const auto t0 = std::chrono::steady_clock::now();
    body();
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (ms < best) {
      best = ms;
      allocs = g_allocs;
      bytes = g_alloc_bytes;
    }
  }
  const Result r{best * 1e6 / iters, static_cast<double>(allocs) / iters,
                 static_cast<double>(bytes) / iters};
  std::printf("%-46s %10.1f ms/1e6  %7.3f allocs/op  %9.0f B/op\n", label, r.ms_per_million,
              r.allocs_per_op, r.bytes_per_op);
  return r;
}

BlockStateKey must_state(const char* name, const char* props) {
  BlockStateKey key{name, props};
  if (!mc::world::full_block_states().lookup(key))
    std::printf("!! probe state does not resolve: %s [%s]\n", name, props);
  return key;
}

// The cell the probe mutates, so every state can be asked through one section
// without growing a palette per iteration.
Chunk& materialise(MemoryWorld& world, int cx, int cz) {
  world.set_block_state(cx, cz, 8, 64, 8, must_state("minecraft:stone", ""));
  auto ref = world.modify_chunk(cx, cz);
  if (!ref)
    std::abort();
  return ref->get();
}

// ---------------------------------------------------------------------------
// reads: what one read costs, per state shape.
// ---------------------------------------------------------------------------
void bench_reads() {
  auto inner = std::make_shared<MemoryWorld>();
  const mc::world::WorldInterface& w = *inner;
  inner->set_block_state(0, 0, 8, 64, 8, must_state("minecraft:stone", ""));
  inner->set_block_state(
      0, 0, 2, 64, 2,
      must_state("minecraft:redstone_wire", "east=side,north=side,power=8,south=side,west=side"));
  inner->set_block_state(0, 0, 4, 64, 4,
                         must_state("minecraft:oak_stairs",
                                    "facing=north,half=bottom,shape=straight,waterlogged=false"));
  inner->set_block_state(
      0, 0, 6, 64, 6,
      must_state("minecraft:oak_leaves", "distance=7,persistent=false,waterlogged=false"));

  const int kIters = 1000000;
  struct Cell {
    const char* label;
    int cx, cz, x, y, z;
  };
  const Cell cells[] = {
      {"read: air (8,64,9)", 0, 0, 8, 64, 9},
      {"read: stone (8,64,8)", 0, 0, 8, 64, 8},
      {"read: redstone_wire, 49B props", 0, 0, 2, 64, 2},
      {"read: oak_stairs, 58B props", 0, 0, 4, 64, 4},
      {"read: oak_leaves, 41B props", 0, 0, 6, 64, 6},
      {"read: unheld chunk (9,9)", 9, 9, 8, 64, 8},
  };
  for (const Cell& c : cells) {
    measure(c.label, kIters, [&w, &c] {
      for (int i = 0; i < kIters; ++i) {
        const BlockStateKey s = w.get_block_state(c.cx, c.cz, c.x, c.y, c.z);
        g_sink = g_sink + static_cast<int64_t>(s.name.size() + s.properties.size());
      }
    });
  }
}

// ---------------------------------------------------------------------------
// solid: the collision predicate `entity_block_is_solid` (session.cpp) as it is
// written today — read the state by value and ask `block_blocks_motion` about
// its name. This is the one *name-only* read on the session path (mobs, orbs,
// TNT, projectiles and the fire rules all ask it through `is_solid`, once per
// candidate cell in A*, once per neighbour in a swept box, once per voxel a ray
// clip enters), so it is the read a `block_blocks_motion_at` accessor would
// shrink the way `light_kinds_at` shrank the write gate's. Measured here so the
// choice between it and the column copy above is a number, not a guess.
// ---------------------------------------------------------------------------
void bench_solid() {
  auto inner = std::make_shared<MemoryWorld>();
  const mc::world::WorldInterface& w = *inner;
  inner->set_block_state(0, 0, 8, 64, 8, must_state("minecraft:stone", ""));
  inner->set_block_state(
      0, 0, 2, 64, 2,
      must_state("minecraft:redstone_wire", "east=side,north=side,power=0,south=side,west=side"));

  const int kIters = 1000000;
  const int kNoProps = mc::world::block_blocks_motion("minecraft:stone") ? 1 : 0;
  (void)kNoProps;
  struct Cell {
    const char* label;
    int cx, cz, x, y, z;
  };
  const Cell cells[] = {
      {"solid: stone (no props)", 0, 0, 8, 64, 8},
      {"solid: air (no props)", 0, 0, 8, 64, 9},
      {"solid: redstone_wire (49B props)", 0, 0, 2, 64, 2},
      {"solid: unheld chunk", 9, 9, 8, 64, 8},
  };
  for (const Cell& c : cells) {
    measure(c.label, kIters, [&w, &c] {
      for (int i = 0; i < kIters; ++i) {
        const bool solid =
            mc::world::block_blocks_motion(w.get_block_state(c.cx, c.cz, c.x, c.y, c.z).name);
        g_sink = g_sink + (solid ? 1 : 0);
      }
    });
  }
}

// ---------------------------------------------------------------------------
// loadchunk: the cost of asking "is there a column here?" with `load_chunk` —
// the column is copied out and then dropped by the caller, because
// `optional<Chunk>::has_value()` is answered from the copy. The natural-spawn
// pass asks exactly this once per candidate chunk per tick (session.cpp's
// `can_spawn_entities_in_chunk`), and both worlds have a store that answers the
// same question without the copy (`has_chunk`). Measured against the fixture
// region so the numbers are a real AnvilWorld column and not an air column.
// ---------------------------------------------------------------------------
void bench_load_chunk(const char* fixtures_dir) {
  // The two worlds' answers, side by side, on the same question: the in-memory
  // world fabricates a column for a position it holds nothing at, so the load
  // question is true there where the store question is false — the reason the
  // load question cannot simply be rewired to `has_chunk`.
  auto mem = std::make_shared<MemoryWorld>();
  // A column MemoryWorld has actually stored, so `has_chunk` is true at it.
  mem->set_block_state(0, 0, 8, 64, 8, must_state("minecraft:stone", ""));
  std::printf("memory: stored column  load=%d store=%d | unheld column  load=%d store=%d\n",
              mem->load_chunk(0, 0).has_value() ? 1 : 0, mem->has_chunk(0, 0) ? 1 : 0,
              mem->load_chunk(9, 9).has_value() ? 1 : 0, mem->has_chunk(9, 9) ? 1 : 0);

  // The equivalence proof for the new accessor: over a coordinate window that
  // carries present columns, absent ones and a dirty (mutated) one, the load
  // question and the load itself must agree, through the bare world and through
  // the `LightWorld` the session actually holds.
  int mismatches = 0;
  std::size_t checked = 0;
  auto check_window = [&](const mc::world::WorldInterface& w, int lo, int hi) {
    for (int cx = lo; cx <= hi; ++cx) {
      for (int cz = lo; cz <= hi; ++cz) {
        const bool by_load = w.load_chunk(cx, cz).has_value();
#if defined(HAVE_CAN_LOAD)
        const bool by_accessor = w.can_load_chunk(cx, cz);
#else
        const bool by_accessor = by_load;
#endif
        if (by_load != by_accessor) {
          ++mismatches;
          std::printf("  !! can_load_chunk(%d,%d)=%d but load has_value=%d\n", cx, cz,
                      by_accessor ? 1 : 0, by_load ? 1 : 0);
        }
        ++checked;
      }
    }
  };
  check_window(*mem, -3, 3);
#if defined(HAVE_CAN_LOAD)
  {
    auto mem_light = std::make_shared<LightWorld>(mem);
    check_window(*mem_light, -3, 3);
  }
#endif

  auto anvil = mc::world::AnvilWorld::open_region_dir(fixtures_dir);
  if (!anvil) {
    std::printf("loadchunk: cannot open region dir %s — skipping the AnvilWorld half\n",
                fixtures_dir);
    return;
  }
  std::printf("anvil: fixture (0,0) present=%d\n", anvil->load_chunk(0, 0).has_value() ? 1 : 0);
  check_window(*anvil, -4, 4);
  auto anvil_light = std::make_shared<LightWorld>(anvil);
  check_window(*anvil_light, -4, 4);
  std::printf("equivalence: %zu positions over both worlds, %d mismatches\n", checked, mismatches);
  if (mismatches != 0)
    std::abort();

  // The timed comparison, on the fixture's one present column. `load_chunk`
  // builds the column and throws it away; the accessor answers from the load's
  // own lookup. Through `LightWorld` too, which is what the session holds — a
  // missing forwarder would fall back to the default and stay slow.
  const int kIters = 20000;
  const Result load_r = measure("  load_chunk(0,0).has_value()", kIters, [&anvil] {
    for (int i = 0; i < kIters; ++i)
      g_sink = g_sink + (anvil->load_chunk(0, 0).has_value() ? 1 : 0);
  });
  const Result store_r = measure("  has_chunk(0,0)", kIters, [&anvil] {
    for (int i = 0; i < kIters; ++i)
      g_sink = g_sink + (anvil->has_chunk(0, 0) ? 1 : 0);
  });
#if defined(HAVE_CAN_LOAD)
  const Result access_r = measure("  can_load_chunk(0,0)", kIters, [&anvil] {
    for (int i = 0; i < kIters; ++i)
      g_sink = g_sink + (anvil->can_load_chunk(0, 0) ? 1 : 0);
  });
  const Result access_wrap_r =
      measure("  (LightWorld) can_load_chunk(0,0)", kIters, [&anvil_light] {
        for (int i = 0; i < kIters; ++i)
          g_sink = g_sink + (anvil_light->can_load_chunk(0, 0) ? 1 : 0);
      });
#else
  const Result access_r{};
  const Result access_wrap_r{};
#endif
  std::printf(
      "  -> per call: load %.2f us / %.0f allocs / %.0f B   accessor %.2f us / %.0f allocs / "
      "%.0f B   store %.2f us\n",
      load_r.ms_per_million / 1000.0, load_r.allocs_per_op, load_r.bytes_per_op,
      access_r.ms_per_million / 1000.0, access_r.allocs_per_op, access_r.bytes_per_op,
      store_r.ms_per_million / 1000.0);
  std::printf("  -> through LightWorld (what the session holds): %.2f us, %.0f allocs, %.0f B\n",
              access_wrap_r.ms_per_million / 1000.0, access_wrap_r.allocs_per_op,
              access_wrap_r.bytes_per_op);

  // How often the natural-spawn pass asks that question: `collect_spawn_chunks`
  // (session.cpp) walks the 17x17 square of chunks around each player
  // (kSpawnDistanceChunk) and keeps the ones whose *centre* is inside
  // kSpawnDistanceBlock, then the pass asks `can_spawn_entities_in_chunk` once
  // per kept candidate. The player stands at the middle of chunk (0,0).
  constexpr int kChunk = mc::world::kSpawnDistanceChunk;
  constexpr int kBlocks = mc::world::kSpawnDistanceBlock;
  int candidates = 0;
  int tracked = 0;
  for (int dx = -kChunk; dx <= kChunk; ++dx) {
    for (int dz = -kChunk; dz <= kChunk; ++dz) {
      ++tracked;
      const double centre_x = static_cast<double>((dx) * 16 + 8);
      const double centre_z = static_cast<double>((dz) * 16 + 8);
      if (centre_x * centre_x + centre_z * centre_z >=
          static_cast<double>(kBlocks) * static_cast<double>(kBlocks))
        continue;
      ++candidates;
    }
  }
  std::printf(
      "spawn pass: %d tracked chunks, %d candidate chunks, each one question -> %.2f MiB of "
      "column per tick, %.1f ms per tick at %.0f us/call\n",
      tracked, candidates,
      static_cast<double>(candidates) * load_r.bytes_per_op / (1024.0 * 1024.0),
      static_cast<double>(candidates) * load_r.ms_per_million / 1e6,
      load_r.ms_per_million / 1000.0);
}

// ---------------------------------------------------------------------------
// fluidtick: the fluid tick's own reads, over a real column. Everything a tick
// asks about goes through the level's `get_block`, which is the session's own
// `world->get_block_state` shape (chunk + local), so the reads counted here are
// the reads a server pays.
// ---------------------------------------------------------------------------
int chunk_of(int v) {
  return v >= 0 ? v / 16 : (v - 15) / 16;
}
int local_of(int v) {
  return v - chunk_of(v) * 16;
}

void bench_fluid_tick() {
  constexpr int kFloorY = -61;
  constexpr int kAirY = -60;
  const int cx = 0, cz = 0;
  auto inner = std::make_shared<MemoryWorld>();
  const mc::world::WorldInterface& w = *inner;
  const BlockStateKey air = must_state("minecraft:air", "");
  const BlockStateKey stone = must_state("minecraft:stone", "");
  const BlockStateKey source = must_state("minecraft:water", "level=0");

  // A one-chunk stone floor with a source at its middle: the fixture's `flat`
  // scene, and the shape whose pool reaches full width (water runs seven cells).
  for (int lx = 0; lx < 16; ++lx)
    for (int lz = 0; lz < 16; ++lz)
      inner->set_block_state(cx, cz, lx, kFloorY, lz, stone);
  inner->set_block_state(cx, cz, 8, kAirY, 8, source);

  int reads = 0;
  int schedules = 0;
  mc::world::FluidLevel level;
  level.get_block = [&w, &reads](int x, int y, int z) {
    ++reads;
    return w.get_block_state(chunk_of(x), chunk_of(z), local_of(x), y, local_of(z));
  };
  level.set_block = [&inner](int x, int y, int z, const BlockStateKey& state) {
    inner->set_block_state(chunk_of(x), chunk_of(z), local_of(x), y, local_of(z), state);
  };
  level.can_hold_fluid = [](const BlockStateKey& state) {
    if (state.name == "minecraft:air" || state.name == "minecraft:water" ||
        state.name == "minecraft:lava" || state.name == "minecraft:bubble_column")
      return true;
    return mc::world::block_property(state.properties, "waterlogged") == "false" &&
           !mc::world::block_blocks_motion(state.name);
  };
  // Settle the pool through the real queue and the real dispatch, so the window
  // below times a settled world rather than a scripted one. The level's
  // schedule callback is the queue's (`run_fluid_tick_position`'s shape: the
  // callback rebound to the entry's own kind before the rule runs) — without
  // that the rules' own schedules would go nowhere and the pool would never
  // grow past the source and its four first cells.
  mc::world::FluidTickQueue queue;
  mc::world::FluidKind tick_kind = mc::world::FluidKind::Water;
  level.schedule_tick = [&queue, &tick_kind, &schedules](int x, int y, int z, int delay) {
    ++schedules;
    queue.schedule(x, y, z, tick_kind, delay);
  };
  queue.schedule(8, kAirY, 8, mc::world::FluidKind::Water, 5);
  for (int tick = 0; tick < 45; ++tick) {
    std::vector<std::pair<mc::world::FluidKind, mc::world::FluidTickPos>> due = queue.advance();
    std::sort(due.begin(), due.end(),
              [](const std::pair<mc::world::FluidKind, mc::world::FluidTickPos>& a,
                 const std::pair<mc::world::FluidKind, mc::world::FluidTickPos>& b) {
                if (a.second.x != b.second.x)
                  return a.second.x < b.second.x;
                if (a.second.y != b.second.y)
                  return a.second.y < b.second.y;
                if (a.second.z != b.second.z)
                  return a.second.z < b.second.z;
                return a.first < b.first;
              });
    for (const auto& [kind, pos] : due) {
      tick_kind = kind;
      mc::world::fluid_tick(level, kind, pos.x, pos.y, pos.z);
    }
  }
  // Past the settle, the timed windows measure the rule alone: their schedules
  // are counted, not queued (a queued entry per iteration would grow the queue
  // to the iteration count and time that growth instead of the tick).
  level.schedule_tick = [&schedules](int, int, int, int) { ++schedules; };

  // The fluid cells the settled scene holds: one tick's dispatch in a live
  // server is exactly this list, once a tick.
  std::vector<std::tuple<int, int, int>> cells;
  for (int lx = 0; lx < 16; ++lx)
    for (int lz = 0; lz < 16; ++lz)
      if (!mc::world::fluid_state_of(w.get_block_state(cx, cz, lx, kAirY, lz)).empty())
        cells.push_back({lx, kAirY, lz});
  std::printf("  settled scene: %zu fluid cells, queue %zu pending\n", cells.size(), queue.size());

  // The read count per tick is deterministic, so it is taken from one clean pass
  // outside the timed window; the timing is best-of-3 like every other window in
  // this file.
  reads = 0;
  for (const auto& [x, y, z] : cells)
    mc::world::fluid_tick(level, mc::world::FluidKind::Water, x, y, z);
  const int reads_per_tick = reads;
  const int kIters = 100000;
  const Result settled = measure("fluidtick: settled pool, one dispatch", kIters, [&] {
    for (int i = 0; i < kIters; ++i)
      for (const auto& [x, y, z] : cells)
        mc::world::fluid_tick(level, mc::world::FluidKind::Water, x, y, z);
  });
  std::printf("  -> settled: %d reads for %zu cells (%.2f per cell), %.3f us/cell\n",
              reads_per_tick, cells.size(),
              static_cast<double>(reads_per_tick) / static_cast<double>(cells.size()),
              settled.ms_per_million / 1e3 / static_cast<double>(cells.size()));

  // The write path: a source with air below it pours a falling state in and
  // re-schedules it, which is the arm whose own reads shrank the most. The cell
  // is reset each iteration so the probe is the same tick every time.
  // The probe cell: a source over a hole, far enough from the pool that it is
  // its own shape (the pool's own water stays inside x, z in [1, 15]).
  inner->set_block_state(cx, cz, 15, kAirY, 15, source);
  inner->set_block_state(cx, cz, 15, kFloorY, 15, air);
  reads = 0;
  schedules = 0;
  mc::world::fluid_tick(level, mc::world::FluidKind::Water, 15, kAirY, 15);
  const int reads_per_pour = reads;
  const int schedules_per_pour = schedules;
  const Result pour = measure("fluidtick: source pouring into air, one tick", kIters, [&] {
    for (int i = 0; i < kIters; ++i) {
      inner->set_block_state(cx, cz, 15, kFloorY, 15, air);
      mc::world::fluid_tick(level, mc::world::FluidKind::Water, 15, kAirY, 15);
    }
  });
  std::printf("  -> pour: %d reads and %d schedules for one tick, %.3f us/tick\n", reads_per_pour,
              schedules_per_pour, pour.ms_per_million / 1e3);

  // The scene's own state, hashed: the A/B runs the same ticks and must leave
  // the same world behind. Every cell of the column the probe touched goes in.
  inner->set_block_state(cx, cz, 15, kFloorY, 15, stone);
  uint64_t hash = 1469598103934665603ULL;
  for (int y = kFloorY - 1; y <= kAirY + 1; ++y) {
    for (int lx = 0; lx < 16; ++lx) {
      for (int lz = 0; lz < 16; ++lz) {
        const BlockStateKey s = w.get_block_state(cx, cz, lx, y, lz);
        hash = fnv1a(hash, s.name.data(), s.name.size());
        hash = fnv1a(hash, s.properties.data(), s.properties.size());
      }
    }
  }
  std::printf("fluidtick: scene state hash %llu\n", static_cast<unsigned long long>(hash));
}

// ---------------------------------------------------------------------------
// gate: the write gate's state read, before and after, in one process.
// ---------------------------------------------------------------------------
void bench_gate() {
  auto inner = std::make_shared<MemoryWorld>();
  const mc::world::WorldInterface& w = *inner;
  inner->set_block_state(0, 0, 8, 64, 8, must_state("minecraft:stone", ""));
  inner->set_block_state(
      0, 0, 2, 64, 2,
      must_state("minecraft:redstone_wire", "east=side,north=side,power=0,south=side,west=side"));
  inner->set_block_state(
      0, 0, 4, 64, 4,
      must_state("minecraft:oak_leaves", "distance=7,persistent=false,waterlogged=false"));
  const int kIters = 500000;
  struct Cell {
    const char* label;
    int cx, cz, x, y, z;
  };
  const Cell cells[] = {
      {"gate read: stone (no props)", 0, 0, 8, 64, 8},
      {"gate read: redstone_wire (props)", 0, 0, 2, 64, 2},
      {"gate read: leaves (props)", 0, 0, 4, 64, 4},
  };
  for (const Cell& c : cells) {
    char old_label[128];
    char new_label[128];
    std::snprintf(old_label, sizeof(old_label), "  old: light_kinds(get_block_state) %s", c.label);
    std::snprintf(new_label, sizeof(new_label), "  new: light_kinds_at(pos)          %s", c.label);
    // Alternated so both see the same machine state.
    const Result old_r = measure(old_label, kIters, [&w, &c] {
      for (int i = 0; i < kIters; ++i) {
        const BlockStateKey s = w.get_block_state(c.cx, c.cz, c.x, c.y, c.z);
        g_sink = g_sink + mc::world::light_kinds(s);
      }
    });
#if defined(HAVE_KINDS_AT)
    const Result new_r = measure(new_label, kIters, [&w, &c] {
      for (int i = 0; i < kIters; ++i)
        g_sink = g_sink + w.light_kinds_at(c.cx, c.cz, c.x, c.y, c.z);
    });
    std::printf("  -> %s: %.1f -> %.1f ms/1e6 (%+.1f%%), allocs %.3f -> %.3f per op\n", c.label,
                old_r.ms_per_million, new_r.ms_per_million,
                100.0 * (new_r.ms_per_million - old_r.ms_per_million) / old_r.ms_per_million,
                old_r.allocs_per_op, new_r.allocs_per_op);
#else
    (void)new_label;
    std::printf("  -> %s: %.1f ms/1e6, allocs %.3f per op (new path not compiled in)\n", c.label,
                old_r.ms_per_million, old_r.allocs_per_op);
#endif
  }
}

// ---------------------------------------------------------------------------
// gatepath: the whole write gate, paired place/revert writes. Run this file
// twice — once against the reverted `world_light.h` — for the matched A/B.
// ---------------------------------------------------------------------------
void bench_gate_paths() {
  struct Pair {
    const char* label;
    const char* a_name;
    const char* a_props;
    const char* b_name;
    const char* b_props;
  };
  const Pair pairs[] = {
      {"gatepath: stone<->dirt (same kinds)", "minecraft:stone", "", "minecraft:dirt", ""},
      {"gatepath: glowstone<->stone (emission)", "minecraft:glowstone", "", "minecraft:stone", ""},
      {"gatepath: wire power 0<->8 (props)", "minecraft:redstone_wire",
       "east=side,north=side,power=0,south=side,west=side", "minecraft:redstone_wire",
       "east=side,north=side,power=8,south=side,west=side"},
      {"gatepath: oak_leaves<->water (dampening)", "minecraft:oak_leaves",
       "distance=7,persistent=false,waterlogged=false", "minecraft:water", "level=0"},
  };
  const int kIters = 200000; // pairs; 2 writes each
  for (const Pair& p : pairs) {
    auto inner = std::make_shared<MemoryWorld>();
    auto light = std::make_shared<LightWorld>(inner);
    const BlockStateKey a = must_state(p.a_name, p.a_props);
    const BlockStateKey b = must_state(p.b_name, p.b_props);
    if (!light->set_block_state(0, 0, 8, 64, 8, a))
      std::abort();
    light->take_dirty_chunks(); // the placement above is setup, not the pair
    const Result r = measure(p.label, kIters * 2, [&] {
      for (int i = 0; i < kIters; ++i) {
        light->set_block_state(0, 0, 8, 64, 8, a);
        light->set_block_state(0, 0, 8, 64, 8, b);
      }
    });
    auto marked = light->take_dirty_chunks();
    std::sort(marked.begin(), marked.end());
    uint64_t marks_hash = 1469598103934665603ULL;
    for (const auto& [cx, cz] : marked) {
      marks_hash = fnv_int(marks_hash, cx);
      marks_hash = fnv_int(marks_hash, cz);
    }
    std::printf("  -> %s: marked %zu chunks, marks hash %llu (%.1f ms/1e6 writes)\n", p.label,
                marked.size(), static_cast<unsigned long long>(marks_hash), r.ms_per_million);
  }
}

// ---------------------------------------------------------------------------
// writes: a fixed edit sequence through the real gate, hashed. Reference
// values — an unmodified build prints the same ones.
// ---------------------------------------------------------------------------
void bench_writes() {
  auto inner = std::make_shared<MemoryWorld>();
  auto light = std::make_shared<LightWorld>(inner);

  struct Edit {
    int cx, cz, x, y, z;
    const char* name;
    const char* props;
  };
  const Edit edits[] = {
      {0, 0, 8, 64, 8, "minecraft:stone", ""},
      {0, 0, 8, 64, 8, "minecraft:dirt", ""},
      {0, 0, 8, 65, 8, "minecraft:stone", ""},
      {0, 0, 8, 65, 8, "minecraft:air", ""},
      {0, 1, 2, 64, 2, "minecraft:oak_leaves", "distance=7,persistent=false,waterlogged=false"},
      {0, 1, 2, 64, 2, "minecraft:water", "level=0"},
      {0, 1, 2, 64, 2, "minecraft:oak_leaves", "distance=7,persistent=false,waterlogged=false"},
      {1, 0, 15, 64, 15, "minecraft:glowstone", ""},
      {1, 0, 15, 64, 15, "minecraft:stone", ""},
      {1, 1, 0, 64, 0, "minecraft:redstone_wire",
       "east=side,north=side,power=8,south=side,west=side"},
      {1, 1, 0, 64, 0, "minecraft:redstone_wire",
       "east=side,north=side,power=0,south=side,west=side"},
  };
  for (const Edit& e : edits) {
    if (!light->set_block_state(e.cx, e.cz, e.x, e.y, e.z, must_state(e.name, e.props))) {
      std::printf("!! write rejected: %s at (%d,%d,%d,%d,%d)\n", e.name, e.cx, e.cz, e.x, e.y, e.z);
      std::abort();
    }
  }

  auto marked = light->take_dirty_chunks();
  std::sort(marked.begin(), marked.end());
  uint64_t marks_hash = 1469598103934665603ULL;
  for (const auto& [cx, cz] : marked) {
    marks_hash = fnv_int(marks_hash, cx);
    marks_hash = fnv_int(marks_hash, cz);
  }
  std::printf("writes: marked %zu chunks, marks hash %llu\n", marked.size(),
              static_cast<unsigned long long>(marks_hash));

  uint64_t light_hash = 1469598103934665603ULL;
  int slots_moved = 0;
  for (const auto& [cx, cz] : marked) {
    const mc::world::LightChange change = mc::world::relight_world_chunk(cx, cz, *inner);
    light_hash = fnv_int(light_hash, change.first_section_y);
    light_hash = fnv_int(light_hash, change.slot_count);
    for (uint64_t word : change.sky_changed)
      light_hash = fnv_int(light_hash, static_cast<int64_t>(word));
    for (uint64_t word : change.block_changed)
      light_hash = fnv_int(light_hash, static_cast<int64_t>(word));
    for (int slot = 0; slot < change.slot_count; ++slot) {
      slots_moved += change.sky_at(slot) ? 1 : 0;
      slots_moved += change.block_at(slot) ? 1 : 0;
    }
  }
  std::printf("writes: relit %zu chunks, %d section-channels moved, lit hash %llu\n", marked.size(),
              slots_moved, static_cast<unsigned long long>(light_hash));

  uint64_t state_hash = 1469598103934665603ULL;
  for (const Edit& e : edits) {
    const BlockStateKey s = light->get_block_state(e.cx, e.cz, e.x, e.y, e.z);
    state_hash = fnv1a(state_hash, s.name.data(), s.name.size());
    state_hash = fnv1a(state_hash, s.properties.data(), s.properties.size());
  }
  std::printf("writes: edited-cell state hash %llu\n", static_cast<unsigned long long>(state_hash));
}

// ---------------------------------------------------------------------------
// relight: one whole-chunk light recompute, on a real overworld column. The
// light pass (session.cpp's `tick_light`) relights up to `kLightChunksPerTick`
// chunks a tick, and the engine's own note measures a full-height overworld
// column at ~7 ms — which makes the recompute the largest single thing a tick
// does once something is being written. Timed here, and hashed: the only thing
// a cheaper recompute may change is how long it takes.
// ---------------------------------------------------------------------------
void bench_relight(const char* fixtures_dir) {
  for (const char* sub : {"ow3region", ""}) {
    const std::string dir =
        std::string(fixtures_dir) + (sub[0] != '\0' ? std::string("/") + sub : std::string());
    auto world = mc::world::AnvilWorld::open_region_dir(dir);
    if (!world) {
      std::printf("relight: cannot open %s\n", dir.c_str());
      continue;
    }
    // Find a present column (the flat fixture has (0,0); ow3region has (-2,-1)).
    int cx = 0, cz = 0;
    if (sub[0] != '\0')
      cx = -2, cz = -1;
    if (!world->can_load_chunk(cx, cz)) {
      std::printf("relight: %s has no column at (%d,%d)\n", dir.c_str(), cx, cz);
      continue;
    }
    char label[160];
    std::snprintf(label, sizeof(label), "relight: %s chunk (%d,%d)", dir.c_str(), cx, cz);
    const int kIters = 30;
    const Result r = measure(label, kIters, [&] {
      for (int i = 0; i < kIters; ++i) {
        const mc::world::LightChange change = mc::world::relight_world_chunk(cx, cz, *world);
        g_sink = g_sink + (change.any() ? 1 : 0);
      }
    });
    // The result, hashed: every section's two light arrays. A change to the
    // engine that leaves this hash alone left the light alone.
    uint64_t light_hash = 1469598103934665603ULL;
    const auto chunk = world->load_chunk(cx, cz);
    if (chunk) {
      for (const auto& section : chunk->sections) {
        for (const auto* layer : {&section.sky_light, &section.block_light}) {
          if (!*layer) {
            light_hash = fnv_int(light_hash, -1);
            continue;
          }
          light_hash = fnv_int(light_hash, static_cast<int64_t>((*layer)->size()));
          light_hash = fnv1a(light_hash, (*layer)->data(), (*layer)->size());
        }
      }
    }
    std::printf("  -> %s: %.3f ms per relight, %zu sections, light hash %llu\n", label,
                r.ms_per_million / 1e6, chunk ? chunk->sections.size() : 0,
                static_cast<unsigned long long>(light_hash));

    // The same recompute over *every* column the region holds, each one against
    // a freshly opened world so the walk is a pure function of the chunk and its
    // neighbours-on-disk: a region-wide fingerprint of the engine's own answer,
    // to be compared between two builds. The masks the relight reports go in
    // too, since a packet difference with identical arrays would still matter.
    uint64_t walk_hash = 1469598103934665603ULL;
    int relit = 0;
    for (int dx = -1; dx <= 1; ++dx) {
      for (int dz = -1; dz <= 1; ++dz) {
        const int rcx = cx + dx;
        const int rcz = cz + dz;
        auto fresh = mc::world::AnvilWorld::open_region_dir(dir);
        if (!fresh || !fresh->can_load_chunk(rcx, rcz))
          continue;
        const mc::world::LightChange change = mc::world::relight_world_chunk(rcx, rcz, *fresh);
        walk_hash = fnv_int(walk_hash, change.first_section_y);
        walk_hash = fnv_int(walk_hash, change.slot_count);
        for (uint64_t word : change.sky_changed)
          walk_hash = fnv_int(walk_hash, static_cast<int64_t>(word));
        for (uint64_t word : change.block_changed)
          walk_hash = fnv_int(walk_hash, static_cast<int64_t>(word));
        const auto lit = fresh->load_chunk(rcx, rcz);
        if (lit) {
          for (const auto& section : lit->sections) {
            for (const auto* layer : {&section.sky_light, &section.block_light}) {
              if (!*layer) {
                walk_hash = fnv_int(walk_hash, -1);
                continue;
              }
              walk_hash = fnv_int(walk_hash, static_cast<int64_t>((*layer)->size()));
              walk_hash = fnv1a(walk_hash, (*layer)->data(), (*layer)->size());
            }
          }
        }
        ++relit;
      }
    }
    std::printf("  -> %s: region walk relit %d columns, light hash %llu\n", dir.c_str(), relit,
                static_cast<unsigned long long>(walk_hash));
  }
}

// ---------------------------------------------------------------------------
// scratch: the light engine's per-relight region. `compute_chunk_light` builds
// a region for the 3x3 ring — `9 * sections * 2` light arrays of 2048 bytes
// each (432 arrays, ~884 KiB at the overworld's 24 sections) — and today every
// one of those is a fresh `std::vector` allocated and zeroed for that call and
// freed at the end of it. This times that primitive in the shapes a fix could
// take: the current per-call vector-of-vectors, one flat buffer per call, and
// each of those kept between calls and re-zeroed in place. `ring` times a whole
// relight; the gap between the two is what the region scratch costs a relight.
// ---------------------------------------------------------------------------
void bench_scratch() {
  const int sections = mc::world::kOverworldSectionCount;
  const size_t arrays = static_cast<size_t>(9 * sections * 2);
  const size_t bytes = arrays * static_cast<size_t>(mc::world::kLightArrayBytes);
  const int kIters = 2000;
  const size_t array_bytes = static_cast<size_t>(mc::world::kLightArrayBytes);

  const Result fresh_vov = measure("scratch: vector-of-vectors per call", kIters, [&] {
    for (int i = 0; i < kIters; ++i) {
      std::vector<std::vector<uint8_t>> layers(arrays, std::vector<uint8_t>(array_bytes, 0));
      for (const std::vector<uint8_t>& layer : layers)
        opaque_read(layer.data(), layer.size());
    }
  });
  const Result fresh_flat = measure("scratch: one flat buffer per call", kIters, [&] {
    for (int i = 0; i < kIters; ++i) {
      std::vector<uint8_t> flat(bytes, 0);
      opaque_read(flat.data(), flat.size());
    }
  });
  std::vector<std::vector<uint8_t>> kept_vov(arrays, std::vector<uint8_t>(array_bytes, 0));
  const Result kept_vov_r = measure("scratch: vector-of-vectors kept, refilled", kIters, [&] {
    for (int i = 0; i < kIters; ++i) {
      for (std::vector<uint8_t>& layer : kept_vov) {
        layer.assign(array_bytes, 0);
        opaque_read(layer.data(), layer.size());
      }
    }
  });
  std::vector<uint8_t> kept_flat(bytes, 0);
  const Result kept_flat_r = measure("scratch: flat buffer kept, refilled", kIters, [&] {
    for (int i = 0; i < kIters; ++i) {
      std::fill(kept_flat.begin(), kept_flat.end(), static_cast<uint8_t>(0));
      opaque_read(kept_flat.data(), kept_flat.size());
    }
  });

  const auto us = [](double ms_per_million) { return ms_per_million / 1e3; };
  std::printf("  -> scratch: %zu arrays, %zu KiB; fresh vov %.2f us, fresh flat %.2f us, "
              "kept vov %.2f us, kept flat %.2f us per relight\n",
              arrays, bytes / 1024, us(fresh_vov.ms_per_million), us(fresh_flat.ms_per_million),
              us(kept_vov_r.ms_per_million), us(kept_flat_r.ms_per_million));
}

// ---------------------------------------------------------------------------
// ring: one relight of a chunk whose 3x3 neighbourhood really is there. The
// light ring only reads neighbours a world *holds* (`has_chunk`), and the
// shipped fixtures hold a single column, so the per-neighbour cost only shows up
// once the neighbours exist — which is the state a live server is in, because
// its chunk sends materialise the whole view. They are materialised here the
// same way a server makes them: by relighting each of the nine once (a relight
// of an absent column creates and stores it).
//
// Two windows, because the ring's per-neighbour cost has two parts:
//   * a clean ring — the copy alone (before: `Chunk::snapshot()`, a ~400 KiB,
//     ~100-allocation copy per neighbour; after: a borrow, no allocation);
//   * a marked ring — the same, plus the heightmap recompute a copy carries for
//     a column the store has not brought current, which a copy pays *again* on
//     every relight (a snapshot refreshes the copy and leaves the store's mark
//     where it was).
// All nine columns' light and motion-blocking heights are hashed at the end: a
// cheaper ring may change how long a relight takes and nothing else.
// ---------------------------------------------------------------------------
void bench_ring(const char* fixtures_dir) {
  auto world = mc::world::AnvilWorld::open_region_dir(std::string(fixtures_dir));
  if (!world) {
    std::printf("ring: cannot open %s\n", fixtures_dir);
    return;
  }
  const int cx = 0, cz = 0;
  if (!world->can_load_chunk(cx, cz)) {
    std::printf("ring: %s has no column at (%d,%d)\n", fixtures_dir, cx, cz);
    return;
  }
  for (int dz = -1; dz <= 1; ++dz)
    for (int dx = -1; dx <= 1; ++dx)
      mc::world::relight_world_chunk(cx + dx, cz + dz, *world);

  const int kIters = 400;
  const auto relight_centre = [&] {
    for (int i = 0; i < kIters; ++i) {
      const mc::world::LightChange change = mc::world::relight_world_chunk(cx, cz, *world);
      g_sink = g_sink + (change.any() ? 1 : 0);
    }
  };
  const Result clean = measure("ring: 9 columns present, clean", kIters, relight_centre);

  // One write per column: every column the ring reads is now marked, its lazy
  // heightmaps behind its blocks — the state a written world hands a relight.
  for (int dz = -1; dz <= 1; ++dz)
    for (int dx = -1; dx <= 1; ++dx)
      world->set_block_state(cx + dx, cz + dz, 8, -59, 8, must_state("minecraft:stone", ""));
  const Result marked = measure("ring: 9 columns marked by a write", kIters, relight_centre);

  uint64_t hash = 1469598103934665603ULL;
  for (int dz = -1; dz <= 1; ++dz) {
    for (int dx = -1; dx <= 1; ++dx) {
      auto chunk = world->load_chunk(cx + dx, cz + dz);
      if (!chunk) {
        hash = fnv_int(hash, 0);
        continue;
      }
      for (const auto& section : chunk->sections) {
        for (const auto* layer : {&section.sky_light, &section.block_light}) {
          if (!*layer) {
            hash = fnv_int(hash, -1);
            continue;
          }
          hash = fnv_int(hash, static_cast<int64_t>((*layer)->size()));
          hash = fnv1a(hash, (*layer)->data(), (*layer)->size());
        }
      }
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
          hash = fnv_int(hash, mc::world::chunk_motion_blocking_height(*chunk, x, z));
    }
  }
  std::printf("  -> ring: clean %.3f ms, marked %.3f ms per relight; "
              "9-column light+height hash %llu\n",
              clean.ms_per_million / 1e6, marked.ms_per_million / 1e6,
              static_cast<unsigned long long>(hash));
}

// ---------------------------------------------------------------------------
// commit: the two-phase relight against the one it replaced, on the shape the
// server actually runs it in — a `MemoryWorld` with the overworld generator and
// a 3x3 region already stored (the warm pass and the chunk sends get it there).
//
// Wall time is the smaller half of this A/B. What the two-phase protocol changes
// is how long the store's mutex is *held*, because that is what serialises every
// other connection: the compute (~7 ms for a full-height column) used to run
// under it, and now only the copy and the commit do. A probe world times its own
// exclusion spans, so the number is measured rather than inferred, and the two
// arms run in the same process on the same scene, back to back (best-of-3 each,
// like every other window here).
//
// The light is hashed after each arm: the arms may differ in cost, never in what
// they compute.
// ---------------------------------------------------------------------------
class LockTimerWorld : public mc::world::MemoryWorld {
public:
  explicit LockTimerWorld(std::shared_ptr<const mc::world::WgOverworldGenerator> generator)
      : mc::world::MemoryWorld(-4, 24, std::move(generator)) {}

  void lock_relight() override {
    mc::world::MemoryWorld::lock_relight();
    locked_at_ = std::chrono::steady_clock::now();
  }
  void unlock_relight() override {
    held_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - locked_at_)
                    .count();
    ++spans_;
    mc::world::MemoryWorld::unlock_relight();
  }

  void reset_timer() {
    held_ns_ = 0;
    spans_ = 0;
  }
  double held_ms() const {
    return static_cast<double>(held_ns_) / 1e6;
  }
  int spans() const {
    return spans_;
  }

private:
  std::chrono::steady_clock::time_point locked_at_{};
  long long held_ns_ = 0;
  int spans_ = 0;
};

// The in-lock arm: the same store, answering "cannot version", which is what
// makes `relight_world_chunk` take the original path (copy the declaration of
// `chunk_content_version` in world/world_interface.h).
class NoVersionTimerWorld : public LockTimerWorld {
public:
  using LockTimerWorld::LockTimerWorld;
  uint64_t chunk_content_version(int /*cx*/, int /*cz*/) const override {
    return 0;
  }
};

uint64_t hash_column_light(const mc::world::WorldInterface& world, int cx, int cz) {
  uint64_t hash = 1469598103934665603ULL;
  const auto chunk = world.load_chunk(cx, cz);
  if (!chunk)
    return hash;
  for (const auto& section : chunk->sections) {
    for (const auto* layer : {&section.sky_light, &section.block_light}) {
      if (!*layer) {
        hash = fnv_int(hash, -1);
        continue;
      }
      hash = fnv_int(hash, static_cast<int64_t>((*layer)->size()));
      hash = fnv1a(hash, (*layer)->data(), (*layer)->size());
    }
  }
  return hash;
}

void bench_commit() {
  const int cx = 0, cz = 0;
  const auto make_generator = [] {
    return std::make_shared<const mc::world::WgOverworldGenerator>(42);
  };
  const auto prepare = [&](mc::world::MemoryWorld& world) {
    for (int dz = -1; dz <= 1; ++dz)
      for (int dx = -1; dx <= 1; ++dx)
        world.ensure_chunk_stored(cx + dx, cz + dz);
    // Lit once, outside the timed window, so every timed call is a relight of a
    // column that is already current (the shape the light pass runs in).
    mc::world::relight_world_chunk(cx, cz, world);
  };

  // `PLUSMC_BENCH_ITERS` raises the window for a profiler (`sample` needs more
  // than a fraction of a second of flood fill to attribute anything).
  int kIters = 20;
  if (const char* text = std::getenv("PLUSMC_BENCH_ITERS"))
    kIters = std::atoi(text) > 0 ? std::atoi(text) : kIters;
  constexpr double kPasses = 3.0; // `measure` takes best-of-3, each running the body

  LockTimerWorld two_phase(make_generator());
  prepare(two_phase);
  two_phase.reset_timer();
  const Result fast = measure("commit: two-phase (copy, compute, commit)", kIters, [&] {
    for (int i = 0; i < kIters; ++i) {
      const mc::world::LightChange change = mc::world::relight_world_chunk(cx, cz, two_phase);
      g_sink = g_sink + (change.any() ? 1 : 0);
    }
  });
  const uint64_t fast_hash = hash_column_light(two_phase, cx, cz);
  std::printf("  -> two-phase: %.3f ms wall, %.3f ms under the exclusion, %.2f spans per relight\n",
              fast.ms_per_million / 1e6, two_phase.held_ms() / (kPasses * kIters),
              two_phase.spans() / (kPasses * kIters));

  NoVersionTimerWorld in_lock(make_generator());
  prepare(in_lock);
  in_lock.reset_timer();
  const Result locked = measure("commit: in-lock (the path it replaced)", kIters, [&] {
    for (int i = 0; i < kIters; ++i) {
      const mc::world::LightChange change = mc::world::relight_world_chunk(cx, cz, in_lock);
      g_sink = g_sink + (change.any() ? 1 : 0);
    }
  });
  const uint64_t locked_hash = hash_column_light(in_lock, cx, cz);
  std::printf("  -> in-lock:   %.3f ms wall, %.3f ms under the exclusion, %.2f spans per relight\n",
              locked.ms_per_million / 1e6, in_lock.held_ms() / (kPasses * kIters),
              in_lock.spans() / (kPasses * kIters));
  std::printf("  -> light hash two-phase %llu, in-lock %llu %s\n",
              static_cast<unsigned long long>(fast_hash),
              static_cast<unsigned long long>(locked_hash),
              fast_hash == locked_hash ? "(identical)" : "(DIFFER)");
}

// ---------------------------------------------------------------------------
// reload: what one `load_chunk` costs when the stored column's lazy heightmaps
// are behind its blocks. `load_chunk` hands out `Chunk::snapshot()`, and the
// light ring calls it nine times per relight (the centre column plus the eight
// neighbours, world_light.cpp) — and a copy is supposed to carry heightmaps
// current with its blocks, with the refresh landing on the copy. So a column the
// store's own readers have not yet brought current is recomputed for *every*
// copy that carries it, even though every copy recomputes the same answer.
//
// Three windows, on one real overworld column:
//   * copies of a clean column — the baseline: what the copy itself costs;
//   * copies of a column one write just marked — the same copies, plus the
//     recompute each one does for the same column;
//   * a write alone vs a write plus one height read — the recompute's own price,
//     which is what the two windows above differ by.
// ---------------------------------------------------------------------------
void bench_reload(const char* fixtures_dir) {
  const std::string dir = std::string(fixtures_dir) + "/ow3region";
  auto world = mc::world::AnvilWorld::open_region_dir(dir);
  if (!world) {
    std::printf("reload: cannot open %s\n", dir.c_str());
    return;
  }
  const int cx = -2, cz = -1;
  if (!world->can_load_chunk(cx, cz)) {
    std::printf("reload: %s has no column at (%d,%d)\n", dir.c_str(), cx, cz);
    return;
  }
  const int lx = 8, lz = 8, y = 200; // air above the terrain: a long walk down
  const std::string air = "minecraft:air";
  const std::string stone = "minecraft:stone";

  const int kIters = 20000;
  const Result clean = measure("reload: load_chunk, stored column clean", kIters, [&] {
    for (int i = 0; i < kIters; ++i) {
      const auto chunk = world->load_chunk(cx, cz);
      g_sink = g_sink + (chunk.has_value() ? 1 : 0);
    }
  });

  // One write marks the column; nothing reads it through the store afterwards,
  // which is exactly the state the light ring finds it in.
  world->set_block_state(cx, cz, lx, y, lz, BlockStateKey{stone, ""});
  const Result dirty = measure("reload: load_chunk, stored column marked", kIters, [&] {
    for (int i = 0; i < kIters; ++i) {
      const auto chunk = world->load_chunk(cx, cz);
      g_sink = g_sink + (chunk.has_value() ? 1 : 0);
    }
  });

  // The recompute on its own: the same write, with and without one height read
  // through the store afterwards (which is what brings the column current).
  const Result write_only = measure("reload: one write, no height read", kIters, [&] {
    for (int i = 0; i < kIters; ++i) {
      const std::string& s = (i & 1) != 0 ? air : stone;
      world->set_block_state(cx, cz, lx, y, lz, BlockStateKey{s, ""});
    }
  });
  const Result write_read = measure("reload: one write + one height read", kIters, [&] {
    for (int i = 0; i < kIters; ++i) {
      const std::string& s = (i & 1) != 0 ? air : stone;
      world->set_block_state(cx, cz, lx, y, lz, BlockStateKey{s, ""});
      g_sink = g_sink + world->motion_blocking_height(cx * 16 + lx, cz * 16 + lz);
    }
  });

  // ms_per_million is milliseconds per million operations, so one operation is
  // that over 1e3 us.
  const auto us = [](double ms_per_million) { return ms_per_million / 1e3; };
  std::printf("  -> per load_chunk: clean %.2f us, marked %.2f us (%+.2f us, %+.0f%%)\n",
              us(clean.ms_per_million), us(dirty.ms_per_million),
              us(dirty.ms_per_million - clean.ms_per_million),
              100.0 * (static_cast<double>(dirty.ms_per_million) /
                           static_cast<double>(clean.ms_per_million) -
                       1.0));
  std::printf("  -> per column refresh: %.2f us (write+read %.2f us - write %.2f us)\n",
              us(write_read.ms_per_million - write_only.ms_per_million),
              us(write_read.ms_per_million), us(write_only.ms_per_million));
  std::printf("  -> refreshes paid per marked column copy: %.2f\n",
              (dirty.ms_per_million - clean.ms_per_million) /
                  (write_read.ms_per_million - write_only.ms_per_million));
}

// ---------------------------------------------------------------------------
// probe: every registry state through one cell, both answers compared.
// ---------------------------------------------------------------------------
void bench_probe() {
  auto inner = std::make_shared<MemoryWorld>();
  Chunk& chunk = materialise(*inner, 0, 0);
  const Chunk::CellIndex cell = chunk.cell_index_at(8, 64, 8);
  if (cell.section < 0 || cell.palette < 0) {
    std::printf("!! probe cell has no palette entry\n");
    std::abort();
  }
  auto& palette = chunk.sections[cell.section].block_palette;

  // Warm the kinds memo (one pass over the registry) and the block lookup
  // outside the measured windows, so the probe counts the two read paths only.
  for (const mc::world::GeneratedBlockState& row : mc::world::kBlockStates) {
    const BlockStateKey key{row.name, row.props};
    g_sink = g_sink + mc::world::light_kinds(key);
  }

  // The two branches the walk above cannot reach, because they have no palette
  // entry to put a state into: a column this world does not hold, and a cell in
  // a section that carries no block container. Both answer the synthetic air
  // state, which is the one thing `light_kinds_at`'s fallbacks must do too.
  struct Fallback {
    int cx, cz, x, y, z;
    const char* what;
  };
  const Fallback fallbacks[] = {
      {9, 9, 8, 64, 8, "unheld column"},
      {0, 0, 8, 200, 8, "section with no block container"},
  };
  for (const Fallback& f : fallbacks) {
    const int kinds_old = mc::world::light_kinds(inner->get_block_state(f.cx, f.cz, f.x, f.y, f.z));
#if defined(HAVE_KINDS_AT)
    const int kinds_new = inner->light_kinds_at(f.cx, f.cz, f.x, f.y, f.z);
#else
    const int kinds_new = kinds_old;
#endif
    std::printf("probe: %-34s light_kinds_at=%d light_kinds(read)=%d %s\n", f.what, kinds_new,
                kinds_old, kinds_new == kinds_old ? "(identical)" : "(DIFFER)");
    if (kinds_new != kinds_old)
      std::abort();
  }

  uint64_t hash_new = 1469598103934665603ULL;
  uint64_t hash_old = 1469598103934665603ULL;
  uint64_t allocs_new = 0;
  uint64_t allocs_old = 0;
  std::size_t states = 0;
  std::size_t mismatches = 0;
  for (const mc::world::GeneratedBlockState& row : mc::world::kBlockStates) {
    palette[cell.palette] = BlockStateKey{row.name, row.props};

    g_allocs = 0;
    const int kinds_old = mc::world::light_kinds(inner->get_block_state(0, 0, 8, 64, 8));
    allocs_old += g_allocs;
    hash_old = fnv_int(hash_old, kinds_old);
#if defined(HAVE_KINDS_AT)
    g_allocs = 0;
    const int kinds_new = inner->light_kinds_at(0, 0, 8, 64, 8);
    allocs_new += g_allocs;
    hash_new = fnv_int(hash_new, kinds_new);
#else
    const int kinds_new = kinds_old;
#endif

    if (kinds_new != kinds_old) {
      if (mismatches < 5)
        std::printf("!! mismatch %s [%s]: light_kinds_at=%d light_kinds(read)=%d\n", row.name,
                    row.props, kinds_new, kinds_old);
      ++mismatches;
    }
    ++states;
  }
  std::printf("probe: %zu states, %zu mismatches, kinds hash new=%llu old=%llu %s\n", states,
              mismatches, static_cast<unsigned long long>(hash_new),
              static_cast<unsigned long long>(hash_old),
              hash_new == hash_old ? "(identical)" : "(DIFFER)");
  std::printf("probe: allocations over the walk: light_kinds_at %llu, read+light_kinds %llu\n",
              static_cast<unsigned long long>(allocs_new),
              static_cast<unsigned long long>(allocs_old));
}

} // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "all";
  const bool all = mode == "all";
  std::printf("== world read path bench (%s) ==\n", mode.c_str());
  if (all || mode == "reads") {
    std::printf("-- reads --\n");
    bench_reads();
  }
  if (all || mode == "solid") {
    std::printf("-- collision predicate's read --\n");
    bench_solid();
  }
  if (all || mode == "loadchunk") {
    std::printf("-- column-existence read --\n");
    bench_load_chunk(argc > 2 ? argv[2] : "tests/fixtures");
  }
  if (all || mode == "relight") {
    std::printf("-- whole-chunk light recompute --\n");
    bench_relight(argc > 2 ? argv[2] : "tests/fixtures");
  }
  if (all || mode == "ring") {
    std::printf("-- relight with the 3x3 ring present --\n");
    bench_ring(argc > 2 ? argv[2] : "tests/fixtures");
  }
  if (all || mode == "commit") {
    std::printf("-- two-phase relight vs in-lock --\n");
    bench_commit();
  }
  if (all || mode == "scratch") {
    std::printf("-- light region scratch --\n");
    bench_scratch();
  }
  if (all || mode == "reload") {
    std::printf("-- column copy with lazy heightmaps behind --\n");
    bench_reload(argc > 2 ? argv[2] : "tests/fixtures");
  }
  if (all || mode == "fluidtick") {
    std::printf("-- fluid tick reads --\n");
    bench_fluid_tick();
  }
  if (all || mode == "gate") {
    std::printf("-- gate state read, old vs new --\n");
    bench_gate();
  }
  if (all || mode == "gatepath") {
    std::printf("-- whole write gate, paired place/revert --\n");
    bench_gate_paths();
  }
  if (all || mode == "writes") {
    std::printf("-- write gate reference hashes --\n");
    bench_writes();
  }
  if (all || mode == "probe") {
    std::printf("-- registry-wide kinds equivalence --\n");
    bench_probe();
  }
  std::printf("(sink %lld)\n", static_cast<long long>(g_sink));
  return 0;
}
