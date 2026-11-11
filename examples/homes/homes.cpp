// Seed plugin: the private data directory, a player lookup and a player
// teleport (ABI 1, slices 2, 5 and 10).
//
// Homes are this plugin's own file under `<data_dir>/homes.txt`, not the host's
// flat config: the format is a list, not a key/value map. The file is rewritten
// on every change through a temporary + rename, and read once in `on_load`.
#include "pluginapi/plugin.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace {

struct Home {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

struct Homes {
  const pmc_host_api* api = nullptr;
  std::filesystem::path file;
  std::mutex mutex;
  std::map<std::string, std::map<std::string, Home>> by_player;
};

void log_line(const Homes& plugin, int level, const std::string& text) {
  plugin.api->log(plugin.api->host, level, text.c_str());
}

void reply(const pmc_command* command, const Homes& plugin, const std::string& text) {
  if (command->reply != nullptr)
    plugin.api->reply(plugin.api->host, command->reply, text.c_str());
}

const char* argument(const pmc_command* command, int index) {
  if (index < 0 || index >= command->argc)
    return nullptr;
  return command->argv[index];
}

// A name the tab-separated file can carry and a command can type.
bool home_name_ok(const std::string& name) {
  if (name.empty() || name.size() > 32)
    return false;
  for (const char c : name) {
    if (c == '\t' || c == '\n' || c == '\r' || c == ' ')
      return false;
  }
  return true;
}

bool parse_double(const char* text, double* out) {
  if (text == nullptr || *text == '\0')
    return false;
  errno = 0;
  char* end = nullptr;
  const double value = std::strtod(text, &end);
  if (end == text || *end != '\0' || errno == ERANGE || !std::isfinite(value))
    return false;
  *out = value;
  return true;
}

void load_locked(Homes& plugin) {
  std::ifstream in(plugin.file);
  if (!in)
    return;
  std::string line;
  while (std::getline(in, line)) {
    // player \t name \t x \t y \t z
    const std::size_t first = line.find('\t');
    if (first == std::string::npos)
      continue;
    const std::size_t second = line.find('\t', first + 1);
    if (second == std::string::npos)
      continue;
    const std::size_t third = line.find('\t', second + 1);
    if (third == std::string::npos)
      continue;
    const std::size_t fourth = line.find('\t', third + 1);
    if (fourth == std::string::npos)
      continue;
    const std::string player = line.substr(0, first);
    const std::string name = line.substr(first + 1, second - first - 1);
    Home home;
    if (player.empty() || !home_name_ok(name) ||
        !parse_double(line.substr(second + 1, third - second - 1).c_str(), &home.x) ||
        !parse_double(line.substr(third + 1, fourth - third - 1).c_str(), &home.y) ||
        !parse_double(line.substr(fourth + 1).c_str(), &home.z)) {
      continue; // a damaged line is skipped, not a refused plugin
    }
    plugin.by_player[player][name] = home;
  }
}

bool save_locked(const Homes& plugin) {
  const std::filesystem::path temporary = plugin.file.string() + ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out)
      return false;
    for (const auto& [player, homes] : plugin.by_player)
      for (const auto& [name, home] : homes)
        out << player << '\t' << name << '\t' << home.x << '\t' << home.y << '\t' << home.z << '\n';
    out.flush();
    if (!out)
      return false;
  }
  std::error_code error;
  std::filesystem::rename(temporary, plugin.file, error);
  if (error) {
    // Windows does not replace an existing destination; POSIX does.
    std::error_code ignored;
    std::filesystem::remove(plugin.file, ignored);
    error.clear();
    std::filesystem::rename(temporary, plugin.file, error);
  }
  if (error) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return false;
  }
  return true;
}

std::string home_arg(const pmc_command* command) {
  if (const char* name = argument(command, 1); name != nullptr && *name != '\0')
    return name;
  return "home";
}

void command_sethome(void* data, pmc_command* command) {
  Homes& plugin = *static_cast<Homes*>(data);
  const std::string name = home_arg(command);
  if (!home_name_ok(name)) {
    reply(command, plugin, "(homes) a name is 1..32 characters with no spaces");
    return;
  }
  if (command->player == nullptr) {
    reply(command, plugin, "(homes) no player");
    return;
  }
  pmc_player player{};
  player.struct_size = sizeof(player);
  if (plugin.api->player_get(plugin.api->host, command->player, &player) != 0) {
    reply(command, plugin, "(homes) no player named " + std::string(command->player));
    return;
  }
  Home home;
  home.x = player.x;
  home.y = player.y;
  home.z = player.z;
  {
    std::lock_guard<std::mutex> lock(plugin.mutex);
    plugin.by_player[command->player][name] = home;
    if (!save_locked(plugin)) {
      reply(command, plugin, "(homes) could not save the home file");
      return;
    }
  }
  log_line(plugin, PMC_LOG_INFO, std::string("sethome ") + command->player + " " + name);
  reply(command, plugin,
        "(homes) saved '" + name + "' at " + std::to_string(home.x) + ", " +
            std::to_string(home.y) + ", " + std::to_string(home.z));
}

void command_home(void* data, pmc_command* command) {
  Homes& plugin = *static_cast<Homes*>(data);
  const std::string name = home_arg(command);
  Home home;
  {
    std::lock_guard<std::mutex> lock(plugin.mutex);
    const auto found = plugin.by_player.find(command->player != nullptr ? command->player : "");
    if (found == plugin.by_player.end()) {
      reply(command, plugin, "(homes) you have no homes; use /sethome");
      return;
    }
    const auto entry = found->second.find(name);
    if (entry == found->second.end()) {
      reply(command, plugin, "(homes) no home named '" + name + "'");
      return;
    }
    home = entry->second;
  }
  pmc_player player{};
  player.struct_size = sizeof(player);
  if (plugin.api->player_get(plugin.api->host, command->player, &player) != 0) {
    reply(command, plugin, "(homes) no player named " + std::string(command->player));
    return;
  }
  const int result =
      plugin.api->entity_teleport(plugin.api->host, player.entity_id, home.x, home.y, home.z);
  if (result != PMC_ENTITY_OK) {
    reply(command, plugin, "(homes) the teleport was refused");
    return;
  }
  log_line(plugin, PMC_LOG_INFO, std::string("home ") + command->player + " " + name);
  reply(command, plugin, "(homes) teleporting to '" + name + "'");
}

void command_delhome(void* data, pmc_command* command) {
  Homes& plugin = *static_cast<Homes*>(data);
  const std::string name = home_arg(command);
  {
    std::lock_guard<std::mutex> lock(plugin.mutex);
    const auto found = plugin.by_player.find(command->player != nullptr ? command->player : "");
    if (found == plugin.by_player.end() || found->second.erase(name) == 0) {
      reply(command, plugin, "(homes) no home named '" + name + "'");
      return;
    }
    if (found->second.empty())
      plugin.by_player.erase(found);
    if (!save_locked(plugin)) {
      reply(command, plugin, "(homes) could not save the home file");
      return;
    }
  }
  reply(command, plugin, "(homes) removed '" + name + "'");
}

void command_list(void* data, pmc_command* command) {
  Homes& plugin = *static_cast<Homes*>(data);
  std::string names;
  {
    std::lock_guard<std::mutex> lock(plugin.mutex);
    const auto found = plugin.by_player.find(command->player != nullptr ? command->player : "");
    if (found != plugin.by_player.end()) {
      for (const auto& [name, home] : found->second) {
        if (!names.empty())
          names += ", ";
        names += name;
      }
    }
  }
  reply(command, plugin,
        names.empty() ? "(homes) you have no homes; use /sethome" : "(homes) " + names);
}

int on_load(const pmc_host_api* api, void** plugin_data) {
  auto plugin = std::make_unique<Homes>();
  plugin->api = api;
  const char* directory = api->data_dir(api->host);
  if (directory == nullptr)
    return 1; // without somewhere to save, a homes plugin has nothing to offer
  plugin->file = std::filesystem::path(directory) / "homes.txt";
  {
    std::lock_guard<std::mutex> lock(plugin->mutex);
    load_locked(*plugin);
  }
  std::size_t homes = 0;
  for (const auto& [player, entries] : plugin->by_player)
    homes += entries.size();
  api->command(api->host, "sethome", "/sethome [name]", command_sethome, plugin.get());
  api->command(api->host, "home", "/home [name]", command_home, plugin.get());
  api->command(api->host, "delhome", "/delhome [name]", command_delhome, plugin.get());
  api->command(api->host, "homes", "/homes", command_list, plugin.get());
  log_line(*plugin, PMC_LOG_INFO, "homes: 4 commands, " + std::to_string(homes) + " saved homes");
  *plugin_data = plugin.release();
  return 0;
}

void on_enable(void* plugin_data) {
  Homes& plugin = *static_cast<Homes*>(plugin_data);
  log_line(plugin, PMC_LOG_INFO, "homes: enabled");
}

void on_disable(void* plugin_data) {
  Homes& plugin = *static_cast<Homes*>(plugin_data);
  std::lock_guard<std::mutex> lock(plugin.mutex);
  if (!save_locked(plugin))
    log_line(plugin, PMC_LOG_WARN, "homes: could not save on disable");
  log_line(plugin, PMC_LOG_INFO, "homes: disabled");
}

void on_unload(void* plugin_data) {
  delete static_cast<Homes*>(plugin_data);
}

const pmc_plugin kPlugin = {
    sizeof(pmc_plugin),
    PMC_PLUGIN_ABI_VERSION,
    "homes",
    "1.0.0",
    "+MC",
    "Seed plugin: /sethome, /home, /delhome and /homes with a private save file.",
    on_load,
    on_enable,
    on_disable,
    on_unload,
};

} // namespace

extern "C" PMC_PLUGIN_EXPORT const pmc_plugin* pmc_plugin_entry(void) {
  return &kPlugin;
}
