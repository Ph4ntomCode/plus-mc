// SPDX-License-Identifier: GPL-3.0-or-later
//
// Writes the random-tick pass's golden output for a small set of chunks: the run
// parameters and the stream's two ends (`random_tick_meta.tsv`), the scene it
// starts from (`random_tick_scene.tsv`), and one interaction trace per chunk
// (`random_tick_seed<seed>_c<cx>_<cz>.tsv`).
//
// This is **not** a vanilla dump, and cannot be: the pass is driven by
// `Level.randValue`, a per-level LCG whose start is drawn when the level is
// constructed, so there is no jar output to compare against — only this
// runtime's own. What the trace pins is exactly what `tests/random_tick_test.cpp`
// could not pin before: the *sampled positions*. That test drives the pass with
// a hand-set `BlockRandomPos` and asserts the sample's shape (every position
// inside the ticking section), which a wrong y-mask, a wrong column stride or a
// stream that advanced one step too far all satisfy. The trace records each
// draw's position and the state read there, so any of those moves a row.
//
// The trace is a full interaction log, not just the samples: every level read
// and write the pass makes, in order, so a rule's own read set (`getGrowthSpeed`'s
// neighbours, `updateDistance`'s) is pinned too. Rows:
//
//   sample   <tick> <section> <x> <y> <z> <state read> <ticked|idle|skip>
//   read     <tick> <section> <x> <y> <z> <state read> -
//   write    <tick> <section> <x> <y> <z> <state written> <notify|silent>
//   drop     <tick> <section> <x> <y> <z> <state dropped> -
//   schedule <tick> <section> <x> <y> <z> delay=<ticks> -
//
// `sample` is `Level.getBlockRandomPos`'s draw plus the pass's own state read,
// which is identifiable without a hook in the pass: it is the first read after
// the LCG advanced (`rand.value` moved since the previous read), and the tick
// that may follow it draws from the caller's `RandomTickRandom`, never from the
// LCG. So the tool drives the real `tick_chunk_random_blocks` and still sees the
// draw. `ticked` means a write followed the sample; `idle` that the state ticks
// but this draw did nothing; `skip` that the state does not randomly tick at
// all, so the pass never looked at it again.
//
// Usage:
//   plusmc-random-tick-dump <fixtures_dir> [seed] [--diff]
//
// `--diff` writes nothing: it re-runs every chunk in memory, reads what is
// checked in, and prints a patch of each file that would change — the row-level
// patch of a trace shows a shifted stream as a moved row, and exits 1 while the
// fixture differs. The review is
//
//   ./build/plusmc-random-tick-dump <fixtures_dir> --diff   # read the patch
//   ./build/plusmc-random-tick-dump <fixtures_dir>          # then rewrite it
//   ctest -R random_tick                                    # and re-pin it
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "fixture_diff.h"
#include "world/chunk.h"
#include "world/random_tick.h"
#include "world/weather.h" // BlockRandomPos: the level's own LCG, one stream per level
#include "world/worldgen_random.h"

namespace {

using mc::world::BlockStateKey;

// The overworld's section range, as the dimension type describes it.
constexpr int kMinSectionY = -4;
constexpr int kSectionCount = 24;

struct Placement {
  int x; // chunk-relative
  int y; // absolute
  int z; // chunk-relative
  const char* name;
  const char* properties;
};

// The scene a chunk starts from, as chunk-relative coordinates so the same
// layout can be recorded for several chunks. Two of them:
//
//   * "farm" — a full farmland layer under a mixed crop layer in section 10 (the
//     crops are dense enough that a 600-tick run really does grow some, so the
//     trace exercises the crop write and not only the sampling), a log with an
//     anchored, a floating and a persistent leaf in section 8, and a copper
//     block in section 12 (randomly ticking with nothing to do — it still makes
//     its section sample, which is the point of it being there);
//   * "idle" — nothing that random-ticks at all, so the pass reads an empty mask
//     and must not draw: the meta's `randEnd` equals its `randStart`.
std::vector<Placement> scene_placements(const std::string& scene) {
  std::vector<Placement> out;
  if (scene == "farm") {
    for (int x = 0; x < 16; ++x) {
      for (int z = 0; z < 16; ++z) {
        out.push_back({x, 99, z, "minecraft:farmland", "moisture=7"});
        switch ((x + z) % 4) {
        case 0:
          out.push_back({x, 100, z, "minecraft:wheat", "age=0"}); // ticks, and can grow
          break;
        case 1:
          out.push_back({x, 100, z, "minecraft:wheat", "age=7"}); // at its last age: not ticking
          break;
        case 2:
          out.push_back({x, 100, z, "minecraft:carrots", "age=0"}); // a *different* crop family
          break;
        default:
          break;
        }
      }
    }
    out.push_back({8, 70, 1, "minecraft:oak_log", ""});
    out.push_back(
        {9, 70, 1, "minecraft:oak_leaves", "distance=1,persistent=false,waterlogged=false"});
    out.push_back(
        {6, 70, 6, "minecraft:oak_leaves", "distance=7,persistent=true,waterlogged=false"});
    // A row of leaves no log holds up: each is at the decay distance already, so
    // the draws that land on one exercise the drop-and-remove branch (and the
    // neighbour fan-out its write triggers). One line of 16 makes that happen a
    // handful of times in a run rather than once in three fixtures.
    for (int x = 0; x < 16; ++x)
      out.push_back(
          {x, 70, 5, "minecraft:oak_leaves", "distance=7,persistent=false,waterlogged=false"});
    out.push_back({0, 130, 0, "minecraft:copper_block", ""});
    return out;
  }
  // "idle": a log, a mature crop and stone — all three answer false to
  // `block_is_randomly_ticking`, so every section's count is 0.
  out.push_back({8, 70, 8, "minecraft:oak_log", ""});
  out.push_back({4, 100, 4, "minecraft:wheat", "age=7"});
  out.push_back({0, 130, 0, "minecraft:stone", ""});
  return out;
}

struct RecordedChunk {
  int32_t chunk_x;
  int32_t chunk_z;
  int tick_speed;
  int ticks;
  const char* scene;
};

// Four chunks, chosen for what each covers rather than for a scan: the default
// `randomTickSpeed` twice (once at each speed, so the draw count per section is
// pinned), a nonzero mask with the pass switched off entirely, and a chunk where
// nothing ticks at all. The last two leave an empty trace, and their pin is the
// meta's `randEnd`: both must end where they started.
constexpr RecordedChunk kChunks[] = {
    {0, 0, 3, 600, "farm"},
    {1, 0, 1, 600, "farm"},
    {2, 0, 3, 600, "idle"},
    {3, 0, 0, 600, "farm"},
};

constexpr int kBrightness = 15; // `getRawBrightness`: open daytime sky

std::string canonical_state(const BlockStateKey& key) {
  std::string text = key.name;
  if (!key.properties.empty()) {
    text += '[';
    text += key.properties;
    text += ']';
  }
  return text;
}

// The section index a cell is in, as the pass's own `min_section_y + index`
// mapping reads it.
int section_index_of(int y) {
  int quotient = y / 16;
  if (y % 16 != 0 && y < 0)
    --quotient;
  return quotient - kMinSectionY;
}

std::string trace_file_name(int64_t seed, int32_t chunk_x, int32_t chunk_z) {
  return "random_tick_seed" + std::to_string(seed) + "_c" + std::to_string(chunk_x) + "_" +
         std::to_string(chunk_z) + ".tsv";
}

int popcount64(uint64_t value) {
  int count = 0;
  while (value != 0) {
    value &= value - 1;
    ++count;
  }
  return count;
}

// ---- one chunk's run --------------------------------------------------------

struct Row {
  const char* kind;
  int tick;
  int section;
  int x;
  int y;
  int z;
  std::string state;
  std::string extra;
};

std::string format_row(const Row& row) {
  char head[128];
  std::snprintf(head, sizeof(head), "%s\t%d\t%d\t%d\t%d\t%d\t", row.kind, row.tick, row.section,
                row.x, row.y, row.z);
  std::string line = head;
  line += row.state;
  line += '\t';
  line += row.extra;
  return line;
}

struct RunResult {
  std::vector<Row> rows;
  uint64_t mask = 0;
  int32_t rand_start = 0;
  int32_t rand_end = 0;
  size_t samples = 0;
  size_t writes = 0;
  size_t drops = 0;
  size_t schedules = 0;
};

RunResult run_chunk(int64_t seed, const RecordedChunk& recorded,
                    const std::vector<Placement>& scene) {
  mc::world::Chunk world;
  world.x = recorded.chunk_x;
  world.z = recorded.chunk_z;
  world.min_section_y = kMinSectionY;
  world.sections.resize(static_cast<size_t>(kSectionCount));
  for (int i = 0; i < kSectionCount; ++i)
    world.sections[static_cast<size_t>(i)].y = kMinSectionY + i;
  for (const Placement& placement : scene) {
    world.set_block_state(placement.x, placement.y, placement.z,
                          BlockStateKey{placement.name, placement.properties});
  }

  RunResult result;
  for (size_t i = 0; i < world.sections.size() && i < 64; ++i) {
    if (world.sections[i].random_tick_blocks > 0)
      result.mask |= 1ull << i;
  }

  // The level's own LCG, as `Level.randValue` is seeded at construction.
  mc::world::WgLegacyRandom level_random(seed);
  mc::world::BlockRandomPos rand;
  rand.value = level_random.next_int();
  result.rand_start = rand.value;

  // The caller's stream (`BlockState.randomTick`'s `random`): the crop's one
  // `nextInt`, and the leaf loot rolls a caller resolves. A fresh one per chunk,
  // so a chunk's trace does not depend on the order chunks are recorded in.
  mc::world::WgLegacyRandom rolls(seed);
  mc::world::RandomTickRandom rng;
  rng.next_int = [&rolls](int bound) { return rolls.next_int(bound); };
  rng.next_float = [&rolls]() { return rolls.next_float(); };

  int tick = 0;
  int pending = -1; // the `sample` row still awaiting its outcome
  int32_t last_seen_value = rand.value;
  auto push = [&result, &pending](Row row) {
    // A write or a drop is the outcome of the sample before it: the tick ran and
    // changed the world. A rule's own `read` rows are *not* an outcome — a crop's
    // roll can miss after them — so a sample with reads and no change stays
    // `idle`. A sample still unanswered when the next draw arrives was `idle` or
    // `skip`, which is already what it says, so nothing rewrites it.
    const bool outcome = std::strcmp(row.kind, "write") == 0 || std::strcmp(row.kind, "drop") == 0;
    if (pending >= 0 && outcome) {
      result.rows[static_cast<size_t>(pending)].extra = "ticked";
      pending = -1;
    }
    if (std::strcmp(row.kind, "sample") == 0)
      pending = static_cast<int>(result.rows.size());
    result.rows.push_back(std::move(row));
  };

  mc::world::RandomTickLevel level;
  level.min_section_y = kMinSectionY;
  level.section_count = kSectionCount;
  level.ticking_sections = [&result](int, int) { return result.mask; };
  level.get_block = [&](int x, int y, int z) {
    const BlockStateKey state =
        world.block_state_at(x - recorded.chunk_x * 16, y, z - recorded.chunk_z * 16);
    // The pass draws immediately before its own state read, so "the LCG moved
    // since the last read" is exactly the sample read; everything else is a
    // rule's own neighbour read.
    const bool is_sample = rand.value != last_seen_value;
    last_seen_value = rand.value;
    Row row{is_sample ? "sample" : "read", tick, section_index_of(y), x, y, z,
            canonical_state(state),        "-"};
    if (is_sample) {
      if (!mc::world::block_is_randomly_ticking(state))
        row.extra = "skip";
      else
        row.extra = "idle"; // `push` upgrades this to `ticked` if a write follows
      ++result.samples;
    }
    push(std::move(row));
    return state;
  };
  level.set_block = [&](int x, int y, int z, const BlockStateKey& state, bool notify) {
    world.set_block_state(x - recorded.chunk_x * 16, y, z - recorded.chunk_z * 16, state);
    ++result.writes;
    push(Row{"write", tick, section_index_of(y), x, y, z, canonical_state(state),
             notify ? "notify" : "silent"});
  };
  level.raw_brightness = [](int, int, int) { return kBrightness; };
  level.schedule_tick = [&](int x, int y, int z, int delay) {
    ++result.schedules;
    push(
        Row{"schedule", tick, section_index_of(y), x, y, z, "delay=" + std::to_string(delay), "-"});
  };
  level.drop_resources = [&](int x, int y, int z, const BlockStateKey& state) {
    ++result.drops;
    push(Row{"drop", tick, section_index_of(y), x, y, z, canonical_state(state), "-"});
  };

  for (tick = 0; tick < recorded.ticks; ++tick)
    mc::world::tick_chunk_random_blocks(level, rng, rand, recorded.tick_speed, recorded.chunk_x,
                                        recorded.chunk_z);

  result.rand_end = rand.value;
  return result;
}

// ---- the artifacts ----------------------------------------------------------

std::string meta_text(int64_t seed, const std::vector<RecordedChunk>& chunks,
                      const std::vector<RunResult>& runs) {
  std::string text;
  text += "# Phase 3 milestone J: the random-tick pass's golden run parameters and stream ends.\n"
          "#\n"
          "# NOT a vanilla dump: the pass samples `Level.randValue`, an LCG seeded when the\n"
          "# level is constructed, so this is a regression pin of this runtime's own output\n"
          "# rather than a comparison against the game jar. `ctest -R random_tick` replays\n"
          "# every row below through `tick_chunk_random_blocks` and compares the chunk's\n"
          "# trace line for line, so a moved draw or a changed draw count cannot land\n"
          "# silently. Regenerate deliberately with `plusmc-random-tick-dump <fixtures_dir>`,\n"
          "# reviewing the change first with its `--diff` mode.\n"
          "#\n"
          "# `randStart`/`randEnd` are `BlockRandomPos.value` before and after the whole run:\n"
          "# a pass that draws once too many or too few ends somewhere else even when the\n"
          "# samples happen to land on the same cells. `sectionMask` is the per-section\n"
          "# ticking count the pass was given (bit i = section `minSectionY + i`), which is\n"
          "# `block_is_randomly_ticking`'s per-state answer counted by `set_block_state` —\n"
          "# the runner recomputes it from the scene and must agree. `scene` names the\n"
          "# layout in random_tick_scene.tsv, `samples` is the number of LCG draws taken\n"
          "# (`popcount(sectionMask) * tickSpeed * ticks`) and `rows` the trace's length.\n"
          "#\n"
          "# seed\tchunkX\tchunkZ\tminSectionY\tsectionCount\ttickSpeed\tticks\tbrightness"
          "\trandStart\trandEnd\tsectionMask\tscene\tsamples\twrites\trows\n";
  for (size_t i = 0; i < chunks.size(); ++i) {
    const RunResult& run = runs[i];
    char line[512];
    std::snprintf(line, sizeof(line),
                  "%lld\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%llu\t%s\t%zu\t%zu\t%zu\n",
                  static_cast<long long>(seed), chunks[i].chunk_x, chunks[i].chunk_z, kMinSectionY,
                  kSectionCount, chunks[i].tick_speed, chunks[i].ticks, kBrightness, run.rand_start,
                  run.rand_end, static_cast<unsigned long long>(run.mask), chunks[i].scene,
                  run.samples, run.writes, run.rows.size());
    text += line;
  }
  return text;
}

std::string scene_text(const std::vector<RecordedChunk>& chunks,
                       const std::vector<std::vector<Placement>>& scenes) {
  std::string text =
      "# The scene each recorded chunk's world starts from: the blocks in the world, as\n"
      "# the `set_block_state` calls the run begins with, in this order. Coordinates are\n"
      "# chunk-relative x/z with the absolute y, so the same layout serves several\n"
      "# chunks. Everything not listed is air.\n"
      "#\n"
      "# chunkX\tchunkZ\tx\ty\tz\tstate\n";
  for (size_t i = 0; i < chunks.size(); ++i) {
    for (const Placement& placement : scenes[i]) {
      text += std::to_string(chunks[i].chunk_x);
      text += '\t';
      text += std::to_string(chunks[i].chunk_z);
      text += '\t';
      text += std::to_string(placement.x);
      text += '\t';
      text += std::to_string(placement.y);
      text += '\t';
      text += std::to_string(placement.z);
      text += '\t';
      text += canonical_state(BlockStateKey{placement.name, placement.properties});
      text += '\n';
    }
  }
  return text;
}

std::string trace_text(int64_t seed, const RecordedChunk& chunk, const RunResult& run) {
  std::string text;
  text += "# Phase 3 milestone J: the random-tick pass's interaction trace for chunk (";
  text += std::to_string(chunk.chunk_x);
  text += ", ";
  text += std::to_string(chunk.chunk_z);
  text += "), seed ";
  text += std::to_string(seed);
  text += ".\n";
  text += "#\n"
          "# NOT a vanilla dump (see random_tick_meta.tsv): every row is one interaction the\n"
          "# pass had with the level, in the order it happened. `sample` is\n"
          "# `Level.getBlockRandomPos`'s draw and the state read at it; `read` is a rule's own\n"
          "# neighbour read (`getGrowthSpeed`, `updateDistance`); `write`, `drop` and\n"
          "# `schedule` are what a tick did. A shift in the LCG stream, a changed draw count\n"
          "# or a changed y-mask moves these rows, which the shape assertions alone cannot.\n"
          "#\n"
          "# kind\ttick\tsection\tx\ty\tz\tstate\textra\n";
  for (const Row& row : run.rows)
    text += format_row(row) + "\n";
  return text;
}

std::vector<std::string> to_lines(const std::string& text) {
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t newline = text.find('\n', pos);
    if (newline == std::string::npos) {
      lines.push_back(text.substr(pos));
      break;
    }
    lines.push_back(text.substr(pos, newline - pos));
    pos = newline + 1;
  }
  return lines;
}

// Prints the patch of one text artifact, and returns true when it would change.
bool diff_text(const std::string& label, const std::string& path, const std::string& regenerated) {
  const fixture_diff::Lines disk = fixture_diff::read_lines(path);
  if (!disk.exists)
    std::printf("  (%s does not exist yet)\n", label.c_str());
  fixture_diff::print_row_diff(label, disk.lines, to_lines(regenerated),
                               /*max_rows=*/60);
  return !disk.exists || disk.lines != to_lines(regenerated);
}

} // namespace

int main(int argc, char** argv) {
  bool diff = false;
  std::string dir;
  int64_t seed = 42;
  bool have_seed = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--diff") == 0) {
      diff = true;
    } else if (dir.empty()) {
      dir = argv[i];
    } else if (!have_seed) {
      seed = std::strtoll(argv[i], nullptr, 10);
      have_seed = true;
    } else {
      std::fprintf(stderr, "usage: %s <fixtures_dir> [seed] [--diff]\n", argv[0]);
      return 2;
    }
  }
  if (dir.empty()) {
    std::fprintf(stderr, "usage: %s <fixtures_dir> [seed] [--diff]\n", argv[0]);
    return 2;
  }

  std::vector<RecordedChunk> chunks(kChunks, kChunks + sizeof(kChunks) / sizeof(kChunks[0]));
  std::vector<std::vector<Placement>> scenes;
  std::vector<RunResult> runs;
  scenes.reserve(chunks.size());
  runs.reserve(chunks.size());
  for (const RecordedChunk& chunk : chunks) {
    scenes.push_back(scene_placements(chunk.scene));
    runs.push_back(run_chunk(seed, chunk, scenes.back()));
    // The pass took exactly one draw per ticking section per tick per speed
    // unit, or the section was skipped before any draw at all. If either fails,
    // the fixture would pin a lie, so refuse to write it.
    const size_t expected = static_cast<size_t>(popcount64(runs.back().mask)) *
                            static_cast<size_t>(chunk.tick_speed > 0 ? chunk.tick_speed : 0) *
                            static_cast<size_t>(chunk.ticks);
    if (runs.back().samples != expected) {
      std::fprintf(stderr, "error: chunk (%d,%d): %zu samples, expected %zu\n", chunk.chunk_x,
                   chunk.chunk_z, runs.back().samples, expected);
      return 1;
    }
    if ((runs.back().mask == 0 || chunk.tick_speed <= 0) &&
        runs.back().rand_end != runs.back().rand_start) {
      std::fprintf(stderr, "error: chunk (%d,%d): an idle pass advanced the LCG\n", chunk.chunk_x,
                   chunk.chunk_z);
      return 1;
    }
  }

  const std::string meta_path = dir + "/random_tick_meta.tsv";
  const std::string scene_path = dir + "/random_tick_scene.tsv";
  const std::string meta = meta_text(seed, chunks, runs);
  const std::string scene = scene_text(chunks, scenes);

  if (diff) {
    std::printf("random-tick fixture: %zu chunks, seed %lld\n", chunks.size(),
                static_cast<long long>(seed));
    bool changed = false;
    changed |= diff_text("random_tick_meta.tsv", meta_path, meta);
    changed |= diff_text("random_tick_scene.tsv", scene_path, scene);
    for (size_t i = 0; i < chunks.size(); ++i) {
      changed |= diff_text(trace_file_name(seed, chunks[i].chunk_x, chunks[i].chunk_z),
                           dir + "/" + trace_file_name(seed, chunks[i].chunk_x, chunks[i].chunk_z),
                           trace_text(seed, chunks[i], runs[i]));
    }
    if (!changed) {
      std::printf("summary: in sync with this run's output\n");
      return 0;
    }
    std::printf("summary: the checked-in fixture is OUT OF SYNC\n");
    std::printf("re-run without --diff to rewrite it, then `ctest -R random_tick`\n");
    return 1;
  }

  std::FILE* meta_file = std::fopen(meta_path.c_str(), "w");
  if (meta_file == nullptr) {
    std::fprintf(stderr, "error: cannot write %s\n", meta_path.c_str());
    return 1;
  }
  std::fputs(meta.c_str(), meta_file);
  std::fclose(meta_file);
  std::FILE* scene_file = std::fopen(scene_path.c_str(), "w");
  if (scene_file == nullptr) {
    std::fprintf(stderr, "error: cannot write %s\n", scene_path.c_str());
    return 1;
  }
  std::fputs(scene.c_str(), scene_file);
  std::fclose(scene_file);
  std::printf("wrote %s (%zu chunks)\n", meta_path.c_str(), chunks.size());
  std::printf("wrote %s (%zu placements)\n", scene_path.c_str(), [&scenes]() {
    size_t total = 0;
    for (const std::vector<Placement>& scene : scenes)
      total += scene.size();
    return total;
  }());
  for (size_t i = 0; i < chunks.size(); ++i) {
    const std::string name = trace_file_name(seed, chunks[i].chunk_x, chunks[i].chunk_z);
    const std::string text = trace_text(seed, chunks[i], runs[i]);
    std::FILE* trace = std::fopen((dir + "/" + name).c_str(), "w");
    if (trace == nullptr) {
      std::fprintf(stderr, "error: cannot write %s\n", (dir + "/" + name).c_str());
      return 1;
    }
    std::fputs(text.c_str(), trace);
    std::fclose(trace);
    std::printf("wrote %s (%zu rows: %zu samples, %zu writes, %zu drops, %zu schedules)\n",
                name.c_str(), runs[i].rows.size(), runs[i].samples, runs[i].writes, runs[i].drops,
                runs[i].schedules);
  }
  return 0;
}
