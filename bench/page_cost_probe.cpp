// SPDX-License-Identifier: GPL-3.0-or-later
//
// What one column's cell arrays cost to build and drop, from the shared heap and
// from `core/page_alloc.h`'s page-granular allocator. The allocator's header
// quotes this probe's numbers for the price of the mapping it buys, so the
// instrument is checked in beside `bench/soak.py` rather than left in /tmp:
//
//   clang++ -O2 -std=c++17 bench/page_cost_probe.cpp -o /tmp/page_cost_probe
//   /tmp/page_cost_probe              # 512 columns, 5 passes, ~190 MB per pass
//   /tmp/page_cost_probe 1024 7       # more columns, more passes
//
// It is not part of the build and nothing asserts on its output; it exists so a
// reader can re-derive the trade on their own machine. Report the median, not a
// single pass: the figures move by ~2x with whatever else the box is doing, and
// the *difference* between the two arms is the number the allocator's header
// cares about.
//
// The shape is the store's: a column is 24 sections (the overworld's default
// height), each section's `block_cells` is 4096 int32 cells = 16 KiB, written in
// full as the parse does, and the column is dropped when its id is evicted.
// Two arms:
//
//   heap    operator new/delete for each 16 KiB buffer (the pre-change shape)
//   mapped  mmap/munmap per buffer, pages written          (the shipped shape)
//
// Each arm is run `passes` times after a warm-up, and every pass allocates and
// drops the whole set, so the heap arm is measured reusing free-list blocks on
// pages malloc already owns (its steady state, and the reason it is cheap) while
// the mapped arm re-faults every page it gave back. That asymmetry is the trade,
// not a measurement artifact.
//
// A third arm (mmap/munmap without touching a page) is reported separately: it
// is the mapping's fixed syscall cost per column, with no write and no fault, so
// it is comparable to neither of the above and is not subtracted from either.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kSections = 24;                                       // overworld column height
constexpr int kCells = 4096;                                        // 16x16x16 block cells
constexpr std::size_t kBufferBytes = kCells * sizeof(std::int32_t); // 16 KiB

std::size_t page_bytes() {
  static const std::size_t k = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
  return k;
}

std::size_t mapped_bytes() {
  const std::size_t page = page_bytes();
  return (kBufferBytes + page - 1) / page * page;
}

double now_us() {
  using clock = std::chrono::steady_clock;
  static const clock::time_point start = clock::now();
  return std::chrono::duration<double, std::micro>(clock::now() - start).count();
}

enum class Shape { kHeap, kMapped, kUntouched };

// `touch` writes every cell, the way `unpack_cells` fills them. The untouched
// shape never writes, so its pages are never faulted in.
template <Shape kShape> double run_once(int columns) {
  const std::size_t bytes = kShape == Shape::kHeap ? kBufferBytes : mapped_bytes();
  std::vector<std::int32_t*> live(static_cast<std::size_t>(columns) * kSections);
  const double begin = now_us();
  for (int c = 0; c < columns; ++c) {
    for (int s = 0; s < kSections; ++s) {
      std::int32_t* p = nullptr;
      if (kShape == Shape::kHeap) {
        p = static_cast<std::int32_t*>(::operator new(kBufferBytes));
      } else {
        void* m =
            ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) {
          std::perror("mmap");
          std::exit(1);
        }
        p = static_cast<std::int32_t*>(m);
      }
      if (kShape == Shape::kUntouched) {
        live[static_cast<std::size_t>(c) * kSections + s] = p;
        continue;
      }
      for (int i = 0; i < kCells; ++i)
        p[i] = (i * 31 + c + s) & 0xff;
      live[static_cast<std::size_t>(c) * kSections + s] = p;
    }
  }
  const double built = now_us();
  for (std::int32_t* p : live) {
    if (kShape == Shape::kHeap)
      ::operator delete(p);
    else
      ::munmap(p, bytes);
  }
  const double dropped = now_us();
  return ((dropped - begin) / columns) * 1.0;
}

template <Shape kShape> double median_of(int columns, int passes) {
  run_once<Shape::kMapped>(16); // warm: the process's own startup, not the shape
  std::vector<double> samples;
  for (int i = 0; i < passes; ++i)
    samples.push_back(run_once<kShape>(columns));
  std::sort(samples.begin(), samples.end());
  std::printf("  passes:");
  for (double s : samples)
    std::printf(" %.1f", s);
  const double median = samples[samples.size() / 2];
  std::printf("  -> median %.1f us/col\n", median);
  return median;
}

} // namespace

int main(int argc, char** argv) {
  const int columns = argc > 1 ? std::atoi(argv[1]) : 512;
  const int passes = argc > 2 ? std::atoi(argv[2]) : 5;
  if (columns <= 0 || passes <= 0) {
    std::fprintf(stderr, "usage: %s [columns] [passes]\n", argv[0]);
    return 2;
  }
  std::printf("page_cost_probe: %d columns x %d sections x %zu B (%zu B mapped), page %zu B\n",
              columns, kSections, kBufferBytes, mapped_bytes(), page_bytes());
  std::printf("heap, pages written    :\n");
  const double heap = median_of<Shape::kHeap>(columns, passes);
  std::printf("mapped, pages written  :\n");
  const double mapped = median_of<Shape::kMapped>(columns, passes);
  std::printf("mapped, pages untouched:\n");
  const double untouched = median_of<Shape::kUntouched>(columns, passes);
  std::printf("\nprice of the mapping: +%.1f us per column built and dropped (%.0f%%)\n",
              mapped - heap, (mapped - heap) / heap * 100.0);
  std::printf("the mapping's syscalls alone: %.1f us per column (%.1f us per mmap/munmap pair)\n",
              untouched, untouched / kSections);
  return 0;
}
