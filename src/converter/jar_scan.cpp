// SPDX-License-Identifier: GPL-3.0-or-later

#include "converter/jar_scan.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "converter/class_scan.h"
#include "converter/report_json.h"
#include "modapi/zip_archive.h"

namespace mc::converter {
namespace {

using mc::modapi::detail::ZipArchive;
using mc::modapi::detail::ZipEntry;

constexpr uint64_t kMaxManifestBytes = 256 * 1024;
constexpr uint64_t kMaxClassBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxClasses = 4096;
constexpr std::size_t kMaxListed = 20;

using detail::json_list;
using detail::json_quote;

bool starts_with(const std::string& text, const std::string& prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

std::string trim(const std::string& text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0)
    ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
    --end;
  return text.substr(begin, end - begin);
}

std::string unquote(const std::string& text) {
  if (text.size() >= 2 && (text.front() == '\'' || text.front() == '"') &&
      text.back() == text.front()) {
    return text.substr(1, text.size() - 2);
  }
  return text;
}

std::vector<std::string> split_list(const std::string& text) {
  std::vector<std::string> values;
  std::string inner = text;
  if (inner.size() >= 2 && inner.front() == '[' && inner.back() == ']')
    inner = inner.substr(1, inner.size() - 2);
  std::size_t start = 0;
  while (start <= inner.size()) {
    const std::size_t comma = inner.find(',', start);
    const std::string item =
        trim(inner.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
    if (!item.empty())
      values.push_back(unquote(item));
    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }
  return values;
}

void append_unique(std::vector<std::string>* values, const std::string& value) {
  if (std::find(values->begin(), values->end(), value) == values->end())
    values->push_back(value);
}

void sort_unique(std::vector<std::string>* values) {
  std::sort(values->begin(), values->end());
  values->erase(std::unique(values->begin(), values->end()), values->end());
}

bool is_supported_api(const std::string& name) {
  static const char* kSupported[] = {
      "org/bukkit/plugin/",    "org/bukkit/event/",         "org/bukkit/command/",
      "org/bukkit/scheduler/", "org/bukkit/configuration/", "org/bukkit/permissions/",
  };
  for (const char* prefix : kSupported) {
    if (starts_with(name, prefix))
      return true;
  }
  return false;
}

bool is_bukkit(const std::string& name) {
  return starts_with(name, "org/bukkit/") || starts_with(name, "io/papermc/") ||
         starts_with(name, "com/destroystokyo/paper/") || starts_with(name, "org/spigotmc/");
}

bool is_server_internal(const std::string& name) {
  return starts_with(name, "net/minecraft/") || starts_with(name, "org/bukkit/craftbukkit/") ||
         starts_with(name, "com/mojang/") || contains(name, "/nms/");
}

bool is_reflection_class(const std::string& name) {
  return starts_with(name, "java/lang/reflect/") || starts_with(name, "java/lang/invoke/") ||
         starts_with(name, "java/lang/instrument/") || starts_with(name, "sun/reflect/") ||
         name == "sun/misc/Unsafe" || starts_with(name, "org/objectweb/asm/");
}

bool is_platform(const std::string& name) {
  static const char* kPrefixes[] = {
      "java/", "javax/", "jdk/", "sun/", "kotlin/", "kotlinx/", "scala/", "org/jetbrains/",
  };
  for (const char* prefix : kPrefixes) {
    if (starts_with(name, prefix))
      return true;
  }
  return false;
}

bool is_reflection_member(const std::string& owner, const std::string& member) {
  if (owner == "java/lang/Class") {
    static const char* kMembers[] = {
        "forName",
        "getMethod",
        "getMethods",
        "getDeclaredMethod",
        "getField",
        "getFields",
        "getDeclaredField",
        "getConstructor",
        "getConstructors",
        "getDeclaredConstructor",
        "getDeclaredConstructors",
        "newInstance",
    };
    for (const char* name : kMembers) {
      if (member == name)
        return true;
    }
  }
  if (owner == "java/lang/ClassLoader" &&
      (member == "defineClass" || member == "loadClass" || member == "getResourceAsStream"))
    return true;
  if (owner == "java/lang/System" && (member == "load" || member == "loadLibrary"))
    return true;
  return starts_with(owner, "java/lang/reflect/") || starts_with(owner, "org/objectweb/asm/");
}

void classify(const std::vector<std::string>& class_refs,
              const std::vector<std::string>& method_refs, Usage* usage) {
  for (const std::string& name : class_refs) {
    if (is_server_internal(name))
      append_unique(&usage->nms, name);
    else if (is_reflection_class(name))
      append_unique(&usage->reflection, name);
    else if (is_supported_api(name))
      append_unique(&usage->supported, name);
    else if (is_bukkit(name))
      append_unique(&usage->wider, name);
    else if (!is_platform(name))
      append_unique(&usage->unknown, name);
  }
  for (const std::string& reference : method_refs) {
    const std::size_t hash = reference.find('#');
    const std::size_t space = reference.find(' ', hash == std::string::npos ? 0 : hash);
    if (hash == std::string::npos || space == std::string::npos)
      continue;
    const std::string owner = reference.substr(0, hash);
    const std::string member = reference.substr(hash + 1, space - hash - 1);
    if (!is_reflection_member(owner, member))
      continue;
    const std::string hint = owner + "#" + member;
    append_unique(&usage->reflection_members, hint);
    if (is_reflection_class(owner))
      append_unique(&usage->reflection, owner);
  }
  sort_unique(&usage->supported);
  sort_unique(&usage->wider);
  sort_unique(&usage->nms);
  sort_unique(&usage->reflection);
  sort_unique(&usage->unknown);
  sort_unique(&usage->reflection_members);
}

std::string count_text(std::size_t count, const char* noun) {
  return std::to_string(count) + " " + noun + (count == 1 ? "" : "s");
}

void print_list(std::string* out, const char* title, const std::vector<std::string>& values) {
  if (values.empty())
    return;
  *out += std::string("  ") + title + ": " + std::to_string(values.size()) + "\n";
  const std::size_t shown = std::min(values.size(), kMaxListed);
  for (std::size_t i = 0; i < shown; ++i)
    *out += "    " + values[i] + "\n";
  if (values.size() > shown)
    *out += "    ... and " + std::to_string(values.size() - shown) + " more\n";
}

} // namespace

bool parse_manifest(const std::string& text, PluginManifest* out, std::string* error) {
  *out = PluginManifest{};
  std::string active_block;
  std::size_t start = 0;
  int line_number = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    std::string line =
        text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    ++line_number;
    start = end == std::string::npos ? text.size() + 1 : end + 1;

    const std::size_t comment = line.find(" #");
    if (comment != std::string::npos)
      line.erase(comment);
    const std::string stripped = trim(line);
    if (stripped.empty() || stripped.front() == '#')
      continue;

    std::size_t indent = 0;
    while (indent < line.size() && line[indent] == ' ')
      ++indent;
    const std::string content = trim(line);
    const std::size_t colon = content.find(':');
    if (colon == std::string::npos) {
      *error = "line " + std::to_string(line_number) + ": expected 'key: value'";
      return false;
    }
    const std::string key = trim(content.substr(0, colon));
    const std::string value = trim(content.substr(colon + 1));
    if (key.empty()) {
      *error = "line " + std::to_string(line_number) + ": empty key";
      return false;
    }
    if (indent > 0) {
      if (active_block == "commands" && indent == 2 && value.empty())
        append_unique(&out->commands, key);
      else if (active_block == "permissions" && indent == 2 && value.empty())
        append_unique(&out->permissions, key);
      continue;
    }

    if (value.empty() && (key == "commands" || key == "permissions")) {
      active_block = key;
      continue;
    }
    active_block.clear();
    if (key == "name")
      out->name = unquote(value);
    else if (key == "version")
      out->version = unquote(value);
    else if (key == "main")
      out->main = unquote(value);
    else if (key == "api-version")
      out->api_version = unquote(value);
    else if (key == "description")
      out->description = unquote(value);
    else if (key == "author")
      out->authors.push_back(unquote(value));
    else if (key == "authors") {
      for (std::string author : split_list(value))
        out->authors.push_back(std::move(author));
    } else if (key == "depend") {
      for (std::string item : split_list(value))
        out->depend.push_back(std::move(item));
    } else if (key == "softdepend") {
      for (std::string item : split_list(value))
        out->softdepend.push_back(std::move(item));
    }
  }
  return true;
}

std::string score_name(Score score) {
  switch (score) {
  case Score::Green:
    return "green";
  case Score::Yellow:
    return "yellow";
  case Score::Red:
    return "red";
  }
  return "red";
}

Analysis analyze_jar(const std::filesystem::path& path) {
  Analysis analysis;
  analysis.package = path.string();
  ZipArchive zip;
  std::string error;
  if (!zip.open(path, &error)) {
    analysis.error = error;
    return analysis;
  }
  analysis.entries = zip.entries().size();

  const ZipEntry* manifest_entry = zip.find("plugin.yml");
  const char* manifest_name = "plugin.yml";
  if (manifest_entry == nullptr) {
    manifest_entry = zip.find("paper-plugin.yml");
    manifest_name = "paper-plugin.yml";
  }
  if (manifest_entry != nullptr) {
    if (manifest_entry->uncompressed_size > kMaxManifestBytes) {
      analysis.error = std::string(manifest_name) + " is larger than the analyzer's limit";
      return analysis;
    }
    std::vector<uint8_t> bytes;
    if (!zip.read(*manifest_entry, &bytes, &error)) {
      analysis.error = std::string(manifest_name) + ": " + error;
      return analysis;
    }
    if (!parse_manifest(std::string(bytes.begin(), bytes.end()), &analysis.manifest, &error)) {
      analysis.error = std::string(manifest_name) + ": " + error;
      return analysis;
    }
    analysis.manifest.file = manifest_name;
    analysis.has_manifest = true;
  } else {
    analysis.notes.push_back("no plugin.yml or paper-plugin.yml");
  }

  std::vector<std::string> class_refs;
  std::vector<std::string> method_refs;
  std::vector<std::string> field_refs;
  std::vector<std::string> class_names;
  for (const ZipEntry& entry : zip.entries()) {
    if (!ends_with(entry.name, ".class") || entry.name == "module-info.class")
      continue;
    if (starts_with(entry.name, "META-INF/versions/")) {
      ++analysis.skipped_entries;
      continue;
    }
    if (analysis.classes.size() >= kMaxClasses) {
      analysis.error = "archive has more classes than the analyzer's limit";
      return analysis;
    }
    if (entry.uncompressed_size > kMaxClassBytes) {
      analysis.error = entry.name + " is larger than the analyzer's class limit";
      return analysis;
    }
    std::vector<uint8_t> bytes;
    if (!zip.read(entry, &bytes, &error)) {
      analysis.error = entry.name + ": " + error;
      return analysis;
    }
    ClassScan scan;
    if (!parse_class(bytes.data(), bytes.size(), &scan, &error)) {
      analysis.error = entry.name + ": " + error;
      return analysis;
    }
    analysis.classes.push_back(
        ClassReport{scan.name, scan.major, scan.method_count,
                    scan.class_refs.size() + scan.method_refs.size() + scan.field_refs.size()});
    class_names.push_back(scan.name);
    class_refs.insert(class_refs.end(), scan.class_refs.begin(), scan.class_refs.end());
    method_refs.insert(method_refs.end(), scan.method_refs.begin(), scan.method_refs.end());
    field_refs.insert(field_refs.end(), scan.field_refs.begin(), scan.field_refs.end());
  }
  if (analysis.classes.empty()) {
    analysis.error = "archive contains no class files";
    return analysis;
  }
  std::sort(
      analysis.classes.begin(), analysis.classes.end(),
      [](const ClassReport& left, const ClassReport& right) { return left.name < right.name; });
  sort_unique(&class_names);
  sort_unique(&class_refs);
  sort_unique(&method_refs);
  sort_unique(&field_refs);
  analysis.class_refs = class_refs;
  const auto parse_member = [](const std::string& text, MemberRef* out) {
    const std::size_t hash = text.find('#');
    const std::size_t space = hash == std::string::npos ? std::string::npos : text.find(' ', hash);
    if (hash == std::string::npos || space == std::string::npos)
      return false;
    out->owner = text.substr(0, hash);
    out->name = text.substr(hash + 1, space - hash - 1);
    out->descriptor = text.substr(space + 1);
    return true;
  };
  for (const std::string& text : method_refs) {
    MemberRef ref;
    if (parse_member(text, &ref))
      analysis.method_refs.push_back(std::move(ref));
  }
  for (const std::string& text : field_refs) {
    MemberRef ref;
    if (parse_member(text, &ref))
      analysis.field_refs.push_back(std::move(ref));
  }

  if (analysis.has_manifest) {
    if (analysis.manifest.main.empty()) {
      analysis.notes.push_back("plugin manifest declares no main class");
    } else {
      std::string main = analysis.manifest.main;
      std::replace(main.begin(), main.end(), '.', '/');
      if (!std::binary_search(class_names.begin(), class_names.end(), main))
        analysis.notes.push_back("declared main class " + analysis.manifest.main +
                                 " is not in the archive");
    }
  }

  classify(class_refs, method_refs, &analysis.usage);
  if (!analysis.usage.unknown.empty())
    analysis.notes.push_back(count_text(analysis.usage.unknown.size(), "bundled or library class") +
                             " outside the API and the platform");
  if (analysis.skipped_entries != 0)
    analysis.notes.push_back(count_text(analysis.skipped_entries, "multi-release entry") +
                             " skipped");

  if (!analysis.has_manifest) {
    analysis.score = Score::Red;
    analysis.reason = "no plugin manifest, so this is not a Bukkit plugin";
  } else if (!analysis.usage.nms.empty() && !analysis.usage.reflection.empty()) {
    analysis.score = Score::Red;
    analysis.reason = "uses " + count_text(analysis.usage.nms.size(), "server-internal class") +
                      " and reflection";
  } else if (!analysis.usage.nms.empty()) {
    analysis.score = Score::Red;
    analysis.reason = "uses " + count_text(analysis.usage.nms.size(), "server-internal class") +
                      " (NMS/CraftBukkit)";
  } else if (!analysis.usage.reflection.empty() || !analysis.usage.reflection_members.empty()) {
    analysis.score = Score::Red;
    analysis.reason = "uses reflection to reach classes or members the API does not expose";
  } else if (!analysis.usage.wider.empty()) {
    analysis.score = Score::Yellow;
    analysis.reason =
        "uses " + count_text(analysis.usage.wider.size(), "Bukkit/Paper class") +
        " outside the mechanically mapped plugin, event, command, scheduler and config set";
  } else {
    analysis.score = Score::Green;
    analysis.reason = "only the mapped plugin, event, command, scheduler and config surface (" +
                      count_text(analysis.usage.supported.size(), "class") + ")";
  }
  analysis.ok = true;
  return analysis;
}

std::string report_text(const Analysis& analysis) {
  std::string out = "+MC converter analyze\n";
  out += "package: " + analysis.package + "\n";
  if (!analysis.ok)
    return out + "error: " + analysis.error + "\n";
  if (analysis.has_manifest) {
    const PluginManifest& manifest = analysis.manifest;
    out += "manifest: " + manifest.file;
    if (!manifest.name.empty())
      out += " (name " + manifest.name + ")";
    if (!manifest.version.empty())
      out += ", version " + manifest.version;
    if (!manifest.api_version.empty())
      out += ", api-version " + manifest.api_version;
    out += "\n";
    if (!manifest.main.empty())
      out += "main: " + manifest.main + "\n";
    if (!manifest.authors.empty()) {
      out += "authors:";
      for (const std::string& author : manifest.authors)
        out += " " + author;
      out += "\n";
    }
    if (!manifest.depend.empty())
      out += "depend: " + std::to_string(manifest.depend.size()) + "\n";
    if (!manifest.commands.empty())
      out += "commands: " + std::to_string(manifest.commands.size()) + "\n";
    if (!manifest.permissions.empty())
      out += "permissions: " + std::to_string(manifest.permissions.size()) + "\n";
  } else {
    out += "manifest: none\n";
  }
  out += "classes: " + std::to_string(analysis.classes.size()) + "\n";
  const std::size_t shown = std::min(analysis.classes.size(), kMaxListed);
  for (std::size_t i = 0; i < shown; ++i) {
    const ClassReport& report = analysis.classes[i];
    out += "  " + report.name + " (major " + std::to_string(report.major) + ", " +
           std::to_string(report.methods) + " methods, " + std::to_string(report.references) +
           " refs)\n";
  }
  if (analysis.classes.size() > shown)
    out += "  ... and " + std::to_string(analysis.classes.size() - shown) + " more\n";
  out += "score: " + score_name(analysis.score) + "\n";
  out += "reason: " + analysis.reason + "\n";
  print_list(&out, "mapped API", analysis.usage.supported);
  print_list(&out, "needs mapping", analysis.usage.wider);
  print_list(&out, "server internals", analysis.usage.nms);
  print_list(&out, "reflection", analysis.usage.reflection);
  print_list(&out, "reflection members", analysis.usage.reflection_members);
  print_list(&out, "other references", analysis.usage.unknown);
  for (const std::string& note : analysis.notes)
    out += "note: " + note + "\n";
  return out;
}

std::string report_json(const Analysis& analysis) {
  std::string out = "{\"tool\":\"plusmc-convert\",\"schema\":1,\"package\":";
  out += json_quote(analysis.package);
  out += ",\"ok\":";
  out += analysis.ok ? "true" : "false";
  if (!analysis.ok) {
    out += ",\"error\":";
    out += json_quote(analysis.error);
    return out + "}\n";
  }
  const PluginManifest& manifest = analysis.manifest;
  out += ",\"manifest\":{\"file\":";
  out += json_quote(manifest.file);
  out += ",\"name\":";
  out += json_quote(manifest.name);
  out += ",\"version\":";
  out += json_quote(manifest.version);
  out += ",\"main\":";
  out += json_quote(manifest.main);
  out += ",\"api_version\":";
  out += json_quote(manifest.api_version);
  out += ",\"authors\":";
  json_list(&out, manifest.authors);
  out += ",\"depend\":";
  json_list(&out, manifest.depend);
  out += ",\"softdepend\":";
  json_list(&out, manifest.softdepend);
  out += ",\"commands\":";
  json_list(&out, manifest.commands);
  out += ",\"permissions\":";
  json_list(&out, manifest.permissions);
  out += "},\"classes\":[";
  const std::size_t class_cap = std::min<std::size_t>(analysis.classes.size(), 500);
  for (std::size_t i = 0; i < class_cap; ++i) {
    if (i != 0)
      out += ",";
    out += "{\"name\":";
    out += json_quote(analysis.classes[i].name);
    out += ",\"major\":";
    out += std::to_string(analysis.classes[i].major);
    out += ",\"methods\":";
    out += std::to_string(analysis.classes[i].methods);
    out += ",\"references\":";
    out += std::to_string(analysis.classes[i].references);
    out += "}";
  }
  out += "],\"score\":";
  out += json_quote(score_name(analysis.score));
  out += ",\"reason\":";
  out += json_quote(analysis.reason);
  out += ",\"usage\":{\"supported\":";
  json_list(&out, analysis.usage.supported);
  out += ",\"wider\":";
  json_list(&out, analysis.usage.wider);
  out += ",\"nms\":";
  json_list(&out, analysis.usage.nms);
  out += ",\"reflection\":";
  json_list(&out, analysis.usage.reflection);
  out += ",\"reflection_members\":";
  json_list(&out, analysis.usage.reflection_members);
  out += ",\"unknown\":";
  json_list(&out, analysis.usage.unknown);
  out += "},\"notes\":";
  json_list(&out, analysis.notes);
  out += "}\n";
  return out;
}

} // namespace mc::converter
