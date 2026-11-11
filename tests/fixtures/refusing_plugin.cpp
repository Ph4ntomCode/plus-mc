// A plugin that registers a tick listener and a command, then refuses to load.
//
// The host must drop both registrations before closing the module: a listener
// left behind would be called on the next tick, after `dlclose`/`FreeLibrary`,
// which is a crash in a server that merely loaded a bad plugin. The loader test
// asserts the process keeps ticking and shuts down cleanly.
#include "pluginapi/plugin.h"

namespace {

void on_tick(void* /*data*/, pmc_event* /*event*/) {}

void on_refused_command(void* /*data*/, pmc_command* /*command*/) {}

int plugin_load(const pmc_host_api* api, void** plugin_data) {
  // Both registrations are real. A host that forgets to purge a refused plugin
  // leaves callbacks pointing into this module.
  if (api->on(api->host, PMC_EVENT_TICK, &on_tick, nullptr) != 0)
    return 2;
  if (api->command(api->host, "refusedcmd", "/refusedcmd", &on_refused_command, nullptr) != 0)
    return 3;
  *plugin_data = nullptr;
  return 1; // refuse after registering
}

const pmc_plugin kPlugin = {
    sizeof(pmc_plugin),
    PMC_PLUGIN_ABI_VERSION,
    "refusing",
    "1.0.0",
    "+MC",
    "Test fixture: registers then refuses on_load.",
    &plugin_load,
    nullptr,
    nullptr,
    nullptr,
};

} // namespace

extern "C" PMC_PLUGIN_EXPORT const pmc_plugin* pmc_plugin_entry(void) {
  return &kPlugin;
}
