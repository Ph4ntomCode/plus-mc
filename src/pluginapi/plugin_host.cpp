// SPDX-License-Identifier: GPL-3.0-or-later
//
// The native plugin host (docs/PLUGIN_API.md, docs/PLAN.md §9.2).
//
// Layout: one `LoadedPlugin` per `+mc-plugin.toml`, a registry of listeners,
// commands and scheduled tasks with the owning plugin recorded on each (so
// unload can drop them wholesale), and one mutex over the registries. A
// callback is always invoked with the registries unlocked — a plugin may call
// `schedule`, `reply` or `log` from inside a handler — and with the owning
// plugin on a thread-local so the log and the error paths can name it.
#include "pluginapi/plugin_host.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "core/log.h"

#ifndef PLUSMC_VERSION_STRING
#define PLUSMC_VERSION_STRING "0.1.0"
#endif

// The opaque handle a plugin is given, defined on this side of the ABI. It is
// a thin wrapper over "send this text to whoever the interaction came from";
// the host decides what that means (a player's feedback line, usually).
struct pmc_reply {
  std::function<void(const std::string&)> send;
};

namespace mc::plugins {
namespace {

constexpr const char* kManifestName = "+mc-plugin.toml";

// The dynamic loader's handle and symbol slots. Unix spells them `void*`
// (dlopen/dlsym); Windows spells them HMODULE/FARPROC
// (LoadLibrary/GetProcAddress). The functions that operate on them live below
// `library_suffix`, next to the spelling that differs.
#if defined(_WIN32)
using LibraryHandle = HMODULE;
using LibrarySymbol = FARPROC;
#else
using LibraryHandle = void*;
using LibrarySymbol = void*;
#endif

// One `key = value` entry from a plugin's config.toml. The type is kept so a
// getter can answer "absent or wrong type" with its fallback instead of
// converting a string to zero.
struct ConfigValue {
  enum class Type { kString, kInteger, kBoolean } type = Type::kString;
  // Shared, and never freed while the plugin is loaded: a pointer a plugin got
  // from `config_string` must stay valid across a later write of that key.
  std::shared_ptr<const std::string> text;
  long long integer = 0;
  bool boolean = false;
};
using ConfigValues = std::unordered_map<std::string, ConfigValue>;

struct Manifest {
  std::string name;
  std::string version;
  std::string library;
  std::string entry = "pmc_plugin_entry";
  std::string author;
  std::string description;
  uint32_t api = PMC_PLUGIN_MANIFEST_API;
  // Names of plugins that must be enabled before this one. They are ordinary
  // plugins from the same `--plugins` tree, or ones an earlier load call
  // already brought in. Version constraints are not part of the manifest yet.
  std::vector<std::string> depends;
};

struct LoadedPlugin {
  std::string name;
  std::string version;
  std::string dir;
  std::string library;
  LibraryHandle handle = nullptr;
  const pmc_plugin* descriptor = nullptr;
  void* data = nullptr;
  // `<plugin directory>/data`, absolute and created before on_load runs.
  std::string data_dir;
  // `<data_dir>/config.toml`, read before on_load and rewritten by the config
  // writers. Every handler thread may read or write it, so the map is behind
  // `config_mutex`; `retired_config_strings` keeps replaced strings alive until
  // unload so a pointer already handed to the plugin never dangles.
  ConfigValues config;
  std::mutex config_mutex;
  std::vector<std::shared_ptr<const std::string>> retired_config_strings;
  // The manifest's dependency names, kept for the runtime-unload rule: a
  // plugin that another loaded plugin depends on cannot be closed.
  std::vector<std::string> depends;
  bool enabled = false;
  bool disabled = false;
  // Runtime lifecycle (slice 21). `active` counts callbacks currently on a
  // stack, and `retiring` refuses new ones; unload sets it, then waits on
  // `idle_cv` for `active` to reach zero before disabling or closing anything.
  // `state_mutex` is only ever held for the counter, never while a callback
  // runs, so a handler that takes its time delays a reload rather than
  // deadlocking it.
  std::mutex state_mutex;
  std::condition_variable idle_cv;
  int active = 0;
  bool retiring = false;
};

struct Listener {
  uint32_t type = 0;
  pmc_event_fn fn = nullptr;
  void* data = nullptr;
  LoadedPlugin* owner = nullptr;
};

struct Command {
  std::string name;
  std::string usage;
  int op_level = 0;
  pmc_command_fn fn = nullptr;
  void* data = nullptr;
  LoadedPlugin* owner = nullptr;
};

struct Task {
  uint64_t id = 0;
  uint64_t due = 0;
  uint32_t repeat = 0;
  pmc_task_fn fn = nullptr;
  void* data = nullptr;
  LoadedPlugin* owner = nullptr;
  bool cancelled = false;
};

struct AsyncTask {
  uint64_t id = 0;
  pmc_task_fn fn = nullptr;
  void* data = nullptr;
  LoadedPlugin* owner = nullptr;
  bool cancelled = false;
};

std::string trim(std::string s) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

std::string unquote(std::string value) {
  if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                            (value.front() == '\'' && value.back() == '\'')))
    return value.substr(1, value.size() - 2);
  return value;
}

// `["a", "b"]`: the one array shape the manifest subset accepts. Quoted
// names, optional whitespace, no escapes or trailing commas.
bool parse_string_array(const std::string& text, std::vector<std::string>* out,
                        std::string* error) {
  if (text.size() < 2 || text.front() != '[' || text.back() != ']') {
    *error = "expected an array of quoted names";
    return false;
  }
  const std::size_t end = text.size() - 1;
  std::size_t i = 1;
  while (i < end) {
    while (i < end && std::isspace(static_cast<unsigned char>(text[i])))
      ++i;
    if (i == end)
      break;
    if (text[i] != '"' && text[i] != '\'') {
      *error = "expected a quoted dependency name";
      return false;
    }
    const char quote = text[i++];
    const std::size_t close = text.find(quote, i);
    if (close == std::string::npos || close > end) {
      *error = "unterminated dependency name";
      return false;
    }
    std::string name = text.substr(i, close - i);
    if (name.empty()) {
      *error = "empty dependency name";
      return false;
    }
    out->push_back(std::move(name));
    i = close + 1;
    while (i < end && std::isspace(static_cast<unsigned char>(text[i])))
      ++i;
    if (i < end) {
      if (text[i] != ',') {
        *error = "expected ',' between dependency names";
        return false;
      }
      ++i;
    }
  }
  return true;
}

// The manifest is deliberately a tiny TOML subset: `key = "value"`, a
// `["a", "b"]` array for `depends`, `#` comments and no tables. A plugin that
// needs more reads its own config; the manifest is what the *host* has to
// understand.
bool read_manifest(const std::filesystem::path& path, Manifest* out, std::string* error) {
  std::ifstream in(path);
  if (!in) {
    *error = "cannot read " + path.string();
    return false;
  }
  std::string line;
  int number = 0;
  while (std::getline(in, line)) {
    ++number;
    if (const auto hash = line.find('#'); hash != std::string::npos)
      line = line.substr(0, hash);
    line = trim(line);
    if (line.empty())
      continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) {
      *error = path.string() + ":" + std::to_string(number) + ": expected key = value";
      return false;
    }
    const std::string key = trim(line.substr(0, eq));
    const std::string value = unquote(trim(line.substr(eq + 1)));
    if (key == "name")
      out->name = value;
    else if (key == "version")
      out->version = value;
    else if (key == "library")
      out->library = value;
    else if (key == "entry")
      out->entry = value;
    else if (key == "author")
      out->author = value;
    else if (key == "description")
      out->description = value;
    else if (key == "depends") {
      std::string why;
      out->depends.clear();
      if (!parse_string_array(trim(line.substr(eq + 1)), &out->depends, &why)) {
        *error = path.string() + ":" + std::to_string(number) + ": depends: " + why;
        return false;
      }
    } else if (key == "api") {
      char* end = nullptr;
      const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
      if (end == value.c_str() || *end != '\0') {
        *error = path.string() + ":" + std::to_string(number) + ": api must be an integer";
        return false;
      }
      out->api = static_cast<uint32_t>(parsed);
    }
    // Unknown keys are ignored on purpose: a newer manifest can carry keys an
    // older host does not read yet.
  }
  if (out->name.empty() || out->version.empty() || out->library.empty()) {
    *error = path.string() + ": name, version and library are required";
    return false;
  }
  return true;
}

// The plugin config is the same tiny TOML subset the manifest uses, with one
// value per line: a quoted string, an integer or true/false. Missing file means
// an empty config; a malformed line, duplicate key or unreadable file refuses
// the plugin rather than guessing what the operator meant.
bool read_config(const std::filesystem::path& path, ConfigValues* out, std::string* error) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec))
    return true;
  std::ifstream in(path);
  if (!in) {
    *error = "cannot read " + path.string();
    return false;
  }
  std::string line;
  int number = 0;
  while (std::getline(in, line)) {
    ++number;
    if (const auto hash = line.find('#'); hash != std::string::npos)
      line = line.substr(0, hash);
    line = trim(line);
    if (line.empty())
      continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) {
      *error = path.string() + ":" + std::to_string(number) + ": expected key = value";
      return false;
    }
    const std::string key = trim(line.substr(0, eq));
    const std::string value = trim(line.substr(eq + 1));
    if (key.empty()) {
      *error = path.string() + ":" + std::to_string(number) + ": empty key";
      return false;
    }
    if (value.empty()) {
      *error = path.string() + ":" + std::to_string(number) + ": value is empty";
      return false;
    }
    ConfigValue entry;
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\''))) {
      entry.type = ConfigValue::Type::kString;
      entry.text = std::make_shared<const std::string>(value.substr(1, value.size() - 2));
    } else if (value == "true" || value == "false") {
      entry.type = ConfigValue::Type::kBoolean;
      entry.boolean = value == "true";
    } else {
      char* end = nullptr;
      errno = 0;
      const long long integer = std::strtoll(value.c_str(), &end, 10);
      if (end == value.c_str() || *end != '\0' || errno == ERANGE) {
        *error = path.string() + ":" + std::to_string(number) +
                 ": value must be a quoted string, an integer or true/false";
        return false;
      }
      entry.type = ConfigValue::Type::kInteger;
      entry.integer = integer;
    }
    if (!out->emplace(key, std::move(entry)).second) {
      *error = path.string() + ":" + std::to_string(number) + ": duplicate key '" + key + "'";
      return false;
    }
  }
  return true;
}

// A key the host will write: non-empty, at most 64 bytes, and only characters
// that cannot break the flat `key = value` file.
bool config_key_ok(const char* key) {
  if (key == nullptr)
    return false;
  const std::size_t size = std::strlen(key);
  if (size == 0 || size > 64)
    return false;
  for (std::size_t i = 0; i < size; ++i) {
    const unsigned char c = static_cast<unsigned char>(key[i]);
    if (!(std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.'))
      return false;
  }
  return true;
}

std::string config_value_text(const ConfigValue& value) {
  switch (value.type) {
  case ConfigValue::Type::kString:
    return "\"" + (value.text != nullptr ? *value.text : std::string()) + "\"";
  case ConfigValue::Type::kInteger:
    return std::to_string(value.integer);
  case ConfigValue::Type::kBoolean:
    return value.boolean ? "true" : "false";
  }
  return {};
}

// Rewrites the whole flat file, sorted by key so the bytes are deterministic,
// then replaces the old file with one rename. Comments and the operator's
// formatting are not preserved; the values are.
bool write_config(const std::filesystem::path& path, const ConfigValues& values,
                  std::string* error) {
  std::vector<const std::pair<const std::string, ConfigValue>*> entries;
  entries.reserve(values.size());
  for (const auto& entry : values)
    entries.push_back(&entry);
  std::sort(entries.begin(), entries.end(),
            [](const auto* a, const auto* b) { return a->first < b->first; });

  const std::filesystem::path temporary = path.string() + ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) {
      *error = "cannot open " + temporary.string() + " for writing";
      return false;
    }
    out << "# +MC plugin config; rewritten by config_set_* (comments are lost).\n";
    for (const auto* entry : entries) {
      out << entry->first << " = " << config_value_text(entry->second) << "\n";
      if (!out) {
        *error = "cannot write " + temporary.string();
        return false;
      }
    }
    out.flush();
    if (!out) {
      *error = "cannot write " + temporary.string();
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(temporary, path, ec);
  if (ec) {
    // Windows does not replace an existing destination; POSIX does. Retry the
    // way the world's saves do, so one code path works on both.
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    ec.clear();
    std::filesystem::rename(temporary, path, ec);
  }
  if (ec) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    *error = "cannot replace " + path.string() + ": " + ec.message();
    return false;
  }
  return true;
}

std::string library_suffix() {
#if defined(_WIN32)
  return ".dll";
#elif defined(__APPLE__)
  return ".dylib";
#else
  return ".so";
#endif
}

// ---- the dynamic loader -----------------------------------------------------
//
// Unix spells the loader dlopen/dlsym/dlclose and returns a `void*` symbol;
// Windows spells it LoadLibrary/GetProcAddress/FreeLibrary and returns a
// FARPROC. Both are hidden below so `load_one` has one shape everywhere.
//
// LoadLibraryExW is given an absolute path and LOAD_WITH_ALTERED_SEARCH_PATH:
// a plugin that ships a sibling DLL (its own dependency) resolves it next to
// itself instead of depending on the server's working directory, and an
// absolute path keeps the bare-name search order out of the decision.

#if defined(_WIN32)
std::string utf8_from_wide(const wchar_t* text, int length) {
  if (text == nullptr || length <= 0)
    return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
  if (bytes <= 0)
    return {};
  std::string out(static_cast<std::size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), bytes, nullptr, nullptr);
  return out;
}

// FormatMessage text for a Win32 error, without the trailing CRLF. The
// fallback names the number when the system has no message for it, which is
// what a plugin author needs to search for.
std::string windows_error_text(DWORD error) {
  wchar_t buffer[512];
  const DWORD length =
      FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                     buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])), nullptr);
  std::size_t used = length != 0 ? static_cast<std::size_t>(length) : 0;
  while (used > 0 &&
         (buffer[used - 1] == L'\r' || buffer[used - 1] == L'\n' || buffer[used - 1] == L' '))
    --used;
  std::string message = utf8_from_wide(buffer, static_cast<int>(used));
  if (message.empty())
    message = "Windows error " + std::to_string(static_cast<unsigned long>(error));
  return message;
}
#endif

LibraryHandle open_library(const std::filesystem::path& path, std::string* error) {
#if defined(_WIN32)
  std::error_code ec;
  std::filesystem::path absolute = std::filesystem::absolute(path, ec);
  if (ec)
    absolute = path;
  HMODULE module = LoadLibraryExW(absolute.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (module == nullptr) {
    *error = windows_error_text(GetLastError());
    return nullptr;
  }
  return module;
#else
  // dlsym's failure is a thread-local error that is only meaningful when it
  // was cleared first; dlopen clears it too, but saying so here keeps the two
  // lookups' contracts the same.
  dlerror();
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    const char* message = dlerror();
    *error = message != nullptr ? message : "unknown error";
  }
  return handle;
#endif
}

LibrarySymbol library_symbol(LibraryHandle handle, const char* name, std::string* error) {
#if defined(_WIN32)
  FARPROC symbol = GetProcAddress(handle, name);
  if (symbol == nullptr)
    *error = windows_error_text(GetLastError());
  return symbol;
#else
  dlerror();
  void* symbol = dlsym(handle, name);
  if (symbol == nullptr) {
    const char* message = dlerror();
    *error = message != nullptr ? message : "unknown error";
  }
  return symbol;
#endif
}

void close_library(LibraryHandle handle) {
#if defined(_WIN32)
  FreeLibrary(handle);
#else
  dlclose(handle);
#endif
}

// The loader hands back a function-pointer-shaped slot (FARPROC on Windows,
// `void*` on Unix). Copying between the two function-pointer types keeps MSVC's
// conversion checks and clang's -Wpedantic quiet; `symbol_as<EntryFn>` is the
// only place the entry's type is named.
template <typename Fn> Fn symbol_as(LibrarySymbol symbol) {
  static_assert(sizeof(Fn) == sizeof(symbol), "function pointer size mismatch");
  Fn fn = nullptr;
  std::memcpy(&fn, &symbol, sizeof(fn));
  return fn;
}

std::filesystem::path resolve_library(const std::filesystem::path& dir, const std::string& name) {
  const std::filesystem::path exact = dir / name;
  std::error_code ec;
  if (std::filesystem::is_regular_file(exact, ec))
    return exact;
  for (const std::string& candidate : {name + library_suffix(), "lib" + name + library_suffix(),
                                       name + ".so", name + ".dylib", name + ".dll"}) {
    const std::filesystem::path path = dir / candidate;
    if (std::filesystem::is_regular_file(path, ec))
      return path;
  }
  return {};
}

mc::log::Level log_level(int level) {
  switch (level) {
  case PMC_LOG_DEBUG:
    return mc::log::Level::Debug;
  case PMC_LOG_WARN:
    return mc::log::Level::Warn;
  case PMC_LOG_ERROR:
    return mc::log::Level::Error;
  default:
    return mc::log::Level::Info;
  }
}

// The plugin whose callback is running on this thread, for log attribution and
// for the config writers that update its live values.
thread_local LoadedPlugin* tls_current = nullptr;

// Saves and restores `tls_current` across a callback. A handler can make the
// host dispatch a nested event (a command that resolves a death, for example),
// and the nested dispatch must leave the outer plugin's attribution in place
// when it returns — otherwise the rest of the outer handler logs as "[plugin]"
// and `schedule_async` loses its owner.
struct ScopedCurrent {
  LoadedPlugin* previous;
  explicit ScopedCurrent(LoadedPlugin* plugin) : previous(tls_current) {
    tls_current = plugin;
  }
  ~ScopedCurrent() {
    tls_current = previous;
  }
  ScopedCurrent(const ScopedCurrent&) = delete;
  ScopedCurrent& operator=(const ScopedCurrent&) = delete;
};

// Marks one plugin callback as in flight, or refuses to start when that
// plugin is being unloaded. Every dispatch site (event listeners, commands,
// scheduled and async tasks) takes one before calling into a module, so an
// unload can wait for the callbacks already running and skip the ones a
// copied listener list would otherwise start after the module closed.
class CallGuard {
public:
  explicit CallGuard(LoadedPlugin* plugin) : plugin_(plugin) {
    std::lock_guard<std::mutex> lock(plugin_->state_mutex);
    entered_ = !plugin_->retiring;
    if (entered_)
      ++plugin_->active;
  }
  ~CallGuard() {
    if (!entered_)
      return;
    std::lock_guard<std::mutex> lock(plugin_->state_mutex);
    if (--plugin_->active == 0)
      plugin_->idle_cv.notify_all();
  }
  CallGuard(const CallGuard&) = delete;
  CallGuard& operator=(const CallGuard&) = delete;
  bool entered() const {
    return entered_;
  }

private:
  LoadedPlugin* plugin_;
  bool entered_ = false;
};

// Shared implementation of the three setters. The caller is the plugin whose
// callback is running (`tls_current`), exactly like the getters. The write is
// serialized per plugin and updates memory only after the file replacement
// succeeded, so a failed call changes neither.
int set_config_value(const char* key, ConfigValue value) {
  LoadedPlugin* owner = tls_current;
  if (owner == nullptr || !config_key_ok(key))
    return PMC_CONFIG_INVALID;
  std::lock_guard<std::mutex> lock(owner->config_mutex);
  ConfigValue previous;
  bool had_previous = false;
  const auto found = owner->config.find(key);
  if (found != owner->config.end()) {
    previous = found->second;
    had_previous = true;
  }
  owner->config[key] = std::move(value);
  std::string why;
  const std::filesystem::path path = std::filesystem::path(owner->data_dir) / "config.toml";
  if (!write_config(path, owner->config, &why)) {
    if (had_previous)
      owner->config[key] = std::move(previous);
    else
      owner->config.erase(key);
    mc::log::error("plugins: ", owner->name, ": ", why);
    return PMC_CONFIG_IO;
  }
  if (had_previous && previous.text != nullptr)
    owner->retired_config_strings.push_back(std::move(previous.text));
  return PMC_CONFIG_OK;
}

} // namespace

struct Host::Impl {
  pmc_host_api api{};
  mutable std::mutex mutex;
  std::vector<std::unique_ptr<LoadedPlugin>> plugins;
  std::vector<Listener> listeners;
  std::vector<Command> commands;
  std::vector<Task> tasks;
  std::size_t failed = 0;
  std::atomic<uint64_t> now{0};
  std::atomic<uint64_t> next_task_id{1};
  // One worker thread for `schedule_async`, started on first use. The queue is
  // drained before unload so no callback outlives its module; `stop_async`
  // joins it when the host dies. Both condition variables share `async_mutex`.
  std::thread async_worker;
  std::mutex async_mutex;
  std::condition_variable async_cv;
  std::condition_variable async_idle_cv;
  std::deque<AsyncTask> async_tasks;
  bool async_accepting = true;
  bool async_stop = false;
  bool async_running = false;
  // The engine's read-only lookups (Host::set_accessors). Copied out under
  // `mutex` before they are called, so a plugin's query never runs engine code
  // while the host's registries are locked.
  PlayerLookup player_lookup;
  BlockLookup block_lookup;
  // The engine's block write (Host::set_writer), copied out under `mutex` for
  // the same reason as the lookups.
  BlockWriter block_writer;
  // The entity registry and the player inventory (Host::set_entities /
  // set_inventory), copied out under `mutex` for the same reason.
  EntityLookup entity_lookup;
  EntityGetter entity_get;
  EntityFinder entity_find;
  EntityDespawner entity_despawn;
  EntityDamager entity_damage;
  EntityTeleporter entity_teleport;
  InventoryReader inventory_read;
  InventoryWriter inventory_write;
  OpLevelLookup op_level;
  // Slice 16: the world clock and weather, copied out under `mutex` before
  // they are called like every other engine callback.
  WorldTime world_time;
  WorldTimeSetter world_time_setter;
  WeatherReader weather_reader;
  WeatherSetter weather_setter;
  // Slice 17: player messaging, copied out under `mutex` like every other
  // engine callback.
  PlayerMessenger message_player;
  Broadcaster broadcast_message;
  PlayerCounter player_counter;
  // Slice 18: the player-state writes, copied out under `mutex` like every
  // other engine callback.
  PlayerGamemodeSetter player_gamemode;
  PlayerHealthSetter player_health;
  PlayerFoodSetter player_food;
  // Only non-null while a lifecycle callback runs. Atomic because a handler on
  // a session thread may call the host table concurrently with on_enable.
  std::atomic<LoadedPlugin*> current{nullptr};
  bool unloaded = false;

  // Runtime lifecycle (slice 21). Requests are queued by the command path and
  // applied by `process_lifecycle_requests` on the tick thread, where no plugin
  // callback is on the stack. `retired_plugins` keeps a closed plugin's
  // bookkeeping alive: a dispatch that copied a listener or command before the
  // purge still holds that pointer and must be able to read `retiring`.
  enum class LifecycleKind { kReload, kUnload };
  struct LifecycleRequest {
    LifecycleKind kind = LifecycleKind::kReload;
    std::string name;
  };
  std::vector<LifecycleRequest> pending_lifecycle;
  std::vector<std::unique_ptr<LoadedPlugin>> retired_plugins;

  // `manifest` has already been read and ABI-checked by load_directory, which
  // also decided the order dependencies require.
  LoadedPlugin* load_one(const std::filesystem::path& dir, const Manifest& manifest);
  // Drops every listener, command and scheduled task a plugin registered. A
  // plugin that refuses (or throws out of) `on_load` after registering would
  // otherwise leave callbacks pointing into a module that is about to be
  // closed — a crash on the next event, not a refused plugin.
  void purge_owner(LoadedPlugin* owner);
  void purge_owner_locked(LoadedPlugin* owner);
  void enable_one(LoadedPlugin& plugin);
  // on_disable then on_unload, in the plugin's own context. Shared by the
  // shutdown path and one-plugin unload.
  void disable_and_unload(LoadedPlugin& plugin);
  bool request_lifecycle(const std::string& name, bool reload, std::string* error);
  void process_lifecycle_requests();
  bool unload_one(const std::string& name, std::string* error);
  void reload_one(const std::string& name);
  void pump(uint64_t ticks);
  // The async worker loop and its two shutdown phases: `drain_async` waits for
  // the queue and the current callback (used before unloading plugins), while
  // `stop_async` additionally joins the thread (used when the host dies).
  void async_loop();
  void drain_async();
  void stop_async();
  ~Impl();
};

// ---- the C host table -------------------------------------------------------

namespace {

Host::Impl& impl_of(void* host) {
  return *static_cast<Host::Impl*>(host);
}

const char* current_name() {
  return tls_current != nullptr ? tls_current->name.c_str() : "plugin";
}

void api_log(void* host, int level, const char* message) {
  (void)host;
  mc::log::detail::compose(log_level(level), "plugins: [", current_name(), "] ",
                           message != nullptr ? message : "");
}

int api_on(void* host, uint32_t event_type, pmc_event_fn fn, void* data) {
  Host::Impl& impl = impl_of(host);
  if (fn == nullptr) {
    mc::log::warn("plugins: [", current_name(), "] listener registration with no callback");
    return -1;
  }
  switch (event_type) {
  case PMC_EVENT_SERVER_START:
  case PMC_EVENT_SERVER_STOP:
  case PMC_EVENT_TICK:
  case PMC_EVENT_PLAYER_CHAT:
  case PMC_EVENT_PLAYER_JOIN:
  case PMC_EVENT_PLAYER_LEAVE:
  case PMC_EVENT_BLOCK_PLACE:
  case PMC_EVENT_BLOCK_BREAK:
  case PMC_EVENT_ITEM_USE_ON:
  case PMC_EVENT_ENTITY_DEATH:
    break;
  default:
    mc::log::warn("plugins: [", current_name(), "] unknown event type ", event_type);
    return -1;
  }
  LoadedPlugin* const owner = impl.current.load();
  if (owner == nullptr) {
    mc::log::warn("plugins: [", current_name(),
                  "] listeners can only be registered while the plugin is loading");
    return -1;
  }
  std::lock_guard<std::mutex> lock(impl.mutex);
  impl.listeners.push_back(Listener{event_type, fn, data, owner});
  return 0;
}

int api_command_level(void* host, const char* name, const char* usage, int op_level,
                      pmc_command_fn fn, void* data) {
  Host::Impl& impl = impl_of(host);
  if (fn == nullptr || name == nullptr || *name == '\0') {
    mc::log::warn("plugins: [", current_name(), "] command registration with no name or callback");
    return -1;
  }
  std::string command = name;
  if (command.front() == '/')
    command.erase(command.begin());
  for (char& c : command)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (command.empty() || command.find(' ') != std::string::npos) {
    mc::log::warn("plugins: [", current_name(), "] invalid command name '", name, "'");
    return -1;
  }
  if (op_level < 0 || op_level > 4) {
    mc::log::warn("plugins: [", current_name(), "] command '", command,
                  "' needs an operator level between 0 and 4, got ", op_level);
    return -1;
  }
  LoadedPlugin* const owner = impl.current.load();
  if (owner == nullptr) {
    mc::log::warn("plugins: [", current_name(),
                  "] commands can only be registered while the plugin is loading");
    return -1;
  }
  std::lock_guard<std::mutex> lock(impl.mutex);
  for (const Command& existing : impl.commands) {
    if (existing.name == command) {
      mc::log::warn("plugins: [", current_name(), "] command '", command,
                    "' is already registered (by ", existing.owner->name, ")");
      return -1;
    }
  }
  impl.commands.push_back(
      Command{command, usage != nullptr ? usage : "", op_level, fn, data, owner});
  return 0;
}

int api_command(void* host, const char* name, const char* usage, pmc_command_fn fn, void* data) {
  return api_command_level(host, name, usage, 0, fn, data);
}

void api_reply(void* host, pmc_reply* reply, const char* message) {
  (void)host;
  if (reply == nullptr || !reply->send || message == nullptr)
    return;
  const std::string text = message;
  try {
    reply->send(text);
  } catch (...) {
    // The session-side callback is engine code; a throw from it would already
    // be a bug, but answering a plugin must never unwind through C frames.
    mc::log::error("plugins: [", current_name(), "] reply delivery failed");
  }
}

uint64_t api_schedule(void* host, uint32_t delay_ticks, uint32_t repeat_ticks, pmc_task_fn fn,
                      void* data) {
  Host::Impl& impl = impl_of(host);
  LoadedPlugin* const owner = impl.current.load();
  if (fn == nullptr || owner == nullptr) {
    mc::log::warn("plugins: [", current_name(),
                  "] tasks need a callback and must be scheduled from a lifecycle callback");
    return 0;
  }
  std::lock_guard<std::mutex> lock(impl.mutex);
  const uint64_t id = impl.next_task_id.fetch_add(1);
  const uint64_t due = impl.now.load() + (delay_ticks == 0 ? 1 : delay_ticks);
  impl.tasks.push_back(Task{id, due, repeat_ticks, fn, data, owner, false});
  return id;
}

uint64_t api_schedule_async(void* host, pmc_task_fn fn, void* data) {
  Host::Impl& impl = impl_of(host);
  LoadedPlugin* owner = impl.current.load();
  if (owner == nullptr && tls_current != nullptr)
    owner = const_cast<LoadedPlugin*>(tls_current);
  if (fn == nullptr || owner == nullptr) {
    mc::log::warn("plugins: [", current_name(),
                  "] async work needs a callback and a plugin callback context");
    return 0;
  }
  std::lock_guard<std::mutex> lock(impl.async_mutex);
  if (!impl.async_accepting)
    return 0;
  if (!impl.async_worker.joinable())
    impl.async_worker = std::thread([&impl] { impl.async_loop(); });
  const uint64_t id = impl.next_task_id.fetch_add(1);
  impl.async_tasks.push_back(AsyncTask{id, fn, data, owner, false});
  impl.async_cv.notify_one();
  return id;
}

int api_cancel(void* host, uint64_t task_id) {
  Host::Impl& impl = impl_of(host);
  {
    std::lock_guard<std::mutex> lock(impl.mutex);
    for (Task& task : impl.tasks) {
      if (task.id == task_id) {
        task.cancelled = true;
        return 0;
      }
    }
  }
  // A queued async task can be dropped; one already running is no longer in
  // the queue and cannot be interrupted.
  std::lock_guard<std::mutex> lock(impl.async_mutex);
  for (AsyncTask& task : impl.async_tasks) {
    if (task.id == task_id) {
      task.cancelled = true;
      return 0;
    }
  }
  return -1;
}

int api_player_op_level(void* host, const char* name) {
  if (host == nullptr || name == nullptr)
    return -1;
  OpLevelLookup lookup;
  {
    Host::Impl& impl = impl_of(host);
    std::lock_guard<std::mutex> lock(impl.mutex);
    lookup = impl.op_level;
  }
  if (!lookup)
    return -1;
  try {
    return lookup(name);
  } catch (...) {
    mc::log::error("plugins: [", current_name(), "] operator lookup threw");
    return -1;
  }
}

// ---- world clock and weather (slice 16) ------------------------------------

int64_t api_world_time(void* host) {
  WorldTime read;
  {
    Host::Impl& impl = impl_of(host);
    std::lock_guard<std::mutex> lock(impl.mutex);
    read = impl.world_time;
  }
  if (!read)
    return -1;
  try {
    return read();
  } catch (...) {
    mc::log::error("plugins: [", current_name(), "] world clock read threw");
    return -1;
  }
}

int api_world_set_time(void* host, int64_t ticks) {
  if (ticks < 0)
    return PMC_WORLD_INVALID;
  WorldTimeSetter set;
  {
    Host::Impl& impl = impl_of(host);
    std::lock_guard<std::mutex> lock(impl.mutex);
    set = impl.world_time_setter;
  }
  if (!set)
    return PMC_WORLD_NO_WORLD;
  try {
    return set(ticks);
  } catch (...) {
    mc::log::error("plugins: [", current_name(), "] world clock write threw");
    return PMC_WORLD_NO_WORLD;
  }
}

int api_world_weather(void* host) {
  WeatherReader read;
  {
    Host::Impl& impl = impl_of(host);
    std::lock_guard<std::mutex> lock(impl.mutex);
    read = impl.weather_reader;
  }
  if (!read)
    return PMC_WORLD_NO_WORLD;
  try {
    return read();
  } catch (...) {
    mc::log::error("plugins: [", current_name(), "] weather read threw");
    return PMC_WORLD_NO_WORLD;
  }
}

int api_world_set_weather(void* host, int weather, int32_t duration_ticks) {
  if (weather < PMC_WEATHER_CLEAR || weather > PMC_WEATHER_THUNDER)
    return PMC_WORLD_INVALID;
  WeatherSetter set;
  {
    Host::Impl& impl = impl_of(host);
    std::lock_guard<std::mutex> lock(impl.mutex);
    set = impl.weather_setter;
  }
  if (!set)
    return PMC_WORLD_NO_WORLD;
  try {
    return set(weather, duration_ticks);
  } catch (...) {
    mc::log::error("plugins: [", current_name(), "] weather write threw");
    return PMC_WORLD_NO_WORLD;
  }
}

// ---- player messaging (slice 17) -------------------------------------------

// `plugin.h` promises 1..4096 UTF-8 bytes; a longer line is refused rather
// than cut, because a truncated sentence is worse than an error the plugin can
// report.
constexpr std::size_t kMaxPluginMessageBytes = 4096;

bool message_line_ok(const char* message) {
  return message != nullptr && message[0] != '\0' &&
         std::string_view(message).size() <= kMaxPluginMessageBytes;
}

int api_player_message(void* host, const char* player, const char* message) {
  if (host == nullptr || player == nullptr || player[0] == '\0' || !message_line_ok(message))
    return PMC_MESSAGE_INVALID;
  PlayerMessenger send;
  {
    Host::Impl& impl = impl_of(host);
    std::lock_guard<std::mutex> lock(impl.mutex);
    send = impl.message_player;
  }
  if (!send)
    return PMC_MESSAGE_NO_PLAYER;
  try {
    return send(player, message);
  } catch (...) {
    mc::log::error("plugins: [", current_name(), "] player message threw");
    return PMC_MESSAGE_NO_PLAYER;
  }
}

int api_broadcast(void* host, const char* message) {
  if (host == nullptr || !message_line_ok(message))
    return PMC_MESSAGE_INVALID;
  Broadcaster send;
  {
    Host::Impl& impl = impl_of(host);
    std::lock_guard<std::mutex> lock(impl.mutex);
    send = impl.broadcast_message;
  }
  if (!send)
    return 0;
  try {
    return send(message);
  } catch (...) {
    mc::log::error("plugins: [", current_name(), "] broadcast threw");
    return 0;
  }
}

int api_player_count(void* host) {
  if (host == nullptr)
    return -1;
  PlayerCounter count;
  {
    Host::Impl& impl = impl_of(host);
    std::lock_guard<std::mutex> lock(impl.mutex);
    count = impl.player_counter;
  }
  if (!count)
    return -1;
  try {
    return count();
  } catch (...) {
    mc::log::error("plugins: [", current_name(), "] player count threw");
    return -1;
  }
}

// ---- player state writes (slice 18) ---------------------------------------

// `plugin.h` promises 20 is the base maximum health; a value above it is
// refused rather than silently clamped, so the plugin can tell the write did
// not happen. The engine clamps again to the target's own attribute maximum.
constexpr float kMaxPluginHealth = 20.0f;

int api_player_set_gamemode(void* host, const char* player, int gamemode) {
  if (host == nullptr || player == nullptr || player[0] == '\0' || gamemode < 0 || gamemode > 3)
    return PMC_PLAYER_INVALID;
  PlayerGamemodeSetter set;
  {
    Host::Impl& impl = impl_of(host);
    std::lock_guard<std::mutex> lock(impl.mutex);
    set = impl.player_gamemode;
  }
  if (!set)
    return PMC_PLAYER_UNSUPPORTED;
  try {
    return set(player, gamemode);
  } catch (...) {
    mc::log::error("plugins: [", current_name(), "] game mode write threw");
    return PMC_PLAYER_REFUSED;
  }
}

int api_player_set_health(void* host, const char* player, float health) {
  if (host == nullptr || player == nullptr || player[0] == '\0' || !std::isfinite(health) ||
      health <= 0.0f || health > kMaxPluginHealth)
    return PMC_PLAYER_INVALID;
  PlayerHealthSetter set;
  {
    Host::Impl& impl = impl_of(host);
    std::lock_guard<std::mutex> lock(impl.mutex);
    set = impl.player_health;
  }
  if (!set)
    return PMC_PLAYER_UNSUPPORTED;
  try {
    return set(player, health);
  } catch (...) {
    mc::log::error("plugins: [", current_name(), "] health write threw");
    return PMC_PLAYER_REFUSED;
  }
}

int api_player_set_food(void* host, const char* player, int food, float saturation) {
  if (host == nullptr || player == nullptr || player[0] == '\0' || food < 0 || food > 20 ||
      !std::isfinite(saturation) || saturation < 0.0f)
    return PMC_PLAYER_INVALID;
  PlayerFoodSetter set;
  {
    Host::Impl& impl = impl_of(host);
    std::lock_guard<std::mutex> lock(impl.mutex);
    set = impl.player_food;
  }
  if (!set)
    return PMC_PLAYER_UNSUPPORTED;
  try {
    return set(player, food, saturation);
  } catch (...) {
    mc::log::error("plugins: [", current_name(), "] food write threw");
    return PMC_PLAYER_REFUSED;
  }
}

uint64_t api_tick_count(void* host) {
  return impl_of(host).now.load();
}

const char* api_server_version(void* host) {
  (void)host;
  static const std::string version =
      std::string("plusmc ") + PLUSMC_VERSION_STRING + " (protocol 777)";
  return version.c_str();
}

// The per-plugin directory. `tls_current` is the plugin whose callback is on
// this thread, so the answer follows the caller without a lookup by name.
const char* api_data_dir(void* host) {
  (void)host;
  if (tls_current == nullptr || tls_current->data_dir.empty())
    return nullptr;
  return tls_current->data_dir.c_str();
}

const char* api_config_string(void* host, const char* key) {
  (void)host;
  if (tls_current == nullptr || key == nullptr)
    return nullptr;
  std::lock_guard<std::mutex> lock(tls_current->config_mutex);
  const auto found = tls_current->config.find(key);
  if (found == tls_current->config.end() || found->second.type != ConfigValue::Type::kString ||
      found->second.text == nullptr)
    return nullptr;
  return found->second.text->c_str();
}

int64_t api_config_int(void* host, const char* key, int64_t fallback) {
  (void)host;
  if (tls_current == nullptr || key == nullptr)
    return fallback;
  std::lock_guard<std::mutex> lock(tls_current->config_mutex);
  const auto found = tls_current->config.find(key);
  if (found == tls_current->config.end() || found->second.type != ConfigValue::Type::kInteger)
    return fallback;
  return found->second.integer;
}

int api_config_bool(void* host, const char* key, int fallback) {
  (void)host;
  if (tls_current == nullptr || key == nullptr)
    return fallback;
  std::lock_guard<std::mutex> lock(tls_current->config_mutex);
  const auto found = tls_current->config.find(key);
  if (found == tls_current->config.end() || found->second.type != ConfigValue::Type::kBoolean)
    return fallback;
  return found->second.boolean ? 1 : 0;
}

int api_config_set_string(void* host, const char* key, const char* value) {
  (void)host;
  if (value == nullptr)
    return PMC_CONFIG_INVALID;
  std::string text(value);
  if (text.find_first_of("\"\n\r") != std::string::npos)
    return PMC_CONFIG_INVALID;
  ConfigValue entry;
  entry.type = ConfigValue::Type::kString;
  entry.text = std::make_shared<const std::string>(std::move(text));
  return set_config_value(key, std::move(entry));
}

int api_config_set_int(void* host, const char* key, int64_t value) {
  (void)host;
  ConfigValue entry;
  entry.type = ConfigValue::Type::kInteger;
  entry.integer = static_cast<long long>(value);
  return set_config_value(key, std::move(entry));
}

int api_config_set_bool(void* host, const char* key, int value) {
  (void)host;
  if (value != 0 && value != 1)
    return PMC_CONFIG_INVALID;
  ConfigValue entry;
  entry.type = ConfigValue::Type::kBoolean;
  entry.boolean = value != 0;
  return set_config_value(key, std::move(entry));
}

int api_player_get(void* host, const char* name, pmc_player* out) {
  // The original struct's size (through `entity_id`, trailing padding
  // included) is the floor: a plugin built against the header before velocity
  // was appended has that struct, and the copy below never writes past what
  // the caller declared.
  if (host == nullptr || name == nullptr || out == nullptr ||
      out->struct_size < offsetof(pmc_player, vx))
    return -1;
  PlayerLookup lookup;
  {
    Host::Impl& impl = impl_of(host);
    std::lock_guard<std::mutex> lock(impl.mutex);
    lookup = impl.player_lookup;
  }
  if (!lookup)
    return -1;
  PlayerView view;
  try {
    if (!lookup(name, &view))
      return -1;
  } catch (...) {
    mc::log::error("plugins: [", current_name(), "] player lookup threw");
    return -1;
  }
  pmc_player player{};
  std::strncpy(player.name, view.name.c_str(), sizeof(player.name) - 1);
  player.x = view.x;
  player.y = view.y;
  player.z = view.z;
  player.yaw = view.yaw;
  player.pitch = view.pitch;
  player.gamemode = view.gamemode;
  player.health = view.health;
  player.entity_id = view.entity_id;
  player.vx = view.vx;
  player.vy = view.vy;
  player.vz = view.vz;
  // Report the bytes this host actually filled, not the caller's larger
  // declaration: an older caller gets its original prefix, and a caller built
  // against a newer header can see that this host stopped at its own sizeof.
  const std::size_t filled = std::min<std::size_t>(out->struct_size, sizeof(player));
  player.struct_size = static_cast<uint32_t>(filled);
  std::memcpy(out, &player, filled);
  return 0;
}

// The shared half of `world_block` / `world_biome`: one engine lookup, one
// bounded copy. `want_biome` selects which of the two strings is delivered;
// the lookup still fills both because it is one world read.
int world_string(void* host, int32_t x, int32_t y, int32_t z, char* out, uint32_t capacity,
                 bool want_biome) {
  if (host == nullptr || out == nullptr)
    return -2;
  BlockLookup lookup;
  {
    Host::Impl& impl = impl_of(host);
    std::lock_guard<std::mutex> lock(impl.mutex);
    lookup = impl.block_lookup;
  }
  if (!lookup)
    return -1;
  std::string state;
  std::string biome;
  try {
    if (!lookup(x, y, z, &state, &biome))
      return -1;
  } catch (...) {
    mc::log::error("plugins: [", current_name(), "] world lookup threw");
    return -1;
  }
  const std::string& text = want_biome ? biome : state;
  if (capacity < text.size() + 1)
    return -2;
  std::memcpy(out, text.c_str(), text.size() + 1);
  return 0;
}

int api_world_block(void* host, int32_t x, int32_t y, int32_t z, char* out, uint32_t capacity) {
  return world_string(host, x, y, z, out, capacity, false);
}

int api_world_biome(void* host, int32_t x, int32_t y, int32_t z, char* out, uint32_t capacity) {
  return world_string(host, x, y, z, out, capacity, true);
}

int api_world_set_block(void* host, int32_t x, int32_t y, int32_t z, const char* state) {
  if (host == nullptr || state == nullptr)
    return PMC_BLOCK_UNKNOWN_STATE;
  BlockWriter writer;
  {
    Host::Impl& impl = impl_of(host);
    std::lock_guard<std::mutex> lock(impl.mutex);
    writer = impl.block_writer;
  }
  if (!writer)
    return PMC_BLOCK_NO_WORLD;
  try {
    return writer(x, y, z, state);
  } catch (...) {
    mc::log::error("plugins: [", current_name(), "] block write threw");
    return PMC_BLOCK_WRITE_FAILED;
  }
}

// Bounded copy into one of the ABI's fixed-size string fields. `strncpy`
// alone does not guarantee the terminator when the source is long, and a
// plugin walking the struct assumes there is one.
void copy_text(char* out, size_t capacity, const std::string& text) {
  if (out == nullptr || capacity == 0)
    return;
  const size_t n = std::min(text.size(), capacity - 1);
  std::memcpy(out, text.data(), n);
  out[n] = '\0';
}

// One engine snapshot in the ABI's fixed layout. Shared by the listing and the
// single-entity read so both report an entity the same way.
pmc_entity to_abi_entity(const EntityView& view) {
  pmc_entity entity{};
  entity.struct_size = sizeof(pmc_entity);
  entity.entity_id = view.entity_id;
  entity.type_id = view.type_id;
  copy_text(entity.type, sizeof(entity.type), view.type);
  copy_text(entity.name, sizeof(entity.name), view.name);
  entity.x = view.x;
  entity.y = view.y;
  entity.z = view.z;
  entity.health = view.health;
  entity.age = view.age;
  entity.vx = view.vx;
  entity.vy = view.vy;
  entity.vz = view.vz;
  entity.yaw = view.yaw;
  entity.pitch = view.pitch;
  return entity;
}

int api_entities(void* host, pmc_entity_fn fn, void* data) {
  if (host == nullptr || fn == nullptr)
    return -1;
  EntityLookup lookup;
  {
    Host::Impl& impl = impl_of(host);
    std::lock_guard<std::mutex> lock(impl.mutex);
    lookup = impl.entity_lookup;
  }
  if (!lookup)
    return -1;
  std::vector<EntityView> views;
  try {
    if (!lookup(&views))
      return -1;
  } catch (...) {
    mc::log::error("plugins: [", current_name(), "] entity lookup threw");
    return -1;
  }
  int visited = 0;
  for (const EntityView& view : views) {
    pmc_entity entity = to_abi_entity(view);
    try {
      fn(data, &entity);
    } catch (...) {
      // Contained like every other callback. The visit stops rather than
      // calling a callback that just threw once per remaining entity.
      mc::log::error("plugins: [", current_name(), "] entity visitor threw");
      break;
    }
    ++visited;
  }
  return visited;
}

int api_entity_get(void* host, int32_t entity_id, pmc_entity* out) {
  // The original fields through `age` are the floor: a plugin built against the
  // header before velocity and facing were appended has that struct, and the
  // copy below never writes past what the caller declared.
  if (host == nullptr || entity_id <= 0 || out == nullptr ||
      out->struct_size < offsetof(pmc_entity, vx))
    return PMC_ENTITY_INVALID;
  EntityGetter getter;
  {
    Host::Impl& impl = impl_of(host);
    std::lock_guard<std::mutex> lock(impl.mutex);
    getter = impl.entity_get;
  }
  if (!getter)
    return PMC_ENTITY_UNSUPPORTED;
  EntityView view;
  try {
    const int rc = getter(entity_id, &view);
    if (rc != PMC_ENTITY_OK)
      return rc;
  } catch (...) {
    mc::log::error("plugins: [", current_name(), "] entity lookup threw");
    return PMC_ENTITY_NOT_FOUND;
  }
  pmc_entity entity = to_abi_entity(view);
  // Report the bytes this host actually filled, not the caller's larger
  // declaration: an older caller gets its original prefix, and a caller built
  // against a newer header can see that this host stopped at its own sizeof.
  const std::size_t filled = std::min<std::size_t>(out->struct_size, sizeof(entity));
  entity.struct_size = static_cast<uint32_t>(filled);
  std::memcpy(out, &entity, filled);
  return PMC_ENTITY_OK;
}

int api_entity_find(void* host, const char* name, pmc_entity* out) {
  // Same floor as `entity_get`: the original fields through `age` are what a
  // plugin built against the pre-velocity header declares.
  if (host == nullptr || name == nullptr || name[0] == '\0' || out == nullptr ||
      out->struct_size < offsetof(pmc_entity, vx))
    return PMC_ENTITY_INVALID;
  EntityFinder finder;
  {
    Host::Impl& impl = impl_of(host);
    std::lock_guard<std::mutex> lock(impl.mutex);
    finder = impl.entity_find;
  }
  if (!finder)
    return PMC_ENTITY_UNSUPPORTED;
  EntityView view;
  try {
    const int rc = finder(name, &view);
    if (rc != PMC_ENTITY_OK)
      return rc;
  } catch (...) {
    mc::log::error("plugins: [", current_name(), "] entity find threw");
    return PMC_ENTITY_NOT_FOUND;
  }
  pmc_entity entity = to_abi_entity(view);
  const std::size_t filled = std::min<std::size_t>(out->struct_size, sizeof(entity));
  entity.struct_size = static_cast<uint32_t>(filled);
  std::memcpy(out, &entity, filled);
  return PMC_ENTITY_OK;
}

int api_entity_despawn(void* host, int32_t entity_id) {
  if (host == nullptr || entity_id <= 0)
    return PMC_ENTITY_INVALID;
  EntityDespawner write;
  {
    Host::Impl& impl = impl_of(host);
    std::lock_guard<std::mutex> lock(impl.mutex);
    write = impl.entity_despawn;
  }
  if (!write)
    return PMC_ENTITY_UNSUPPORTED;
  try {
    return write(entity_id);
  } catch (...) {
    mc::log::error("plugins: [", current_name(), "] entity despawn threw");
    return PMC_ENTITY_REFUSED;
  }
}

int api_entity_damage(void* host, int32_t entity_id, float amount) {
  if (host == nullptr || entity_id <= 0 || !std::isfinite(amount) || amount <= 0.0f)
    return PMC_ENTITY_INVALID;
  EntityDamager write;
  {
    Host::Impl& impl = impl_of(host);
    std::lock_guard<std::mutex> lock(impl.mutex);
    write = impl.entity_damage;
  }
  if (!write)
    return PMC_ENTITY_UNSUPPORTED;
  try {
    return write(entity_id, amount);
  } catch (...) {
    mc::log::error("plugins: [", current_name(), "] entity damage threw");
    return PMC_ENTITY_REFUSED;
  }
}

int api_entity_teleport(void* host, int32_t entity_id, double x, double y, double z) {
  if (host == nullptr || entity_id <= 0 || !std::isfinite(x) || !std::isfinite(y) ||
      !std::isfinite(z))
    return PMC_ENTITY_INVALID;
  EntityTeleporter write;
  {
    Host::Impl& impl = impl_of(host);
    std::lock_guard<std::mutex> lock(impl.mutex);
    write = impl.entity_teleport;
  }
  if (!write)
    return PMC_ENTITY_UNSUPPORTED;
  try {
    return write(entity_id, x, y, z);
  } catch (...) {
    mc::log::error("plugins: [", current_name(), "] entity teleport threw");
    return PMC_ENTITY_REFUSED;
  }
}

int api_inventory_get(void* host, const char* player, int32_t slot, pmc_item* out) {
  if (host == nullptr || out == nullptr || out->struct_size < sizeof(pmc_item))
    return PMC_INVENTORY_OUT_OF_RANGE;
  if (player == nullptr)
    return PMC_INVENTORY_NO_PLAYER;
  InventoryReader read;
  {
    Host::Impl& impl = impl_of(host);
    std::lock_guard<std::mutex> lock(impl.mutex);
    read = impl.inventory_read;
  }
  if (!read)
    return PMC_INVENTORY_NO_PLAYER;
  ItemView view;
  int result = PMC_INVENTORY_NO_PLAYER;
  try {
    result = read(player, slot, &view);
  } catch (...) {
    mc::log::error("plugins: [", current_name(), "] inventory read threw");
    return PMC_INVENTORY_NO_PLAYER;
  }
  if (result != PMC_INVENTORY_OK)
    return result;
  out->item = view.item;
  out->count = view.count;
  out->damage = view.damage;
  out->max_stack_size = view.max_stack_size;
  copy_text(out->name, sizeof(out->name), view.name);
  return PMC_INVENTORY_OK;
}

int api_inventory_set(void* host, const char* player, int32_t slot, const char* item,
                      int32_t count) {
  if (host == nullptr || player == nullptr)
    return PMC_INVENTORY_NO_PLAYER;
  InventoryWriter write;
  {
    Host::Impl& impl = impl_of(host);
    std::lock_guard<std::mutex> lock(impl.mutex);
    write = impl.inventory_write;
  }
  if (!write)
    return PMC_INVENTORY_NO_PLAYER;
  try {
    // A null item is only meaningful for a clear (count 0); the engine rejects
    // it for a real stack the same way it rejects an unknown name.
    return write(player, slot, item != nullptr ? item : "", count);
  } catch (...) {
    mc::log::error("plugins: [", current_name(), "] inventory write threw");
    return PMC_INVENTORY_WRITE_FAILED;
  }
}

} // namespace

// ---- loading ----------------------------------------------------------------

LoadedPlugin* Host::Impl::load_one(const std::filesystem::path& dir, const Manifest& manifest) {
  const std::filesystem::path library = resolve_library(dir, manifest.library);
  if (library.empty()) {
    mc::log::error("plugins: ", manifest.name, ": cannot find library '", manifest.library, "' in ",
                   dir.string());
    ++failed;
    return nullptr;
  }
  std::string why_load;
  LibraryHandle handle = open_library(library, &why_load);
  if (handle == nullptr) {
    mc::log::error("plugins: ", manifest.name, ": load failed: ", why_load);
    ++failed;
    return nullptr;
  }
  using EntryFn = const pmc_plugin* (*)();
  std::string why_symbol;
  const LibrarySymbol raw = library_symbol(handle, manifest.entry.c_str(), &why_symbol);
  const EntryFn entry = raw != nullptr ? symbol_as<EntryFn>(raw) : nullptr;
  if (entry == nullptr) {
    mc::log::error("plugins: ", manifest.name, ": no '", manifest.entry, "' symbol in ",
                   library.string(), ": ", why_symbol);
    close_library(handle);
    ++failed;
    return nullptr;
  }
  const pmc_plugin* descriptor = nullptr;
  try {
    descriptor = entry();
  } catch (...) {
    mc::log::error("plugins: ", manifest.name, ": ", manifest.entry, " threw");
    close_library(handle);
    ++failed;
    return nullptr;
  }
  if (descriptor == nullptr) {
    mc::log::error("plugins: ", manifest.name, ": ", manifest.entry, " returned null");
    close_library(handle);
    ++failed;
    return nullptr;
  }
  if (descriptor->abi_version != PMC_PLUGIN_ABI_VERSION) {
    mc::log::error("plugins: ", manifest.name, ": descriptor says ABI ", descriptor->abi_version,
                   " but this engine is ABI ", PMC_PLUGIN_ABI_VERSION);
    close_library(handle);
    ++failed;
    return nullptr;
  }
  if (descriptor->struct_size < offsetof(pmc_plugin, on_unload)) {
    mc::log::error("plugins: ", manifest.name, ": descriptor struct_size ", descriptor->struct_size,
                   " is smaller than ABI 1's layout");
    close_library(handle);
    ++failed;
    return nullptr;
  }
  if (descriptor->name == nullptr || manifest.name != descriptor->name) {
    mc::log::error("plugins: ", manifest.name, ": descriptor name '",
                   descriptor->name != nullptr ? descriptor->name : "(null)",
                   "' does not match the manifest");
    close_library(handle);
    ++failed;
    return nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& existing : plugins) {
      if (existing->name == descriptor->name) {
        mc::log::error("plugins: ", manifest.name, ": already loaded from ", existing->dir);
        close_library(handle);
        ++failed;
        return nullptr;
      }
    }
  }

  auto plugin = std::make_unique<LoadedPlugin>();
  plugin->name = manifest.name;
  plugin->version = manifest.version;
  plugin->dir = dir.string();
  plugin->library = library.string();
  plugin->handle = handle;
  plugin->descriptor = descriptor;
  plugin->depends = manifest.depends;

  // The ABI's `data_dir` promise: a real directory, absolute, before on_load
  // can ask for it. A plugin that cannot get one is refused rather than handed
  // a path it cannot use.
  std::error_code abs_ec;
  const std::filesystem::path absolute_data = std::filesystem::absolute(dir / "data", abs_ec);
  plugin->data_dir = (abs_ec ? dir / "data" : absolute_data).string();
  std::error_code data_ec;
  std::filesystem::create_directories(plugin->data_dir, data_ec);
  if (data_ec) {
    mc::log::error("plugins: ", plugin->name, ": cannot create data directory ", plugin->data_dir,
                   ": ", data_ec.message());
    close_library(handle);
    ++failed;
    return nullptr;
  }

  std::string config_why;
  if (!read_config(std::filesystem::path(plugin->data_dir) / "config.toml", &plugin->config,
                   &config_why)) {
    mc::log::error("plugins: ", plugin->name, ": ", config_why);
    close_library(handle);
    ++failed;
    return nullptr;
  }

  if (descriptor->on_load != nullptr) {
    current.store(plugin.get());
    int rc = 0;
    {
      ScopedCurrent scope(plugin.get());
      try {
        rc = descriptor->on_load(&api, &plugin->data);
      } catch (const std::exception& e) {
        mc::log::error("plugins: ", plugin->name, ": on_load threw: ", e.what());
        rc = 1;
      } catch (...) {
        mc::log::error("plugins: ", plugin->name, ": on_load threw");
        rc = 1;
      }
    }
    current.store(nullptr);
    if (rc != 0) {
      mc::log::error("plugins: ", plugin->name, ": on_load refused the plugin (", rc, ")");
      // on_load may have registered listeners, commands or tasks before it
      // refused (or threw). They must be gone before the module is closed, or
      // the next event would call into unmapped code.
      purge_owner(plugin.get());
      close_library(handle);
      ++failed;
      return nullptr;
    }
  }

  std::size_t listeners = 0;
  std::size_t commands = 0;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (const Listener& listener : this->listeners)
      if (listener.owner == plugin.get())
        ++listeners;
    for (const Command& command : this->commands)
      if (command.owner == plugin.get())
        ++commands;
    plugins.push_back(std::move(plugin));
  }
  const LoadedPlugin& loaded = *plugins.back();
  mc::log::info("plugins: loaded ", loaded.name, " ", loaded.version, " (", commands, " command",
                commands == 1 ? "" : "s", ", ", listeners, " listener", listeners == 1 ? "" : "s",
                ") from ", loaded.library);
  return plugins.back().get();
}

void Host::Impl::enable_one(LoadedPlugin& plugin) {
  if (plugin.enabled || plugin.descriptor == nullptr || plugin.descriptor->on_enable == nullptr)
    return;
  current.store(&plugin);
  {
    ScopedCurrent scope(&plugin);
    try {
      plugin.descriptor->on_enable(plugin.data);
    } catch (const std::exception& e) {
      mc::log::error("plugins: ", plugin.name, ": on_enable threw: ", e.what());
    } catch (...) {
      mc::log::error("plugins: ", plugin.name, ": on_enable threw");
    }
  }
  current.store(nullptr);
  plugin.enabled = true;
  mc::log::info("plugins: enabled ", plugin.name);
}

void Host::Impl::pump(uint64_t ticks) {
  const uint64_t at = now.fetch_add(ticks) + ticks;
  struct Due {
    pmc_task_fn fn;
    void* data;
    LoadedPlugin* owner;
  };
  std::vector<Due> due;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (Task& task : tasks) {
      if (task.cancelled || task.due > at)
        continue;
      due.push_back(Due{task.fn, task.data, task.owner});
      if (task.repeat > 0) {
        // A task more than one interval overdue runs once and reschedules from
        // the moment it actually ran, rather than firing on every pump until
        // it catches up (the documented scheduler behavior).
        const uint64_t next = task.due + task.repeat;
        task.due = next <= at ? at + task.repeat : next;
      } else {
        task.cancelled = true; // one-shot: removed below, after it runs
      }
    }
  }
  for (const Due& run : due) {
    CallGuard guard(run.owner);
    if (!guard.entered())
      continue;
    ScopedCurrent scope(run.owner);
    try {
      run.fn(run.data);
    } catch (const std::exception& e) {
      mc::log::error("plugins: [", current_name(), "] task threw: ", e.what());
    } catch (...) {
      mc::log::error("plugins: [", current_name(), "] task threw");
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex);
    tasks.erase(
        std::remove_if(tasks.begin(), tasks.end(), [](const Task& task) { return task.cancelled; }),
        tasks.end());
  }
}

void Host::Impl::async_loop() {
  for (;;) {
    AsyncTask task;
    {
      std::unique_lock<std::mutex> lock(async_mutex);
      async_cv.wait(lock, [&] { return async_stop || !async_tasks.empty(); });
      if (async_tasks.empty()) {
        if (async_stop)
          return;
        continue;
      }
      task = std::move(async_tasks.front());
      async_tasks.pop_front();
      if (task.cancelled) {
        if (async_tasks.empty() && !async_running)
          async_idle_cv.notify_all();
        continue;
      }
      async_running = true;
    }
    {
      CallGuard guard(task.owner);
      if (!guard.entered())
        continue;
      ScopedCurrent scope(task.owner);
      try {
        task.fn(task.data);
      } catch (const std::exception& e) {
        mc::log::error("plugins: [", current_name(), "] async task threw: ", e.what());
      } catch (...) {
        mc::log::error("plugins: [", current_name(), "] async task threw");
      }
    }
    {
      std::lock_guard<std::mutex> lock(async_mutex);
      async_running = false;
      if (async_tasks.empty())
        async_idle_cv.notify_all();
    }
  }
}

void Host::Impl::drain_async() {
  {
    std::lock_guard<std::mutex> lock(async_mutex);
    async_accepting = false;
  }
  std::unique_lock<std::mutex> lock(async_mutex);
  async_idle_cv.wait(lock, [&] { return async_tasks.empty() && !async_running; });
  async_accepting = true;
}

void Host::Impl::stop_async() {
  if (!async_worker.joinable())
    return;
  {
    std::lock_guard<std::mutex> lock(async_mutex);
    async_accepting = false;
  }
  {
    std::unique_lock<std::mutex> lock(async_mutex);
    async_idle_cv.wait(lock, [&] { return async_tasks.empty() && !async_running; });
    async_stop = true;
  }
  async_cv.notify_all();
  async_worker.join();
  async_stop = false;
  async_accepting = true;
}

Host::Impl::~Impl() {
  stop_async();
}

// ---- the Host surface -------------------------------------------------------

Host::Host() : impl_(new Impl()) {
  impl_->api.struct_size = sizeof(pmc_host_api);
  impl_->api.abi_version = PMC_PLUGIN_ABI_VERSION;
  impl_->api.host = impl_.get();
  impl_->api.log = &api_log;
  impl_->api.on = &api_on;
  impl_->api.command = &api_command;
  impl_->api.reply = &api_reply;
  impl_->api.schedule = &api_schedule;
  impl_->api.cancel = &api_cancel;
  impl_->api.tick_count = &api_tick_count;
  impl_->api.server_version = &api_server_version;
  impl_->api.player_get = &api_player_get;
  impl_->api.world_block = &api_world_block;
  impl_->api.world_biome = &api_world_biome;
  impl_->api.world_set_block = &api_world_set_block;
  impl_->api.entities = &api_entities;
  impl_->api.inventory_get = &api_inventory_get;
  impl_->api.inventory_set = &api_inventory_set;
  impl_->api.entity_despawn = &api_entity_despawn;
  impl_->api.entity_damage = &api_entity_damage;
  impl_->api.entity_teleport = &api_entity_teleport;
  impl_->api.entity_get = &api_entity_get;
  impl_->api.entity_find = &api_entity_find;
  impl_->api.data_dir = &api_data_dir;
  impl_->api.config_string = &api_config_string;
  impl_->api.config_int = &api_config_int;
  impl_->api.config_bool = &api_config_bool;
  impl_->api.config_set_string = &api_config_set_string;
  impl_->api.config_set_int = &api_config_set_int;
  impl_->api.config_set_bool = &api_config_set_bool;
  impl_->api.schedule_async = &api_schedule_async;
  impl_->api.command_level = &api_command_level;
  impl_->api.player_op_level = &api_player_op_level;
  impl_->api.world_time = &api_world_time;
  impl_->api.world_set_time = &api_world_set_time;
  impl_->api.world_weather = &api_world_weather;
  impl_->api.world_set_weather = &api_world_set_weather;
  impl_->api.player_message = &api_player_message;
  impl_->api.broadcast = &api_broadcast;
  impl_->api.player_count = &api_player_count;
  impl_->api.player_set_gamemode = &api_player_set_gamemode;
  impl_->api.player_set_health = &api_player_set_health;
  impl_->api.player_set_food = &api_player_set_food;
}

void Host::set_accessors(PlayerLookup player, BlockLookup block) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->player_lookup = std::move(player);
  impl_->block_lookup = std::move(block);
}

void Host::set_writer(BlockWriter block) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->block_writer = std::move(block);
}

void Host::set_entities(EntityLookup entities) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->entity_lookup = std::move(entities);
}

void Host::set_entity_getter(EntityGetter getter) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->entity_get = std::move(getter);
}

void Host::set_entity_finder(EntityFinder finder) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->entity_find = std::move(finder);
}

void Host::set_entity_writes(EntityDespawner despawn, EntityDamager damage,
                             EntityTeleporter teleport) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->entity_despawn = std::move(despawn);
  impl_->entity_damage = std::move(damage);
  impl_->entity_teleport = std::move(teleport);
}

void Host::set_inventory(InventoryReader read, InventoryWriter write) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->inventory_read = std::move(read);
  impl_->inventory_write = std::move(write);
}

void Host::set_op_levels(OpLevelLookup levels) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->op_level = std::move(levels);
}

void Host::set_world_state(WorldTime time, WorldTimeSetter set_time, WeatherReader weather,
                           WeatherSetter set_weather) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->world_time = std::move(time);
  impl_->world_time_setter = std::move(set_time);
  impl_->weather_reader = std::move(weather);
  impl_->weather_setter = std::move(set_weather);
}

void Host::set_messaging(PlayerMessenger message, Broadcaster broadcast, PlayerCounter count) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->message_player = std::move(message);
  impl_->broadcast_message = std::move(broadcast);
  impl_->player_counter = std::move(count);
}

void Host::set_player_state(PlayerGamemodeSetter gamemode, PlayerHealthSetter health,
                            PlayerFoodSetter food) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->player_gamemode = std::move(gamemode);
  impl_->player_health = std::move(health);
  impl_->player_food = std::move(food);
}

Host::~Host() {
  unload_all();
}

bool Host::load_directory(const std::string& dir, std::string* error) {
  if (dir.empty())
    return true;
  const std::filesystem::path root(dir);
  std::error_code ec;
  impl_->unloaded = false;
  if (!std::filesystem::is_directory(root, ec)) {
    if (error != nullptr)
      *error = "plugins: " + dir + " is not a directory";
    return false;
  }
  std::vector<std::filesystem::path> candidates;
  if (std::filesystem::is_regular_file(root / kManifestName, ec)) {
    // `--plugins` may point straight at one plugin instead of a directory of
    // them, which is what a development loop wants.
    candidates.push_back(root);
  } else {
    for (const auto& entry : std::filesystem::directory_iterator(root, ec))
      if (entry.is_directory())
        candidates.push_back(entry.path());
    std::sort(candidates.begin(), candidates.end());
  }

  // Parse every manifest before loading anything: `depends` names plugins, and
  // the order that satisfies them is computed over the whole set. A plugin may
  // depend on one an earlier load_directory call already brought in, which is
  // why the names already loaded are part of the satisfied set.
  struct Candidate {
    std::filesystem::path dir;
    Manifest manifest;
  };
  std::vector<Candidate> found;
  std::unordered_map<std::string, std::size_t> by_name;
  std::unordered_map<std::string, bool> available;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& plugin : impl_->plugins)
      available[plugin->name] = true;
  }
  for (const std::filesystem::path& candidate : candidates) {
    if (!std::filesystem::is_regular_file(candidate / kManifestName, ec))
      continue; // an ordinary directory in the plugins root is not an error
    Manifest manifest;
    std::string why;
    if (!read_manifest(candidate / kManifestName, &manifest, &why)) {
      mc::log::error("plugins: ", why);
      ++impl_->failed;
      continue;
    }
    if (manifest.api != PMC_PLUGIN_ABI_VERSION) {
      mc::log::error("plugins: ", manifest.name, " wants ABI ", manifest.api,
                     " but this engine is ABI ", PMC_PLUGIN_ABI_VERSION);
      ++impl_->failed;
      continue;
    }
    if (available.count(manifest.name) != 0) {
      mc::log::error("plugins: ", manifest.name, ": already loaded");
      ++impl_->failed;
      continue;
    }
    if (!by_name.emplace(manifest.name, found.size()).second) {
      mc::log::error("plugins: ", manifest.name, ": another plugin in ", root.string(),
                     " uses the same name");
      ++impl_->failed;
      continue;
    }
    found.push_back(Candidate{candidate, std::move(manifest)});
  }

  // Depth-first topological sort. `state` is unvisited / visiting / done /
  // refused. A back edge is a dependency cycle and refuses every plugin on it;
  // a missing dependency refuses only the plugins that need it.
  std::vector<int> state(found.size(), 0);
  std::vector<int> stack;
  std::vector<std::size_t> order;
  std::function<bool(std::size_t)> visit = [&](std::size_t index) -> bool {
    if (state[index] == 2)
      return true;
    if (state[index] == 3)
      return false;
    if (state[index] == 1) {
      std::string cycle;
      auto at = std::find(stack.begin(), stack.end(), static_cast<int>(index));
      for (; at != stack.end(); ++at)
        cycle += found[static_cast<std::size_t>(*at)].manifest.name + " -> ";
      cycle += found[index].manifest.name;
      mc::log::error("plugins: dependency cycle: ", cycle, " (all refused)");
      state[index] = 3;
      return false;
    }
    state[index] = 1;
    stack.push_back(static_cast<int>(index));
    for (const std::string& dependency : found[index].manifest.depends) {
      const auto target = by_name.find(dependency);
      if (target == by_name.end()) {
        if (available.count(dependency) != 0)
          continue; // an earlier load call already satisfied it
        mc::log::error("plugins: ", found[index].manifest.name, " depends on '", dependency,
                       "', which is not present");
        state[index] = 3;
        stack.pop_back();
        return false;
      }
      if (!visit(target->second)) {
        mc::log::error("plugins: ", found[index].manifest.name, " depends on '", dependency,
                       "', which was refused");
        state[index] = 3;
        stack.pop_back();
        return false;
      }
    }
    stack.pop_back();
    state[index] = 2;
    order.push_back(index);
    return true;
  };
  for (std::size_t i = 0; i < found.size(); ++i)
    visit(i);
  // Every plugin the sort refused is a load failure like a bad manifest or a
  // failed dlopen; count each one once, not once per edge that named it.
  for (const int state_of : state)
    if (state_of == 3)
      ++impl_->failed;

  // Load in dependency order. A plugin whose dependency failed to load is
  // refused too, so a dependent never runs against an absent peer.
  std::size_t loaded = 0;
  std::unordered_map<std::string, bool> loaded_ok = available;
  for (const std::size_t index : order) {
    const Candidate& candidate = found[index];
    bool deps_ok = true;
    for (const std::string& dependency : candidate.manifest.depends) {
      const auto it = loaded_ok.find(dependency);
      if (it == loaded_ok.end() || !it->second) {
        mc::log::error("plugins: ", candidate.manifest.name, " depends on '", dependency,
                       "', which did not load");
        deps_ok = false;
        break;
      }
    }
    if (!deps_ok) {
      loaded_ok[candidate.manifest.name] = false;
      ++impl_->failed;
      continue;
    }
    if (impl_->load_one(candidate.dir, candidate.manifest) != nullptr) {
      loaded_ok[candidate.manifest.name] = true;
      ++loaded;
    } else {
      loaded_ok[candidate.manifest.name] = false;
    }
  }
  mc::log::info("plugins: ", loaded, " loaded, ", impl_->failed, " failed from ", root.string());
  return true;
}

void Host::enable_all() {
  std::vector<LoadedPlugin*> loaded;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& plugin : impl_->plugins)
      loaded.push_back(plugin.get());
  }
  for (LoadedPlugin* plugin : loaded)
    impl_->enable_one(*plugin);
}

void Host::Impl::purge_owner(LoadedPlugin* owner) {
  std::lock_guard<std::mutex> lock(mutex);
  purge_owner_locked(owner);
}

void Host::Impl::purge_owner_locked(LoadedPlugin* owner) {
  listeners.erase(
      std::remove_if(listeners.begin(), listeners.end(),
                     [owner](const Listener& listener) { return listener.owner == owner; }),
      listeners.end());
  commands.erase(std::remove_if(commands.begin(), commands.end(),
                                [owner](const Command& command) { return command.owner == owner; }),
                 commands.end());
  tasks.erase(std::remove_if(tasks.begin(), tasks.end(),
                             [owner](const Task& task) { return task.owner == owner; }),
              tasks.end());
}

// ---- runtime unload and hot reload (slice 21) --------------------------------

void Host::Impl::disable_and_unload(LoadedPlugin& plugin) {
  if (plugin.enabled && !plugin.disabled && plugin.descriptor != nullptr &&
      plugin.descriptor->on_disable != nullptr) {
    current.store(&plugin);
    {
      ScopedCurrent scope(&plugin);
      try {
        plugin.descriptor->on_disable(plugin.data);
      } catch (const std::exception& e) {
        mc::log::error("plugins: ", plugin.name, ": on_disable threw: ", e.what());
      } catch (...) {
        mc::log::error("plugins: ", plugin.name, ": on_disable threw");
      }
    }
    current.store(nullptr);
    plugin.disabled = true;
    mc::log::info("plugins: disabled ", plugin.name);
  }
  if (plugin.descriptor != nullptr && plugin.descriptor->on_unload != nullptr) {
    current.store(&plugin);
    {
      ScopedCurrent scope(&plugin);
      try {
        plugin.descriptor->on_unload(plugin.data);
      } catch (const std::exception& e) {
        mc::log::error("plugins: ", plugin.name, ": on_unload threw: ", e.what());
      } catch (...) {
        mc::log::error("plugins: ", plugin.name, ": on_unload threw");
      }
    }
    current.store(nullptr);
  }
}

bool Host::Impl::unload_one(const std::string& name, std::string* error) {
  LoadedPlugin* plugin = nullptr;
  std::string dependent;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& loaded : plugins)
      if (loaded->name == name) {
        plugin = loaded.get();
        break;
      }
    if (plugin == nullptr) {
      if (error != nullptr)
        *error = "no plugin named '" + name + "'";
      return false;
    }
    for (const auto& loaded : plugins) {
      if (loaded.get() == plugin)
        continue;
      for (const std::string& dependency : loaded->depends)
        if (dependency == name) {
          dependent = loaded->name;
          break;
        }
      if (!dependent.empty())
        break;
    }
  }
  if (!dependent.empty()) {
    if (error != nullptr)
      *error = dependent + " depends on it";
    return false;
  }
  // Stop new callbacks, then wait for the ones already running. The counter is
  // held only for the increment and the decrement, never across plugin code,
  // so a slow handler delays the unload instead of deadlocking it.
  {
    std::unique_lock<std::mutex> lock(plugin->state_mutex);
    plugin->retiring = true;
    plugin->idle_cv.wait(lock, [plugin] { return plugin->active == 0; });
  }
  // The shared worker may still be running this plugin's blocking callback.
  drain_async();
  disable_and_unload(*plugin);
  if (plugin->handle != nullptr)
    close_library(plugin->handle);
  plugin->handle = nullptr;
  {
    // The registrations go, but the plugin object moves to `retired_plugins`
    // instead of being destroyed: a dispatch that copied a listener before the
    // purge still holds this pointer and must read `retiring` from live memory.
    std::lock_guard<std::mutex> lock(mutex);
    purge_owner_locked(plugin);
    for (auto it = plugins.begin(); it != plugins.end(); ++it) {
      if (it->get() == plugin) {
        retired_plugins.push_back(std::move(*it));
        plugins.erase(it);
        break;
      }
    }
  }
  mc::log::info("plugins: unloaded ", plugin->name);
  return true;
}

void Host::Impl::reload_one(const std::string& name) {
  std::string dir;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& loaded : plugins)
      if (loaded->name == name) {
        dir = loaded->dir;
        break;
      }
  }
  if (dir.empty()) {
    mc::log::error("plugins: cannot reload ", name, ": it is not loaded");
    return;
  }
  std::string why;
  if (!unload_one(name, &why)) {
    mc::log::error("plugins: cannot reload ", name, ": ", why);
    return;
  }
  // The manifest is re-read, so a rebuild that changed the version or the
  // dependencies is honoured; only the configuration is expected to be the
  // common edit, but the file on disk is the source of truth either way.
  Manifest manifest;
  if (!read_manifest(std::filesystem::path(dir) / kManifestName, &manifest, &why)) {
    mc::log::error("plugins: reload ", name, " failed: ", why);
    return;
  }
  if (manifest.api != PMC_PLUGIN_ABI_VERSION) {
    mc::log::error("plugins: reload ", name, " failed: the manifest wants ABI ", manifest.api,
                   " but this engine is ABI ", PMC_PLUGIN_ABI_VERSION);
    return;
  }
  if (manifest.name != name) {
    mc::log::error("plugins: reload ", name, " failed: the manifest now names '", manifest.name,
                   "'");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (const std::string& dependency : manifest.depends) {
      bool present = false;
      for (const auto& loaded : plugins)
        if (loaded->name == dependency) {
          present = true;
          break;
        }
      if (!present) {
        mc::log::error("plugins: reload ", name, " failed: it depends on '", dependency,
                       "', which is not loaded");
        return;
      }
    }
  }
  LoadedPlugin* loaded = load_one(std::filesystem::path(dir), manifest);
  if (loaded == nullptr) {
    mc::log::error("plugins: reload ", name, " failed: the module did not load");
    return;
  }
  enable_one(*loaded);
  mc::log::info("plugins: reloaded ", loaded->name, " ", loaded->version);
}

bool Host::Impl::request_lifecycle(const std::string& name, bool reload, std::string* error) {
  std::lock_guard<std::mutex> lock(mutex);
  LoadedPlugin* found = nullptr;
  for (const auto& plugin : plugins)
    if (plugin->name == name) {
      found = plugin.get();
      break;
    }
  if (found == nullptr) {
    if (error != nullptr)
      *error = "no plugin named '" + name + "'";
    return false;
  }
  // Refused here for an immediate answer, and again when the request runs: the
  // tree may have changed between the two.
  for (const auto& plugin : plugins) {
    if (plugin.get() == found)
      continue;
    for (const std::string& dependency : plugin->depends)
      if (dependency == name) {
        if (error != nullptr)
          *error = plugin->name + " depends on it";
        return false;
      }
  }
  pending_lifecycle.push_back(
      LifecycleRequest{reload ? LifecycleKind::kReload : LifecycleKind::kUnload, name});
  return true;
}

void Host::Impl::process_lifecycle_requests() {
  // Swapped under the lock and run without it: a reload reads a manifest,
  // waits for callbacks and opens a module, none of which may hold the
  // registries a handler needs.
  std::vector<LifecycleRequest> requests;
  {
    std::lock_guard<std::mutex> lock(mutex);
    requests.swap(pending_lifecycle);
  }
  for (const LifecycleRequest& request : requests) {
    if (request.kind == LifecycleKind::kReload) {
      reload_one(request.name);
    } else {
      std::string why;
      if (!unload_one(request.name, &why))
        mc::log::error("plugins: cannot unload ", request.name, ": ", why);
    }
  }
}

void Host::unload_all() {
  if (impl_->unloaded)
    return;
  impl_->unloaded = true;
  std::vector<LoadedPlugin*> loaded;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& plugin : impl_->plugins)
      loaded.push_back(plugin.get());
  }
  // No callback may be running when its module is closed: first stop new ones
  // and wait for the ones on session threads, then let the async worker finish.
  for (LoadedPlugin* plugin : loaded) {
    std::unique_lock<std::mutex> lock(plugin->state_mutex);
    plugin->retiring = true;
    plugin->idle_cv.wait(lock, [plugin] { return plugin->active == 0; });
  }
  impl_->drain_async();
  for (auto it = loaded.rbegin(); it != loaded.rend(); ++it) {
    LoadedPlugin& plugin = **it;
    impl_->disable_and_unload(plugin);
    if (plugin.handle != nullptr)
      close_library(plugin.handle);
    plugin.handle = nullptr;
    mc::log::info("plugins: unloaded ", plugin.name);
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->listeners.clear();
  impl_->commands.clear();
  impl_->tasks.clear();
  impl_->plugins.clear();
  impl_->retired_plugins.clear();
}

std::vector<Host::PluginInfo> Host::loaded_plugins() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<PluginInfo> out;
  out.reserve(impl_->plugins.size());
  for (const auto& plugin : impl_->plugins)
    out.push_back(PluginInfo{plugin->name, plugin->version, plugin->enabled && !plugin->disabled,
                             plugin->depends});
  return out;
}

bool Host::request_reload(const std::string& name, std::string* error) {
  return impl_->request_lifecycle(name, true, error);
}

bool Host::request_unload(const std::string& name, std::string* error) {
  return impl_->request_lifecycle(name, false, error);
}

std::size_t Host::plugin_count() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->plugins.size();
}

std::size_t Host::failed_count() const {
  return impl_->failed;
}

uint64_t Host::tick_count() const {
  return impl_->now.load();
}

void Host::tick(uint64_t ticks) {
  if (ticks > 0) {
    impl_->pump(ticks);
    fire(PMC_EVENT_TICK, nullptr, nullptr, {}, false);
  }
  // A reload requested by this tick's own handler, or by a session thread,
  // runs here: the tick thread is outside every plugin callback now, so the
  // module can be closed without unwinding through it.
  impl_->process_lifecycle_requests();
}

bool Host::fire(uint32_t type, const std::string* player, const std::string* message,
                const std::function<void(const std::string&)>& reply, bool cancelable,
                const BlockEvent* block, const ItemUseEvent* item_use,
                const EntityDeathEvent* death) {
  std::vector<Listener> copy;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const Listener& listener : impl_->listeners)
      if (listener.type == type)
        copy.push_back(listener);
  }
  if (copy.empty())
    return false;
  pmc_reply handle{reply};
  pmc_event event{};
  event.struct_size = sizeof(pmc_event);
  event.type = type;
  event.cancelable = cancelable ? 1 : 0;
  event.player = player != nullptr ? player->c_str() : nullptr;
  event.message = message != nullptr ? message->c_str() : nullptr;
  event.tick = impl_->now.load();
  event.reply = reply ? &handle : nullptr;
  if (block != nullptr) {
    event.block_x = block->x;
    event.block_y = block->y;
    event.block_z = block->z;
    event.block = block->block.c_str();
    event.previous = block->previous.c_str();
    event.block_id = block->block_id;
  }
  if (item_use != nullptr) {
    event.block_x = item_use->x;
    event.block_y = item_use->y;
    event.block_z = item_use->z;
    event.block = item_use->block.c_str();
    event.block_id = item_use->block_id;
    event.item = item_use->item.c_str();
    event.face = item_use->face;
  }
  if (death != nullptr) {
    event.entity_id = death->entity_id;
    event.entity_type = death->type.c_str();
    event.entity_x = death->x;
    event.entity_y = death->y;
    event.entity_z = death->z;
    event.killer_entity_id = death->killer_entity_id;
    event.killer_name = death->killer_name.empty() ? nullptr : death->killer_name.c_str();
  }
  // Every listener runs, even when an earlier one cancels: one chat filter must
  // not starve another plugin's observer, and a cancel means "this handler
  // objects", not "stop delivering". The host honours a cancel from any of
  // them when it returns.
  bool cancelled = false;
  for (const Listener& listener : copy) {
    // A listener copied just before its plugin was unloaded is skipped rather
    // than called into a closed module; one that was already running keeps the
    // unload waiting until it returns.
    CallGuard guard(listener.owner);
    if (!guard.entered())
      continue;
    ScopedCurrent scope(listener.owner);
    event.cancel = 0;
    try {
      listener.fn(listener.data, &event);
    } catch (const std::exception& e) {
      mc::log::error("plugins: [", current_name(), "] event ", type, " threw: ", e.what());
    } catch (...) {
      mc::log::error("plugins: [", current_name(), "] event ", type, " threw");
    }
    if (event.cancelable != 0 && event.cancel != 0)
      cancelled = true;
  }
  return cancelled;
}

bool Host::dispatch_command(const std::string& player, const std::vector<std::string>& tokens,
                            const std::function<void(const std::string&)>& reply) {
  if (tokens.empty())
    return false;
  std::string name = tokens.front();
  if (!name.empty() && name.front() == '/')
    name.erase(name.begin());
  for (char& c : name)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  Command found;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const Command& command : impl_->commands) {
      if (command.name == name) {
        found = command;
        break;
      }
    }
  }
  if (found.fn == nullptr)
    return false;
  if (found.op_level > 0) {
    OpLevelLookup lookup;
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      lookup = impl_->op_level;
    }
    int level = 0;
    if (lookup) {
      try {
        level = lookup(player);
      } catch (...) {
        level = 0;
      }
    }
    if (level < found.op_level) {
      if (reply)
        reply("You do not have permission to use /" + name + " (requires operator level " +
              std::to_string(found.op_level) + ").");
      mc::log::info("plugins: ", player, " denied /", name, " (operator level ", level, " < ",
                    found.op_level, ")");
      return true;
    }
  }
  std::vector<const char*> argv;
  argv.reserve(tokens.size());
  for (const std::string& token : tokens)
    argv.push_back(token.c_str());
  pmc_reply handle{reply};
  pmc_command command{};
  command.struct_size = sizeof(pmc_command);
  command.name = found.name.c_str();
  command.argc = static_cast<int>(argv.size());
  command.argv = argv.data();
  command.player = player.c_str();
  command.reply = reply ? &handle : nullptr;
  CallGuard guard(found.owner);
  if (!guard.entered())
    return true; // the owner was unloaded between the lookup and the call
  ScopedCurrent scope(found.owner);
  try {
    found.fn(found.data, &command);
  } catch (const std::exception& e) {
    mc::log::error("plugins: [", current_name(), "] command '", name, "' threw: ", e.what());
  } catch (...) {
    mc::log::error("plugins: [", current_name(), "] command '", name, "' threw");
  }
  return true;
}

Host& host() {
  static Host instance;
  return instance;
}

bool fire_player_chat(const std::string& player, const std::string& message,
                      const std::function<void(const std::string&)>& reply) {
  return host().fire(PMC_EVENT_PLAYER_CHAT, &player, &message, reply, true);
}

void fire_player_join(const std::string& player,
                      const std::function<void(const std::string&)>& reply) {
  host().fire(PMC_EVENT_PLAYER_JOIN, &player, nullptr, reply, false);
}

void fire_player_leave(const std::string& player) {
  host().fire(PMC_EVENT_PLAYER_LEAVE, &player, nullptr, {}, false);
}

bool fire_block_place(const std::string& player, const BlockEvent& block,
                      const std::function<void(const std::string&)>& reply) {
  return host().fire(PMC_EVENT_BLOCK_PLACE, &player, nullptr, reply, true, &block);
}

bool fire_block_break(const std::string& player, const BlockEvent& block,
                      const std::function<void(const std::string&)>& reply) {
  return host().fire(PMC_EVENT_BLOCK_BREAK, &player, nullptr, reply, true, &block);
}

bool fire_item_use_on(const std::string& player, const ItemUseEvent& use,
                      const std::function<void(const std::string&)>& reply) {
  return host().fire(PMC_EVENT_ITEM_USE_ON, &player, nullptr, reply, true, nullptr, &use);
}

void fire_entity_death(const EntityDeathEvent& death) {
  host().fire(PMC_EVENT_ENTITY_DEATH, nullptr, nullptr, {}, false, nullptr, nullptr, &death);
}

bool dispatch_command(const std::string& player, const std::vector<std::string>& tokens,
                      const std::function<void(const std::string&)>& reply) {
  return host().dispatch_command(player, tokens, reply);
}

} // namespace mc::plugins
