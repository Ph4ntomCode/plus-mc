// SPDX-License-Identifier: GPL-3.0-or-later
//
// A section's block cells — 4096 palette indices — stored the way vanilla's
// `PalettedContainer` stores them, not one `int32_t` each.
//
// The measurement that asked for this (docs/PHASE5.md §4): a loaded column cost
// ~460 KB, of which `block_cells` was 384 KB. That single buffer was 69% of the
// server's RSS at 60 players, and it spent four bytes per block on a section
// whose palette is usually a couple of dozen states and, for the overwhelming
// majority of a generated column, exactly one:
//
//   storage                bits/cell   bytes per section   full 24-section column
//   int32_t cells (before)        32              16384 B                  384 KiB
//   8-bit palette index            8               4096 B                   96 KiB
//   4-bit palette index            4               2048 B                   48 KiB
//   uniform (one state)            0                  0 B                      0 B
//
// Three properties make this free where it matters:
//
//  - The bytes on the *wire* do not change. The encoder packs from the values
//    (`pack_cells` in world/chunk.cpp), choosing vanilla's own width from the
//    palette size; this container's storage width is nobody else's business.
//  - The bytes on *disk* do not change either: the save path does the same.
//  - No call site outside this file can tell. The whole interface is `size()`,
//    `operator[]` and `set()` — and the split between the last two is the point:
//    `operator[]` is const-only and `set()` is the one place that can widen the
//    storage, so a reader cannot accidentally grow one and a writer cannot
//    forget that it might (the compiler rejects `cells[i] = v`, which four sites
//    used to spell and now spell `cells.set(i, v)`).
//
// The widths are vanilla's own (`Strategy.SECTION_STATES`: minimum four bits,
// then eight, then the global palette): a section's footprint here can never be
// larger than vanilla's, and it is *empty* for the sections vanilla also stores
// with no array at all (the single-entry palette — all-air and single-state
// sections, which is most of a column). The width only grows (uniform -> 4 -> 8
// -> 32 bits) when a write does not fit, which is vanilla growing its palette;
// it never shrinks, because a section that has held a 300-state palette once
// will hold one again and rebuilding is the expensive direction.
//
// The layout is dense and index-ordered: value `i` starts at bit `i * bits`. The
// widths are powers of two that divide 64 (4, 8, 32), so a cell never crosses a
// word and both accessors are one shift, one mask and one load or store — that
// is why there is no straddling path here, and why the widths are the only thing
// `width_for` is allowed to return.
//
// The buffer comes from `core/page_alloc.h`, the allocator written for exactly
// this: a released column's bulk buffers have to be *returnable* to the operating
// system, and a shared-heap block cannot be given back whole once the store's own
// churn has left live allocations around it (the header and docs/PHASE5.md §4
// have the measurement). Note where the widths land on that allocator's cutoff:
// 4-bit cells are 2 KiB and stay on the heap, 8-bit cells are exactly one page
// and 32-bit cells are 16 KiB — four pages — so the sections that own a large
// buffer get it unmapped whole when the column goes, and the ones that own no
// buffer at all have nothing to unmap.
//
// The saving is not only bytes. `Chunk::estimated_bytes` counts `bytes()`, and
// the store's budget (world/memory_world.h's `ChunkBudget`) is kept in those
// estimates, so the same budget now holds several times the columns: the same
// reachable world for a fraction of the RAM, or the same RAM with far fewer
// evictions (both are measured in docs/PHASE5.md §4).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/page_alloc.h"

namespace mc::world {

class SectionCells {
public:
  // The storage widths, in the order they grow. 0 is the singleton section —
  // vanilla's single-entry palette — which owns no buffer at all.
  static constexpr int kUniformBits = 0;
  static constexpr int kMinBits = 4;
  static constexpr int kMidBits = 8;
  static constexpr int kMaxBits = 32;

  SectionCells() = default;

  std::size_t size() const noexcept {
    return count_;
  }
  bool empty() const noexcept {
    return count_ == 0;
  }

  // Every cell is one value and no buffer is held. A caller that would otherwise
  // walk all 4096 cells to ask a per-state question (how many are randomly
  // ticking, is any of them not air) can answer it once instead.
  bool uniform() const noexcept {
    return bits_ == kUniformBits;
  }
  // That value when `uniform()`; meaningless otherwise.
  int32_t uniform_value() const noexcept {
    return uniform_;
  }

  // What the cells actually occupy, and whether that storage is its own mapping
  // rather than a heap block (`core/page_alloc.h`). `bytes()` is what
  // `Chunk::estimated_bytes` counts; `page_mapped()` is for the test that pins
  // the allocator's side of the bargain.
  std::size_t bytes() const noexcept {
    return words_.size() * sizeof(uint64_t);
  }
  bool page_mapped() const noexcept {
    return mc::core::buffer_is_page_mapped(bytes());
  }

  // The buffer's first byte, or null when the section is uniform. For the tests
  // that pin the allocator's contract — page alignment, one mapping per section —
  // and for nothing else: a caller that walks the cells through this would be
  // writing its own bit unpacker, which is the thing this type exists to keep in
  // one place.
  const void* buffer() const noexcept {
    return words_.empty() ? nullptr : words_.data();
  }

  // The read: one cell. No bounds check, like the `std::vector` this replaces —
  // every caller already compares the index against `size()`.
  int32_t operator[](std::size_t i) const noexcept {
    if (bits_ == kUniformBits)
      return uniform_;
    const std::size_t bit = i * static_cast<std::size_t>(bits_);
    return static_cast<int32_t>((words_[bit >> 6] >> (bit & 63)) & mask());
  }

  // The write, and the only way one happens. Grows the storage to the smallest
  // width that holds `value` when the current one does not — which is the
  // expensive case and the only one that allocates, so the common write is a
  // mask and a store into a buffer that already exists.
  //
  // `i` must be below `size()`; a write past the end is dropped rather than
  // growing the cell count, because the count is the caller's invariant (it is
  // how the section knows its shape) and a container that extended itself from a
  // miscounted index would hide the bug rather than surface it.
  void set(std::size_t i, int32_t value) {
    if (i >= count_)
      return;
    if (bits_ == kUniformBits) {
      if (value == uniform_)
        return; // still one value: no buffer needed
      materialise(width_for(value), uniform_);
    } else if (!fits(value, bits_)) {
      widen(width_for(value));
    }
    write_cell(words_.data(), i, bits_, value);
  }

  // `std::vector`'s resize: growing keeps the cells that exist and zero-fills
  // the rest, shrinking keeps the prefix. The common call on a fresh section is
  // `assign(kBlockCellsPerSection, 0)` followed by a bulk fill, and both of those
  // leave the container uniform and bufferless until a write actually needs one.
  void resize(std::size_t count) {
    if (count == count_)
      return;
    if (count < count_) {
      count_ = count;
      return;
    }
    if (bits_ == kUniformBits && uniform_ == 0) {
      count_ = count; // zeros stay zeros
      return;
    }
    if (bits_ == kUniformBits) {
      // A non-zero fill makes the new tail a second value, so the container has
      // to materialise; the smallest width that holds the fill holds everything.
      const int bits = width_for(uniform_);
      std::vector<uint64_t, mc::core::PageAllocator<uint64_t>> words(word_count(count, bits), 0);
      for (std::size_t i = 0; i < count_; ++i)
        write_cell(words.data(), i, bits, uniform_);
      words_.swap(words);
      bits_ = bits;
    } else {
      // Same width and same indexing: the prefix keeps its exact bit positions,
      // so it is a word copy and the tail is already zero. `words_` can be
      // *longer* than the new buffer after a shrink (the shrink keeps the
      // allocation), so the copy is bounded by both sizes — and the cells in
      // [count_, count) are written as zero rather than left as whatever the
      // longer buffer still held, which is what "zero-fills the rest" means.
      std::vector<uint64_t, mc::core::PageAllocator<uint64_t>> words(word_count(count, bits_), 0);
      const std::size_t copy_words = std::min(words_.size(), words.size());
      for (std::size_t i = 0; i < copy_words; ++i)
        words[i] = words_[i];
      for (std::size_t i = count_; i < count; ++i)
        write_cell(words.data(), i, bits_, 0);
      words_.swap(words);
    }
    count_ = count;
  }

  // Every cell set to `value`. One value is one value whatever it is, so this is
  // the one bulk entry point that can *drop* a buffer: a full section of stone
  // costs nothing here, exactly as it costs vanilla nothing (the single-entry
  // palette holds the state, not an array of 4096 copies of it).
  void assign(std::size_t count, int32_t value) {
    count_ = count;
    words_.clear();
    words_.shrink_to_fit();
    bits_ = kUniformBits;
    uniform_ = value;
  }

  // The bulk load: a whole container unpacked in one pass, from the packed
  // layout the disk and the wire use (values per long, no straddling).
  //
  // This exists because the one-cell-at-a-time form costs a `set` per cell, and
  // `set` has to re-ask two questions the loader already answered: is the
  // section still uniform (it cannot be, we are about to write every cell) and
  // does the value fit the current width (the container declared the width).
  // The loader is the hot half of a cold Anvil read — 4096 cells over every
  // section that has a container — so it gets one materialisation and one direct
  // store per cell. Returns false when `longs` is too short for `count` cells at
  // `bits`, exactly like the generic loop.
  bool assign_packed(const int64_t* longs, std::size_t nlongs, int bits, std::size_t count) {
    if (bits < 1 || bits > kMaxBits || nlongs == 0 || count == 0)
      return false;
    const int per_long = 64 / bits;
    const std::size_t needed =
        (count + static_cast<std::size_t>(per_long) - 1) / static_cast<std::size_t>(per_long);
    if (nlongs < needed)
      return false;
    // The container's declared width is a floor for ours, exactly as in
    // `reserve_width`: 5- and 6-bit containers live at 8 here.
    const int width = bits <= kMinBits ? kMinBits : (bits <= kMidBits ? kMidBits : kMaxBits);
    // The common containers (4 and 8 bits, and a section whose count is a whole
    // number of longs) already hold exactly our layout, cell for cell: the disk
    // packing is "values per long, no straddling" and so is this container, and
    // at equal widths the word *is* the answer. That is the whole of a generated
    // terrain section's load, so it is a copy rather than 4096 unpacked stores.
    if (bits == width && count % static_cast<std::size_t>(per_long) == 0) {
      words_.resize(needed);
      std::memcpy(words_.data(), longs, needed * sizeof(uint64_t));
      bits_ = width;
      count_ = count;
      uniform_ = 0;
      return true;
    }
    std::vector<uint64_t, mc::core::PageAllocator<uint64_t>> words(word_count(count, width), 0);
    const uint64_t mask = mask_for(bits);
    for (std::size_t i = 0; i < needed; ++i) {
      const uint64_t w = static_cast<uint64_t>(longs[i]);
      const std::size_t base = i * static_cast<std::size_t>(per_long);
      for (int k = 0; k < per_long; ++k) {
        const std::size_t cell = base + static_cast<std::size_t>(k);
        if (cell >= count)
          break;
        write_cell(words.data(), cell, width, static_cast<int32_t>((w >> (k * bits)) & mask));
      }
    }
    words_.swap(words);
    bits_ = width;
    count_ = count;
    uniform_ = 0;
    return true;
  }

  // Pre-grow to the width a bulk loader knows it is about to need: the disk path
  // reads the container's declared bits before it unpacks a single cell, and
  // unpacking into a 4-bit buffer first would rebuild the whole section twice
  // more on the way to 32. `bits_wanted` is that declared width; the storage
  // width is the smallest of ours that holds it.
  void reserve_width(int bits_wanted) {
    const int bits =
        bits_wanted <= kMinBits ? kMinBits : (bits_wanted <= kMidBits ? kMidBits : kMaxBits);
    if (count_ == 0 || (bits_ != kUniformBits && bits_ >= bits))
      return;
    if (bits_ == kUniformBits)
      materialise(bits, uniform_);
    else
      widen(bits);
  }

private:
  // The smallest storage width that holds `value`. Negative values — which no
  // cell of a loaded section holds, but a fabricated one can be handed — do not
  // fit any of them as unsigned and land on the widest.
  static int width_for(int32_t value) noexcept {
    const uint32_t u = static_cast<uint32_t>(value);
    if (u < (uint32_t{1} << kMinBits))
      return kMinBits;
    if (u < (uint32_t{1} << kMidBits))
      return kMidBits;
    return kMaxBits;
  }

  static bool fits(int32_t value, int bits) noexcept {
    return bits >= kMaxBits || static_cast<uint32_t>(value) < (uint32_t{1} << bits);
  }

  static std::size_t word_count(std::size_t count, int bits) noexcept {
    return (count * static_cast<std::size_t>(bits) + 63) / 64;
  }

  static uint64_t mask_for(int bits) noexcept {
    return bits >= kMaxBits ? 0xFFFFFFFFull : ((uint64_t{1} << bits) - 1);
  }

  // One cell, inside one word: the widths all divide 64 (see the header note).
  static int32_t read_cell(const uint64_t* words, std::size_t i, int bits) noexcept {
    const std::size_t bit = i * static_cast<std::size_t>(bits);
    return static_cast<int32_t>((words[bit >> 6] >> (bit & 63)) & mask_for(bits));
  }

  static void write_cell(uint64_t* words, std::size_t i, int bits, int32_t value) noexcept {
    const std::size_t bit = i * static_cast<std::size_t>(bits);
    const std::size_t word = bit >> 6;
    const int shift = static_cast<int>(bit & 63);
    // `field` is the mask shifted into place, so the clear and the set are one
    // read-modify-write into a word the section already owns.
    const uint64_t field = mask_for(bits) << shift;
    words[word] = (words[word] & ~field) |
                  ((static_cast<uint64_t>(static_cast<uint32_t>(value)) << shift) & field);
  }

  uint64_t mask() const noexcept {
    return mask_for(bits_);
  }

  // Give the container a buffer at `bits`, every cell holding `fill`. Used when
  // a uniform section stops being one.
  //
  // The buffer has to hold *both* the fill and the value that asked for it, and
  // the fill is the one that can be wider — `assign(n, 300)` followed by
  // `set(i, 1)` would otherwise truncate every other cell to 12 — so the width
  // asked for is a floor, not the answer.
  void materialise(int bits, int32_t fill) {
    bits = std::max(bits, width_for(fill));
    std::vector<uint64_t, mc::core::PageAllocator<uint64_t>> words(word_count(count_, bits), 0);
    if (fill != 0) {
      for (std::size_t i = 0; i < count_; ++i)
        write_cell(words.data(), i, bits, fill);
    }
    words_.swap(words);
    bits_ = bits;
    uniform_ = fill;
  }

  // The expensive direction: a value that does not fit, so every cell is read
  // out of the old layout and written into a wider one. Vanilla pays the same
  // rebuild when its palette outgrows a strategy.
  void widen(int bits) {
    std::vector<uint64_t, mc::core::PageAllocator<uint64_t>> words(word_count(count_, bits), 0);
    for (std::size_t i = 0; i < count_; ++i)
      write_cell(words.data(), i, bits, read_cell(words_.data(), i, bits_));
    words_.swap(words);
    bits_ = bits;
  }

  std::size_t count_ = 0;
  // kUniformBits means "no buffer": `uniform_` is every cell.
  int bits_ = kUniformBits;
  int32_t uniform_ = 0;
  std::vector<uint64_t, mc::core::PageAllocator<uint64_t>> words_;
};

} // namespace mc::world
