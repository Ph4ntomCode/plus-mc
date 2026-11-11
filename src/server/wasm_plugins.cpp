// SPDX-License-Identifier: GPL-3.0-or-later
//
// The live-session half of Phase 8: a `plusmc:host` implementation over the
// running server, and a loader that owns one persistent `mc::wasm::Instance`
// per package. The runtime executes the module; this file decides what its
// calls mean in a live world.

#include "server/wasm_plugins.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>

#include "core/log.h"
#include "net/byte_buffer.h"
#include "server/session.h"
#include "wasm/component.h"
#include "wasm/module.h"
#include "wasm/runtime.h"
#include "world/block_item_table.h"
#include "world/block_states.h"
#include "world/chunk.h"
#include "world/fire_table.h"
#include "world/item.h"
#include "world/registry.h"
#include "world/stats.h"
#include "world/tnt.h"

namespace mc::server {

namespace {

namespace fs = std::filesystem;

constexpr const char* kManifestName = "+mc-wasm.toml";
constexpr std::size_t kMaxConfigBytes = 64u * 1024u;

std::string trim(std::string text) {
  std::size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])))
    ++begin;
  std::size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
    --end;
  return text.substr(begin, end - begin);
}

std::string sanitize_name(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  for (char c : name) {
    const unsigned char byte = static_cast<unsigned char>(c);
    if (std::isalnum(byte) || c == '-' || c == '_' || c == '.')
      out.push_back(c);
    else
      out.push_back('-');
  }
  return out.empty() ? std::string("plugin") : out;
}

std::string quote_value(const std::string& value) {
  std::string out = "\"";
  for (char c : value) {
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    default:
      out.push_back(c);
      break;
    }
  }
  out += "\"";
  return out;
}

std::string unquote_value(std::string value) {
  if (value.size() < 2 || value.front() != '"' || value.back() != '"')
    return value;
  std::string out;
  out.reserve(value.size() - 2);
  for (std::size_t i = 1; i + 1 < value.size(); ++i) {
    if (value[i] == '\\' && i + 2 < value.size()) {
      const char next = value[++i];
      out.push_back(next == 'n' ? '\n' : next);
    } else {
      out.push_back(value[i]);
    }
  }
  return out;
}

// One plugin's key/value store, persisted as `<config-dir>/wasm/<name>.toml`.
// The format is deliberately the same line-oriented one the native plugin host
// writes (`key = "value"`), so an operator can edit it by hand.
class ConfigStore {
public:
  ConfigStore(std::string name, fs::path path) : name_(std::move(name)), path_(std::move(path)) {
    load();
  }

  std::int32_t get(std::string_view key, std::string* value) const {
    if (value == nullptr)
      return -1;
    const auto it = values_.find(std::string(key));
    if (it == values_.end())
      return -1;
    *value = it->second;
    return static_cast<std::int32_t>(value->size());
  }

  std::int32_t set(std::string_view key, std::string_view value) {
    if (key.empty())
      return 0;
    values_[std::string(key)] = std::string(value);
    return write() ? 1 : 0;
  }

  std::int32_t keys(std::vector<std::string>* out) const {
    if (out == nullptr)
      return -1;
    out->clear();
    for (const auto& entry : values_)
      out->push_back(entry.first);
    return static_cast<std::int32_t>(out->size());
  }

private:
  void load() {
    std::error_code ec;
    if (!fs::is_regular_file(path_, ec))
      return;
    const auto size = fs::file_size(path_, ec);
    if (ec || size > kMaxConfigBytes) {
      mc::log::warn("wasm: [", name_, "] ignoring an unreadable config ", path_.string());
      return;
    }
    std::ifstream in(path_);
    if (!in)
      return;
    std::string line;
    while (std::getline(in, line)) {
      line = trim(line);
      if (line.empty() || line.front() == '#')
        continue;
      const std::size_t equals = line.find('=');
      if (equals == std::string::npos)
        continue;
      const std::string key = trim(line.substr(0, equals));
      if (key.empty())
        continue;
      values_[key] = unquote_value(trim(line.substr(equals + 1)));
    }
  }

  bool write() const {
    std::error_code ec;
    if (!path_.parent_path().empty())
      fs::create_directories(path_.parent_path(), ec);
    fs::path temporary = path_;
    temporary += ".tmp";
    {
      std::ofstream out(temporary, std::ios::trunc);
      if (!out)
        return false;
      out << "# +MC WASM plugin config; rewritten by config_set (comments are lost).\n";
      for (const auto& [key, value] : values_)
        out << key << " = " << quote_value(value) << "\n";
      out.close();
      if (!out)
        return false;
    }
    fs::rename(temporary, path_, ec);
    if (ec) {
      fs::remove(path_, ec);
      fs::rename(temporary, path_, ec);
      if (ec) {
        fs::remove(temporary, ec);
        return false;
      }
    }
    return true;
  }

  std::string name_;
  fs::path path_;
  std::map<std::string, std::string> values_;
};

mc::net::ComponentNode text_node(const mc::wasm::TextComponent& text) {
  mc::net::ComponentNode node;
  node.text = text.text;
  node.color = text.color;
  node.has_bold = text.has_bold;
  node.bold = text.bold;
  node.has_italic = text.has_italic;
  node.italic = text.italic;
  node.has_underlined = text.has_underlined;
  node.underlined = text.underlined;
  node.has_strikethrough = text.has_strikethrough;
  node.strikethrough = text.strikethrough;
  node.has_obfuscated = text.has_obfuscated;
  node.obfuscated = text.obfuscated;
  node.insertion = text.insertion;
  node.font = text.font;
  node.click_action = text.click_action;
  node.click_value = text.click_value;
  if (text.hover_text)
    node.hover_text = std::make_shared<mc::net::ComponentNode>(text_node(*text.hover_text));
  for (const mc::wasm::TextComponent& child : text.children)
    node.extra.push_back(text_node(child));
  return node;
}

// The server's `plusmc:host`. Every method has a live engine behind it; the
// only contextual piece is `message`, which broadcasts when no event player is
// set (the lifecycle has no sender) and addresses that player when one is.
class ServerHost final : public mc::wasm::Host {
public:
  ServerHost(std::string name, fs::path config_path, const SessionSettings& settings)
      : name_(std::move(name)), data_folder_(config_path.parent_path() / config_path.stem()),
        config_(name_, std::move(config_path)), max_players_(settings.max_players),
        view_distance_(settings.view_distance), simulation_distance_(settings.simulation_distance),
        default_gamemode_(settings.gamemode), difficulty_(settings.difficulty),
        online_mode_(settings.online_mode), whitelist_enabled_(settings.whitelist_enabled),
        motd_(settings.motd) {}

  void log(std::string_view text) override {
    mc::log::info("wasm: [", name_, "] ", text);
  }

  void log_level(std::int32_t level, std::string_view text) override {
    if (level >= 4)
      mc::log::error("wasm: [", name_, "] ", text);
    else if (level == 3)
      mc::log::warn("wasm: [", name_, "] ", text);
    else
      mc::log::info("wasm: [", name_, "] ", text);
  }

  void message(std::string_view text) override {
    if (context_player_.empty()) {
      const int sent = plugin_broadcast_message(std::string(text));
      if (sent < 0)
        mc::log::warn("wasm: [", name_, "] broadcast refused (", sent, ")");
    } else {
      const MessageResult result = plugin_player_message(context_player_, std::string(text));
      if (result != MessageResult::Ok)
        mc::log::warn("wasm: [", name_, "] message to ", context_player_, " refused (",
                      static_cast<int>(result), ")");
    }
  }

  std::int32_t broadcast(std::string_view text) override {
    const int sent = plugin_broadcast_message(std::string(text));
    if (sent < 0)
      mc::log::warn("wasm: [", name_, "] broadcast refused (", sent, ")");
    return sent;
  }

  bool find_player(std::string_view name, std::string* canonical) override {
    PluginEntity entity;
    if (plugin_find_entity(std::string(name), &entity) != EntityResult::Ok ||
        entity.type != "minecraft:player")
      return false;
    if (canonical != nullptr)
      *canonical = entity.name;
    return true;
  }

  bool send_system_message(std::string_view player, std::string_view text, bool overlay) override {
    const MessageResult result =
        plugin_player_message(std::string(player), std::string(text), overlay);
    if (result != MessageResult::Ok)
      mc::log::warn("wasm: [", name_, "] system message to ", player, " refused (",
                    static_cast<int>(result), ")");
    return result == MessageResult::Ok;
  }

  bool send_system_message(std::string_view player, const mc::wasm::TextComponent& text,
                           bool overlay) override {
    const MessageResult result =
        plugin_player_message(std::string(player), text_node(text), overlay);
    if (result != MessageResult::Ok)
      mc::log::warn("wasm: [", name_, "] system message to ", player, " refused (",
                    static_cast<int>(result), ")");
    return result == MessageResult::Ok;
  }

  bool player_pose(std::string_view name, mc::wasm::PlayerPose* out) override {
    if (out == nullptr)
      return false;
    PlayerSnapshot snapshot;
    if (!lookup_player_snapshot(std::string(name), &snapshot))
      return false;
    out->x = snapshot.x;
    out->y = snapshot.y;
    out->z = snapshot.z;
    out->yaw = snapshot.yaw;
    out->pitch = snapshot.pitch;
    return true;
  }

  bool player_gamemode(std::string_view name, std::int32_t* out) override {
    if (out == nullptr)
      return false;
    PlayerSnapshot snapshot;
    if (!lookup_player_snapshot(std::string(name), &snapshot))
      return false;
    *out = snapshot.gamemode;
    return true;
  }

  bool player_health(std::string_view name, float* out) override {
    if (out == nullptr)
      return false;
    PlayerSnapshot snapshot;
    if (!lookup_player_snapshot(std::string(name), &snapshot))
      return false;
    *out = snapshot.health;
    return true;
  }

  bool player_absorption(std::string_view name, float* out) override {
    if (out == nullptr)
      return false;
    PlayerSnapshot snapshot;
    if (!lookup_player_snapshot(std::string(name), &snapshot))
      return false;
    *out = snapshot.absorption;
    return true;
  }

  bool player_vitals(std::string_view name, mc::wasm::PlayerVitals* out) override {
    if (out == nullptr)
      return false;
    PlayerSnapshot snapshot;
    if (!lookup_player_snapshot(std::string(name), &snapshot))
      return false;
    out->max_health = snapshot.max_health;
    out->food_level = snapshot.food_level;
    out->saturation = snapshot.saturation;
    out->exhaustion = snapshot.exhaustion;
    return true;
  }

  bool player_experience(std::string_view name, mc::wasm::PlayerExperience* out) override {
    if (out == nullptr)
      return false;
    PlayerSnapshot snapshot;
    if (!lookup_player_snapshot(std::string(name), &snapshot))
      return false;
    out->level = snapshot.experience_level;
    out->progress = snapshot.experience_progress;
    out->points = snapshot.experience_points;
    return true;
  }

  bool player_uuid(std::string_view name, std::uint64_t* high,
                   std::uint64_t* low) override {
    if (high == nullptr || low == nullptr)
      return false;
    PlayerSnapshot snapshot;
    if (!lookup_player_snapshot(std::string(name), &snapshot))
      return false;
    std::uint64_t high_bits = 0;
    std::uint64_t low_bits = 0;
    for (int i = 0; i < 8; ++i) {
      high_bits = (high_bits << 8) | snapshot.uuid[i];
      low_bits = (low_bits << 8) | snapshot.uuid[i + 8];
    }
    *high = high_bits;
    *low = low_bits;
    return true;
  }

  bool player_camera_entity_id(std::string_view name, std::int32_t* out) override {
    if (out == nullptr)
      return false;
    PlayerSnapshot snapshot;
    if (!lookup_player_snapshot(std::string(name), &snapshot))
      return false;
    *out = snapshot.entity_id;
    return true;
  }

  std::int32_t player_count() override {
    return online_player_count();
  }

  void all_players(std::vector<std::string>* out) override {
    if (out == nullptr)
      return;
    std::vector<PluginEntity> entities;
    if (!plugin_list_entities(&entities))
      return;
    for (const PluginEntity& entity : entities) {
      if (entity.type == "minecraft:player")
        out->push_back(entity.name);
    }
  }

  std::int32_t player_count_in_world(std::string_view world) override {
    return has_world(world) ? online_player_count() : 0;
  }

  void players_in_world(std::string_view world, std::vector<std::string>* out) override {
    if (out == nullptr || !has_world(world))
      return;
    all_players(out);
  }

  std::int32_t item_max_count(std::string_view registry_key) override {
    const auto id = mc::world::ItemRegistry::instance().id_of(registry_key);
    if (!id)
      return 0;
    return mc::world::ItemRegistry::instance().max_stack_size(*id);
  }

  std::int32_t item_max_damage(std::string_view registry_key) override {
    const auto id = mc::world::ItemRegistry::instance().id_of(registry_key);
    const mc::world::GeneratedItem* item =
        id ? mc::world::ItemRegistry::instance().by_id(*id) : nullptr;
    return item ? item->max_damage : 0;
  }

  bool item_damageable(std::string_view registry_key) override {
    const auto id = mc::world::ItemRegistry::instance().id_of(registry_key);
    const mc::world::GeneratedItem* item =
        id ? mc::world::ItemRegistry::instance().by_id(*id) : nullptr;
    return item != nullptr && (item->flags & mc::world::kItemIsDamageable) != 0;
  }

  std::string server_motd() const override {
    return motd_;
  }

  std::int32_t server_max_players() override {
    return max_players_;
  }
  std::int32_t server_view_distance() override {
    return view_distance_;
  }
  std::int32_t server_simulation_distance() override {
    return simulation_distance_;
  }
  std::int32_t server_default_gamemode() override {
    return default_gamemode_;
  }
  std::int32_t server_difficulty() override {
    return difficulty_;
  }
  bool server_online_mode() override {
    return online_mode_;
  }
  bool server_whitelist_enabled() override {
    return whitelist_enabled_;
  }

  std::int32_t world_get_block(std::int32_t x, std::int32_t y, std::int32_t z) override {
    return plugin_get_block(x, y, z);
  }

  bool world_get_block_info(std::int32_t x, std::int32_t y, std::int32_t z,
                            mc::wasm::BlockInfo* out) override {
    if (out == nullptr)
      return false;
    const std::int32_t state_id = plugin_get_block(x, y, z);
    if (state_id < 0)
      return false;
    const std::optional<mc::world::BlockStateKey> key = mc::world::block_state_by_id(state_id);
    if (!key)
      return false;

    *out = mc::wasm::BlockInfo{};
    out->name = key->name;
    out->default_state_id = static_cast<std::uint16_t>(state_id);
    out->is_air = key->name == "minecraft:air" || key->name == "minecraft:cave_air" ||
                  key->name == "minecraft:void_air";

    const std::int32_t block_id = mc::world::block_id_of_name(key->name);
    out->id = static_cast<std::uint16_t>(std::max(0, block_id));
    if (const auto default_state = mc::world::default_block_state(key->name))
      out->default_state_id = static_cast<std::uint16_t>(default_state->id);
    out->blast_resistance = mc::world::block_blast_resistance(key->name);
    out->is_solid = mc::world::block_blocks_motion(key->name);

    static const std::unordered_map<std::string, std::vector<std::uint16_t>> kStateIds = [] {
      std::unordered_map<std::string, std::vector<std::uint16_t>> by_name;
      for (const mc::world::GeneratedBlockState& state : mc::world::kBlockStates)
        by_name[state.name].push_back(static_cast<std::uint16_t>(state.id));
      return by_name;
    }();
    const auto state_ids = kStateIds.find(key->name);
    if (state_ids != kStateIds.end())
      out->state_ids = state_ids->second;

    // The fire table is sorted by block name and carries the same two odds
    // Pumpkin's `flammable` record does.
    const auto fire = std::lower_bound(
        std::begin(mc::world::kFireFlammability), std::end(mc::world::kFireFlammability), key->name,
        [](const mc::world::FireFlammability& row, const std::string& name) {
          return std::string_view(row.name) < name;
        });
    if (fire != std::end(mc::world::kFireFlammability) && key->name == fire->name) {
      out->is_flammable = fire->ignite_odds > 0 || fire->burn_odds > 0;
      out->has_flammable = out->is_flammable;
      out->spread_chance = static_cast<std::uint8_t>(std::clamp<int>(fire->ignite_odds, 0, 255));
      out->burn_chance = static_cast<std::uint8_t>(std::clamp<int>(fire->burn_odds, 0, 255));
    }

    // A block item is the item whose placement table names this block. Most
    // blocks have one; a block with no item (fire, a crop's body) answers 0.
    for (const mc::world::GeneratedBlockItem& entry : mc::world::kBlockItems) {
      if (key->name != entry.block)
        continue;
      if (const auto item = mc::world::ItemRegistry::instance().id_of(entry.item))
        out->item_id = static_cast<std::uint16_t>(*item);
      break;
    }

    // +MC's block-state table has no static hardness, map colour or movement
    // multipliers. Those fields keep the neutral values in BlockInfo (0, 0,
    // 0.6, 1, 1); the rest of the record is the live registry's answer.
    return true;
  }

  std::int32_t world_set_block(std::int32_t x, std::int32_t y, std::int32_t z,
                               std::int32_t state) override {
    const std::optional<mc::world::BlockStateKey> key = mc::world::block_state_by_id(state);
    if (!key)
      return 0;
    std::string token = key->name;
    if (!key->properties.empty())
      token += "[" + key->properties + "]";
    return plugin_set_block(x, y, z, token) == BlockWriteResult::Ok ? 1 : 0;
  }

  std::int64_t world_time() override {
    return world_time_ticks();
  }
  bool world_set_time(std::int64_t ticks) override {
    return plugin_set_world_time(ticks);
  }
  std::int32_t world_weather() override {
    return plugin_world_weather();
  }

  std::int32_t entity_find(std::string_view name) override {
    PluginEntity entity;
    if (plugin_find_entity(std::string(name), &entity) != EntityResult::Ok)
      return -1;
    return entity.entity_id;
  }

  std::string data_folder() const override {
    return data_folder_.string();
  }

  std::int32_t config_get(std::string_view key, std::string* value) override {
    return config_.get(key, value);
  }
  std::int32_t config_set(std::string_view key, std::string_view value) override {
    return config_.set(key, value);
  }
  std::int32_t config_keys(std::vector<std::string>* keys) override {
    return config_.keys(keys);
  }

  void set_context_player(std::string player) {
    context_player_ = std::move(player);
  }

private:
  std::string name_;
  fs::path data_folder_;
  ConfigStore config_;
  std::string context_player_;
  std::string motd_;
  std::int32_t max_players_ = 0;
  std::int32_t view_distance_ = 0;
  std::int32_t simulation_distance_ = 0;
  std::int32_t default_gamemode_ = 0;
  std::int32_t difficulty_ = 2;
  bool online_mode_ = true;
  bool whitelist_enabled_ = false;
};

mc::wasm::Limits server_limits() {
  mc::wasm::Limits limits;
  limits.max_steps = 10'000'000;
  limits.max_memory_pages = 256; // 16 MiB
  limits.max_call_depth = 64;
  limits.max_millis = 2000;
  limits.max_host_bytes = 4u * 1024u * 1024u;
  return limits;
}

std::string refusal_reasons(const mc::wasm::PackageReport& gate) {
  std::string reason;
  for (const std::string& entry : gate.reasons)
    reason += reason.empty() ? entry : "; " + entry;
  return reason.empty() ? std::string("not hostable") : reason;
}

} // namespace

struct WasmPlugins::Loaded {
  std::string name;
  std::string version;
  std::string module;
  std::unique_ptr<ServerHost> host;
  std::unique_ptr<mc::wasm::Instance> instance;
};

WasmPlugins::WasmPlugins() = default;
WasmPlugins::~WasmPlugins() {
  unload_all();
}

bool WasmPlugins::load_directory(const fs::path& dir, const fs::path& config_dir,
                                 const SessionSettings& settings, std::string* error) {
  if (!plugins_.empty()) {
    *error = "wasm plugins are already loaded";
    return false;
  }
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    *error = "--wasm-plugins " + dir.string() + " is not a directory";
    return false;
  }

  std::vector<fs::path> packages;
  if (fs::is_regular_file(dir / kManifestName, ec)) {
    packages.push_back(dir);
  } else {
    fs::directory_iterator it(dir, ec);
    if (ec) {
      *error = "--wasm-plugins " + dir.string() + ": " + ec.message();
      return false;
    }
    for (const fs::directory_entry& entry : it) {
      std::error_code type_error;
      if (entry.is_directory(type_error) &&
          fs::is_regular_file(entry.path() / kManifestName, type_error))
        packages.push_back(entry.path());
    }
    std::sort(packages.begin(), packages.end());
  }
  if (packages.empty()) {
    mc::log::info("wasm: no packages in ", dir.string());
    return true;
  }

  for (const fs::path& package : packages) {
    const mc::wasm::PackageReport gate = mc::wasm::inspect_wasm(package);
    if (!gate.ok) {
      mc::log::error("wasm: ", gate.error);
      ++failed_;
      continue;
    }
    const std::string name =
        gate.package.name.empty() ? package.stem().string() : gate.package.name;
    if (gate.status != "component" && gate.status != "hostable") {
      mc::log::warn("wasm: ", name, " refused: ", refusal_reasons(gate));
      ++failed_;
      continue;
    }
    if (gate.status == "hostable" &&
        (gate.module.table_count != 0 || gate.module.memory_count > 1)) {
      mc::log::warn("wasm: ", name, " refused: tables and multiple memories are not hosted");
      ++failed_;
      continue;
    }

    auto host = std::make_unique<ServerHost>(
        name, config_dir / "wasm" / (sanitize_name(name) + ".toml"), settings);
    std::string runtime_error;
    std::unique_ptr<mc::wasm::Instance> instance =
        gate.status == "component"
            ? mc::wasm::Instance::create_component(package, server_limits(), *host, &runtime_error)
            : mc::wasm::Instance::create(package, server_limits(), *host, &runtime_error);
    if (!instance) {
      mc::log::error("wasm: ", name, " refused: ", runtime_error);
      ++failed_;
      continue;
    }
    if (!instance->start(&runtime_error)) {
      mc::log::error("wasm: ", name, " failed to start: ", runtime_error);
      ++failed_;
      continue;
    }
    const mc::wasm::RunReport& report = instance->report();
    mc::log::info("wasm: loaded ", name, " ", gate.package.version,
                  gate.status == "component" ? " (component)" : "", " (", report.steps, " steps, ",
                  report.millis, " ms)");

    auto loaded = std::make_unique<Loaded>();
    loaded->name = name;
    loaded->version = gate.package.version;
    loaded->module = gate.module.file;
    loaded->host = std::move(host);
    loaded->instance = std::move(instance);
    plugins_.push_back(std::move(loaded));
    info_.push_back(WasmPluginInfo{name, gate.package.version, gate.module.file, true, report.steps,
                                   report.millis});
  }
  return true;
}

void WasmPlugins::tick(std::uint64_t ticks) {
  if (ticks == 0)
    return;
  ticks_ += ticks;
  for (const auto& plugin : plugins_) {
    std::string error;
    if (!plugin->instance->tick(static_cast<std::int32_t>(ticks_), &error))
      mc::log::warn("wasm: ", plugin->name, " tick handler failed: ", error);
  }
}

void WasmPlugins::unload_all() {
  for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
    Loaded& plugin = **it;
    std::string error;
    if (!plugin.instance->stop(&error))
      mc::log::warn("wasm: ", plugin.name, " failed to stop: ", error);
    else
      mc::log::info("wasm: unloaded ", plugin.name);
  }
  plugins_.clear();
  info_.clear();
}

std::size_t WasmPlugins::count() const {
  return plugins_.size();
}

std::size_t WasmPlugins::failed_count() const {
  return failed_;
}

const std::vector<WasmPluginInfo>& WasmPlugins::plugins() const {
  return info_;
}

WasmPlugins& wasm_plugins() {
  static WasmPlugins plugins;
  return plugins;
}

} // namespace mc::server
