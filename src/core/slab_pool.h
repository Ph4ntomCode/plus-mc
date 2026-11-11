// SPDX-License-Identifier: GPL-3.0-or-later
//
// 2 KiB slots from dedicated 4 KiB pages — the storage behind a section's light
// arrays.
//
// Why this and not `core/page_alloc.h` directly (measured, docs/PHASE5.md §4): a
// light array is 2,048 bytes, half a page, so mapping one array on its own spends
// a whole page to hold half of one and would *raise* the store's resident set
// while it is loaded. But the array is 57% of what a packed column still leaves
// on the heap (34 KB of a ~60 KB column, 17 arrays on a generated column), and
// the release path cannot give those frames back — `free()` returns a hole in a
// region the allocator keeps, exactly the shape `core/page_alloc.h` was written
// for. So the arrays need a mapping with *no* waste and a whole-page return,
// which means sharing a page: two arrays per page, and the page is unmapped when
// both slots are free.
//
// That is a slab, and the trade is asymmetric on purpose:
//
//  - While loaded, resident bytes are what 2 KiB heap blocks were (4 KiB per two
//    arrays, no slack) — the slab never costs more than the heap it replaces.
//  - At release, pages whose two slots are *both* free are unmapped at once. A
//    page shared by a released column's array and a surviving column's array
//    stays mapped, which is the price of sharing: the release returns the pages
//    whose halves were both released, not every array unconditionally.
//
// The pairing is invisible to the caller. `LightSlabAllocator<T>` is a
// `std::allocator`-shaped allocator for `std::vector`, so a light array is still
// a `std::vector<uint8_t>` in every signature and every byte is at the same
// offset; only `allocate`/`deallocate` know about pages. Sizes other than one
// slot (a vector that grows past 2,048 bytes) fall back to `operator new`, so the
// container's other operations keep working.
//
// `PLUSMC_LIGHT_ALLOC=0` turns the pool off without a rebuild — the same A/B the
// cell allocator has — and both sides of `allocate`/`deallocate` read that
// predicate once, so a vector is never freed through the other path.
//
// Bookkeeping: one `uint8_t` mask and one page address per live page, in a
// mutex-guarded map. At a peak of 1,721 columns that is ~15k pages (60 MB) and a
// few hundred KB of table — the table is the only thing that cannot live inside
// the page, because the page is exactly two 2,048-byte slots and a header would
// take the first array's bytes.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <new>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/page_alloc.h"

namespace mc::core {

class LightSlabPool {
public:
  // One light array (`world/light.h`'s kLightArrayBytes, which asserts this).
  static constexpr std::size_t kSlotBytes = 2048;

  // Two slots per page where the platform's page is 4 KiB; more where it is
  // larger (one mask bit each). A slot is always a multiple of kSlotBytes from a
  // page boundary, so it is kSlotBytes-aligned.
  static std::size_t slots_per_page() {
    return page_bytes() / kSlotBytes;
  }

  // Read once, like `page_mapping_min_bytes()`: an allocation and the
  // deallocation that frees it must take the same path. `PLUSMC_LIGHT_ALLOC=0`
  // answers false and every slot goes to `operator new`.
  static bool enabled() {
    static const bool kEnabled = [] {
#if defined(PLUSMC_PAGE_ALLOC_MAPPED)
      const char* text = std::getenv("PLUSMC_LIGHT_ALLOC");
      return !(text != nullptr && text[0] == '0' && text[1] == '\0');
#else
      return false;
#endif
    }();
    return kEnabled;
  }

  // One slot's bytes, or null when the pool cannot grow. The allocator turns
  // that into `bad_alloc` rather than a heap block: `deallocate` has only the
  // size to go on, so an allocation that silently changed path would be freed
  // through the other one (and a slot handed to `operator delete` is not a
  // leak a test would see until the pool's map said the page was still live).
  static void* acquire() {
    if (!enabled())
      return nullptr;
#if defined(PLUSMC_PAGE_ALLOC_MAPPED)
    std::lock_guard<std::mutex> lock(mutex());
    SlotMap& pages = live_pages();
    // Reuse a free slot on a page the pool already owns. The candidate has to
    // be checked rather than trusted: a page can be listed twice (two releases
    // before either is consumed) and the second visit then finds it full. The
    // old code always picked slot 0 or 1, which is only correct for a
    // two-slot page; a 16 KiB page on an ARM host has eight and would hand
    // out an occupied slot.
    while (!partial_pages().empty()) {
      const auto candidate = partial_pages().begin();
      const std::uintptr_t page = *candidate;
      partial_pages().erase(candidate);
      auto it = pages.find(page);
      if (it == pages.end())
        continue; // a stale entry whose page has already been unmapped
      const SlotMask free = ~it->second & full_mask();
      if (free == 0)
        continue; // full: the other entry for this page is the useful one
      const int slot = static_cast<int>(std::countr_zero(free));
      it->second |= slot_bit(slot);
      if ((~it->second & full_mask()) != 0)
        partial_pages().insert(page); // still a free slot for the next caller
      return reinterpret_cast<void*>(page + static_cast<std::size_t>(slot) * kSlotBytes);
    }
    void* page = map_pages(page_bytes());
    if (page == nullptr)
      return nullptr;
    // Slot 0 is taken here and the page joins the partial set immediately, so
    // the other slot(s) are handed out before another page is mapped. Leaving
    // it off until a release was the bug that made every 2 KiB array spend a
    // whole page (tests/chunk_cache_test.cpp: `light_slot_check`).
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(page);
    pages.emplace(address, slot_bit(0));
    if ((~slot_bit(0) & full_mask()) != 0)
      partial_pages().insert(address);
    return page;
#else
    return nullptr;
#endif
  }

  static void release(void* slot) noexcept {
    if (!enabled() || slot == nullptr)
      return;
#if defined(PLUSMC_PAGE_ALLOC_MAPPED)
    std::lock_guard<std::mutex> lock(mutex());
    SlotMap& pages = live_pages();
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(slot);
    const std::uintptr_t page = address & ~(static_cast<std::uintptr_t>(page_bytes()) - 1);
    auto it = pages.find(page);
    if (it == pages.end())
      return; // not ours: nothing to do (the pool only ever hands out its slots)
    const int index = static_cast<int>((address - page) / kSlotBytes);
    if (index < 0 || index >= static_cast<int>(slots_per_page()) ||
        (it->second & slot_bit(index)) == 0)
      return; // not a live slot: ignore rather than corrupt the page's mask
    it->second &= ~slot_bit(index);
    if (it->second == 0) {
      partial_pages().erase(page); // it may still be listed from a prior release
      unmap_pages(reinterpret_cast<void*>(page), page_bytes());
      pages.erase(it);
      return;
    }
    partial_pages().insert(page);
#endif
  }

  // For the test that pins the mechanism: the bytes the pool currently holds
  // mapped, and the pages it has (a page holds `slots_per_page()` slots).
  static std::size_t mapped_bytes() {
    std::lock_guard<std::mutex> lock(mutex());
    return live_pages().size() * page_bytes();
  }
  static std::size_t mapped_pages() {
    std::lock_guard<std::mutex> lock(mutex());
    return live_pages().size();
  }

private:
  // One bit per slot. A 64 KiB page holds 32 two-KiB slots, which does not fit
  // in the `uint8_t` this used to be.
  using SlotMask = std::uint32_t;
  using SlotMap = std::unordered_map<std::uintptr_t, SlotMask>;

  static SlotMask slot_bit(int index) {
    return SlotMask{1} << static_cast<unsigned>(index);
  }
  // Every slot of a page, with the shift kept below the type's width (a 64 KiB
  // page is exactly 32 slots).
  static SlotMask full_mask() {
    const std::size_t slots = slots_per_page();
    return slots >= 32 ? ~SlotMask{0} : (SlotMask{1} << slots) - 1;
  }

  // Function-local statics so the pool is initialised on first use, not at
  // static-init time (the mmap path is not available in every early context).
  //
  // They are deliberately *leaked* (allocated with `new` and never deleted).
  // The arrays these statics serve outlive them: `g_server_world` in
  // server/session.cpp is constructed during static init, the pool's statics
  // are constructed later on first allocation, and C++ destroys function-local
  // statics in reverse order of construction — so at process exit the world's
  // chunks (and their `LightArray`s) are destroyed *after* this bookkeeping.
  // A by-value static made that a mutex-lock/map-use after destruction, which
  // aborted every wire server on SIGTERM (`worldgen-wire`, `world_join`, ...
  // expected rc 0, got 134). Leaking the three tables keeps `release()` valid
  // for the whole teardown; the process is exiting, so the operating system
  // reclaims them with everything else.
  static std::mutex& mutex() {
    static std::mutex* m = new std::mutex;
    return *m;
  }
  static SlotMap& live_pages() {
    static SlotMap* map = new SlotMap;
    return *map;
  }
  // A page address with at least one free slot. The set is what keeps a
  // release from listing the same page twice: the old vector could, and a
  // duplicate visit with every slot already taken was one step away from
  // handing out a slot two live arrays shared.
  static std::unordered_set<std::uintptr_t>& partial_pages() {
    static std::unordered_set<std::uintptr_t>* pages = new std::unordered_set<std::uintptr_t>;
    return *pages;
  }
};

// `std::allocator`-shaped, stateless (so containers move and swap without
// rebinding), and only special-cased for exactly one 2 KiB slot.
template <typename T> class LightSlabAllocator {
public:
  using value_type = T;
  using is_always_equal = std::true_type;

  LightSlabAllocator() noexcept = default;
  template <typename U> LightSlabAllocator(const LightSlabAllocator<U>&) noexcept {}

  [[nodiscard]] T* allocate(std::size_t n) {
    // The enabled check is on *this* side first: `acquire()` answers null both
    // when the pool is off and when it cannot map, and the first of those is the
    // heap path, not an out-of-memory. (`deallocate` reads the same predicate
    // and the same size, so the two always agree on which path an array took.)
    if constexpr (sizeof(T) == 1 && alignof(T) <= LightSlabPool::kSlotBytes) {
      if (n * sizeof(T) == LightSlabPool::kSlotBytes && LightSlabPool::enabled()) {
        if (void* slot = LightSlabPool::acquire())
          return static_cast<T*>(slot);
        throw std::bad_alloc();
      }
    }
    return static_cast<T*>(::operator new(n * sizeof(T)));
  }

  void deallocate(T* p, std::size_t n) noexcept {
    if (p == nullptr)
      return;
    if constexpr (sizeof(T) == 1 && alignof(T) <= LightSlabPool::kSlotBytes) {
      if (n * sizeof(T) == LightSlabPool::kSlotBytes && LightSlabPool::enabled()) {
        LightSlabPool::release(p);
        return;
      }
    }
    ::operator delete(p);
  }
};

template <typename T, typename U>
bool operator==(const LightSlabAllocator<T>&, const LightSlabAllocator<U>&) noexcept {
  return true;
}

template <typename T, typename U>
bool operator!=(const LightSlabAllocator<T>&, const LightSlabAllocator<U>&) noexcept {
  return false;
}

} // namespace mc::core
