// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 8, slice 2: the pinned WASM runtime behind the package gate.
//
// The runtime executes a `hostable` core module's lifecycle exports with four
// hard budgets — instructions, memory pages, call depth and wall-clock time —
// and a host interface that owns every effect a plugin can have. It is the
// engine's own MVP interpreter, pinned at ABI 1: no external runtime is
// fetched, so a build is reproducible and a module cannot reach a JIT that
// this repository does not control. `Host` is the seam a wasmtime backend can
// replace later without changing the plugin-facing ABI.
//
// The interpreter implements the WebAssembly MVP numeric/control/memory
// subset plus funcref tables, reference types, typed `select`, saturating
// conversions and bulk memory. A module that uses `externref`, SIMD, threads,
// GC or multi-value results is refused at load with the feature named, never
// half-executed. Full static type validation is not performed; the loader
// checks structure, indices and opcodes, and the machine traps on an invalid
// stack use instead of reading out of bounds.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mc::wasm {

// Pumpkin v0.2's `block` record: the static definition behind a block state.
// The canonical layout is 56 bytes, with the name and `state-ids` list allocated
// through the module's `cabi_realloc`. Fields +MC does not model are left at
// their neutral value by a host that cannot answer them.
struct BlockInfo {
  std::uint16_t id = 0; // Block registry id
  std::string name;     // "minecraft:stone"
  float hardness = 0.0f;
  float blast_resistance = 0.0f;
  std::uint8_t map_color = 0;
  float slipperiness = 0.6f;
  float velocity_multiplier = 1.0f;
  float jump_velocity_multiplier = 1.0f;
  std::uint16_t item_id = 0;
  std::uint16_t default_state_id = 0;
  std::vector<std::uint16_t> state_ids;
  bool is_solid = true;
  bool is_air = false;
  bool is_flammable = false;
  bool has_flammable = false;
  std::uint8_t spread_chance = 0;
  std::uint8_t burn_chance = 0;
};

// A Pumpkin `text-component` as the runtime carries it. Plain text is the
// common case; the style fields and click/hover data are only written when the
// component used the matching builder method. `plain()` is the text a host
// without rich-component support can still deliver.
struct TextComponent {
  std::string text;
  std::string color; // named ("gold", "dark_blue") or #rrggbb
  bool has_bold = false;
  bool bold = false;
  bool has_italic = false;
  bool italic = false;
  bool has_underlined = false;
  bool underlined = false;
  bool has_strikethrough = false;
  bool strikethrough = false;
  bool has_obfuscated = false;
  bool obfuscated = false;
  std::string insertion;
  std::string font;
  std::string click_action;
  std::string click_value;
  std::shared_ptr<TextComponent> hover_text;
  std::vector<TextComponent> children;

  std::string plain() const {
    std::string out = text;
    for (const TextComponent& child : children)
      out += child.plain();
    return out;
  }
};

// The live pose of one player, read under the registry lock so a plugin sees a
// consistent position/orientation pair. A host with no such player answers
// false rather than inventing coordinates for it.
struct PlayerPose {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  float yaw = 0.0f;
  float pitch = 0.0f;
};

// The live vitals of one player, read under the registry lock. `max_health`
// includes the player's active attribute modifiers; the other three are the
// same numbers the server's own set_health packet carries.
struct PlayerVitals {
  float max_health = 20.0f;
  std::uint8_t food_level = 20;
  float saturation = 5.0f;
  float exhaustion = 0.0f;
};

// The live experience values of one player, read under the registry lock.
struct PlayerExperience {
  std::int32_t level = 0;
  float progress = 0.0f;
  std::int32_t points = 0;
};

// Pumpkin v0.2's `permission` record. `default_case` is the
// `permission-default` variant (0 = deny, 1 = allow, 2 = op); `default_level`
// is the `permission-level` carried by `op`.
struct PermissionChild {
  std::string node;
  bool value = false;
};

struct PermissionGrant {
  std::string node;
  std::string description;
  std::int32_t default_case = 0;
  std::int32_t default_level = 0;
  std::vector<PermissionChild> children;
};

// Pumpkin v0.2's `argument-type` payload, resolved from the canonical-ABI
// joined form. `type` is the WIT case label (`integer`, `string`, ...). Numeric
// cases keep their exact-width bounds: `float`/`double` use the `float`/`double`
// fields, `integer`/`long` the integer fields and `time` its optional minimum.
// A resource case keeps the registry string it was built with.
struct CommandArgument {
  std::string type;
  std::optional<float> min_float;
  std::optional<float> max_float;
  std::optional<double> min_double;
  std::optional<double> max_double;
  std::optional<std::int32_t> min_integer;
  std::optional<std::int32_t> max_integer;
  std::optional<std::int64_t> min_long;
  std::optional<std::int64_t> max_long;
  std::optional<std::int32_t> string_type; // 0 single-word, 1 quotable, 2 greedy
  bool score_holder_multiple = false;
  std::optional<std::int32_t> time_min;
  std::string resource;
};

// One resolved command-node tree a plugin attached to a command. The tree is
// copied out of the resource handles at `context.register-command`, so the host
// receives values that outlive the guest's handles.
struct CommandNodeRegistration {
  enum class Kind { Literal, Argument };
  Kind kind = Kind::Literal;
  std::string name;
  CommandArgument argument;
  bool executable = false;
  std::optional<std::uint32_t> execute_handler_id;
  std::optional<std::uint32_t> suggest_handler_id;
  std::optional<std::uint32_t> require_handler_id;
  std::vector<CommandNodeRegistration> children;
};

// A command resource resolved by `context.register-command`. `names` keeps the
// primary name first and the aliases after it; `handler_id` is the id the
// plugin set through `command.execute-with-handler-id`; `nodes` is the resolved
// command-node tree, including argument payloads.
struct CommandRegistration {
  std::string name;
  std::vector<std::string> aliases;
  std::string description;
  std::string permission;
  std::uint32_t handler_id = 0;
  std::vector<CommandNodeRegistration> nodes;
};

// Pumpkin v0.2's `marketplace-metadata` record. A host that has not verified a
// signed plugin returns false, which the runtime lowers to `none`.
struct MarketplaceMetadata {
  std::string marketplace_url;
  std::int64_t plugin_id = 0;
  std::string plugin_name;
  std::string version;
  std::int64_t dev_id = 0;
  std::string dev_name;
  bool is_paid = false;
  std::int64_t user_id = 0;
  std::optional<std::string> license_key;
  std::string issued_at;
};

// Every effect a plugin can have goes through this interface. The CLI records
// the strings and keeps a small block/config map; a server implementation can
// map them onto its own world later without changing the runtime.
class Host {
public:
  virtual ~Host() = default;
  virtual void log(std::string_view text) = 0;
  // Pumpkin's `logging.log(level, message)`. Hosts without severity channels
  // keep working through the default, which drops the level.
  virtual void log_level(std::int32_t /*level*/, std::string_view text) {
    log(text);
  }
  // Pumpkin's `logging.log-tracing(event: list<u8>)`. The bytes are the
  // plugin's opaque trace payload, not necessarily UTF-8; hosts that have no
  // separate trace channel fall back to the ordinary log.
  virtual void log_tracing(std::string_view event) {
    log(event);
  }
  // `reply`, `player_message` and `broadcast` all arrive here; a server host
  // routes them, the CLI prints them.
  virtual void message(std::string_view text) = 0;
  // Pumpkin's `player.chat(message)`. The tracked WIT member is async, but the
  // host completes it synchronously; a host that has no per-player channel
  // falls back to the same message sink as broadcast.
  virtual void player_chat(std::string_view text) {
    message(text);
  }
  // Pumpkin's `server.broadcast(message)`: a system-chat line to every player.
  // The answer is the number of recipients, or a negative refusal.
  virtual std::int32_t broadcast(std::string_view text) {
    message(text);
    return 0;
  }
  // Resolve an exact player name for `server.get-player-by-name`. `canonical`
  // receives the registry's spelling; a host with no player registry returns
  // false and the component sees `none`.
  virtual bool find_player(std::string_view /*name*/, std::string* /*canonical*/) {
    return false;
  }
  // Pumpkin's `player.get-position`, `get-yaw` and `get-pitch`. The handle is
  // one the host issued, so a host resolves it against the same live registry
  // that answered `get-player-by-name`; false means the player is gone.
  virtual bool player_pose(std::string_view /*name*/, PlayerPose* /*out*/) {
    return false;
  }
  // Pumpkin's `player.get-gamemode`. The WIT enum lowers to a signed 32-bit
  // canonical value; false means the player is no longer in the live registry.
  virtual bool player_gamemode(std::string_view /*name*/, std::int32_t* /*out*/) {
    return false;
  }
  // Pumpkin's `player.get-health`. False means the player is no longer in the
  // live registry, so the handle is stale rather than reporting a default.
  virtual bool player_health(std::string_view /*name*/, float* /*out*/) {
    return false;
  }
  // Pumpkin's `player.get-absorption`. False means the player is no longer in
  // the live registry, so the handle is stale rather than reporting a default.
  virtual bool player_absorption(std::string_view /*name*/, float* /*out*/) {
    return false;
  }
  // Pumpkin's `player.get-max-health`, `get-food-level`, `get-saturation` and
  // `get-exhaustion`. False means the player is no longer in the live registry,
  // so the handle is stale rather than reporting default vitals.
  virtual bool player_vitals(std::string_view /*name*/, PlayerVitals* /*out*/) {
    return false;
  }
  // Pumpkin's `player.get-experience-level`, `get-experience-progress` and
  // `get-experience-points`. False means the player is no longer in the live
  // registry, so the handle is stale rather than reporting default XP.
  virtual bool player_experience(std::string_view /*name*/, PlayerExperience* /*out*/) {
    return false;
  }
  // Pumpkin's `player.get-id`. The two halves are returned in canonical UUID
  // order (the same layout `uuid.generate` and `uuid.to-string` use). False
  // means the player is no longer in the live registry.
  virtual bool player_uuid(std::string_view /*name*/, std::uint64_t* /*high*/,
                           std::uint64_t* /*low*/) {
    return false;
  }
  // Pumpkin's `player.get-camera-entity-id`. +MC never overrides a player's
  // camera, so the live snapshot's entity id is the camera target. False means
  // the handle is stale rather than reporting a fabricated entity.
  virtual bool player_camera_entity_id(std::string_view /*name*/, std::int32_t* /*out*/) {
    return false;
  }
  // Pumpkin's `player.send-system-message(text, overlay)`. `overlay` asks for
  // the action-bar channel; a host without one may still deliver the text as a
  // system-chat line, which is what the CLI host does.
  virtual bool send_system_message(std::string_view /*player*/, std::string_view text,
                                   bool /*overlay*/) {
    player_chat(text);
    return true;
  }
  // A rich component keeps its style tree; a host without that channel receives
  // the same content as plain text through the overload above.
  virtual bool send_system_message(std::string_view player, const TextComponent& text,
                                   bool overlay) {
    return send_system_message(player, text.plain(), overlay);
  }
  virtual std::int32_t player_count() {
    return 0;
  }
  // Pumpkin's `server.get-all-players`. The host fills the names of every
  // player currently in play; the runtime turns each into a typed handle.
  virtual void all_players(std::vector<std::string>* /*out*/) {}
  // Pumpkin's `server.get-player-count-in-world`. The host answers for a world
  // its registry knows; a host without per-world presence returns zero.
  virtual std::int32_t player_count_in_world(std::string_view /*world*/) {
    return 0;
  }
  // Pumpkin's `server.get-players-in-world`: the host fills the connected
  // players for a world it knows; a host without that world returns an empty
  // list rather than fabricating handles.
  virtual void players_in_world(std::string_view /*world*/,
                                std::vector<std::string>* /*out*/) {}
  // Pumpkin's `context.register-permission`. The default accepts a namespaced
  // node and refuses anything else with the error string the plugin sees; a
  // host that tracks permissions overrides this to store the grant.
  virtual bool register_permission(const PermissionGrant& permission, std::string* error) {
    if (permission.node.empty() || permission.node.find(':') == std::string::npos) {
      if (error != nullptr)
        *error = "permission node must be namespaced (namespace:node)";
      return false;
    }
    return true;
  }
  // Pumpkin's `context.register-command`. The command resource's handle has
  // already been resolved; this is the host's chance to publish it.
  virtual void register_command(const CommandRegistration& /*command*/) {}
  // Pumpkin's `context.get-data-folder`. The runtime gives a plugin no direct
  // filesystem, so the host names the folder it would hand this plugin. The
  // default is deterministic for a host with no per-plugin directory.
  virtual std::string data_folder() const {
    return ".";
  }
  // Pumpkin's `get-marketplace-metadata`. False is `none`: no verified
  // marketplace signature for this plugin.
  virtual bool marketplace_metadata(MarketplaceMetadata* /*out*/) {
    return false;
  }
  // Pumpkin's scalar `server` settings. A host without a configured server
  // answers the neutral value; the live host uses the startup SessionSettings.
  // The server's Message of the Day. The default matches the CLI host; a
  // live server overrides it with its startup SessionSettings.
  virtual std::string server_motd() const {
    return "A +MC server";
  }
  virtual std::int32_t server_max_players() {
    return 0;
  }
  virtual std::int32_t server_view_distance() {
    return 0;
  }
  virtual std::int32_t server_simulation_distance() {
    return 0;
  }
  virtual std::int32_t server_default_gamemode() {
    return 0;
  }
  virtual std::int32_t server_difficulty() {
    return 2;
  }
  virtual bool server_online_mode() {
    return true;
  }
  virtual bool server_whitelist_enabled() {
    return false;
  }
  virtual std::int32_t world_get_block(std::int32_t /*x*/, std::int32_t /*y*/, std::int32_t /*z*/) {
    return 0;
  }
  // The full Pumpkin `block` definition behind the state at a position. A host
  // without a block registry returns false; the runtime then hands the module
  // an air definition rather than inventing metadata for an unknown block.
  virtual bool world_get_block_info(std::int32_t /*x*/, std::int32_t /*y*/, std::int32_t /*z*/,
                                    BlockInfo* /*out*/) {
    return false;
  }
  virtual std::int32_t world_set_block(std::int32_t /*x*/, std::int32_t /*y*/, std::int32_t /*z*/,
                                       std::int32_t /*state*/) {
    return 0;
  }
  virtual std::int64_t world_time() {
    return 0;
  }
  // Pumpkin's `world.set-time-of-day(ticks)`. False means the host refused
  // the value (the live server refuses a negative tick).
  virtual bool world_set_time(std::int64_t /*ticks*/) {
    return false;
  }
  virtual std::int32_t world_weather() {
    return 0;
  }
  // Whether the host has a world with this name. The CLI host and the server
  // host both expose the single overworld; a host with more worlds overrides
  // this without changing the resource-handle contract.
  virtual bool has_world(std::string_view name) {
    return name == "minecraft:overworld";
  }
  virtual std::int32_t entity_find(std::string_view /*name*/) {
    return -1;
  }
  // The registry's maximum stack size for an item key. The CLI host has no
  // item registry and answers the common vanilla size; the live server host
  // overrides this with `world::ItemRegistry`. A zero answer means the host
  // does not know the key, and the runtime refuses to invent an item.
  virtual std::int32_t item_max_count(std::string_view /*registry_key*/) {
    return 64;
  }
  // Registry durability for the item, 0 when it has none. The live host reads
  // the generated item table; the CLI host uses the same table directly.
  virtual std::int32_t item_max_damage(std::string_view /*registry_key*/) {
    return 0;
  }
  virtual bool item_damageable(std::string_view /*registry_key*/) {
    return false;
  }
  virtual std::int32_t config_get(std::string_view /*key*/, std::string* /*value*/) {
    return -1;
  }
  // A record result over the config store: `{key, value}`. The default echoes
  // the requested key and delegates the lookup to config_get; a host that
  // canonicalizes keys may override it.
  virtual std::int32_t config_pair(std::string_view key, std::string* stored_key,
                                   std::string* value) {
    if (stored_key != nullptr) {
      stored_key->clear();
      if (!key.empty())
        stored_key->assign(key.data(), key.size());
    }
    return config_get(key, value);
  }
  virtual std::int32_t config_set(std::string_view /*key*/, std::string_view /*value*/) {
    return 0;
  }
  // Enumerates the config keys for `config_keys`. A host without enumeration
  // returns -1, which a component sees as an empty list.
  virtual std::int32_t config_keys(std::vector<std::string>* /*keys*/) {
    return -1;
  }
};

struct Limits {
  std::uint64_t max_steps = 1000000;
  std::uint32_t max_memory_pages = 256; // 16 MiB
  std::uint32_t max_call_depth = 64;
  std::uint64_t max_millis = 1000;
  // Bytes a plugin may hand the host through `log`, `reply`, `player_message`,
  // `broadcast` and `config_set`. Without it a module inside its step budget
  // could queue a gigabyte of text for the host to hold.
  std::uint64_t max_host_bytes = 1u << 20;
};

struct RunReport {
  bool ok = false;
  // The limits the run was held to, echoed so a report is self-contained.
  Limits limits;
  std::string error;
  std::string trap;
  bool budget_exhausted = false;
  std::string file;
  std::string sha256;
  std::uint64_t size = 0;
  // "module" for a core module, "component" when the run went
  // through the component layer.
  std::string kind = "module";
  std::uint64_t steps = 0;
  std::uint64_t millis = 0;
  std::uint32_t peak_pages = 0;
  std::size_t peak_depth = 0;
  bool ran_load = false;
  bool ran_enable = false;
  bool ran_disable = false;
  std::int32_t load_result = 0;
  std::int32_t enable_result = 0;
  std::int32_t disable_result = 0;
  std::vector<std::string> log;
  std::vector<std::string> messages;
  // A lifted string result, in lifecycle call order. A string parameter to a
  // lower is one-way; this is the return direction.
  std::vector<std::string> returns;
  // "name=value" for every numeric host call a component or module made, in
  // call order. A component's lifecycle exports are zero-argument, so this is
  // how a test observes calls whose results it does not return.
  std::vector<std::string> host_calls;
};

// A function to call on a core module held in memory. A component's nested
// core module is not a file, so the component adapter hands its bytes here
// instead of going through `run_module`. `function_index` is absolute in the
// module's function index space (imports first).
struct ExportCall {
  std::string name;
  std::uint32_t function_index = 0;
  // Pumpkin v0.2's lifecycle is an async export taking an owned `context`
  // handle. `async` means the core function returns no value and must call
  // `task.return`; `takes_handle` supplies the one i32 handle argument.
  bool async = false;
  bool takes_handle = false;
  // `init-plugin` is called for its effects and has no result to check.
  bool no_result = false;
  // A compiler may export an async lifecycle through the callback ABI, whose
  // core signature is `(context:i32) -> i32`. The compiled-core callback ABI
  // calls it synchronously and ignores that core status; task.return is still
  // used for the stackful v0.2 export.
  bool ignore_core_result = false;
  // A lifted UTF-8 string result. The core function returns one i32: a pointer
  // to a (pointer, length) return area in memory 0. The host copies the string
  // out and, when `has_post_return` is set, calls the module's post-return
  // function with that pointer.
  bool string_result = false;
  bool has_post_return = false;
  std::uint32_t post_return = 0;
};

// One core-module import a component's `canon lower` produced. `module` and
// `name` are the core import's module/field pair, `host_function` is the
// `plusmc:host` call it is bound to, and `signature` is the flattened
// canonical-ABI signature the linker checked against the module's declaration.
struct CoreImportBinding {
  std::string module;
  std::string name;
  std::string host_function;
  std::string signature;
  // A canonical lower whose result is a string, list<string>, two-string
  // record or Pumpkin `block`. The host allocates the return area and the
  // payload through the module's `realloc` function; `realloc` is an absolute
  // index in the core module's function index space and must name a function
  // the module defines. A list result uses the canonical (pointer, length)
  // array with an 8-byte pair per string; a record result uses two 8-byte pairs
  // in its return area; a block result is the 56-byte block layout.
  bool string_result = false;
  bool list_result = false;
  bool record_result = false;
  bool block_result = false;
  bool has_realloc = false;
  std::uint32_t realloc = 0;
  // A canon-lower with the async option. Its core signature ends in an i32
  // status and its host call returns a subtask instead of a synchronous value.
  bool async_lower = false;
};

// Instantiates a core module from bytes and calls `calls` in order. Each call
// must take no parameters and return nothing or one value; a non-zero answer
// refuses the module and stops the walk. The first three calls fill the
// report's load/enable/disable fields, so a caller may pass fewer.
RunReport run_core_in_memory(const std::vector<std::uint8_t>& bytes, const std::string& label,
                             const std::vector<ExportCall>& calls, const Limits& limits, Host& host,
                             const std::vector<CoreImportBinding>& bindings = {});

// Instantiates `path`, then calls `pmc_wasm_load(1)`, `pmc_wasm_enable()` and
// `pmc_wasm_disable()` in that order. A non-zero lifecycle result refuses the
// plugin; a trap or a budget stop ends the run with the reason. Each call gets
// `limits.max_steps` of its own; `report.steps` is the sum across calls.
RunReport run_module(const std::filesystem::path& path, const Limits& limits, Host& host);

// A live instance: parse and instantiate once, then drive the lifecycle exports
// as a server's own lifecycle runs. Memory, globals and the parsed module stay
// alive between calls, so `enable` can act on state `load` created. The server
// owns one of these per plugin; `run_module` is the one-shot form of the same
// machine. `create` hosts a core module; `create_component` hosts the linked
// v0.2 component shape. Both return nullptr with `*error` on a gate, parse or
// instantiation refusal. `start` runs the module's load/enable calls (or the
// component's `init-plugin` then `on-load`); `stop` runs disable/on-unload
// once. Each call gets a fresh `limits.max_millis` window; the step, memory,
// depth and host-byte budgets are cumulative for the instance's life.
class Instance {
public:
  ~Instance();
  Instance(Instance&&) noexcept;
  Instance& operator=(Instance&&) noexcept;
  Instance(const Instance&) = delete;
  Instance& operator=(const Instance&) = delete;

  static std::unique_ptr<Instance> create(const std::filesystem::path& path, const Limits& limits,
                                          Host& host, std::string* error);
  // The component form of `create`: the package must link through the hosted
  // canon-lift shape, including Pumpkin v0.2's async lifecycle when present.
  static std::unique_ptr<Instance> create_component(const std::filesystem::path& path,
                                                    const Limits& limits, Host& host,
                                                    std::string* error);
  bool start(std::string* error);
  bool stop(std::string* error);
  // Dispatches one `server-tick-start` event to a handler registered through
  // `context.register-event`. A plugin with no tick handler, or one that has
  // not started, is a successful no-op.
  bool tick(std::int32_t tick, std::string* error);
  const RunReport& report() const;

private:
  struct Impl;
  explicit Instance(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

std::string run_report_text(const RunReport& report);
std::string run_report_json(const RunReport& report);

} // namespace mc::wasm
