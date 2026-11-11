// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tick accounting — see core/metrics.h for what each number means and why the
// loop reports it. Nothing here touches the world or the sim: the caller passes
// in what the loop already measured, so the module stays testable in isolation
// and the loop stays the only place that knows about the clock.
#include "core/metrics.h"

#include <chrono>
#include <cstdio>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <malloc/malloc.h>
#elif defined(__linux__)
#include <unistd.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// windows.h has to precede psapi.h: psapi's prototypes use WINBOOL/DWORD from
// the former. A blank line keeps clang-format from sorting them into the wrong
// order (`IncludeBlocks: Preserve` sorts each block, not across blocks).
#include <windows.h>

#include <psapi.h>
#endif

namespace mc::metrics {
namespace {
constexpr double kTickMs = static_cast<double>(kTickUs) / 1000.0;
} // namespace

void Counter::record(int64_t ticks_run, int64_t work_us, int64_t sleep_us, int64_t wall_us,
                     bool drop_backlog) {
  ++loops;
  if (ticks_run > 0)
    ticks += static_cast<uint64_t>(ticks_run);
  if (drop_backlog)
    ++resets;
  wall_ms += static_cast<double>(wall_us) / 1000.0;
  work_ms += static_cast<double>(work_us) / 1000.0;
  sleep_ms += static_cast<double>(sleep_us) / 1000.0;
  const double per_tick_ms =
      ticks_run > 0 ? static_cast<double>(work_us) / 1000.0 / static_cast<double>(ticks_run) : 0.0;
  if (per_tick_ms > work_ms_peak)
    work_ms_peak = per_tick_ms;
}

Summary summarise(const Counter& window, uint64_t ticks_total, double uptime_s) {
  Summary s;
  s.ticks_total = ticks_total;
  s.resets = window.resets;
  s.uptime_s = uptime_s;
  if (window.wall_ms > 0) {
    s.tps = static_cast<double>(window.ticks) / (window.wall_ms / 1000.0);
    s.busy = window.work_ms / window.wall_ms;
  }
  if (window.ticks > 0)
    s.work_ms_avg = window.work_ms / static_cast<double>(window.ticks);
  s.work_ms_peak = window.work_ms_peak;
  s.tick_load = s.work_ms_avg / kTickMs;
  return s;
}

void reset(Counter& window) {
  window = Counter{};
}

std::string format_line(const Summary& s, int players, uint64_t rss_bytes,
                        const WorldStore& world) {
  char line[512];
  std::snprintf(line, sizeof(line),
                "metrics: tps=%.2f work_avg_ms=%.3f work_peak_ms=%.3f busy_pct=%.1f "
                "tick_pct=%.1f resync=%llu players=%d rss_bytes=%llu uptime_s=%.0f "
                "ticks=%llu world_chunks=%llu world_bytes=%llu world_written_chunks=%llu "
                "world_chunks_stored=%llu",
                s.tps, s.work_ms_avg, s.work_ms_peak, s.busy * 100.0, s.tick_load * 100.0,
                static_cast<unsigned long long>(s.resets), players,
                static_cast<unsigned long long>(rss_bytes), s.uptime_s,
                static_cast<unsigned long long>(s.ticks_total),
                static_cast<unsigned long long>(world.columns),
                static_cast<unsigned long long>(world.bytes),
                static_cast<unsigned long long>(world.written_columns),
                static_cast<unsigned long long>(world.stored_columns));
  return std::string(line);
}

std::string format_json_line(const Summary& s, int players, uint64_t rss_bytes, int64_t ts_unix,
                             const WorldStore& world) {
  // One JSON object, no nesting, no whitespace: a JSON-lines record a host can
  // append to and tail. The field names and the precision are format_line's, so
  // the two forms of one window carry the same numbers (the file is the export,
  // the stderr line is the harness's parser input). Two fields are the file's
  // own: `ts_unix` (the wall clock, which `uptime_s` deliberately cannot give)
  // and `v` (the format version, `kJsonFormatVersion`) — every record is
  // written to satisfy docs/metrics.schema.json, which is the published
  // contract for a consumer other than `tail -f`.
  char line[768];
  std::snprintf(
      line, sizeof(line),
      "{\"v\":%d,\"ts_unix\":%lld,\"tps\":%.2f,\"work_avg_ms\":%.3f,"
      "\"work_peak_ms\":%.3f,"
      "\"busy_pct\":%.1f,\"tick_pct\":%.1f,\"resync\":%llu,\"players\":%d,"
      "\"rss_bytes\":%llu,\"uptime_s\":%.0f,\"ticks\":%llu,"
      "\"world_chunks\":%llu,\"world_bytes\":%llu,\"world_written_chunks\":%llu,"
      "\"world_chunks_stored\":%llu}",
      kJsonFormatVersion, static_cast<long long>(ts_unix), s.tps, s.work_ms_avg, s.work_ms_peak,
      s.busy * 100.0, s.tick_load * 100.0, static_cast<unsigned long long>(s.resets), players,
      static_cast<unsigned long long>(rss_bytes), s.uptime_s,
      static_cast<unsigned long long>(s.ticks_total),
      static_cast<unsigned long long>(world.columns), static_cast<unsigned long long>(world.bytes),
      static_cast<unsigned long long>(world.written_columns),
      static_cast<unsigned long long>(world.stored_columns));
  return std::string(line);
}

int64_t monotonic_us() {
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
}

uint64_t resident_set_bytes() {
#if defined(__APPLE__)
  mach_task_basic_info_data_t info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                &count) != KERN_SUCCESS)
    return 0;
  return static_cast<uint64_t>(info.resident_size);
#elif defined(__linux__)
  // /proc/self/statm reports pages; field 2 is the resident set.
  std::FILE* f = std::fopen("/proc/self/statm", "r");
  if (f == nullptr)
    return 0;
  unsigned long total_pages = 0, resident_pages = 0;
  const int read = std::fscanf(f, "%lu %lu", &total_pages, &resident_pages);
  std::fclose(f);
  if (read != 2)
    return 0;
  const long page_size = ::sysconf(_SC_PAGESIZE);
  if (page_size <= 0)
    return 0;
  return static_cast<uint64_t>(resident_pages) * static_cast<uint64_t>(page_size);
#elif defined(_WIN32)
  // The working set is the resident set's Windows name: the pages currently
  // backed by physical memory. `GetProcessMemoryInfo` needs psapi.lib, which
  // src/CMakeLists.txt links on WIN32 only.
  PROCESS_MEMORY_COUNTERS counters{};
  if (!::GetProcessMemoryInfo(::GetCurrentProcess(), &counters,
                              static_cast<DWORD>(sizeof(counters))))
    return 0;
  return static_cast<uint64_t>(counters.WorkingSetSize);
#else
  return 0; // an unknown platform: the loop simply omits RSS.
#endif
}

uint64_t physical_footprint_bytes() {
#if defined(__APPLE__)
  // `phys_footprint` is the charged-memory figure (dirty + compressed + iokit
  // mappings, minus pages already marked reusable), which is why it is the right
  // second opinion next to the resident set: it is what the process still costs
  // the system after a free, and RSS is what still occupies physical frames.
  task_vm_info_data_t info;
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
      KERN_SUCCESS)
    return 0;
  return static_cast<uint64_t>(info.phys_footprint);
#else
  // /proc has no footprint equivalent; RSS is the kernel's charge there.
  // Windows has no separate charged-memory figure either: the working set is
  // the number a host watches, so the footprint stays 0 rather than reporting
  // commit charge under a name that would mean something else.
  return 0;
#endif
}

PageRelease release_free_pages() {
  PageRelease result;
  result.rss_before = resident_set_bytes();
  result.footprint_before = physical_footprint_bytes();
#if defined(__APPLE__)
  // A null zone means every zone and a zero goal means "as much as you can".
  // The return value is the gesture's own count of what it handed the system.
  result.allocator_bytes = static_cast<uint64_t>(malloc_zone_pressure_relief(nullptr, 0));
#elif defined(__linux__) && defined(__GLIBC__)
  // The zero argument is the number of bytes to keep at the top of the heap:
  // 0 means "trim all you can". `malloc_trim` only answers yes/no, so the
  // allocator side is left at zero and the RSS delta is the evidence. musl has
  // no equivalent, which is why the guard is glibc rather than linux.
  malloc_trim(0);
#elif defined(_WIN32)
  // `EmptyWorkingSet` is the platform's "hand the rebuildable pages back"
  // gesture. Like pressure relief it only answers yes/no, so the RSS delta is
  // the evidence; unlike pressure relief it can also push pages that are still
  // hot to the standby list, which is why the idle-release caller is the only
  // user (nobody is connected when it runs).
  (void)::EmptyWorkingSet(::GetCurrentProcess());
#endif
  result.rss_after = resident_set_bytes();
  result.footprint_after = physical_footprint_bytes();
  result.rss_bytes =
      result.rss_after < result.rss_before ? result.rss_before - result.rss_after : 0;
  return result;
}

} // namespace mc::metrics
