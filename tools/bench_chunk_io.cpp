// SPDX-License-Identifier: GPL-3.0-or-later
//
// The chunk I/O measurements behind docs/PHASE5.md's 22 Oct sections:
//
//   plusmc-bench-chunk-io wire   the body a chunk send builds (encode) and what
//                                each zlib level costs and sizes it to
//   plusmc-bench-chunk-io read   the cold read: the tree reader against the
//                                direct one, on generated records and on every
//                                record in the fixture region file
//   plusmc-bench-chunk-io disk   the compressed record's bytes and 4 KiB
//                                sectors, and what each optional component
//                                costs them
//   plusmc-bench-chunk-io save   the record writer: the Value tree against the
//                                direct writer, byte for byte and in time
//
// Each timing is a minimum over repetitions, not a mean: the box this runs on
// is shared, and a mean reports the neighbours' load (the numbers in PHASE5 say
// how many pairs were taken and what the spread was).
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <zlib.h>

#include "protocol/framing.h"
#include "world/chunk.h"
#include "world/generated_chunk.h"
#include "world/nbt.h"
#include "world/registry.h"
#include "world/worldgen_chunk.h"

using Clock = std::chrono::steady_clock;

namespace {

double ms_since(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Decorated generated columns, the stage the server stores and sends.
std::vector<mc::world::Chunk> generate_columns(int side) {
  mc::world::WgOverworldGenerator gen(42);
  std::vector<mc::world::Chunk> columns;
  columns.reserve(static_cast<std::size_t>(side * side));
  for (int i = 0; i < side; ++i)
    for (int j = 0; j < side; ++j) {
      mc::world::Chunk chunk;
      mc::world::fill_generated_decorated_chunk(gen, i * 9 - 27, j * 9 - 27, chunk);
      columns.push_back(std::move(chunk));
    }
  return columns;
}

bool deflate_bytes(const std::vector<uint8_t>& in, int level, std::vector<uint8_t>* out) {
  z_stream zs{};
  if (deflateInit2(&zs, level, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
    return false;
  out->resize(deflateBound(&zs, static_cast<uLong>(in.size())));
  zs.next_in = const_cast<Bytef*>(in.data());
  zs.avail_in = static_cast<uInt>(in.size());
  zs.next_out = out->data();
  zs.avail_out = static_cast<uInt>(out->size());
  const int rc = deflate(&zs, Z_FINISH);
  out->resize(zs.total_out);
  deflateEnd(&zs);
  return rc == Z_STREAM_END;
}

// The fixture region's records, inflated: the one chunk captured from vanilla
// 26.2, which is what proves the direct reader handles a record it did not
// write itself.
void append_fixture_records(std::vector<std::vector<uint8_t>>* out) {
  std::ifstream f("tests/fixtures/r.0.0.mca", std::ios::binary);
  if (!f)
    return;
  const std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
  if (file.size() < 8192)
    return;
  for (int i = 0; i < 1024; ++i) {
    const size_t at = static_cast<size_t>(i) * 4;
    const uint32_t location = (static_cast<uint32_t>(file[at]) << 24) |
                              (static_cast<uint32_t>(file[at + 1]) << 16) |
                              (static_cast<uint32_t>(file[at + 2]) << 8) | file[at + 3];
    const uint32_t sector = location >> 8;
    if (sector == 0 || (location & 0xFF) == 0)
      continue;
    const size_t record = static_cast<size_t>(sector) * 4096;
    if (record + 5 > file.size())
      continue;
    const uint32_t length = (static_cast<uint32_t>(file[record]) << 24) |
                            (static_cast<uint32_t>(file[record + 1]) << 16) |
                            (static_cast<uint32_t>(file[record + 2]) << 8) | file[record + 3];
    if (length < 2 || record + 4 + length > file.size())
      continue;
    const uint8_t compression = file[record + 4];
    if (compression != 2)
      continue;
    std::vector<uint8_t> inflated(1 << 20);
    uLongf produced = static_cast<uLongf>(inflated.size());
    const uLongf in_len = static_cast<uLongf>(length - 1);
    if (uncompress(inflated.data(), &produced, file.data() + record + 5, in_len) != Z_OK)
      continue;
    inflated.resize(produced);
    out->push_back(std::move(inflated));
  }
}

bool inflate_bytes(const std::vector<uint8_t>& in, std::vector<uint8_t>* out) {
  z_stream zs{};
  if (inflateInit(&zs) != Z_OK)
    return false;
  out->resize(256 * 1024);
  zs.next_in = const_cast<Bytef*>(in.data());
  zs.avail_in = static_cast<uInt>(in.size());
  zs.next_out = out->data();
  zs.avail_out = static_cast<uInt>(out->size());
  const int rc = inflate(&zs, Z_FINISH);
  out->resize(zs.total_out);
  inflateEnd(&zs);
  return rc == Z_STREAM_END;
}

// Every field the two readers must agree on, folded into one number; the
// equality check is a hash comparison, not a byte comparison of the Chunk (whose
// cells are deliberately not a stable layout).
uint64_t hash_chunk(const mc::world::Chunk& chunk) {
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&h](uint64_t v) {
    for (int i = 0; i < 8; ++i)
      h = (h ^ ((v >> (i * 8)) & 0xFF)) * 1099511628211ull;
  };
  mix(static_cast<uint64_t>(chunk.x));
  mix(static_cast<uint64_t>(chunk.z));
  for (const auto& [type, longs] : chunk.heightmaps) {
    mix(static_cast<uint64_t>(type));
    mix(longs.size());
    for (const int64_t v : longs)
      mix(static_cast<uint64_t>(v));
  }
  for (const auto& section : chunk.sections) {
    mix(static_cast<uint64_t>(section.y));
    mix(static_cast<uint64_t>(section.has_blocks));
    mix(static_cast<uint64_t>(section.has_biomes));
    mix(static_cast<uint64_t>(section.random_tick_blocks));
    for (const auto& key : section.block_palette) {
      for (const char c : key.name)
        mix(static_cast<uint8_t>(c));
      for (const char c : key.properties)
        mix(static_cast<uint8_t>(c));
    }
    for (const auto& name : section.biome_palette)
      for (const char c : name)
        mix(static_cast<uint8_t>(c));
    for (std::size_t i = 0; i < section.block_cells.size(); ++i)
      mix(static_cast<uint64_t>(section.block_cells[i]));
    for (std::size_t i = 0; i < section.biome_cells.size(); ++i)
      mix(static_cast<uint64_t>(section.biome_cells[i]));
    if (section.sky_light) {
      mix(section.sky_light->size());
      for (const uint8_t b : *section.sky_light)
        mix(b);
    }
    if (section.block_light) {
      mix(section.block_light->size());
      for (const uint8_t b : *section.block_light)
        mix(b);
    }
  }
  mix(chunk.block_entities.size());
  return h;
}

void bench_wire() {
  const std::vector<mc::world::Chunk> columns = generate_columns(6);
  // The biome ids the wire encoder resolves names through are the order the
  // configuration phase advertises, i.e. the same file the server loads.
  std::vector<std::string> biome_names;
  {
    std::ifstream in("data/vanilla/26.2/clientconfig/registries/worldgen_biome.txt");
    for (std::string line; std::getline(in, line);)
      if (!line.empty())
        biome_names.push_back(line);
  }
  mc::world::NameListBiomeTable biomes(biome_names);
  const auto& blocks = mc::world::full_block_states();

  std::vector<std::vector<uint8_t>> bodies;
  bodies.reserve(columns.size());
  for (const auto& chunk : columns) {
    auto body = mc::world::encode_chunk_body(chunk, blocks, biomes);
    if (!body) {
      std::printf("wire: encode failed\n");
      return;
    }
    bodies.push_back(std::move(*body));
  }

  double best = 1e9;
  std::size_t bytes = 0;
  for (int rep = 0; rep < 10; ++rep) {
    const auto start = Clock::now();
    std::size_t total = 0;
    for (const auto& chunk : columns) {
      auto body = mc::world::encode_chunk_body(chunk, blocks, biomes);
      if (!body)
        return;
      total += body->size();
    }
    best = std::min(best, ms_since(start) / static_cast<double>(columns.size()));
    bytes = total / columns.size();
  }
  std::printf("wire: encode %.3f ms/column (min of 10), %zu B body\n", best, bytes);

  for (const int level : {1, 2, 3, 4, 5, 6, 9}) {
    double level_best = 1e9;
    std::size_t compressed = 0;
    std::vector<uint8_t> out;
    for (int rep = 0; rep < 5; ++rep) {
      const auto start = Clock::now();
      std::size_t total = 0;
      for (const auto& body : bodies) {
        if (!deflate_bytes(body, level, &out))
          return;
        total += out.size();
      }
      level_best = std::min(level_best, ms_since(start) / static_cast<double>(bodies.size()));
      compressed = total / bodies.size();
    }
    std::printf("wire: zlib %d %.3f ms/body, %zu B (min of 5)\n", level, level_best, compressed);
  }
}

// The send path's half that is not the body: framing. A chunk packet goes
// through `append_frame` once per recipient threshold, so the zlib stream's
// setup is part of the tick cost. `PLUSMC_FRAME_ZSTREAM=off` is the one-shot
// `compress2` arm; unset is the reused stream. The compressed bytes must be
// identical between the arms (framing_test pins the bytes).
void bench_frames() {
  const std::vector<mc::world::Chunk> columns = generate_columns(6);
  std::vector<std::string> biome_names;
  {
    std::ifstream in("data/vanilla/26.2/clientconfig/registries/worldgen_biome.txt");
    for (std::string line; std::getline(in, line);)
      if (!line.empty())
        biome_names.push_back(line);
  }
  mc::world::NameListBiomeTable biomes(biome_names);
  const auto& blocks = mc::world::full_block_states();
  std::vector<std::vector<uint8_t>> bodies;
  bodies.reserve(columns.size());
  for (const auto& chunk : columns) {
    auto body = mc::world::encode_chunk_body(chunk, blocks, biomes);
    if (!body) {
      std::printf("frames: encode failed\n");
      return;
    }
    bodies.push_back(std::move(*body));
  }

  const char* arm = std::getenv("PLUSMC_FRAME_ZSTREAM");
  const bool reused = !(arm != nullptr && std::strcmp(arm, "off") == 0);
  std::vector<uint8_t> frame;
  double best = 1e9;
  std::size_t bytes = 0;
  for (int rep = 0; rep < 10; ++rep) {
    const auto start = Clock::now();
    std::size_t total = 0;
    for (const auto& body : bodies) {
      frame.clear();
      if (!mc::protocol::append_frame(&frame, body, 1))
        return;
      total += frame.size();
    }
    best = std::min(best, ms_since(start) / static_cast<double>(bodies.size()));
    bytes = total / bodies.size();
  }
  std::printf("frames: append %.3f ms/frame, %zu B frame (min of 10, zstream=%s)\n", best, bytes,
              reused ? "reused" : "one-shot");
}

void bench_read() {
  const std::vector<mc::world::Chunk> columns = generate_columns(7);
  std::vector<std::vector<uint8_t>> records;
  for (const auto& chunk : columns)
    records.push_back(mc::world::nbt::serialize(mc::world::serialize_chunk_nbt(chunk), ""));
  const std::size_t generated = records.size();
  append_fixture_records(&records);

  std::size_t mismatches = 0;
  for (const auto& record : records) {
    auto tree_root = mc::world::nbt::parse(record.data(), record.size());
    if (!tree_root) {
      std::printf("read: tree parse failed\n");
      return;
    }
    mc::world::Chunk tree;
    mc::world::Chunk direct;
    if (!mc::world::parse_chunk_nbt(*tree_root, -4, 24, &tree) ||
        !mc::world::parse_chunk_nbt_direct(record.data(), record.size(), -4, 24, &direct)) {
      std::printf("read: parse failed\n");
      return;
    }
    if (hash_chunk(tree) != hash_chunk(direct))
      ++mismatches;
  }
  std::printf("read: %zu records (%zu generated, %zu fixture), %zu mismatches\n", records.size(),
              generated, records.size() - generated, mismatches);

  std::vector<std::vector<uint8_t>> compressed_records;
  compressed_records.reserve(records.size());
  for (const auto& record : records) {
    std::vector<uint8_t> compressed;
    if (!deflate_bytes(record, 6, &compressed))
      return;
    compressed_records.push_back(std::move(compressed));
  }

  std::vector<uint8_t> inflated;
  double inflate_best = 1e9;
  for (int rep = 0; rep < 10; ++rep) {
    const auto start = Clock::now();
    for (const auto& compressed : compressed_records)
      if (!inflate_bytes(compressed, &inflated))
        return;
    inflate_best = std::min(inflate_best, ms_since(start) / static_cast<double>(records.size()));
  }

  double tree_best = 1e9;
  double direct_best = 1e9;
  double read_tree_best = 1e9;
  double read_direct_best = 1e9;
  for (int rep = 0; rep < 10; ++rep) {
    auto start = Clock::now();
    for (const auto& record : records) {
      auto root = mc::world::nbt::parse(record.data(), record.size());
      mc::world::Chunk chunk;
      mc::world::parse_chunk_nbt(*root, -4, 24, &chunk);
    }
    tree_best = std::min(tree_best, ms_since(start) / static_cast<double>(records.size()));

    start = Clock::now();
    for (const auto& record : records) {
      mc::world::Chunk chunk;
      mc::world::parse_chunk_nbt_direct(record.data(), record.size(), -4, 24, &chunk);
    }
    direct_best = std::min(direct_best, ms_since(start) / static_cast<double>(records.size()));

    // The cold read a session pays: decompress, then parse.
    start = Clock::now();
    for (const auto& compressed : compressed_records) {
      std::vector<uint8_t> record;
      if (!inflate_bytes(compressed, &record))
        return;
      auto root = mc::world::nbt::parse(record.data(), record.size());
      mc::world::Chunk chunk;
      mc::world::parse_chunk_nbt(*root, -4, 24, &chunk);
    }
    read_tree_best =
        std::min(read_tree_best, ms_since(start) / static_cast<double>(records.size()));

    start = Clock::now();
    for (const auto& compressed : compressed_records) {
      std::vector<uint8_t> record;
      if (!inflate_bytes(compressed, &record))
        return;
      mc::world::Chunk chunk;
      mc::world::parse_chunk_nbt_direct(record.data(), record.size(), -4, 24, &chunk);
    }
    read_direct_best =
        std::min(read_direct_best, ms_since(start) / static_cast<double>(records.size()));
  }
  std::printf("read: parse tree %.3f ms/column, direct %.3f ms/column (min of 10)\n", tree_best,
              direct_best);
  std::printf("read: cold (inflate+parse) tree %.3f ms/column, direct %.3f ms/column "
              "(inflate alone ~%.3f ms)\n",
              read_tree_best, read_direct_best, inflate_best);
}

// A block entity with one of every tag the direct writer has to reproduce, so
// the equality check covers the `write_nbt_value` half and not just the fields
// this server models.
void append_bench_block_entity(mc::world::Chunk* chunk) {
  mc::world::nbt::Value compound;
  compound.type = mc::world::nbt::Tag::Compound;
  compound.fields.emplace_back("id", mc::world::nbt::make_string("minecraft:chest"));
  compound.fields.emplace_back("x", mc::world::nbt::make_int(chunk->x * 16 + 3));
  compound.fields.emplace_back("y", mc::world::nbt::make_int(70));
  compound.fields.emplace_back("z", mc::world::nbt::make_int(chunk->z * 16 + 5));
  compound.fields.emplace_back("KeepPacked", mc::world::nbt::make_byte(0));
  compound.fields.emplace_back("Float", mc::world::nbt::make_float(1.5f));
  compound.fields.emplace_back("Double", mc::world::nbt::make_double(2.25));
  compound.fields.emplace_back("Long", mc::world::nbt::make_long(-7));
  compound.fields.emplace_back("Bytes",
                               mc::world::nbt::make_byte_array_u8(std::vector<uint8_t>{0, 255, 7}));
  compound.fields.emplace_back("Ints", mc::world::nbt::make_int_array({1, -2, 3}));
  compound.fields.emplace_back("Longs", mc::world::nbt::make_long_array({1, -2, 3}));
  mc::world::nbt::Value list;
  list.type = mc::world::nbt::Tag::List;
  list.list_type = mc::world::nbt::Tag::String;
  list.items = {mc::world::nbt::make_string("a"), mc::world::nbt::make_string("b")};
  compound.fields.emplace_back("Names", std::move(list));
  chunk->block_entities.push_back(std::move(compound));
}

void bench_save() {
  std::vector<mc::world::Chunk> columns = generate_columns(7);
  // One column carries a block entity so the generic-tag writer is measured and
  // compared too, not only the fields the server models.
  append_bench_block_entity(&columns.front());

  std::size_t mismatches = 0;
  std::size_t bytes = 0;
  for (const auto& chunk : columns) {
    const std::vector<uint8_t> tree =
        mc::world::nbt::serialize(mc::world::serialize_chunk_nbt(chunk), "");
    const std::vector<uint8_t> direct = mc::world::serialize_chunk_nbt_bytes(chunk);
    if (tree != direct)
      ++mismatches;
    bytes = std::max(bytes, direct.size());
  }
  std::printf("save: %zu columns, %zu byte mismatches (tree vs direct), %zu B largest record\n",
              columns.size(), mismatches, bytes);

  double tree_best = 1e9;
  double direct_best = 1e9;
  for (int rep = 0; rep < 10; ++rep) {
    auto start = Clock::now();
    for (const auto& chunk : columns) {
      const std::vector<uint8_t> record =
          mc::world::nbt::serialize(mc::world::serialize_chunk_nbt(chunk), "");
      if (record.size() == 0)
        return;
    }
    tree_best = std::min(tree_best, ms_since(start) / static_cast<double>(columns.size()));

    start = Clock::now();
    for (const auto& chunk : columns) {
      const std::vector<uint8_t> record = mc::world::serialize_chunk_nbt_bytes(chunk);
      if (record.size() == 0)
        return;
    }
    direct_best = std::min(direct_best, ms_since(start) / static_cast<double>(columns.size()));
  }
  std::printf("save: record bytes tree %.3f ms/column, direct %.3f ms/column (min of 10)\n",
              tree_best, direct_best);
}

void bench_disk() {
  const std::vector<mc::world::Chunk> columns = generate_columns(7);
  struct Variant {
    const char* name;
    bool drop_heightmaps;
    bool drop_uniform_sky;
    bool drop_all_sky;
  };
  const Variant variants[] = {{"full", false, false, false},
                              {"no heightmaps", true, false, false},
                              {"no uniform sky", false, true, false},
                              {"no sky", false, false, true}};
  for (const auto& variant : variants) {
    std::size_t compressed = 0;
    std::size_t sectors = 0;
    std::vector<uint8_t> out;
    for (const auto& base : columns) {
      mc::world::Chunk chunk = base;
      if (variant.drop_heightmaps)
        chunk.heightmaps.clear();
      for (auto& section : chunk.sections) {
        if (variant.drop_all_sky) {
          section.sky_light.reset();
        } else if (variant.drop_uniform_sky && section.sky_light && section.sky_light->uniform() &&
                   section.sky_light->uniform_level() == 15) {
          section.sky_light.reset();
        }
      }
      const std::vector<uint8_t> record =
          mc::world::nbt::serialize(mc::world::serialize_chunk_nbt(chunk), "");
      if (!deflate_bytes(record, 6, &out))
        return;
      compressed += out.size();
      sectors += (out.size() + 5 + 4095) / 4096;
    }
    std::printf("disk: %-16s %zu B/column compressed, %zu sectors (%.2f KiB/column on disk)\n",
                variant.name, compressed / columns.size(), sectors,
                static_cast<double>(sectors) * 4096.0 / static_cast<double>(columns.size()) /
                    1024.0);
  }
}

} // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "all";
  const bool all = mode == "all";
  std::printf("== chunk io bench (%s) ==\n", mode.c_str());
  if (all || mode == "wire")
    bench_wire();
  if (all || mode == "frames")
    bench_frames();
  if (all || mode == "read")
    bench_read();
  if (all || mode == "disk")
    bench_disk();
  if (all || mode == "save")
    bench_save();
  return 0;
}
