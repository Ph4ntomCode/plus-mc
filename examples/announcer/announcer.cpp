// Seed plugin: a scheduled broadcast plus the config reads (ABI 1, slices 11
// and 17).
//
// `message` and `interval_ticks` come from the plugin's own `config.toml`; the
// repeating task is the tick scheduler, and the delivery is `broadcast`, so the
// plugin never needs a player handle. `/announce` sends one line immediately.
#include "pluginapi/plugin.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace {

struct Announcer {
  const pmc_host_api* api = nullptr;
  std::string message = "Welcome to the server!";
  int interval_ticks = 6000;
  uint64_t task = 0;
  std::atomic<uint64_t> sent{0};
};

void log_line(const Announcer& plugin, int level, const std::string& text) {
  plugin.api->log(plugin.api->host, level, text.c_str());
}

void reply(const pmc_command* command, const Announcer& plugin, const std::string& text) {
  if (command->reply != nullptr)
    plugin.api->reply(plugin.api->host, command->reply, text.c_str());
}

void announce_task(void* data) {
  Announcer& plugin = *static_cast<Announcer*>(data);
  const int reached = plugin.api->broadcast(plugin.api->host, plugin.message.c_str());
  if (reached > 0)
    plugin.sent.fetch_add(1);
}

void command_announce(void* data, pmc_command* command) {
  Announcer& plugin = *static_cast<Announcer*>(data);
  if (command->argc < 2) {
    reply(command, plugin, "usage: /announce <text>");
    return;
  }
  std::string text;
  for (int i = 1; i < command->argc; ++i) {
    if (i != 1)
      text += ' ';
    text += command->argv[i] != nullptr ? command->argv[i] : "";
  }
  const int reached = plugin.api->broadcast(plugin.api->host, text.c_str());
  if (reached < 0) {
    reply(command, plugin, "(announcer) the message was refused");
    return;
  }
  log_line(plugin, PMC_LOG_INFO, "announced to " + std::to_string(reached) + " player(s)");
  reply(command, plugin, "(announcer) sent to " + std::to_string(reached) + " player(s)");
}

void command_status(void* data, pmc_command* command) {
  Announcer& plugin = *static_cast<Announcer*>(data);
  reply(command, plugin,
        "(announcer) every " + std::to_string(plugin.interval_ticks) + " ticks, " +
            std::to_string(plugin.sent.load()) + " sent, message: " + plugin.message);
}

int on_load(const pmc_host_api* api, void** plugin_data) {
  auto plugin = std::make_unique<Announcer>();
  plugin->api = api;
  if (const char* configured = api->config_string(api->host, "message"); configured != nullptr)
    plugin->message = configured;
  const long long interval = api->config_int(api->host, "interval_ticks", 6000);
  // 20 ticks is one second; a longer upper bound keeps a typo from flooding.
  plugin->interval_ticks =
      static_cast<int>(interval < 20 ? 20 : (interval > 72000 ? 72000 : interval));
  api->command(api->host, "announce", "/announce <text>", command_announce, plugin.get());
  api->command(api->host, "announcer", "/announcer", command_status, plugin.get());
  log_line(*plugin, PMC_LOG_INFO,
           "announcer: 2 commands, every " + std::to_string(plugin->interval_ticks) + " ticks");
  *plugin_data = plugin.release();
  return 0;
}

void on_enable(void* plugin_data) {
  Announcer& plugin = *static_cast<Announcer*>(plugin_data);
  plugin.task =
      plugin.api->schedule(plugin.api->host, static_cast<uint32_t>(plugin.interval_ticks),
                           static_cast<uint32_t>(plugin.interval_ticks), announce_task, &plugin);
  if (plugin.task == 0)
    log_line(plugin, PMC_LOG_WARN, "announcer: the scheduler refused the task");
  else
    log_line(plugin, PMC_LOG_INFO, "announcer: scheduled");
}

void on_disable(void* plugin_data) {
  Announcer& plugin = *static_cast<Announcer*>(plugin_data);
  if (plugin.task != 0)
    plugin.api->cancel(plugin.api->host, plugin.task);
  plugin.task = 0;
  log_line(plugin, PMC_LOG_INFO, "announcer: disabled");
}

void on_unload(void* plugin_data) {
  delete static_cast<Announcer*>(plugin_data);
}

const pmc_plugin kPlugin = {
    sizeof(pmc_plugin),
    PMC_PLUGIN_ABI_VERSION,
    "announcer",
    "1.0.0",
    "+MC",
    "Seed plugin: a config-driven repeating broadcast with /announce and /announcer.",
    on_load,
    on_enable,
    on_disable,
    on_unload,
};

} // namespace

extern "C" PMC_PLUGIN_EXPORT const pmc_plugin* pmc_plugin_entry(void) {
  return &kPlugin;
}
