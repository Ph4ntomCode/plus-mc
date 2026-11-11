// SPDX-License-Identifier: GPL-3.0-or-later
//
// The world a disk-store measurement needs: real overworld columns at full
// size, written as Anvil region files the server serves through `--world`.
//
// The tree has no client-generated region larger than a single captured chunk
// (tests/fixtures/ow3region), so measurements on `AnvilWorld` had to be run on
// a small *authored* world — 21 KB columns of plain/lake/mesa terrain, where a
// column's byte estimate says nothing about a played-in overworld. This tool
// writes the engine's own overworld generator instead — the same
// `WgOverworldGenerator` that `--worldgen` serves, parity-checked against the
// vanilla dumps in tests/fixtures/worldgen — into an Anvil world: real terrain,
// biomes and heightmaps, every column relit so it carries the light arrays a
// played-in vanilla column carries, at whatever size the generator gives them
// (~39 KB of estimate per column for the desert seed the 27 Oct residency A/B
// used, against 57.6 KB for the captured vanilla column at
// tests/fixtures/ow3region chunk (-2,-1)).
//
// Two things about the write-out are deliberate:
//
//   * the spawn square is centred on chunk (0,0), because the server reads
//     `SpawnX`/`SpawnZ` from the minimal level.dat this tool leaves behind and
//     the bots join at the spawn point (`main.cpp`, `--world` open path);
//   * `--settle-leaves` (the default) runs the leaf-distance relaxation to its
//     fixed point before the write-out. The generator now settles each column
//     as it decorates (`decorate_generated_chunk` ends with
//     `settle_generated_leaf_distances`), but a column is settled before the
//     next column's tree can place a leaf across the border, so this world-wide
//     pass still writes those canopies — 6,339 leaves on its first pass for
//     seed 42's 625 columns, 1,163 more over the next five. A world served in
//     the raw default state (`--no-settle-leaves`) is a world the engine's own
//     tick loop calls *decaying*: every leaf is one random tick from being
//     dropped, and `docs/PHASE3.md`'s standing "two adjacent leaves whose log
//     is gone never stop rescheduling each other" load covers the canopy. A
//     forested seed could not be served at a measurable tick at all before that
//     load was fixed (0.3-0.8 TPS, 1.5-3.8 s of work per tick, ~145k
//     block-update packets/s to 20 bots); with the generation-time settle and
//     the scheduled tick's no-op it reads ~20 TPS (`docs/PHASE5.md`'s forested
//     A/B). The rule is the engine's own `leaves_update_distance`, so the two
//     cannot drift.
//
// Usage:
//   ./build/plusmc-gen-real-world --out /tmp/realworld --seed 48 --radius 12
//   ./build/plusmc --world /tmp/realworld --no-flat-world
//
// A seed whose spawn square is treeless (no leaves at all) is what the first
// residency A/B (27 Oct) needed; the forested square carries its own A/B now
// (28 Oct). `--report-leaves` prints the leaf census the settle step starts and
// ends at, which is how a seed is chosen. `--no-settle-leaves` writes the
// generator's raw default states, which is the shape to serve when the leaf
// machinery itself is what is being measured.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <zlib.h>

#include "world/anvil.h"
#include "world/chunk.h"
#include "world/leaves.h"
#include "world/memory_world.h"
#include "world/random_tick.h"
#include "world/world_light.h"
#include "world/worldgen_chunk.h"

namespace {
namespace fs = std::filesystem;

// Floor division, for a chunk's region and its local slot (the region layout
// `world/anvil.cpp` reads is `cx >> 5` with a floor, not a truncation).
int floor_div(int a, int b) {
  return a >= 0 ? a / b : -((-a + b - 1) / b);
}

// A region file under construction: 4 KiB of u32 BE locations
// (sector << 8 | sector count), 4 KiB of timestamps, then the records from
// sector 2 on. Same layout anvil.cpp parses, including the zlib type byte.
class RegionWriter {
public:
  void add(int cx, int cz, const std::vector<uint8_t>& payload, std::string* error) {
    const int lx = cx & 31, lz = cz & 31;
    const uint32_t length = static_cast<uint32_t>(1 + payload.size());
    const uint32_t sectors = (length + 4 + 4095) / 4096;
    if (sectors > 255) {
      *error = "record needs more than 255 sectors";
      return;
    }
    const uint32_t sector = 2 + static_cast<uint32_t>(body_.size() / 4096);
    const size_t idx = static_cast<size_t>(lz * 32 + lx) * 4;
    const uint32_t loc = (sector << 8) | sectors;
    locations_[idx] = static_cast<uint8_t>(loc >> 24);
    locations_[idx + 1] = static_cast<uint8_t>(loc >> 16);
    locations_[idx + 2] = static_cast<uint8_t>(loc >> 8);
    locations_[idx + 3] = static_cast<uint8_t>(loc);
    timestamps_[idx] = timestamps_[idx + 1] = timestamps_[idx + 2] = 0;
    timestamps_[idx + 3] = 1;
    body_.push_back(static_cast<uint8_t>(length >> 24));
    body_.push_back(static_cast<uint8_t>(length >> 16));
    body_.push_back(static_cast<uint8_t>(length >> 8));
    body_.push_back(static_cast<uint8_t>(length));
    body_.push_back(2); // zlib
    body_.insert(body_.end(), payload.begin(), payload.end());
    while (body_.size() % 4096 != 0)
      body_.push_back(0);
  }

  bool write(const fs::path& path, std::string* error) const {
    const fs::path tmp = path.string() + ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      *error = "cannot create " + tmp.string();
      return false;
    }
    out.write(reinterpret_cast<const char*>(locations_.data()),
              static_cast<std::streamsize>(locations_.size()));
    out.write(reinterpret_cast<const char*>(timestamps_.data()),
              static_cast<std::streamsize>(timestamps_.size()));
    out.write(reinterpret_cast<const char*>(body_.data()),
              static_cast<std::streamsize>(body_.size()));
    out.close();
    if (!out) {
      *error = "write failed for " + tmp.string();
      return false;
    }
    fs::rename(tmp, path);
    return true;
  }

  std::size_t bytes() const {
    return body_.size();
  }

private:
  std::vector<uint8_t> locations_ = std::vector<uint8_t>(4096, 0);
  std::vector<uint8_t> timestamps_ = std::vector<uint8_t>(4096, 0);
  std::vector<uint8_t> body_;
};

bool zlib_compress(const std::vector<uint8_t>& in, std::vector<uint8_t>* out) {
  uLongf bound = compressBound(static_cast<uLong>(in.size()));
  out->resize(bound);
  const int rc = compress2(out->data(), &bound, in.data(), static_cast<uLong>(in.size()),
                           Z_DEFAULT_COMPRESSION);
  if (rc != Z_OK)
    return false;
  out->resize(bound);
  return true;
}

// How many leaves each distance holds, over the authored square.
std::map<int, long long> leaf_census(const mc::world::MemoryWorld& store, int radius) {
  std::map<int, long long> by_distance;
  for (int cz = -radius; cz <= radius; ++cz) {
    for (int cx = -radius; cx <= radius; ++cx) {
      const auto chunk = store.load_chunk(cx, cz);
      if (!chunk)
        continue;
      for (int y = chunk->min_section_y * 16;
           y < (chunk->min_section_y + static_cast<int>(chunk->sections.size())) * 16; ++y)
        for (int z = 0; z < 16; ++z)
          for (int x = 0; x < 16; ++x) {
            const auto state = chunk->block_state_at(x, y, z);
            if (mc::world::leaves_is_leaves(state.name))
              ++by_distance[mc::world::state_prop_int(state.properties, "distance").value_or(-1)];
          }
    }
  }
  return by_distance;
}

// One relaxation sweep, each write visible to the next read: the distance wave
// vanilla's block updates run as a tree lands, one layer per pass.
long long settle_leaves_pass(mc::world::MemoryWorld& store, const mc::world::RandomTickLevel& level,
                             int radius) {
  long long changed = 0;
  for (int cz = -radius; cz <= radius; ++cz) {
    for (int cx = -radius; cx <= radius; ++cx) {
      const auto chunk = store.load_chunk(cx, cz);
      if (!chunk)
        continue;
      for (int y = chunk->min_section_y * 16;
           y < (chunk->min_section_y + static_cast<int>(chunk->sections.size())) * 16; ++y) {
        for (int z = 0; z < 16; ++z) {
          for (int x = 0; x < 16; ++x) {
            const auto state = chunk->block_state_at(x, y, z);
            if (!mc::world::leaves_is_leaves(state.name))
              continue;
            const int wx = cx * 16 + x, wz = cz * 16 + z;
            const auto next = mc::world::leaves_update_distance(level, state, wx, y, wz);
            if (next.properties != state.properties) {
              store.set_block_state(cx, cz, x, y, z, next);
              ++changed;
            }
          }
        }
      }
    }
  }
  return changed;
}

void print_census(const char* what, const std::map<int, long long>& census) {
  long long total = 0;
  for (const auto& [distance, count] : census)
    total += count;
  std::printf("%s: %lld leaf/leaves\n", what, total);
  for (const auto& [distance, count] : census)
    std::printf("  distance=%-2d %lld\n", distance, count);
}

} // namespace

int main(int argc, char** argv) {
  fs::path out;
  int64_t seed = 42;
  int radius = 12;
  bool settle = true;
  bool report_leaves = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const char* next = i + 1 < argc ? argv[i + 1] : nullptr;
    if (arg == "--out" && next != nullptr) {
      out = next;
      ++i;
    } else if (arg == "--seed" && next != nullptr) {
      seed = std::strtoll(next, nullptr, 10);
      ++i;
    } else if (arg == "--radius" && next != nullptr) {
      radius = std::atoi(next);
      ++i;
    } else if (arg == "--report-leaves") {
      report_leaves = true;
    } else if (arg == "--no-settle-leaves") {
      settle = false;
    } else {
      std::fprintf(stderr, "%s: unknown argument '%s'\n", argv[0], arg.c_str());
      return 2;
    }
  }
  if (out.empty() || radius < 1 || radius > 15) {
    std::fprintf(stderr,
                 "usage: %s --out DIR [--seed N] [--radius 1..15] [--settle-leaves|"
                 "--no-settle-leaves] [--report-leaves]\n",
                 argv[0]);
    return 2;
  }
  fs::create_directories(out);
  auto anvil = mc::world::AnvilWorld::open_world(out);
  if (!anvil) {
    std::fprintf(stderr, "cannot open the world at %s\n", out.string().c_str());
    return 1;
  }
  if (!anvil->ensure_level_dat(4903, seed)) {
    std::fprintf(stderr, "cannot write level.dat\n");
    return 1;
  }

  auto store = std::make_shared<mc::world::MemoryWorld>(
      mc::world::kOverworldMinSectionY, mc::world::kOverworldSectionCount,
      std::make_shared<mc::world::WgOverworldGenerator>(seed));
  // The square plus the one-column ring the relight's 3x3 reads, built first: a
  // relight of a column whose neighbour is unbuilt would generate that
  // neighbour mid-region.
  for (int cz = -radius - 1; cz <= radius + 1; ++cz)
    for (int cx = -radius - 1; cx <= radius + 1; ++cx)
      store->ensure_chunk_stored(cx, cz);
  std::printf("built %d column(s)\n", (2 * radius + 3) * (2 * radius + 3));

  if (settle) {
    mc::world::RandomTickLevel level;
    level.get_block = [&store](int x, int y, int z) {
      return store->get_block_state(floor_div(x, 16), floor_div(z, 16), x - 16 * floor_div(x, 16),
                                    y, z - 16 * floor_div(z, 16));
    };
    if (report_leaves)
      print_census("as generated", leaf_census(*store, radius));
    for (int pass = 1; pass <= 16; ++pass) {
      const long long changed = settle_leaves_pass(*store, level, radius);
      std::printf("settle pass %d: %lld leaf write(s)\n", pass, changed);
      if (changed == 0)
        break;
    }
    if (report_leaves)
      print_census("after settling", leaf_census(*store, radius));
  } else {
    std::printf("leaves: settled off, the generator's default states are written\n");
  }

  std::printf("relighting...\n");
  std::fflush(stdout);
  for (int cz = -radius; cz <= radius; ++cz)
    for (int cx = -radius; cx <= radius; ++cx)
      mc::world::relight_world_chunk(cx, cz, *store);

  std::map<std::pair<int, int>, RegionWriter> regions;
  std::size_t columns = 0;
  unsigned long long estimated = 0;
  for (int cz = -radius; cz <= radius; ++cz) {
    for (int cx = -radius; cx <= radius; ++cx) {
      const auto chunk = store->load_chunk(cx, cz);
      if (!chunk) {
        std::fprintf(stderr, "no column at %d,%d\n", cx, cz);
        return 1;
      }
      estimated += chunk->estimated_bytes();
      const std::vector<uint8_t> nbt = mc::world::serialize_chunk_nbt_bytes(*chunk, 4903);
      std::vector<uint8_t> compressed;
      if (!zlib_compress(nbt, &compressed)) {
        std::fprintf(stderr, "zlib failed at %d,%d\n", cx, cz);
        return 1;
      }
      std::string error;
      regions[{floor_div(cx, 32), floor_div(cz, 32)}].add(cx, cz, compressed, &error);
      if (!error.empty()) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
      }
      ++columns;
    }
  }
  for (auto& [key, writer] : regions) {
    const fs::path path = fs::path(anvil->region_dir()) / ("r." + std::to_string(key.first) + "." +
                                                           std::to_string(key.second) + ".mca");
    std::string error;
    if (!writer.write(path, &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    std::printf("  %s (%.1f MiB)\n", path.filename().string().c_str(),
                static_cast<double>(writer.bytes()) / 1048576.0);
  }
  std::printf("wrote %zu column(s), %.1f KB estimated each, %.1f MB total\n", columns,
              static_cast<double>(estimated) / static_cast<double>(columns) / 1024.0,
              static_cast<double>(estimated) / 1048576.0);
  return 0;
}
