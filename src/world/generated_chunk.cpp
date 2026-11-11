// SPDX-License-Identifier: GPL-3.0-or-later
//
// The implementation behind `world/generated_chunk.h`. The only thing worth
// spelling out here is the code map: the surface stage's codes (the noise
// stage's ten, then the materials `world/worldgen_surface.cpp`'s rule tree
// writes) are the palette entries the chunk model stores, and the stage's own
// `wg_surface_state` is the single place that says which state a code is. Water
// and lava are the `level=0` states — the stage's fluids are sources, and every
// reader of a fluid reads its level from the property (world/fluid.cpp), so the
// source spelling is the one to write.
//
// This is the overworld stage the engine implements: noise, biomes, surface
// rules, carvers and — in `fill_generated_decorated_chunk`, the column the server
// serves — the feature stage on top. The noise, surface and carved entry points
// stay because the vanilla probe dumps (`tests/fixtures/worldgen/surface_*`,
// `carved_*`) pin exactly those stages, and the parity test compares at that
// boundary `(PLUSMC_WORLDGEN_STAGE=carved)` rather than pretending the gap is
// closed: this engine's decoration stream is its own, and the applied structure
// kinds are the hand-coded swamp hut, igloo, buried treasure, desert pyramid,
// jungle temple, mineshaft, stronghold, ocean monument and woodland mansion
// plus the template shipwreck, ocean ruin, ruined portal, village, pillager
// outpost, trail ruins and ancient city. The dimension bridge applies the
// bastion, nether fortress and end city in their own dimensions; the Nether arm
// also runs the table-carried nether-cave carvers before decoration.
#include "world/generated_chunk.h"

#include "world/light.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "world/leaves.h" // settle_generated_leaf_distances
#include "world/worldgen_chunk.h"
#include "world/worldgen_feature.h"
#include "world/worldgen_structure.h"
#include "world/worldgen_structure_piece.h"
#include "world/worldgen_surface.h"

namespace mc::world {

namespace {

constexpr size_t kCodeCount = kWgSurfaceBlockCount;

// The grid `WgNoiseChunk::fill_chunk` writes: `[(x*16 + z)*kWgHeight + (y - kWgMinY)]`.
constexpr size_t kCellsPerColumn = static_cast<size_t>(16) * kWgHeight;
constexpr size_t kCellsPerChunk = static_cast<size_t>(16) * kCellsPerColumn;

} // namespace

namespace {

// Generation-time light. A generated column knows every one of its blocks
// before anyone asks for it, so it can answer light the way the engine does
// rather than by a heightmap shortcut: the same per-column source rule (15 at
// and above the highest cell that stops the sky) and the same flood, one level
// per step into shadow. That pass is what lights an overhang's underside, a
// cave mouth and a shaft's floor; a rule that only knows where rain stops reads
// all three as 0.
//
// The pass is `centre_only`: the ring is unmodelled, so it is exact wherever the
// light comes from a source inside the column and cannot see light that enters
// across a chunk border from a neighbour's source. That last part is the 3x3
// relight's contribution and the number `bench/light_seam.cpp` splits into
// interior and border cells. The ring itself is still never built here — that is
// the cost the send path's shortcut exists to remove — and a column's own pass
// is small next to its terrain.
//
// The first version shaded a border cell one step down instead (14 written under
// the surface against a neighbour's height). That put light inside solid blocks
// — the seam priced it at ~10% of all sky cells disagreeing with a relight — and
// it needed a full noise fill of four neighbours to find the heights, by far the
// most expensive part of a column.
void fill_generated_sky_light(Chunk& chunk) {
  LightEnvironment env;
  env.min_y = chunk.min_section_y * 16;
  env.max_y = env.min_y + static_cast<int>(chunk.sections.size()) * 16;
  // `centre_only` is what keeps the engine's 48x48 read area from treating the
  // halo as fully open and lighting the border from nothing.
  env.centre_only = true;
  const int base_x = chunk.x * 16;
  const int base_z = chunk.z * 16;
  const auto in_chunk = [base_x, base_z](int x, int z) {
    return x >= base_x && x < base_x + 16 && z >= base_z && z < base_z + 16;
  };
  // The chunk is the caller's own, so hand the engine a region over it instead
  // of the per-cell callbacks: `kind_at` then reads a palette entry through a
  // memoised table rather than copying a `BlockStateKey` (two strings) for every
  // one of the ~100k cells, and `height_at` reads the column's heightmap. The
  // ring slots stay null, which the engine reads as air; `centre_only` still
  // keeps the halo columns from being seeded, so the result is the same pass.
  // A/B arm `PLUSMC_LIGHT_REGION=off` is the callback path this used to take.
  static const bool kUseRegion = [] {
    const char* text = std::getenv("PLUSMC_LIGHT_REGION");
    return text == nullptr || std::strcmp(text, "off") != 0;
  }();
  LightRegion region;
  if (kUseRegion) {
    region.centre_x = chunk.x;
    region.centre_z = chunk.z;
    region.set(0, 0, &chunk);
    env.region = &region;
  }
  // The bounds are the chunk's own, not `[x*16-16, ...)`: an off-by-one halo
  // would wrap the column's own blocks outside its border and light it from a
  // copy of itself.
  env.get_block = [&chunk, &in_chunk](int x, int y, int z) {
    if (!in_chunk(x, z))
      return BlockStateKey{"minecraft:air", ""};
    return chunk.block_state_at(x & 15, y, z & 15);
  };
  env.height = [&chunk, &in_chunk, &env](int x, int z) {
    if (!in_chunk(x, z))
      return env.min_y;
    return chunk_motion_blocking_height(chunk, x & 15, z & 15);
  };
  compute_chunk_light(chunk, env);
  // Both layers are kept. The decorated stage places emitters (a lava lake, a
  // glowstone cluster) after this pass would otherwise be trusted as final, and
  // a served column carries light before it is sent — `chunk_has_light` is what
  // tells the send path not to relight it — so dropping the block layer here
  // would hand a client dark lava. The pass already seeds and floods both
  // layers, so keeping the block array costs nothing beyond the bytes.
}

// How far past the noise stage a column is built. The entry points differ only
// in how many passes ran over the same sound array; everything after the array
// (palettes, biome cells, derives) is identical. `kDecorated` is the carved
// column with the feature stage applied on top and the derives rebuilt from the
// decorated cells.
enum class Stage { kNoise, kSurface, kCarved, kDecorated };

// The one implementation behind the three entry points.
void fill_from_codes(const WgOverworldGenerator& generator, int cx, int cz, Stage stage,
                     Chunk& out) {
  // In the fixture's own cell layout, so this function, `test_noise_chunks` and
  // `test_surface_chunks` read the same array the same way: the column the
  // server serves is the one the surface dumps pin.
  std::vector<uint8_t> codes(kCellsPerChunk);
  std::vector<const char*> biome_names(static_cast<size_t>(kWgHeight / 16) * kBiomeCellsPerSection);
  bool biomes_filled = false;
  switch (stage) {
  case Stage::kDecorated:
  case Stage::kCarved:
    // One `NoiseChunk` serves the terrain stages and the biome fill: the
    // 1,536 climate samples read the router and flat caches the surface pass
    // already warmed instead of a third wrapped graph over the same chunk.
    generator.fill_carved_chunk_with_biomes(cx, cz, codes.data(), biome_names.data());
    biomes_filled = true;
    break;
  case Stage::kSurface:
    generator.fill_surface_chunk(cx, cz, codes.data());
    break;
  case Stage::kNoise:
    generator.fill_noise_chunk(cx, cz, codes.data());
    break;
  }

  out = Chunk{};
  out.x = cx;
  out.z = cz;
  out.min_section_y = kWgMinY / 16;                           // -4
  const int section_count = static_cast<int>(kWgHeight) / 16; // 24
  out.sections.resize(static_cast<size_t>(section_count));
  for (int i = 0; i < section_count; ++i) {
    Section& section = out.sections[static_cast<size_t>(i)];
    section.y = out.min_section_y + i;
    // Every section carries an explicit container, like the air column
    // `MemoryWorld` fabricates: a single-entry palette never has to be
    // materialised by a reader, and the encoder emits a uniform section from
    // it either way.
    section.has_blocks = true;
    section.block_cells.assign(kBlockCellsPerSection, 0);
    section.has_biomes = true;
    section.biome_cells.assign(kBiomeCellsPerSection, 0);
  }

  // The biome stage's own answer, cell for cell: one bare graph answers all
  // 1536 cells of the chunk (`fill_biome_chunk`), written in the chunk model's
  // cell order, so each section's 64 names are a contiguous slice. The palette
  // per section is then the first-appearance order of the names it actually
  // holds, like the block palettes above.
  if (!biomes_filled)
    generator.fill_biome_chunk(cx, cz, biome_names.data());

  // The highest non-air cell in each column, relative to the section stack's
  // bottom, recorded while the palettes walk the cells: the heightmap refresh
  // knows then where its search may start. No heightmap answer can be above
  // this cell (the answer *is* an opaque cell, so it is non-air), which is what
  // makes the hint exact rather than a shortcut.
  std::array<int16_t, 256> column_tops{};
  for (int sy = 0; sy < section_count; ++sy) {
    Section& section = out.sections[static_cast<size_t>(sy)];
    // The section's palette, in the order the section's cells first name each
    // state — one entry per code that actually occurs.
    std::array<int32_t, kCodeCount> palette_of{};
    palette_of.fill(-1);
    const int min_y = (out.min_section_y + sy) * 16;
    for (int ly = 0; ly < 16; ++ly) {
      const int y = min_y + ly;
      for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
          const uint8_t code = codes[(static_cast<size_t>(x) * 16 + z) * kWgHeight +
                                     static_cast<size_t>(y - kWgMinY)];
          if (!wg_surface_code_is_air(code)) {
            const int16_t rel = static_cast<int16_t>(sy * 16 + ly + 1);
            int16_t& top = column_tops[static_cast<size_t>(z * 16 + x)];
            if (rel > top)
              top = rel;
          }
          int32_t& palette_index = palette_of[code];
          if (palette_index < 0) {
            palette_index = static_cast<int32_t>(section.block_palette.size());
            const WgSurfaceState& state = wg_surface_state(code);
            section.block_palette.push_back(
                BlockStateKey{std::string(state.name), std::string(state.properties)});
          }
          // Section cells are y-major: `(y*16 + z)*16 + x` (world/chunk.h).
          section.block_cells.set(static_cast<size_t>((ly * 16 + z) * 16 + x), palette_index);
        }
      }
    }

    // The section's biome palette, the same first-appearance rule over the 64
    // cells `chunk_biome_at` indexes (`(y*4 + z)*4 + x`).
    //
    // The names are the generated biome table's own `const char*` entries (the
    // only producer of `biome_names`), so the same biome is normally the same
    // pointer: one pointer compare replaces the `std::string` compare and its
    // `strlen` per cell. The string fallback keeps two spellings of one biome
    // from becoming two palette entries (the wire would collapse them anyway,
    // but the on-disk palette would not), and it only runs on a pointer miss —
    // i.e. when the cell names a biome the section has not held yet.
    std::vector<std::pair<const char*, int32_t>> palette_of_name;
    palette_of_name.reserve(4);
    const size_t biome_base = static_cast<size_t>(sy) * kBiomeCellsPerSection;
    for (size_t cell = 0; cell < static_cast<size_t>(kBiomeCellsPerSection); ++cell) {
      const char* name = biome_names[biome_base + cell];
      int32_t biome_index = -1;
      for (const auto& [seen, index] : palette_of_name) {
        if (seen == name || (seen != nullptr && name != nullptr && std::strcmp(seen, name) == 0)) {
          biome_index = index;
          break;
        }
      }
      if (biome_index < 0) {
        biome_index = static_cast<int32_t>(section.biome_palette.size());
        palette_of_name.emplace_back(name, biome_index);
        section.biome_palette.emplace_back(name);
      }
      section.biome_cells.set(cell, biome_index);
    }
  }

  // The two derived stores a write path maintains per write, in one pass (see
  // `Chunk::rebuild_after_bulk_write`): heightmaps current with the blocks and
  // each section's ticking count.
  // A/B arm `PLUSMC_HEIGHTMAP_TOP_HINT=off` starts every column at the top of
  // the stack, which is what the refresh did before the hint.
  static const bool kTopHint = [] {
    const char* text = std::getenv("PLUSMC_HEIGHTMAP_TOP_HINT");
    return text == nullptr || std::strcmp(text, "off") != 0;
  }();
  out.rebuild_after_bulk_write(kTopHint ? column_tops.data() : nullptr);
  // A decorated column overwrites the carved one and rebuilds its light from
  // what the features left, so a light pass here would be computed and thrown
  // away; decoration reads blocks and heightmaps, never light. A/B arm
  // `PLUSMC_DECORATED_SINGLE_LIGHT=off` keeps the old double pass.
  static const bool kSingleLightPass = [] {
    const char* text = std::getenv("PLUSMC_DECORATED_SINGLE_LIGHT");
    return text == nullptr || std::strcmp(text, "off") != 0;
  }();
  if (stage != Stage::kDecorated || !kSingleLightPass)
    fill_generated_sky_light(out);
  if (stage == Stage::kDecorated) {
    // The feature stage writes blocks over the carved column; the two derives
    // and the sky-light fill are rebuilt from what it left behind, exactly as
    // they are for a carved column above.
    if (wg_decoration_stream(generator.table().dimension) == WgDecorationStream::kLegacy)
      decorate_generated_chunk_legacy(generator, out);
    else
      decorate_generated_chunk(generator, out);
    // Structure pieces come after the feature stage, with a stream of their own
    // (`worldgen_structure_piece.h` says why). The writer reads block cells and
    // the surface stage's heights, never this chunk's heightmaps, so the one
    // rebuild below covers both passes.
    apply_generated_structures(generator, out);
    // The feature stage settled the canopy it placed; a piece can still write
    // wood or leaves next to one (a post, a fallen tree, a hut's fence), which
    // is exactly what decides a leaf's distance, so the column's own settle runs
    // over what the pieces left before the derived stores are rebuilt from it
    // (`world/leaves.h`). On a column no piece touched this is one pass that
    // writes nothing.
    settle_generated_leaf_distances(out);
    out.rebuild_after_bulk_write();
    fill_generated_sky_light(out);
  }
}

} // namespace

void fill_generated_chunk(const WgOverworldGenerator& generator, int cx, int cz, Chunk& out) {
  fill_from_codes(generator, cx, cz, Stage::kNoise, out);
}

namespace {

void fill_dimension_from_codes(const WgOverworldGenerator& generator, int cx, int cz, bool surface,
                               bool carve, Chunk& out) {
  const WgDensityTable& table = generator.table();
  const int section_count = table.height / 16;
  std::vector<uint8_t> codes(static_cast<size_t>(16) * 16 * table.height);
  std::vector<const char*> biome_names(static_cast<size_t>(section_count) * kBiomeCellsPerSection);
  bool biomes_filled = false;
  if (carve) {
    generator.fill_carved_chunk_with_biomes(cx, cz, codes.data(), biome_names.data());
    biomes_filled = true;
  } else if (surface) {
    generator.fill_surface_chunk(cx, cz, codes.data());
  } else {
    generator.fill_noise_chunk(cx, cz, codes.data());
  }
  if (!biomes_filled)
    generator.fill_biome_chunk(cx, cz, biome_names.data());

  out = Chunk{};
  out.x = cx;
  out.z = cz;
  out.min_section_y = table.min_y / 16;
  out.sections.resize(static_cast<size_t>(section_count));
  for (int i = 0; i < section_count; ++i) {
    Section& section = out.sections[static_cast<size_t>(i)];
    section.y = out.min_section_y + i;
    section.has_blocks = true;
    section.block_cells.assign(kBlockCellsPerSection, 0);
    section.has_biomes = true;
    section.biome_cells.assign(kBiomeCellsPerSection, 0);
  }

  std::array<int16_t, 256> column_tops{};
  for (int sy = 0; sy < section_count; ++sy) {
    Section& section = out.sections[static_cast<size_t>(sy)];
    std::array<int32_t, kWgSurfaceBlockCount> palette_of{};
    palette_of.fill(-1);
    const int min_y = (out.min_section_y + sy) * 16;
    for (int ly = 0; ly < 16; ++ly) {
      const int y = min_y + ly;
      for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
          const uint8_t code = codes[(static_cast<size_t>(x) * 16 + z) * table.height +
                                     static_cast<size_t>(y - table.min_y)];
          if (!wg_surface_code_is_air(code)) {
            const int16_t rel = static_cast<int16_t>(sy * 16 + ly + 1);
            int16_t& top = column_tops[static_cast<size_t>(z * 16 + x)];
            if (rel > top)
              top = rel;
          }
          int32_t& palette_index = palette_of[code];
          if (palette_index < 0) {
            palette_index = static_cast<int32_t>(section.block_palette.size());
            const WgSurfaceState& state = wg_surface_state(code);
            const bool is_default_block = code == 1 && table.default_block != nullptr;
            section.block_palette.push_back(BlockStateKey{
                is_default_block ? std::string(table.default_block) : std::string(state.name),
                is_default_block ? std::string() : std::string(state.properties)});
          }
          section.block_cells.set(static_cast<size_t>((ly * 16 + z) * 16 + x), palette_index);
        }
      }
    }

    std::vector<std::pair<const char*, int32_t>> palette_of_name;
    palette_of_name.reserve(4);
    const size_t biome_base = static_cast<size_t>(sy) * kBiomeCellsPerSection;
    for (size_t cell = 0; cell < static_cast<size_t>(kBiomeCellsPerSection); ++cell) {
      const char* name = biome_names[biome_base + cell];
      int32_t biome_index = -1;
      for (const auto& [seen, index] : palette_of_name) {
        if (seen == name || (seen != nullptr && name != nullptr && std::strcmp(seen, name) == 0)) {
          biome_index = index;
          break;
        }
      }
      if (biome_index < 0) {
        biome_index = static_cast<int32_t>(section.biome_palette.size());
        palette_of_name.emplace_back(name, biome_index);
        section.biome_palette.emplace_back(name);
      }
      section.biome_cells.set(cell, biome_index);
    }
  }

  out.rebuild_after_bulk_write(column_tops.data());
}

} // namespace

void fill_generated_dimension_chunk(const WgOverworldGenerator& generator, int cx, int cz,
                                    Chunk& out) {
  fill_dimension_from_codes(generator, cx, cz, /*surface=*/false, /*carve=*/false, out);
}

namespace {

// The End's `NoiseGeneratorSettings` is 128 blocks high, but its dimension type
// is 256 (`the_end`: min_y 0, height 256). Vanilla's noise column therefore
// stops at y 128 and the upper half is air; the section stack still spans the
// whole dimension, and the feature stage's fixed-height geometry (the exit
// platform, the spikes, outer islands) is placed against that full stack.
// Extending the bridge's eight sections with eight empty ones here keeps the
// lower half exactly the table's own answer while giving the encoder and the
// heightmaps the 16 sections the client expects.
void extend_end_to_level_height(Chunk& chunk) {
  constexpr int kEndLevelSections = 256 / 16;
  while (chunk.sections.size() < kEndLevelSections) {
    Section section;
    section.y = chunk.min_section_y + static_cast<int>(chunk.sections.size());
    section.has_blocks = true;
    section.block_palette.push_back(BlockStateKey{"minecraft:air", ""});
    section.block_cells.assign(kBlockCellsPerSection, 0);
    section.has_biomes = true;
    section.biome_palette.emplace_back("minecraft:the_end");
    section.biome_cells.assign(kBiomeCellsPerSection, 0);
    chunk.sections.push_back(std::move(section));
  }
  chunk.rebuild_after_bulk_write();
}

bool structure_box_intersects_chunk(const WgPieceBox& box, int chunk_x, int chunk_z) {
  const int min_x = chunk_x * 16;
  const int min_z = chunk_z * 16;
  return box.max_x >= min_x && box.min_x < min_x + 16 && box.max_z >= min_z &&
         box.min_z < min_z + 16;
}

bool jigsaw_intersects_chunk(const WgJigsawStart& start, int chunk_x, int chunk_z) {
  for (uint32_t i = 0; i < start.piece_count; ++i)
    if (structure_box_intersects_chunk(start.pieces[i].box, chunk_x, chunk_z))
      return true;
  return false;
}

bool fortress_intersects_chunk(const WgFortressStart& start, int chunk_x, int chunk_z) {
  for (uint32_t i = 0; i < start.piece_count; ++i)
    if (structure_box_intersects_chunk(start.pieces[i].box, chunk_x, chunk_z))
      return true;
  return false;
}

// `minecraft:nether_complexes` holds both the fortress and the bastion. The
// set's weighted choice is made once per source chunk; each destination chunk
// rebuilds the same deterministic start and writes only the pieces whose box
// reaches it. Eight source chunks of reach covers every piece of both starts.
void apply_nether_structures(const WgOverworldGenerator& generator, Chunk& chunk) {
  const WgStructureSet* set = wg_structure_set_by_name("minecraft:nether_complexes");
  if (set == nullptr)
    return;
  WgClimateSampler sampler(generator.state(), generator.table());
  WgGeneratedHeights heights(generator);
  constexpr int kReach = 8;
  for (int dz = -kReach; dz <= kReach; ++dz) {
    for (int dx = -kReach; dx <= kReach; ++dx) {
      const int source_x = chunk.x + dx;
      const int source_z = chunk.z + dz;
      if (!wg_structure_is_chunk(generator.seed(), *set, source_x, source_z))
        continue;
      if (wg_nether_complex_kind(generator.seed(), source_x, source_z) ==
          WgNetherComplexKind::kFortress) {
        const WgFortressStart start = wg_fortress_start(generator.seed(), source_x, source_z);
        if (start.valid && fortress_intersects_chunk(start, chunk.x, chunk.z))
          wg_place_fortress(chunk, start);
      } else {
        const WgJigsawStart start =
            wg_bastion_start(generator.seed(), source_x, source_z, sampler, heights, false);
        if (start.valid && jigsaw_intersects_chunk(start, chunk.x, chunk.z))
          wg_place_jigsaw(chunk, start, heights);
      }
    }
  }
}

void apply_end_structures(const WgOverworldGenerator& generator, Chunk& chunk) {
  const WgStructureSet* set = wg_structure_set_by_name("minecraft:end_cities");
  if (set == nullptr)
    return;
  WgClimateSampler sampler(generator.state(), generator.table());
  WgGeneratedHeights heights(generator);
  constexpr int kReach = 8;
  for (int dz = -kReach; dz <= kReach; ++dz) {
    for (int dx = -kReach; dx <= kReach; ++dx) {
      const int source_x = chunk.x + dx;
      const int source_z = chunk.z + dz;
      if (!wg_structure_is_chunk(generator.seed(), *set, source_x, source_z))
        continue;
      const WgJigsawStart start =
          wg_end_city_start(generator.seed(), source_x, source_z, sampler, heights, false);
      if (start.valid && jigsaw_intersects_chunk(start, chunk.x, chunk.z))
        wg_place_jigsaw(chunk, start, heights);
    }
  }
}

} // namespace

void fill_generated_dimension_decorated_chunk(const WgOverworldGenerator& generator, int cx, int cz,
                                              Chunk& out, WgDecorationStats* stats) {
  const WgDensityTable& table = generator.table();
  // The decorated column follows vanilla's status chain: noise and biomes,
  // carvers, surface rules, structure starts, then `applyBiomeDecoration`.
  // Features must see the structure pieces, which is why the structure pass is
  // before decoration rather than after it.
  //
  // The End already matches its reference exactly, so it keeps the single-column
  // path and does not pay the 9x cost. The Nether and overworld run decoration
  // against a 3x3 region: a vein, tree or disk at a chunk border can write into
  // its neighbour and read the neighbour's blocks, exactly as vanilla's
  // `WorldGenRegion` allows. The old centre-only path clipped every such write;
  // that is why the reference c0 magma field had 165 blocks in two clusters
  // while the port's centre-chunk `ore_magma` wrote 2.
  if (table.dimension == WgDimensionKind::kEnd) {
    fill_dimension_from_codes(generator, cx, cz, /*surface=*/true, /*carve=*/false, out);
    extend_end_to_level_height(out);
    apply_end_structures(generator, out);
    out.rebuild_after_bulk_write();
    if (wg_decoration_stream(table.dimension) == WgDecorationStream::kLegacy)
      decorate_generated_chunk_legacy(generator, out, stats);
    else
      decorate_generated_chunk(generator, out, stats);
    settle_generated_leaf_distances(out);
    out.rebuild_after_bulk_write();
    fill_generated_sky_light(out);
    return;
  }

  std::array<Chunk, 9> region;
  Chunk* region_chunks[9] = {};
  for (int dz = -1; dz <= 1; ++dz) {
    for (int dx = -1; dx <= 1; ++dx) {
      Chunk& column = region[static_cast<size_t>((dz + 1) * 3 + (dx + 1))];
      // The Nether's table carries its cave carvers; the overworld's own carver
      // pass is equally part of this status. The End branch above has already
      // returned, so `carve=true` is correct for both remaining dimensions.
      fill_dimension_from_codes(generator, cx + dx, cz + dz, /*surface=*/true, /*carve=*/true,
                                column);
      if (table.dimension == WgDimensionKind::kOverworld)
        apply_generated_structures(generator, column);
      else
        apply_nether_structures(generator, column);
      column.rebuild_after_bulk_write();
      region_chunks[static_cast<size_t>((dz + 1) * 3 + (dx + 1))] = &column;
    }
  }

  // Vanilla's region runs each source chunk's feature stage once. The centre
  // goes first so its own features keep the same stream and result as before;
  // the eight neighbours then run and their border writes land in the centre.
  if (wg_decoration_stream(table.dimension) == WgDecorationStream::kLegacy)
    decorate_generated_chunk_legacy(generator, region[4], stats);
  else
    decorate_generated_chunk(generator, region[4], stats, region_chunks);
  for (int i = 0; i < 9; ++i) {
    if (i == 4)
      continue;
    if (wg_decoration_stream(table.dimension) == WgDecorationStream::kLegacy)
      decorate_generated_chunk_legacy(generator, region[static_cast<size_t>(i)], nullptr);
    else
      decorate_generated_chunk(generator, region[static_cast<size_t>(i)], nullptr, region_chunks);
  }

  settle_generated_leaf_distances(region[4]);
  region[4].rebuild_after_bulk_write();
  fill_generated_sky_light(region[4]);
  out = std::move(region[4]);
}

void fill_generated_dimension_surface_chunk(const WgOverworldGenerator& generator, int cx, int cz,
                                            Chunk& out) {
  fill_dimension_from_codes(generator, cx, cz, /*surface=*/true, /*carve=*/false, out);
}

void fill_generated_dimension_carved_chunk(const WgOverworldGenerator& generator, int cx, int cz,
                                           Chunk& out) {
  // The Nether table carries its own cave carvers; the End has none, so asking
  // for the carved stage there returns the surface column unchanged.
  fill_dimension_from_codes(generator, cx, cz, /*surface=*/true,
                            /*carve=*/generator.table().dimension != WgDimensionKind::kEnd, out);
}

void fill_generated_surface_chunk(const WgOverworldGenerator& generator, int cx, int cz,
                                  Chunk& out) {
  fill_from_codes(generator, cx, cz, Stage::kSurface, out);
}

void fill_generated_carved_chunk(const WgOverworldGenerator& generator, int cx, int cz,
                                 Chunk& out) {
  fill_from_codes(generator, cx, cz, Stage::kCarved, out);
}

void fill_generated_decorated_chunk(const WgOverworldGenerator& generator, int cx, int cz,
                                    Chunk& out) {
  fill_from_codes(generator, cx, cz, Stage::kDecorated, out);
}

} // namespace mc::world
