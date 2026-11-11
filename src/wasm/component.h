// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 8, slice 3: the component adapter.
//
// A Pumpkin plugin is a `wasm32-wasip2` component, not a core module. Its
// imports are WIT interfaces (`pumpkin:plugin/logging@0.2.0`, ...) encoded in
// the component layer, where the slice-1 gate only checks a section table.
// This file reads that layer with the same bounded discipline and answers the
// question an operator has before a runtime is asked to host one:
//
//   * which interfaces does this component import, and does each tracked member
//     have a `plusmc:host` call with a compatible shape?
//   * does it export the lifecycle callback the host drives (`on-load`)?
//
// The table transcribes `pumpkin-plugin-wit` v0.2 as it is written, not a
// guessed Pumpkin API: `log(level, message)`, resource methods with an implicit
// `self`, `async` setters and `text-component` arguments are recorded as such.
//
// `mapped` is an interface verdict, not a run. The canonical-ABI shim that
// turns a WIT interface into the host's C calls, and the runtime that executes
// the module, are the other half of slice 3: nothing here executes anything.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace mc::wasm {

// One WIT member the adapter's table knows about. `member` is the instance
// export name (`[method]server.broadcast` for a resource method); `signature`
// is the declaration as `pumpkin-plugin-wit` v0.2 writes it; `host` is the
// function in `plusmc:host` that can back it, or empty when no host call has a
// compatible shape. `method` marks a resource method, which carries an
// implicit `self` handle the host has to resolve.
struct PinnedMemberRef {
  const char* member;
  const char* signature;
  const char* host;
  bool method;
};

// One tracked Pumpkin v0.2 interface. The table is a subset of Pumpkin's WIT:
// only members this host can either back or deliberately record as unbacked are
// listed, and unlisted members are not assessed. `coverage: full` therefore
// means every listed member of every imported interface is backed, not that the
// interface is complete. A type-only interface has no members to call: tracking
// it records that its value definitions are recognised and needs no host
// semantics. Every callable interface backs every member the table lists;
// unlisted members are outside this tracked subset.
struct PinnedInterfaceRef {
  const char* name;
  const PinnedMemberRef* members;
  std::size_t member_count;
};

// The tracked Pumpkin v0.2 surface, transcribed from `pumpkin-plugin-wit` v0.2
// (`log.wit`, `server.wit`, `world.wit`, `player.wit`, `context.wit`,
// `uuid.wit`, `i18n.wit`, `item-stack.wit` and the package's type-only
// interfaces). `signature` is the real
// WIT declaration, not a shape this host invented: a `log` that takes only a
// string is not Pumpkin's `log(level, message)`, and pretending otherwise would
// make the adapter's coverage a fiction. `host` is set for every member listed
// below, including resource methods with their implicit `self` handle, the typed
// player/text handles, `server.get-player-by-name`'s option result and the plain
// text `text-component` members. The async `player.chat`, `world.set-block` and
// `server.broadcast` members accept either a synchronous canonical lowering or
// the async ABI; the runtime completes their one-shot host work behind a subtask
// and delivers it through waitable-set.join/wait. The checked-in cargo-component
// artifact uses synchronous wrappers because its generator emits them. `log-tracing`'s `list<u8>`
// and the full `block` result are backed through their canonical memory/record forms. The real WIT
// still has many more members and interfaces than this tracked subset.
//
// The table is inline so the adapter and the component linker share one copy:
// the adapter reports what the host can back, and the linker lowers exactly
// those members.
inline constexpr PinnedMemberRef kLoggingMembers[] = {
    {"log", "func(level: level, message: string)", "log_level", false},
    {"log-tracing", "func(event: list<u8>)", "log_tracing", false},
};

inline constexpr PinnedMemberRef kServerMembers[] = {
    {"[method]server.get-difficulty", "func() -> difficulty", "server_difficulty", true},
    {"[method]server.get-motd", "func() -> string", "server_motd", true},
    {"[method]server.get-player-count", "func() -> u32", "player_count", true},
    {"[method]server.get-all-players", "func() -> list<player>", "server_all_players", true},
    {"[method]server.get-player-count-in-world", "func(world-ref: %world) -> u32",
     "server_player_count_in_world", true},
    {"[method]server.get-players-in-world", "func(world-ref: %world) -> list<player>",
     "server_players_in_world", true},
    {"[method]server.get-max-players", "func() -> u32", "server_max_players", true},
    {"[method]server.is-online-mode", "func() -> bool", "server_online_mode", true},
    {"[method]server.has-whitelist", "func() -> bool", "server_whitelist_enabled", true},
    {"[method]server.get-view-distance", "func() -> u8", "server_view_distance", true},
    {"[method]server.get-simulation-distance", "func() -> u8", "server_simulation_distance", true},
    {"[method]server.get-default-gamemode", "func() -> game-mode", "server_default_gamemode", true},
    {"[method]server.get-player-by-name", "func(name: string) -> option<player>",
     "server_player_by_name", true},
    {"[method]server.get-world-by-name", "func(name: string) -> option<%world>",
     "pumpkin_server_world_by_name", true},
    {"[method]server.has-world", "func(name: string) -> bool", "server_has_world", true},
    {"[method]server.broadcast", "async func(message: string)", "server_broadcast", true},
};

inline constexpr PinnedMemberRef kTextMembers[] = {
    {"[static]text-component.text", "static func(plain: string) -> text-component", "text_text",
     false},
    {"[method]text-component.add-text", "func(text: string)", "text_add_text", true},
    {"[method]text-component.add-child", "func(child: text-component)", "text_add_child", true},
    {"[method]text-component.color-named", "func(color: named-color)", "text_color_named", true},
    {"[method]text-component.color-rgb", "func(color: rgb-color)", "text_color_rgb", true},
    {"[method]text-component.bold", "func(value: bool)", "text_bold", true},
    {"[method]text-component.italic", "func(value: bool)", "text_italic", true},
    {"[method]text-component.underlined", "func(value: bool)", "text_underlined", true},
    {"[method]text-component.strikethrough", "func(value: bool)", "text_strikethrough", true},
    {"[method]text-component.obfuscated", "func(value: bool)", "text_obfuscated", true},
    {"[method]text-component.insertion", "func(text: string)", "text_insertion", true},
    {"[method]text-component.font", "func(font: string)", "text_font", true},
    {"[method]text-component.click-open-url", "func(url: string)", "text_click_open_url", true},
    {"[method]text-component.click-open-file", "func(path: string)", "text_click_open_file", true},
    {"[method]text-component.click-run-command", "func(command: string)", "text_click_run_command",
     true},
    {"[method]text-component.click-suggest-command", "func(command: string)",
     "text_click_suggest_command", true},
    {"[method]text-component.click-change-page", "func(page: u32)", "text_click_change_page", true},
    {"[method]text-component.click-copy-to-clipboard", "func(text: string)",
     "text_click_copy_to_clipboard", true},
    {"[method]text-component.hover-show-text", "func(text: text-component)", "text_hover_show_text",
     true},
};

inline constexpr PinnedMemberRef kWorldMembers[] = {
    {"[method]world.get-block-state-id", "func(pos: block-pos) -> u16", "world_get_block", true},
    {"[method]world.get-block", "func(pos: block-pos) -> block", "world_get_block_info", true},
    {"[method]world.get-block-id", "func(pos: block-pos) -> u16", "world_get_block_id", true},
    {"[method]world.set-time-of-day", "func(time: u64)", "world_set_time", true},
    {"[method]world.set-block",
     "async func(pos: block-pos, block: block, update-flags: block-flags)",
     "pumpkin_world_set_block", true},
    {"[method]world.get-time-of-day", "func() -> u64", "world_time", true},
    // `world_weather` reports the mode (0 clear, 1 rain, 2 thunder), so the two
    // predicates are derived from one call: rain is mode != 0, thunder mode == 2.
    {"[method]world.is-raining", "func() -> bool", "world_weather", true},
    {"[method]world.is-thundering", "func() -> bool", "world_weather", true},
};

inline constexpr PinnedMemberRef kPlayerMembers[] = {
    {"[method]player.get-id", "func() -> uuid", "player_get_id", true},
    {"[method]player.get-name", "func() -> string", "player_get_name", true},
    {"[method]player.get-position", "func() -> position", "player_get_position", true},
    {"[method]player.get-yaw", "func() -> f32", "player_get_yaw", true},
    {"[method]player.get-pitch", "func() -> f32", "player_get_pitch", true},
    {"[method]player.get-gamemode", "func() -> game-mode", "player_get_gamemode", true},
    {"[method]player.get-health", "func() -> f32", "player_get_health", true},
    {"[method]player.get-max-health", "func() -> f32", "player_get_max_health", true},
    {"[method]player.get-food-level", "func() -> u8", "player_get_food_level", true},
    {"[method]player.get-saturation", "func() -> f32", "player_get_saturation", true},
    {"[method]player.get-exhaustion", "func() -> f32", "player_get_exhaustion", true},
    {"[method]player.get-absorption", "func() -> f32", "player_get_absorption", true},
    {"[method]player.get-experience-level", "func() -> s32", "player_get_experience_level", true},
    {"[method]player.get-experience-progress", "func() -> f32", "player_get_experience_progress",
     true},
    {"[method]player.get-experience-points", "func() -> s32", "player_get_experience_points", true},
    {"[method]player.get-camera-entity-id", "func() -> u32", "player_get_camera_entity_id", true},
    {"[method]player.send-system-message", "func(text: text-component, overlay: bool)",
     "player_send_system_message", true},
    {"[method]player.chat", "async func(message: string)", "player_chat", true},
};

inline constexpr PinnedMemberRef kSchedulerMembers[] = {
    {"schedule-delayed-task", "func(handler-id: u32, delay-ticks: u64) -> u32",
     "scheduler_schedule_delayed", false},
    {"schedule-repeating-task", "func(handler-id: u32, delay-ticks: u64, period-ticks: u64) -> u32",
     "scheduler_schedule_repeating", false},
    {"cancel-task", "func(task-id: u32)", "scheduler_cancel", false},
};

inline constexpr PinnedMemberRef kUuidMembers[] = {
    {"generate", "func() -> uuid", "uuid_generate", false},
    {"parse", "func(s: string) -> option<uuid>", "uuid_parse", false},
    {"to-string", "func(id: uuid) -> string", "uuid_to_string", false},
};

inline constexpr PinnedMemberRef kI18nMembers[] = {
    {"translate", "func(key: string, locale: locale) -> string", "i18n_translate", false},
    {"load-translations", "func(namespace: string, json: string, locale: locale)",
     "i18n_load_translations", false},
};

inline constexpr PinnedMemberRef kItemStackMembers[] = {
    {"[constructor]item-stack", "constructor(registry-key: string, count: u8)", "item_stack_new",
     false},
    {"[method]item-stack.get-registry-key", "async func() -> string", "item_stack_get_registry_key",
     true},
    {"[method]item-stack.get-count", "async func() -> u8", "item_stack_get_count", true},
    {"[method]item-stack.set-count", "async func(count: u8)", "item_stack_set_count", true},
    {"[method]item-stack.get-max-count", "async func() -> u8", "item_stack_get_max_count", true},
    {"[method]item-stack.clone", "async func() -> item-stack", "item_stack_clone", true},
    {"[method]item-stack.with-count", "async func(count: u8) -> item-stack",
     "item_stack_with_count", true},
    {"[method]item-stack.is-similar", "async func(other: item-stack) -> bool",
     "item_stack_is_similar", true},
    {"[method]item-stack.get-damage", "async func() -> s32", "item_stack_get_damage", true},
    {"[method]item-stack.set-damage", "async func(damage: s32)", "item_stack_set_damage", true},
    {"[method]item-stack.get-max-damage", "async func() -> s32", "item_stack_get_max_damage", true},
    {"[method]item-stack.is-damageable", "async func() -> bool", "item_stack_is_damageable", true},
    {"[method]item-stack.is-unbreakable", "async func() -> bool", "item_stack_is_unbreakable",
     true},
    {"[method]item-stack.set-unbreakable", "async func(unbreakable: bool)",
     "item_stack_set_unbreakable", true},
    {"[method]item-stack.get-repair-cost", "async func() -> s32", "item_stack_get_repair_cost",
     true},
    {"[method]item-stack.set-repair-cost", "async func(cost: s32)", "item_stack_set_repair_cost",
     true},
    {"[method]item-stack.get-item-model", "async func() -> option<string>",
     "item_stack_get_item_model", true},
    {"[method]item-stack.set-item-model", "async func(model: option<string>)",
     "item_stack_set_item_model", true},
    {"[method]item-stack.get-enchantment-glint-override", "async func() -> option<bool>",
     "item_stack_get_enchantment_glint_override", true},
    {"[method]item-stack.set-enchantment-glint-override", "async func(glint: option<bool>)",
     "item_stack_set_enchantment_glint_override", true},
    {"[method]item-stack.get-attribute-modifiers", "async func() -> list<item-attribute-modifier>",
     "item_stack_get_attribute_modifiers", true},
    {"[method]item-stack.add-attribute-modifier", "async func(modifier: item-attribute-modifier)",
     "item_stack_add_attribute_modifier", true},
    {"[method]item-stack.remove-attribute-modifiers", "async func(attribute: attribute)",
     "item_stack_remove_attribute_modifiers", true},
    {"[method]item-stack.clear-attribute-modifiers", "async func()",
     "item_stack_clear_attribute_modifiers", true},
    {"[method]item-stack.set-custom-data",
     "async func(namespace: string, key: string, value: nbt-tree)", "item_stack_set_custom_data",
     true},
    {"[method]item-stack.get-custom-data",
     "async func(namespace: string, key: string) -> option<nbt-tree>", "item_stack_get_custom_data",
     true},
    {"[method]item-stack.remove-custom-data", "async func(namespace: string, key: string)",
     "item_stack_remove_custom_data", true},
    {"[method]item-stack.has-custom-data", "async func(namespace: string, key: string) -> bool",
     "item_stack_has_custom_data", true},
    {"[method]item-stack.get-components", "async func() -> list<data-component-value>",
     "item_stack_get_components", true},
    {"[method]item-stack.set-component", "async func(component: data-component, value: list<u8>)",
     "item_stack_set_component", true},
    {"[method]item-stack.remove-component", "async func(component: data-component)",
     "item_stack_remove_component", true},
    {"[method]item-stack.get-enchantments", "async func() -> list<enchantment-value>",
     "item_stack_get_enchantments", true},
    {"[method]item-stack.add-enchantment", "async func(enchantment: enchantment, level: u32)",
     "item_stack_add_enchantment", true},
    {"[method]item-stack.remove-enchantment", "async func(enchantment: enchantment)",
     "item_stack_remove_enchantment", true},
    {"[method]item-stack.get-custom-enchantments", "async func() -> list<custom-enchantment-value>",
     "item_stack_get_custom_enchantments", true},
    {"[method]item-stack.add-custom-enchantment", "async func(enchantment-id: string, level: u32)",
     "item_stack_add_custom_enchantment", true},
    {"[method]item-stack.remove-custom-enchantment", "async func(enchantment-id: string)",
     "item_stack_remove_custom_enchantment", true},
    {"[method]item-stack.get-custom-enchantment-level",
     "async func(enchantment-id: string) -> option<u32>", "item_stack_get_custom_enchantment_level",
     true},
    {"[method]item-stack.has-custom-enchantment", "async func(enchantment-id: string) -> bool",
     "item_stack_has_custom_enchantment", true},
    {"[method]item-stack.get-lore", "async func() -> list<text-component>", "item_stack_get_lore",
     true},
    {"[method]item-stack.set-lore", "async func(lore: list<text-component>)", "item_stack_set_lore",
     true},
    {"[method]item-stack.add-lore", "async func(line: text-component)", "item_stack_add_lore",
     true},
    {"[method]item-stack.get-custom-name", "async func() -> option<text-component>",
     "item_stack_get_custom_name", true},
    {"[method]item-stack.set-custom-name", "async func(name: option<text-component>)",
     "item_stack_set_custom_name", true},
};

inline constexpr PinnedMemberRef kCommandMembers[] = {
    {"[constructor]command", "constructor(names: list<string>, description: string)", "command_new",
     false},
    {"[method]command.then", "func(node: command-node)", "command_then", true},
    {"[method]command.execute-with-handler-id", "func(handler-id: u32)", "command_execute", true},
    {"[static]command-node.literal", "static func(name: string) -> command-node",
     "command_node_literal", false},
    {"[static]command-node.argument",
     "static func(name: string, %type: argument-type) -> command-node", "command_node_argument",
     false},
    {"[method]command-node.then", "func(node: command-node)", "command_node_then", true},
    {"[method]command-node.execute-with-handler-id", "func(handler-id: u32)",
     "command_node_execute", true},
    {"[method]command-node.suggest-with-handler-id", "func(handler-id: u32)",
     "command_node_suggest", true},
    {"[method]command-node.require-with-handler-id", "func(handler-id: u32)",
     "command_node_require", true},
};

inline constexpr PinnedMemberRef kContextMembers[] = {
    {"[method]context.register-event",
     "func(handler-id: u32, event-type: event-type, event-priority: event-priority, blocking: "
     "bool)",
     "pumpkin_context_register_event", true},
    {"[method]context.get-server", "func() -> server", "pumpkin_context_get_server", true},
    {"[method]context.get-data-folder", "func() -> string", "pumpkin_context_get_data_folder",
     true},
    {"[method]context.register-command", "func(command: command, permission: string)",
     "pumpkin_context_register_command", true},
    {"[method]context.register-permission", "func(permission: permission) -> result<_, string>",
     "pumpkin_context_register_permission", true},
    {"[method]context.get-marketplace-metadata", "func() -> option<marketplace-metadata>",
     "pumpkin_context_marketplace_metadata", true},
};

// The v0.2 package has twenty-three interfaces that export only value types.
// They import no host function, so there is nothing to lower: listing them keeps
// the adapter from calling their definitions an unbacked generic stub.
inline constexpr PinnedInterfaceRef kPinnedInterfaceTable[] = {
    {"pumpkin:plugin/advancement@0.2.0", nullptr, 0},
    {"pumpkin:plugin/attributes@0.2.0", nullptr, 0},
    {"pumpkin:plugin/bedrock-packets@0.2.0", nullptr, 0},
    {"pumpkin:plugin/biomes@0.2.0", nullptr, 0},
    {"pumpkin:plugin/command@0.2.0", kCommandMembers, std::size(kCommandMembers)},
    {"pumpkin:plugin/common@0.2.0", nullptr, 0},
    {"pumpkin:plugin/context@0.2.0", kContextMembers, std::size(kContextMembers)},
    {"pumpkin:plugin/damage-types@0.2.0", nullptr, 0},
    {"pumpkin:plugin/data-components@0.2.0", nullptr, 0},
    {"pumpkin:plugin/entity-statuses@0.2.0", nullptr, 0},
    {"pumpkin:plugin/entity-types@0.2.0", nullptr, 0},
    {"pumpkin:plugin/entity@0.2.0", nullptr, 0},
    {"pumpkin:plugin/event@0.2.0", nullptr, 0},
    {"pumpkin:plugin/forms@0.2.0", nullptr, 0},
    {"pumpkin:plugin/game-events@0.2.0", nullptr, 0},
    {"pumpkin:plugin/game-rules@0.2.0", nullptr, 0},
    {"pumpkin:plugin/i18n@0.2.0", kI18nMembers, std::size(kI18nMembers)},
    {"pumpkin:plugin/item-stack@0.2.0", kItemStackMembers, std::size(kItemStackMembers)},
    {"pumpkin:plugin/java-dialogs@0.2.0", nullptr, 0},
    {"pumpkin:plugin/java-packets@0.2.0", nullptr, 0},
    {"pumpkin:plugin/logging@0.2.0", kLoggingMembers, std::size(kLoggingMembers)},
    {"pumpkin:plugin/particles@0.2.0", nullptr, 0},
    {"pumpkin:plugin/permission@0.2.0", nullptr, 0},
    {"pumpkin:plugin/player@0.2.0", kPlayerMembers, std::size(kPlayerMembers)},
    {"pumpkin:plugin/potions@0.2.0", nullptr, 0},
    {"pumpkin:plugin/scheduler@0.2.0", kSchedulerMembers, std::size(kSchedulerMembers)},
    {"pumpkin:plugin/screens@0.2.0", nullptr, 0},
    {"pumpkin:plugin/server@0.2.0", kServerMembers, std::size(kServerMembers)},
    {"pumpkin:plugin/sounds@0.2.0", nullptr, 0},
    {"pumpkin:plugin/statistics@0.2.0", nullptr, 0},
    {"pumpkin:plugin/status-effect@0.2.0", nullptr, 0},
    {"pumpkin:plugin/text@0.2.0", kTextMembers, std::size(kTextMembers)},
    {"pumpkin:plugin/uuid@0.2.0", kUuidMembers, std::size(kUuidMembers)},
    {"pumpkin:plugin/world@0.2.0", kWorldMembers, std::size(kWorldMembers)},
};

inline const std::vector<PinnedInterfaceRef>& pinned_interfaces() {
  static const std::vector<PinnedInterfaceRef> table(std::begin(kPinnedInterfaceTable),
                                                     std::end(kPinnedInterfaceTable));
  return table;
}

inline const PinnedInterfaceRef* find_pinned_interface(std::string_view name) {
  for (const PinnedInterfaceRef& pinned : pinned_interfaces()) {
    if (name == std::string_view(pinned.name))
      return &pinned;
  }
  return nullptr;
}

struct ComponentImportRef {
  // The full extern name as encoded (an `implements` name is used here, with
  // any version suffix applied, because that is the interface being imported).
  std::string name;
  // "instance", "func", "module", "component", "type" or "value".
  std::string kind;
  bool pinned = false;
  // "member  signature -> plusmc:host/call" for every backed member.
  std::vector<std::string> backed;
  // "member  signature" for every listed member the host cannot back.
  std::vector<std::string> unbacked;
};

struct ComponentExportRef {
  std::string name;
  std::string kind;
  std::uint32_t index = 0;
};

struct ComponentReport {
  // True when a verdict was produced, including `blocked` and
  // `not-component`. False only when the file could not be read or parsed.
  bool ok = false;
  std::string error;
  std::string file;
  std::string sha256;
  std::uint64_t size = 0;
  std::uint32_t version = 0;
  bool component = false;
  std::string package_name;
  std::string package_version;
  unsigned package_api = 0;
  std::vector<ComponentImportRef> imports;
  std::vector<ComponentExportRef> exports;
  std::vector<std::string> notes;
  std::vector<std::string> reasons;
  // "mapped" (every import is an interface instance or type and `on-load` is
  // exported; an unlisted interface is bound to the generic stub), "blocked"
  // (an import has another kind, or the lifecycle export is absent),
  // "not-component" (a core module; use `inspect`), "unreadable" (the gate or
  // the component layer did not parse).
  std::string status;
  // "full" only when every listed member of every imported interface is backed
  // and no unlisted interface needed a stub; "partial" when one is listed but
  // unbacked or an unlisted interface is stubbed; empty when blocked. `full` is
  // about this table's subset, not WIT completeness.
  std::string coverage;
};

// `path` is a `.wasm` file or a `+mc-wasm.toml` package directory. The gate is
// run first, so a manifest, a missing module and a core-module header are
// answered exactly as `inspect` answers them; only a component reaches the
// component-layer reader.
ComponentReport adapt_component(const std::filesystem::path& path);

std::string adapt_text(const ComponentReport& report);
std::string adapt_json(const ComponentReport& report);

} // namespace mc::wasm
