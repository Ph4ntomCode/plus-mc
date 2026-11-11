// SPDX-License-Identifier: GPL-3.0-or-later
//
// A biome map: sample the overworld generator's biome stage over a large
// rectangle and render it as a PNG, one pixel per quart cell (4 x 4 blocks).
//
// This is a sanity check, not a test: the exact checks live in
// tests/worldgen_test.cpp (cell-for-cell against the vanilla dump). What this
// shows is the shape at a scale where "is the biome distribution plausible" is
// a question you can answer by looking — oceans and rivers are contiguous,
// climate bands run east-west, and no biome dominates a whole map because a
// table row is stuck. The histogram and the legend next to the PNG are the
// numbers behind that look.
//
// Usage:
//   plusmc-biome-map --seed 42 --origin -1024 -1024 --blocks 2048 2048 \
//       --y 63 --out build/biome_map.png
//
// Sampling is `WgOverworldGenerator::biome_at_quart`, the same call
// `MemoryWorld::biome_name_at` makes for an unstored column, so the map is the
// generator's own answer, not a re-derivation.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <zlib.h>

#include "world/worldgen_chunk.h"

namespace {

struct Options {
  int64_t seed = 42;
  int origin_x = -1024;
  int origin_z = -1024;
  int width = 2048;
  int height = 2048;
  int y = 63;
  std::string out = "biome_map.png";
  std::string find;
};

Options parse(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "biome-map: %s needs a value\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--seed")
      options.seed = std::strtoll(value("--seed"), nullptr, 10);
    else if (arg == "--origin") {
      options.origin_x = std::atoi(value("--origin"));
      options.origin_z = std::atoi(value("--origin"));
    } else if (arg == "--blocks") {
      options.width = std::atoi(value("--blocks"));
      options.height = std::atoi(value("--blocks"));
    } else if (arg == "--y")
      options.y = std::atoi(value("--y"));
    else if (arg == "--out")
      options.out = value("--out");
    else if (arg == "--find")
      options.find = value("--find");
    else {
      std::fprintf(stderr, "biome-map: unknown argument %s\n", arg.c_str());
      std::exit(2);
    }
  }
  return options;
}

// PNG writing, the one piece of plumbing zlib does not give us: PNG framing.
// Eight-bit RGB, no interlacing, filter 0 on every scanline.
void write_png(const std::string& path, int width, int height, const std::vector<uint8_t>& rgb) {
  auto be32 = [](uint32_t v) {
    return std::vector<uint8_t>{static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16),
                                static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
  };
  auto chunk = [&](const char type[4], const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    const std::vector<uint8_t> len = be32(static_cast<uint32_t>(data.size()));
    out.insert(out.end(), len.begin(), len.end());
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    uint32_t crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const Bytef*>(type), 4);
    if (!data.empty())
      crc = crc32(crc, data.data(), static_cast<uInt>(data.size()));
    const std::vector<uint8_t> crc_bytes = be32(crc);
    out.insert(out.end(), crc_bytes.begin(), crc_bytes.end());
    return out;
  };

  // Raw scanlines: one filter byte (0) then width RGB triples.
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(height) * (1 + static_cast<size_t>(width) * 3));
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb.begin() + static_cast<size_t>(y) * width * 3,
               rgb.begin() + static_cast<size_t>(y + 1) * width * 3);
  }
  uLongf compressed_size = compressBound(static_cast<uLong>(raw.size()));
  std::vector<uint8_t> compressed(compressed_size);
  if (compress2(compressed.data(), &compressed_size, raw.data(), static_cast<uLong>(raw.size()),
                Z_BEST_SPEED) != Z_OK) {
    std::fprintf(stderr, "biome-map: zlib compression failed\n");
    std::exit(1);
  }
  compressed.resize(compressed_size);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  const std::vector<uint8_t> ihdr = [&] {
    std::vector<uint8_t> out = be32(static_cast<uint32_t>(width));
    const std::vector<uint8_t> h = be32(static_cast<uint32_t>(height));
    out.insert(out.end(), h.begin(), h.end());
    out.insert(out.end(), {8, 2, 0, 0, 0}); // 8-bit, truecolour, deflate
    return out;
  }();
  for (const std::vector<uint8_t>& part :
       {chunk("IHDR", ihdr), chunk("IDAT", compressed), chunk("IEND", {})}) {
    png.insert(png.end(), part.begin(), part.end());
  }

  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  if (!file) {
    std::fprintf(stderr, "biome-map: cannot write %s\n", path.c_str());
    std::exit(1);
  }
}

struct Hsv {
  double h, s, v;
};

void hsv_to_rgb(double h, double s, double v, uint8_t* rgb) {
  h = h - std::floor(h);
  const double i = std::floor(h * 6.0);
  const double f = h * 6.0 - i;
  const double p = v * (1.0 - s);
  const double q = v * (1.0 - f * s);
  const double t = v * (1.0 - (1.0 - f) * s);
  double r = v, g = t, b = p;
  switch (static_cast<int>(i) % 6) {
  case 0:
    r = v;
    g = t;
    b = p;
    break;
  case 1:
    r = q;
    g = v;
    b = p;
    break;
  case 2:
    r = p;
    g = v;
    b = t;
    break;
  case 3:
    r = p;
    g = q;
    b = v;
    break;
  case 4:
    r = t;
    g = p;
    b = v;
    break;
  default:
    r = v;
    g = p;
    b = q;
    break;
  }
  rgb[0] = static_cast<uint8_t>(r * 255.0 + 0.5);
  rgb[1] = static_cast<uint8_t>(g * 255.0 + 0.5);
  rgb[2] = static_cast<uint8_t>(b * 255.0 + 0.5);
}

} // namespace

int main(int argc, char** argv) {
  const Options options = parse(argc, argv);
  if (options.width <= 0 || options.height <= 0 || options.width % 4 != 0 ||
      options.height % 4 != 0) {
    std::fprintf(stderr, "biome-map: --blocks must be positive multiples of 4\n");
    return 2;
  }

  const int pixels_x = options.width / 4;
  const int pixels_z = options.height / 4;
  mc::world::WgOverworldGenerator generator(options.seed);

  std::vector<const char*> names(static_cast<size_t>(pixels_x) * pixels_z);
  std::map<std::string, int64_t> histogram;
  for (int px = 0; px < pixels_x; ++px) {
    const int quart_x = (options.origin_x >> 2) + px;
    for (int pz = 0; pz < pixels_z; ++pz) {
      const int quart_z = (options.origin_z >> 2) + pz;
      const char* name = generator.biome_at_quart(quart_x, options.y >> 2, quart_z);
      names[static_cast<size_t>(pz) * pixels_x + px] = name;
      ++histogram[name];
    }
  }

  std::vector<std::string> palette_names;
  palette_names.reserve(histogram.size());
  for (const auto& [name, count] : histogram)
    palette_names.push_back(name);
  std::sort(palette_names.begin(), palette_names.end());

  std::map<std::string, std::array<uint8_t, 3>> colors;
  std::vector<uint8_t> rgb(static_cast<size_t>(pixels_x) * pixels_z * 3);
  for (size_t i = 0; i < palette_names.size(); ++i) {
    std::array<uint8_t, 3> color{};
    // Golden-angle hues: distinct for neighbours in the sorted list without a
    // hand-picked palette for all 55 biomes.
    hsv_to_rgb(0.6180339887498949 * static_cast<double>(i), 0.62, 0.86, color.data());
    colors.emplace(palette_names[i], color);
  }
  for (size_t i = 0; i < names.size(); ++i) {
    const std::array<uint8_t, 3>& color = colors[names[i]];
    rgb[i * 3 + 0] = color[0];
    rgb[i * 3 + 1] = color[1];
    rgb[i * 3 + 2] = color[2];
  }

  write_png(options.out, pixels_x, pixels_z, rgb);

  std::string legend_path = options.out;
  const size_t dot = legend_path.find_last_of('.');
  legend_path =
      (dot == std::string::npos ? legend_path : legend_path.substr(0, dot)) + "_legend.tsv";
  {
    std::ofstream legend(legend_path);
    legend << "# biome\tcount\tshare\trgb\n";
    char line[256];
    for (const std::string& name : palette_names) {
      const int64_t count = histogram[name];
      const std::array<uint8_t, 3>& color = colors[name];
      std::snprintf(line, sizeof(line), "%s\t%lld\t%.4f\t#%02X%02X%02X\n", name.c_str(),
                    static_cast<long long>(count),
                    static_cast<double>(count) / static_cast<double>(names.size()), color[0],
                    color[1], color[2]);
      legend << line;
    }
  }

  if (!options.find.empty()) {
    std::printf("spots with %s (block coords):\n", options.find.c_str());
    int shown = 0;
    for (int pz = 0; pz < pixels_z && shown < 10; pz += 8) {
      for (int px = 0; px < pixels_x && shown < 10; px += 8) {
        if (options.find != names[static_cast<size_t>(pz) * pixels_x + px])
          continue;
        std::printf("  chunk %d %d (block %d %d)\n", (options.origin_x + px * 4) >> 4,
                    (options.origin_z + pz * 4) >> 4, options.origin_x + px * 4 + 2,
                    options.origin_z + pz * 4 + 2);
        ++shown;
      }
    }
    if (shown == 0)
      std::printf("  (none found)\n");
  }

  const int64_t total = static_cast<int64_t>(names.size());
  std::printf("biome map: seed %lld, %d x %d blocks at y=%d, %d x %d quart pixels\n",
              static_cast<long long>(options.seed), options.width, options.height, options.y,
              pixels_x, pixels_z);
  std::printf("wrote %s and %s\n", options.out.c_str(), legend_path.c_str());
  std::vector<std::pair<int64_t, std::string>> top;
  for (const auto& [name, count] : histogram)
    top.push_back({count, name});
  std::sort(top.rbegin(), top.rend());
  for (size_t i = 0; i < top.size() && i < 10; ++i) {
    std::printf("  %-32s %8lld  %5.1f%%\n", top[i].second.c_str(),
                static_cast<long long>(top[i].first),
                100.0 * static_cast<double>(top[i].first) / static_cast<double>(total));
  }
  std::printf("  (%zu distinct biomes)\n", histogram.size());
  return 0;
}
