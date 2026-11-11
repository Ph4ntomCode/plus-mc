// SPDX-License-Identifier: GPL-3.0-or-later
//
// The world stores' height reads: a block write must be visible to the very
// next height question, through *every* path a caller can ask by, and both
// backends must answer alike.
//
// The bug this pins, from the audit of the lazy-heightmap rewrite: the query
// was left in two forms — `chunk_motion_blocking_height(const Chunk&)`, which
// answered the stored (possibly stale) entry, and `(Chunk&)`, which brought the
// column current first. Correctness therefore rested on which overload a caller
// happened to bind, and the two stores bound different ones: AnvilWorld's const
// readers took the stale form, so a file-backed world handed back the height
// from *before* the write, while MemoryWorld got the fresh value only because
// its chunk map is `mutable`. There is one form now, over one owner of the
// refresh rule (`Chunk` in world/chunk.h: a write marks a column, a read asks
// through `height_at`, and a copy leaves through `snapshot` — the two public
// ways in, the primitives being private), and this file exercises all four ways
// a height leaves a store:
//
//   * the interface read itself — `world.motion_blocking_height` / `world_surface_height`
//     on a `const WorldInterface&`, which is how weather, fire's rain branch and
//     the natural spawner ask;
//   * a live `modify_chunk` reference, which is how the relight takes a column;
//   * the copy `load_chunk` hands out, read the *raw* way the wire encoder reads
//     it (`heightmaps` with no query in between), so a client is sent the height
//     its blocks imply;
//   * a column read back from disk through a freshly opened AnvilWorld, which is
//     what the flush's serializer snapshot decides.
//
// Usage: heightmap_test

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "world/anvil.h"
#include "world/chunk.h"
#include "world/memory_world.h"
#include "world/world_interface.h"

namespace {

int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECK_EQ(actual, expected)                                                                 \
  do {                                                                                             \
    const auto a_ = (actual);                                                                      \
    const auto e_ = (expected);                                                                    \
    if (!(a_ == e_)) {                                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s == %s (got %lld, want %lld)\n", __FILE__, __LINE__,     \
                   #actual, #expected, static_cast<long long>(a_), static_cast<long long>(e_));    \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

mc::world::BlockStateKey key(const char* name) {
  return mc::world::BlockStateKey{name, ""};
}

// The column's *stored* heightmap entry, decoded straight out of the long array
// — the view the wire encoder has, which reads `chunk.heightmaps` with no query
// in between. Deliberately independent of the store's own reader: this is the
// test decoding the same bytes, at the bit width `recompute_column_heightmaps`
// packs with (`Heightmap(<height + 1>)`).
int stored_height(const mc::world::Chunk& chunk, mc::world::HeightmapType type, int x, int z) {
  const int min_y = chunk.min_section_y * 16;
  int bits = 1;
  while ((1 << bits) < static_cast<int>(chunk.sections.size()) * 16 + 1)
    ++bits;
  const int per_long = 64 / bits;
  const int idx = z * 16 + x;
  for (const auto& [t, longs] : chunk.heightmaps) {
    if (t != type)
      continue;
    const size_t li = static_cast<size_t>(idx / per_long);
    if (li >= longs.size())
      return min_y;
    const int off = (idx % per_long) * bits;
    const uint64_t value = static_cast<uint64_t>(longs[li]) >> off;
    return min_y + static_cast<int>(value & ((1ULL << bits) - 1ULL));
  }
  return min_y; // the type is absent: the reader answers the min build height
}

// One write, then the two heights the write is supposed to lead to.
struct Step {
  int y;
  const char* block;
  int motion;  // `Level.getHeight(MOTION_BLOCKING)` — where the rain lands
  int surface; // `Level.getHeight(WORLD_SURFACE)` — one past the top non-air cell
};

// A column built and taken apart, one write at a time, covering the three ways a
// write can move a height and the two ways it cannot:
//   * a floor appears under an all-air column (both maps move);
//   * a plant on top of it (the surface map moves, the rain map does not — a
//     torch is not a roof);
//   * a write *below* the top (neither map moves);
//   * a fluid, which counts for motion blocking (`blocksMotion() ||
//     !fluidState.isEmpty()`);
//   * removing the roof, the plant and the floors again (both maps fall back, to
//     the next thing below and finally to the dimension's min build height).
const Step kSteps[] = {
    {64, "minecraft:stone", 65, 65}, // a floor at 64: one past it
    {65, "minecraft:torch", 65, 66}, // a plant: surface only
    {60, "minecraft:stone", 65, 66}, // below the top: neither
    {70, "minecraft:water", 71, 71}, // a fluid roof: motion blocking
    {70, "minecraft:air", 65, 66},   // roof removed: back to the plant
    {65, "minecraft:air", 65, 65},   // plant removed: back to the floor
    {64, "minecraft:air", 61, 61},   // top floor removed: what the lower one holds
    {60, "minecraft:air", -64, -64}, // column empty: the min build height
};

// Replays `kSteps` against one store and answers with the pair of heights it read
// after each write. Both stores are driven by this one function, so the two
// ladders can be compared step for step — the point being that neither backend's
// answers depend on how a caller reached the column.
std::vector<std::pair<int, int>> write_ladder(mc::world::WorldInterface& world) {
  std::vector<std::pair<int, int>> ladder;
  for (const Step& step : kSteps) {
    if (!world.set_block_state(0, 0, 3, step.y, 5, key(step.block))) {
      std::fprintf(stderr, "FAIL write (%d) refused\n", step.y);
      ++failures;
    }
    ladder.emplace_back(world.motion_blocking_height(3, 5), world.world_surface_height(3, 5));
  }
  return ladder;
}

const std::vector<std::pair<int, int>> kExpectedLadder = [] {
  std::vector<std::pair<int, int>> want;
  for (const Step& step : kSteps)
    want.emplace_back(step.motion, step.surface);
  return want;
}();

void check_ladder(const char* who, const std::vector<std::pair<int, int>>& ladder) {
  if (ladder == kExpectedLadder)
    return;
  ++failures;
  std::fprintf(stderr, "FAIL %s: the height ladder is not vanilla's\n", who);
  for (size_t i = 0; i < ladder.size() && i < kExpectedLadder.size(); ++i) {
    if (ladder[i] != kExpectedLadder[i])
      std::fprintf(stderr, "  step %zu (y=%d %s): got motion %d surface %d, want %d %d\n", i,
                   kSteps[i].y, kSteps[i].block, ladder[i].first, ladder[i].second,
                   kExpectedLadder[i].first, kExpectedLadder[i].second);
  }
}

// The height a *copy* of the column carries in its own arrays, and the height a
// live reference answers through the query. Both must be the write's height: the
// first is what the wire encoder reads, the second is what the light engine and
// the weather ask.
void check_read_paths(const char* who, mc::world::WorldInterface& world, int motion, int surface) {
  auto copy = world.load_chunk(0, 0);
  CHECK(copy.has_value());
  if (copy) {
    CHECK_EQ(stored_height(*copy, mc::world::HeightmapType::MotionBlocking, 3, 5), motion);
    CHECK_EQ(stored_height(*copy, mc::world::HeightmapType::WorldSurface, 3, 5), surface);
    CHECK_EQ(mc::world::chunk_motion_blocking_height(*copy, 3, 5), motion);
    CHECK_EQ(mc::world::chunk_world_surface_height(*copy, 3, 5), surface);
  } else {
    std::fprintf(stderr, "FAIL %s: load_chunk returned nothing\n", who);
  }
  auto live = world.modify_chunk(0, 0);
  CHECK(live.has_value());
  if (live) {
    CHECK_EQ(mc::world::chunk_motion_blocking_height(live->get(), 3, 5), motion);
    CHECK_EQ(mc::world::chunk_world_surface_height(live->get(), 3, 5), surface);
  }
}

// The in-memory store: a write through the interface, then the same four reads.
void test_memory_world() {
  std::printf("-- memory world\n");
  mc::world::MemoryWorld world;
  // The interface read, not the class's own — the path every weather/fire/spawn
  // question takes. Air column first, to pin the baseline.
  const mc::world::WorldInterface& iface = world;
  CHECK_EQ(iface.motion_blocking_height(3, 5), -64);
  CHECK_EQ(iface.world_surface_height(3, 5), -64);
  CHECK_EQ(iface.motion_blocking_height(3 + 16, 5), -64); // the next column over

  const std::vector<std::pair<int, int>> ladder = write_ladder(world);
  check_ladder("memory world", ladder);

  // The same four reads over a column that holds something: a floor with a plant
  // on it, which is where the two maps differ.
  CHECK(world.set_block_state(0, 0, 3, 64, 5, key("minecraft:stone")));
  CHECK(world.set_block_state(0, 0, 3, 65, 5, key("minecraft:torch")));
  check_read_paths("memory world", world, 65, 66);

  // Writes that cannot move a height must not move it, and for these the column
  // is not even marked: another block with the same three heightmap predicates
  // (planks over stone — both a full floor where the rain lands), and the same
  // block with another property. The answer has to stay on the floor the last
  // write left, not fall back to the stored entry or to air.
  CHECK(world.set_block_state(0, 0, 3, 64, 5, key("minecraft:stone")));
  CHECK_EQ(iface.motion_blocking_height(3, 5), 65);
  CHECK(world.set_block_state(0, 0, 3, 64, 5, key("minecraft:oak_planks")));
  CHECK_EQ(iface.motion_blocking_height(3, 5), 65);
  CHECK(world.set_block_state(0, 0, 3, 64, 5,
                              mc::world::BlockStateKey{"minecraft:oak_stairs", "facing=north"}));
  CHECK(world.set_block_state(0, 0, 3, 64, 5,
                              mc::world::BlockStateKey{"minecraft:oak_stairs", "facing=east"}));
  CHECK_EQ(iface.motion_blocking_height(3, 5), 65); // the floor, not the plant
  CHECK_EQ(iface.world_surface_height(3, 5), 66);   // the plant at 65 is still the top

  // The ends of the build range. An entry is packed at the width of the
  // dimension's height (9 bits for 384), so the highest height a block can imply
  // has to survive the packing, and the outermost cell (15, 15) has to index its
  // own entry (bit z*16 + x = 255, the last bit of the four words).
  CHECK(world.set_block_state(0, 0, 15, 319, 15, key("minecraft:stone")));
  CHECK_EQ(iface.motion_blocking_height(15, 15), 320);
  CHECK(world.set_block_state(0, 0, 15, 319, 15, key("minecraft:air")));
  CHECK_EQ(iface.motion_blocking_height(15, 15), -64); // back to an empty column
  CHECK(world.set_block_state(0, 0, 0, -64, 0, key("minecraft:stone")));
  CHECK_EQ(iface.motion_blocking_height(0, 0), -63); // the first row is a floor too

  // Ordering across columns: a write to one column must not move another's
  // answer, and the copy boundary (`Chunk::snapshot`, the one way a column
  // leaves a store) has to bring *every* marked column current — three columns
  // are behind at this point, so it has to find all three, not the first one it
  // sees.
  CHECK(world.set_block_state(0, 0, 7, 70, 9, key("minecraft:stone")));
  auto copy = world.load_chunk(0, 0);
  CHECK(copy.has_value());
  if (copy) {
    CHECK_EQ(stored_height(*copy, mc::world::HeightmapType::MotionBlocking, 7, 9), 71);
    CHECK_EQ(stored_height(*copy, mc::world::HeightmapType::MotionBlocking, 15, 15), -64);
    CHECK_EQ(stored_height(*copy, mc::world::HeightmapType::MotionBlocking, 0, 0), -63);
    CHECK_EQ(stored_height(*copy, mc::world::HeightmapType::MotionBlocking, 3, 5), 65);
  }
  CHECK_EQ(iface.motion_blocking_height(3, 5), 65); // untouched by the other column
  // A repeated write and a repeated read change nothing: the first read cleared
  // the mark, and a write that flips no predicate is not marked again.
  CHECK(world.set_block_state(0, 0, 3, 64, 5, key("minecraft:oak_planks")));
  CHECK_EQ(iface.motion_blocking_height(3, 5), 65);
  CHECK_EQ(iface.motion_blocking_height(3, 5), 65);
  // A column outside the 16x16 (the guard on the dirty word index — the bit is
  // z*16 + x, so an unguarded 16 would index word 4 of four): the min build
  // height, like every read of a column that does not exist.
  if (copy) {
    CHECK_EQ(mc::world::chunk_motion_blocking_height(*copy, 16, 16), -64);
    CHECK_EQ(mc::world::chunk_motion_blocking_height(*copy, -1, -1), -64);
    CHECK_EQ(mc::world::chunk_world_surface_height(*copy, 255, 255), -64);
  }
}

// The file-backed store, on a scratch region directory: the same ladder through
// the const interface, then the flush and a *reopened* world — the one read that
// sees what the serializer snapshot wrote, and so what a restarted server (or any
// other reader of the file) would answer.
void test_anvil_world(const std::filesystem::path& region_dir) {
  std::printf("-- anvil world (%s)\n", region_dir.string().c_str());
  auto world = mc::world::AnvilWorld::open_region_dir(region_dir);
  CHECK(world != nullptr);
  if (!world)
    return;
  // Air column first: the synthesized column and the fall-back answer for a
  // column that is not on disk are both the dimension's min build height.
  const mc::world::WorldInterface& iface = *world;
  CHECK_EQ(iface.motion_blocking_height(3, 5), -64);
  CHECK_EQ(iface.motion_blocking_height(3 + 16, 5), -64);

  const std::vector<std::pair<int, int>> ladder = write_ladder(*world);
  check_ladder("anvil world", ladder);

  // The reads that used to bind the stale overload: `AnvilWorld::motion_blocking_height`
  // under the world lock, over the cached column, and the copy `load_chunk` hands
  // the encoder. Both answered the column's stored pre-write heightmap, while
  // MemoryWorld answered the blocks above.
  CHECK(world->set_block_state(0, 0, 3, 64, 5, key("minecraft:stone")));
  CHECK(world->set_block_state(0, 0, 3, 65, 5, key("minecraft:torch")));
  check_read_paths("anvil world", *world, 65, 66);

  // A second dirty column, at the very top of the build range, so the flush
  // serializes two marked columns and the packed height (384, the largest number
  // 9 bits can hold for this dimension) survives NBT and zlib on the way out.
  CHECK(world->set_block_state(0, 0, 15, 319, 15, key("minecraft:stone")));

  // Then flush both: what lands on disk is the serializer's snapshot, so the
  // reopened world's answers are the file's.
  CHECK(world->save_all());
  auto reopened = mc::world::AnvilWorld::open_region_dir(region_dir);
  CHECK(reopened != nullptr);
  if (reopened) {
    const mc::world::WorldInterface& reopened_iface = *reopened;
    CHECK_EQ(reopened_iface.motion_blocking_height(3, 5), 65);
    CHECK_EQ(reopened_iface.world_surface_height(3, 5), 66);
    CHECK_EQ(reopened_iface.motion_blocking_height(15, 15), 320);
    auto copy = reopened->load_chunk(0, 0);
    CHECK(copy.has_value());
    if (copy) {
      CHECK_EQ(stored_height(*copy, mc::world::HeightmapType::MotionBlocking, 3, 5), 65);
      CHECK_EQ(stored_height(*copy, mc::world::HeightmapType::WorldSurface, 3, 5), 66);
      CHECK_EQ(stored_height(*copy, mc::world::HeightmapType::MotionBlocking, 15, 15), 320);
    }
  }
}

// The audit's own scenario on real vanilla data: a *fixture* overworld column,
// written and then read back through the same const interface the weather, fire
// and the spawner use. The file's stored heightmap says where the surface is;
// the write has to move it, and the escaping copy has to carry the moved value.
// Nothing is saved here — the fixture region file is committed test data.
void test_anvil_fixture(const std::string& fixtures_dir) {
  std::printf("-- anvil fixture (a real 26.2 region file)\n");
  if (fixtures_dir.empty()) {
    std::printf("  SKIP no fixtures directory given on the command line\n");
    return;
  }
  const std::string region = fixtures_dir + "/ow3region";
  auto world = mc::world::AnvilWorld::open_region_dir(region);
  if (!world) {
    std::printf("  SKIP ow3region fixture missing (%s)\n", region.c_str());
    return;
  }
  const int cx = -2, cz = -1; // the fixture chunk the light test also reads
  const mc::world::WorldInterface& iface = *world;

  // A column of the real chunk with generated terrain and room above it, so the
  // write lands inside the build range whatever the fixture holds.
  int x = -1, z = -1, before = 0;
  for (int lx = 0; lx < 16 && x < 0; ++lx) {
    for (int lz = 0; lz < 16 && x < 0; ++lz) {
      const int h = iface.motion_blocking_height(cx * 16 + lx, cz * 16 + lz);
      if (h > -60 && h < 300) {
        x = lx;
        z = lz;
        before = h;
      }
    }
  }
  CHECK(x >= 0);
  if (x < 0)
    return;
  const int ax = cx * 16 + x;
  const int az = cz * 16 + z;
  // A block in the cell the rain reaches (the file's own surface height).
  CHECK(world->set_block_state(cx, cz, x, before, z, key("minecraft:stone")));
  CHECK_EQ(iface.motion_blocking_height(ax, az), before + 1);
  CHECK_EQ(iface.world_surface_height(ax, az), before + 1);
  auto copy = world->load_chunk(cx, cz);
  CHECK(copy.has_value());
  if (copy) {
    CHECK_EQ(stored_height(*copy, mc::world::HeightmapType::MotionBlocking, x, z), before + 1);
    CHECK_EQ(stored_height(*copy, mc::world::HeightmapType::WorldSurface, x, z), before + 1);
  }
  // And a column of the same chunk the write did not touch still answers the
  // file's own value: the mark is per column, not per chunk. A second world over
  // the same (unsaved) region file is that file's answer.
  auto from_disk = mc::world::AnvilWorld::open_region_dir(region);
  CHECK(from_disk != nullptr);
  if (from_disk) {
    const mc::world::WorldInterface& disk = *from_disk;
    const int other_x = (x + 1) % 16; // never the written column
    const int other_z = (z + 7) % 16;
    CHECK_EQ(iface.motion_blocking_height(cx * 16 + other_x, cz * 16 + other_z),
             disk.motion_blocking_height(cx * 16 + other_x, cz * 16 + other_z));
  }
}

} // namespace

int main(int argc, char** argv) {
  const std::string fixtures_dir = argc > 1 ? argv[1] : "";
  std::printf("heightmap_test\n");
  test_memory_world();
  test_anvil_fixture(fixtures_dir);

  const std::filesystem::path scratch =
      std::filesystem::temp_directory_path() /
      ("plusmc-heightmap-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const std::filesystem::path region_dir = scratch / "region";
  std::error_code ec;
  std::filesystem::create_directories(region_dir, ec);
  if (ec) {
    std::fprintf(stderr, "FAIL could not create scratch region dir %s: %s\n",
                 region_dir.string().c_str(), ec.message().c_str());
    ++failures;
  } else {
    test_anvil_world(region_dir);
  }
  // Unconditional: a failing check above must not leave the scratch directory
  // behind.
  std::filesystem::remove_all(scratch, ec);

  if (failures != 0) {
    std::fprintf(stderr, "heightmap_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("heightmap_test: all checks passed\n");
  return 0;
}
