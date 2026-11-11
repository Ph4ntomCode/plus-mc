// SPDX-License-Identifier: GPL-3.0-or-later
//
// Page-granular storage for the world store's bulk column buffers.
//
// Why this exists (measured, docs/PHASE5.md §4): an idle unload of 4032 columns
// drops the process's `phys_footprint` from 1887 MB to 57 MB and the resident
// set only from 1893 MB to 1624 MB. The columns *are* freed — the charge falls
// by the whole 1.8 GB — but `free()` hands the blocks back to the allocator, not
// to the kernel: the pages stay mapped and counted as resident, and the kernel
// only takes them when it wants them. Nothing in the process can push a
// reusable page out of RSS, which is why the release line says "allocator handed
// back 0 MB" after calling the platform's own gesture.
//
// Two probes isolate the cause. A standalone probe of the same allocation shape
// — 4096 columns x 28 fully touched 16 KiB blocks, allocated and then freed as a
// contiguous run — does return, RSS 1890 -> 39 MB on `free()` alone. Freeing the
// same blocks when the heap also holds interleaved live allocations, or after
// the store's own load/evict churn has scattered them, returns almost nothing:
// the blocks of a released column are spread across regions that each still hold
// live data, and a region is the smallest thing the allocator can give back.
//
// So a column's bulk buffers are the one thing that must not come from a shared
// heap. An allocation of a page or more here is its own anonymous mapping, and
// `deallocate` munmaps it: whole pages leave the process's resident set at the
// moment the column is released, whatever else the heap is holding, and the
// allocator's free lists can no longer interleave them with anything.
//
// The cost is CPU rather than bytes: one `mmap`/`munmap` pair and a first-touch
// fault on each page, instead of a free-list walk, and a page of slack when a
// buffer is not a whole number of pages (a 16 KiB one is exactly four pages and
// pays none; at eight bits a cell array is 4 KiB and pays none either). Buffers
// *below* a page — the 2048-byte light arrays, the four-bit cell arrays that
// most generated sections have, the 256-byte biome cells, a palette's own array
// — keep the default allocator: mapping half a page per light array would cost a
// whole page of resident set for each of them, which is worse than what they
// leave behind. `docs/PHASE5.md` §4 records what that leaves on the table, and
// measures the consequence: the packed cell container moved the common width
// below the cutoff, so on generated terrain this allocator maps nothing at all
// and the store's remaining resident set is the heap's share (light arrays,
// palettes, biome cells). The cutoff stays at one page because a 2 KiB mapping
// would double that buffer's resident cost to return it.
#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>
#include <type_traits>

// Which platforms have the page-granular backing at all. The implementation
// deliberately lives in `core/page_alloc.cpp`, not here: this header reaches
// most of the engine (through `world/mob.h`, `world/chunk.h`, ...), and
// `<windows.h>` defines `near`, `far`, `min` and `max` as macros that would
// break ordinary identifiers in every one of those translation units.
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__) || defined(__unix__)
#define PLUSMC_PAGE_ALLOC_MAPPED 1
#endif

namespace mc::core {

// `bytes` rounded up to a whole number of pages.
inline std::size_t round_up_to_page(std::size_t bytes, std::size_t page) {
  return (bytes + page - 1) / page * page;
}

// One anonymous, private, zero-filled run of pages, and its release. The two
// platforms promise the same thing through different spellings: the pages are
// not part of the shared heap, and releasing returns them to the operating
// system at once. `nullptr` means the mapping could not be made (the caller
// turns that into `bad_alloc` or "cannot grow"); `bytes` is already rounded to
// `page_bytes()`.
//
// Windows keeps the same shape with `VirtualAlloc`/`VirtualFree`: the commit is
// the requested size, the reservation is rounded up to the allocation
// granularity (64 KiB), and `MEM_RELEASE` frees the whole reservation. Only the
// committed pages count against the working set, which is the number the release
// line reports.
#if defined(PLUSMC_PAGE_ALLOC_MAPPED)
void* map_pages(std::size_t bytes) noexcept;
void unmap_pages(void* p, std::size_t bytes) noexcept;
std::size_t system_page_bytes() noexcept;
#endif

// The system's page size, and the size every mapping here is rounded to. Asked
// once: the answer cannot change under a running process, and `round_up_to_page`
// is on the hot path of `allocate`/`deallocate`.
inline std::size_t page_bytes() {
#if defined(PLUSMC_PAGE_ALLOC_MAPPED)
  static const std::size_t kPage = system_page_bytes();
  return kPage;
#else
  return 4096;
#endif
}

// The cutoff: an allocation of this size or more is its own mapping. One page by
// default; `PLUSMC_PAGE_ALLOC_MIN_KB` raises it (whole KiB, 0 = the default), and
// raising it past 4 KiB keeps every packed cell array (4 KiB at eight bits,
// 16 KiB at 32) on the shared heap entirely. That knob exists because the return
// is not free, and the price is measured by `bench/page_cost_probe.cpp` (checked
// in, and re-runnable on any machine): building and dropping a column's 24 cell
// arrays costs 310 us from the heap against 442 us with a mapping each on a
// moderately loaded box (+132 us, +43%), and 252 against 953 us (+278%) on a
// busier one. Of the first pair's gap, ~30 us is the 24 `mmap`/`munmap` pairs
// (measured with the pages never touched — 1.2-1.3 us a pair) and the rest is
// first-touch faults, because the pages the previous drop returned have to be
// faulted in again to be reused, against a free-list hit on a page malloc already
// owns and never gave back. The direction never moves, only the multiple: the
// heap gets relatively cheaper as it settles and the faults get dearer as memory
// pressure rises, so quote the range (1.4x-3.8x) rather than a constant. A
// per-column mapping instead of per-section would remove the syscall share and
// keep the faults, so granularity is not the lever: the heap is cheaper in CPU
// precisely because it is not returning the frames. A host that would rather
// spend CPU than RSS can turn the mapping off without a rebuild; the default is
// on because the trade is only worth making where the store actually needs the
// frames back, and `docs/PHASE5.md` §4 measures where that is.
//
// Read once, into a function-local static, and never re-read: an allocation and
// the deallocation that frees it must see the same predicate, or the wrong one of
// the two paths would be taken (and `deallocate` has no other way to tell).
inline std::size_t page_mapping_min_bytes() {
  static const std::size_t kMin = [] {
    const std::size_t fallback = page_bytes();
    const char* text = std::getenv("PLUSMC_PAGE_ALLOC_MIN_KB");
    if (text == nullptr || *text == '\0')
      return fallback;
    char* end = nullptr;
    const long kib = std::strtol(text, &end, 10);
    // Unparsable, negative and "0" all mean the default; anything above a GiB is
    // a cutoff no allocation reaches, i.e. the mapping is off.
    if (end == text || kib < 0 || kib == 0)
      return fallback;
    return static_cast<std::size_t>(kib > (1L << 20) ? (1L << 20) : kib) * 1024;
  }();
  return kMin;
}

// A `std::allocator`-shaped allocator that answers allocations of one page or
// more with a private anonymous mapping and releases them with `unmap_pages`
// (VirtualAlloc/VirtualFree on Windows, mmap/munmap elsewhere; see the note
// above). Stateless, so every instance compares equal and containers built from
// it move, swap and copy-assign without rebinding.
//
// The class has to know, at `deallocate` time, which of its two paths an earlier
// `allocate` took. It recomputes the predicate from the byte count instead of
// storing a header: a container must pass `deallocate` the same `n` it was given
// by `allocate`, and both sides read the same page size, so the two can never
// disagree. That keeps the user pointer page-aligned (no header to skip) and
// costs one multiply and a compare on the way out.
template <typename T> class PageAllocator {
public:
  using value_type = T;
  using is_always_equal = std::true_type;

  PageAllocator() noexcept = default;
  template <typename U> PageAllocator(const PageAllocator<U>&) noexcept {}

  [[nodiscard]] T* allocate(std::size_t n) {
    const std::size_t bytes = n * sizeof(T);
    if (owns_mapping(bytes)) {
#if defined(PLUSMC_PAGE_ALLOC_MAPPED)
      // Anonymous and private: zero-filled pages, no file, and nothing else in
      // the process can see them. The mapping's length is what the release
      // will be handed back, so it is the rounded figure rather than `bytes`.
      if (void* p = map_pages(round_up_to_page(bytes, page_bytes())))
        return static_cast<T*>(p);
#endif
      throw std::bad_alloc();
    }
    return static_cast<T*>(small_new(bytes));
  }

  void deallocate(T* p, std::size_t n) noexcept {
    if (p == nullptr)
      return;
    const std::size_t bytes = n * sizeof(T);
    if (owns_mapping(bytes)) {
#if defined(PLUSMC_PAGE_ALLOC_MAPPED)
      unmap_pages(p, round_up_to_page(bytes, page_bytes()));
      return;
#endif
    }
    small_delete(p);
  }

private:
  // The one predicate, read by both paths: is this allocation big enough that a
  // page of its own is worth having?
  static bool owns_mapping(std::size_t bytes) {
    return bytes >= page_mapping_min_bytes();
  }

  // Over-aligned types cannot use the plain `operator new` pair, and the mapping
  // path above is already page-aligned whatever the type asks for.
  static void* small_new(std::size_t bytes) {
#if defined(__cpp_aligned_new)
    if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
      return ::operator new(bytes, std::align_val_t{alignof(T)});
#endif
    return ::operator new(bytes);
  }

  static void small_delete(T* p) noexcept {
#if defined(__cpp_aligned_new)
    if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
      ::operator delete(p, std::align_val_t{alignof(T)});
      return;
    }
#endif
    ::operator delete(p);
  }
};

template <typename T, typename U>
bool operator==(const PageAllocator<T>&, const PageAllocator<U>&) noexcept {
  return true;
}

template <typename T, typename U>
bool operator!=(const PageAllocator<T>&, const PageAllocator<U>&) noexcept {
  return false;
}

// Whether a buffer of `bytes` comes from this allocator's own mapping rather
// than the shared heap — the predicate `allocate`/`deallocate` agree on, exposed
// for the startup line that reports the cutoff and for callers that want to say
// which side of it they are on.
inline bool buffer_is_page_mapped(std::size_t bytes) {
  return bytes >= page_mapping_min_bytes();
}

} // namespace mc::core
