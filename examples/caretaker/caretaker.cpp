// Seed plugin: the three player-state writes (ABI 1, slice 18).
//
// `/heal`, `/feed` and `/gm` are the operations a hub, minigame or moderation
// plugin reaches for. Each one is *queued* for the target's own session — the
// call reports accepted, not completed — so the answers say "queued" rather
// than claiming the client already drew the new bar.
#include "pluginapi/plugin.h"

#include <cerrno>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

struct Caretaker {
  const pmc_host_api* api = nullptr;
};

void log_line(const Caretaker& plugin, int level, const std::string& text) {
  plugin.api->log(plugin.api->host, level, text.c_str());
}

void reply(const pmc_command* command, const Caretaker& plugin, const std::string& text) {
  if (command->reply != nullptr)
    plugin.api->reply(plugin.api->host, command->reply, text.c_str());
}

const char* argument(const pmc_command* command, int index) {
  if (index < 0 || index >= command->argc)
    return nullptr;
  return command->argv[index];
}

// The first argument when it is present, else whoever typed the command, so
// `/heal` means the caller and `/heal Bob` means Bob.
std::string target_of(const pmc_command* command) {
  if (const char* named = argument(command, 1); named != nullptr && *named != '\0')
    return named;
  return command->player != nullptr ? command->player : "";
}

bool parse_long(const char* text, long* out) {
  if (text == nullptr || *text == '\0')
    return false;
  errno = 0;
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (end == text || *end != '\0' || errno == ERANGE)
    return false;
  *out = value;
  return true;
}

void command_heal(void* data, pmc_command* command) {
  Caretaker& plugin = *static_cast<Caretaker*>(data);
  const std::string target = target_of(command);
  if (target.empty()) {
    reply(command, plugin, "usage: /heal [player] [health 0-20]");
    return;
  }
  float health = 20.0f;
  if (const char* text = argument(command, 2); text != nullptr) {
    char* end = nullptr;
    health = std::strtof(text, &end);
    if (end == text || *end != '\0' || !(health > 0.0f) || health > 20.0f) {
      reply(command, plugin, "usage: /heal [player] [health 0-20]");
      return;
    }
  }
  const int result = plugin.api->player_set_health(plugin.api->host, target.c_str(), health);
  if (result == PMC_PLAYER_NO_PLAYER) {
    reply(command, plugin, "(caretaker) no player named " + target);
    return;
  }
  if (result != PMC_PLAYER_OK) {
    reply(command, plugin, "(caretaker) cannot heal " + target);
    return;
  }
  log_line(plugin, PMC_LOG_INFO, "heal " + target + " -> " + std::to_string(health));
  reply(command, plugin, "(caretaker) queued " + std::to_string(health) + " health for " + target);
}

void command_feed(void* data, pmc_command* command) {
  Caretaker& plugin = *static_cast<Caretaker*>(data);
  const std::string target = target_of(command);
  if (target.empty()) {
    reply(command, plugin, "usage: /feed [player] [food 0-20] [saturation]");
    return;
  }
  long food = 20;
  if (const char* text = argument(command, 2);
      text != nullptr && (!parse_long(text, &food) || food < 0 || food > 20)) {
    reply(command, plugin, "usage: /feed [player] [food 0-20] [saturation]");
    return;
  }
  float saturation = 5.0f;
  if (const char* text = argument(command, 3); text != nullptr) {
    char* end = nullptr;
    saturation = std::strtof(text, &end);
    if (end == text || *end != '\0' || !(saturation >= 0.0f)) {
      reply(command, plugin, "usage: /feed [player] [food 0-20] [saturation]");
      return;
    }
  }
  const int result = plugin.api->player_set_food(plugin.api->host, target.c_str(),
                                                 static_cast<int>(food), saturation);
  if (result == PMC_PLAYER_NO_PLAYER) {
    reply(command, plugin, "(caretaker) no player named " + target);
    return;
  }
  if (result != PMC_PLAYER_OK) {
    reply(command, plugin, "(caretaker) cannot feed " + target);
    return;
  }
  log_line(plugin, PMC_LOG_INFO,
           "feed " + target + " -> " + std::to_string(food) + "/" + std::to_string(saturation));
  reply(command, plugin, "(caretaker) queued food " + std::to_string(food) + " for " + target);
}

void command_gm(void* data, pmc_command* command) {
  Caretaker& plugin = *static_cast<Caretaker*>(data);
  const char* mode_text = argument(command, 1);
  if (mode_text == nullptr) {
    reply(command, plugin, "usage: /gm <0-3|survival|creative|adventure|spectator> [player]");
    return;
  }
  int mode = -1;
  const std::string mode_name = mode_text;
  if (mode_name == "survival" || mode_name == "s")
    mode = 0;
  else if (mode_name == "creative" || mode_name == "c")
    mode = 1;
  else if (mode_name == "adventure" || mode_name == "a")
    mode = 2;
  else if (mode_name == "spectator" || mode_name == "sp")
    mode = 3;
  else {
    long parsed = 0;
    if (!parse_long(mode_text, &parsed) || parsed < 0 || parsed > 3) {
      reply(command, plugin, "usage: /gm <0-3|survival|creative|adventure|spectator> [player]");
      return;
    }
    mode = static_cast<int>(parsed);
  }
  std::string target = command->player != nullptr ? command->player : "";
  if (const char* named = argument(command, 2); named != nullptr && *named != '\0')
    target = named;
  if (target.empty()) {
    reply(command, plugin, "(caretaker) no target");
    return;
  }
  const int result = plugin.api->player_set_gamemode(plugin.api->host, target.c_str(), mode);
  if (result == PMC_PLAYER_NO_PLAYER) {
    reply(command, plugin, "(caretaker) no player named " + target);
    return;
  }
  if (result != PMC_PLAYER_OK) {
    reply(command, plugin, "(caretaker) cannot change " + target);
    return;
  }
  log_line(plugin, PMC_LOG_INFO, "gamemode " + target + " -> " + std::to_string(mode));
  reply(command, plugin, "(caretaker) queued gamemode " + std::to_string(mode) + " for " + target);
}

int on_load(const pmc_host_api* api, void** plugin_data) {
  auto plugin = std::make_unique<Caretaker>();
  plugin->api = api;
  api->command(api->host, "heal", "/heal [player] [health 0-20]", command_heal, plugin.get());
  api->command(api->host, "feed", "/feed [player] [food 0-20] [saturation]", command_feed,
               plugin.get());
  api->command(api->host, "gm", "/gm <0-3|survival|creative|adventure|spectator> [player]",
               command_gm, plugin.get());
  log_line(*plugin, PMC_LOG_INFO, "caretaker: 3 commands ready (heal, feed, gm)");
  *plugin_data = plugin.release();
  return 0;
}

void on_enable(void* plugin_data) {
  Caretaker& plugin = *static_cast<Caretaker*>(plugin_data);
  log_line(plugin, PMC_LOG_INFO, "caretaker: enabled");
}

void on_disable(void* plugin_data) {
  Caretaker& plugin = *static_cast<Caretaker*>(plugin_data);
  log_line(plugin, PMC_LOG_INFO, "caretaker: disabled");
}

void on_unload(void* plugin_data) {
  delete static_cast<Caretaker*>(plugin_data);
}

const pmc_plugin kPlugin = {
    sizeof(pmc_plugin),
    PMC_PLUGIN_ABI_VERSION,
    "caretaker",
    "1.0.0",
    "+MC",
    "Seed plugin: /heal, /feed and /gm through the player-state writes.",
    on_load,
    on_enable,
    on_disable,
    on_unload,
};

} // namespace

extern "C" PMC_PLUGIN_EXPORT const pmc_plugin* pmc_plugin_entry(void) {
  return &kPlugin;
}
