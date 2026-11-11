// Seed plugin: inventory writes (ABI 1, slice 4).
//
// `/kit` queues one stack into a chosen slot; the target's own session applies
// it on its next tick, so the answer says "queued" and the slot can be read back
// with `inventory_get` (which the greeter example demonstrates).
#include "pluginapi/plugin.h"

#include <cerrno>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

struct Kit {
  const pmc_host_api* api = nullptr;
};

void reply(const pmc_command* command, const Kit& plugin, const std::string& text) {
  if (command->reply != nullptr)
    plugin.api->reply(plugin.api->host, command->reply, text.c_str());
}

const char* argument(const pmc_command* command, int index) {
  if (index < 0 || index >= command->argc)
    return nullptr;
  return command->argv[index];
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

void command_kit(void* data, pmc_command* command) {
  Kit& plugin = *static_cast<Kit*>(data);
  const char* item = argument(command, 1);
  if (item == nullptr) {
    reply(command, plugin, "usage: /kit <item> [count] [slot 0-40]");
    return;
  }
  long count = 1;
  if (const char* text = argument(command, 2); text != nullptr && !parse_long(text, &count)) {
    reply(command, plugin, "usage: /kit <item> [count] [slot 0-40]");
    return;
  }
  long slot = 0;
  if (const char* text = argument(command, 3); text != nullptr && !parse_long(text, &slot)) {
    reply(command, plugin, "usage: /kit <item> [count] [slot 0-40]");
    return;
  }
  if (count < 1 || count > 64 || slot < 0 || slot > 40) {
    reply(command, plugin, "usage: /kit <item> [count 1-64] [slot 0-40]");
    return;
  }
  if (command->player == nullptr) {
    reply(command, plugin, "(kit) no player");
    return;
  }
  const int result =
      plugin.api->inventory_set(plugin.api->host, command->player, static_cast<int32_t>(slot), item,
                                static_cast<int32_t>(count));
  switch (result) {
  case PMC_INVENTORY_OK:
    reply(command, plugin,
          "(kit) queued " + std::to_string(count) + " " + item + " into slot " +
              std::to_string(slot));
    break;
  case PMC_INVENTORY_NO_PLAYER:
    reply(command, plugin, "(kit) no player named " + std::string(command->player));
    break;
  case PMC_INVENTORY_UNKNOWN_ITEM:
    reply(command, plugin, "(kit) unknown item '" + std::string(item) + "'");
    break;
  case PMC_INVENTORY_OUT_OF_RANGE:
    reply(command, plugin, "(kit) slot or count is out of range");
    break;
  default:
    reply(command, plugin, "(kit) the write could not be queued");
    break;
  }
}

void command_list(void* data, pmc_command* command) {
  Kit& plugin = *static_cast<Kit*>(data);
  reply(command, plugin,
        "(kit) starter items: minecraft:stone, minecraft:bread, minecraft:torch, "
        "minecraft:iron_pickaxe, minecraft:oak_log");
}

int on_load(const pmc_host_api* api, void** plugin_data) {
  auto plugin = std::make_unique<Kit>();
  plugin->api = api;
  api->command(api->host, "kit", "/kit <item> [count 1-64] [slot 0-40]", command_kit, plugin.get());
  api->command(api->host, "kits", "/kits", command_list, plugin.get());
  api->log(api->host, PMC_LOG_INFO, "kit: 2 commands ready (kit, kits)");
  *plugin_data = plugin.release();
  return 0;
}

void on_enable(void* plugin_data) {
  Kit& plugin = *static_cast<Kit*>(plugin_data);
  plugin.api->log(plugin.api->host, PMC_LOG_INFO, "kit: enabled");
}

void on_disable(void* plugin_data) {
  Kit& plugin = *static_cast<Kit*>(plugin_data);
  plugin.api->log(plugin.api->host, PMC_LOG_INFO, "kit: disabled");
}

void on_unload(void* plugin_data) {
  delete static_cast<Kit*>(plugin_data);
}

const pmc_plugin kPlugin = {
    sizeof(pmc_plugin),
    PMC_PLUGIN_ABI_VERSION,
    "kit",
    "1.0.0",
    "+MC",
    "Seed plugin: /kit and /kits through the inventory writers.",
    on_load,
    on_enable,
    on_disable,
    on_unload,
};

} // namespace

extern "C" PMC_PLUGIN_EXPORT const pmc_plugin* pmc_plugin_entry(void) {
  return &kPlugin;
}
