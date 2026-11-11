// SPDX-License-Identifier: GPL-3.0-or-later

#include "converter/porter.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "converter/api_map.h"
#include "converter/report_json.h"

namespace mc::converter {
namespace {

namespace fs = std::filesystem;
using detail::json_list;
using detail::json_quote;

// A port of a very large plugin can name thousands of gap members; the diff
// stays useful by listing the first thousand and counting the rest. The full
// mapper report is still available through `map --json`.
constexpr std::size_t kMaxWorkItems = 1000;
constexpr std::size_t kTextPreview = 20;

// The generated source carries two markers. An update inserts new handlers and
// registrations immediately above them, so every line a human wrote (or may
// have moved) is kept. They are plain comments: a source that loses them still
// compiles, and an update then falls back to marking the new work manual.
constexpr const char* kHandlerMarker =
    "// +mc-convert:handler-marker: new handlers are inserted above this line.";
constexpr const char* kRegistrationMarker =
    "  // +mc-convert:registration-marker: new registrations are inserted above this line.";

struct EventTemplate {
  const char* reference;
  const char* constant;
  const char* handler;
  const char* note;
};

// The ABI events a class reference can be carried over to. A class not in this
// table is a gap with an explanation, never a silently empty handler.
constexpr EventTemplate kEvents[] = {
    {"org/bukkit/event/player/PlayerJoinEvent", "PMC_EVENT_PLAYER_JOIN", "on_player_join", ""},
    {"org/bukkit/event/player/PlayerQuitEvent", "PMC_EVENT_PLAYER_LEAVE", "on_player_leave", ""},
    {"org/bukkit/event/player/AsyncPlayerChatEvent", "PMC_EVENT_PLAYER_CHAT", "on_player_chat",
     "no format string or recipient list"},
    {"org/bukkit/event/block/BlockPlaceEvent", "PMC_EVENT_BLOCK_PLACE", "on_block_place",
     "the ABI reports the cell and state, not the item stack"},
    {"org/bukkit/event/block/BlockBreakEvent", "PMC_EVENT_BLOCK_BREAK", "on_block_break",
     "no drops or experience context"},
    {"org/bukkit/event/player/PlayerInteractEvent", "PMC_EVENT_ITEM_USE_ON", "on_item_use_on",
     "buckets and flint and steel only"},
};

std::string toml_escape(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const unsigned char c : value) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20 || c == 0x7f) {
        static constexpr char kHex[] = "0123456789abcdef";
        out += "\\u00";
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0f]);
      } else {
        out.push_back(static_cast<char>(c));
      }
      break;
    }
  }
  return out;
}

std::string c_escape(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const unsigned char c : value) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20 || c == 0x7f) {
        // A fixed three-digit octal escape cannot absorb the next character
        // the way a hex escape can.
        out += '\\';
        out += static_cast<char>('0' + ((c >> 6) & 0x07));
        out += static_cast<char>('0' + ((c >> 3) & 0x07));
        out += static_cast<char>('0' + (c & 0x07));
      } else {
        out.push_back(static_cast<char>(c));
      }
      break;
    }
  }
  return out;
}

bool is_identifier_char(const unsigned char c) {
  return std::isalnum(c) != 0;
}

// A filename, TOML `library` value and C++ project name all come from here, so
// it has to be stable and safe on every filesystem: lower-case letters, digits
// and underscores, never leading with a digit.
std::string identifier_from(const std::string& name) {
  std::string out;
  bool separator = false;
  for (const unsigned char c : name) {
    if (is_identifier_char(c) != 0) {
      out.push_back(static_cast<char>(std::tolower(c)));
      separator = false;
    } else if (!out.empty() && !separator) {
      out.push_back('_');
      separator = true;
    }
  }
  while (!out.empty() && out.back() == '_')
    out.pop_back();
  if (out.empty())
    out = "converted_plugin";
  if (std::isdigit(static_cast<unsigned char>(out.front())) != 0)
    out.insert(out.begin(), 'p');
  return out;
}

std::string join(const std::vector<std::string>& values, const std::string& separator) {
  std::string out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0)
      out += separator;
    out += values[i];
  }
  return out;
}

const ClassMapping* find_class(const MappingReport& report, const std::string& reference) {
  for (const ClassMapping& mapping : report.classes) {
    if (mapping.reference == reference)
      return &mapping;
  }
  return nullptr;
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

// A marker has to be unique: inserting at the wrong copy could put a handler
// inside another function. `duplicated` is set instead of guessing.
std::size_t find_marker(const std::string& source, const std::string& marker, bool* duplicated) {
  const std::size_t first = source.find(marker);
  if (first == std::string::npos)
    return first;
  *duplicated = source.find(marker, first + marker.size()) != std::string::npos;
  return first;
}

// New handlers must not collide with a function the human already wrote (or
// with another new handler from the same update).
std::string unique_command_identifier(const std::string& name, const std::string& source,
                                      const std::vector<std::string>& used) {
  const std::string base = identifier_from(name);
  std::string candidate = base;
  auto taken = [&](const std::string& value) {
    return contains(used, value) || source.find("void command_" + value + "(") != std::string::npos;
  };
  for (int suffix = 2; taken(candidate); ++suffix)
    candidate = base + "_" + std::to_string(suffix);
  return candidate;
}

std::string unique_handler_name(const std::string& base, const std::string& source,
                                const std::vector<std::string>& used) {
  std::string candidate = base;
  auto taken = [&](const std::string& value) {
    return contains(used, value) || source.find("void " + value + "(") != std::string::npos;
  };
  for (int suffix = 2; taken(candidate); ++suffix)
    candidate = base + "_" + std::to_string(suffix);
  return candidate;
}

void add_work(PortReport* report, std::string kind, std::string reference, std::string status,
              std::string action) {
  if (report->work.size() >= kMaxWorkItems) {
    ++report->work_truncated;
    return;
  }
  report->work.push_back(
      PortWorkItem{std::move(kind), std::move(reference), std::move(status), std::move(action)});
}

std::string trim_copy(const std::string& value) {
  const std::size_t begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos)
    return {};
  const std::size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

// `library` becomes a filename and a CMake target during an update, so it must
// be exactly the shape identifier_from produces, not an arbitrary string from a
// hand-edited manifest.
bool is_library_name(const std::string& value) {
  if (value.empty() || std::isdigit(static_cast<unsigned char>(value.front())) != 0)
    return false;
  for (const unsigned char c : value) {
    if (std::islower(c) == 0 && std::isdigit(c) == 0 && c != '_')
      return false;
  }
  return true;
}

bool read_manifest_library(const fs::path& path, std::string* library, std::string* error) {
  std::ifstream in(path);
  if (!in) {
    *error = "cannot read " + path.string();
    return false;
  }
  std::string line;
  int number = 0;
  while (std::getline(in, line)) {
    ++number;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const std::string stripped = trim_copy(line);
    if (stripped.empty() || stripped.front() == '#')
      continue;
    const std::size_t equals = stripped.find('=');
    if (equals == std::string::npos)
      continue;
    if (trim_copy(stripped.substr(0, equals)) != "library")
      continue;
    std::string value = trim_copy(stripped.substr(equals + 1));
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\''))) {
      value = value.substr(1, value.size() - 2);
    }
    if (value.empty()) {
      *error = path.string() + ": line " + std::to_string(number) + ": library is empty";
      return false;
    }
    *library = value;
    return true;
  }
  *error = path.string() + " has no library entry";
  return false;
}

bool write_file(const fs::path& path, const std::string& content, std::string* error) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    *error = "cannot open " + path.string() + " for writing";
    return false;
  }
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  out.close();
  if (!out) {
    *error = "cannot write " + path.string();
    return false;
  }
  return true;
}

std::string c_string_or_null(const std::string& value) {
  return value.empty() ? "nullptr" : "\"" + c_escape(value) + "\"";
}

std::string command_manifest(const PortReport& report) {
  std::string out = "# Generated by `plusmc-convert port` from " + report.package + "\n";
  out += "# `library` is the module this project builds; the host adds the\n";
  out += "# platform suffix (.dylib, .so or .dll).\n";
  out += "name = \"" + toml_escape(report.plugin_name) + "\"\n";
  out += "version = \"" + toml_escape(report.plugin_version) + "\"\n";
  out += "api = 1\n";
  out += "library = \"" + toml_escape(report.identifier) + "\"\n";
  out += "entry = \"pmc_plugin_entry\"\n";
  return out;
}

struct CommandStub {
  std::string name;
  std::string identifier;
};

std::vector<CommandStub> command_stubs(const std::vector<std::string>& names) {
  std::vector<CommandStub> stubs;
  for (const std::string& name : names) {
    const std::string identifier = identifier_from(name);
    std::string unique = identifier;
    for (int suffix = 2;
         std::any_of(stubs.begin(), stubs.end(),
                     [&](const CommandStub& stub) { return stub.identifier == unique; });
         ++suffix) {
      unique = identifier + "_" + std::to_string(suffix);
    }
    stubs.push_back(CommandStub{name, unique});
  }
  return stubs;
}

struct GeneratedEvent {
  const EventTemplate* event = nullptr;
  MapStatus status = MapStatus::Mapped;
  std::string note;
};

// The generated command handler is functional on its own: it logs the
// invocation and answers the caller with the original usage line. It is not a
// translation of the Java body (the body was never read), but it is a working
// handler with no placeholder comment, and it keeps the command usable while
// the original behavior is written.
std::string command_stub_text(const CommandStub& command) {
  std::string out = "// The original manifest declared /" + command.name + ".\n";
  out += "void command_" + command.identifier + "(void* data, pmc_command* command) {\n";
  out += "  State* state = static_cast<State*>(data);\n";
  out += "  std::string invoked = \"/\";\n";
  out += "  invoked += command->name;\n";
  out += "  for (int i = 1; i < command->argc; ++i) {\n";
  out += "    invoked += ' ';\n";
  out += "    invoked += command->argv[i];\n";
  out += "  }\n";
  out += "  state->api->log(state->api->host, PMC_LOG_DEBUG, invoked.c_str());\n";
  out += "  std::string usage = \"usage: /\";\n";
  out += "  usage += command->name;\n";
  out += "  state->api->reply(state->api->host, command->reply, usage.c_str());\n";
  out += "}\n";
  out += "\n";
  return out;
}

std::string command_registration_text(const CommandStub& command) {
  return "  failed |= api->command(api->host, \"" + c_escape(command.name) + "\", \"/" +
         c_escape(command.name) + "\", command_" + command.identifier + ", state);\n";
}

// The generated listener records the event at debug level, including the
// player when the ABI carries one. It never changes `cancel`, so it cannot
// alter engine behavior by accident. This is a complete, working listener; the
// original recipient list / format / drop logic is separate port work named in
// port.json.
std::string event_stub_text(const GeneratedEvent& generated, const std::string& handler) {
  const std::string reference = generated.event->reference;
  const std::size_t slash = reference.rfind('/');
  const std::string label = slash == std::string::npos ? reference : reference.substr(slash + 1);
  std::string out = "// The original listener used " + reference + ".\n";
  if (!generated.note.empty())
    out += "// ABI limit: " + generated.note + ".\n";
  out += "void " + handler + "(void* data, pmc_event* event) {\n";
  out += "  State* state = static_cast<State*>(data);\n";
  out += "  std::string line = \"" + c_escape(label) + "\";\n";
  out += "  if (event->player != nullptr) {\n";
  out += "    line += \": \";\n";
  out += "    line += event->player;\n";
  out += "  }\n";
  out += "  state->api->log(state->api->host, PMC_LOG_DEBUG, line.c_str());\n";
  out += "}\n";
  out += "\n";
  return out;
}

std::string event_registration_text(const GeneratedEvent& generated, const std::string& handler) {
  return "  failed |= api->on(api->host, " + std::string(generated.event->constant) + ", " +
         handler + ", state);\n";
}

std::string generate_source(const PortReport& report, const PluginManifest& manifest,
                            const std::vector<CommandStub>& commands,
                            const std::vector<GeneratedEvent>& events) {
  std::string out;
  out += "// Generated by `plusmc-convert port` from " + report.package + ".\n";
  out += "//\n";
  out += "// This is a scaffold, not a port. Every command and event handler below is a\n";
  out += "// working default (it logs and, for a command, answers with its usage); the\n";
  out += "// original method bodies were never decompiled or read. `port.json` lists the\n";
  out += "// behavior that still has to be written.\n";
  out += "\n";
  out += "#include <string>\n";
  out += "\n";
  out += "#include \"pluginapi/plugin.h\"\n";
  out += "\n";
  out += "namespace {\n";
  out += "\n";
  out += "struct State {\n";
  out += "  const pmc_host_api* api = nullptr;\n";
  out += "};\n";
  out += "\n";

  for (const GeneratedEvent& generated : events)
    out += event_stub_text(generated, generated.event->handler);

  for (const CommandStub& command : commands)
    out += command_stub_text(command);

  out += kHandlerMarker;
  out += "\n\n";
  out += "int on_load(const pmc_host_api* api, void** plugin_data) {\n";
  out += "  State* state = new State();\n";
  out += "  state->api = api;\n";
  out += "  int failed = 0;\n";
  for (const CommandStub& command : commands)
    out += command_registration_text(command);
  for (const GeneratedEvent& generated : events)
    out += event_registration_text(generated, generated.event->handler);
  out += kRegistrationMarker;
  out += "\n";
  out += "  if (failed != 0) {\n";
  out += "    delete state;\n";
  out += "    return 1;\n";
  out += "  }\n";
  out += "  *plugin_data = state;\n";
  out += "  return 0;\n";
  out += "}\n";
  out += "\n";
  out += "void on_enable(void* plugin_data) {\n";
  out += "  State* state = static_cast<State*>(plugin_data);\n";
  out += "  state->api->log(state->api->host, PMC_LOG_INFO, \"" + c_escape(report.plugin_name) +
         " enabled (generated port scaffold)\");\n";
  out += "}\n";
  out += "\n";
  out += "void on_disable(void* plugin_data) {\n";
  out += "  State* state = static_cast<State*>(plugin_data);\n";
  out += "  state->api->log(state->api->host, PMC_LOG_INFO, \"" + c_escape(report.plugin_name) +
         " disabled\");\n";
  out += "}\n";
  out += "\n";
  out += "void on_unload(void* plugin_data) {\n";
  out += "  delete static_cast<State*>(plugin_data);\n";
  out += "}\n";
  out += "\n";
  out += "} // namespace\n";
  out += "\n";
  out += "extern \"C\" PMC_PLUGIN_EXPORT const pmc_plugin* pmc_plugin_entry(void) {\n";
  out += "  static const pmc_plugin plugin = {\n";
  out += "      sizeof(pmc_plugin),\n";
  out += "      PMC_PLUGIN_ABI_VERSION,\n";
  out += "      \"" + c_escape(report.plugin_name) + "\",\n";
  out += "      \"" + c_escape(report.plugin_version) + "\",\n";
  out += "      " + c_string_or_null(join(manifest.authors, ", ")) + ",\n";
  out += "      " + c_string_or_null(manifest.description) + ",\n";
  out += "      on_load,\n";
  out += "      on_enable,\n";
  out += "      on_disable,\n";
  out += "      on_unload,\n";
  out += "  };\n";
  out += "  return &plugin;\n";
  out += "}\n";
  return out;
}

std::string scaffold_cmake(const PortReport& report) {
  std::string out;
  out += "# Generated by `plusmc-convert port`. Builds one plugin module against the\n";
  out += "# +MC plugin ABI. Pass the engine tree with -DPLUSMC_ROOT=/path/to/plusmc.\n";
  out += "cmake_minimum_required(VERSION 3.24)\n";
  out += "project(" + report.identifier + "_plugin LANGUAGES CXX)\n";
  out += "\n";
  out += "set(CMAKE_CXX_STANDARD 20)\n";
  out += "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n";
  out += "set(CMAKE_CXX_EXTENSIONS OFF)\n";
  out += "\n";
  out += "set(PLUSMC_ROOT \"\" CACHE PATH \"Path to the +MC tree (contains "
         "src/pluginapi/plugin.h)\")\n";
  out += "if(NOT PLUSMC_ROOT)\n";
  out += "  message(FATAL_ERROR \"Pass -DPLUSMC_ROOT=/path/to/plusmc\")\n";
  out += "endif()\n";
  out += "\n";
  out += "add_library(" + report.identifier + " MODULE " + report.identifier + ".cpp)\n";
  out += "target_include_directories(" + report.identifier + " PRIVATE ${PLUSMC_ROOT}/src)\n";
  out += "set_target_properties(" + report.identifier + " PROPERTIES\n";
  out += "  PREFIX \"\"\n";
  out += "  OUTPUT_NAME \"" + report.identifier + "\")\n";
  out += "\n";
  out += "if(WIN32)\n";
  out += "  set_target_properties(" + report.identifier + " PROPERTIES SUFFIX \".dll\")\n";
  out += "elseif(APPLE)\n";
  out += "  set_target_properties(" + report.identifier + " PROPERTIES SUFFIX \".dylib\")\n";
  out += "else()\n";
  out += "  set_target_properties(" + report.identifier + " PROPERTIES SUFFIX \".so\")\n";
  out += "endif()\n";
  out += "\n";
  out += "if(CMAKE_CXX_COMPILER_ID MATCHES \"Clang|GNU\")\n";
  out += "  target_compile_options(" + report.identifier + " PRIVATE -Wall -Wextra)\n";
  out += "endif()\n";
  return out;
}

std::string scaffold_readme(const PortReport& report) {
  std::string out;
  out += "# " + report.plugin_name + " — +MC port scaffold\n";
  out += "\n";
  out += "Generated by `plusmc-convert port` from `" + report.package + "`. This is a\n";
  out += "**starting point, not a port**: it builds and loads, and every handler has a\n";
  out += "working default body (log + usage reply). The original jar was read statically;\n";
  out += "no method body was decompiled.\n";
  out += "\n";
  out += "## What is here\n";
  out += "\n";
  out += "- `+mc-plugin.toml` — the native manifest (name, version, ABI 1, module).\n";
  out += "- `" + report.identifier + ".cpp` — the lifecycle, " + std::to_string(report.commands) +
         " command handler(s) and " + std::to_string(report.events) + " event listener(s).\n";
  out += "- `CMakeLists.txt` — builds the module against `src/pluginapi/plugin.h`.\n";
  out += "- `port.json` — the machine-readable work list behind the summary below.\n";
  out += "\n";
  out += "## Build and load\n";
  out += "\n";
  out += "```sh\n";
  out += "cmake -S . -B build -DPLUSMC_ROOT=/path/to/plusmc\n";
  out += "cmake --build build\n";
  out += "# copy build/" + report.identifier + ".dylib (macOS), .so (Linux) or .dll (Windows)\n";
  out += "# beside +mc-plugin.toml, then start the server with:\n";
  out += "#   ./plusmc --plugins <this directory>\n";
  out += "```\n";
  out += "\n";
  out += "The host looks for `" + report.identifier +
         ".dylib`/`.so`/`.dll` next to the manifest; a `lib` prefix is\n";
  out += "also accepted.\n";
  out += "\n";
  out += "## What still has to be written\n";
  out += "\n";
  out += "The scaffold has " + std::to_string(report.work.size()) + " work item(s)";
  if (report.work_truncated != 0)
    out += " (the first " + std::to_string(report.work.size()) + "; " +
           std::to_string(report.work_truncated) + " more are counted in `port.json`)";
  out += ":\n";
  out += "\n";
  const std::size_t shown = std::min(report.work.size(), kTextPreview);
  for (std::size_t i = 0; i < shown; ++i) {
    const PortWorkItem& item = report.work[i];
    out += "- **" + item.kind + "** `" + item.reference + "` (" + item.status + ") — " +
           item.action + "\n";
  }
  if (report.work.size() > shown)
    out += "- … and " + std::to_string(report.work.size() - shown) + " more in `port.json`.\n";
  out += "\n";
  out += "## Honest limits\n";
  out += "\n";
  out += "- The analyzer reads the constant pool, not control flow. A reference that\n";
  out += "  is present but never reached still appears in the work list.\n";
  out += "- Command names and permissions come from the manifest. Their original\n";
  out += "  bodies, tab completion and permission checks are not recoverable this way.\n";
  out += "- Only the ABI 1 events listed under `event` work items are wired. A wider\n";
  out += "  Bukkit event is a gap, even when a similar ABI event exists.\n";
  out += "- The generated source is a compile-checked skeleton; it does not preserve the\n";
  out += "  original plugin's scheduling, storage or configuration layout.\n";
  return out;
}

const char* port_status_name(MapStatus status) {
  switch (status) {
  case MapStatus::Mapped:
    return "mapped";
  case MapStatus::Partial:
    return "partial";
  case MapStatus::Gap:
    return "gap";
  }
  return "gap";
}

void add_mapping_work(PortReport* report, const MappingReport& mapping) {
  for (const ClassMapping& item : mapping.classes) {
    if (item.status == MapStatus::Gap) {
      add_work(report, "reference", item.reference, "gap",
               item.note.empty() ? "no native equivalent in ABI 1"
                                 : "no native equivalent: " + item.note);
    }
  }
  for (const MemberMapping& item : mapping.members) {
    if (item.status == MapStatus::Mapped)
      continue;
    std::string reference = item.owner + "#" + item.name;
    if (!item.descriptor.empty())
      reference += "(" + item.descriptor + ")";
    const std::string note = item.note.empty()
                                 ? (item.status == MapStatus::Gap ? "no native equivalent in ABI 1"
                                                                  : "native path is incomplete")
                                 : item.note;
    add_work(report, "reference", reference, port_status_name(item.status),
             (item.field ? "field: " : "") + note);
  }
}

} // namespace

PortReport port_jar(const PortOptions& options) {
  PortReport report;
  report.package = options.package.string();
  report.out_dir = options.out_dir.string();

  const Analysis analysis = analyze_jar(options.package);
  if (!analysis.ok) {
    report.error = analysis.error;
    return report;
  }
  if (!analysis.has_manifest) {
    report.error = "the archive has no plugin.yml or paper-plugin.yml; there is nothing to port";
    return report;
  }
  if (analysis.manifest.name.empty()) {
    report.error = analysis.manifest.file + ": the manifest has no name";
    return report;
  }

  const MappingReport mapping = map_analysis(analysis);
  if (!mapping.ok) {
    report.error = mapping.error;
    return report;
  }
  report.plugin_name = analysis.manifest.name;
  report.plugin_version = analysis.manifest.version.empty() ? "0.0.0" : analysis.manifest.version;
  report.identifier = identifier_from(analysis.manifest.name);
  report.score = mapping.score;
  for (const ClassMapping& item : mapping.classes) {
    switch (item.status) {
    case MapStatus::Mapped:
      ++report.mapped;
      break;
    case MapStatus::Partial:
      ++report.partial;
      break;
    case MapStatus::Gap:
      ++report.gap;
      break;
    }
  }
  for (const MemberMapping& item : mapping.members) {
    switch (item.status) {
    case MapStatus::Mapped:
      ++report.mapped;
      break;
    case MapStatus::Partial:
      ++report.partial;
      break;
    case MapStatus::Gap:
      ++report.gap;
      break;
    }
  }

  const std::string source_name = report.identifier + ".cpp";
  std::string preserved_source;
  std::error_code ec;
  if (options.from_port) {
    report.from_port = true;
    const bool exists = fs::exists(options.out_dir, ec);
    if (ec) {
      report.error = "cannot inspect " + options.out_dir.string() + ": " + ec.message();
      return report;
    }
    if (!exists) {
      report.error = "--from-port needs an existing port at " + options.out_dir.string() +
                     "; run port without --from-port into an empty directory first";
      return report;
    }
    if (!fs::is_directory(options.out_dir, ec)) {
      report.error = options.out_dir.string() + " is not a directory";
      return report;
    }
    const fs::path manifest_path = options.out_dir / "+mc-plugin.toml";
    if (!fs::exists(manifest_path)) {
      report.error = "--from-port found no +mc-plugin.toml in " + options.out_dir.string() +
                     "; it updates an existing port";
      return report;
    }
    std::string library;
    std::string manifest_error;
    if (!read_manifest_library(manifest_path, &library, &manifest_error)) {
      report.error = manifest_error;
      return report;
    }
    if (!is_library_name(library)) {
      report.error =
          manifest_path.string() + ": library \"" + library + "\" is not a valid module name";
      return report;
    }
    if (library != report.identifier) {
      report.error = "the plugin identifier changed from " + library + " to " + report.identifier +
                     "; refusing to orphan " + library +
                     ".cpp (rename the plugin back or start a new port)";
      return report;
    }
    const fs::path source_path = options.out_dir / source_name;
    const bool source_exists = fs::exists(source_path, ec);
    if (ec) {
      report.error = "cannot inspect " + source_path.string() + ": " + ec.message();
      return report;
    }
    if (!source_exists) {
      report.error = "--from-port needs the preserved source " + source_path.string() +
                     "; refusing to regenerate it over a missing file";
      return report;
    }
    std::ifstream source_in(source_path, std::ios::binary);
    if (!source_in) {
      report.error = "cannot read " + source_path.string();
      return report;
    }
    preserved_source.assign(std::istreambuf_iterator<char>(source_in),
                            std::istreambuf_iterator<char>());
    if (preserved_source.empty()) {
      report.error =
          source_path.string() + " is empty; refusing to update a port with no preserved handlers";
      return report;
    }
    report.preserved.push_back(source_name);
    if (fs::exists(options.out_dir / "CMakeLists.txt"))
      report.preserved.push_back("CMakeLists.txt");
    if (fs::exists(options.out_dir / "README.md"))
      report.preserved.push_back("README.md");
  } else {
    if (fs::exists(options.out_dir, ec)) {
      if (!fs::is_directory(options.out_dir, ec)) {
        report.error = options.out_dir.string() + " is not a directory";
        return report;
      }
      if (!fs::is_empty(options.out_dir, ec)) {
        report.error = options.out_dir.string() + " is not empty; refusing to overwrite it";
        return report;
      }
    } else if (ec) {
      report.error = "cannot inspect " + options.out_dir.string() + ": " + ec.message();
      return report;
    }
    fs::create_directories(options.out_dir, ec);
    if (ec) {
      report.error = "cannot create " + options.out_dir.string() + ": " + ec.message();
      return report;
    }
  }

  std::vector<GeneratedEvent> events;
  for (const EventTemplate& candidate : kEvents) {
    if (!contains(analysis.usage.supported, candidate.reference) &&
        !contains(analysis.usage.wider, candidate.reference)) {
      continue;
    }
    GeneratedEvent generated;
    generated.event = &candidate;
    generated.note = candidate.note;
    if (const ClassMapping* item = find_class(mapping, candidate.reference); item != nullptr) {
      generated.status = item->status;
      if (!item->note.empty())
        generated.note = item->note;
    }
    events.push_back(generated);
  }

  std::vector<std::string> commands = analysis.manifest.commands;
  std::sort(commands.begin(), commands.end());
  commands.erase(std::unique(commands.begin(), commands.end()), commands.end());
  std::vector<std::string> permissions = analysis.manifest.permissions;
  std::sort(permissions.begin(), permissions.end());
  permissions.erase(std::unique(permissions.begin(), permissions.end()), permissions.end());

  report.commands = commands.size();
  report.permissions = permissions.size();
  report.events = events.size();

  std::vector<std::string> missing_commands;
  std::vector<const GeneratedEvent*> missing_events;
  if (report.from_port) {
    for (const std::string& name : commands) {
      if (preserved_source.find("\"" + c_escape(name) + "\"") == std::string::npos)
        missing_commands.push_back(name);
    }
    for (const GeneratedEvent& generated : events) {
      if (preserved_source.find(generated.event->constant) == std::string::npos)
        missing_events.push_back(&generated);
    }
  }

  // Insertion is all-or-nothing: unless both generated markers occur exactly
  // once, nothing is written and the new handlers stay manual. A half-inserted
  // source would be worse than an honest work item.
  bool insert = false;
  std::string updated_source;
  if (report.from_port && (!missing_commands.empty() || !missing_events.empty())) {
    bool handler_duplicated = false;
    bool registration_duplicated = false;
    const std::size_t handler_pos =
        find_marker(preserved_source, kHandlerMarker, &handler_duplicated);
    const std::size_t registration_pos =
        find_marker(preserved_source, kRegistrationMarker, &registration_duplicated);
    if (handler_pos != std::string::npos && registration_pos != std::string::npos &&
        !handler_duplicated && !registration_duplicated) {
      insert = true;
      std::string handlers_text;
      std::string registrations_text;
      std::vector<std::string> used_identifiers;
      std::vector<std::string> used_handlers;
      for (const std::string& name : missing_commands) {
        const std::string identifier =
            unique_command_identifier(name, preserved_source, used_identifiers);
        used_identifiers.push_back(identifier);
        const CommandStub stub{name, identifier};
        handlers_text += command_stub_text(stub);
        registrations_text += command_registration_text(stub);
      }
      for (const GeneratedEvent* generated : missing_events) {
        const std::string handler =
            unique_handler_name(generated->event->handler, preserved_source, used_handlers);
        used_handlers.push_back(handler);
        handlers_text += event_stub_text(*generated, handler);
        registrations_text += event_registration_text(*generated, handler);
      }
      updated_source = preserved_source;
      // Splice the later insertion first so the earlier position stays valid.
      if (handler_pos > registration_pos) {
        updated_source.insert(handler_pos, handlers_text);
        updated_source.insert(registration_pos, registrations_text);
      } else {
        updated_source.insert(registration_pos, registrations_text);
        updated_source.insert(handler_pos, handlers_text);
      }
      report.inserted = missing_commands.size() + missing_events.size();
    }
  }

  for (const std::string& name : commands) {
    // The check is textual on purpose: the source is the human's, not a format
    // this tool parses. A registration string is enough to know the handler is
    // wired; whether its body is finished is not knowable here.
    const bool registered = !report.from_port || preserved_source.find("\"" + c_escape(name) +
                                                                       "\"") != std::string::npos;
    std::string status;
    std::string action;
    if (!report.from_port) {
      status = "generated";
      action = "the handler is registered with a generated default body; replace it with the "
               "original /" +
               name + " behavior";
    } else if (registered) {
      status = "preserved";
      action = "already registered in the preserved source; confirm the handler is finished";
    } else if (insert) {
      status = "inserted";
      action = "a generated handler and its registration were inserted at the generated marker; "
               "replace the default body with the original /" +
               name + " behavior";
    } else {
      status = "manual";
      action = "the updated jar declares /" + name +
               " but the preserved source does not register it; add the handler and "
               "registration by hand";
    }
    add_work(&report, "command", name, status, action);
  }
  for (const std::string& permission : permissions) {
    add_work(&report, "permission", permission, "gap",
             "ABI 1 has no permission nodes; map it to command_level/player_op_level or a "
             "manual check");
  }
  for (const GeneratedEvent& generated : events) {
    const bool registered =
        !report.from_port || preserved_source.find(generated.event->constant) != std::string::npos;
    std::string status;
    std::string action;
    if (!report.from_port) {
      status = port_status_name(generated.status);
      action = "the listener is subscribed with a generated logging body" +
               (generated.note.empty() ? std::string() : "; ABI limit: " + generated.note);
    } else if (registered) {
      status = "preserved";
      action = "already subscribed in the preserved source; confirm the listener is finished";
    } else if (insert) {
      status = "inserted";
      action = "a generated listener and its subscription were inserted at the generated marker; "
               "replace the logging body with the original listener behavior";
    } else {
      status = "manual";
      action = "the updated jar references this event but the preserved source does not "
               "subscribe; add it by hand";
    }
    add_work(&report, "event", generated.event->reference, status, action);
  }
  if (analysis.manifest.main.empty()) {
    add_work(&report, "manual", analysis.manifest.file, "manual",
             "the manifest names no main class; choose the entry point by hand");
  }
  add_mapping_work(&report, mapping);

  if (report.from_port) {
    if (report.inserted != 0) {
      report.notes.push_back("inserted " + std::to_string(report.inserted) +
                             " new handler(s) at the generated marker; every existing source line "
                             "was kept");
      report.notes.push_back("--from-port rewrote +mc-plugin.toml, port.json and " + source_name +
                             "; CMakeLists.txt and README.md were left untouched");
    } else {
      report.notes.push_back("--from-port rewrote +mc-plugin.toml and port.json only; " +
                             join(report.preserved, ", ") + " left untouched");
    }
    if (!insert && (!missing_commands.empty() || !missing_events.empty())) {
      report.notes.push_back("the generated insertion marker is missing or repeated in " +
                             source_name +
                             "; no handlers were inserted and the new work items "
                             "are manual");
    }
  }

  if (report.score == Score::Red)
    report.notes.push_back("the analyzer score is red; this scaffold is a starting point, not a "
                           "portable plugin");
  if (!permissions.empty())
    report.notes.push_back(std::to_string(permissions.size()) +
                           " permission node(s) have no ABI equivalent; they are work items, "
                           "not generated code");
  if (commands.empty())
    report.notes.push_back("the manifest declares no commands; add handlers in on_load");
  if (events.empty())
    report.notes.push_back("no ABI event class was referenced; subscribe in on_load if the "
                           "original listener used a wider event");
  if (report.work_truncated != 0)
    report.notes.push_back(std::to_string(report.work_truncated) + " work item(s) beyond the " +
                           std::to_string(kMaxWorkItems) +
                           "-item cap are counted but not listed; use `map --json` for the "
                           "complete reference report");

  if (report.from_port) {
    report.files = {"+mc-plugin.toml", source_name, "CMakeLists.txt", "README.md", "port.json"};
    std::string error;
    if (insert && !write_file(options.out_dir / source_name, updated_source, &error)) {
      report.error = error;
      return report;
    }
    if (!write_file(options.out_dir / "+mc-plugin.toml", command_manifest(report), &error)) {
      report.error = error;
      return report;
    }
    report.ok = true;
    if (!write_file(options.out_dir / "port.json", port_report_json(report), &error)) {
      report.ok = false;
      report.error = error;
      return report;
    }
    return report;
  }

  const std::vector<std::pair<std::string, std::string>> files = {
      {"+mc-plugin.toml", command_manifest(report)},
      {source_name, generate_source(report, analysis.manifest, command_stubs(commands), events)},
      {"CMakeLists.txt", scaffold_cmake(report)},
      {"README.md", scaffold_readme(report)},
  };
  for (const auto& file : files) {
    std::string error;
    if (!write_file(options.out_dir / file.first, file.second, &error)) {
      report.error = error;
      return report;
    }
    report.files.push_back(file.first);
  }

  report.files.push_back("port.json");
  report.ok = true;
  std::string json_error;
  if (!write_file(options.out_dir / "port.json", port_report_json(report), &json_error)) {
    report.ok = false;
    report.error = json_error;
    return report;
  }
  return report;
}

std::string port_report_text(const PortReport& report) {
  std::string out = "+MC converter port\n";
  out += "package: " + report.package + "\n";
  if (!report.ok)
    return out + "error: " + report.error + "\n";
  out += "plugin: " + report.plugin_name + " " + report.plugin_version + " (" + report.identifier +
         ")\n";
  out += "score: " + score_name(report.score) + "\n";
  out += "out: " + report.out_dir + "\n";
  if (report.from_port) {
    out += "update: rewrote +mc-plugin.toml and port.json; preserved " +
           join(report.preserved, ", ") + "\n";
    if (report.inserted != 0) {
      out += "inserted: " + std::to_string(report.inserted) + " handler(s) into " +
             report.identifier + ".cpp at the generated marker\n";
    }
  }
  out += "files: " + std::to_string(report.files.size()) + " (" + join(report.files, ", ") + ")\n";
  if (report.from_port) {
    out += "commands: " + std::to_string(report.commands) +
           " declared, permissions: " + std::to_string(report.permissions) +
           ", events: " + std::to_string(report.events) + "\n";
  } else {
    out += "commands: " + std::to_string(report.commands) +
           " generated, permissions: " + std::to_string(report.permissions) +
           ", events: " + std::to_string(report.events) + "\n";
  }
  out += "mapping: " + std::to_string(report.mapped) + " mapped, " +
         std::to_string(report.partial) + " partial, " + std::to_string(report.gap) + " gap\n";
  out += "work: " + std::to_string(report.work.size()) + " item(s)";
  if (report.work_truncated != 0)
    out += " (" + std::to_string(report.work_truncated) + " more not listed)";
  out += "\n";
  const std::size_t shown = std::min(report.work.size(), kTextPreview);
  for (std::size_t i = 0; i < shown; ++i) {
    const PortWorkItem& item = report.work[i];
    out +=
        "  " + item.kind + "  " + item.reference + " [" + item.status + "] " + item.action + "\n";
  }
  if (report.work.size() > shown)
    out += "  ... " + std::to_string(report.work.size() - shown) + " more in port.json\n";
  for (const std::string& note : report.notes)
    out += "note: " + note + "\n";
  return out;
}

std::string port_report_json(const PortReport& report) {
  std::string out = "{\"tool\":\"plusmc-convert\",\"schema\":1,\"mode\":\"port\",\"package\":";
  out += json_quote(report.package);
  out += ",\"ok\":";
  out += report.ok ? "true" : "false";
  if (!report.ok) {
    out += ",\"error\":";
    out += json_quote(report.error);
    return out + "}\n";
  }
  out += ",\"score\":";
  out += json_quote(score_name(report.score));
  out += ",\"plugin\":{\"name\":";
  out += json_quote(report.plugin_name);
  out += ",\"version\":";
  out += json_quote(report.plugin_version);
  out += ",\"identifier\":";
  out += json_quote(report.identifier);
  out += "},\"out\":";
  out += json_quote(report.out_dir);
  out += ",\"from_port\":";
  out += report.from_port ? "true" : "false";
  out += ",\"preserved\":";
  json_list(&out, report.preserved);
  out += ",\"files\":";
  json_list(&out, report.files);
  out += ",\"summary\":{\"commands\":";
  out += std::to_string(report.commands);
  out += ",\"permissions\":";
  out += std::to_string(report.permissions);
  out += ",\"events\":";
  out += std::to_string(report.events);
  out += ",\"inserted\":";
  out += std::to_string(report.inserted);
  out += ",\"mapped\":";
  out += std::to_string(report.mapped);
  out += ",\"partial\":";
  out += std::to_string(report.partial);
  out += ",\"gap\":";
  out += std::to_string(report.gap);
  out += ",\"work\":";
  out += std::to_string(report.work.size());
  out += ",\"work_truncated\":";
  out += std::to_string(report.work_truncated);
  out += "},\"work\":[";
  for (std::size_t i = 0; i < report.work.size(); ++i) {
    if (i != 0)
      out += ",";
    out += "{\"kind\":";
    out += json_quote(report.work[i].kind);
    out += ",\"reference\":";
    out += json_quote(report.work[i].reference);
    out += ",\"status\":";
    out += json_quote(report.work[i].status);
    out += ",\"action\":";
    out += json_quote(report.work[i].action);
    out += "}";
  }
  out += "],\"notes\":";
  json_list(&out, report.notes);
  out += "}\n";
  return out;
}

} // namespace mc::converter
