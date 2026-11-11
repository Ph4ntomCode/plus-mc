// SPDX-License-Identifier: GPL-3.0-or-later
#include "server/client_config.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "core/json_util.h"
#include "core/log.h"

namespace mc::server {

namespace fs = std::filesystem;

namespace {

using mc::core::json_int_field;

// JSON helpers live in core/json_util.h (shared with main.cpp).

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return {};
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

ClientConfigData load_client_config(const std::string& data_root, const std::string& version) {
  ClientConfigData out;

  fs::path root(data_root);
  std::error_code ec;
  int64_t best_dv = -1;
  fs::path best_dir;
  if (!version.empty()) {
    best_dir = root / version;
    if (!fs::exists(best_dir / "version.json"))
      return out;
    best_dv = 0;
  } else {
    for (const auto& entry : fs::directory_iterator(root, ec)) {
      if (ec || !entry.is_directory()) {
        ec.clear();
        continue;
      }
      fs::path vj = entry.path() / "version.json";
      if (!fs::exists(vj))
        continue;
      std::string text = read_file(vj);
      auto dv = json_int_field(text, "world_version");
      if (!dv)
        continue;
      if (*dv > best_dv) {
        best_dv = *dv;
        best_dir = entry.path();
      }
    }
  }
  if (best_dv < 0)
    return out; // no vanilla data present

  fs::path cc = best_dir / "clientconfig";
  if (!fs::is_directory(cc))
    return out;

  // Registry listings: registries/<suffix>.txt -> minecraft:<suffix>. The
  // filename stem is a filesystem-safe form of the registry id; today the
  // only synced registry whose id has a path separator is worldgen/biome,
  // captured as worldgen_biome.txt. Sending the underscore form would make
  // the client treat the listing as an unknown registry and silently fall
  // back to its baked order (same numbers today, but not faithful).
  fs::path regs = cc / "registries";
  std::error_code rec;
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(regs, rec)) {
    if (!rec && e.path().extension() == ".txt")
      files.push_back(e.path());
    rec.clear();
  }
  std::sort(files.begin(), files.end());
  for (const auto& f : files) {
    std::string suffix = f.stem().string();
    std::string rid =
        suffix == "worldgen_biome" ? "minecraft:worldgen/biome" : "minecraft:" + suffix;
    std::string text = read_file(f);
    std::vector<std::string> names;
    size_t pos = 0;
    while (pos < text.size()) {
      size_t nl = text.find('\n', pos);
      if (nl == std::string::npos)
        nl = text.size();
      std::string line = text.substr(pos, nl - pos);
      pos = nl + 1;
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (!line.empty())
        names.push_back(line);
    }
    if (!names.empty())
      out.registries.emplace_back(rid, std::move(names));
  }

  // Tags payload.
  fs::path tags_path = cc / "tags.bin";
  if (fs::exists(tags_path)) {
    std::string bytes = read_file(tags_path);
    out.tags.assign(bytes.begin(), bytes.end());
  }

  // Version id from the directory name via version.json "id".
  std::string vtext = read_file(best_dir / "version.json");
  size_t ki = vtext.find("\"id\"");
  if (ki != std::string::npos) {
    size_t qi = vtext.find('"', ki + 5);
    if (qi != std::string::npos) {
      size_t qe = vtext.find('"', qi + 1);
      if (qe != std::string::npos)
        out.version = vtext.substr(qi + 1, qe - qi - 1);
    }
  }

  if (out.registries.empty() || out.tags.empty()) {
    log::warn("clientconfig under ", cc.string(),
              " is incomplete (registries=", out.registries.size(), ", tags=", out.tags.size(),
              " bytes)");
  }
  return out;
}

} // namespace mc::server
