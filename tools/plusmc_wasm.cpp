// SPDX-License-Identifier: GPL-3.0-or-later
//
// `plusmc-wasm` is the Phase 8 tool: `inspect` is the package gate (slice 1),
// `run` is the pinned runtime (slice 2). `inspect` reads a `.wasm` module (or a
// directory holding `+mc-wasm.toml` and the module it names) and reports
// whether the pinned host surface can satisfy every import; `run` goes on to
// instantiate a hostable module and call its lifecycle exports under the
// budgets the flags set. Neither command ever links a module against anything
// but `plusmc:host`.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "wasm/component_run.h"
#include "wasm/module.h"
#include "wasm/runtime.h"
#include "world/item_registry.h"

namespace {

// The CLI's ceilings for a run. They are not the sandbox's limits (the runtime
// holds the module to whatever Limits says); they are the range a host should
// be able to ask for by hand, so a typo cannot turn one command into an
// unbounded run.
constexpr std::uint64_t kMaxStepsCeiling = 1000000000ull;
constexpr std::uint64_t kMaxMillisCeiling = 3600000ull;
constexpr std::uint64_t kMaxDepthCeiling = 100000ull;
constexpr std::uint64_t kMaxPagesCeiling = 65536ull;

void usage() {
  std::cout << "usage:\n"
            << "  plusmc-wasm inspect <module.wasm | package-dir> [--json]\n"
            << "                      [--require-hostable]\n"
            << "  plusmc-wasm run <module.wasm | package-dir> [--json]\n"
            << "                 [--max-steps N] [--max-millis N] [--max-call-depth N]\n"
            << "                 [--max-memory-pages N] [--host-bytes N]\n"
            << "                 [--host-player NAME]\n";
}

// The CLI host keeps the smallest state a run needs to be observable: the
// config map a plugin writes and reads back, and the blocks it writes. Reads
// the CLI cannot know (the world clock, the weather, a player count) answer
// zero, and `entity_find` answers -1 (no such entity), which is what the
// report documents so a plugin test is not asserting a server's state.
class CliHost final : public mc::wasm::Host {
public:
  explicit CliHost(std::string host_player = {}) : host_player_(std::move(host_player)) {}

  void log(std::string_view) override {}
  void message(std::string_view) override {}

  std::int32_t broadcast(std::string_view /*text*/) override {
    return host_player_.empty() ? 0 : 1;
  }

  bool find_player(std::string_view name, std::string* canonical) override {
    if (host_player_.empty() || name != host_player_)
      return false;
    if (canonical != nullptr)
      *canonical = host_player_;
    return true;
  }

  bool send_system_message(std::string_view player, std::string_view /*text*/,
                           bool /*overlay*/) override {
    return !host_player_.empty() && player == host_player_;
  }

  bool player_pose(std::string_view player, mc::wasm::PlayerPose* out) override {
    if (host_player_.empty() || player != host_player_ || out == nullptr)
      return false;
    // The CLI has no live world. These values are a deterministic host-player
    // pose for exercising the geometry lowering, not a fabricated server read.
    // The camera entity id follows the same rule: it is the host-player's own
    // entity id, never a guest-supplied handle.
    *out = mc::wasm::PlayerPose{12.0, 64.0, -3.0, 90.0f, -15.0f};
    return true;
  }

  bool player_gamemode(std::string_view player, std::int32_t* out) override {
    if (host_player_.empty() || player != host_player_ || out == nullptr)
      return false;
    // Survival is the CLI host-player's deterministic read for this adapter.
    *out = 0;
    return true;
  }

  bool player_health(std::string_view player, float* out) override {
    if (host_player_.empty() || player != host_player_ || out == nullptr)
      return false;
    *out = 20.0f;
    return true;
  }

  bool player_absorption(std::string_view player, float* out) override {
    if (host_player_.empty() || player != host_player_ || out == nullptr)
      return false;
    // A deterministic two absorption hearts for the host-player adapter run.
    *out = 2.0f;
    return true;
  }

  bool player_vitals(std::string_view player, mc::wasm::PlayerVitals* out) override {
    if (host_player_.empty() || player != host_player_ || out == nullptr)
      return false;
    // Deterministic host-player vitals for the adapter's scalar reads.
    *out = mc::wasm::PlayerVitals{20.0f, 18, 4.0f, 1.0f};
    return true;
  }

  bool player_experience(std::string_view player, mc::wasm::PlayerExperience* out) override {
    if (host_player_.empty() || player != host_player_ || out == nullptr)
      return false;
    // Deterministic host-player experience for the adapter's scalar reads.
    *out = mc::wasm::PlayerExperience{30, 1.0f, 1234};
    return true;
  }

  bool player_uuid(std::string_view player, std::uint64_t* high, std::uint64_t* low) override {
    if (host_player_.empty() || player != host_player_ || high == nullptr || low == nullptr)
      return false;
    // Deterministic host-player UUID for the adapter's UUID record path.
    *high = 0x0011223344556677ULL;
    *low = 0x8899aabbccddeeffULL;
    return true;
  }

  bool player_camera_entity_id(std::string_view player, std::int32_t* out) override {
    if (host_player_.empty() || player != host_player_ || out == nullptr)
      return false;
    // +MC never overrides the camera, so the CLI host-player's camera is its
    // deterministic entity id.
    *out = 42;
    return true;
  }

  std::int32_t player_count() override {
    return host_player_.empty() ? 0 : 1;
  }

  void all_players(std::vector<std::string>* out) override {
    if (out != nullptr && !host_player_.empty())
      out->push_back(host_player_);
  }

  std::int32_t player_count_in_world(std::string_view world) override {
    return has_world(world) ? player_count() : 0;
  }

  void players_in_world(std::string_view world, std::vector<std::string>* out) override {
    if (out == nullptr || !has_world(world))
      return;
    all_players(out);
  }

  static const mc::world::GeneratedItem* find_item(std::string_view registry_key) {
    for (const mc::world::GeneratedItem& item : mc::world::kItems) {
      if (registry_key == item.name)
        return &item;
    }
    return nullptr;
  }

  std::int32_t item_max_count(std::string_view registry_key) override {
    const mc::world::GeneratedItem* item = find_item(registry_key);
    return item ? item->max_stack_size : 0;
  }
  std::int32_t item_max_damage(std::string_view registry_key) override {
    const mc::world::GeneratedItem* item = find_item(registry_key);
    return item ? item->max_damage : 0;
  }
  bool item_damageable(std::string_view registry_key) override {
    const mc::world::GeneratedItem* item = find_item(registry_key);
    return item != nullptr && (item->flags & mc::world::kItemIsDamageable) != 0;
  }

  // The CLI has no server.properties, so these mirror the defaults main.cpp
  // uses for the live server. They are still real reads of the host's chosen
  // settings, not guesses made by the component linker.
  std::int32_t server_max_players() override {
    return 20;
  }
  std::int32_t server_view_distance() override {
    return 8;
  }
  std::int32_t server_simulation_distance() override {
    return 8;
  }
  std::int32_t server_default_gamemode() override {
    return 0;
  }
  std::int32_t server_difficulty() override {
    return 2;
  }
  bool server_online_mode() override {
    return true;
  }
  bool server_whitelist_enabled() override {
    return false;
  }

  bool marketplace_metadata(mc::wasm::MarketplaceMetadata* out) override {
    if (out == nullptr)
      return false;
    out->marketplace_url = "https://marketplace.pumpkinmc.org";
    out->plugin_id = 4242;
    out->plugin_name = "compiled-plugin";
    out->version = "0.2.0";
    out->dev_id = 7;
    out->dev_name = "Buffy";
    out->is_paid = false;
    out->user_id = 0;
    out->license_key.reset();
    out->issued_at = "2026-11-08T00:00:00Z";
    return true;
  }

  std::string data_folder() const override {
    return "plusmc-wasm-data";
  }

  std::int32_t world_get_block(std::int32_t x, std::int32_t y, std::int32_t z) override {
    const auto it =
        blocks_.find(std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z));
    return it == blocks_.end() ? 0 : it->second;
  }

  bool world_get_block_info(std::int32_t x, std::int32_t y, std::int32_t z,
                            mc::wasm::BlockInfo* out) override {
    if (out == nullptr)
      return false;
    const std::int32_t state = world_get_block(x, y, z);
    // The CLI's world is a state map, not a block registry. Air and stone are
    // the two definitions its fixture uses; the server host owns the full
    // registry-backed answer.
    if (state == 0) {
      *out = mc::wasm::BlockInfo{};
      out->name = "minecraft:air";
      out->item_id = 0;
      out->default_state_id = 0;
      out->state_ids = {0};
      out->is_solid = false;
      out->is_air = true;
      return true;
    }
    if (state == 1) {
      *out = mc::wasm::BlockInfo{};
      out->id = 1;
      out->name = "minecraft:stone";
      out->hardness = 1.5f;
      out->blast_resistance = 6.0f;
      out->map_color = 11;
      out->slipperiness = 0.6f;
      out->velocity_multiplier = 1.0f;
      out->jump_velocity_multiplier = 1.0f;
      out->item_id = 1;
      out->default_state_id = 1;
      out->state_ids = {1};
      out->is_solid = true;
      out->is_air = false;
      return true;
    }
    return false;
  }

  std::int32_t world_set_block(std::int32_t x, std::int32_t y, std::int32_t z,
                               std::int32_t state) override {
    blocks_[std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z)] = state;
    return 1;
  }

  std::int64_t world_time() override {
    return world_time_;
  }
  bool world_set_time(std::int64_t ticks) override {
    if (ticks < 0)
      return false;
    world_time_ = ticks;
    return true;
  }
  std::int32_t world_weather() override {
    return 0;
  }
  std::int32_t entity_find(std::string_view) override {
    return -1;
  }

  std::int32_t config_get(std::string_view key, std::string* value) override {
    const auto it = config_.find(std::string(key));
    if (it == config_.end())
      return -1;
    *value = it->second;
    return static_cast<std::int32_t>(value->size());
  }

  std::int32_t config_set(std::string_view key, std::string_view value) override {
    config_[std::string(key)] = std::string(value);
    return 1;
  }

  std::int32_t config_keys(std::vector<std::string>* keys) override {
    if (keys == nullptr)
      return -1;
    keys->clear();
    for (const auto& entry : config_)
      keys->push_back(entry.first);
    return static_cast<std::int32_t>(keys->size());
  }

private:
  std::string host_player_;
  std::map<std::string, std::string> config_;
  std::map<std::string, std::int32_t> blocks_;
  std::int64_t world_time_ = 0;
};

bool parse_number(const std::string& text, std::uint64_t* out) {
  if (text.empty())
    return false;
  for (const char c : text) {
    if (c < '0' || c > '9')
      return false;
  }
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0')
    return false;
  *out = value;
  return true;
}

bool parse_budget(const std::string& flag, const std::string& text, std::uint64_t ceiling,
                  std::uint64_t* out) {
  if (!parse_number(text, out)) {
    std::cerr << "plusmc-wasm: " << flag << " needs a number\n";
    return false;
  }
  if (*out > ceiling) {
    std::cerr << "plusmc-wasm: " << flag << " " << text << " is out of range (at most " << ceiling
              << ")\n";
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    usage();
    return 2;
  }
  const std::string command = argv[1];
  if (command != "inspect" && command != "run") {
    usage();
    return 2;
  }

  std::string target;
  bool json = false;
  bool require_hostable = false;
  std::string host_player;
  mc::wasm::Limits limits;
  for (int i = 2; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--json") {
      json = true;
    } else if (argument == "--require-hostable") {
      require_hostable = true;
    } else if (argument == "--host-player") {
      if (i + 1 >= argc || argv[i + 1][0] == '\0') {
        std::cerr << "plusmc-wasm: --host-player needs a name\n";
        return 2;
      }
      host_player = argv[++i];
    } else if (argument == "--help" || argument == "-h") {
      usage();
      return 0;
    } else if (!argument.empty() && argument.front() == '-') {
      const bool known = argument == "--max-steps" || argument == "--max-millis" ||
                         argument == "--max-call-depth" || argument == "--max-memory-pages" ||
                         argument == "--host-bytes";
      if (!known) {
        std::cerr << "plusmc-wasm: unknown option " << argument << "\n";
        return 2;
      }
      if (i + 1 >= argc) {
        std::cerr << "plusmc-wasm: " << argument << " needs a number\n";
        return 2;
      }
      const std::string value = argv[++i];
      std::uint64_t parsed = 0;
      std::uint64_t ceiling = kMaxStepsCeiling;
      if (argument == "--max-millis") {
        ceiling = kMaxMillisCeiling;
      } else if (argument == "--max-call-depth") {
        ceiling = kMaxDepthCeiling;
      } else if (argument == "--max-memory-pages") {
        ceiling = kMaxPagesCeiling;
      } else if (argument == "--host-bytes") {
        ceiling = 0xffffffffull;
      }
      if (!parse_budget(argument, value, ceiling, &parsed))
        return 2;
      if (argument == "--max-steps") {
        limits.max_steps = parsed;
      } else if (argument == "--max-millis") {
        limits.max_millis = parsed;
      } else if (argument == "--max-call-depth") {
        limits.max_call_depth = static_cast<std::uint32_t>(parsed);
      } else if (argument == "--max-memory-pages") {
        limits.max_memory_pages = static_cast<std::uint32_t>(parsed);
      } else {
        limits.max_host_bytes = parsed;
      }
    } else if (target.empty()) {
      target = argument;
    } else {
      std::cerr << "plusmc-wasm: this command takes one module or package\n";
      return 2;
    }
  }
  if (target.empty()) {
    usage();
    return 2;
  }

  if (command == "inspect") {
    const mc::wasm::PackageReport report = mc::wasm::inspect_wasm(target);
    std::cout << (json ? mc::wasm::package_json(report) : mc::wasm::package_text(report));
    if (!report.ok) {
      std::cerr << "plusmc-wasm: " << report.error << "\n";
      return 1;
    }
    if (require_hostable && report.status != "hostable") {
      std::cerr << "plusmc-wasm: " << target << " is " << report.status << ", not hostable\n";
      return 1;
    }
    return 0;
  }

  CliHost host(host_player);
  // A component in the hosted shape is driven through its canon lifts; a
  // core module (or a package around one) goes through `run_module`.
  const mc::wasm::PackageReport gate = mc::wasm::inspect_wasm(target);
  const mc::wasm::RunReport report = gate.ok && gate.status == "component"
                                         ? mc::wasm::run_component(target, limits, host)
                                         : mc::wasm::run_module(target, limits, host);
  std::cout << (json ? mc::wasm::run_report_json(report) : mc::wasm::run_report_text(report));
  if (!report.ok) {
    std::cerr << "plusmc-wasm: " << target << " " << (report.error.empty() ? "trapped" : "refused")
              << ": " << (report.error.empty() ? report.trap : report.error) << "\n";
    // A JSON report is the result: the caller reads `ok`/`status` from the
    // document, so the process exits 0. Text mode keeps the exit code for
    // shells and CI.
    return json ? 0 : 1;
  }
  return 0;
}
