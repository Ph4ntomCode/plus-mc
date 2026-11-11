// A plugin compiled against the pre-velocity snapshots, as a test fixture.
//
// `player_get` and `entity_get` are C ABI calls: the host knows a plugin's
// struct only from the `struct_size` the caller declares, so a plugin built
// before the velocity (and the entity's facing) fields were appended keeps its
// shorter structs. This module reproduces that caller: `OldPlayer` and
// `OldEntity` are the original field lists (trailing padding included), the
// calls pass those sizes, and a canary fills the bytes where `vx` now starts.
// `tests/plugin_test.py` runs `/oldabi <player>` and `/oldentget <id>` and
// asserts each prefix was filled while the canary stayed untouched; it runs
// `/oldptrunc [player]` and `/oldtrunc <id>` with structs that stop before the
// pre-velocity prefixes and asserts the host rejects them without writing. It
// also runs `/newplayer [player]` and `/newentity <id>` with structs larger
// than this host's snapshots, asserting the host fills its own size, reports
// that count in `struct_size`, and leaves the extension bytes alone. Finally,
// `/entitysize` walks the `entities` visitor and checks that every host-owned
// snapshot carries the host's `sizeof(pmc_entity)`, and `/invtrunc [player]`
// passes a `pmc_item` that stops before `name`, which `inventory_get` must
// refuse with `PMC_INVENTORY_OUT_OF_RANGE` without writing.
//
// It is a test fixture, not an example: it includes the current header for the
// host table and command types, then declares the old snapshot itself.
#include "pluginapi/plugin.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

// The pre-velocity snapshot, field for field.
struct OldPlayer {
  uint32_t struct_size;
  char name[64];
  double x, y, z;
  float yaw, pitch;
  int gamemode;
  float health;
  int32_t entity_id;
};

// The host accepts the call only when the declared size reaches the first
// appended field; this is the same number a binary built before the append
// would pass.
static_assert(sizeof(OldPlayer) == offsetof(pmc_player, vx),
              "the fixture must declare exactly the pre-velocity player prefix");

// The same prefix for `entity_get`: the fields through `age`, trailing padding
// included, with velocity and facing appended after them.
struct OldEntity {
  uint32_t struct_size;
  int32_t entity_id;
  int32_t type_id;
  char type[64];
  char name[64];
  double x, y, z;
  float health;
  int32_t age;
};

static_assert(sizeof(OldEntity) == offsetof(pmc_entity, vx),
              "the fixture must declare exactly the pre-velocity entity prefix");

// A prefix that stops at the position: the layout before `health` and `age`
// were added, and well below the pre-velocity floor. `entity_get` must reject
// this size, not treat it as a shorter prefix.
struct TruncatedEntity {
  uint32_t struct_size;
  int32_t entity_id;
  int32_t type_id;
  char type[64];
  char name[64];
  double x, y, z;
};

static_assert(sizeof(TruncatedEntity) == offsetof(pmc_entity, health),
              "the truncated struct must stop before the pre-velocity prefix");

// A prefix that stops before the player snapshot's last word: the layout
// before `entity_id` was added, and below the pre-velocity floor. `player_get`
// must reject this size rather than fill a partial snapshot.
struct TruncatedPlayer {
  uint32_t struct_size;
  char name[64];
  double x, y, z;
  float yaw, pitch;
  int gamemode;
  float health;
};

static_assert(sizeof(TruncatedPlayer) == offsetof(pmc_player, entity_id),
              "the truncated player struct must stop before the pre-velocity prefix");

// The reverse direction: a caller built against a newer header than this host.
// The host knows only `sizeof(pmc_player)` / `sizeof(pmc_entity)`, so it must
// fill that much, report it in `struct_size`, and leave everything past its
// own snapshot untouched.
struct FuturePlayer {
  pmc_player base;
  uint64_t marker;
  double extra;
};

static_assert(sizeof(FuturePlayer) > sizeof(pmc_player),
              "the future player struct must extend the host snapshot");

struct FutureEntity {
  pmc_entity base;
  uint64_t marker;
  double extra;
};

static_assert(sizeof(FutureEntity) > sizeof(pmc_entity),
              "the future entity struct must extend the host snapshot");

// The item snapshot is not append-only: its size rule is exact, so a struct
// that stops before `name` is refused rather than filled as a prefix.
struct TruncatedItem {
  uint32_t struct_size;
  int32_t item;
  int32_t count;
  int32_t damage;
  int32_t max_stack_size;
};

static_assert(sizeof(TruncatedItem) == offsetof(pmc_item, name),
              "the truncated item struct must stop before the name");
static_assert(sizeof(TruncatedItem) < sizeof(pmc_item),
              "the truncated item struct must be below the exact-size floor");

// Every byte the older plugin does not own starts as this value, so a write
// past a fixture struct's declared prefix is visible instead of silent.
constexpr unsigned char kTail = 0xA5;

struct OldAbi {
  const pmc_host_api* api = nullptr;
};

void on_oldabi_command(void* data, pmc_command* command) {
  auto& plugin = *static_cast<OldAbi*>(data);
  const char* target =
      command->argc > 1 ? command->argv[1] : (command->player != nullptr ? command->player : "");
  if (plugin.api->player_get == nullptr) {
    if (command->reply != nullptr)
      plugin.api->reply(plugin.api->host, command->reply, "(oldabi) this host has no player_get");
    return;
  }
  // One buffer sized for the current struct so the canary has somewhere to
  // live; the host receives the old prefix and the old size, exactly as it
  // would from a module compiled against the older header.
  alignas(pmc_player) unsigned char buffer[sizeof(pmc_player)];
  std::memset(buffer, kTail, sizeof(buffer));
  auto* old = reinterpret_cast<OldPlayer*>(buffer);
  old->struct_size = static_cast<uint32_t>(sizeof(OldPlayer));
  const int rc =
      plugin.api->player_get(plugin.api->host, target, reinterpret_cast<pmc_player*>(old));
  if (rc != 0) {
    const std::string reply = std::string("(oldabi) no player named '") + target + "'";
    if (command->reply != nullptr)
      plugin.api->reply(plugin.api->host, command->reply, reply.c_str());
    return;
  }
  bool tail_intact = true;
  for (std::size_t i = sizeof(OldPlayer); i < sizeof(buffer); ++i)
    tail_intact = tail_intact && buffer[i] == kTail;
  char line[320];
  // `%.*s`: the name is the host's to fill, but a host that wrote nothing must
  // not run this print past the fixture's own buffer.
  std::snprintf(
      line, sizeof(line), "(oldabi) %.*s at %.2f, %.2f, %.2f (entity %d, health %.1f), %s",
      static_cast<int>(sizeof(old->name) - 1), old->name, old->x, old->y, old->z, old->entity_id,
      static_cast<double>(old->health), tail_intact ? "tail intact" : "tail written");
  if (command->reply != nullptr)
    plugin.api->reply(plugin.api->host, command->reply, line);
}

bool parse_entity_id(const char* text, int32_t* out) {
  if (text == nullptr || *text == '\0')
    return false;
  char* end = nullptr;
  errno = 0;
  const long value = std::strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value <= 0 || value > INT32_MAX)
    return false;
  *out = static_cast<int32_t>(value);
  return true;
}

void on_oldentget_command(void* data, pmc_command* command) {
  auto& plugin = *static_cast<OldAbi*>(data);
  if (plugin.api->entity_get == nullptr) {
    if (command->reply != nullptr)
      plugin.api->reply(plugin.api->host, command->reply,
                        "(oldentget) this host has no entity_get");
    return;
  }
  int32_t id = 0;
  if (command->argc < 2 || !parse_entity_id(command->argv[1], &id)) {
    if (command->reply != nullptr)
      plugin.api->reply(plugin.api->host, command->reply,
                        "(oldentget) usage: /oldentget <entity id>");
    return;
  }
  // An `OldEntity`-sized buffer, canaried past the prefix; the host receives
  // the old size, exactly as it would from a module built against the old
  // header.
  alignas(pmc_entity) unsigned char buffer[sizeof(pmc_entity)];
  std::memset(buffer, kTail, sizeof(buffer));
  auto* old = reinterpret_cast<OldEntity*>(buffer);
  old->struct_size = static_cast<uint32_t>(sizeof(OldEntity));
  const int rc = plugin.api->entity_get(plugin.api->host, id, reinterpret_cast<pmc_entity*>(old));
  char line[320];
  if (rc != PMC_ENTITY_OK) {
    std::snprintf(line, sizeof(line), "(oldentget) lookup failed (%d)", rc);
  } else {
    bool tail_intact = true;
    for (std::size_t i = sizeof(OldEntity); i < sizeof(buffer); ++i)
      tail_intact = tail_intact && buffer[i] == kTail;
    // `%.*s`: the host fills the strings, but a host that wrote nothing must
    // not run this print past the fixture's own buffers.
    std::snprintf(line, sizeof(line),
                  "(oldentget) entity %d: %.*s \"%.*s\" at %.2f, %.2f, %.2f "
                  "(health %.1f, age %d), %s",
                  old->entity_id, static_cast<int>(sizeof(old->type) - 1), old->type,
                  static_cast<int>(sizeof(old->name) - 1), old->name, old->x, old->y, old->z,
                  static_cast<double>(old->health), old->age,
                  tail_intact ? "tail intact" : "tail written");
  }
  if (command->reply != nullptr)
    plugin.api->reply(plugin.api->host, command->reply, line);
}

void on_oldptrunc_command(void* data, pmc_command* command) {
  auto& plugin = *static_cast<OldAbi*>(data);
  const char* target =
      command->argc > 1 ? command->argv[1] : (command->player != nullptr ? command->player : "");
  if (plugin.api->player_get == nullptr) {
    if (command->reply != nullptr)
      plugin.api->reply(plugin.api->host, command->reply,
                        "(oldptrunc) this host has no player_get");
    return;
  }
  // Room for the current struct so a host that writes before checking the
  // size cannot run off this buffer; the canary proves it wrote nothing.
  alignas(pmc_player) unsigned char buffer[sizeof(pmc_player)];
  std::memset(buffer, kTail, sizeof(buffer));
  auto* truncated = reinterpret_cast<TruncatedPlayer*>(buffer);
  truncated->struct_size = static_cast<uint32_t>(sizeof(TruncatedPlayer));
  const int rc =
      plugin.api->player_get(plugin.api->host, target, reinterpret_cast<pmc_player*>(truncated));
  bool untouched = true;
  for (std::size_t i = offsetof(TruncatedPlayer, name); i < sizeof(buffer); ++i)
    untouched = untouched && buffer[i] == kTail;
  char line[96];
  std::snprintf(line, sizeof(line), "(oldptrunc) rc %d, struct %s", rc,
                untouched ? "untouched" : "written");
  if (command->reply != nullptr)
    plugin.api->reply(plugin.api->host, command->reply, line);
}

void on_oldtrunc_command(void* data, pmc_command* command) {
  auto& plugin = *static_cast<OldAbi*>(data);
  if (plugin.api->entity_get == nullptr) {
    if (command->reply != nullptr)
      plugin.api->reply(plugin.api->host, command->reply, "(oldtrunc) this host has no entity_get");
    return;
  }
  int32_t id = 0;
  if (command->argc < 2 || !parse_entity_id(command->argv[1], &id)) {
    if (command->reply != nullptr)
      plugin.api->reply(plugin.api->host, command->reply,
                        "(oldtrunc) usage: /oldtrunc <entity id>");
    return;
  }
  // Room for the current struct so a host that writes before checking the
  // size cannot run off this buffer; the canary proves it wrote nothing.
  alignas(pmc_entity) unsigned char buffer[sizeof(pmc_entity)];
  std::memset(buffer, kTail, sizeof(buffer));
  auto* truncated = reinterpret_cast<TruncatedEntity*>(buffer);
  truncated->struct_size = static_cast<uint32_t>(sizeof(TruncatedEntity));
  const int rc =
      plugin.api->entity_get(plugin.api->host, id, reinterpret_cast<pmc_entity*>(truncated));
  bool untouched = true;
  for (std::size_t i = offsetof(TruncatedEntity, entity_id); i < sizeof(buffer); ++i)
    untouched = untouched && buffer[i] == kTail;
  char line[96];
  std::snprintf(line, sizeof(line), "(oldtrunc) rc %d, struct %s", rc,
                untouched ? "untouched" : "written");
  if (command->reply != nullptr)
    plugin.api->reply(plugin.api->host, command->reply, line);
}

void on_newplayer_command(void* data, pmc_command* command) {
  auto& plugin = *static_cast<OldAbi*>(data);
  const char* target =
      command->argc > 1 ? command->argv[1] : (command->player != nullptr ? command->player : "");
  if (plugin.api->player_get == nullptr) {
    if (command->reply != nullptr)
      plugin.api->reply(plugin.api->host, command->reply,
                        "(newplayer) this host has no player_get");
    return;
  }
  // The whole larger struct is canaried; only the host's own prefix may move.
  FuturePlayer future;
  std::memset(&future, kTail, sizeof(future));
  future.base.struct_size = static_cast<uint32_t>(sizeof(FuturePlayer));
  const int rc = plugin.api->player_get(plugin.api->host, target, &future.base);
  char line[320];
  if (rc != 0) {
    std::snprintf(line, sizeof(line), "(newplayer) no player named '%s'", target);
  } else {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&future);
    bool extension_intact = true;
    for (std::size_t i = sizeof(pmc_player); i < sizeof(FuturePlayer); ++i)
      extension_intact = extension_intact && bytes[i] == kTail;
    // The caller declared `sizeof(FuturePlayer)`; the host must report the
    // smaller number it actually filled.
    std::snprintf(line, sizeof(line),
                  "(newplayer) %.*s at %.2f, %.2f, %.2f "
                  "(vel %.3f, %.3f, %.3f), struct_size %u of %zu, extension %s",
                  static_cast<int>(sizeof(future.base.name) - 1), future.base.name, future.base.x,
                  future.base.y, future.base.z, future.base.vx, future.base.vy, future.base.vz,
                  future.base.struct_size, sizeof(FuturePlayer),
                  extension_intact ? "intact" : "written");
  }
  if (command->reply != nullptr)
    plugin.api->reply(plugin.api->host, command->reply, line);
}

void on_newentity_command(void* data, pmc_command* command) {
  auto& plugin = *static_cast<OldAbi*>(data);
  if (plugin.api->entity_get == nullptr) {
    if (command->reply != nullptr)
      plugin.api->reply(plugin.api->host, command->reply,
                        "(newentity) this host has no entity_get");
    return;
  }
  int32_t id = 0;
  if (command->argc < 2 || !parse_entity_id(command->argv[1], &id)) {
    if (command->reply != nullptr)
      plugin.api->reply(plugin.api->host, command->reply,
                        "(newentity) usage: /newentity <entity id>");
    return;
  }
  FutureEntity future;
  std::memset(&future, kTail, sizeof(future));
  future.base.struct_size = static_cast<uint32_t>(sizeof(FutureEntity));
  const int rc = plugin.api->entity_get(plugin.api->host, id, &future.base);
  char line[320];
  if (rc != PMC_ENTITY_OK) {
    std::snprintf(line, sizeof(line), "(newentity) lookup failed (%d)", rc);
  } else {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&future);
    bool extension_intact = true;
    for (std::size_t i = sizeof(pmc_entity); i < sizeof(FutureEntity); ++i)
      extension_intact = extension_intact && bytes[i] == kTail;
    // The caller declared `sizeof(FutureEntity)`; the host must report the
    // smaller number it actually filled.
    std::snprintf(line, sizeof(line),
                  "(newentity) entity %d: %.*s \"%.*s\" at %.2f, %.2f, %.2f "
                  "(age %d, yaw %.1f, pitch %.1f), struct_size %u of %zu, extension %s",
                  future.base.entity_id, static_cast<int>(sizeof(future.base.type) - 1),
                  future.base.type, static_cast<int>(sizeof(future.base.name) - 1),
                  future.base.name, future.base.x, future.base.y, future.base.z, future.base.age,
                  static_cast<double>(future.base.yaw), static_cast<double>(future.base.pitch),
                  future.base.struct_size, sizeof(FutureEntity),
                  extension_intact ? "intact" : "written");
  }
  if (command->reply != nullptr)
    plugin.api->reply(plugin.api->host, command->reply, line);
}

struct EntitySizeScan {
  int count = 0;
  int wrong = 0;
  uint32_t size = 0;
};

void visit_entity_size(void* data, const pmc_entity* entity) {
  auto& scan = *static_cast<EntitySizeScan*>(data);
  ++scan.count;
  scan.size = entity->struct_size;
  if (entity->struct_size != sizeof(pmc_entity))
    ++scan.wrong;
}

void on_invtrunc_command(void* data, pmc_command* command) {
  auto& plugin = *static_cast<OldAbi*>(data);
  const char* target =
      command->argc > 1 ? command->argv[1] : (command->player != nullptr ? command->player : "");
  if (plugin.api->inventory_get == nullptr) {
    if (command->reply != nullptr)
      plugin.api->reply(plugin.api->host, command->reply,
                        "(invtrunc) this host has no inventory_get");
    return;
  }
  // Slot 0 and a live name are valid; the declared size is what must be
  // refused, and the canary proves the refusal wrote nothing.
  alignas(pmc_item) unsigned char buffer[sizeof(pmc_item)];
  std::memset(buffer, kTail, sizeof(buffer));
  auto* truncated = reinterpret_cast<TruncatedItem*>(buffer);
  truncated->struct_size = static_cast<uint32_t>(sizeof(TruncatedItem));
  const int rc = plugin.api->inventory_get(plugin.api->host, target, 0,
                                           reinterpret_cast<pmc_item*>(truncated));
  bool untouched = true;
  for (std::size_t i = offsetof(TruncatedItem, item); i < sizeof(buffer); ++i)
    untouched = untouched && buffer[i] == kTail;
  char line[96];
  std::snprintf(line, sizeof(line), "(invtrunc) rc %d, struct %s", rc,
                untouched ? "untouched" : "written");
  if (command->reply != nullptr)
    plugin.api->reply(plugin.api->host, command->reply, line);
}

void on_entitysize_command(void* data, pmc_command* command) {
  auto& plugin = *static_cast<OldAbi*>(data);
  if (plugin.api->entities == nullptr) {
    if (command->reply != nullptr)
      plugin.api->reply(plugin.api->host, command->reply,
                        "(entitysize) this host cannot list entities");
    return;
  }
  EntitySizeScan scan;
  const int visited = plugin.api->entities(plugin.api->host, &visit_entity_size, &scan);
  char line[160];
  if (visited < 0) {
    std::snprintf(line, sizeof(line), "(entitysize) this server has no world");
  } else if (scan.count == 0) {
    std::snprintf(line, sizeof(line), "(entitysize) no entities");
  } else if (scan.wrong != 0) {
    std::snprintf(line, sizeof(line), "(entitysize) %d %s, %d wrong (last struct_size %u)",
                  scan.count, scan.count == 1 ? "entity" : "entities", scan.wrong, scan.size);
  } else {
    std::snprintf(line, sizeof(line), "(entitysize) %d %s, struct_size %u (all match)", scan.count,
                  scan.count == 1 ? "entity" : "entities", scan.size);
  }
  if (command->reply != nullptr)
    plugin.api->reply(plugin.api->host, command->reply, line);
}

bool register_command(const pmc_host_api* api, void* plugin, const char* name, const char* usage,
                      pmc_command_fn fn) {
  return api->command(api->host, name, usage, fn, plugin) == 0;
}

int plugin_load(const pmc_host_api* api, void** plugin_data) {
  auto* plugin = new OldAbi();
  plugin->api = api;
  if (!register_command(api, plugin, "oldabi", "/oldabi [player]", &on_oldabi_command) ||
      !register_command(api, plugin, "oldentget", "/oldentget <entity id>",
                        &on_oldentget_command) ||
      !register_command(api, plugin, "oldtrunc", "/oldtrunc <entity id>", &on_oldtrunc_command) ||
      !register_command(api, plugin, "oldptrunc", "/oldptrunc [player]", &on_oldptrunc_command) ||
      !register_command(api, plugin, "newplayer", "/newplayer [player]", &on_newplayer_command) ||
      !register_command(api, plugin, "newentity", "/newentity <entity id>",
                        &on_newentity_command) ||
      !register_command(api, plugin, "entitysize", "/entitysize", &on_entitysize_command) ||
      !register_command(api, plugin, "invtrunc", "/invtrunc [player]", &on_invtrunc_command)) {
    delete plugin;
    return 1;
  }
  *plugin_data = plugin;
  return 0;
}

void plugin_unload(void* data) {
  delete static_cast<OldAbi*>(data);
}

const pmc_plugin kPlugin = {
    sizeof(pmc_plugin),
    PMC_PLUGIN_ABI_VERSION,
    "oldabi",
    "1.7.0",
    "+MC",
    "Test fixture: size rules on the player, entity, visitor and item paths.",
    &plugin_load,
    nullptr,
    nullptr,
    &plugin_unload,
};

} // namespace

extern "C" PMC_PLUGIN_EXPORT const pmc_plugin* pmc_plugin_entry(void) {
  return &kPlugin;
}
