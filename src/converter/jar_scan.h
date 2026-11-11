// SPDX-License-Identifier: GPL-3.0-or-later
//
// `plusmc-convert analyze`: a decompile-free scan of a Bukkit/Spigot/Paper
// plugin JAR (docs/PLAN.md §9.3, Phase 7 slice 1). It reads `plugin.yml` (or
// `paper-plugin.yml`) and every class's constant pool, then reports the API
// version, the classes and members the plugin names, its server-internal (NMS)
// and reflection use, and a Green/Yellow/Red compatibility score. It never
// loads, links or executes plugin code.

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace mc::converter {

struct PluginManifest {
  std::string file;
  std::string name;
  std::string version;
  std::string main;
  std::string api_version;
  std::string description;
  std::vector<std::string> authors;
  std::vector<std::string> depend;
  std::vector<std::string> softdepend;
  std::vector<std::string> commands;
  std::vector<std::string> permissions;
};

struct ClassReport {
  std::string name;
  uint16_t major = 0;
  std::size_t methods = 0;
  std::size_t references = 0;
};

// One method or field reference, in the constant pool's own terms.
struct MemberRef {
  std::string owner;
  std::string name;
  std::string descriptor;
};

enum class Score { Green, Yellow, Red };

struct Usage {
  std::vector<std::string> supported;
  std::vector<std::string> wider;
  std::vector<std::string> nms;
  std::vector<std::string> reflection;
  std::vector<std::string> unknown;
  std::vector<std::string> reflection_members;
};

struct Analysis {
  std::string package;
  bool ok = false;
  std::string error;
  bool has_manifest = false;
  PluginManifest manifest;
  std::vector<ClassReport> classes;
  // The references the mapper consumes: every class the archive names, and
  // every method/field member (owner, name, descriptor), both sorted.
  std::vector<std::string> class_refs;
  std::vector<MemberRef> method_refs;
  std::vector<MemberRef> field_refs;
  Usage usage;
  Score score = Score::Red;
  std::string reason;
  std::vector<std::string> notes;
  std::size_t entries = 0;
  std::size_t skipped_entries = 0;
};

Analysis analyze_jar(const std::filesystem::path& path);

// The manifest reader is exposed for its own negative tests: it accepts the
// flat subset a plugin manifest uses and refuses a malformed line with its
// number rather than guessing.
bool parse_manifest(const std::string& text, PluginManifest* out, std::string* error);

std::string score_name(Score score);
std::string report_text(const Analysis& analysis);
std::string report_json(const Analysis& analysis);

} // namespace mc::converter
