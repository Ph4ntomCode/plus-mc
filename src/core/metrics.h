// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tick accounting for the server loop — the first thing Phase 5 needs, because
// every performance target in `docs/PLAN.md` §8 is phrased in ticks and
// milliseconds and none of those numbers existed: the loop in `main.cpp` slept
// a flat 50 ms and never looked at what a tick cost.
//
// What is measured, and why each number is the one that matters:
//
//   * **ticks per second, over the window.** The loop turns wall time into
//     whole 50 ms ticks; whether the process actually *runs* that many is the
//     "steady 20.0 TPS" target. On its first run this reporter showed the old
//     fixed-sleep shape at **19.70 TPS idle** rather than 20.0 — the period was
//     `sleep(50 ms) + work`, so every tick the work cost was time the clock
//     never got back, and the deficit accumulated into catch-up bursts (loaded
//     windows spread 19.68..20.51). `docs/PHASE5.md` has the before/after.
//   * **work per tick, average and peak.** The sim's own cost with the sleep
//     excluded: average is what a server operator calls `mspt` (the plan's
//     "never sustained above ~45 ms" is a statement about this number), and the
//     peak is what a player feels as a stutter.
//   * **busy percent** — work as a fraction of one core across the window.
//     This is the headroom number: it says how much of a single sim thread is
//     left before the region parallelism of §6.2 stops being optional.
//   * **resync count** — how often the loop dropped a backlog instead of
//     stacking catch-up ticks onto an already-late tick (see `Counter::record`).
//     Non-zero is the honest sign that the machine, not the code, is the limit.
//
// Resident set size comes along because §8's memory target ("stable long-run
// RSS") is otherwise only measurable with an external profiler.
#pragma once

#include <cstdint>
#include <string>

namespace mc::metrics {

// The timestep the whole server is built on (vanilla's 20 TPS).
constexpr int64_t kTickUs = 50000;

// The version of the JSON-lines export (`format_json_line`). It is written as
// the first key of every record and it is what `docs/metrics.schema.json`
// publishes, so a consumer can say which format it read without guessing from
// the key set. The bumping rule is the schema's own, written down there too: a
// **rename, removal or retype** of a field bumps this; **adding** a key does
// not, because a reader that ignores keys it does not know keeps working (that
// is how `ts_unix` arrived), and neither does a change to a value's precision.
constexpr int kJsonFormatVersion = 1;

// One reporting window's counters. `main.cpp` owns the instance and resets it
// once a window has been reported.
struct Counter {
  uint64_t loops = 0;      // loop iterations
  uint64_t ticks = 0;      // whole ticks executed
  uint64_t resets = 0;     // backlogs dropped (see `record`)
  double wall_ms = 0;      // wall time the iterations covered
  double work_ms = 0;      // time inside the tick work, across all ticks
  double work_ms_peak = 0; // the worst single tick's work
  double sleep_ms = 0;     // time waiting for the next tick to come due

  // One loop iteration: `ticks_run` whole ticks ran in `work_us`, the loop
  // waited `sleep_us` for them to come due, and the iteration itself took
  // `wall_us`. `drop_backlog` says the loop resynced its deadline because it
  // was further behind than its catch-up limit — a tick that was skipped rather
  // than run late, which is why it is counted instead of hidden.
  void record(int64_t ticks_run, int64_t work_us, int64_t sleep_us, int64_t wall_us,
              bool drop_backlog);
};

struct Summary {
  double tps = 0;          // ticks / wall seconds
  double work_ms_avg = 0;  // per tick
  double work_ms_peak = 0; // worst tick in the window
  double busy = 0;         // work / wall: the fraction of one core the sim used
  double tick_load = 0;    // work_ms_avg / 50 ms: how full one tick is
  double uptime_s = 0;
  uint64_t ticks_total = 0; // since start, not just this window
  uint64_t resets = 0;      // this window
};

// What the world store is holding, as the store itself accounts for it
// (`WorldInterface::resident_columns`/`resident_bytes`/`columns_with_writes`), a
// gauge sampled at report time rather than a per-window counter. It is on the
// line because the store's ceiling is a host knob and its occupancy is the only
// way to tune it: RSS says the process is 2 GB, this says whether that is 4096
// streamed columns or a leak, and `world_written_chunks` says how much of it is
// world state the store may not give back. A world with no column cache (a
// disk-backed one) reports zeros.
//
// `stored_columns` is the one cumulative number here (`WorldInterface::
// columns_stored`): insertions since the store was constructed, not what it holds
// now. The two differ whenever the budget evicts, and that is exactly when the
// resident gauge stops being a measure of the fill: a cell near its ceiling can
// store 1,500 columns and never show more than the ceiling. Throughput readers
// (`bench/soak.py`'s generation block) take this counter and only fall back to
// `world_chunks` when a server predates it.
struct WorldStore {
  uint64_t columns = 0;         // resident columns, right now
  uint64_t bytes = 0;           // their estimated own bytes (Chunk::estimated_bytes)
  uint64_t written_columns = 0; // of those, the ones carrying a write
  uint64_t stored_columns = 0;  // columns stored since start (cumulative)
};

// `window` is the counters to summarise; `ticks_total` is the process-wide tick
// count the caller keeps (so the line says how far into a run it is) and
// `uptime_s` is seconds since the loop started. `world` is the store's gauge and
// defaults to zeroed, so a caller that has no world (or a test) is unaffected.
Summary summarise(const Counter& window, uint64_t ticks_total, double uptime_s);

// Zero a window's counters (the totals stay the caller's business).
void reset(Counter& window);

// The reporter's own line, on stderr like every other log line so stdout stays
// machine-readable. The `key=value` pairs **are** the interface `bench/soak.py`
// and `docs/PHASE5.md` parse — adding a key is fine, renaming one is not:
//
//   metrics: tps=20.00 work_avg_ms=0.421 work_peak_ms=1.104 busy_pct=0.8
//            tick_pct=0.8 resync=0 players=2 rss_bytes=33013760 uptime_s=15
//            ticks=301 world_chunks=1204 world_bytes=566231040
//            world_written_chunks=0 world_chunks_stored=1204
std::string format_line(const Summary& s, int players, uint64_t rss_bytes,
                        const WorldStore& world = {});

// The same window as one JSON object (`--metrics-file`, Phase-5 item 8's metrics
// export). This is the consumer side of the line above: identical field names
// and identical rounding, so a forwarder's numbers and a `bench/soak.py` run's
// numbers are comparable without a conversion table. `ts_unix` is the only
// addition — the wall clock as whole seconds since the epoch, which a log
// shipper needs and `uptime_s` cannot give it. (`uptime_s` stays monotonic; the
// tick budget still never touches the wall clock.)
//
//   {"v":1,"ts_unix":1760000000,"tps":20.00,"work_avg_ms":0.421,...,"ticks":301}
//
// `v` is `kJsonFormatVersion` (see above) and `docs/metrics.schema.json` is the
// published schema these lines are written to satisfy.
std::string format_json_line(const Summary& s, int players, uint64_t rss_bytes, int64_t ts_unix,
                             const WorldStore& world = {});

// Microseconds from a monotonic clock (never the wall clock: a tick budget
// must not move when the system clock is adjusted).
int64_t monotonic_us();

// Resident set size of this process in bytes, or 0 when the platform has no
// answer. OS-touching code stays small and isolated (PLAN §3.5): mach on macOS,
// /proc on Linux.
uint64_t resident_set_bytes();

// The process's own "memory in use" figure, as this platform charges it. On
// macOS that is `phys_footprint` — the number Activity Monitor shows, which
// excludes pages the allocator has already marked reusable and counts compressed
// ones — and on Linux it is 0, because /proc has no footprint equivalent: RSS
// *is* the kernel's charge there, so `resident_set_bytes` already answers it.
//
// It exists next to `resident_set_bytes` because the two disagree exactly when it
// matters: after a large free plus the allocator gesture below, macOS can report
// a resident set that has not moved while the footprint falls by gigabytes (the
// pages are reusable and will be reused, but they are no longer charged to the
// process). Measuring "what the store gave back to the OS" with RSS alone would
// read as "nothing" in that case.
uint64_t physical_footprint_bytes();

// What one call to the platform's allocator gesture did, from both sides: the
// allocator's own count of what it handed back, and the process's memory
// accounting either side of the call.
struct PageRelease {
  uint64_t allocator_bytes = 0; // the gesture's own count (0 where it has none)
  uint64_t rss_bytes = 0;       // resident set before - after
  uint64_t rss_before = 0;
  uint64_t rss_after = 0;
  uint64_t footprint_before = 0; // `physical_footprint_bytes`; 0 where unknown
  uint64_t footprint_after = 0;
};

// Asks the allocator to give the pages it is holding but no longer using back to
// the operating system, and answers what that did (see `PageRelease`).
//
// It exists because "freed" and "given back" are two different things: a chunk
// column is hundreds of KB of many small allocations, and `free` hands those
// blocks back to the allocator, not to the kernel — the allocator keeps the pages
// in its arenas until something asks. macOS has `malloc_zone_pressure_relief`,
// glibc has `malloc_trim`; the platform with neither does nothing and reports
// zeros. Note what the two shapes measure: freeing a *contiguous run* of blocks
// is returned by `free` alone (measured, 1.8 GB of 16 KiB blocks freed → RSS 1890
// → 39 MB with the gesture contributing nothing), while a heap whose free blocks
// are interleaved with live ones gives back only what whole regions can be
// released — which is the fragmentation story the at-scale unload measurement in
// docs/PHASE5.md §4 turns on. So this is worth calling after a large release, and
// there is no promise it finds anything.
//
// Note what the store no longer puts here: a column's cell arrays — the bulk of
// one — are mappings of their own now (`core/page_alloc.h`), unmapped as the
// column is erased, so those frames are already gone by the time this runs and
// what is left for the gesture is the heap's share of a column (light arrays,
// palettes, biome cells, block entities) plus everything else the process
// streamed. The call stays because that share is real — it is what the line's
// `allocator handed back` half reports — but the resident-set delta this is
// usually read for is now mostly the erase above it.
PageRelease release_free_pages();

} // namespace mc::metrics
