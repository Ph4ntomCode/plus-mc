// SPDX-License-Identifier: GPL-3.0-or-later

#include "converter/api_map.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "converter/report_json.h"

namespace mc::converter {
namespace {

using detail::json_list;
using detail::json_quote;

struct ClassEntry {
  const char* reference;
  const char* native_symbol;
  MapStatus status;
  const char* note;
};

struct MemberEntry {
  const char* owner;
  const char* name;
  const char* native_symbol;
  MapStatus status;
  const char* note;
};

// An overload whose behavior differs from the name-only entry. The descriptor
// is part of the key, so the array and single-string forms of `sendMessage`
// cannot share one decision.
struct OverloadEntry {
  const char* owner;
  const char* name;
  const char* descriptor;
  const char* native_symbol;
  MapStatus status;
  const char* note;
};

// The mapped surface: the classes `score: green` accepts, each with the ABI
// call that replaces it. A member the table does not name is a gap, even when
// its owner is mapped; that distinction is the point of the report.
constexpr ClassEntry kClasses[] = {
    {"org/bukkit/event/Listener", "pmc_host_api::on + pmc_event_fn", MapStatus::Mapped, ""},
    {"org/bukkit/event/Cancellable", "pmc_event::cancel", MapStatus::Mapped,
     "cancelable event types only"},
    {"org/bukkit/command/CommandExecutor", "pmc_host_api::command", MapStatus::Mapped, ""},
    {"org/bukkit/event/player/PlayerJoinEvent", "PMC_EVENT_PLAYER_JOIN", MapStatus::Mapped, ""},
    {"org/bukkit/event/player/PlayerQuitEvent", "PMC_EVENT_PLAYER_LEAVE", MapStatus::Mapped, ""},
    {"org/bukkit/plugin/java/JavaPlugin",
     "pmc_plugin lifecycle (on_load/on_enable/on_disable/on_unload)", MapStatus::Mapped, ""},

    {"org/bukkit/plugin/Plugin", "pmc_plugin + pmc_host_api", MapStatus::Partial,
     "logger and config are host calls, not Plugin methods"},
    {"org/bukkit/plugin/PluginBase", "pmc_plugin", MapStatus::Partial,
     "the same lifecycle without a JavaPlugin inheritance chain"},
    {"org/bukkit/event/EventHandler", "pmc_host_api::on", MapStatus::Partial,
     "no priority or ignoreCancelled"},
    {"org/bukkit/event/Event", "pmc_event", MapStatus::Partial,
     "only the ABI's event types are delivered"},
    {"org/bukkit/command/Command", "pmc_command", MapStatus::Partial, "name, usage and argv only"},
    {"org/bukkit/command/CommandSender", "pmc_reply + pmc_command::player", MapStatus::Partial,
     "messages and the name; no permission checks"},
    {"org/bukkit/scheduler/BukkitRunnable", "pmc_host_api::schedule / cancel", MapStatus::Partial,
     "cancel by task id, not by object"},
    {"org/bukkit/scheduler/BukkitScheduler", "pmc_host_api::schedule / schedule_async / cancel",
     MapStatus::Partial, "one sync and one async queue"},
    {"org/bukkit/scheduler/BukkitTask", "pmc_host_api::schedule return id", MapStatus::Partial,
     "a uint64 id, not a task object"},
    {"org/bukkit/configuration/ConfigurationSection",
     "pmc_host_api::config_string / config_int / config_bool / config_set_*", MapStatus::Partial,
     "flat keys, typed; writes rewrite config.toml"},
    {"org/bukkit/configuration/FileConfiguration",
     "pmc_host_api::config_string / config_int / config_bool / config_set_*", MapStatus::Partial,
     "flat keys, typed; writes rewrite config.toml"},
    {"org/bukkit/permissions/Permissible", "pmc_host_api::player_op_level", MapStatus::Partial,
     "operator levels only; no permission nodes"},
    {"org/bukkit/plugin/PluginDescriptionFile", "+mc-plugin.toml manifest", MapStatus::Partial,
     "distribution fields live in mod.json"},
    {"org/bukkit/World", "world_block / world_biome / world_set_block / entities",
     MapStatus::Partial, "no weather, time, spawn or dimension API"},
    {"org/bukkit/block/Block", "world_block / world_set_block", MapStatus::Partial,
     "canonical state names, no Block object"},
    {"org/bukkit/entity/Entity", "pmc_entity + entity_get/damage/teleport/despawn",
     MapStatus::Partial, "snapshots, no components or rotation writes"},
    {"org/bukkit/entity/LivingEntity", "pmc_entity::health + entity_damage", MapStatus::Partial,
     "damage only; no healing, potions or attribute API"},
    {"org/bukkit/entity/Player", "pmc_player + inventory_get/inventory_set", MapStatus::Partial,
     "snapshots and queued inventory writes"},
    {"org/bukkit/inventory/Inventory", "pmc_host_api::inventory_get / inventory_set",
     MapStatus::Partial, "slots only; no Inventory object"},
    {"org/bukkit/inventory/PlayerInventory", "pmc_host_api::inventory_get / inventory_set",
     MapStatus::Partial, "the player's own 0..40 slot numbering"},
    {"org/bukkit/inventory/ItemStack", "pmc_item", MapStatus::Partial,
     "no components; a write installs a plain stack"},
    {"org/bukkit/Location", "pmc_player / pmc_entity coordinates", MapStatus::Partial,
     "no world handle"},
    {"org/bukkit/Material", "canonical block/item registry names", MapStatus::Partial,
     "names, not Material constants"},
    {"org/bukkit/event/player/AsyncPlayerChatEvent", "PMC_EVENT_PLAYER_CHAT", MapStatus::Partial,
     "no format string or recipient list"},
    {"org/bukkit/event/block/BlockPlaceEvent", "PMC_EVENT_BLOCK_PLACE", MapStatus::Partial,
     "no item-in-hand or build context"},
    {"org/bukkit/event/block/BlockBreakEvent", "PMC_EVENT_BLOCK_BREAK", MapStatus::Partial,
     "no drops or experience context"},
    {"org/bukkit/event/player/PlayerInteractEvent", "PMC_EVENT_ITEM_USE_ON", MapStatus::Partial,
     "buckets and flint and steel only"},

    {"org/bukkit/Bukkit", "", MapStatus::Gap, "no static server facade; use the host table"},
    {"org/bukkit/Server", "", MapStatus::Gap, "only server_version and tick_count are exposed"},
    {"org/bukkit/plugin/PluginManager", "", MapStatus::Gap,
     "the host loads plugins; plugins do not load plugins"},
    {"org/bukkit/command/TabCompleter", "", MapStatus::Gap, "no tab-completion hook"},
    {"org/bukkit/command/PluginCommand", "", MapStatus::Gap,
     "register through pmc_host_api::command"},
    {"org/bukkit/configuration/MemoryConfiguration", "", MapStatus::Gap,
     "the host config is a flat file, not an in-memory tree"},
    {"org/bukkit/permissions/Permission", "", MapStatus::Gap, "operator levels only"},
    {"org/bukkit/event/player/PlayerMoveEvent", "", MapStatus::Gap,
     "movement is relayed, not delivered to plugins"},
    {"org/bukkit/event/entity/EntityDamageEvent", "", MapStatus::Gap,
     "damage is a write, not an event"},
    {"org/bukkit/event/inventory/InventoryClickEvent", "", MapStatus::Gap,
     "menu events are not exposed"},
    {"org/bukkit/event/player/PlayerChatEvent", "", MapStatus::Gap,
     "the async chat event is the exposed one"},
    {"org/bukkit/event/server/ServerLoadEvent", "", MapStatus::Gap,
     "server start is a lifecycle callback"},
};

// Member-level mapping, keyed by owner + name. An overload that means
// something materially different gets an exact-descriptor entry in
// `kOverloads`; every other descriptor of that name shares this decision,
// which is only safe when the behavior is the same.
constexpr MemberEntry kMembers[] = {
    {"org/bukkit/plugin/java/JavaPlugin", "getName", "pmc_plugin::name", MapStatus::Mapped, ""},
    {"org/bukkit/plugin/java/JavaPlugin", "getDataFolder", "pmc_host_api::data_dir",
     MapStatus::Mapped, ""},
    {"org/bukkit/plugin/java/JavaPlugin", "getLogger", "pmc_host_api::log", MapStatus::Partial,
     "one formatted line at a level, not a java.util.logging.Logger"},
    {"org/bukkit/plugin/java/JavaPlugin", "getConfig",
     "pmc_host_api::config_string / config_int / config_bool", MapStatus::Partial,
     "no live FileConfiguration object"},
    {"org/bukkit/plugin/java/JavaPlugin", "onEnable", "pmc_plugin::on_enable", MapStatus::Mapped,
     ""},
    {"org/bukkit/plugin/java/JavaPlugin", "onDisable", "pmc_plugin::on_disable", MapStatus::Mapped,
     ""},
    {"org/bukkit/plugin/java/JavaPlugin", "saveDefaultConfig", "", MapStatus::Gap,
     "the host reads config.toml; it does not write one"},
    {"org/bukkit/plugin/java/JavaPlugin", "saveConfig", "pmc_host_api::config_set_*",
     MapStatus::Partial, "one key at a time; rewrites config.toml"},
    {"org/bukkit/plugin/java/JavaPlugin", "reloadConfig", "", MapStatus::Gap,
     "config is read once, before on_load"},
    {"org/bukkit/plugin/java/JavaPlugin", "getCommand", "", MapStatus::Gap,
     "registration only; there is no command lookup"},
    {"org/bukkit/plugin/java/JavaPlugin", "getServer", "", MapStatus::Gap, "no Server object"},
    {"org/bukkit/plugin/java/JavaPlugin", "isEnabled", "", MapStatus::Gap,
     "no enabled flag is exposed"},

    {"org/bukkit/command/CommandExecutor", "onCommand", "pmc_command_fn", MapStatus::Partial,
     "argv instead of Command; the boolean return is ignored"},
    {"org/bukkit/command/CommandExecutor", "onTabComplete", "", MapStatus::Gap,
     "no tab-completion hook"},

    {"org/bukkit/command/CommandSender", "sendMessage", "pmc_host_api::reply", MapStatus::Mapped,
     ""},
    {"org/bukkit/command/CommandSender", "getName", "pmc_command::player", MapStatus::Mapped, ""},
    {"org/bukkit/command/CommandSender", "isOp", "pmc_host_api::player_op_level",
     MapStatus::Partial, "level >= 1 is the operator answer"},
    {"org/bukkit/command/CommandSender", "hasPermission", "", MapStatus::Gap,
     "no permission nodes"},

    {"org/bukkit/scheduler/BukkitRunnable", "run", "pmc_task_fn", MapStatus::Mapped, ""},
    {"org/bukkit/scheduler/BukkitRunnable", "cancel", "pmc_host_api::cancel", MapStatus::Partial,
     "needs the task id returned by schedule"},
    {"org/bukkit/scheduler/BukkitScheduler", "runTask", "pmc_host_api::schedule",
     MapStatus::Partial, "delay 0 means the next pump"},
    {"org/bukkit/scheduler/BukkitScheduler", "runTaskLater", "pmc_host_api::schedule",
     MapStatus::Partial, "ticks, not a TimeUnit"},
    {"org/bukkit/scheduler/BukkitScheduler", "runTaskTimer", "pmc_host_api::schedule",
     MapStatus::Partial, "delay and repeat are tick counts"},
    {"org/bukkit/scheduler/BukkitScheduler", "runTaskAsynchronously",
     "pmc_host_api::schedule_async", MapStatus::Partial, "one worker, no delay"},
    {"org/bukkit/scheduler/BukkitScheduler", "cancelTask", "pmc_host_api::cancel",
     MapStatus::Partial, "a uint64 task id"},

    {"org/bukkit/configuration/ConfigurationSection", "getString", "pmc_host_api::config_string",
     MapStatus::Mapped, ""},
    {"org/bukkit/configuration/ConfigurationSection", "getInt", "pmc_host_api::config_int",
     MapStatus::Mapped, ""},
    {"org/bukkit/configuration/ConfigurationSection", "getLong", "pmc_host_api::config_int",
     MapStatus::Partial, "int64 in the ABI"},
    {"org/bukkit/configuration/ConfigurationSection", "getBoolean", "pmc_host_api::config_bool",
     MapStatus::Mapped, ""},
    {"org/bukkit/configuration/ConfigurationSection", "getDouble", "", MapStatus::Gap,
     "no floating-point config getter"},
    {"org/bukkit/configuration/ConfigurationSection", "set", "pmc_host_api::config_set_*",
     MapStatus::Partial, "one key at a time; rewrites config.toml"},
    {"org/bukkit/configuration/ConfigurationSection", "getKeys", "", MapStatus::Gap,
     "keys are not enumerable"},

    {"org/bukkit/permissions/Permissible", "hasPermission", "", MapStatus::Gap,
     "no permission nodes"},
    {"org/bukkit/permissions/Permissible", "isPermissionSet", "", MapStatus::Gap,
     "no permission nodes"},
    {"org/bukkit/permissions/Permissible", "isOp", "pmc_host_api::player_op_level",
     MapStatus::Partial, "level >= 1 is the operator answer"},

    {"org/bukkit/World", "getBlockAt", "pmc_host_api::world_block", MapStatus::Partial,
     "returns a canonical state name, not a Block"},
    {"org/bukkit/World", "getEntities", "pmc_host_api::entities", MapStatus::Partial,
     "snapshots, one callback per entity"},
    {"org/bukkit/World", "getPlayers", "pmc_host_api::entities", MapStatus::Partial,
     "filter the entity visitor for players"},
    {"org/bukkit/World", "getPlayer", "pmc_host_api::player_get", MapStatus::Partial,
     "by name, snapshot"},
    {"org/bukkit/World", "spawn", "", MapStatus::Gap, "no entity spawn API in ABI 1"},
    {"org/bukkit/World", "dropItem", "", MapStatus::Gap, "no item-spawn API in ABI 1"},
    {"org/bukkit/World", "getTime", "", MapStatus::Gap, "no world clock accessor"},
    {"org/bukkit/World", "setTime", "", MapStatus::Gap, "no world clock setter"},

    {"org/bukkit/block/Block", "getType", "pmc_host_api::world_block", MapStatus::Partial,
     "a state name, not a Material"},
    {"org/bukkit/block/Block", "setType", "pmc_host_api::world_set_block", MapStatus::Partial,
     "a canonical state name"},
    {"org/bukkit/block/Block", "getBlockData", "pmc_host_api::world_block", MapStatus::Partial,
     "a state name, not BlockData"},
    {"org/bukkit/block/Block", "setBlockData", "pmc_host_api::world_set_block", MapStatus::Partial,
     "a canonical state name"},
    {"org/bukkit/block/Block", "getLocation", "block coordinates", MapStatus::Partial,
     "x/y/z are the cell the event carries"},

    {"org/bukkit/entity/Entity", "getLocation", "pmc_entity x/y/z/yaw/pitch", MapStatus::Partial,
     "a snapshot, not a live Location"},
    {"org/bukkit/entity/Entity", "getHealth", "pmc_entity::health", MapStatus::Mapped, ""},
    {"org/bukkit/entity/Entity", "setHealth", "pmc_host_api::entity_damage", MapStatus::Partial,
     "damage only; there is no heal write"},
    {"org/bukkit/entity/Entity", "teleport", "pmc_host_api::entity_teleport", MapStatus::Partial,
     "position only; rotation is not carried"},
    {"org/bukkit/entity/Entity", "remove", "pmc_host_api::entity_despawn", MapStatus::Mapped, ""},
    {"org/bukkit/entity/Entity", "getType", "pmc_entity::type / type_id", MapStatus::Partial,
     "a registry name and protocol id"},
    {"org/bukkit/entity/Entity", "getUniqueId", "", MapStatus::Gap,
     "entity ids are int32, not UUIDs"},
    {"org/bukkit/entity/Entity", "getVelocity", "pmc_entity vx/vy/vz", MapStatus::Partial,
     "observed or simulated delta, not a Vector"},
    {"org/bukkit/entity/Entity", "getWorld", "", MapStatus::Gap,
     "the engine serves one world and exposes no handle"},

    {"org/bukkit/entity/Player", "getName", "pmc_player::name", MapStatus::Mapped, ""},
    {"org/bukkit/entity/Player", "getGameMode", "pmc_player::gamemode", MapStatus::Mapped, ""},
    {"org/bukkit/entity/Player", "getHealth", "pmc_player::health", MapStatus::Mapped, ""},
    {"org/bukkit/entity/Player", "getLocation", "pmc_player x/y/z/yaw/pitch", MapStatus::Mapped,
     ""},
    {"org/bukkit/entity/Player", "getInventory", "pmc_host_api::inventory_get / inventory_set",
     MapStatus::Partial, "by slot; no PlayerInventory object"},
    {"org/bukkit/entity/Player", "sendMessage", "pmc_host_api::reply", MapStatus::Mapped, ""},
    {"org/bukkit/entity/Player", "kick", "", MapStatus::Gap, "no disconnect API"},
    {"org/bukkit/entity/Player", "isSneaking", "", MapStatus::Gap, "pose is not exposed"},
    {"org/bukkit/entity/Player", "isSprinting", "", MapStatus::Gap, "pose is not exposed"},

    {"org/bukkit/inventory/ItemStack", "getType", "pmc_item::item / name", MapStatus::Partial,
     "an item id and registry name, not a Material"},
    {"org/bukkit/inventory/ItemStack", "getAmount", "pmc_item::count", MapStatus::Mapped, ""},
    {"org/bukkit/inventory/ItemStack", "setAmount", "pmc_host_api::inventory_set",
     MapStatus::Partial, "a queued whole-slot write"},
    {"org/bukkit/inventory/ItemStack", "getDurability", "pmc_item::damage", MapStatus::Partial,
     "read-only; a write installs a plain stack"},
    {"org/bukkit/inventory/ItemStack", "getItemMeta", "", MapStatus::Gap,
     "components are not exposed in ABI 1"},
};

// Exact-descriptor entries, checked before `kMembers`. Only a behavior
// difference belongs here: `sendMessage(String)` is one reply, while the array
// form needs one reply per line and cannot claim the mapped path.
constexpr OverloadEntry kOverloads[] = {
    {"org/bukkit/command/CommandSender", "sendMessage", "([Ljava/lang/String;)V",
     "pmc_host_api::reply", MapStatus::Partial, "one line per call; the ABI has no array form"},
    {"org/bukkit/entity/Player", "sendMessage", "([Ljava/lang/String;)V", "pmc_host_api::reply",
     MapStatus::Partial, "one line per call; the ABI has no array form"},
};

bool starts_with(const std::string& text, const std::string& prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool is_bukkit(const std::string& name) {
  return starts_with(name, "org/bukkit/") || starts_with(name, "io/papermc/") ||
         starts_with(name, "com/destroystokyo/paper/") || starts_with(name, "org/spigotmc/");
}

bool is_server_internal(const std::string& name) {
  return starts_with(name, "net/minecraft/") || starts_with(name, "org/bukkit/craftbukkit/") ||
         starts_with(name, "com/mojang/");
}

bool is_reflection(const std::string& name) {
  return starts_with(name, "java/lang/reflect/") || starts_with(name, "java/lang/invoke/") ||
         starts_with(name, "java/lang/instrument/") || starts_with(name, "sun/reflect/") ||
         name == "sun/misc/Unsafe" || starts_with(name, "org/objectweb/asm/");
}

const ClassEntry* find_class(const std::string& reference) {
  for (const ClassEntry& entry : kClasses) {
    if (reference == entry.reference)
      return &entry;
  }
  return nullptr;
}

const MemberEntry* find_member(const std::string& owner, const std::string& name) {
  for (const MemberEntry& entry : kMembers) {
    if (owner == entry.owner && name == entry.name)
      return &entry;
  }
  return nullptr;
}

const OverloadEntry* find_overload(const std::string& owner, const std::string& name,
                                   const std::string& descriptor) {
  for (const OverloadEntry& entry : kOverloads) {
    if (owner == entry.owner && name == entry.name && descriptor == entry.descriptor)
      return &entry;
  }
  return nullptr;
}

void add_class(MappingReport* report, const std::string& reference, const char* native_symbol,
               MapStatus status, const char* note) {
  report->classes.push_back(ClassMapping{reference, native_symbol == nullptr ? "" : native_symbol,
                                         status, note == nullptr ? "" : note});
}

void add_member(MappingReport* report, const MemberRef& ref, bool field, const char* native_symbol,
                MapStatus status, const char* note) {
  report->members.push_back(MemberMapping{ref.owner, ref.name, ref.descriptor, field,
                                          native_symbol == nullptr ? "" : native_symbol, status,
                                          note == nullptr ? "" : note});
}

std::size_t count_status(const MappingReport& report, MapStatus status) {
  std::size_t count = 0;
  for (const ClassMapping& mapping : report.classes) {
    if (mapping.status == status)
      ++count;
  }
  for (const MemberMapping& mapping : report.members) {
    if (mapping.status == status)
      ++count;
  }
  return count;
}

std::size_t group_count(const MappingReport& report, MapStatus status, bool members) {
  std::size_t count = 0;
  if (members) {
    for (const MemberMapping& mapping : report.members) {
      if (mapping.status == status)
        ++count;
    }
  } else {
    for (const ClassMapping& mapping : report.classes) {
      if (mapping.status == status)
        ++count;
    }
  }
  return count;
}

std::string coverage_json(const MappingReport& report) {
  const std::size_t total = mapping_reference_count(report);
  const std::size_t covered = mapping_covered_count(report);
  const double ratio = total == 0 ? 0.0 : static_cast<double>(covered) / static_cast<double>(total);
  char text[32];
  std::snprintf(text, sizeof(text), "%.3f", ratio);
  return text;
}

std::string print_native(const std::string& native_symbol) {
  return native_symbol.empty() ? "-" : native_symbol;
}

} // namespace

std::size_t mapping_reference_count(const MappingReport& report) {
  return report.classes.size() + report.members.size();
}

std::size_t mapping_covered_count(const MappingReport& report) {
  return count_status(report, MapStatus::Mapped) + count_status(report, MapStatus::Partial);
}

std::string map_status_name(MapStatus status) {
  switch (status) {
  case MapStatus::Mapped:
    return "mapped";
  case MapStatus::Partial:
    return "partial";
  case MapStatus::Gap:
    return "gap";
  }
  return "gap";
}

MappingReport map_analysis(const Analysis& analysis) {
  MappingReport report;
  report.package = analysis.package;
  if (!analysis.ok) {
    report.error = analysis.error;
    return report;
  }
  report.score = analysis.score;
  std::size_t ignored = 0;

  for (const std::string& reference : analysis.class_refs) {
    if (is_server_internal(reference)) {
      add_class(&report, reference, nullptr, MapStatus::Gap,
                "server-internal (NMS/CraftBukkit): not exposed by ABI 1");
    } else if (is_reflection(reference)) {
      add_class(&report, reference, nullptr, MapStatus::Gap, "reflection: no static mapping");
    } else if (!is_bukkit(reference)) {
      ++ignored;
    } else if (const ClassEntry* entry = find_class(reference)) {
      add_class(&report, reference, entry->native_symbol, entry->status, entry->note);
    } else {
      add_class(&report, reference, nullptr, MapStatus::Gap,
                "no mapping entry for this API class in slice 1");
    }
  }

  const auto map_member = [&](const MemberRef& ref, bool field) {
    if (is_server_internal(ref.owner)) {
      add_member(&report, ref, field, nullptr, MapStatus::Gap,
                 "server-internal (NMS/CraftBukkit): not exposed by ABI 1");
      return;
    }
    const std::string member_key = ref.owner + "#" + ref.name;
    if (std::binary_search(analysis.usage.reflection_members.begin(),
                           analysis.usage.reflection_members.end(), member_key) ||
        is_reflection(ref.owner)) {
      add_member(&report, ref, field, nullptr, MapStatus::Gap, "reflection: no static mapping");
      return;
    }
    if (!is_bukkit(ref.owner)) {
      ++ignored;
      return;
    }
    if (const OverloadEntry* overload = find_overload(ref.owner, ref.name, ref.descriptor)) {
      add_member(&report, ref, field, overload->native_symbol, overload->status, overload->note);
    } else if (const MemberEntry* entry = find_member(ref.owner, ref.name)) {
      add_member(&report, ref, field, entry->native_symbol, entry->status, entry->note);
    } else if (find_class(ref.owner) != nullptr) {
      add_member(&report, ref, field, nullptr, MapStatus::Gap,
                 "member is not in the mapping table; its owner has a native path");
    } else {
      add_member(&report, ref, field, nullptr, MapStatus::Gap,
                 "no mapping entry for the owner class in slice 1");
    }
  };
  for (const MemberRef& ref : analysis.method_refs)
    map_member(ref, false);
  for (const MemberRef& ref : analysis.field_refs)
    map_member(ref, true);

  std::sort(report.classes.begin(), report.classes.end(),
            [](const ClassMapping& left, const ClassMapping& right) {
              return left.reference < right.reference;
            });
  std::sort(report.members.begin(), report.members.end(),
            [](const MemberMapping& left, const MemberMapping& right) {
              if (left.owner != right.owner)
                return left.owner < right.owner;
              if (left.name != right.name)
                return left.name < right.name;
              return left.descriptor < right.descriptor;
            });

  if (!analysis.usage.nms.empty())
    report.notes.push_back(std::to_string(analysis.usage.nms.size()) +
                           " server-internal class reference(s) are gaps by definition");
  if (!analysis.usage.reflection.empty() || !analysis.usage.reflection_members.empty())
    report.notes.push_back("reflection is reported as a gap: it has no static mapping");
  if (ignored != 0)
    report.notes.push_back(std::to_string(ignored) +
                           " non-API reference(s) ignored (JDK, bundled libraries or the plugin "
                           "itself)");
  if (analysis.score == Score::Red)
    report.notes.push_back("the analyzer score is red; the gap list is the work a port needs");
  report.ok = true;
  return report;
}

std::string map_report_text(const MappingReport& report) {
  std::string out = "+MC converter map\n";
  out += "package: " + report.package + "\n";
  if (!report.ok)
    return out + "error: " + report.error + "\n";
  out += "score: " + score_name(report.score) + "\n";
  out += "classes: " + std::to_string(report.classes.size()) + " (" +
         std::to_string(group_count(report, MapStatus::Mapped, false)) + " mapped, " +
         std::to_string(group_count(report, MapStatus::Partial, false)) + " partial, " +
         std::to_string(group_count(report, MapStatus::Gap, false)) + " gap)\n";
  out += "members: " + std::to_string(report.members.size()) + " (" +
         std::to_string(group_count(report, MapStatus::Mapped, true)) + " mapped, " +
         std::to_string(group_count(report, MapStatus::Partial, true)) + " partial, " +
         std::to_string(group_count(report, MapStatus::Gap, true)) + " gap)\n";
  const std::size_t total = mapping_reference_count(report);
  const std::size_t covered = mapping_covered_count(report);
  const unsigned percent = total == 0 ? 0 : static_cast<unsigned>((covered * 100) / total);
  out += "coverage: " + std::to_string(percent) + "% (" + std::to_string(covered) + " of " +
         std::to_string(total) +
         " API references have a native path; partial entries name the "
         "missing half)\n";
  for (const ClassMapping& mapping : report.classes) {
    out += "  " + map_status_name(mapping.status) + "  " + mapping.reference + " -> " +
           print_native(mapping.native_symbol);
    if (!mapping.note.empty())
      out += " (" + mapping.note + ")";
    out += "\n";
  }
  if (!report.members.empty())
    out += "members:\n";
  for (const MemberMapping& mapping : report.members) {
    // A method's descriptor already begins with '('; a field's does not, so
    // the ':' keeps `owner#field:I` readable.
    const std::string signature = mapping.owner + "#" + mapping.name +
                                  (mapping.field ? ":" + mapping.descriptor : mapping.descriptor);
    out += "  " + map_status_name(mapping.status) + "  " + signature +
           (mapping.field ? " (field)" : "") + " -> " + print_native(mapping.native_symbol);
    if (!mapping.note.empty())
      out += " (" + mapping.note + ")";
    out += "\n";
  }
  for (const std::string& note : report.notes)
    out += "note: " + note + "\n";
  return out;
}

std::string map_report_json(const MappingReport& report) {
  std::string out = "{\"tool\":\"plusmc-convert\",\"schema\":1,\"mode\":\"map\",\"package\":";
  out += json_quote(report.package);
  out += ",\"ok\":";
  out += report.ok ? "true" : "false";
  if (!report.ok) {
    out += ",\"error\":";
    out += json_quote(report.error);
    return out + "}\n";
  }
  out += ",\"score\":";
  out += json_quote(score_name(report.score));
  out += ",\"summary\":{\"classes\":";
  out += std::to_string(report.classes.size());
  out += ",\"members\":";
  out += std::to_string(report.members.size());
  out += ",\"mapped\":";
  out += std::to_string(count_status(report, MapStatus::Mapped));
  out += ",\"partial\":";
  out += std::to_string(count_status(report, MapStatus::Partial));
  out += ",\"gap\":";
  out += std::to_string(count_status(report, MapStatus::Gap));
  out += ",\"coverage\":";
  out += coverage_json(report);
  out += "},\"classes\":[";
  for (std::size_t i = 0; i < report.classes.size(); ++i) {
    if (i != 0)
      out += ",";
    out += "{\"reference\":";
    out += json_quote(report.classes[i].reference);
    out += ",\"native\":";
    out += json_quote(report.classes[i].native_symbol);
    out += ",\"status\":";
    out += json_quote(map_status_name(report.classes[i].status));
    out += ",\"note\":";
    out += json_quote(report.classes[i].note);
    out += "}";
  }
  out += "],\"members\":[";
  for (std::size_t i = 0; i < report.members.size(); ++i) {
    if (i != 0)
      out += ",";
    out += "{\"owner\":";
    out += json_quote(report.members[i].owner);
    out += ",\"name\":";
    out += json_quote(report.members[i].name);
    out += ",\"descriptor\":";
    out += json_quote(report.members[i].descriptor);
    out += ",\"field\":";
    out += report.members[i].field ? "true" : "false";
    out += ",\"native\":";
    out += json_quote(report.members[i].native_symbol);
    out += ",\"status\":";
    out += json_quote(map_status_name(report.members[i].status));
    out += ",\"note\":";
    out += json_quote(report.members[i].note);
    out += "}";
  }
  out += "],\"notes\":";
  json_list(&out, report.notes);
  out += "}\n";
  return out;
}

} // namespace mc::converter
