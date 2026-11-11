// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 8 slice 4: WASM plugins in a live server.
//
// `--wasm-plugins <dir>` discovers WASM packages (a `+mc-wasm.toml` beside
// their module), builds the same bounded gate and pinned runtime the CLI uses,
// and runs each package before the listener opens. A core module uses
// `pmc_wasm_load` / `pmc_wasm_enable`; a component uses its linked `on-load`,
// including Pumpkin v0.2's `init-plugin` and async `on-load`/`on-unload`.
// Shutdown runs `pmc_wasm_disable` or `on-unload` in reverse load order. Every
// effect the module can have goes through the server-side `Host`: log lines to
// the server log, broadcast/player messages through the live player registry,
// block and clock reads/writes through the engine's own paths, the entity
// registry, and a per-plugin key/value config file under `<config-dir>/wasm/`.
//
// A package that fails the gate or exceeds a budget is logged and skipped; one
// broken package never prevents the server from starting. Only an unreadable
// `--wasm-plugins` directory is a startup error.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace mc::server {

struct SessionSettings;

struct WasmPluginInfo {
  std::string name;
  std::string version;
  std::string module;
  bool enabled = false;
  std::uint64_t steps = 0;
  std::uint64_t millis = 0;
};

class WasmPlugins {
public:
  WasmPlugins();
  ~WasmPlugins();
  WasmPlugins(const WasmPlugins&) = delete;
  WasmPlugins& operator=(const WasmPlugins&) = delete;

  // Loads every package under `dir` (or `dir` itself when it is one package)
  // and starts each one. Config files live under `<config_dir>/wasm/`.
  // Returns false only when `dir` cannot be read; per-package refusals are
  // logged, counted and skipped.
  bool load_directory(const std::filesystem::path& dir, const std::filesystem::path& config_dir,
                      const SessionSettings& settings, std::string* error);

  // Dispatches the server-tick-start event to every package with a
  // registered tick handler. `ticks` is the number of ticks this batch
  // advanced; the plugin sees a cumulative tick count.
  void tick(std::uint64_t ticks);

  // Disable every running package, in reverse load order. Idempotent.
  void unload_all();

  std::size_t count() const;
  std::size_t failed_count() const;
  const std::vector<WasmPluginInfo>& plugins() const;

private:
  struct Loaded;
  std::vector<std::unique_ptr<Loaded>> plugins_;
  std::vector<WasmPluginInfo> info_;
  std::size_t failed_ = 0;
  std::uint64_t ticks_ = 0;
};

// The process-wide instance main.cpp wires into boot and shutdown.
WasmPlugins& wasm_plugins();

} // namespace mc::server
