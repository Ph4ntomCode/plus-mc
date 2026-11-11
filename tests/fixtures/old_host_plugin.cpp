// A plugin compiled against the original `pmc_host_api`, as a test fixture.
//
// The host table only grows: `struct_size` says how many bytes the host built,
// and a plugin built against an older header reads only that prefix. This
// module reproduces such a caller: `OldHost` is the original table through
// `server_version`, it checks the advertised `struct_size` before touching a
// field, registers a command through `command`, answers through `reply`, and
// uses `log`/`tick_count`/`server_version` — never an appended accessor.
// `tests/plugin_test.py` loads it beside the example and asserts `/oldhost`
// reports a table that covers the prefix.
#include "pluginapi/plugin.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

// The original host table, field for field, through `server_version`.
struct OldHost {
  uint32_t struct_size;
  uint32_t abi_version;
  void* host;
  void (*log)(void* host, int level, const char* message);
  int (*on)(void* host, uint32_t event_type, pmc_event_fn fn, void* plugin_data);
  int (*command)(void* host, const char* name, const char* usage, pmc_command_fn fn,
                 void* plugin_data);
  void (*reply)(void* host, pmc_reply* reply, const char* message);
  uint64_t (*schedule)(void* host, uint32_t delay_ticks, uint32_t repeat_ticks, pmc_task_fn fn,
                       void* plugin_data);
  int (*cancel)(void* host, uint64_t task_id);
  uint64_t (*tick_count)(void* host);
  const char* (*server_version)(void* host);
};

struct OldHostPlugin {
  const OldHost* api = nullptr;
};

void on_oldhost_command(void* data, pmc_command* command) {
  auto& plugin = *static_cast<OldHostPlugin*>(data);
  const OldHost* api = plugin.api;
  // The only fields this fixture may touch are inside this prefix.
  const bool size_ok = api->struct_size >= sizeof(OldHost);
  const bool abi_ok = api->abi_version == PMC_PLUGIN_ABI_VERSION;
  const char* version = api->server_version(api->host);
  char line[256];
  std::snprintf(line, sizeof(line),
                "(oldhost) prefix %s: table %u bytes, prefix %zu bytes, abi %u, "
                "tick %llu, server %s",
                (size_ok && abi_ok) ? "ok" : "bad", api->struct_size, sizeof(OldHost),
                api->abi_version, static_cast<unsigned long long>(api->tick_count(api->host)),
                version != nullptr ? version : "(none)");
  api->reply(api->host, command->reply, line);
}

int plugin_load(const pmc_host_api* api, void** plugin_data) {
  // This is the old caller: read only the prefix, and only after the host says
  // the table holds it.
  const auto* old = reinterpret_cast<const OldHost*>(api);
  if (old->struct_size < sizeof(OldHost) || old->abi_version != PMC_PLUGIN_ABI_VERSION)
    return 1;
  auto* plugin = new OldHostPlugin();
  plugin->api = old;
  if (old->command(old->host, "oldhost", "/oldhost", &on_oldhost_command, plugin) != 0) {
    delete plugin;
    return 1;
  }
  old->log(old->host, PMC_LOG_INFO, "old-host fixture: read the original table prefix");
  *plugin_data = plugin;
  return 0;
}

void plugin_enable(void* data) {
  auto& plugin = *static_cast<OldHostPlugin*>(data);
  plugin.api->log(plugin.api->host, PMC_LOG_INFO, "old-host fixture: enabled");
}

void plugin_disable(void* data) {
  auto& plugin = *static_cast<OldHostPlugin*>(data);
  plugin.api->log(plugin.api->host, PMC_LOG_INFO, "old-host fixture: disabled");
}

void plugin_unload(void* data) {
  delete static_cast<OldHostPlugin*>(data);
}

const pmc_plugin kPlugin = {
    sizeof(pmc_plugin),
    PMC_PLUGIN_ABI_VERSION,
    "oldhost",
    "1.0.0",
    "+MC",
    "Test fixture: reads only the original pmc_host_api prefix.",
    &plugin_load,
    &plugin_enable,
    &plugin_disable,
    &plugin_unload,
};

} // namespace

extern "C" PMC_PLUGIN_EXPORT const pmc_plugin* pmc_plugin_entry(void) {
  return &kPlugin;
}
