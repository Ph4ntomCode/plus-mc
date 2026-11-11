// SPDX-License-Identifier: GPL-3.0-or-later
//
// Author a custom-terrain world: real Anvil region files that the server loads
// through `--world <dir>` exactly like a world a player built by hand.
//
// Why this exists. Every world this tree could serve until now was one of three
// things: the void (`MemoryWorld`'s fabricated air columns), the flat world, or
// terrain this server generated itself (`--worldgen <seed>`). None of them is
// the path a host actually takes — point `--world` at a directory someone else
// made, load the columns off disk, live in them — and that path has its own cost
// (every column comes off disk, then gets relit before it can be sent) and its
// own failure modes (a state the encoder cannot resolve voids the chunk on the
// wire; a heightmap the file does not carry is a client that cannot predict
// collisions). `tests/custom_world_test.py` is the test; the manifest this writes
// beside the world is what lets it assert "the custom terrain arrived intact"
// against the *design* instead of a second copy of the coordinates.
//
// What it builds, deterministically (no RNG: the same bytes every run, and the
// same bytes the test's own regeneration compares against):
//
//   * a grass plain at y=64 over dirt and stone, bedrock at y=-64,
//   * a stepped snow-capped pyramid at the centre of the map (4 blocks a step,
//     reaching y=96), with a cobblestone-and-glass house on its summit,
//   * a lake (sand floor at y=58, water up to y=62) with a sand beach around it,
//   * a sandstone-capped mesa in one corner,
//   * a carved cave tunnel, a vertical shaft to the surface, a lava pool at the
//     bottom of it, an iron vein and a gravel pocket in the stone,
//   * oak trees on the plain.
//
// Light is deliberately absent from the file. A hand-authored world usually has
// none, and serving one is then the server's own work (`ensure_chunk_light`
// relights the column before it goes out) — measuring that is the point, not
// hiding it.
//
// Cells go in through `Chunk::set_block_state` and the derived stores (heightmaps
// and the random-tick count) through `Chunk::rebuild_after_bulk_write`, so the
// file carries exactly what a played-in column carries. Every state is checked
// against the server's own block table first: a state the encoder cannot resolve
// makes the server void the chunk on the wire rather than fail loudly, which
// would turn a typo here into a mystery there.
//
// Usage:
//   ./build/plusmc-gen-custom-world --out /tmp/customworld [--chunks 7]
//   ./build/plusmc --world /tmp/customworld --no-flat-world    # then play in it
//
// `--chunks N` builds an NxN block of chunks from (0,0) (default 7, i.e. 112x112
// blocks and 49 columns). The manifest lands at
// `<out>/custom_world_features.json`.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <zlib.h>

#include "world/anvil.h"
#include "world/chunk.h"
#include "world/registry.h"

namespace {

namespace fs = std::filesystem;
using mc::world::AnvilWorld;
using mc::world::BlockStateKey;
using mc::world::Chunk;

// --- the design's own numbers, shared by the builder and the manifest -------
constexpr int kMinY = -64;      // overworld floor (kOverworldMinSectionY * 16)
constexpr int kPlainY = 64;     // surface block at y=63
constexpr int kLakeFloorY = 58; // lake surface block at y=57, water [58, 62)
constexpr int kLakeSurfaceY = 62;
constexpr int kPyramidApexY = 96; // summit surface block at y=95
constexpr int kPyramidRing = 32;  // the pyramid's steps reach the plain here
constexpr int kMesaTopY = 78;     // mesa surface block at y=77
constexpr int kHouseHalf = 4;     // walls on the perimeter of a 9x9 square
// The seed written into level.dat. The tool does not use it to generate anything
// (the design below is hand-authored), but a real level.dat carries the seed its
// terrain came from, and the server reads it back for Join Game's `hashedSeed`.
// `tests/custom_world_test.py` asserts that value on the wire.
constexpr int64_t kCustomWorldSeed = 20261018;

// The lake's block rectangle and the beach margin around it.
bool in_lake(int x, int z, int w) {
  return x >= 4 && x < 28 && z >= w - 32 && z < w - 8;
}
bool near_lake(int x, int z, int w) {
  return x >= 1 && x < 31 && z >= w - 35 && z < w - 5;
}
bool in_mesa(int x, int z, int w) {
  return x >= w - 24 && z < 40;
}

// Where the *surface block* of column (x, z) sits, in world y: the height the
// top block occupies is this value, and the column's blocks run [kMinY, h).
int surface_height(int x, int z, int w) {
  // The pyramid first, then the features that cut into it (the lake's basin is
  // lower than the steps it meets, the mesa's top is higher than the plain), so
  // each override wins where they overlap.
  int h = kPlainY;
  const int ring = std::max(std::abs(x - w / 2), std::abs(z - w / 2));
  if (ring < kPyramidRing)
    h = kPlainY + ((kPyramidRing - ring) / 4) * 4;
  if (in_mesa(x, z, w))
    h = kMesaTopY;
  if (in_lake(x, z, w))
    h = kLakeFloorY;
  return h;
}

const BlockStateKey kAir{"minecraft:air", ""};
const BlockStateKey kBedrock{"minecraft:bedrock", ""};
const BlockStateKey kStone{"minecraft:stone", ""};
const BlockStateKey kDirt{"minecraft:dirt", ""};
const BlockStateKey kGrass{"minecraft:grass_block", "snowy=false"};
const BlockStateKey kSand{"minecraft:sand", ""};
const BlockStateKey kSandstone{"minecraft:sandstone", ""};
const BlockStateKey kWater{"minecraft:water", "level=0"};
const BlockStateKey kSnow{"minecraft:snow_block", ""};
const BlockStateKey kLog{"minecraft:oak_log", "axis=y"};
const BlockStateKey kLeaves{"minecraft:oak_leaves", "distance=7,persistent=true,waterlogged=false"};
const BlockStateKey kGlass{"minecraft:glass", ""};
const BlockStateKey kPlanks{"minecraft:oak_planks", ""};
const BlockStateKey kCobble{"minecraft:cobblestone", ""};
const BlockStateKey kIron{"minecraft:iron_ore", ""};
const BlockStateKey kGravel{"minecraft:gravel", ""};
const BlockStateKey kLava{"minecraft:lava", "level=0"};

BlockStateKey top_block(int x, int z, int h, int w) {
  if (h >= 92)
    return kSnow; // the pyramid's snow cap
  if (h == kMesaTopY)
    return kSandstone;
  if (h == kLakeFloorY || near_lake(x, z, w))
    return kSand;
  return kGrass;
}

BlockStateKey sub_block(int x, int z, int h, int w) {
  if (h == kMesaTopY)
    return kSandstone;
  if (h == kLakeFloorY || near_lake(x, z, w))
    return kSand;
  return kDirt;
}

// The trees, fixed: all on the plain, clear of the pyramid, the lake and the
// mesa (the test's manifest asserts their trunks and one canopy cell each).
struct Tree {
  int x, z;
};
constexpr Tree kTrees[] = {{8, 12}, {8, 64}, {20, 12}, {8, 108}, {32, 108}, {100, 108}};
constexpr int kTreeHeight = 5; // trunk blocks, above the surface block

// The cave tunnel: a tube from near the origin towards the far corner, and a
// vertical shaft that drops into a lava pool.
int tunnel_z(int x, int w) {
  const int t = x - 4;
  return 4 + (t * 40) / w;
}

struct Feature {
  const char* name;
  int x, y, z;
  const BlockStateKey* key;
};

// The design as data. Deliberately typed out from the same constants the builder
// uses, and asserted by the test against the chunks the *client* receives: a
// disagreement between this list and the builder is a bug in one of them, and
// the test is what says so.
std::vector<Feature> designed_features(int w) {
  const int hx = w / 2, hz = w / 2;
  const int tunnel_x = 40;
  const int tunnel_y = -19;
  const int shaft_x = w - 8, shaft_z = w / 2;
  std::vector<Feature> f = {
      {"bedrock floor", 8, kMinY, 12, &kBedrock},
      {"plain surface", 8, kPlainY - 1, 12, &kGrass},
      {"plain subsoil", 8, kPlainY - 3, 12, &kDirt},
      {"deep stone", 8, 0, 12, &kStone},
      {"pyramid apex", hx, kPyramidApexY - 1, hz, &kSnow},
      {"pyramid step", hx - 20, kPlainY + 12 - 1, hz, &kGrass},
      {"house wall", hx - kHouseHalf, kPyramidApexY + 1, hz - kHouseHalf, &kCobble},
      {"house window", hx, kPyramidApexY + 1, hz - kHouseHalf, &kGlass},
      {"house roof", hx, kPyramidApexY + 3, hz, &kPlanks},
      {"house interior", hx, kPyramidApexY + 1, hz, &kAir},
      {"lake water", 16, kLakeSurfaceY - 1, w - 22, &kWater},
      {"lake floor", 16, kLakeFloorY - 1, w - 22, &kSand},
      {"beach sand", 16, kPlainY - 1, w - 34, &kSand},
      {"mesa cap", w - 4, kMesaTopY - 1, 8, &kSandstone},
      {"cave tunnel air", tunnel_x, tunnel_y, tunnel_z(tunnel_x, w), &kAir},
      {"shaft mouth", shaft_x, kPlainY - 1, shaft_z, &kAir},
      {"shaft air", shaft_x, -21, shaft_z, &kAir},
      {"lava pool", shaft_x, -24, shaft_z, &kLava},
      {"iron vein", 20, -45, 20, &kIron},
      {"gravel pocket", 80, -45, 88, &kGravel},
      {"tree trunk", kTrees[0].x, kPlainY + 2, kTrees[0].z, &kLog},
      {"tree canopy", kTrees[0].x + 2, kPlainY + 4, kTrees[0].z, &kLeaves},
  };
  return f;
}

// --- the region file -------------------------------------------------------

void put_be32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v >> 24));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v));
}

// A fresh region file: header (1024 location words + 1024 timestamps), then
// records from sector 2 on, exactly the layout `world/anvil.cpp` reads and
// writes (u32 length, u8 compression id 2 = zlib, deflated NBT).
class RegionWriter {
public:
  void add(int cx, int cz, const std::vector<uint8_t>& zlib_payload, std::string* error) {
    const int lx = cx & 31, lz = cz & 31;
    const uint32_t length = static_cast<uint32_t>(1 + zlib_payload.size());
    const uint32_t sectors = (length + 4 + 4095) / 4096;
    if (sectors > 255) {
      *error = "chunk record needs more than 255 sectors";
      return;
    }
    const uint32_t sector = 2 + static_cast<uint32_t>(body_.size() / 4096);
    const size_t idx = static_cast<size_t>(lz * 32 + lx) * 4;
    locations_[idx] = static_cast<uint8_t>((sector << 8 | sectors) >> 24);
    locations_[idx + 1] = static_cast<uint8_t>((sector << 8 | sectors) >> 16);
    locations_[idx + 2] = static_cast<uint8_t>((sector << 8 | sectors) >> 8);
    locations_[idx + 3] = static_cast<uint8_t>(sector << 8 | sectors);
    timestamps_[idx] = timestamps_[idx + 1] = timestamps_[idx + 2] = 0;
    timestamps_[idx + 3] = 1; // a non-zero timestamp, like any written record
    put_be32(body_, length);
    body_.push_back(2);
    body_.insert(body_.end(), zlib_payload.begin(), zlib_payload.end());
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
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
      *error = "rename to " + path.string() + " failed: " + ec.message();
      return false;
    }
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

// --- the world -------------------------------------------------------------

void build_chunk(Chunk& c, int cx, int cz, int w) {
  c = Chunk{};
  c.x = cx;
  c.z = cz;
  c.min_section_y = kMinY / 16; // -4
  const int sections = 24;      // 384 blocks: the overworld's own shape
  c.sections.resize(static_cast<size_t>(sections));
  for (int i = 0; i < sections; ++i) {
    mc::world::Section& s = c.sections[static_cast<size_t>(i)];
    s.y = c.min_section_y + i;
    s.has_blocks = true;
    s.block_palette = {kAir};
    s.block_cells.assign(mc::world::kBlockCellsPerSection, 0);
    s.has_biomes = true;
    s.biome_palette = {"minecraft:plains"};
    s.biome_cells.assign(mc::world::kBiomeCellsPerSection, 0);
  }
  // One write in world coordinates, dropped when it lands outside this chunk:
  // the features are designed as world geometry and clipped per column, the same
  // way the generator's own feature stage works.
  auto put = [&](int x, int y, int z, const BlockStateKey& key) {
    if ((x >> 4) != cx || (z >> 4) != cz)
      return;
    c.set_block_state(x & 15, y, z & 15, key);
  };

  for (int x = cx * 16; x < cx * 16 + 16; ++x) {
    for (int z = cz * 16; z < cz * 16 + 16; ++z) {
      const int h = surface_height(x, z, w);
      for (int y = kMinY; y < h; ++y) {
        if (y == kMinY)
          put(x, y, z, kBedrock);
        else if (y >= h - 1)
          put(x, y, z, top_block(x, z, h, w));
        else if (y >= h - 4)
          put(x, y, z, sub_block(x, z, h, w));
        else
          put(x, y, z, kStone);
      }
      if (in_lake(x, z, w)) {
        for (int y = h; y < kLakeSurfaceY; ++y)
          put(x, y, z, kWater);
      }
    }
  }

  // The cave tunnel, then the shaft that drops into the lava pool at its bottom.
  for (int x = 4; x < w - 4; ++x) {
    const int z = tunnel_z(x, w);
    for (int y = -21; y <= -18; ++y)
      for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
          put(x + dx, y, z + dz, kAir);
  }
  // The shaft drops all the way into the lava chamber below it (its lowest air
  // layer is y=-22), so the pool is reachable from the surface rather than sealed
  // under three blocks of stone.
  const int sx = w - 8, sz = w / 2;
  for (int y = -22; y <= kPlainY - 1; ++y)
    for (int dz = -1; dz <= 1; ++dz)
      for (int dx = -1; dx <= 1; ++dx)
        put(sx + dx, y, sz + dz, kAir);
  for (int y = -24; y <= -22; ++y)
    for (int dz = -2; dz <= 2; ++dz)
      for (int dx = -2; dx <= 2; ++dx)
        put(sx + dx, y, sz + dz, y <= -23 ? kLava : kAir);

  // Ore blobs in the deep stone: a Manhattan ball, unconditionally (every cell
  // they reach is stone by construction — the surface never comes near these).
  auto ball = [&](int ox, int oy, int oz, const BlockStateKey& key) {
    for (int dy = -3; dy <= 3; ++dy)
      for (int dz = -3; dz <= 3; ++dz)
        for (int dx = -3; dx <= 3; ++dx)
          if (std::abs(dx) + std::abs(dy) + std::abs(dz) <= 4)
            put(ox + dx, oy + dy, oz + dz, key);
  };
  ball(20, -45, 20, kIron);
  ball(80, -45, 88, kGravel);

  // Trees: a trunk with a two-layer canopy, the top layer pulled in.
  for (const Tree& t : kTrees) {
    for (int i = 0; i < kTreeHeight; ++i)
      put(t.x, kPlainY + i, t.z, kLog);
    for (int dy = 0; dy <= 2; ++dy) {
      const int reach = dy == 2 ? 1 : 2;
      const int y = kPlainY + kTreeHeight - 2 + dy;
      for (int dz = -reach; dz <= reach; ++dz)
        for (int dx = -reach; dx <= reach; ++dx) {
          if (dx == 0 && dz == 0 && dy == 0)
            continue; // the trunk's own cell
          if (std::abs(dx) == reach && std::abs(dz) == reach)
            continue; // corners
          put(t.x + dx, y, t.z + dz, kLeaves);
        }
    }
  }

  // The house on the summit: cobblestone walls, a glass window in each of the
  // four walls, an oak-plank roof, hollow inside.
  const int hx = w / 2, hz = w / 2;
  for (int y = kPyramidApexY; y <= kPyramidApexY + 2; ++y) {
    for (int dz = -kHouseHalf; dz <= kHouseHalf; ++dz) {
      for (int dx = -kHouseHalf; dx <= kHouseHalf; ++dx) {
        const bool perimeter = std::abs(dx) == kHouseHalf || std::abs(dz) == kHouseHalf;
        if (!perimeter)
          continue;
        const bool window = y == kPyramidApexY + 1 && ((dx == 0 && std::abs(dz) == kHouseHalf) ||
                                                       (dz == 0 && std::abs(dx) == kHouseHalf));
        put(hx + dx, y, hz + dz, window ? kGlass : kCobble);
      }
    }
  }
  for (int dz = -kHouseHalf; dz <= kHouseHalf; ++dz)
    for (int dx = -kHouseHalf; dx <= kHouseHalf; ++dx)
      put(hx + dx, kPyramidApexY + 3, hz + dz, kPlanks);

  // The derived stores a played-in column carries: heightmaps (created here,
  // because the file must not go out without them) and the ticking count.
  c.rebuild_after_bulk_write();
}

bool write_manifest(const fs::path& path, int chunks, const std::vector<Feature>& features,
                    std::string* error) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    *error = "cannot create " + path.string();
    return false;
  }
  out << "{\n  \"chunks\": " << chunks << ",\n  \"seed\": " << kCustomWorldSeed
      << ",\n  \"chunk_span\": [" << chunks << ", " << chunks
      << "],\n  \"region\": \"r.0.0.mca\",\n  \"note\": \"generated by plusmc-gen-custom-world;"
         " coordinates are world blocks, `name`/`properties` are the state the design puts"
         " there\",\n  \"features\": [\n";
  for (size_t i = 0; i < features.size(); ++i) {
    const Feature& f = features[i];
    out << "    {\"name\": \"" << f.name << "\", \"x\": " << f.x << ", \"y\": " << f.y
        << ", \"z\": " << f.z << ", \"chunk\": [" << (f.x >> 4) << ", " << (f.z >> 4)
        << "], \"block\": \"" << f.key->name << "\", \"properties\": \"" << f.key->properties
        << "\"}" << (i + 1 == features.size() ? "\n" : ",\n");
  }
  out << "  ]\n}\n";
  return true;
}

} // namespace

int main(int argc, char** argv) {
  fs::path out_dir;
  int chunks = 7;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const char* next = i + 1 < argc ? argv[i + 1] : nullptr;
    if (arg == "--out" && next != nullptr) {
      out_dir = next;
      ++i;
    } else if (arg == "--chunks" && next != nullptr) {
      chunks = std::atoi(next);
      ++i;
    } else if (arg == "--help" || arg == "-h") {
      std::printf("usage: %s --out DIR [--chunks N]\n", argv[0]);
      return 0;
    } else {
      std::fprintf(stderr, "%s: unknown argument '%s'\n", argv[0], arg.c_str());
      return 2;
    }
  }
  if (out_dir.empty() || chunks < 1 || chunks > 32) {
    std::fprintf(stderr, "usage: %s --out DIR [--chunks N]  (N in [1, 32])\n", argv[0]);
    return 2;
  }
  const int w = chunks * 16; // blocks across

  // Every state this tool writes, checked against the server's own table: the
  // encoder resolves states by name+properties, and one it cannot resolve voids
  // the chunk on the wire instead of failing here.
  const mc::world::BlockStateTable& table = mc::world::full_block_states();
  for (const Feature& f : designed_features(w)) {
    if (!table.lookup(*f.key)) {
      std::fprintf(stderr, "%s: '%s' with properties '%s' is not in the block table\n", argv[0],
                   f.key->name.c_str(), f.key->properties.c_str());
      return 1;
    }
  }

  std::error_code ec;
  fs::create_directories(out_dir, ec);
  auto world = AnvilWorld::open_world(out_dir);
  if (!world) {
    std::fprintf(stderr, "%s: cannot open world at %s\n", argv[0], out_dir.string().c_str());
    return 1;
  }
  if (!world->ensure_level_dat(4903, kCustomWorldSeed)) {
    std::fprintf(stderr, "%s: cannot write level.dat under %s\n", argv[0],
                 out_dir.string().c_str());
    return 1;
  }

  RegionWriter region;
  std::string error;
  size_t column_cells = 0;
  for (int cx = 0; cx < chunks; ++cx) {
    for (int cz = 0; cz < chunks; ++cz) {
      Chunk chunk;
      build_chunk(chunk, cx, cz, w);
      for (const mc::world::Section& s : chunk.sections)
        column_cells += s.block_cells.size();
      const std::vector<uint8_t> nbt =
          mc::world::nbt::serialize(mc::world::serialize_chunk_nbt(chunk), "");
      std::vector<uint8_t> compressed;
      if (!zlib_compress(nbt, &compressed)) {
        std::fprintf(stderr, "%s: zlib failed for chunk (%d,%d)\n", argv[0], cx, cz);
        return 1;
      }
      region.add(cx, cz, compressed, &error);
      if (!error.empty()) {
        std::fprintf(stderr, "%s: %s\n", argv[0], error.c_str());
        return 1;
      }
    }
  }
  const fs::path region_path = world->region_dir() / "r.0.0.mca";
  if (!region.write(region_path, &error)) {
    std::fprintf(stderr, "%s: %s\n", argv[0], error.c_str());
    return 1;
  }
  const std::vector<Feature> features = designed_features(w);
  if (!write_manifest(out_dir / "custom_world_features.json", chunks, features, &error)) {
    std::fprintf(stderr, "%s: %s\n", argv[0], error.c_str());
    return 1;
  }

  std::printf("authored %dx%d chunks (%d blocks across, %zu cells) into %s\n", chunks, chunks, w,
              column_cells, out_dir.string().c_str());
  std::printf("  region   %s (%.1f KiB)\n", region_path.string().c_str(),
              static_cast<double>(region.bytes()) / 1024.0);
  std::printf("  manifest %s (%zu features)\n",
              (out_dir / "custom_world_features.json").string().c_str(), features.size());
  std::printf("  level.dat seed %lld\n", static_cast<long long>(kCustomWorldSeed));
  return 0;
}
