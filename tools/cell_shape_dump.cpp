// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a generated column's block cells actually cost, and where they land on
// `core/page_alloc.h`'s cutoff — the measurement behind docs/PHASE5.md §4's
// statement that the packed cell container moved the common width below the page
// the allocator maps, so a released generated column has no cell frames to give
// back.
//
//   ./build/plusmc-cell-shape                 # seed 42, 16 columns
//   ./build/plusmc-cell-shape --seed 7 --columns 24
//
// It fills real columns through `fill_generated_chunk`, the same path the server's
// world store takes, and prints one line per column: the store's own
// `Chunk::estimated_bytes` for it, the bytes its cell buffers hold, and how many
// of those are page-mapped. The histogram at the end is the shape: `0 B` is a
// uniform section (vanilla's single-entry palette, no buffer at all), 2,048 B is
// four bits per cell, 4,096 B is eight bits (one page, so mapped), and 16,384 B
// is 32 bits (four pages, mapped).
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>

#include "world/chunk.h"
#include "world/generated_chunk.h"
#include "world/memory_world.h"
#include "world/world_light.h"
#include "world/worldgen_chunk.h"

namespace {

int parse_int(const char* text, int fallback) {
  if (text == nullptr)
    return fallback;
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (end == text || value <= 0 || value > 1 << 20)
    return fallback;
  return static_cast<int>(value);
}

} // namespace

int main(int argc, char** argv) {
  int64_t seed = 42;
  int columns = 16;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--seed" && i + 1 < argc) {
      seed = std::strtoll(argv[++i], nullptr, 10);
    } else if (arg == "--columns" && i + 1 < argc) {
      columns = parse_int(argv[++i], columns);
    } else {
      std::fprintf(stderr, "usage: %s [--seed N] [--columns N]\n", argv[0]);
      return 2;
    }
  }

  mc::world::WgOverworldGenerator generator(seed);
  mc::world::Chunk chunk;
  std::map<std::size_t, int> sections_by_bytes;
  std::size_t total_cells = 0;
  std::size_t total_mapped = 0;
  std::size_t total_sections = 0;
  // A spread rather than a run: the generator's own variety (ocean floor, hills,
  // the far terrain) is what decides a section's palette width.
  const int grid =
      columns > 0 ? static_cast<int>(std::ceil(std::sqrt(static_cast<double>(columns)))) : 1;
  int done = 0;
  for (int gz = 0; gz < grid && done < columns; ++gz) {
    for (int gx = 0; gx < grid && done < columns; ++gx, ++done) {
      const int cx = (gx - grid / 2) * 137;
      const int cz = (gz - grid / 2) * 149;
      mc::world::fill_generated_chunk(generator, cx, cz, chunk);
      std::size_t cells = 0;
      std::size_t mapped = 0;
      for (const mc::world::Section& section : chunk.sections) {
        const std::size_t bytes = section.block_cells.bytes();
        cells += bytes;
        if (section.block_cells.page_mapped())
          mapped += bytes;
        ++sections_by_bytes[bytes];
        ++total_sections;
      }
      std::printf("chunk %6d,%6d  estimate %8zu B  cells %7zu B  mapped %7zu B  sections %zu\n", cx,
                  cz, chunk.estimated_bytes(), cells, mapped, chunk.sections.size());
      total_cells += cells;
      total_mapped += mapped;
    }
  }

  const std::size_t count = static_cast<std::size_t>(done);
  std::printf("\nper column: cells %zu B, of them page-mapped %zu B\n", total_cells / count,
              total_mapped / count);
  std::printf("sections by buffer size (over %zu columns, %zu sections):\n", count, total_sections);
  for (const auto& [bytes, n] : sections_by_bytes) {
    const char* what = bytes == 0      ? "uniform, no buffer (0 bits)"
                       : bytes == 2048 ? "four bits per cell, heap (below the cutoff)"
                       : bytes == 4096 ? "eight bits per cell, one page, mapped"
                                       : "32 bits per cell, four pages, mapped";
    std::printf("  %6zu B  %5d sections  %s\n", bytes, n, what);
  }

  // The other half of a packed column: the light engine's arrays. Lit through the
  // real entry point (`MemoryWorld` + `relight_world_chunk`), so the count is
  // what the server holds after the send path has lit a column — cells alone
  // understate what a released column leaves behind.
  auto world = std::make_shared<mc::world::MemoryWorld>(
      /*min_section_y=*/-4, /*section_count=*/24,
      std::make_shared<mc::world::WgOverworldGenerator>(seed));
  const std::size_t pages_before = mc::core::LightSlabPool::mapped_pages();
  std::size_t light_bytes = 0;  // what the arrays actually hold (uniform = 0)
  std::size_t light_arrays = 0; // arrays a reader sees, uniform or not
  std::size_t uniform_arrays = 0;
  std::size_t sky_arrays = 0;
  std::size_t block_arrays = 0;
  std::size_t lit_columns = 0;
  for (int gz = 0, i = 0; gz < grid && i < columns; ++gz) {
    for (int gx = 0; gx < grid && i < columns; ++gx, ++i) {
      const int cx = (gx - grid / 2) * 137;
      const int cz = (gz - grid / 2) * 149;
      world->load_chunk(cx, cz);
      mc::world::relight_world_chunk(cx, cz, *world);
      const auto lit = world->load_chunk(cx, cz);
      if (!lit)
        continue;
      for (const mc::world::Section& section : lit->sections) {
        if (section.sky_light) {
          light_bytes += section.sky_light->storage_bytes();
          uniform_arrays += section.sky_light->uniform() ? 1 : 0;
          ++light_arrays;
          ++sky_arrays;
        }
        if (section.block_light) {
          light_bytes += section.block_light->storage_bytes();
          uniform_arrays += section.block_light->uniform() ? 1 : 0;
          ++light_arrays;
          ++block_arrays;
        }
      }
      ++lit_columns;
    }
  }
  if (lit_columns > 0) {
    const std::size_t pages_after = mc::core::LightSlabPool::mapped_pages();
    std::printf("\nper lit column: light %zu arrays, %zu B held (sky %zu, block %zu; %zu of the "
                "arrays uniform, no buffer)\n",
                light_arrays / lit_columns, light_bytes / lit_columns, sky_arrays / lit_columns,
                block_arrays / lit_columns, uniform_arrays / lit_columns);
    std::printf("the pool holds %zu B per array, %zu arrays per page; the %zu lit columns left %zu "
                "page(s) mapped (PLUSMC_LIGHT_ALLOC %s)\n",
                mc::core::LightSlabPool::kSlotBytes, mc::core::LightSlabPool::slots_per_page(),
                lit_columns, pages_after - pages_before,
                mc::core::LightSlabPool::enabled() ? "on" : "off");
  }
  return 0;
}
